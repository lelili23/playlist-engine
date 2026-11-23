#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "plgen/brute_force.hpp"
#include "plgen/groundtruth.hpp"
#include "plgen/hnsw.hpp"

using namespace plgen;

namespace {

FeatureStore random_store(std::size_t n, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  FeatureStore s;
  s.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    Vec v;
    for (auto& x : v) x = u(rng);
    s.add({.spotify_id = std::to_string(i)}, v);
  }
  return s;
}

std::vector<SongId> ids_of(const std::vector<Neighbor>& r) {
  std::vector<SongId> ids;
  for (const auto& n : r) ids.push_back(n.id);
  return ids;
}

}  // namespace

TEST(Hnsw, EmptyAndTinyIndexes) {
  const FeatureStore empty;
  const HnswIndex e(empty);
  EXPECT_TRUE(e.search(Vec{}, 5, 10).empty());

  const auto one = random_store(1, 1);
  const HnswIndex i1(one);
  EXPECT_EQ(ids_of(i1.search(one.vector(0), 5, 10)), (std::vector<SongId>{0}));
  EXPECT_TRUE(i1.search(one.vector(0), 5, 10, SongId{0}).empty());  // only song is excluded
}

TEST(Hnsw, ResultsSortedAndExcludeWorks) {
  const auto s = random_store(500, 2);
  const HnswIndex idx(s);
  const auto r = idx.search(s.vector(7), 10, 50, SongId{7});
  ASSERT_EQ(r.size(), 10u);
  EXPECT_TRUE(std::is_sorted(r.begin(), r.end(), closer));
  for (const auto& n : r) EXPECT_NE(n.id, 7u);
}

TEST(Hnsw, GraphRespectsLinkLimitsAndHasNoBadLinks) {
  const auto s = random_store(2000, 3);
  const HnswIndex idx(s, {.M = 8});
  for (SongId id = 0; id < s.size(); ++id) {
    for (int l = 0; l <= idx.level_of(id); ++l) {
      const auto& ls = idx.links(id, l);
      EXPECT_LE(ls.size(), idx.max_links(l));
      for (const SongId nb : ls) {
        EXPECT_NE(nb, id) << "self-link";
        ASSERT_LT(nb, s.size());
        EXPECT_GE(idx.level_of(nb), l) << "linked to a song not on this layer";
      }
    }
  }
  EXPECT_GE(idx.max_level(), 1);  // 2000 songs, M=8: at least one upper layer
}

// The real test: approximate results must match exact brute force closely.
TEST(Hnsw, HighRecallAgainstBruteForce) {
  const auto s = random_store(5000, 4);
  const HnswIndex idx(s);
  double recall = 0;
  const SongId queries = 200;
  for (SongId q = 0; q < queries; ++q) {
    const auto exact = ids_of(brute_force_knn(s, s.vector(q), 10, q));
    const auto approx = ids_of(idx.search(s.vector(q), 10, 100, q));
    recall += recall_at_k(approx, exact);
  }
  EXPECT_GE(recall / queries, 0.95);
}

TEST(Hnsw, HigherEfNeverHurtsAndDoesFewerDistancesThanBruteForce) {
  const auto s = random_store(5000, 5);
  const HnswIndex idx(s);
  double r_low = 0, r_high = 0;
  SearchStats st;
  for (SongId q = 0; q < 100; ++q) {
    const auto exact = ids_of(brute_force_knn(s, s.vector(q), 10, q));
    r_low += recall_at_k(ids_of(idx.search(s.vector(q), 10, 10, q)), exact);
    r_high += recall_at_k(ids_of(idx.search(s.vector(q), 10, 200, q, &st)), exact);
  }
  EXPECT_GE(r_high, r_low);
  EXPECT_LT(st.distance_computations / 100, s.size() / 2);  // well under half the songs per query
}

TEST(Hnsw, SameSeedSameIndex) {
  const auto s = random_store(1000, 6);
  const HnswIndex a(s), b(s);
  for (SongId q = 0; q < 20; ++q) {
    EXPECT_EQ(a.search(s.vector(q), 5, 50), b.search(s.vector(q), 5, 50));
  }
}

// ---- Persistence ----

namespace {
std::filesystem::path temp_path(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}
}  // namespace

TEST(HnswPersistence, SaveThenLoadGivesIdenticalResults) {
  const auto s = random_store(3000, 7);
  const HnswIndex built(s);
  const auto path = temp_path("plgen_index_roundtrip.bin");
  built.save(path);
  const HnswIndex loaded = HnswIndex::load(path, s);
  std::filesystem::remove(path);

  EXPECT_EQ(loaded.size(), built.size());
  EXPECT_EQ(loaded.max_level(), built.max_level());
  for (SongId q = 0; q < 50; ++q) {
    EXPECT_EQ(loaded.search(s.vector(q), 10, 50, q), built.search(s.vector(q), 10, 50, q));
  }
}

TEST(HnswPersistence, RefusesIndexForDifferentData) {
  const auto a = random_store(500, 8);
  const auto b = random_store(500, 9);  // same size, different songs
  const auto path = temp_path("plgen_index_mismatch.bin");
  HnswIndex(a).save(path);
  EXPECT_THROW((void)HnswIndex::load(path, b), std::runtime_error);
  std::filesystem::remove(path);
}

TEST(HnswPersistence, RefusesTruncatedOrBogusFiles) {
  const auto s = random_store(500, 10);
  const auto path = temp_path("plgen_index_bad.bin");
  HnswIndex(s).save(path);
  std::filesystem::resize_file(path, std::filesystem::file_size(path) / 2);
  EXPECT_THROW((void)HnswIndex::load(path, s), std::runtime_error);

  { std::ofstream(path) << "definitely not an index"; }
  EXPECT_THROW((void)HnswIndex::load(path, s), std::runtime_error);
  std::filesystem::remove(path);
  EXPECT_THROW((void)HnswIndex::load(path, s), std::runtime_error);  // missing file
}

// ---- Parallel build ----

TEST(HnswParallel, SameRecallAsSingleThreaded) {
  const auto s = random_store(5000, 11);
  const HnswIndex idx(s, {.build_threads = 4});
  double recall = 0;
  for (SongId q = 0; q < 200; ++q) {
    std::vector<SongId> exact, approx;
    for (const auto& n : brute_force_knn(s, s.vector(q), 10, q)) exact.push_back(n.id);
    for (const auto& n : idx.search(s.vector(q), 10, 100, q)) approx.push_back(n.id);
    recall += recall_at_k(approx, exact);
  }
  EXPECT_GE(recall / 200, 0.95);
}

TEST(HnswParallel, GraphIsWellFormed) {
  const auto s = random_store(3000, 12);
  const HnswIndex idx(s, {.M = 8, .build_threads = 4});
  for (SongId id = 0; id < s.size(); ++id) {
    for (int l = 0; l <= idx.level_of(id); ++l) {
      const auto& ls = idx.links(id, l);
      EXPECT_LE(ls.size(), idx.max_links(l));
      for (const SongId nb : ls) {
        EXPECT_NE(nb, id);
        ASSERT_LT(nb, s.size());
        EXPECT_GE(idx.level_of(nb), l);
      }
    }
    EXPECT_FALSE(idx.links(id, 0).empty()) << "song " << id << " is unreachable";
  }
}
