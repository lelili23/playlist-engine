#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "plgen/dataset.hpp"
#include "plgen/genre_graph.hpp"
#include "plgen/hnsw.hpp"
#include "plgen/http.hpp"
#include "plgen/recommender.hpp"

namespace plgen {

// Everything the API needs, built once at startup and then only READ.
// Because no request ever modifies it, any number of worker threads can use
// it at the same time without locks.
//
// Endpoints:
//   GET /health                          -> {"status":"ok","songs":81063}
//   GET /search?q=somebody else&limit=10 -> matching songs with their ids
//   GET /similar?song=<id>&k=20          -> similar songs, re-ranked
//   GET /genres                          -> every genre name (for the LLM prompt)
//   GET /recommend?energy=..&valence=..&danceability=..&acousticness=..&tempo=..
//                  &speechiness=..&instrumentalness=..[&mode=0|1]
//                  [&genres=a,b][&seeds=id1,id2][&k=20]
//        -> songs near a target sound and/or seed songs (called by the Go gateway)
class Engine {
 public:
  // If index_path is set: load the index from it when it matches this data,
  // otherwise build one and save it there for next time.
  explicit Engine(LoadedDataset data, HnswParams hnsw = {}, RerankOptions rerank = {},
                  const std::filesystem::path& index_path = {});

  // How the index was obtained, e.g. "loaded from data/index.bin" (for startup logs).
  [[nodiscard]] const std::string& index_origin() const noexcept { return index_origin_; }

  // The HNSW index holds a reference to data_.store, so moving or copying an
  // Engine would leave that reference dangling. The compiler now forbids both.
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;

  // Router: picks the endpoint for a request. Thread-safe.
  [[nodiscard]] HttpResponse handle(const HttpRequest& req) const;

  [[nodiscard]] const FeatureStore& store() const noexcept { return data_.store; }

 private:
  [[nodiscard]] HttpResponse health() const;
  [[nodiscard]] HttpResponse search(const HttpRequest& req) const;
  [[nodiscard]] HttpResponse similar(const HttpRequest& req) const;
  [[nodiscard]] HttpResponse genres() const;
  [[nodiscard]] HttpResponse recommend_endpoint(const HttpRequest& req) const;
  void append_song(std::string& out, SongId id) const;

  // Order matters: members are constructed top to bottom, NOT in the order the
  // constructor lists them. index_ needs data_ to exist, and building index_
  // writes to index_origin_, so both must be declared before index_.
  LoadedDataset data_;
  GenreGraph genres_;
  std::string index_origin_;
  HnswIndex index_;
  RerankOptions rerank_;
  std::vector<std::string> lower_names_;  // precomputed for /search
};

}  // namespace plgen
