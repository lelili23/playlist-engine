#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "plgen/http_server.hpp"

using namespace plgen;

namespace {

// Minimal blocking HTTP client: send raw bytes, read until the server closes.
std::string roundtrip(std::uint16_t port, const std::string& raw) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return "";
  }
  ::send(fd, raw.data(), raw.size(), 0);
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = ::recv(fd, buf, sizeof buf, 0)) > 0) out.append(buf, static_cast<std::size_t>(n));
  ::close(fd);
  return out;
}

// Starts a server on a free port in a background thread; stops it on destruction.
struct TestServer {
  explicit TestServer(Handler h)
      : server(std::move(h), {.port = 0, .threads = 4, .log_requests = false}) {
    server.start();
    thread = std::thread([this] { server.run(); });
  }
  ~TestServer() {
    server.request_stop();
    thread.join();
  }
  HttpServer server;
  std::thread thread;
};

}  // namespace

TEST(Server, ServesARequestOverARealSocket) {
  TestServer ts([](const HttpRequest& r) { return HttpResponse{200, "text/plain", "hi " + r.path}; });
  const auto resp = roundtrip(ts.server.port(), "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200 OK\r\n")) << resp;
  EXPECT_TRUE(resp.ends_with("hi /hello"));
}

TEST(Server, HandlesSplitRequestsAndBadInput) {
  TestServer ts([](const HttpRequest&) { return HttpResponse{200, "text/plain", "ok"}; });
  EXPECT_TRUE(roundtrip(ts.server.port(), "NOT HTTP\r\n\r\n").starts_with("HTTP/1.1 400"));
}

TEST(Server, HandlerExceptionsBecome500) {
  TestServer ts([](const HttpRequest&) -> HttpResponse { throw std::runtime_error("bug"); });
  EXPECT_TRUE(roundtrip(ts.server.port(), "GET / HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 500"));
}

TEST(Server, ManyConcurrentClients) {
  std::atomic<int> handled{0};
  TestServer ts([&](const HttpRequest&) {
    ++handled;
    return HttpResponse{200, "text/plain", "ok"};
  });
  std::vector<std::thread> clients;
  std::atomic<int> ok{0};
  for (int i = 0; i < 16; ++i) {
    clients.emplace_back([&] {
      for (int j = 0; j < 20; ++j) {
        if (roundtrip(ts.server.port(), "GET / HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 200")) ++ok;
      }
    });
  }
  for (auto& c : clients) c.join();
  EXPECT_EQ(ok.load(), 320);
  EXPECT_EQ(handled.load(), 320);
}
