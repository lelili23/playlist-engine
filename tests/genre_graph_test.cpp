#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "plgen/genre_graph.hpp"

using namespace plgen;

namespace {
void add(FeatureStore& s, std::vector<std::string> genres) {
  s.add({.spotify_id = std::to_string(s.size()), .genres = std::move(genres)}, Vec{});
}
}  // namespace

TEST(GenreGraph, LearnsRelatednessFromSharedTags) {
  FeatureStore s;
  add(s, {"alt-rock", "alternative"});
  add(s, {"alt-rock", "alternative"});
  add(s, {"alt-rock"});
  add(s, {"alternative"});
  add(s, {"sertanejo"});
  add(s, {"sertanejo"});
  const auto g = GenreGraph::build(s);

  EXPECT_EQ(g.genre_count(), 3u);
  // alt-rock: 3 songs, alternative: 3 songs, 2 shared -> 2 / min(3, 3)
  EXPECT_FLOAT_EQ(g.relatedness("alt-rock", "alternative"), 2.0f / 3.0f);
  EXPECT_FLOAT_EQ(g.relatedness("alternative", "alt-rock"), 2.0f / 3.0f);  // symmetric
  EXPECT_FLOAT_EQ(g.relatedness("alt-rock", "sertanejo"), 0.0f);
  EXPECT_FLOAT_EQ(g.relatedness("pop", "pop"), 1.0f);       // same name, even if unseen
  EXPECT_FLOAT_EQ(g.relatedness("alt-rock", "unknown"), 0.0f);
}

TEST(GenreGraph, FastPathMatchesStringPath) {
  FeatureStore s;
  add(s, {"alt-rock", "alternative"});
  add(s, {"alt-rock"});
  add(s, {"metal", "alt-rock"});
  add(s, {"sertanejo"});
  const auto g = GenreGraph::build(s);
  const std::vector<std::string> wanted{"alternative", "metal", "not-a-genre"};
  const auto ids = g.ids_of(wanted);
  EXPECT_EQ(ids.size(), 2u);  // unknown name skipped
  for (SongId id = 0; id < s.size(); ++id) {
    EXPECT_FLOAT_EQ(g.best_relatedness(ids, id), g.best_relatedness(wanted, s.meta(id))) << id;
  }
}

TEST(GenreGraph, BestRelatednessUsesClosestPair) {
  FeatureStore s;
  add(s, {"indie", "indie-pop"});
  add(s, {"indie"});
  add(s, {"metal"});
  const auto g = GenreGraph::build(s);
  const SongMeta a{.genres = {"metal", "indie"}};
  const SongMeta b{.genres = {"indie-pop"}};
  EXPECT_FLOAT_EQ(g.best_relatedness(a, b), 1.0f);  // indie vs indie-pop: 1 shared / min(2, 1)
}
