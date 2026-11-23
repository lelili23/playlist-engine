#include <gtest/gtest.h>

#include <string>

#include "plgen/recommender.hpp"

using namespace plgen;

namespace {

Vec at(float x) {
  Vec v{};
  v[0] = x;
  return v;
}

SongId add(FeatureStore& s, float x, std::string genre, std::string artist, int popularity = 0) {
  return s.add({.spotify_id = std::to_string(s.size()),
                .name = "song",
                .artists = std::move(artist),
                .genres = {std::move(genre)},
                .popularity = popularity},
               at(x));
}

std::vector<SongId> ids_of(const std::vector<Recommendation>& r) {
  std::vector<SongId> ids;
  for (const auto& x : r) ids.push_back(x.id);
  return ids;
}

}  // namespace

TEST(Recommender, SameGenreOutranksSlightlyCloserOtherGenre) {
  FeatureStore s;
  const SongId q = add(s, 0.0f, "funk", "A");
  const SongId other = add(s, 0.05f, "country", "B");  // dist 0.0025, wrong genre
  const SongId same = add(s, 0.10f, "funk", "C");      // dist 0.01, same genre
  const auto r = similar_songs(s, GenreGraph::build(s), q, 2);
  EXPECT_EQ(ids_of(r), (std::vector<SongId>{same, other}));
  EXPECT_FLOAT_EQ(r[1].score, r[1].distance + RerankOptions{}.genre_penalty +
                                  RerankOptions{}.obscure_penalty);
}

TEST(Recommender, GenrePenaltySlidesWithRelatedness) {
  const RerankOptions o;
  EXPECT_FLOAT_EQ(genre_penalty_for(0.0f, o), o.genre_penalty);           // unrelated
  EXPECT_FLOAT_EQ(genre_penalty_for(1.0f, o), 0.0f);                      // same genre
  EXPECT_FLOAT_EQ(genre_penalty_for(o.related_full, o), 0.0f);            // same family
  EXPECT_FLOAT_EQ(genre_penalty_for(o.related_full / 2, o), o.genre_penalty / 2);
}

TEST(Recommender, ObscureCopiesArePushedDown) {
  FeatureStore s;
  const SongId q = add(s, 0.0f, "pop", "Q", 50);
  add(s, 0.010f, "pop", "A", 0);                     // obscure copy, slightly closer
  const SongId known = add(s, 0.015f, "pop", "B", 60);
  EXPECT_EQ(similar_songs(s, GenreGraph::build(s), q, 1).front().id, known);
}

TEST(Recommender, NeverReturnsTheQuery) {
  FeatureStore s;
  const SongId q = add(s, 0.0f, "pop", "A");
  add(s, 0.0f, "pop", "B");
  for (const auto& r : similar_songs(s, GenreGraph::build(s), q, 10)) EXPECT_NE(r.id, q);
}

TEST(Recommender, CapsSongsPerArtist) {
  FeatureStore s;
  const SongId q = add(s, 0.0f, "pop", "Q");
  for (int i = 1; i <= 4; ++i) add(s, 0.01f * static_cast<float>(i), "pop", "Same Artist");
  const SongId other = add(s, 0.5f, "pop", "Other");

  const auto r = similar_songs(s, GenreGraph::build(s), q, 3);  // default cap: 2 per artist
  ASSERT_EQ(r.size(), 3u);
  EXPECT_EQ(r[2].id, other);  // third slot goes to a new artist, even though it's farther
}

TEST(Recommender, PopularityBreaksNearTies) {
  FeatureStore s;
  const SongId q = add(s, 0.0f, "pop", "Q");
  const SongId obscure = add(s, 0.010f, "pop", "A", 0);
  const SongId hit = add(s, 0.011f, "pop", "B", 90);
  EXPECT_EQ(similar_songs(s, GenreGraph::build(s), q, 1).front().id, hit);
  (void)obscure;
}
