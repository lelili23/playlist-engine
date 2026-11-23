#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace plgen {

// Dense internal index 0..n-1. Not the Spotify string ID: 4 bytes per
// reference matters once HNSW stores millions of neighbor links.
using SongId = std::uint32_t;

// Feature dimensions. A compile-time constant lets the compiler fully unroll
// the distance loop. (10 floats = 40 bytes; the SIMD milestone will pad to 16.)
inline constexpr std::size_t kDim = 10;

using Vec = std::array<float, kDim>;
using VecView = std::span<const float, kDim>;  // non-owning view of one vector

struct Neighbor {
  SongId id;
  float dist;  // squared L2 distance (no sqrt: it doesn't change the ordering)

  friend bool operator==(const Neighbor&, const Neighbor&) = default;
};

// Total order used everywhere results are ranked: closer first, ties broken
// by smaller id so results are deterministic (tests and recall depend on it).
[[nodiscard]] constexpr bool closer(const Neighbor& a, const Neighbor& b) noexcept {
  return a.dist < b.dist || (a.dist == b.dist && a.id < b.id);
}

}  // namespace plgen
