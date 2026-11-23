#include <gtest/gtest.h>

#include <sstream>

#include "plgen/dataset.hpp"

using namespace plgen;

namespace {
// Same columns as the Kaggle CSV (including the unnamed index column).
constexpr const char* kHeader =
    ",track_id,artists,album_name,track_name,popularity,duration_ms,explicit,danceability,"
    "energy,key,loudness,mode,speechiness,acousticness,instrumentalness,liveness,valence,"
    "tempo,time_signature,track_genre\n";

std::string row(int idx, const std::string& id, const std::string& name, float tempo,
                const std::string& genre, const std::string& artist = "Some Artist") {
  return std::to_string(idx) + "," + id + "," + artist + ",Album," + name +
         ",50,200000,False,0.5,0.6,0,-6.0,1,0.05,0.2,0.0,0.1,0.7," + std::to_string(tempo) +
         ",4," + genre + "\n";
}
}  // namespace

TEST(Dataset, ParsesFiltersAndDedupes) {
  std::istringstream in(std::string(kHeader) +                      //
                        row(0, "id1", "Song One", 120, "pop") +     //
                        row(1, "id2", "\"Hello, World\"", 90, "rock") +  // comma in name
                        row(2, "id1", "Song One", 120, "dance") +   // duplicate id
                        row(3, "id3", "No Tempo", 0, "pop") +       // bad tempo
                        "4,id4,broken row\n" +                       // malformed
                        row(5, "id5", "Song Five", 140, "edm"));

  const LoadedDataset ds = load_spotify_csv(in);
  EXPECT_EQ(ds.stats.rows_read, 6u);
  EXPECT_EQ(ds.stats.duplicate_rows, 1u);
  EXPECT_EQ(ds.stats.dropped_bad_tempo, 1u);
  EXPECT_EQ(ds.stats.dropped_malformed, 1u);
  EXPECT_EQ(ds.stats.songs, 3u);

  const auto id1 = ds.store.find("id1");
  ASSERT_TRUE(id1.has_value());
  EXPECT_EQ(ds.store.meta(*id1).genres, (std::vector<std::string>{"pop", "dance"}));

  const auto id2 = ds.store.find("id2");
  ASSERT_TRUE(id2.has_value());
  EXPECT_EQ(ds.store.meta(*id2).name, "Hello, World");
  EXPECT_FALSE(ds.store.find("id3").has_value());
}

TEST(Dataset, MergesSameSongUnderDifferentTrackIds) {
  std::istringstream in(std::string(kHeader) +
                        row(0, "single", "Hey Ya", 160, "funk", "OutKast") +
                        row(1, "album", "HEY YA", 160, "hip-hop", "outkast") +   // re-release
                        row(2, "album", "HEY YA", 160, "dance", "outkast") +     // its genre row
                        row(3, "cover", "Hey Ya", 120, "acoustic", "Someone Else"));
  const LoadedDataset ds = load_spotify_csv(in);
  EXPECT_EQ(ds.stats.duplicate_songs, 1u);
  EXPECT_EQ(ds.stats.duplicate_rows, 1u);
  EXPECT_EQ(ds.stats.songs, 2u);  // the original + the cover by a different artist
  const auto id = ds.store.find("single");
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(ds.store.meta(*id).genres, (std::vector<std::string>{"funk", "hip-hop", "dance"}));
  EXPECT_FALSE(ds.store.find("album").has_value());
}

TEST(Dataset, MergesVersionsOfTheSameTitle) {
  std::istringstream in(std::string(kHeader) +
                        row(0, "a", "One Kiss", 124, "dance", "Calvin Harris;Dua Lipa") +
                        row(1, "b", "\"One Kiss (with Dua Lipa)\"", 124, "house", "Calvin Harris;Dua Lipa") +
                        row(2, "c", "One Kiss - Remastered 2019", 124, "edm", "Calvin Harris;Dua Lipa") +
                        row(3, "d", "One More Time", 124, "house", "Calvin Harris;Dua Lipa") +  // different song
                        row(4, "e", "(Intro)", 124, "pop", "X"));  // nothing before " (": kept whole
  const LoadedDataset ds = load_spotify_csv(in);
  EXPECT_EQ(ds.stats.duplicate_songs, 2u);
  EXPECT_EQ(ds.stats.songs, 3u);
  const auto id = ds.store.find("a");
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(ds.store.meta(*id).genres, (std::vector<std::string>{"dance", "house", "edm"}));
}

TEST(Dataset, MissingColumnThrows) {
  std::istringstream in("track_id,tempo\nabc,120\n");
  EXPECT_THROW((void)load_spotify_csv(in), std::runtime_error);
}
