#pragma once

#include <cstddef>
#include <vector>

#include "plgen/feature_store.hpp"
#include "plgen/genre_graph.hpp"
#include "plgen/hnsw.hpp"
#include "plgen/types.hpp"

namespace plgen {

// Tuning knobs for the re-ranking stage. Distances between close songs are
// small (roughly 0.005–0.05), so penalties and bonuses are on that scale too.
struct RerankOptions {
  std::size_t candidates = 1000;    // nearest songs to retrieve before re-ranking
  float genre_penalty = 0.08f;      // full penalty for a completely unrelated genre
  float related_full = 0.5f;        // relatedness at/above this counts as "same family"
  float popularity_bonus = 0.02f;   // subtracted, scaled by popularity / 100
  int obscure_below = 5;            // popularity under this is treated as an obscure copy...
  float obscure_penalty = 0.02f;    // ...and gets this added
  std::size_t max_per_artist = 2;   // variety: at most this many songs per artist (0 = no cap)
};

struct Recommendation {
  SongId id;
  float distance;     // raw squared distance from the vector search
  float relatedness;  // genre relatedness to the query, 0..1
  float score;        // distance + penalties - bonuses; lower is better
};

// Two-stage recommendation, the standard pattern in real recommenders:
//   1. Retrieval: fetch the `candidates` nearest songs by audio features, using
//      the HNSW index if one is given (fast, approximate), else brute force (exact).
//   2. Re-ranking: re-score that short list with signals the vector doesn't hold
//      (genre relatedness, popularity, artist variety), then return the best k.
[[nodiscard]] std::vector<Recommendation> similar_songs(const FeatureStore& store,
                                                        const GenreGraph& genres, SongId query,
                                                        std::size_t k,
                                                        const RerankOptions& opts = {},
                                                        const HnswIndex* index = nullptr);

// What to recommend around: a point in feature space plus optional genre hints.
struct RecommendQuery {
  Vec target{};
  std::vector<std::string> genres;  // preferred genres; empty = no genre preference
  std::vector<SongId> exclude;      // never return these (the query song, seeds)
};

// The general version: songs near `q.target`, re-ranked. similar_songs() is
// this with the query song's own vector and genres.
[[nodiscard]] std::vector<Recommendation> recommend(const FeatureStore& store,
                                                   const GenreGraph& genres,
                                                   const RecommendQuery& q, std::size_t k,
                                                   const RerankOptions& opts = {},
                                                   const HnswIndex* index = nullptr);

// The genre part of the score: 0 for the same family, genre_penalty for unrelated,
// sliding linearly in between.
[[nodiscard]] float genre_penalty_for(float relatedness, const RerankOptions& opts) noexcept;

}  // namespace plgen
