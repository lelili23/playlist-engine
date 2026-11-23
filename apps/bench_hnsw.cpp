// HNSW vs. brute force: build time, recall@k, latency and work per query.
//
//   plgen_bench_hnsw --csv data/dataset.csv [--groundtruth data/groundtruth_k20.bin]
//   plgen_bench_hnsw --synthetic 1000000 --queries 200
//   Options: --k 20 --M 16 --efc 200 --ef 10,20,40,80,160,320 --seed 42 --build-threads 1
//
// For each ef it prints one row. Recall@k is the fraction of the true k nearest
// songs (from brute force) that HNSW also returned: 1.000 = identical answers.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "plgen/brute_force.hpp"
#include "plgen/dataset.hpp"
#include "plgen/groundtruth.hpp"
#include "plgen/hnsw.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::string csv;
  std::size_t synthetic = 0;
  std::size_t queries = 1000;
  std::size_t k = 20;
  std::uint32_t seed = 42;
  std::string groundtruth;
  plgen::HnswParams hnsw;
  std::vector<std::size_t> efs{10, 20, 40, 80, 160, 320};
};

[[noreturn]] void usage() {
  std::cerr << "usage: plgen_bench_hnsw (--csv PATH | --synthetic N) [--groundtruth PATH] "
               "[--queries N] [--k K] [--M M] [--efc N] [--ef a,b,c] [--seed S]\n";
  std::exit(2);
}

Options parse_args(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) usage();
      return argv[++i];
    };
    if (arg == "--csv") o.csv = value();
    else if (arg == "--synthetic") o.synthetic = std::stoul(value());
    else if (arg == "--queries") o.queries = std::stoul(value());
    else if (arg == "--k") o.k = std::stoul(value());
    else if (arg == "--seed") o.seed = static_cast<std::uint32_t>(std::stoul(value()));
    else if (arg == "--groundtruth") o.groundtruth = value();
    else if (arg == "--M") o.hnsw.M = std::stoul(value());
    else if (arg == "--efc") o.hnsw.ef_construction = std::stoul(value());
    else if (arg == "--build-threads") o.hnsw.build_threads = std::stoul(value());
    else if (arg == "--ef") {
      o.efs.clear();
      std::stringstream ss(value());
      for (std::string tok; std::getline(ss, tok, ',');) o.efs.push_back(std::stoul(tok));
    } else usage();
  }
  if (o.csv.empty() == (o.synthetic == 0)) usage();
  return o;
}

plgen::FeatureStore make_synthetic(std::size_t n, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  plgen::FeatureStore store;
  store.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    plgen::Vec v;
    for (auto& x : v) x = u(rng);
    store.add({.spotify_id = "syn" + std::to_string(i)}, v);
  }
  return store;
}

struct Latency {
  double mean, p50, p99, qps;
};

Latency summarize(std::vector<double> us, double total_s) {
  std::sort(us.begin(), us.end());
  double sum = 0;
  for (double x : us) sum += x;
  auto pct = [&](double p) { return us[static_cast<std::size_t>(p * static_cast<double>(us.size() - 1))]; };
  return {sum / static_cast<double>(us.size()), pct(0.50), pct(0.99),
          static_cast<double>(us.size()) / total_s};
}

}  // namespace

int main(int argc, char** argv) try {
  Options opt = parse_args(argc, argv);

  std::optional<plgen::LoadedDataset> loaded;
  plgen::FeatureStore synthetic;
  if (!opt.csv.empty()) loaded.emplace(plgen::load_spotify_csv(opt.csv));
  else synthetic = make_synthetic(opt.synthetic, opt.seed);
  const plgen::FeatureStore& store = loaded ? loaded->store : synthetic;
  std::cout << "songs: " << store.size() << ", dims: " << plgen::kDim << "\n";

  // ---- Queries + exact answers ----
  std::vector<plgen::SongId> queries;
  std::vector<std::vector<plgen::SongId>> truth;
  if (!opt.groundtruth.empty()) {
    auto gt = plgen::read_groundtruth(opt.groundtruth);
    queries = std::move(gt.queries);
    truth = std::move(gt.answers);
    opt.k = gt.k;
    std::cout << "ground truth: " << queries.size() << " queries from " << opt.groundtruth << "\n";
  } else {
    std::mt19937 rng(opt.seed);
    std::uniform_int_distribution<plgen::SongId> pick(0, static_cast<plgen::SongId>(store.size() - 1));
    queries.resize(opt.queries);
    for (auto& q : queries) q = pick(rng);
  }
  for (auto q : queries) {
    if (q >= store.size()) throw std::runtime_error("ground truth doesn't match this dataset; regenerate it");
  }

  // ---- Brute force baseline (also produces truth if we don't have it) ----
  std::vector<double> bf_us;
  std::uint64_t checksum = 0;
  const auto t_bf = Clock::now();
  for (std::size_t i = 0; i < queries.size(); ++i) {
    const auto t0 = Clock::now();
    const auto r = plgen::brute_force_knn(store, store.vector(queries[i]), opt.k, queries[i]);
    bf_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    for (const auto& n : r) checksum += n.id;
    if (opt.groundtruth.empty()) {
      std::vector<plgen::SongId> ids;
      for (const auto& n : r) ids.push_back(n.id);
      truth.push_back(std::move(ids));
    }
  }
  const auto bf = summarize(bf_us, std::chrono::duration<double>(Clock::now() - t_bf).count());

  // ---- Build ----
  const auto t_build = Clock::now();
  const plgen::HnswIndex index(store, opt.hnsw);
  const double build_s = std::chrono::duration<double>(Clock::now() - t_build).count();
  std::cout << std::fixed << std::setprecision(2) << "HNSW build: " << build_s << " s (M="
            << opt.hnsw.M << ", ef_construction=" << opt.hnsw.ef_construction
            << ", threads=" << opt.hnsw.build_threads << "), layers: " << index.max_level() + 1 << ", link memory: "
            << static_cast<double>(index.link_memory_bytes()) / (1024.0 * 1024.0) << " MiB\n\n";

  // ---- Table ----
  std::cout << "  method       recall@" << opt.k
            << "   mean us    p50 us    p99 us        QPS   distances/query  speedup\n";
  auto row = [&](const std::string& name, double recall, const Latency& l, double dists) {
    std::cout << std::setw(14) << std::left << name << std::right << std::setprecision(3)
              << std::setw(9) << recall << std::setprecision(1) << std::setw(10) << l.mean
              << std::setw(10) << l.p50 << std::setw(10) << l.p99 << std::setprecision(0)
              << std::setw(11) << l.qps << std::setw(18) << dists << std::setprecision(1)
              << std::setw(8) << bf.mean / l.mean << "x\n";
  };
  row("brute force", 1.0, bf, static_cast<double>(store.size()));

  for (const std::size_t ef : opt.efs) {
    std::vector<double> us;
    double recall = 0;
    plgen::SearchStats stats;
    const auto t_all = Clock::now();
    for (std::size_t i = 0; i < queries.size(); ++i) {
      const auto t0 = Clock::now();
      const auto r = index.search(store.vector(queries[i]), opt.k, ef, queries[i], &stats);
      us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
      std::vector<plgen::SongId> ids;
      for (const auto& n : r) {
        ids.push_back(n.id);
        checksum += n.id;
      }
      recall += plgen::recall_at_k(ids, truth[i]);
    }
    const auto l = summarize(us, std::chrono::duration<double>(Clock::now() - t_all).count());
    const auto nq = static_cast<double>(queries.size());
    row("HNSW ef=" + std::to_string(ef), recall / nq, l,
        static_cast<double>(stats.distance_computations) / nq);
  }
  std::cout << "\nchecksum " << checksum << "\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << "error: " << e.what() << '\n';
  return 1;
}
