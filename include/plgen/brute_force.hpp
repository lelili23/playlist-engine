#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "plgen/feature_store.hpp"
#include "plgen/types.hpp"

namespace plgen {

// Exact k-nearest-neighbor search: compares the query against every song.
// O(n·d + n·log k) time, O(k) extra memory.
// Returns min(k, n) results sorted closest-first (ties by smaller id).
// `exclude` skips one id — typically the query song itself.
[[nodiscard]] std::vector<Neighbor> brute_force_knn(const FeatureStore& store, VecView query,
                                                    std::size_t k,
                                                    std::optional<SongId> exclude = std::nullopt);

}  // namespace plgen
