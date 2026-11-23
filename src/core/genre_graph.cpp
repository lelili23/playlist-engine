#include "plgen/genre_graph.hpp"

#include <algorithm>

namespace plgen {

GenreGraph GenreGraph::build(const FeatureStore& store) {
  GenreGraph g;
  // Pass 1: give every genre a dense id and count its songs.
  for (SongId id = 0; id < store.size(); ++id) {
    for (const auto& name : store.meta(id).genres) {
      const auto [it, inserted] =
          g.id_of_.emplace(name, static_cast<std::uint32_t>(g.counts_.size()));
      if (inserted) g.counts_.push_back(0);
      ++g.counts_[it->second];
    }
  }

  // Pass 2: count co-occurrences in a flat G×G matrix (row-major, like FeatureStore).
  const std::size_t n = g.counts_.size();
  g.both_.assign(n * n, 0);
  std::vector<std::uint32_t> ids;
  for (SongId id = 0; id < store.size(); ++id) {
    ids.clear();
    for (const auto& name : store.meta(id).genres) ids.push_back(g.id_of_.at(name));
    for (std::size_t i = 0; i < ids.size(); ++i) {
      for (std::size_t j = i + 1; j < ids.size(); ++j) {
        if (ids[i] == ids[j]) continue;
        ++g.both_[ids[i] * n + ids[j]];
        ++g.both_[ids[j] * n + ids[i]];  // keep the matrix symmetric
      }
    }
    std::vector<GenreId> song_ids;
    for (const std::uint32_t gid : ids) song_ids.push_back(static_cast<GenreId>(gid));
    g.song_genres_.push_back(std::move(song_ids));
  }

  // Precompute every pair's relatedness once, so re-ranking is just a lookup.
  g.rel_.assign(n * n, 0.0f);
  for (std::size_t a = 0; a < n; ++a) {
    for (std::size_t b = 0; b < n; ++b) {
      const std::uint32_t smaller = std::min(g.counts_[a], g.counts_[b]);
      g.rel_[a * n + b] = a == b ? 1.0f
                          : smaller == 0
                              ? 0.0f
                              : static_cast<float>(g.both_[a * n + b]) / static_cast<float>(smaller);
    }
  }
  return g;
}

std::vector<GenreGraph::GenreId> GenreGraph::ids_of(const std::vector<std::string>& names) const {
  std::vector<GenreId> out;
  for (const auto& name : names) {
    if (const auto it = id_of_.find(name); it != id_of_.end()) {
      out.push_back(static_cast<GenreId>(it->second));
    }
  }
  return out;
}

float GenreGraph::best_relatedness(const std::vector<GenreId>& wanted, SongId song) const {
  const std::size_t n = counts_.size();
  float best = 0.0f;
  for (const GenreId a : wanted) {
    for (const GenreId b : song_genres_[song]) best = std::max(best, rel_[a * n + b]);
  }
  return best;
}

float GenreGraph::relatedness(const std::string& a, const std::string& b) const {
  if (a == b) return 1.0f;
  const auto ia = id_of_.find(a), ib = id_of_.find(b);
  if (ia == id_of_.end() || ib == id_of_.end()) return 0.0f;
  const std::uint32_t both = both_[ia->second * counts_.size() + ib->second];
  const std::uint32_t smaller = std::min(counts_[ia->second], counts_[ib->second]);
  return smaller == 0 ? 0.0f : static_cast<float>(both) / static_cast<float>(smaller);
}

float GenreGraph::best_relatedness(const std::vector<std::string>& wanted,
                                   const SongMeta& song) const {
  float best = 0.0f;
  for (const auto& ga : wanted) {
    for (const auto& gb : song.genres) best = std::max(best, relatedness(ga, gb));
  }
  return best;
}

float GenreGraph::best_relatedness(const SongMeta& a, const SongMeta& b) const {
  return best_relatedness(a.genres, b);
}

std::vector<std::string> GenreGraph::names() const {
  std::vector<std::string> out;
  out.reserve(id_of_.size());
  for (const auto& [name, id] : id_of_) out.push_back(name);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace plgen
