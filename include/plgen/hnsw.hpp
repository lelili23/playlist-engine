#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <vector>

#include "plgen/feature_store.hpp"
#include "plgen/types.hpp"

namespace plgen {

// HNSW: Hierarchical Navigable Small World graph (Malkov & Yashunin, 2016).
//
// The idea in three sentences:
//  - Every song is a node, linked to a handful of songs that are close to it.
//  - To search, start somewhere and keep hopping to whichever neighbor is
//    closer to the query, until no neighbor is closer.
//  - To make the hops long at first and short at the end, songs are stacked in
//    layers: every song is on layer 0, a random ~1/M of them also on layer 1,
//    ~1/M² on layer 2, ... Search starts at the sparse top layer (big jumps
//    across the catalog) and drops down a layer whenever it gets stuck.
//
// Tuning knobs:
//   M                max links per song per layer (layer 0 allows 2·M).
//                    More links = better recall, more memory, slower build.
//   ef_construction  how hard to look for good neighbors while building.
//   ef (at search)   how many candidates to keep while searching. The main
//                    speed vs. accuracy dial: higher = better recall, slower.
struct HnswParams {
  std::size_t M = 16;
  std::size_t ef_construction = 200;
  std::uint32_t seed = 42;  // layer assignment is random; a fixed seed makes builds repeatable
  // Threads used to build. 1 = fully deterministic (same seed, same graph).
  // More threads build faster; the graph then depends on thread timing, but
  // its quality (recall) is the same.
  std::size_t build_threads = 1;
};

// Optional counters, so you can see how much work a search actually did.
struct SearchStats {
  std::size_t distance_computations = 0;  // brute force does one per song
  std::size_t hops = 0;                   // nodes whose neighbor lists were expanded
};

class HnswIndex {
 public:
  // Builds the index over every song in `store`. The store must outlive the
  // index and must not change afterwards (we keep a reference, not a copy).
  explicit HnswIndex(const FeatureStore& store, HnswParams params = {});

  // ---- Persistence: build once, start instantly afterwards ----
  // Binary format with a fingerprint of the song vectors, so an index built
  // for different data (or different features) is refused instead of silently
  // returning garbage. Throws std::runtime_error on any problem.
  void save(const std::filesystem::path& path) const;
  [[nodiscard]] static HnswIndex load(const std::filesystem::path& path, const FeatureStore& store);

  // 64-bit FNV-1a hash of every vector in the store.
  [[nodiscard]] static std::uint64_t fingerprint(const FeatureStore& store);

  // Approximate k nearest neighbors, closest first. `ef` is raised to at least k.
  // Thread-safe: many threads may search at once (the HTTP server will).
  [[nodiscard]] std::vector<Neighbor> search(VecView query, std::size_t k, std::size_t ef,
                                             std::optional<SongId> exclude = std::nullopt,
                                             SearchStats* stats = nullptr) const;

  [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }
  [[nodiscard]] int max_level() const noexcept { return max_level_; }
  [[nodiscard]] int level_of(SongId id) const { return levels_.at(id); }
  [[nodiscard]] const std::vector<SongId>& links(SongId id, int level) const {
    return links_.at(id).at(static_cast<std::size_t>(level));
  }
  [[nodiscard]] std::size_t max_links(int level) const noexcept {
    return level == 0 ? 2 * params_.M : params_.M;
  }
  // Approximate bytes used by the graph links (vectors live in the FeatureStore).
  [[nodiscard]] std::size_t link_memory_bytes() const noexcept;

 private:
  struct NoBuild {};  // tag: construct an empty index (used by load)
  HnswIndex(const FeatureStore& store, HnswParams params, NoBuild);

  [[nodiscard]] int random_level();
  void allocate_links();
  void build_parallel(std::size_t threads);
  void insert(SongId id);
  // Copies node `id`'s layer-`level` links into `out`, locking during a parallel build.
  void copy_links(SongId id, int level, std::vector<SongId>& out) const;

  // Best-first search within one layer, starting from `entry`. Returns up to
  // `ef` closest nodes found, sorted closest first.
  [[nodiscard]] std::vector<Neighbor> search_layer(VecView query, SongId entry, std::size_t ef,
                                                   int level, SearchStats* stats) const;

  // Picks at most `max` neighbors from `candidates` (sorted closest first),
  // preferring ones that point in DIFFERENT directions (the HNSW heuristic).
  [[nodiscard]] std::vector<SongId> select_neighbors(const std::vector<Neighbor>& candidates,
                                                     std::size_t max) const;

  [[nodiscard]] float dist(SongId a, SongId b) const;

  const FeatureStore& store_;
  HnswParams params_;
  double level_mult_;  // 1 / ln(M): makes each layer ~M times sparser than the one below
  std::mt19937 rng_;

  std::vector<int> levels_;                           // top layer of each song
  std::vector<std::vector<std::vector<SongId>>> links_;  // links_[song][layer] = neighbor ids
  SongId entry_point_ = 0;  // a song on the top layer; every search starts here
  int max_level_ = -1;      // -1 = empty index

  // Locks that exist only while a multi-threaded build runs. One mutex per song
  // guards that song's link lists; `global` guards entry_point_ / max_level_.
  // Searches after the build never lock: the graph is read-only by then.
  struct BuildSync {
    explicit BuildSync(std::size_t n) : node(new std::mutex[n]) {}
    std::unique_ptr<std::mutex[]> node;
    std::mutex global;
  };
  std::unique_ptr<BuildSync> sync_;
};

}  // namespace plgen
