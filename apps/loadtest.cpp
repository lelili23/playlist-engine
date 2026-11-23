// HTTP load generator for the engine (or any server that speaks HTTP/1.1).
//
//   plgen_loadtest --csv data/dataset.csv [--port 8080] [--concurrency 1,4,16,64] [--seconds 5]
//   plgen_loadtest --path "/health" --concurrency 32
//
// With --csv it fires GET /similar?song=<random real id>&k=20 requests; with --path
// it repeats one path. For each concurrency level, N client threads send
// requests back-to-back for a fixed time, then it reports throughput,
// latency percentiles, errors and 503s (the server shedding load).
//
// "Closed loop": each client waits for its response before sending the next
// request, like N users clicking as fast as they can.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "plgen/dataset.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 8080;
  std::string csv;
  std::string path;
  std::vector<int> concurrency{1, 4, 16, 64};
  double seconds = 5;
};

[[noreturn]] void usage() {
  std::cerr << "usage: plgen_loadtest (--csv PATH | --path /x) [--host H] [--port P] "
               "[--concurrency 1,4,16] [--seconds S]\n";
  std::exit(2);
}

enum class Outcome { kOk, kBusy, kError };

// One request on a fresh connection (the server closes after each response).
Outcome request(const sockaddr_in& addr, const std::string& raw) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return Outcome::kError;
  timeval tv{10, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  Outcome result = Outcome::kError;
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0 &&
      ::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(raw.size())) {
    std::string resp;
    char buf[8192];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof buf, 0)) > 0) resp.append(buf, static_cast<std::size_t>(n));
    if (resp.starts_with("HTTP/1.1 200")) result = Outcome::kOk;
    else if (resp.starts_with("HTTP/1.1 503")) result = Outcome::kBusy;
  }
  ::close(fd);
  return result;
}

struct ThreadResult {
  std::vector<double> latencies_ms;  // successful requests only
  std::size_t busy = 0, errors = 0;
};

}  // namespace

int main(int argc, char** argv) try {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) usage();
      return argv[++i];
    };
    if (arg == "--host") opt.host = value();
    else if (arg == "--port") opt.port = static_cast<std::uint16_t>(std::stoul(value()));
    else if (arg == "--csv") opt.csv = value();
    else if (arg == "--path") opt.path = value();
    else if (arg == "--seconds") opt.seconds = std::stod(value());
    else if (arg == "--concurrency") {
      opt.concurrency.clear();
      std::stringstream ss(value());
      for (std::string tok; std::getline(ss, tok, ',');) opt.concurrency.push_back(std::stoi(tok));
    } else usage();
  }
  if (opt.csv.empty() == opt.path.empty()) usage();

  // Build the request mix up front, so generating requests costs nothing during the test.
  std::vector<std::string> requests;
  if (!opt.csv.empty()) {
    const auto ds = plgen::load_spotify_csv(opt.csv);
    std::mt19937 rng(7);
    std::uniform_int_distribution<plgen::SongId> pick(0, static_cast<plgen::SongId>(ds.store.size() - 1));
    for (int i = 0; i < 5000; ++i) {
      requests.push_back("GET /similar?song=" + ds.store.meta(pick(rng)).spotify_id +
                         "&k=20 HTTP/1.1\r\nHost: x\r\n\r\n");
    }
  } else {
    requests.push_back("GET " + opt.path + " HTTP/1.1\r\nHost: x\r\n\r\n");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(opt.port);
  if (::inet_pton(AF_INET, opt.host.c_str(), &addr.sin_addr) != 1) throw std::runtime_error("bad host");
  if (request(addr, requests.front()) == Outcome::kError) {
    throw std::runtime_error("server not reachable on " + opt.host + ":" + std::to_string(opt.port));
  }

  std::cout << "clients    req/s    p50 ms   p90 ms   p99 ms     ok    503s  errors\n";
  for (const int clients : opt.concurrency) {
    std::vector<ThreadResult> results(static_cast<std::size_t>(clients));
    std::atomic<bool> go{false};
    const auto duration = std::chrono::duration<double>(opt.seconds);
    Clock::time_point start;
    {
      std::vector<std::jthread> threads;
      for (int c = 0; c < clients; ++c) {
        threads.emplace_back([&, c] {
          auto& r = results[static_cast<std::size_t>(c)];
          std::size_t next = static_cast<std::size_t>(c) * 97;  // each client walks the mix differently
          while (!go.load()) std::this_thread::yield();
          const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(duration);
          while (Clock::now() < deadline) {
            const auto t0 = Clock::now();
            const Outcome o = request(addr, requests[next++ % requests.size()]);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            if (o == Outcome::kOk) r.latencies_ms.push_back(ms);
            else if (o == Outcome::kBusy) ++r.busy;
            else ++r.errors;
          }
        });
      }
      start = Clock::now();
      go.store(true);  // release all clients at once
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

    std::vector<double> all;
    std::size_t busy = 0, errors = 0;
    for (auto& r : results) {
      all.insert(all.end(), r.latencies_ms.begin(), r.latencies_ms.end());
      busy += r.busy;
      errors += r.errors;
    }
    std::sort(all.begin(), all.end());
    auto pct = [&](double p) {
      return all.empty() ? 0.0 : all[static_cast<std::size_t>(p * static_cast<double>(all.size() - 1))];
    };
    std::cout << std::setw(7) << clients << std::fixed << std::setprecision(0) << std::setw(9)
              << static_cast<double>(all.size()) / elapsed << std::setprecision(2) << std::setw(10)
              << pct(0.50) << std::setw(9) << pct(0.90) << std::setw(9) << pct(0.99) << std::setw(7)
              << all.size() << std::setw(8) << busy << std::setw(8) << errors << '\n';
  }
  return 0;
} catch (const std::exception& e) {
  std::cerr << "error: " << e.what() << '\n';
  return 1;
}
