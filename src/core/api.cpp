#include "plgen/api.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <optional>

#include "plgen/json.hpp"

namespace plgen {
namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

HttpResponse json(int status, std::string body) { return {status, "application/json", std::move(body)}; }

HttpResponse error(int status, std::string_view message) {
  return json(status, "{\"error\":" + json_string(message) + "}");
}

// Optional integer query parameter with bounds. nullopt = present but invalid.
std::optional<std::size_t> int_param(const HttpRequest& req, const std::string& name,
                                     std::size_t fallback, std::size_t lo, std::size_t hi) {
  const auto it = req.query.find(name);
  if (it == req.query.end()) return fallback;
  std::size_t v = 0;
  const auto& s = it->second;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || ptr != s.data() + s.size() || v < lo || v > hi) return std::nullopt;
  return v;
}

// Splits "a,b,,c" into {"a","b","c"}.
std::vector<std::string> split_csv_param(const std::string& s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t comma = s.find(',', start);
    std::string part = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!part.empty()) out.push_back(std::move(part));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

// Loads the index if a matching file exists, else builds it (and saves it if a
// path was given). Reports what happened through `origin`.
HnswIndex load_or_build(const FeatureStore& store, const HnswParams& params,
                        const std::filesystem::path& path, std::string& origin) {
  if (!path.empty() && std::filesystem::exists(path)) {
    try {
      origin = "loaded from " + path.string();
      return HnswIndex::load(path, store);
    } catch (const std::exception& e) {
      std::cerr << "index file not usable (" << e.what() << "), rebuilding\n";
    }
  }
  HnswIndex built(store, params);
  origin = "built with " + std::to_string(params.build_threads) + " thread(s)";
  if (!path.empty()) {
    try {
      built.save(path);
      origin += ", saved to " + path.string();
    } catch (const std::exception& e) {
      std::cerr << "could not save index: " << e.what() << '\n';
    }
  }
  return built;
}

std::string fmt(float x) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.5f", static_cast<double>(x));
  return buf;
}

}  // namespace

Engine::Engine(LoadedDataset data, HnswParams hnsw, RerankOptions rerank,
               const std::filesystem::path& index_path)
    : data_(std::move(data)),
      genres_(GenreGraph::build(data_.store)),
      index_origin_(),
      index_(load_or_build(data_.store, hnsw, index_path, index_origin_)),
      rerank_(rerank) {
  lower_names_.reserve(data_.store.size());
  for (SongId id = 0; id < data_.store.size(); ++id) {
    const auto& m = data_.store.meta(id);
    lower_names_.push_back(lower(m.name + " " + m.artists));
  }
}

HttpResponse Engine::handle(const HttpRequest& req) const {
  if (req.method != "GET") return error(405, "only GET is supported for now");
  if (req.path == "/health") return health();
  if (req.path == "/search") return search(req);
  if (req.path == "/similar") return similar(req);
  if (req.path == "/genres") return genres();
  if (req.path == "/recommend") return recommend_endpoint(req);
  if (req.path == "/") {
    return json(200,
                "{\"endpoints\":[\"/health\",\"/search?q=somebody else\","
                "\"/similar?song=<id>&k=20\",\"/genres\",\"/recommend?energy=...\"]}");
  }
  return error(404, "no such endpoint: " + req.path);
}

HttpResponse Engine::health() const {
  return json(200, "{\"status\":\"ok\",\"songs\":" + std::to_string(store().size()) + "}");
}

void Engine::append_song(std::string& out, SongId id) const {
  const auto& m = store().meta(id);
  out += "{\"id\":";
  append_json_string(out, m.spotify_id);
  out += ",\"name\":";
  append_json_string(out, m.name);
  out += ",\"artists\":";
  append_json_string(out, m.artists);
  out += ",\"genres\":[";
  for (std::size_t i = 0; i < m.genres.size(); ++i) {
    if (i) out += ',';
    append_json_string(out, m.genres[i]);
  }
  out += "],\"popularity\":" + std::to_string(m.popularity) + "}";
}

HttpResponse Engine::search(const HttpRequest& req) const {
  const auto q = req.query.find("q");
  if (q == req.query.end() || q->second.empty()) return error(400, "missing ?q=<song or artist>");
  const auto limit = int_param(req, "limit", 10, 1, 50);
  if (!limit) return error(400, "limit must be 1-50");

  const std::string needle = lower(q->second);
  std::vector<SongId> hits;
  for (SongId id = 0; id < lower_names_.size(); ++id) {
    if (lower_names_[id].find(needle) != std::string::npos) hits.push_back(id);
  }
  // Most popular first: usually the version people mean.
  std::stable_sort(hits.begin(), hits.end(), [&](SongId a, SongId b) {
    return store().meta(a).popularity > store().meta(b).popularity;
  });
  if (hits.size() > *limit) hits.resize(*limit);

  std::string out = "{\"results\":[";
  for (std::size_t i = 0; i < hits.size(); ++i) {
    if (i) out += ',';
    append_song(out, hits[i]);
  }
  out += "]}";
  return json(200, std::move(out));
}

HttpResponse Engine::similar(const HttpRequest& req) const {
  const auto t0 = std::chrono::steady_clock::now();
  const auto song = req.query.find("song");
  if (song == req.query.end() || song->second.empty()) {
    return error(400, "missing ?song=<spotify id> (find ids with /search?q=...)");
  }
  const auto k = int_param(req, "k", 20, 1, 100);
  if (!k) return error(400, "k must be 1-100");
  const auto query = store().find(song->second);
  if (!query) return error(404, "unknown song id: " + song->second);

  const auto recs = similar_songs(store(), genres_, *query, *k, rerank_, &index_);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  std::string out = "{\"query\":";
  append_song(out, *query);
  out += ",\"took_ms\":" + fmt(static_cast<float>(ms)) + ",\"results\":[";
  for (std::size_t i = 0; i < recs.size(); ++i) {
    if (i) out += ',';
    out += "{\"rank\":" + std::to_string(i + 1) + ",\"score\":" + fmt(recs[i].score) +
           ",\"distance\":" + fmt(recs[i].distance) + ",\"song\":";
    append_song(out, recs[i].id);
    out += '}';
  }
  out += "]}";
  return json(200, std::move(out));
}

HttpResponse Engine::genres() const {
  std::string out = "{\"genres\":[";
  const auto names = genres_.names();
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i) out += ',';
    append_json_string(out, names[i]);
  }
  out += "]}";
  return json(200, std::move(out));
}

HttpResponse Engine::recommend_endpoint(const HttpRequest& req) const {
  const auto k = int_param(req, "k", 20, 1, 100);
  if (!k) return error(400, "k must be 1-100");

  // ---- Seed songs (optional) ----
  RecommendQuery q;
  std::vector<SongId> seeds;
  if (const auto it = req.query.find("seeds"); it != req.query.end()) {
    for (const auto& sid : split_csv_param(it->second)) {
      const auto id = store().find(sid);
      if (!id) return error(404, "unknown seed song id: " + sid);
      seeds.push_back(*id);
    }
  }

  // ---- Target profile (optional, but all-or-nothing) ----
  struct Field {
    const char* name;
    float lo, hi;
    float TargetProfile::*member;  // pointer-to-member: which field of TargetProfile to set
  };
  static constexpr Field kFields[] = {
      {"energy", 0, 1, &TargetProfile::energy},
      {"valence", 0, 1, &TargetProfile::valence},
      {"danceability", 0, 1, &TargetProfile::danceability},
      {"acousticness", 0, 1, &TargetProfile::acousticness},
      {"tempo", 40, 250, &TargetProfile::tempo},
      {"speechiness", 0, 1, &TargetProfile::speechiness},
      {"instrumentalness", 0, 1, &TargetProfile::instrumentalness},
  };
  TargetProfile profile;
  std::size_t given = 0;
  std::string missing;
  for (const Field& f : kFields) {
    const auto it = req.query.find(f.name);
    if (it == req.query.end()) {
      missing += missing.empty() ? f.name : std::string(", ") + f.name;
      continue;
    }
    float v = 0;
    const auto& s = it->second;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size() || !(v >= f.lo && v <= f.hi)) {
      return error(400, std::string(f.name) + " must be a number in [" + fmt(f.lo) + ", " +
                            fmt(f.hi) + "]");
    }
    profile.*(f.member) = v;
    ++given;
  }
  if (given > 0 && given < std::size(kFields)) return error(400, "missing: " + missing);
  if (const auto it = req.query.find("mode"); it != req.query.end()) {
    if (it->second != "0" && it->second != "1") return error(400, "mode must be 0 or 1");
    profile.mode = it->second == "1" ? 1 : 0;
  }
  const bool has_profile = given > 0;
  if (!has_profile && seeds.empty()) {
    return error(400, "give a target profile (energy, valence, ...) and/or seeds=id1,id2");
  }

  // ---- Combine into one target point ----
  // Seeds: their average vector. Profile + seeds: halfway between the two.
  Vec seed_mean{};
  for (const SongId s : seeds) {
    const VecView v = store().vector(s);
    for (std::size_t d = 0; d < kDim; ++d) seed_mean[d] += v[d] / static_cast<float>(seeds.size());
  }
  const Vec idea = has_profile ? data_.normalizer.transform_target(profile) : Vec{};
  const float w_idea = has_profile ? (seeds.empty() ? 1.0f : 0.5f) : 0.0f;
  for (std::size_t d = 0; d < kDim; ++d) q.target[d] = w_idea * idea[d] + (1 - w_idea) * seed_mean[d];

  // Genres: as given, else the seeds' genres, else no preference.
  if (const auto it = req.query.find("genres"); it != req.query.end()) {
    q.genres = split_csv_param(it->second);
  } else {
    for (const SongId s : seeds) {
      for (const auto& g : store().meta(s).genres) {
        if (std::find(q.genres.begin(), q.genres.end(), g) == q.genres.end()) q.genres.push_back(g);
      }
    }
  }
  q.exclude = seeds;

  const auto recs = recommend(store(), genres_, q, *k, rerank_, &index_);

  std::string out = "{\"seeds\":[";
  for (std::size_t i = 0; i < seeds.size(); ++i) {
    if (i) out += ',';
    append_song(out, seeds[i]);
  }
  out += "],\"results\":[";
  for (std::size_t i = 0; i < recs.size(); ++i) {
    if (i) out += ',';
    out += "{\"rank\":" + std::to_string(i + 1) + ",\"score\":" + fmt(recs[i].score) +
           ",\"distance\":" + fmt(recs[i].distance) + ",\"song\":";
    append_song(out, recs[i].id);
    out += '}';
  }
  out += "]}";
  return json(200, std::move(out));
}

}  // namespace plgen
