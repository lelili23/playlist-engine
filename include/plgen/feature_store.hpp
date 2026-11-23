#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "plgen/types.hpp"

namespace plgen {

struct SongMeta {
  std::string spotify_id{};
  std::string name{};
  std::string artists{};              // as in the CSV: "Artist A;Artist B"
  std::vector<std::string> genres{};  // a track can be tagged with several genres
  int popularity = 0;                 // 0..100 from Spotify, used when re-ranking
};

// Owns every song's vector and metadata.
//
// Struct-of-arrays layout: all vectors live in ONE contiguous float array,
// separate from the (string-heavy) metadata. Search only touches vectors, so
// it streams through memory in order — cache- and prefetcher-friendly.
class FeatureStore {
 public:
  void reserve(std::size_t n);

  // Throws std::invalid_argument if spotify_id is already present.
  SongId add(SongMeta meta, const Vec& vec);

  // WARNING: the returned span points into internal storage. A later add()
  // may reallocate and leave it dangling. Build the store fully, then search.
  [[nodiscard]] VecView vector(SongId id) const;

  [[nodiscard]] const SongMeta& meta(SongId id) const { return meta_.at(id); }
  [[nodiscard]] SongMeta& meta(SongId id) { return meta_.at(id); }

  [[nodiscard]] std::optional<SongId> find(const std::string& spotify_id) const;
  [[nodiscard]] std::size_t size() const noexcept { return meta_.size(); }

  // Raw access for tight loops (brute force, SIMD, serialization later).
  [[nodiscard]] const float* data() const noexcept { return data_.data(); }

 private:
  std::vector<float> data_;  // size() * kDim floats, row-major
  std::vector<SongMeta> meta_;
  std::unordered_map<std::string, SongId> by_spotify_id_;
};

}  // namespace plgen
