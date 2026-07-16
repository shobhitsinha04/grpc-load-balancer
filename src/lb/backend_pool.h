// The routable set of backend replicas, and the active health checking that
// decides who is in it.
//
// Ownership model: Backend objects are heap-allocated once at construction and
// never destroyed or moved until the pool dies, so raw Backend* handed out by
// Pick() stay valid for the life of the pool. Only membership of the healthy_
// snapshot changes at runtime, never the addresses of the Backends themselves.

#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "echo.grpc.pb.h"
#include "health.grpc.pb.h"

namespace lb {

struct Backend {
  std::string name;
  std::string address;
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<echo::v1::EchoService::Stub> echo_stub;
  std::unique_ptr<grpc::health::v1::Health::Stub> health_stub;

  // Starts false: a backend must prove it is serving before receiving traffic,
  // rather than being assumed good and discovered bad by failing user requests.
  std::atomic<bool> healthy{false};

  std::atomic<int> consecutive_ok{0};
  std::atomic<int> consecutive_fail{0};

  std::atomic<uint64_t> rpcs_ok{0};
  std::atomic<uint64_t> rpcs_fail{0};
  std::atomic<uint64_t> checks_ok{0};
  std::atomic<uint64_t> checks_fail{0};
  std::atomic<uint64_t> transitions{0};
};

class BackendPool {
 public:
  struct Options {
    int interval_ms = 300;
    int timeout_ms = 200;

    // Asymmetric by design. Evicting on a single failure sheds a bad backend
    // before many user requests land on it; requiring several consecutive
    // successes to re-admit stops a half-broken backend from oscillating in
    // and out of rotation. Cheap to be wrong in the safe direction: a healthy
    // backend re-added a few hundred ms late costs nothing.
    int unhealthy_threshold = 1;
    int healthy_threshold = 2;

    // Upper bound on random delay added to each check interval. Without it,
    // every checker thread -- and, at scale, every LB replica -- would settle
    // into lockstep and hit each backend simultaneously. Jittering every round
    // matters more than staggering the start: fixed intervals drift back into
    // sync over time, random ones stay smeared.
    int jitter_ms = 100;
  };

  BackendPool(const std::vector<std::string>& targets, Options opts);
  ~BackendPool();

  BackendPool(const BackendPool&) = delete;
  BackendPool& operator=(const BackendPool&) = delete;

  void Start();
  void Stop();

  // Round-robin over the currently healthy backends. Returns nullptr when none
  // are healthy, which the caller must surface as UNAVAILABLE rather than
  // silently routing to a known-bad backend.
  Backend* Pick();

  // Round-robin, skipping `exclude` — used to retry an RPC that failed against
  // a backend the health checker has not yet caught up with.
  Backend* PickExcluding(const Backend* exclude);

  size_t HealthyCount() const;
  const std::vector<std::unique_ptr<Backend>>& backends() const { return backends_; }

 private:
  void HealthLoop(Backend* b, unsigned seed);
  bool RunCheck(Backend* b);
  void RebuildHealthy();

  const Options opts_;
  std::vector<std::unique_ptr<Backend>> backends_;

  mutable std::shared_mutex mu_;
  std::vector<Backend*> healthy_;  // guarded by mu_

  std::atomic<uint64_t> rr_{0};
  std::atomic<bool> running_{false};
  std::vector<std::thread> threads_;
};

}  // namespace lb
