#include "plgen/recommender.hpp"

#include <algorithm>
#include <unordered_map>

#include "plgen/brute_force.hpp"

namespace plgen {

float genre_penalty_for(float relatedness, const RerankOptions& opts) noexcept {
  const float closeness = std::min(1.0f, relatedness / opts.related_full);
  return opts.genre_penalty * (1.0f - closeness);
}

std::vector<Recommendation> recommend(const FeatureStore& store, const GenreGraph& genres,
                                      const RecommendQuery& q, std::size_t k,
                                      const RerankOptions& opts, const HnswIndex* index) {
  if (k == 0 || store.size() == 0) return {};
  auto excluded = [&](SongId id) {
    return std::find(q.exclude.begin(), q.exclude.end(), id) != q.exclude.end();
  };

  // Stage 1: retrieval. Fetch extra so that dropping excluded songs still leaves enough.
  const std::size_t want = std::max(opts.candidates, k) + q.exclude.size();
  std::vector<Neighbor> pool = index ? index->search(q.target, want, /*ef=*/want)
                                     : brute_force_knn(store, q.target, want);
  std::erase_if(pool, [&](const Neighbor& n) { return excluded(n.id); });

  // Stage 2: re-score every candidate. Convert genre names to ids ONCE here,
  // so the loop below never compares strings.
  const auto wanted = genres.ids_of(q.genres);
  std::vector<Recommendation> scored;
  scored.reserve(pool.size());
  for (const Neighbor& nb : pool) {
    const SongMeta& m = store.meta(nb.id);
    // No genre preference -> every song counts as a perfect genre match.
    const float rel = q.genres.empty() ? 1.0f : genres.best_relatedness(wanted, nb.id);
    float score = nb.dist + genre_penalty_for(rel, opts);
    score -= opts.popularity_bonus * static_cast<float>(m.popularity) / 100.0f;
    if (m.popularity < opts.obscure_below) score += opts.obscure_penalty;
    scored.push_back({nb.id, nb.dist, rel, score});
  }

  // Lower score first; ties by id so output is deterministic.
  std::sort(scored.begin(), scored.end(), [](const Recommendation& a, const Recommendation& b) {
    return a.score < b.score || (a.score == b.score && a.id < b.id);
  });

  // Walk the ranked list, skipping artists we've already used max_per_artist times.
  std::vector<Recommendation> out;
  out.reserve(k);
  std::unordered_map<std::string, std::size_t> per_artist;
  for (const auto& r : scored) {
    if (out.size() == k) break;
    if (opts.max_per_artist > 0) {
      auto& used = per_artist[store.meta(r.id).artists];
      if (used >= opts.max_per_artist) continue;
      ++used;
    }
    out.push_back(r);
  }
  return out;
}

std::vector<Recommendation> similar_songs(const FeatureStore& store, const GenreGraph& genres,
                                          SongId query, std::size_t k,
                                          const RerankOptions& opts, const HnswIndex* index) {
  const VecView v = store.vector(query);
  RecommendQuery q;
  std::copy(v.begin(), v.end(), q.target.begin());
  q.genres = store.meta(query).genres;
  q.exclude = {query};
  return recommend(store, genres, q, k, opts, index);
}

}  // namespace plgen
