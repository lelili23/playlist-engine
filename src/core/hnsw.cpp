#include "plgen/hnsw.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <queue>
#include <stdexcept>
#include <thread>

#include "plgen/distance.hpp"

namespace plgen {
namespace {

// "Have I already looked at this node during this search?"
//
// Clearing an 80k-entry array before every search would cost more than the
// search itself. Instead each entry stores the number of the search that last
// visited it; bumping `epoch` "clears" everything in O(1).
// thread_local: each server thread gets its own copy, so searches never share it.
class VisitedSet {
 public:
  void reset(std::size_t n) {
    if (marks_.size() < n) marks_.resize(n, 0);
    if (++epoch_ == 0) {  // wrapped around after 4 billion searches: really clear once
      std::fill(marks_.begin(), marks_.end(), 0);
      epoch_ = 1;
    }
  }
  // Returns true the first time `id` is seen in this search.
  bool insert(SongId id) {
    if (marks_[id] == epoch_) return false;
    marks_[id] = epoch_;
    return true;
  }

 private:
  std::vector<std::uint32_t> marks_;
  std::uint32_t epoch_ = 0;
};

thread_local VisitedSet t_visited;

// Comparators for std::priority_queue (whose top is the "largest" element).
struct FartherFirst {  // top = closest  -> a min-heap by distance
  bool operator()(const Neighbor& a, const Neighbor& b) const { return closer(b, a); }
};
struct CloserFirst {  // top = farthest -> a max-heap by distance (same trick as brute force)
  bool operator()(const Neighbor& a, const Neighbor& b) const { return closer(a, b); }
};

}  // namespace

HnswIndex::HnswIndex(const FeatureStore& store, HnswParams params, NoBuild)
    : store_(store),
      params_(params),
      level_mult_(1.0 / std::log(static_cast<double>(std::max<std::size_t>(params.M, 2)))),
      rng_(params.seed) {
  params_.M = std::max<std::size_t>(params_.M, 2);
}

HnswIndex::HnswIndex(const FeatureStore& store, HnswParams params)
    : HnswIndex(store, params, NoBuild{}) {
  // Draw every song's layer up front, in id order, so the layers are the same
  // however many threads build the graph.
  levels_.reserve(store.size());
  for (SongId id = 0; id < store.size(); ++id) levels_.push_back(random_level());
  allocate_links();
  if (store.size() == 0) return;

  insert(0);  // the first song becomes the entry point; nothing to search yet
  const std::size_t threads = std::min(std::max<std::size_t>(params_.build_threads, 1),
                                       static_cast<std::size_t>(store.size()));
  if (threads <= 1) {
    for (SongId id = 1; id < store.size(); ++id) insert(id);
  } else {
    build_parallel(threads);
  }
}

void HnswIndex::allocate_links() {
  // Allocate every song's link lists BEFORE any insert, so the outer vectors
  // never reallocate while threads are reading them.
  links_.resize(levels_.size());
  for (SongId id = 0; id < levels_.size(); ++id) {
    links_[id].resize(static_cast<std::size_t>(levels_[id]) + 1);
    for (int l = 0; l <= levels_[id]; ++l) {
      links_[id][static_cast<std::size_t>(l)].reserve(max_links(l) + 1);
    }
  }
}

void HnswIndex::build_parallel(std::size_t threads) {
  sync_ = std::make_unique<BuildSync>(store_.size());
  std::atomic<SongId> next{1};  // each thread grabs the next song to insert
  {
    std::vector<std::jthread> workers;
    for (std::size_t t = 0; t < threads; ++t) {
      workers.emplace_back([&] {
        for (SongId id = next++; id < store_.size(); id = next++) insert(id);
      });
    }
  }  // jthreads join here
  sync_.reset();  // graph is complete: searches from now on need no locks
}

void HnswIndex::copy_links(SongId id, int level, std::vector<SongId>& out) const {
  const auto& src = links_[id][static_cast<std::size_t>(level)];
  if (sync_) {
    const std::lock_guard lock(sync_->node[id]);
    out.assign(src.begin(), src.end());
  } else {
    out.assign(src.begin(), src.end());
  }
}

float HnswIndex::dist(SongId a, SongId b) const {
  return squared_l2(store_.vector(a), store_.vector(b));
}

int HnswIndex::random_level() {
  // Exponential decay: P(level >= L) = M^-L. With M = 16, about 1 in 16 songs
  // reach layer 1, 1 in 256 reach layer 2, 1 in 4096 reach layer 3, ...
  std::uniform_real_distribution<double> unif(0.0, 1.0);
  const double u = 1.0 - unif(rng_);  // (0, 1], so log never sees 0
  return static_cast<int>(-std::log(u) * level_mult_);
}

std::vector<Neighbor> HnswIndex::search_layer(VecView query, SongId entry, std::size_t ef,
                                              int level, SearchStats* stats) const {
  t_visited.reset(store_.size());
  t_visited.insert(entry);

  const Neighbor start{entry, squared_l2(query, store_.vector(entry))};
  std::size_t computed = 1;

  // `to_explore`: nodes whose neighbors we still need to check, closest first.
  // `best`:       the ef closest nodes found so far; top = the worst of them.
  std::priority_queue<Neighbor, std::vector<Neighbor>, FartherFirst> to_explore;
  std::priority_queue<Neighbor, std::vector<Neighbor>, CloserFirst> best;
  to_explore.push(start);
  best.push(start);

  while (!to_explore.empty()) {
    const Neighbor current = to_explore.top();
    // Stop when the closest unexplored node is already worse than everything we
    // kept: nothing reachable through it is likely to improve the result.
    if (best.size() >= ef && closer(best.top(), current)) break;
    to_explore.pop();
    if (stats) ++stats->hops;

    // During a parallel build another thread may be editing this list, so we
    // read a locked copy. After the build, read the list directly (no copy, no lock).
    thread_local std::vector<SongId> scratch;
    const std::vector<SongId>* nbs = &links_[current.id][static_cast<std::size_t>(level)];
    if (sync_) {
      copy_links(current.id, level, scratch);
      nbs = &scratch;
    }
    for (const SongId nb : *nbs) {
      if (!t_visited.insert(nb)) continue;
      const Neighbor cand{nb, squared_l2(query, store_.vector(nb))};
      ++computed;
      if (best.size() < ef || closer(cand, best.top())) {
        to_explore.push(cand);
        best.push(cand);
        if (best.size() > ef) best.pop();  // drop the worst
      }
    }
  }
  if (stats) stats->distance_computations += computed;

  std::vector<Neighbor> out(best.size());
  for (auto it = out.rbegin(); it != out.rend(); ++it) {  // drain worst-first, fill from the back
    *it = best.top();
    best.pop();
  }
  return out;
}

std::vector<SongId> HnswIndex::select_neighbors(const std::vector<Neighbor>& candidates,
                                                std::size_t max) const {
  // Plain "take the M closest" tends to link a song only to one tight cluster.
  // The heuristic keeps a candidate only if it is closer to the new song than to
  // any neighbor already chosen. Candidates hiding behind a chosen neighbor are
  // skipped, so links fan out in different directions, which keeps the graph
  // navigable across clusters.
  std::vector<SongId> chosen;
  chosen.reserve(max);
  for (const Neighbor& c : candidates) {
    if (chosen.size() >= max) break;
    bool diverse = true;
    for (const SongId s : chosen) {
      if (dist(c.id, s) < c.dist) {
        diverse = false;
        break;
      }
    }
    if (diverse) chosen.push_back(c.id);
  }
  return chosen;
}

void HnswIndex::insert(SongId id) {
  const int level = levels_[id];

  // Read the current entry point. In a parallel build it may change under us,
  // so read it under the global lock. If this song will become the new top
  // (rare: ~1 in M^level songs), keep holding the lock for the whole insert so
  // two threads can't both claim the top at once.
  std::unique_lock<std::mutex> global;
  if (sync_) global = std::unique_lock(sync_->global);
  SongId entry = entry_point_;
  const int top = max_level_;
  if (global && level <= top) global.unlock();

  if (top < 0) {  // the very first song
    entry_point_ = id;
    max_level_ = level;
    return;
  }

  const VecView q = store_.vector(id);

  // Phase 1: on layers above the new song's top layer, just greedily walk
  // toward it (ef = 1) to find a good starting point lower down.
  for (int l = top; l > level; --l) {
    entry = search_layer(q, entry, 1, l, nullptr).front().id;
  }

  // Phase 2: on each layer the new song lives on, find its neighbors and link both ways.
  for (int l = std::min(level, top); l >= 0; --l) {
    const auto candidates = search_layer(q, entry, params_.ef_construction, l, nullptr);
    const auto chosen = select_neighbors(candidates, params_.M);
    const auto lu = static_cast<std::size_t>(l);
    {
      std::unique_lock<std::mutex> own;
      if (sync_) own = std::unique_lock(sync_->node[id]);
      links_[id][lu] = chosen;
    }

    for (const SongId nb : chosen) {
      std::unique_lock<std::mutex> lock;
      if (sync_) lock = std::unique_lock(sync_->node[nb]);
      auto& nb_links = links_[nb][lu];
      nb_links.push_back(id);
      if (nb_links.size() > max_links(l)) {
        // Neighbor has too many links now: re-pick its best ones with the same heuristic.
        std::vector<Neighbor> pool;
        pool.reserve(nb_links.size());
        for (const SongId x : nb_links) pool.push_back({x, dist(nb, x)});
        std::sort(pool.begin(), pool.end(), closer);
        nb_links = select_neighbors(pool, max_links(l));
      }
    }
    entry = candidates.front().id;  // closest found becomes the start for the next layer down
  }

  if (level > top) {  // new tallest song becomes the global entry point (lock still held)
    max_level_ = level;
    entry_point_ = id;
  }
}

std::vector<Neighbor> HnswIndex::search(VecView query, std::size_t k, std::size_t ef,
                                        std::optional<SongId> exclude, SearchStats* stats) const {
  if (k == 0 || max_level_ < 0) return {};
  // Ask for one extra result if we'll drop the excluded song afterwards.
  const std::size_t want = k + (exclude ? 1 : 0);
  ef = std::max(ef, want);

  // Greedy descent through the sparse upper layers: long jumps, one candidate.
  SongId entry = entry_point_;
  for (int l = max_level_; l > 0; --l) {
    entry = search_layer(query, entry, 1, l, stats).front().id;
  }
  // Careful search on the full bottom layer.
  std::vector<Neighbor> found = search_layer(query, entry, ef, 0, stats);

  if (exclude) {
    std::erase_if(found, [&](const Neighbor& n) { return n.id == *exclude; });
  }
  if (found.size() > k) found.resize(k);
  return found;
}

std::size_t HnswIndex::link_memory_bytes() const noexcept {
  std::size_t bytes = 0;
  for (const auto& per_level : links_) {
    for (const auto& l : per_level) bytes += l.capacity() * sizeof(SongId);
  }
  return bytes;
}

// ---- Persistence ----
namespace {

constexpr char kIndexMagic[4] = {'P', 'L', 'G', 'H'};
constexpr std::uint32_t kIndexVersion = 1;

template <typename T>
void put(std::ofstream& out, const T& v) {
  out.write(reinterpret_cast<const char*>(&v), sizeof v);
}

template <typename T>
T take(std::ifstream& in) {
  T v{};
  in.read(reinterpret_cast<char*>(&v), sizeof v);
  if (!in) throw std::runtime_error("index file is truncated");
  return v;
}

}  // namespace

std::uint64_t HnswIndex::fingerprint(const FeatureStore& store) {
  std::uint64_t h = 1469598103934665603ULL;  // FNV-1a offset basis
  auto mix = [&](const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
      h ^= p[i];
      h *= 1099511628211ULL;  // FNV prime
    }
  };
  const std::uint64_t n = store.size(), dim = kDim;
  mix(&n, sizeof n);
  mix(&dim, sizeof dim);
  mix(store.data(), store.size() * kDim * sizeof(float));
  return h;
}

void HnswIndex::save(const std::filesystem::path& path) const {
  // Write to a temporary file, then rename: a crash mid-write can never leave
  // a half-written index where the real one should be.
  const auto tmp = std::filesystem::path(path).concat(".tmp");
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + tmp.string());
    out.write(kIndexMagic, sizeof kIndexMagic);
    put(out, kIndexVersion);
    put(out, static_cast<std::uint32_t>(kDim));
    put(out, static_cast<std::uint64_t>(levels_.size()));
    put(out, fingerprint(store_));
    put(out, static_cast<std::uint64_t>(params_.M));
    put(out, static_cast<std::uint64_t>(params_.ef_construction));
    put(out, params_.seed);
    put(out, static_cast<std::int32_t>(max_level_));
    put(out, entry_point_);
    for (SongId id = 0; id < levels_.size(); ++id) {
      put(out, static_cast<std::int32_t>(levels_[id]));
      for (const auto& ls : links_[id]) {
        put(out, static_cast<std::uint32_t>(ls.size()));
        out.write(reinterpret_cast<const char*>(ls.data()),
                  static_cast<std::streamsize>(ls.size() * sizeof(SongId)));
      }
    }
    if (!out) throw std::runtime_error("failed writing " + tmp.string());
  }
  std::filesystem::rename(tmp, path);
}

HnswIndex HnswIndex::load(const std::filesystem::path& path, const FeatureStore& store) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path.string());

  char magic[4];
  in.read(magic, sizeof magic);
  if (!in || std::memcmp(magic, kIndexMagic, sizeof magic) != 0) {
    throw std::runtime_error("not a playlist-engine index file");
  }
  if (take<std::uint32_t>(in) != kIndexVersion) throw std::runtime_error("unsupported index version");
  if (take<std::uint32_t>(in) != kDim) throw std::runtime_error("index built with different feature dimensions");
  const auto n = take<std::uint64_t>(in);
  if (n != store.size() || take<std::uint64_t>(in) != fingerprint(store)) {
    throw std::runtime_error("index was built for different song data; rebuild it");
  }

  HnswParams params;
  params.M = static_cast<std::size_t>(take<std::uint64_t>(in));
  params.ef_construction = static_cast<std::size_t>(take<std::uint64_t>(in));
  params.seed = take<std::uint32_t>(in);
  HnswIndex idx(store, params, NoBuild{});
  idx.max_level_ = take<std::int32_t>(in);
  idx.entry_point_ = take<SongId>(in);
  if (n > 0 && idx.entry_point_ >= n) throw std::runtime_error("corrupt index: bad entry point");

  // Never trust a file: check every count and id, so a corrupt file can't make
  // us read out of bounds later.
  idx.levels_.resize(n);
  idx.links_.resize(n);
  for (std::uint64_t id = 0; id < n; ++id) {
    const auto level = take<std::int32_t>(in);
    if (level < 0 || level > idx.max_level_) throw std::runtime_error("corrupt index: bad level");
    idx.levels_[id] = level;
    idx.links_[id].resize(static_cast<std::size_t>(level) + 1);
    for (int l = 0; l <= level; ++l) {
      const auto count = take<std::uint32_t>(in);
      if (count > idx.max_links(l)) throw std::runtime_error("corrupt index: too many links");
      auto& ls = idx.links_[id][static_cast<std::size_t>(l)];
      ls.resize(count);
      in.read(reinterpret_cast<char*>(ls.data()), static_cast<std::streamsize>(count * sizeof(SongId)));
      if (!in) throw std::runtime_error("index file is truncated");
      for (const SongId nb : ls) {
        if (nb >= n) throw std::runtime_error("corrupt index: link out of range");
      }
    }
  }
  if (n > 0 && idx.levels_[idx.entry_point_] != idx.max_level_) {
    throw std::runtime_error("corrupt index: entry point is not on the top layer");
  }
  return idx;
}

}  // namespace plgen
