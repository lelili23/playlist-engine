#include "plgen/normalizer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace plgen {
namespace {  // anonymous namespace = file-private helpers (like `private static` in Java)

float clamp01(float x) noexcept { return std::clamp(x, 0.0f, 1.0f); }

// Value at quantile q (0..1) using nth_element: O(n) on average, no full sort.
float quantile(std::vector<float>& values, double q) {
  const auto idx = static_cast<std::size_t>(q * static_cast<double>(values.size() - 1));
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(idx),
                   values.end());
  return values[idx];
}

}  // namespace

std::pair<float, float> key_to_circle(int key, int mode) noexcept {
  if (key < 0 || key > 11) return {0.0f, 0.0f};
  const int major_key = (mode == 0) ? (key + 3) % 12 : key;  // relative major
  const int fifths_pos = (7 * major_key) % 12;                // steps around circle of fifths
  const double angle = 2.0 * std::numbers::pi * fifths_pos / 12.0;
  return {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
}

Normalizer::Normalizer(float tempo_lo, float tempo_hi, FeatureWeights weights)
    : tempo_lo_(tempo_lo), tempo_hi_(tempo_hi), weights_(weights) {
  if (!(tempo_hi_ > tempo_lo_)) {
    throw std::invalid_argument("Normalizer: tempo_hi must be greater than tempo_lo");
  }
  scale_[kEnergy] = std::sqrt(weights_.energy);
  scale_[kValence] = std::sqrt(weights_.valence);
  scale_[kDanceability] = std::sqrt(weights_.danceability);
  scale_[kAcousticness] = std::sqrt(weights_.acousticness);
  scale_[kTempo] = std::sqrt(weights_.tempo);
  scale_[kKeyCos] = std::sqrt(weights_.key);
  scale_[kKeySin] = std::sqrt(weights_.key);
  scale_[kMode] = std::sqrt(weights_.mode);
  scale_[kSpeechiness] = std::sqrt(weights_.speechiness);
  scale_[kInstrumentalness] = std::sqrt(weights_.instrumentalness);
}

Normalizer Normalizer::fit(std::span<const RawFeatures> rows, FeatureWeights weights) {
  if (rows.empty()) throw std::invalid_argument("Normalizer::fit: no rows");
  std::vector<float> tempos;
  tempos.reserve(rows.size());
  for (const auto& r : rows) tempos.push_back(r.tempo);

  const float lo = quantile(tempos, 0.01);
  float hi = quantile(tempos, 0.99);
  if (!(hi > lo)) hi = lo + 1.0f;  // degenerate data (all same tempo): avoid divide by zero
  return Normalizer(lo, hi, weights);
}

Vec Normalizer::transform(const RawFeatures& raw) const noexcept {
  Vec v{};
  v[kEnergy] = clamp01(raw.energy);
  v[kValence] = clamp01(raw.valence);
  v[kDanceability] = clamp01(raw.danceability);
  v[kAcousticness] = clamp01(raw.acousticness);

  const float t = std::clamp(raw.tempo, tempo_lo_, tempo_hi_);
  v[kTempo] = (t - tempo_lo_) / (tempo_hi_ - tempo_lo_);

  // Radius 0.5 so opposite keys are distance 1 apart, like the 0..1 features.
  const auto [c, s] = key_to_circle(raw.key, raw.mode);
  v[kKeyCos] = 0.5f * c;
  v[kKeySin] = 0.5f * s;

  v[kMode] = (raw.mode == 1) ? 1.0f : 0.0f;
  v[kSpeechiness] = std::min(raw.speechiness, kSpeechCap) / kSpeechCap;
  v[kInstrumentalness] = clamp01(raw.instrumentalness);

  for (std::size_t i = 0; i < kDim; ++i) v[i] *= scale_[i];
  return v;
}

Vec Normalizer::transform_target(const TargetProfile& t) const noexcept {
  RawFeatures raw;
  raw.energy = t.energy;
  raw.valence = t.valence;
  raw.danceability = t.danceability;
  raw.acousticness = t.acousticness;
  raw.tempo = t.tempo;
  raw.speechiness = t.speechiness;
  raw.instrumentalness = t.instrumentalness;
  raw.key = -1;  // unknown -> centre of the key circle
  raw.mode = t.mode.value_or(1);
  Vec v = transform(raw);
  if (!t.mode) v[kMode] = 0.5f * scale_[kMode];
  return v;
}

}  // namespace plgen
