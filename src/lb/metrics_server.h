// Minimal Prometheus exposition endpoint.
//
// Prometheus scrapes over plain HTTP/1.1 and parses a line-oriented text
// format, so a dependency-free socket loop covers it. Deliberately not using
// prometheus-cpp: this endpoint is read-only, single-route, and low-traffic,
// and vendoring a library for it would add build surface without buying
// anything the format doesn't already give us for free.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lb {

// Cumulative latency histogram in the Prometheus sense: each bucket counts
// observations <= its upper bound, and buckets are cumulative on render.
class Histogram {
 public:
  Histogram();

  void Observe(double seconds);

  // Renders bucket/sum/count lines for `name`, Prometheus text format.
  std::string Render(const std::string& name, const std::string& help) const;

 private:
  // Bounds chosen for a same-host RPC hop: sub-millisecond at the low end,
  // seconds at the high end to catch a backend hitting its deadline.
  static const std::vector<double>& Bounds();

  std::vector<std::unique_ptr<std::atomic<uint64_t>>> counts_;
  // Sum is kept in integer microseconds because std::atomic<double> is not
  // lock-free on every target; the division happens once, at render time.
  std::atomic<uint64_t> sum_micros_{0};
  std::atomic<uint64_t> count_{0};
};

class MetricsServer {
 public:
  // `renderer` is invoked per scrape and must return a complete metrics body.
  using Renderer = std::function<std::string()>;

  MetricsServer(int port, Renderer renderer);
  ~MetricsServer();

  MetricsServer(const MetricsServer&) = delete;
  MetricsServer& operator=(const MetricsServer&) = delete;

  bool Start();
  void Stop();

 private:
  void Loop();

  const int port_;
  const Renderer renderer_;
  int listen_fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace lb
