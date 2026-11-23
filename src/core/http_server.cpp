#include "plgen/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <syncstream>
#include <thread>

#include "plgen/json.hpp"
#include "plgen/thread_pool.hpp"

namespace plgen {
namespace {

// RAII wrapper: closes the socket when it goes out of scope, on every path
// (normal return, early return, exception). Like try-with-resources in Java.
class Fd {
 public:
  explicit Fd(int fd) noexcept : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0) ::close(fd_);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  [[nodiscard]] int get() const noexcept { return fd_; }

 private:
  int fd_;
};

void set_timeouts(int fd, int ms) {
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

// send() may write only part of the buffer; keep going until it's all out.
void send_all(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    // MSG_NOSIGNAL: if the client hung up, return an error instead of killing
    // the whole process with SIGPIPE.
    const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return;  // client gone or timed out; nothing useful left to do
    sent += static_cast<std::size_t>(n);
  }
}

HttpResponse error_response(int status, std::string_view message) {
  return {status, "application/json", "{\"error\":" + json_string(message) + "}"};
}

}  // namespace

HttpServer::HttpServer(Handler handler, ServerOptions options)
    : handler_(std::move(handler)), opts_(options) {}

HttpServer::~HttpServer() {
  if (listen_fd_ >= 0) ::close(listen_fd_);
}

void HttpServer::start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) throw std::runtime_error("socket(): " + std::string(std::strerror(errno)));

  // Allow restarting right away instead of waiting ~60 s for the old socket to expire.
  const int yes = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);  // accept connections on every network interface
  addr.sin_port = htons(opts_.port);         // htons: host to network byte order (big-endian)
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    throw std::runtime_error("bind() on port " + std::to_string(opts_.port) + ": " +
                             std::strerror(errno));
  }
  if (::listen(listen_fd_, SOMAXCONN) < 0) {
    throw std::runtime_error("listen(): " + std::string(std::strerror(errno)));
  }

  socklen_t len = sizeof addr;
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  bound_port_ = ntohs(addr.sin_port);
}

void HttpServer::run() {
  if (listen_fd_ < 0) start();
  const std::size_t threads =
      opts_.threads ? opts_.threads : std::max(1u, std::thread::hardware_concurrency());
  ThreadPool pool(threads, opts_.max_queue);

  while (!stop_.load()) {
    // Wait up to 200 ms for a new connection, then loop to re-check stop_.
    // (A plain blocking accept() would never notice a shutdown request.)
    pollfd pfd{listen_fd_, POLLIN, 0};
    const int ready = ::poll(&pfd, 1, 200);
    if (ready <= 0) continue;  // timeout, or interrupted by a signal

    const int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) continue;
    set_timeouts(client, opts_.io_timeout_ms);

    const bool queued = pool.try_submit([this, client] { handle_connection(client); });
    if (!queued) {  // overloaded: refuse quickly rather than make everyone wait
      const Fd fd(client);
      send_all(fd.get(), serialize(error_response(503, "server busy, try again")));
    }
  }
  // Leaving this scope destroys the pool, which finishes in-flight requests first.
}

void HttpServer::handle_connection(int raw_fd) const {
  const Fd fd(raw_fd);
  const auto t0 = std::chrono::steady_clock::now();

  // Read until we have a full request. It may arrive in several pieces.
  std::string buffer;
  HttpRequest req;
  ParseResult parsed = ParseResult::kIncomplete;
  char chunk[4096];
  while (parsed == ParseResult::kIncomplete) {
    const ssize_t n = ::recv(fd.get(), chunk, sizeof chunk, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return;  // client closed or timed out before sending a full request
    buffer.append(chunk, static_cast<std::size_t>(n));
    if (buffer.size() > opts_.max_request_bytes) {
      send_all(fd.get(), serialize(error_response(413, "request too large")));
      return;
    }
    parsed = parse_request(buffer, req);
  }

  HttpResponse resp;
  if (parsed == ParseResult::kBad) {
    resp = error_response(400, "malformed HTTP request");
  } else {
    try {
      resp = handler_(req);
    } catch (const std::exception& e) {
      std::osyncstream(std::cerr) << "handler error: " << e.what() << '\n';
      resp = error_response(500, "internal error");
    }
  }
  send_all(fd.get(), serialize(resp));

  if (opts_.log_requests) {
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    // osyncstream: each log line is written in one piece, even with many threads logging.
    std::osyncstream(std::cout) << req.method << ' ' << req.target << " -> " << resp.status
                                << " (" << ms << " ms)\n";
  }
}

}  // namespace plgen
