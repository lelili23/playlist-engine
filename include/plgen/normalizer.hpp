#pragma once

#include <optional>
#include <span>
#include <utility>

#include "plgen/types.hpp"

namespace plgen {

// Audio features as they appear in the dataset, before any transformation.
struct RawFeatures {
  float energy = 0;        // 0..1
  float valence = 0;       // 0..1, musical "positivity" (our proxy for mood)
  float danceability = 0;  // 0..1
  float acousticness = 0;  // 0..1
  float tempo = 0;         // BPM
  int key = -1;            // pitch class 0..11 (C=0), -1 = unknown
  int mode = 1;            // 1 = major, 0 = minor
  float speechiness = 0;   // 0..1, spoken words: rap/podcasts high, sung songs low
  float instrumentalness = 0;  // 0..1, near 1 = no vocals
};

// A wished-for sound rather than a real song, e.g. "rainy day study" turned into
// numbers by the LLM or the fallback table. Same units as RawFeatures.
// Key is never part of a target (no one asks for "songs in F#"); mode is optional.
struct TargetProfile {
  float energy = 0.5f;
  float valence = 0.5f;
  float danceability = 0.5f;
  float acousticness = 0.5f;
  float tempo = 110.0f;  // BPM
  float speechiness = 0.05f;
  float instrumentalness = 0.0f;
  std::optional<int> mode{};  // 1 major, 0 minor, nullopt = do not care
};

// How much each feature matters in "similarity". Applied as sqrt(w) per
// dimension so that plain squared L2 equals the weighted distance.
struct FeatureWeights {
  float energy = 1.0f;
  float valence = 1.0f;
  float danceability = 0.7f;
  float acousticness = 0.7f;
  float tempo = 0.8f;
  float key = 0.3f;
  float mode = 0.2f;
  float speechiness = 0.8f;
  float instrumentalness = 0.6f;
};

// Layout of the output vector (keep in sync with Normalizer::transform).
enum Dim : std::size_t {
  kEnergy = 0,
  kValence = 1,
  kDanceability = 2,
  kAcousticness = 3,
  kTempo = 4,
  kKeyCos = 5,
  kKeySin = 6,
  kMode = 7,
  kSpeechiness = 8,
  kInstrumentalness = 9,
};

// Speechiness above this is "mostly spoken" (rap verses, spoken word). Most
// sung songs sit below 0.1, so we stretch 0..kSpeechCap to 0..1 to spread them out.
inline constexpr float kSpeechCap = 0.5f;

// Maps a key to a point on the unit circle laid out by the circle of fifths.
// Minor keys are mapped to their relative major (A minor -> C major) because
// they share the same notes. Unknown keys map to (0, 0): the centre, which is
// equally far from every key.
[[nodiscard]] std::pair<float, float> key_to_circle(int key, int mode) noexcept;

// Turns RawFeatures into the weighted, normalized vector we search over.
// Stateful on purpose: the same fitted transform must later be applied to the
// LLM's target profile (milestone 5) and saved next to the index (milestone 7).
class Normalizer {
 public:
  Normalizer(float tempo_lo, float tempo_hi, FeatureWeights weights = {});

  // Learns the tempo clipping range from the data (1st..99th percentile).
  // Throws std::invalid_argument if rows is empty.
  [[nodiscard]] static Normalizer fit(std::span<const RawFeatures> rows,
                                      FeatureWeights weights = {});

  [[nodiscard]] Vec transform(const RawFeatures& raw) const noexcept;

  // Same transform for a target profile. Key goes to the circle's centre (equally
  // far from every key) and a missing mode to 0.5 (equally far from major and
  // minor), so dimensions the target doesn't care about don't favor any song.
  [[nodiscard]] Vec transform_target(const TargetProfile& t) const noexcept;

  [[nodiscard]] float tempo_lo() const noexcept { return tempo_lo_; }
  [[nodiscard]] float tempo_hi() const noexcept { return tempo_hi_; }
  [[nodiscard]] const FeatureWeights& weights() const noexcept { return weights_; }

 private:
  float tempo_lo_;
  float tempo_hi_;
  FeatureWeights weights_;
  Vec scale_;  // sqrt(weight) per dimension, precomputed once
};

}  // namespace plgen
