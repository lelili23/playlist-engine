#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "plgen/distance.hpp"
#include "plgen/normalizer.hpp"

using namespace plgen;

namespace {
float circle_dist(int k1, int m1, int k2, int m2) {
  const auto [c1, s1] = key_to_circle(k1, m1);
  const auto [c2, s2] = key_to_circle(k2, m2);
  return std::hypot(c1 - c2, s1 - s2);
}
}  // namespace

TEST(KeyToCircle, RelativeMinorEqualsMajor) {
  // A minor (key 9, mode 0) shares its notes with C major (key 0, mode 1).
  EXPECT_NEAR(circle_dist(9, 0, 0, 1), 0.0f, 1e-6f);
}

TEST(KeyToCircle, FifthIsCloseTritoneIsFar) {
  const float fifth = circle_dist(0, 1, 7, 1);    // C -> G: neighbors on the circle
  const float tritone = circle_dist(0, 1, 6, 1);  // C -> F#: opposite sides
  EXPECT_LT(fifth, tritone);
  EXPECT_NEAR(tritone, 2.0f, 1e-5f);  // diameter of the unit circle
}

TEST(KeyToCircle, WrapsAround) {
  // C and F are also neighbors (F is a fifth below C), despite 0 vs 5.
  EXPECT_NEAR(circle_dist(0, 1, 5, 1), circle_dist(0, 1, 7, 1), 1e-5f);
}

TEST(KeyToCircle, UnknownKeyIsCentre) {
  const auto [c, s] = key_to_circle(-1, 1);
  EXPECT_EQ(c, 0.0f);
  EXPECT_EQ(s, 0.0f);
}

TEST(Normalizer, TempoClipsToRange) {
  const FeatureWeights unweighted{1, 1, 1, 1, 1, 1, 1, 1, 1};
  const Normalizer n(60.0f, 200.0f, unweighted);
  EXPECT_FLOAT_EQ(n.transform({.tempo = 60.0f})[kTempo], 0.0f);
  EXPECT_FLOAT_EQ(n.transform({.tempo = 200.0f})[kTempo], 1.0f);
  EXPECT_FLOAT_EQ(n.transform({.tempo = 30.0f})[kTempo], 0.0f);   // clipped low
  EXPECT_FLOAT_EQ(n.transform({.tempo = 250.0f})[kTempo], 1.0f);  // clipped high
  EXPECT_FLOAT_EQ(n.transform({.tempo = 130.0f})[kTempo], 0.5f);
}

TEST(Normalizer, WeightsAreBakedInAsSqrt) {
  // Two songs differing only in energy by 1.0: squared distance must equal the weight.
  FeatureWeights w;
  w.energy = 0.25f;
  const Normalizer n(60.0f, 200.0f, w);
  const Vec a = n.transform({.energy = 0.0f, .tempo = 100.0f, .key = 0});
  const Vec b = n.transform({.energy = 1.0f, .tempo = 100.0f, .key = 0});
  EXPECT_NEAR(squared_l2(a, b), 0.25f, 1e-6f);
}

TEST(Normalizer, SpeechinessIsStretchedAndCapped) {
  const FeatureWeights unweighted{1, 1, 1, 1, 1, 1, 1, 1, 1};
  const Normalizer n(60.0f, 200.0f, unweighted);
  EXPECT_FLOAT_EQ(n.transform({.tempo = 100, .speechiness = 0.05f})[kSpeechiness], 0.1f);
  EXPECT_FLOAT_EQ(n.transform({.tempo = 100, .speechiness = 0.25f})[kSpeechiness], 0.5f);
  EXPECT_FLOAT_EQ(n.transform({.tempo = 100, .speechiness = 0.9f})[kSpeechiness], 1.0f);
}

TEST(Normalizer, TargetIgnoresKeyAndOptionalMode) {
  const Normalizer n(60.0f, 200.0f);
  const Vec v = n.transform_target({.energy = 0.3f, .tempo = 130.0f});
  EXPECT_FLOAT_EQ(v[kKeyCos], 0.0f);  // centre of the key circle
  EXPECT_FLOAT_EQ(v[kKeySin], 0.0f);
  const Vec major = n.transform({.tempo = 100, .mode = 1});
  const Vec minor = n.transform({.tempo = 100, .mode = 0});
  EXPECT_FLOAT_EQ(v[kMode], (major[kMode] + minor[kMode]) / 2);  // equally far from both
  EXPECT_FLOAT_EQ(n.transform_target({.tempo = 130.0f, .mode = 0})[kMode], 0.0f);
}

TEST(Normalizer, FitUsesPercentiles) {
  std::vector<RawFeatures> rows;
  for (int bpm = 1; bpm <= 1000; ++bpm) rows.push_back({.tempo = static_cast<float>(bpm)});
  const Normalizer n = Normalizer::fit(rows);
  EXPECT_NEAR(n.tempo_lo(), 10.0f, 1.0f);   // ~1st percentile
  EXPECT_NEAR(n.tempo_hi(), 990.0f, 1.0f);  // ~99th percentile
}

TEST(Normalizer, FitRejectsEmpty) {
  EXPECT_THROW((void)Normalizer::fit(std::vector<RawFeatures>{}), std::invalid_argument);
}
