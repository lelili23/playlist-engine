#pragma once

#include <cstddef>
#include <filesystem>
#include <istream>
#include <iosfwd>

#include "plgen/feature_store.hpp"
#include "plgen/normalizer.hpp"

namespace plgen {

struct LoadStats {
  std::size_t rows_read = 0;
  std::size_t dropped_malformed = 0;  // wrong field count or unparsable number
  std::size_t dropped_bad_tempo = 0;  // tempo <= 0
  std::size_t duplicate_rows = 0;     // same track_id seen again (extra genre merged)
  std::size_t duplicate_songs = 0;    // same name + artists under a different track_id
  std::size_t songs = 0;              // unique songs kept
};

std::ostream& operator<<(std::ostream& os, const LoadStats& s);

struct LoadedDataset {
  FeatureStore store;
  Normalizer normalizer;
  LoadStats stats;
};

// Loads the Kaggle "Spotify Tracks Dataset" (maharshipandya) CSV.
// Pipeline: parse -> filter -> dedupe by track_id -> fit normalizer -> transform -> store.
[[nodiscard]] LoadedDataset load_spotify_csv(const std::filesystem::path& path,
                                             FeatureWeights weights = {});
[[nodiscard]] LoadedDataset load_spotify_csv(std::istream& in, FeatureWeights weights = {});

}  // namespace plgen
