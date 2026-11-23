#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "plgen/types.hpp"

namespace plgen {

// Exact top-k answers for a fixed set of queries, produced by brute force.
// The HNSW benchmark (milestone 3) loads this to measure recall@k.
struct GroundTruth {
  std::uint32_t k = 0;
  std::vector<SongId> queries;              // query song ids
  std::vector<std::vector<SongId>> answers;  // answers[i] = exact top-k for queries[i]
};

// Binary format (native little-endian):
//   magic "PLGT" | u32 version=1 | u32 num_queries | u32 k
//   then per query: u32 query_id, u32 answer_count, answer_count × u32 ids
void write_groundtruth(const std::filesystem::path& path, const GroundTruth& gt);
[[nodiscard]] GroundTruth read_groundtruth(const std::filesystem::path& path);

// |approx ∩ exact| / |exact|. 1.0 means the approximate search found every true neighbor.
[[nodiscard]] double recall_at_k(const std::vector<SongId>& approx,
                                 const std::vector<SongId>& exact);

}  // namespace plgen
