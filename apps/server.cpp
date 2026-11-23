// The Playlist Engine web server.
//
//   plgen_server [--csv data/dataset.csv] [--port 8080] [--threads N] [--quiet]
//                [--index data/index.bin] [--build-threads N]
//
// With --index, the first start builds the HNSW index and saves it; later starts
// load it in well under a second.
//
// Then open http://localhost:8080/search?q=somebody%20else in a browser.
// Stop with Ctrl+C: in-flight requests finish before it exits.

#include <chrono>
#include <csignal>
#include <filesystem>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "plgen/api.hpp"
#include "plgen/http_server.hpp"

namespace {

plgen::HttpServer* g_server = nullptr;  // for the signal handler only

extern "C" void on_signal(int) {
  // Signal handlers may only do async-signal-safe things; storing to a
  // lock-free std::atomic<bool> qualifies.
  if (g_server) g_server->request_stop();
}

}  // namespace

int main(int argc, char** argv) try {
  std::string csv = "data/dataset.csv";
  std::filesystem::path index_path;
  plgen::HnswParams hnsw;
  hnsw.build_threads = std::max(1u, std::thread::hardware_concurrency());
  plgen::ServerOptions opts;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error("missing value for " + std::string(arg));
      return argv[++i];
    };
    if (arg == "--csv") csv = value();
    else if (arg == "--port") opts.port = static_cast<std::uint16_t>(std::stoul(value()));
    else if (arg == "--threads") opts.threads = std::stoul(value());
    else if (arg == "--quiet") opts.log_requests = false;
    else if (arg == "--index") index_path = value();
    else if (arg == "--build-threads") hnsw.build_threads = std::stoul(value());
    else throw std::runtime_error("unknown option " + std::string(arg));
  }

  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  std::cout << "loading " << csv << " ...\n";
  const plgen::Engine engine(plgen::load_spotify_csv(csv), hnsw, {}, index_path);
  std::cout << "ready: " << engine.store().size() << " songs, index " << engine.index_origin()
            << ", startup " << std::chrono::duration<double>(Clock::now() - t0).count() << " s\n";

  // The lambda captures `engine` by reference: every worker thread calls into
  // the same read-only engine.
  plgen::HttpServer server([&engine](const plgen::HttpRequest& r) { return engine.handle(r); },
                           opts);
  server.start();
  g_server = &server;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::cout << "listening on http://localhost:" << server.port() << "  (Ctrl+C to stop)\n"
            << "  try: http://localhost:" << server.port() << "/search?q=somebody%20else\n";
  server.run();
  std::cout << "\nshut down cleanly\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << "error: " << e.what() << '\n';
  return 1;
}
