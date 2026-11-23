#include "plgen/feature_store.hpp"

#include <limits>
#include <stdexcept>

namespace plgen {

void FeatureStore::reserve(std::size_t n) {
  data_.reserve(n * kDim);
  meta_.reserve(n);
  by_spotify_id_.reserve(n);
}

SongId FeatureStore::add(SongMeta meta, const Vec& vec) {
  if (meta_.size() >= std::numeric_limits<SongId>::max()) {
    throw std::length_error("FeatureStore: too many songs for 32-bit SongId");
  }
  const auto id = static_cast<SongId>(meta_.size());
  const auto [it, inserted] = by_spotify_id_.emplace(meta.spotify_id, id);
  if (!inserted) throw std::invalid_argument("FeatureStore: duplicate id " + meta.spotify_id);

  data_.insert(data_.end(), vec.begin(), vec.end());
  meta_.push_back(std::move(meta));  // move: no string copies
  return id;
}

VecView FeatureStore::vector(SongId id) const {
  if (id >= meta_.size()) throw std::out_of_range("FeatureStore: bad SongId");
  return VecView(data_.data() + static_cast<std::size_t>(id) * kDim, kDim);
}

std::optional<SongId> FeatureStore::find(const std::string& spotify_id) const {
  const auto it = by_spotify_id_.find(spotify_id);
  if (it == by_spotify_id_.end()) return std::nullopt;
  return it->second;
}

}  // namespace plgen
