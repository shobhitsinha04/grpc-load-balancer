#include "backend_pool.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>

namespace lb {
namespace {

std::string ShortName(const std::string& address) {
  const size_t colon = address.rfind(':');
  return colon == std::string::npos ? address : "be-" + address.substr(colon + 1);
}

}  // namespace

BackendPool::BackendPool(const std::vector<std::string>& targets, Options opts)
    : opts_(opts) {
  backends_.reserve(targets.size());
  for (const std::string& target : targets) {
    auto b = std::make_unique<Backend>();
    b->address = target;
    b->name = ShortName(target);
    b->channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    b->echo_stub = echo::v1::EchoService::NewStub(b->channel);
    // Both stubs ride the same channel, so the health check traverses the same
    // HTTP/2 connection the real traffic uses. A check over a separate
    // connection could pass while the connection carrying requests is broken.
    b->health_stub = grpc::health::v1::Health::NewStub(b->channel);
    backends_.push_back(std::move(b));
  }
}

BackendPool::~BackendPool() { Stop(); }

void BackendPool::Start() {
  if (running_.exchange(true)) return;
  threads_.reserve(backends_.size());
  for (size_t i = 0; i < backends_.size(); ++i) {
    // One checker per backend: a slow or hung backend then delays only its own
    // checks instead of head-of-line blocking every other backend's check.
    threads_.emplace_back([this, i] {
      HealthLoop(backends_[i].get(), static_cast<unsigned>(i * 7919 + 13));
    });
  }
}

void BackendPool::Stop() {
  if (!running_.exchange(false)) return;
  for (std::thread& t : threads_) {
    if (t.joinable()) t.join();
  }
  threads_.clear();
}

bool BackendPool::RunCheck(Backend* b) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() +
                   std::chrono::milliseconds(opts_.timeout_ms));

  grpc::health::v1::HealthCheckRequest req;
  req.set_service("");  // "" == the server as a whole, per the protocol.

  grpc::health::v1::HealthCheckResponse resp;
  const grpc::Status status = b->health_stub->Check(&ctx, req, &resp);

  // Two distinct failure modes collapse to the same verdict, and both matter:
  //   - !status.ok(): process is gone / unreachable / timed out.
  //   - status.ok() but not SERVING: process is alive and answering, and is
  //     telling us not to send it traffic. Only an L7 check can see this.
  return status.ok() &&
         resp.status() == grpc::health::v1::HealthCheckResponse::SERVING;
}

void BackendPool::HealthLoop(Backend* b, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> jitter(0, std::max(0, opts_.jitter_ms));

  // Stagger the first check so N backends are not probed on the same tick.
  std::this_thread::sleep_for(std::chrono::milliseconds(jitter(rng)));

  while (running_.load()) {
    const bool ok = RunCheck(b);
    bool changed = false;

    if (ok) {
      b->checks_ok.fetch_add(1, std::memory_order_relaxed);
      b->consecutive_fail.store(0);
      const int streak = b->consecutive_ok.fetch_add(1) + 1;
      if (!b->healthy.load() && streak >= opts_.healthy_threshold) {
        b->healthy.store(true);
        const bool first_admission = b->transitions.fetch_add(1) == 0;
        changed = true;
        std::cout << "[lb] health: " << b->name << " (" << b->address
                  << ") -> HEALTHY, "
                  << (first_admission ? "added to rotation" : "re-added to rotation")
                  << "\n" << std::flush;
      }
    } else {
      b->checks_fail.fetch_add(1, std::memory_order_relaxed);
      b->consecutive_ok.store(0);
      const int streak = b->consecutive_fail.fetch_add(1) + 1;
      if (b->healthy.load() && streak >= opts_.unhealthy_threshold) {
        b->healthy.store(false);
        b->transitions.fetch_add(1);
        changed = true;
        std::cout << "[lb] health: " << b->name << " (" << b->address
                  << ") -> UNHEALTHY, evicted from rotation\n" << std::flush;
      }
    }

    if (changed) RebuildHealthy();
    std::this_thread::sleep_for(
        std::chrono::milliseconds(opts_.interval_ms + jitter(rng)));
  }
}

void BackendPool::RebuildHealthy() {
  std::vector<Backend*> next;
  next.reserve(backends_.size());
  for (const auto& b : backends_) {
    if (b->healthy.load()) next.push_back(b.get());
  }
  std::unique_lock<std::shared_mutex> lock(mu_);
  healthy_.swap(next);
}

Backend* BackendPool::Pick() {
  std::shared_lock<std::shared_mutex> lock(mu_);
  if (healthy_.empty()) return nullptr;
  // relaxed: we need a distinct ticket per call, not ordering against other
  // memory. Wrap-around at 2^64 is harmless -- modulo stays uniform.
  const uint64_t ticket = rr_.fetch_add(1, std::memory_order_relaxed);
  return healthy_[ticket % healthy_.size()];
}

Backend* BackendPool::PickExcluding(const Backend* exclude) {
  std::shared_lock<std::shared_mutex> lock(mu_);
  if (healthy_.empty()) return nullptr;
  const size_t n = healthy_.size();
  for (size_t i = 0; i < n; ++i) {
    const uint64_t ticket = rr_.fetch_add(1, std::memory_order_relaxed);
    Backend* cand = healthy_[ticket % n];
    if (cand != exclude) return cand;
  }
  return nullptr;  // exclude was the only healthy backend.
}

size_t BackendPool::HealthyCount() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return healthy_.size();
}

}  // namespace lb
