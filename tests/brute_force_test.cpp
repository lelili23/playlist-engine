#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <string>

#include "plgen/brute_force.hpp"
#include "plgen/distance.hpp"

using namespace plgen;

namespace {

Vec at(float x, float y = 0) {
  Vec v{};
  v[0] = x;
  v[1] = y;
  return v;
}

FeatureStore line_store() {
  // Five points on a line at x = 0, 1, 2, 3, 10.
  FeatureStore s;
  int i = 0;
  for (float x : {0.0f, 1.0f, 2.0f, 3.0f, 10.0f}) s.add({.spotify_id = std::to_string(i++)}, at(x));
  return s;
}

std::vector<SongId> ids_of(const std::vector<Neighbor>& r) {
  std::vector<SongId> ids;
  for (const auto& n : r) ids.push_back(n.id);
  return ids;
}

// Obviously-correct reference: compute every distance, sort everything.
std::vector<Neighbor> reference_knn(const FeatureStore& s, VecView q, std::size_t k,
                                    std::optional<SongId> exclude) {
  std::vector<Neighbor> all;
  for (SongId id = 0; id < s.size(); ++id) {
    if (exclude && *exclude == id) continue;
    all.push_back({id, squared_l2(q, s.vector(id))});
  }
  std::sort(all.begin(), all.end(), closer);
  all.resize(std::min(k, all.size()));
  return all;
}

}  // namespace

TEST(BruteForce, HandPlacedPoints) {
  const auto s = line_store();
  const Vec q = at(2.2f);
  EXPECT_EQ(ids_of(brute_force_knn(s, q, 3)), (std::vector<SongId>{2, 3, 1}));
}

TEST(BruteForce, ResultsSortedAndDistancesCorrect) {
  const auto s = line_store();
  const auto r = brute_force_knn(s, at(0.0f), 5);
  ASSERT_EQ(r.size(), 5u);
  EXPECT_TRUE(std::is_sorted(r.begin(), r.end(), closer));
  EXPECT_FLOAT_EQ(r.back().dist, 100.0f);  // squared: 10^2
}

TEST(BruteForce, ExcludesQuerySong) {
  const auto s = line_store();
  const auto r = brute_force_knn(s, s.vector(1), 2, SongId{1});
  EXPECT_EQ(ids_of(r), (std::vector<SongId>{0, 2}));  // tie at distance 1, broken by id
}

TEST(BruteForce, EdgeCases) {
  const auto s = line_store();
  EXPECT_TRUE(brute_force_knn(s, at(0), 0).empty());
  EXPECT_EQ(brute_force_knn(s, at(0), 100).size(), 5u);  // k > n
  EXPECT_TRUE(brute_force_knn(FeatureStore{}, at(0), 5).empty());
}

TEST(BruteForce, TiesAreDeterministic) {
  FeatureStore s;
  for (int i = 0; i < 10; ++i) s.add({.spotify_id = std::to_string(i)}, at(1.0f));  // all equal
  EXPECT_EQ(ids_of(brute_force_knn(s, at(0), 3)), (std::vector<SongId>{0, 1, 2}));
}

// Property test against the full-sort reference on random data.
// This same reference becomes the correctness baseline for HNSW.
TEST(BruteForce, MatchesReferenceOnRandomData) {
  std::mt19937 rng(123);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  FeatureStore s;
  for (int i = 0; i < 2000; ++i) {
    Vec v;
    for (auto& x : v) x = u(rng);
    s.add({.spotify_id = std::to_string(i)}, v);
  }
  for (SongId q = 0; q < 50; ++q) {
    for (std::size_t k : {1u, 7u, 20u}) {
      EXPECT_EQ(brute_force_knn(s, s.vector(q), k, q), reference_knn(s, s.vector(q), k, q))
          << "query " << q << " k " << k;
    }
  }
}
