#include "metrics_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace lb {

const std::vector<double>& Histogram::Bounds() {
  // Calibrated against measurement, not guessed: a loopback hop through the
  // balancer lands around 0.25ms, so the useful resolution is well below 1ms.
  // Buckets start at 50us to keep the common case spread across several
  // buckets instead of piling into the first one, and still run out to 2.5s so
  // a backend timing out is visible rather than lost in +Inf.
  static const std::vector<double>* kBounds = new std::vector<double>{
      0.00005, 0.0001, 0.00025, 0.0005, 0.00075, 0.001, 0.0025,
      0.005,   0.01,   0.025,   0.05,   0.1,     0.25,  0.5,
      1.0,     2.5};
  return *kBounds;
}

namespace {

// Render bucket bounds the way they were authored: le="0.001", not le="0.0010"
// (fixed precision) and not le="1e-04" (default float switches to scientific
// below 1e-4, which is valid but reads badly in a dashboard legend).
std::string FormatBound(double v) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(6) << v;
  std::string s = os.str();
  s.erase(s.find_last_not_of('0') + 1);
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

}  // namespace

Histogram::Histogram() {
  // +1 for the implicit +Inf bucket.
  counts_.reserve(Bounds().size() + 1);
  for (size_t i = 0; i < Bounds().size() + 1; ++i) {
    counts_.push_back(std::make_unique<std::atomic<uint64_t>>(0));
  }
}

void Histogram::Observe(double seconds) {
  const auto& bounds = Bounds();
  // Store non-cumulatively; Render() accumulates. Keeps Observe O(log n)
  // rather than touching every bucket on the hot path.
  size_t idx = std::lower_bound(bounds.begin(), bounds.end(), seconds) - bounds.begin();
  counts_[idx]->fetch_add(1, std::memory_order_relaxed);
  sum_micros_.fetch_add(static_cast<uint64_t>(seconds * 1e6), std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
}

std::string Histogram::Render(const std::string& name, const std::string& help) const {
  std::ostringstream os;
  os << "# HELP " << name << " " << help << "\n";
  os << "# TYPE " << name << " histogram\n";

  uint64_t cumulative = 0;
  const auto& bounds = Bounds();
  for (size_t i = 0; i < bounds.size(); ++i) {
    cumulative += counts_[i]->load(std::memory_order_relaxed);
    os << name << "_bucket{le=\"" << FormatBound(bounds[i]) << "\"} " << cumulative
       << "\n";
  }
  cumulative += counts_[bounds.size()]->load(std::memory_order_relaxed);
  os << name << "_bucket{le=\"+Inf\"} " << cumulative << "\n";

  os << name << "_sum " << std::fixed << std::setprecision(6)
     << (sum_micros_.load(std::memory_order_relaxed) / 1e6) << "\n";
  os << name << "_count " << count_.load(std::memory_order_relaxed) << "\n";
  return os.str();
}

MetricsServer::MetricsServer(int port, Renderer renderer)
    : port_(port), renderer_(std::move(renderer)) {}

MetricsServer::~MetricsServer() { Stop(); }

bool MetricsServer::Start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    std::cerr << "[lb] metrics: socket() failed: " << std::strerror(errno) << "\n";
    return false;
  }

  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port_));

  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "[lb] metrics: bind(" << port_ << ") failed: "
              << std::strerror(errno) << "\n";
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 16) < 0) {
    std::cerr << "[lb] metrics: listen() failed: " << std::strerror(errno) << "\n";
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  running_ = true;
  thread_ = std::thread([this] { Loop(); });
  return true;
}

void MetricsServer::Stop() {
  if (!running_.exchange(false)) return;
  if (listen_fd_ >= 0) {
    // Wakes the poll() in Loop() so the thread observes running_ == false.
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
}

void MetricsServer::Loop() {
  while (running_.load()) {
    pollfd pfd{};
    pfd.fd = listen_fd_;
    pfd.events = POLLIN;
    // Bounded wait so shutdown is observed promptly without a self-pipe.
    int rc = ::poll(&pfd, 1, 200);
    if (rc <= 0) continue;
    if (!running_.load()) break;

    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;

    char buf[2048];
    ::recv(fd, buf, sizeof(buf), 0);  // Single route; request line is ignored.

    const std::string body = renderer_ ? renderer_() : std::string();
    std::ostringstream os;
    os << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    const std::string out = os.str();

    size_t sent = 0;
    while (sent < out.size()) {
      ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, 0);
      if (n <= 0) break;
      sent += static_cast<size_t>(n);
    }
    ::close(fd);
  }
}

}  // namespace lb
