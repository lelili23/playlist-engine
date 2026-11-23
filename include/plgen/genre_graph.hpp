#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "plgen/feature_store.hpp"

namespace plgen {

// Learns how related genres are from the data itself, with no hand-made table.
//
// About 1 in 6 songs carry several genre tags (e.g. alt-rock + alternative +
// rock). If two genres often tag the SAME songs, they're probably related.
// We measure that with the overlap coefficient:
//
//     relatedness(a, b) = songs tagged with both / songs in the smaller genre
//
// It's 1.0 for a genre with itself, ~0.65 for alt-rock vs alternative,
// ~0.2 for alt-rock vs rock, and 0 for alt-rock vs sertanejo.
class GenreGraph {
 public:
  [[nodiscard]] static GenreGraph build(const FeatureStore& store);

  // 0..1. Unknown genres are unrelated to everything except themselves.
  [[nodiscard]] float relatedness(const std::string& a, const std::string& b) const;

  // Highest relatedness between any genre of song a and any genre of song b.
  [[nodiscard]] float best_relatedness(const SongMeta& a, const SongMeta& b) const;
  [[nodiscard]] float best_relatedness(const std::vector<std::string>& wanted,
                                       const SongMeta& song) const;

  // All genre names, sorted (for the /genres endpoint and the LLM prompt).
  [[nodiscard]] std::vector<std::string> names() const;

  [[nodiscard]] std::size_t genre_count() const noexcept { return counts_.size(); }

  // ---- Fast path for re-ranking (no string comparisons or hashing) ----
  using GenreId = std::uint16_t;
  // Genre names -> ids. Unknown names are skipped.
  [[nodiscard]] std::vector<GenreId> ids_of(const std::vector<std::string>& names) const;
  // Same as best_relatedness(names, store.meta(song)) but with precomputed ids:
  // a few array lookups instead of hashing strings.
  [[nodiscard]] float best_relatedness(const std::vector<GenreId>& wanted, SongId song) const;

 private:
  std::unordered_map<std::string, std::uint32_t> id_of_;  // genre name -> dense index
  std::vector<std::uint32_t> counts_;                     // songs per genre
  std::vector<std::uint32_t> both_;                       // G×G matrix: songs tagged with both
  std::vector<float> rel_;                  // G×G relatedness, precomputed from counts_ and both_
  std::vector<std::vector<GenreId>> song_genres_;  // song id -> its genre ids
};

}  // namespace plgen
