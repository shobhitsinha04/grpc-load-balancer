// Echo backend replica.
//
// Two jobs: serve EchoService, and report its own health over the standard
// gRPC health protocol so the load balancer can decide whether to route to it.
//
// The health service here is gRPC's own built-in implementation, enabled with
// EnableDefaultHealthCheckService. We are not hand-rolling a health endpoint:
// the wire protocol a real Kubernetes probe or Envoy sidecar would speak is
// exactly what this serves.

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "echo.grpc.pb.h"

namespace {

// Signal handlers may only touch async-signal-safe state, which rules out
// calling SetServingStatus (it takes locks) directly from the handler. So the
// handler only stores an intent here, and a watcher thread applies it.
volatile std::sig_atomic_t g_health_intent = 0;  // 0=none, 1=unhealthy, 2=healthy
volatile std::sig_atomic_t g_shutdown = 0;

void HandleSignal(int sig) {
  switch (sig) {
    case SIGUSR1: g_health_intent = 1; break;
    case SIGUSR2: g_health_intent = 2; break;
    case SIGINT:
    case SIGTERM: g_shutdown = 1; break;
    default: break;
  }
}

std::string FlagValue(int argc, char** argv, const std::string& name,
                      const std::string& fallback) {
  const std::string prefix = "--" + name + "=";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
  }
  return fallback;
}

class EchoServiceImpl final : public echo::v1::EchoService::Service {
 public:
  explicit EchoServiceImpl(std::string id) : id_(std::move(id)) {}

  grpc::Status Echo(grpc::ServerContext*, const echo::v1::EchoRequest* req,
                    echo::v1::EchoResponse* resp) override {
    resp->set_message(req->message());
    // Stamping our identity is what lets the demo prove the balancing policy
    // from the client side rather than trusting the balancer's own logs.
    resp->set_served_by(id_);
    resp->set_backend_request_count(++served_);
    return grpc::Status::OK;
  }

  uint64_t served() const { return served_.load(); }

 private:
  const std::string id_;
  std::atomic<uint64_t> served_{0};
};

}  // namespace

int main(int argc, char** argv) {
  const std::string port = FlagValue(argc, argv, "port", "50051");
  const std::string id = FlagValue(argc, argv, "id", "backend-" + port);
  const std::string address = "0.0.0.0:" + port;

  std::signal(SIGUSR1, HandleSignal);
  std::signal(SIGUSR2, HandleSignal);
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  // Must precede ServerBuilder so the health service gets registered.
  grpc::EnableDefaultHealthCheckService(true);

  EchoServiceImpl service(id);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "[" << id << "] FATAL: could not bind " << address << "\n";
    return 1;
  }

  grpc::HealthCheckServiceInterface* health = server->GetHealthCheckService();
  if (health == nullptr) {
    std::cerr << "[" << id << "] FATAL: health service not enabled\n";
    return 1;
  }
  // The empty service name is the protocol's convention for "the server as a
  // whole", and is what the load balancer asks about.
  health->SetServingStatus("", true);

  std::cout << "[" << id << "] serving on " << address
            << "  (SIGUSR1 = report UNHEALTHY, SIGUSR2 = report SERVING)\n"
            << std::flush;

  while (!g_shutdown) {
    if (g_health_intent != 0) {
      const bool serving = (g_health_intent == 2);
      g_health_intent = 0;
      health->SetServingStatus("", serving);
      // The process stays up and keeps accepting TCP connections; only the
      // health answer changes. That distinction is the whole point of the
      // demo: an L4 balancer would still route here.
      std::cout << "[" << id << "] health -> "
                << (serving ? "SERVING" : "NOT_SERVING") << std::flush << "\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  std::cout << "[" << id << "] shutting down after " << service.served()
            << " RPCs\n" << std::flush;
  server->Shutdown(std::chrono::system_clock::now() +
                   std::chrono::milliseconds(200));
  server->Wait();
  return 0;
}
