// Brute-force k-NN benchmark + ground-truth generator.
//
//   plgen_bench --csv data/dataset.csv [--queries 1000] [--k 20] [--seed 42]
//               [--groundtruth data/groundtruth_k20.bin]
//   plgen_bench --synthetic 1000000      # random songs, to see how cost scales with n
//
// Reports per-query latency percentiles and QPS. With --groundtruth, also saves
// the exact answers so the HNSW benchmark can measure recall@k against them.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "plgen/brute_force.hpp"
#include "plgen/dataset.hpp"
#include "plgen/groundtruth.hpp"

namespace {

using Clock = std::chrono::steady_clock;  // monotonic: never jumps like system_clock

struct Options {
  std::string csv;
  std::size_t synthetic = 0;
  std::size_t queries = 1000;
  std::size_t k = 20;
  std::uint32_t seed = 42;
  std::string groundtruth;
};

[[noreturn]] void usage() {
  std::cerr << "usage: plgen_bench (--csv PATH | --synthetic N) [--queries N] [--k K] "
               "[--seed S] [--groundtruth PATH]\n";
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
    else usage();
  }
  if (o.csv.empty() == (o.synthetic == 0)) usage();  // exactly one data source
  return o;
}

// Random songs drawn from plausible ranges, pushed through the same Normalizer.
plgen::FeatureStore make_synthetic(std::size_t n, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  std::uniform_real_distribution<float> tempo(60.0f, 200.0f);
  std::uniform_int_distribution<int> key(0, 11);
  std::uniform_int_distribution<int> mode(0, 1);
  const plgen::Normalizer norm(60.0f, 200.0f);

  plgen::FeatureStore store;
  store.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    plgen::RawFeatures raw;
    raw.energy = unit(rng);
    raw.valence = unit(rng);
    raw.danceability = unit(rng);
    raw.acousticness = unit(rng);
    raw.tempo = tempo(rng);
    raw.key = key(rng);
    raw.mode = mode(rng);
    raw.speechiness = unit(rng);
    raw.instrumentalness = unit(rng);
    store.add({.spotify_id = "syn" + std::to_string(i)}, norm.transform(raw));
  }
  return store;
}

double percentile(const std::vector<double>& sorted, double p) {
  const auto idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
  return sorted[idx];
}

}  // namespace

int main(int argc, char** argv) try {
  const Options opt = parse_args(argc, argv);

  // ---- Load ----
  const auto t_load = Clock::now();
  std::optional<plgen::LoadedDataset> loaded;
  plgen::FeatureStore synthetic_store;
  if (!opt.csv.empty()) {
    loaded.emplace(plgen::load_spotify_csv(opt.csv));
  } else {
    synthetic_store = make_synthetic(opt.synthetic, opt.seed);
  }
  const plgen::FeatureStore& store = loaded ? loaded->store : synthetic_store;
  const double load_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_load).count();

  std::cout << "== data ==\n";
  if (loaded) {
    std::cout << loaded->stats << "tempo clip range:   " << loaded->normalizer.tempo_lo()
              << " .. " << loaded->normalizer.tempo_hi() << " BPM\n";
  }
  std::cout << "songs: " << store.size() << ", dims: " << plgen::kDim
            << ", vector memory: " << std::fixed << std::setprecision(2)
            << static_cast<double>(store.size() * plgen::kDim * sizeof(float)) / (1024.0 * 1024.0)
            << " MiB, load time: " << load_ms << " ms\n";

  if (store.size() < 2) {
    std::cerr << "need at least 2 songs\n";
    return 1;
  }

  // ---- Pick queries (fixed seed => same queries every run) ----
  std::mt19937 rng(opt.seed);
  std::uniform_int_distribution<plgen::SongId> pick(0,
                                                    static_cast<plgen::SongId>(store.size() - 1));
  std::vector<plgen::SongId> queries(opt.queries);
  for (auto& q : queries) q = pick(rng);

  // ---- Warm up caches, branch predictors and CPU frequency ----
  std::uint64_t checksum = 0;
  for (std::size_t i = 0; i < std::min<std::size_t>(50, queries.size()); ++i) {
    const auto r = plgen::brute_force_knn(store, store.vector(queries[i]), opt.k, queries[i]);
    checksum += r.empty() ? 0 : r.front().id;
  }

  // ---- Timed run ----
  std::vector<double> latencies_us;
  latencies_us.reserve(queries.size());
  plgen::GroundTruth gt{.k = static_cast<std::uint32_t>(opt.k), .queries = queries, .answers = {}};
  gt.answers.reserve(queries.size());

  const auto t_all = Clock::now();
  for (const plgen::SongId q : queries) {
    const auto t0 = Clock::now();
    const auto result = plgen::brute_force_knn(store, store.vector(q), opt.k, q);
    const auto t1 = Clock::now();
    latencies_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());

    // Use the result so the optimizer can't delete the work we're timing.
    for (const auto& nb : result) checksum += nb.id;

    std::vector<plgen::SongId> ids;
    ids.reserve(result.size());
    for (const auto& nb : result) ids.push_back(nb.id);
    gt.answers.push_back(std::move(ids));
  }
  const double total_s = std::chrono::duration<double>(Clock::now() - t_all).count();

  std::sort(latencies_us.begin(), latencies_us.end());
  double sum = 0;
  for (double l : latencies_us) sum += l;

  std::cout << "\n== brute force k=" << opt.k << ", " << queries.size() << " queries ==\n"
            << std::setprecision(1) << "mean " << sum / static_cast<double>(latencies_us.size())
            << " us | p50 " << percentile(latencies_us, 0.50) << " us | p95 "
            << percentile(latencies_us, 0.95) << " us | p99 " << percentile(latencies_us, 0.99)
            << " us | QPS (1 thread) " << std::setprecision(0)
            << static_cast<double>(queries.size()) / total_s << '\n'
            << "checksum " << checksum << '\n';

  // ---- Sanity check: print one query's neighbors (real data only) ----
  if (loaded) {
    const plgen::SongId q = queries.front();
    const auto& m = store.meta(q);
    std::cout << "\nexample: \"" << m.name << "\" by " << m.artists << " [" << m.genres.front()
              << "]\n";
    const auto r = plgen::brute_force_knn(store, store.vector(q), 5, q);
    std::cout << std::setprecision(4);
    for (const auto& nb : r) {
      const auto& nm = store.meta(nb.id);
      std::cout << "  " << nb.dist << "  " << nm.name << " - " << nm.artists << " ["
                << nm.genres.front() << "]\n";
    }
  }

  if (!opt.groundtruth.empty()) {
    plgen::write_groundtruth(opt.groundtruth, gt);
    std::cout << "\nwrote ground truth to " << opt.groundtruth << '\n';
  }
  return 0;
} catch (const std::exception& e) {
  std::cerr << "error: " << e.what() << '\n';
  return 1;
}
