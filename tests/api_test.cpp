#include <gtest/gtest.h>

#include <sstream>

#include "plgen/api.hpp"

using namespace plgen;

namespace {

constexpr const char* kHeader =
    ",track_id,artists,album_name,track_name,popularity,duration_ms,explicit,danceability,"
    "energy,key,loudness,mode,speechiness,acousticness,instrumentalness,liveness,valence,"
    "tempo,time_signature,track_genre\n";

std::string row(const std::string& id, const std::string& name, const std::string& artist,
                float energy, const std::string& genre) {
  return "0," + id + "," + artist + ",Album," + name + ",50,200000,False,0.5," +
         std::to_string(energy) + ",0,-6.0,1,0.05,0.2,0.0,0.1,0.5,120,4," + genre + "\n";
}

const Engine& engine() {
  static const Engine e = [] {  // built once for all tests (C++ guarantees thread-safe init)
    std::string csv = kHeader;
    csv += row("se", "Somebody Else", "The 1975", 0.79f, "alt-rock");
    csv += row("ss", "The Sound", "The 1975", 0.80f, "alt-rock");
    csv += row("ws", "Watermelon Sugar", "Harry Styles", 0.81f, "pop");
    csv += row("q", "\"Quote \"\"Song\"\"\"", "Someone", 0.20f, "jazz");  // CSV-escaped quotes
    std::istringstream in(csv);
    return Engine(load_spotify_csv(in));
  }();
  return e;
}

HttpResponse get(const std::string& target) {
  HttpRequest r;
  const std::string raw = "GET " + target + " HTTP/1.1\r\n\r\n";
  EXPECT_EQ(parse_request(raw, r), ParseResult::kOk);
  return engine().handle(r);
}

}  // namespace

TEST(Api, Health) {
  const auto r = get("/health");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body, R"({"status":"ok","songs":4})");
}

TEST(Api, SearchFindsByNameOrArtistCaseInsensitive) {
  const auto r = get("/search?q=SOMEBODY%20else");
  EXPECT_EQ(r.status, 200);
  EXPECT_NE(r.body.find("\"id\":\"se\""), std::string::npos);
  EXPECT_NE(get("/search?q=1975").body.find("\"id\":\"ss\""), std::string::npos);
  EXPECT_NE(get("/search?q=quote").body.find(R"(Quote \"Song\")"), std::string::npos);  // escaped
}

TEST(Api, SimilarReturnsRankedResultsWithoutTheQuery) {
  const auto r = get("/similar?song=se&k=2");
  EXPECT_EQ(r.status, 200);
  EXPECT_NE(r.body.find("\"rank\":1"), std::string::npos);
  EXPECT_NE(r.body.find("\"rank\":2"), std::string::npos);
  EXPECT_EQ(r.body.find("\"rank\":3"), std::string::npos);
  // The query appears once (as "query"), never inside results.
  const auto results = r.body.substr(r.body.find("\"results\""));
  EXPECT_EQ(results.find("\"id\":\"se\""), std::string::npos);
}

TEST(Api, ValidatesInput) {
  EXPECT_EQ(get("/similar").status, 400);
  EXPECT_EQ(get("/similar?song=nope").status, 404);
  EXPECT_EQ(get("/similar?song=se&k=0").status, 400);
  EXPECT_EQ(get("/similar?song=se&k=abc").status, 400);
  EXPECT_EQ(get("/similar?song=se&k=101").status, 400);
  EXPECT_EQ(get("/search").status, 400);
  EXPECT_EQ(get("/nope").status, 404);
}

TEST(Api, GenresListsEveryGenreSorted) {
  EXPECT_EQ(get("/genres").body, R"({"genres":["alt-rock","jazz","pop"]})");
}

TEST(Api, RecommendFromProfile) {
  // Low-energy target: the jazz song (energy 0.2) should come first.
  const auto r = get("/recommend?energy=0.2&valence=0.5&danceability=0.5&acousticness=0.2"
                     "&tempo=120&speechiness=0.05&instrumentalness=0&k=1");
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_NE(r.body.find("\"id\":\"q\""), std::string::npos) << r.body;
}

TEST(Api, RecommendFromSeedsExcludesSeedsFromResults) {
  const auto r = get("/recommend?seeds=se&k=3");
  ASSERT_EQ(r.status, 200) << r.body;
  const auto results = r.body.substr(r.body.find("\"results\""));
  EXPECT_EQ(results.find("\"id\":\"se\""), std::string::npos);  // seed listed under "seeds" only
  EXPECT_NE(r.body.find("\"seeds\":[{\"id\":\"se\""), std::string::npos);
}

TEST(Api, RecommendValidation) {
  EXPECT_EQ(get("/recommend").status, 400);                         // nothing to go on
  EXPECT_EQ(get("/recommend?energy=0.5").status, 400);              // partial profile
  EXPECT_EQ(get("/recommend?seeds=nope").status, 404);              // unknown seed
  EXPECT_EQ(get("/recommend?energy=2&valence=0.5&danceability=0.5&acousticness=0.2"
                "&tempo=120&speechiness=0.05&instrumentalness=0").status, 400);  // out of range
  EXPECT_EQ(get("/recommend?seeds=se&mode=7").status, 400);
}

TEST(Api, OnlyGet) {
  HttpRequest r;
  ASSERT_EQ(parse_request("DELETE /health HTTP/1.1\r\n\r\n", r), ParseResult::kOk);
  EXPECT_EQ(engine().handle(r).status, 405);
}
