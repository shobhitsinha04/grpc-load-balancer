// Demo traffic generator.
//
// Sends N Echo RPCs at the load balancer and tallies the `served_by` field of
// each reply. The distribution it prints is measured from the client side --
// the balancer is never asked to grade its own homework.

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <thread>

#include "common/flags.h"
#include "echo.grpc.pb.h"

int main(int argc, char** argv) {
  const std::string target = common::FlagValue(argc, argv, "target", "127.0.0.1:50050");
  const int requests = common::FlagInt(argc, argv, "requests", 30);
  const int timeout_ms = common::FlagInt(argc, argv, "timeout_ms", 2000);
  const int delay_ms = common::FlagInt(argc, argv, "delay_ms", 0);
  const std::string label = common::FlagValue(argc, argv, "label", "");
  const std::string message = common::FlagValue(argc, argv, "message", "ping");

  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  auto stub = echo::v1::EchoService::NewStub(channel);

  std::map<std::string, int> tally;
  std::map<std::string, int> errors;
  int failed = 0;

  for (int i = 0; i < requests; ++i) {
    echo::v1::EchoRequest req;
    req.set_message(message + "-" + std::to_string(i));

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::milliseconds(timeout_ms));

    echo::v1::EchoResponse resp;
    const grpc::Status status = stub->Echo(&ctx, req, &resp);
    if (status.ok()) {
      tally[resp.served_by()]++;
    } else {
      ++failed;
      errors[status.error_message().empty() ? "(no message)" : status.error_message()]++;
    }
    if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }

  if (!label.empty()) std::cout << "\n  " << label << "\n";
  std::cout << "  " << std::string(46, '-') << "\n";

  const int ok = requests - failed;
  for (const auto& [backend, count] : tally) {
    const int bars = ok > 0 ? (count * 24) / ok : 0;
    std::cout << "  " << std::left << std::setw(12) << backend << std::right
              << std::setw(4) << count << "  " << std::string(bars, '#') << "\n";
  }
  if (failed > 0) {
    std::cout << "  " << std::left << std::setw(12) << "FAILED" << std::right
              << std::setw(4) << failed << "\n";
    for (const auto& [msg, count] : errors) {
      std::cout << "                  " << count << "x " << msg << "\n";
    }
  }
  std::cout << "  " << std::string(46, '-') << "\n"
            << "  " << ok << "/" << requests << " ok across " << tally.size()
            << " backend(s)\n" << std::flush;

  // Non-zero exit lets the demo script assert on outcomes.
  return failed > 0 ? 1 : 0;
}
