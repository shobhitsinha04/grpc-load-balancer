// L7 gRPC load balancer.
//
// What makes this Layer 7 rather than Layer 4: this process is itself a gRPC
// server. It terminates the client's HTTP/2 connection, decodes the request
// down to a specific RPC (echo.v1.EchoService/Echo) and its protobuf message,
// chooses a backend, and issues a *new* gRPC call on a *separate* connection.
// Nothing is forwarded at the packet or byte-stream level. Because the balancer
// understands the RPC boundary, it can balance individual calls -- an L4
// balancer pins a whole TCP connection to one backend, and since gRPC
// multiplexes many calls over one long-lived HTTP/2 connection, that would pin
// every call from a client to a single backend and defeat balancing entirely.

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "backend_pool.h"
#include "common/flags.h"
#include "echo.grpc.pb.h"
#include "metrics_server.h"

namespace {

volatile std::sig_atomic_t g_shutdown = 0;
void OnSignal(int) { g_shutdown = 1; }

class LbEchoService final : public echo::v1::EchoService::Service {
 public:
  LbEchoService(lb::BackendPool* pool, lb::Histogram* latency, int timeout_ms,
                int max_attempts)
      : pool_(pool),
        latency_(latency),
        timeout_ms_(timeout_ms),
        max_attempts_(max_attempts) {}

  grpc::Status Echo(grpc::ServerContext* ctx, const echo::v1::EchoRequest* req,
                    echo::v1::EchoResponse* resp) override {
    const auto started = std::chrono::steady_clock::now();
    const lb::Backend* last_tried = nullptr;

    for (int attempt = 0; attempt < max_attempts_; ++attempt) {
      lb::Backend* b = (attempt == 0) ? pool_->Pick() : pool_->PickExcluding(last_tried);
      if (b == nullptr) break;

      grpc::ClientContext cctx;
      ForwardMetadata(ctx, &cctx);

      // Honour whichever deadline is tighter. Ignoring the client's deadline
      // would leave the balancer working on a call the client already gave up
      // on; ignoring ours would let one slow backend hold the request forever.
      const auto lb_deadline = std::chrono::system_clock::now() +
                               std::chrono::milliseconds(timeout_ms_);
      cctx.set_deadline(std::min(ctx->deadline(), lb_deadline));

      const grpc::Status status = b->echo_stub->Echo(&cctx, *req, resp);
      if (status.ok()) {
        b->rpcs_ok.fetch_add(1, std::memory_order_relaxed);
        requests_ok_.fetch_add(1, std::memory_order_relaxed);
        Record(started);
        return grpc::Status::OK;
      }

      // Reaching here means health checking had not yet caught up with this
      // backend. Retrying elsewhere converts a user-visible error into a
      // slightly slower success; the health loop evicts it moments later.
      b->rpcs_fail.fetch_add(1, std::memory_order_relaxed);
      retries_.fetch_add(1, std::memory_order_relaxed);
      last_tried = b;
    }

    requests_failed_.fetch_add(1, std::memory_order_relaxed);
    Record(started);
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "no healthy backend available");
  }

  uint64_t requests_ok() const { return requests_ok_.load(); }
  uint64_t requests_failed() const { return requests_failed_.load(); }
  uint64_t retries() const { return retries_.load(); }

 private:
  void Record(std::chrono::steady_clock::time_point started) {
    const auto elapsed = std::chrono::steady_clock::now() - started;
    latency_->Observe(
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count());
  }

  // Pass client metadata through to the backend. Reserved HTTP/2 pseudo-headers
  // and gRPC's own control headers are skipped: gRPC regenerates those for the
  // outbound call, and copying them would either be rejected or would smuggle
  // the client's timeout past the deadline logic above.
  static void ForwardMetadata(const grpc::ServerContext* from,
                              grpc::ClientContext* to) {
    for (const auto& kv : from->client_metadata()) {
      const std::string key(kv.first.data(), kv.first.size());
      if (key.empty() || key[0] == ':') continue;
      if (key.rfind("grpc-", 0) == 0) continue;
      if (key == "user-agent" || key == "te" || key == "content-type" ||
          key == "authority" || key == "host") {
        continue;
      }
      to->AddMetadata(key, std::string(kv.second.data(), kv.second.size()));
    }
  }

  lb::BackendPool* const pool_;
  lb::Histogram* const latency_;
  const int timeout_ms_;
  const int max_attempts_;

  std::atomic<uint64_t> requests_ok_{0};
  std::atomic<uint64_t> requests_failed_{0};
  std::atomic<uint64_t> retries_{0};
};

std::string RenderMetrics(const lb::BackendPool& pool, const LbEchoService& svc,
                          const lb::Histogram& latency) {
  std::ostringstream os;

  os << "# HELP lb_requests_total RPCs handled by the load balancer, by outcome.\n"
     << "# TYPE lb_requests_total counter\n"
     << "lb_requests_total{result=\"ok\"} " << svc.requests_ok() << "\n"
     << "lb_requests_total{result=\"failed\"} " << svc.requests_failed() << "\n";

  os << "# HELP lb_retries_total Times an RPC was re-sent to a different backend.\n"
     << "# TYPE lb_retries_total counter\n"
     << "lb_retries_total " << svc.retries() << "\n";

  os << "# HELP lb_healthy_backends Backends currently in rotation.\n"
     << "# TYPE lb_healthy_backends gauge\n"
     << "lb_healthy_backends " << pool.HealthyCount() << "\n";

  os << "# HELP lb_backend_healthy Whether a backend is in rotation (1) or evicted (0).\n"
     << "# TYPE lb_backend_healthy gauge\n";
  for (const auto& b : pool.backends()) {
    os << "lb_backend_healthy{backend=\"" << b->name << "\",address=\""
       << b->address << "\"} " << (b->healthy.load() ? 1 : 0) << "\n";
  }

  os << "# HELP lb_backend_rpcs_total RPCs routed to each backend, by outcome.\n"
     << "# TYPE lb_backend_rpcs_total counter\n";
  for (const auto& b : pool.backends()) {
    os << "lb_backend_rpcs_total{backend=\"" << b->name << "\",result=\"ok\"} "
       << b->rpcs_ok.load() << "\n";
    os << "lb_backend_rpcs_total{backend=\"" << b->name << "\",result=\"failed\"} "
       << b->rpcs_fail.load() << "\n";
  }

  os << "# HELP lb_health_checks_total Active health checks performed, by outcome.\n"
     << "# TYPE lb_health_checks_total counter\n";
  for (const auto& b : pool.backends()) {
    os << "lb_health_checks_total{backend=\"" << b->name << "\",result=\"ok\"} "
       << b->checks_ok.load() << "\n";
    os << "lb_health_checks_total{backend=\"" << b->name << "\",result=\"fail\"} "
       << b->checks_fail.load() << "\n";
  }

  os << "# HELP lb_backend_transitions_total Health state flips, for spotting flapping.\n"
     << "# TYPE lb_backend_transitions_total counter\n";
  for (const auto& b : pool.backends()) {
    os << "lb_backend_transitions_total{backend=\"" << b->name << "\"} "
       << b->transitions.load() << "\n";
  }

  os << latency.Render("lb_request_duration_seconds",
                       "End-to-end latency through the load balancer, including "
                       "backend time and any retry.");
  return os.str();
}

}  // namespace

int main(int argc, char** argv) {
  const int port = common::FlagInt(argc, argv, "port", 50050);
  const int metrics_port = common::FlagInt(argc, argv, "metrics_port", 9100);
  const std::string backends_csv =
      common::FlagValue(argc, argv, "backends", "127.0.0.1:50051");
  const int timeout_ms = common::FlagInt(argc, argv, "timeout_ms", 500);
  const int max_attempts = common::FlagInt(argc, argv, "max_attempts", 3);

  lb::BackendPool::Options opts;
  opts.interval_ms = common::FlagInt(argc, argv, "health_interval_ms", opts.interval_ms);
  opts.timeout_ms = common::FlagInt(argc, argv, "health_timeout_ms", opts.timeout_ms);
  opts.healthy_threshold =
      common::FlagInt(argc, argv, "healthy_threshold", opts.healthy_threshold);
  opts.unhealthy_threshold =
      common::FlagInt(argc, argv, "unhealthy_threshold", opts.unhealthy_threshold);
  opts.jitter_ms = common::FlagInt(argc, argv, "health_jitter_ms", opts.jitter_ms);

  const std::vector<std::string> targets = common::Split(backends_csv, ',');
  if (targets.empty()) {
    std::cerr << "[lb] FATAL: --backends is empty\n";
    return 1;
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  // The balancer has no use for SIGUSR1/SIGUSR2, but their default disposition
  // is to terminate the process -- so a stray one kills the load balancer.
  // That is not hypothetical: the backends use these signals to toggle health,
  // and the obvious way to address a backend by port, `lsof -ti:50052`, also
  // matches this process, because it holds an established connection to that
  // backend. `kill -USR1 $(lsof -ti:50052)` then signals both, and the
  // balancer dies while the backend does the right thing -- an outage that
  // looks like a load balancer bug and is really a fan-out signal. Ignoring
  // signals a daemon does not use is cheap; dying from one is not.
  std::signal(SIGUSR1, SIG_IGN);
  std::signal(SIGUSR2, SIG_IGN);

  lb::BackendPool pool(targets, opts);
  lb::Histogram latency;
  LbEchoService service(&pool, &latency, timeout_ms, max_attempts);

  // The balancer serves the same standard health protocol it consumes, so an
  // orchestrator can probe it the same way it probes the backends. Kubernetes'
  // native `readinessProbe: grpc:` speaks exactly this.
  grpc::EnableDefaultHealthCheckService(true);

  const std::string address = "0.0.0.0:" + std::to_string(port);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "[lb] FATAL: could not bind " << address << "\n";
    return 1;
  }

  grpc::HealthCheckServiceInterface* health = server->GetHealthCheckService();
  if (health == nullptr) {
    std::cerr << "[lb] FATAL: health service not enabled\n";
    return 1;
  }
  // A balancer with an empty pool can only return UNAVAILABLE, so it reports
  // itself NOT_SERVING and lets the orchestrator route around it. Starts false:
  // readiness is earned once the first backend passes a check, which keeps a
  // freshly started LB out of a Service's endpoints until it can actually serve.
  health->SetServingStatus("", false);

  lb::MetricsServer metrics(metrics_port,
                            [&] { return RenderMetrics(pool, service, latency); });
  if (!metrics.Start()) {
    std::cerr << "[lb] FATAL: could not start metrics endpoint\n";
    return 1;
  }

  pool.Start();

  std::cout << "[lb] listening on " << address << "\n"
            << "[lb] metrics on 0.0.0.0:" << metrics_port << "/metrics\n"
            << "[lb] backends (" << targets.size() << "): " << backends_csv << "\n"
            << "[lb] policy: round-robin | health: every " << opts.interval_ms
            << "ms +0-" << opts.jitter_ms << "ms jitter, timeout "
            << opts.timeout_ms << "ms, evict after " << opts.unhealthy_threshold
            << " fail, re-add after " << opts.healthy_threshold << " ok\n"
            << std::flush;

  // Mirror pool state into our own health status. Polling rather than pushing
  // from the health threads keeps BackendPool unaware of the gRPC server, and
  // at a 50ms tick the readiness lag is far below any orchestrator's probe
  // period.
  bool advertised_serving = false;
  while (!g_shutdown) {
    const bool serving = pool.HealthyCount() > 0;
    if (serving != advertised_serving) {
      advertised_serving = serving;
      health->SetServingStatus("", serving);
      std::cout << "[lb] self-health -> " << (serving ? "SERVING" : "NOT_SERVING")
                << " (" << pool.HealthyCount() << " healthy backends)\n"
                << std::flush;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  std::cout << "\n[lb] shutting down (ok=" << service.requests_ok()
            << " failed=" << service.requests_failed()
            << " retries=" << service.retries() << ")\n" << std::flush;
  pool.Stop();
  metrics.Stop();
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(200));
  server->Wait();
  return 0;
}
