#pragma once

#include "plgen/types.hpp"

namespace plgen {

// Squared Euclidean distance. Feature weights are already baked into the
// stored vectors (each dim is scaled by sqrt(weight)), so this plain formula
// equals the weighted distance. Brute force, HNSW and (later) SIMD all share it.
[[nodiscard]] inline float squared_l2(VecView a, VecView b) noexcept {
  float sum = 0.0f;
  for (std::size_t i = 0; i < kDim; ++i) {
    const float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

}  // namespace plgen
