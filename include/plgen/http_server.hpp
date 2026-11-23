#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "plgen/http.hpp"

namespace plgen {

using Handler = std::function<HttpResponse(const HttpRequest&)>;

struct ServerOptions {
  std::uint16_t port = 8080;        // 0 = let the OS pick a free port (used by tests)
  std::size_t threads = 0;          // 0 = one per CPU core
  std::size_t max_queue = 1024;     // connections waiting for a worker before we answer 503
  int io_timeout_ms = 5000;         // give up on clients that stop sending/reading
  std::size_t max_request_bytes = 64 * 1024;
  bool log_requests = true;
};

// A small multithreaded HTTP/1.1 server on POSIX sockets (Linux / WSL / macOS).
//
//   main thread:   accept() new connections, hand each to the thread pool
//   worker thread: read request -> parse -> call handler -> write response -> close
//
// The handler is called from several threads at once, so it must be thread-safe.
// Ours only reads shared data (the song store and index), which is safe without locks.
class HttpServer {
 public:
  HttpServer(Handler handler, ServerOptions options);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Opens the listening socket. Throws std::runtime_error (e.g. port in use).
  void start();
  // Serves until request_stop(). Blocks the calling thread.
  void run();
  // Safe to call from another thread or a signal handler (just sets an atomic flag).
  void request_stop() noexcept { stop_.store(true); }

  [[nodiscard]] std::uint16_t port() const noexcept { return bound_port_; }

 private:
  void handle_connection(int fd) const;

  Handler handler_;
  ServerOptions opts_;
  int listen_fd_ = -1;
  std::uint16_t bound_port_ = 0;
  std::atomic<bool> stop_{false};
};

}  // namespace plgen
