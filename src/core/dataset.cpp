#include "plgen/dataset.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "plgen/csv.hpp"

namespace plgen {
namespace {

// std::from_chars: locale-independent, no exceptions, no allocation.
// Much faster than std::stof and doesn't silently accept "12abc".
template <typename T>
std::optional<T> parse_number(std::string_view s) {
  T value{};
  const auto* end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, value);
  if (ec != std::errc{} || ptr != end) return std::nullopt;
  return value;
}

// The core title without version decorations, so these all match:
//   "One Kiss", "One Kiss (with Dua Lipa)", "One Kiss - Remastered 2019", "One Kiss [Live]"
// Cuts at the first " (", " [" or " - " as long as some title is left before it.
std::string_view base_title(std::string_view name) {
  std::size_t cut = name.size();
  for (const std::string_view marker : {" (", " [", " - "}) {
    const std::size_t pos = name.find(marker);
    if (pos != std::string_view::npos && pos > 0) cut = std::min(cut, pos);
  }
  return name.substr(0, cut);
}

// "Song Title" + "Artist A;Artist B" -> lowercase key. The same recording is often
// listed under several track_ids (single, album, compilation, remaster); this key catches them.
std::string song_key(std::string_view name, std::string_view artists) {
  name = base_title(name);
  std::string key;
  key.reserve(name.size() + artists.size() + 1);
  for (char c : name) key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  key.push_back('\x1f');  // separator that can't appear in names, so "ab"+"c" != "a"+"bc"
  for (char c : artists) key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return key;
}

void add_genre(SongMeta& meta, std::string genre) {
  if (std::find(meta.genres.begin(), meta.genres.end(), genre) == meta.genres.end()) {
    meta.genres.push_back(std::move(genre));
  }
}

struct ParsedRow {
  SongMeta meta;
  RawFeatures raw;
};

}  // namespace

std::ostream& operator<<(std::ostream& os, const LoadStats& s) {
  return os << "rows read:          " << s.rows_read << '\n'
            << "dropped malformed:  " << s.dropped_malformed << '\n'
            << "dropped tempo<=0:   " << s.dropped_bad_tempo << '\n'
            << "duplicate track_id: " << s.duplicate_rows << '\n'
            << "same song, new id:  " << s.duplicate_songs << '\n'
            << "unique songs kept:  " << s.songs << '\n';
}

LoadedDataset load_spotify_csv(std::istream& in, FeatureWeights weights) {
  CsvReader reader(in);
  std::vector<std::string> fields;
  if (!reader.next(fields)) throw std::runtime_error("CSV is empty");

  const CsvHeader header(fields);
  const std::size_t c_id = header.index_of("track_id");
  const std::size_t c_name = header.index_of("track_name");
  const std::size_t c_artists = header.index_of("artists");
  const std::size_t c_genre = header.index_of("track_genre");
  const std::size_t c_energy = header.index_of("energy");
  const std::size_t c_valence = header.index_of("valence");
  const std::size_t c_dance = header.index_of("danceability");
  const std::size_t c_acoustic = header.index_of("acousticness");
  const std::size_t c_tempo = header.index_of("tempo");
  const std::size_t c_key = header.index_of("key");
  const std::size_t c_mode = header.index_of("mode");
  const std::size_t c_speech = header.index_of("speechiness");
  const std::size_t c_instr = header.index_of("instrumentalness");
  const std::size_t c_pop = header.index_of("popularity");

  LoadStats stats;
  std::vector<ParsedRow> rows;
  std::unordered_map<std::string, std::size_t> row_of_id;    // track_id -> index in rows
  std::unordered_map<std::string, std::size_t> row_of_song;  // name+artists -> index in rows

  // Pass 1: parse, filter, dedupe.
  while (reader.next(fields)) {
    ++stats.rows_read;
    if (fields.size() != header.size()) {
      ++stats.dropped_malformed;
      continue;
    }

    const auto energy = parse_number<float>(fields[c_energy]);
    const auto valence = parse_number<float>(fields[c_valence]);
    const auto dance = parse_number<float>(fields[c_dance]);
    const auto acoustic = parse_number<float>(fields[c_acoustic]);
    const auto tempo = parse_number<float>(fields[c_tempo]);
    const auto key = parse_number<int>(fields[c_key]);
    const auto mode = parse_number<int>(fields[c_mode]);
    const auto speech = parse_number<float>(fields[c_speech]);
    const auto instr = parse_number<float>(fields[c_instr]);
    const auto pop = parse_number<int>(fields[c_pop]);
    if (!energy || !valence || !dance || !acoustic || !tempo || !key || !mode || !speech ||
        !instr || !pop ||
        fields[c_id].empty()) {
      ++stats.dropped_malformed;
      continue;
    }
    if (*tempo <= 0.0f) {
      ++stats.dropped_bad_tempo;
      continue;
    }

    // Same track listed under another genre: merge the genre, keep one row.
    if (const auto it = row_of_id.find(fields[c_id]); it != row_of_id.end()) {
      ++stats.duplicate_rows;
      add_genre(rows[it->second].meta, std::move(fields[c_genre]));
      continue;
    }

    // Same song re-released under a different track_id: keep the first copy.
    // Remember this track_id too, so its later genre rows are counted as duplicates.
    std::string skey = song_key(fields[c_name], fields[c_artists]);
    if (const auto it = row_of_song.find(skey); it != row_of_song.end()) {
      ++stats.duplicate_songs;
      row_of_id.emplace(fields[c_id], it->second);
      add_genre(rows[it->second].meta, std::move(fields[c_genre]));
      continue;
    }

    row_of_song.emplace(std::move(skey), rows.size());
    row_of_id.emplace(fields[c_id], rows.size());
    rows.push_back(ParsedRow{
        .meta = {.spotify_id = std::move(fields[c_id]),
                 .name = std::move(fields[c_name]),
                 .artists = std::move(fields[c_artists]),
                 .genres = {std::move(fields[c_genre])},
                 .popularity = *pop},
        .raw = {.energy = *energy,
                .valence = *valence,
                .danceability = *dance,
                .acousticness = *acoustic,
                .tempo = *tempo,
                .key = *key,
                .mode = *mode,
                .speechiness = *speech,
                .instrumentalness = *instr},
    });
  }
  if (rows.empty()) throw std::runtime_error("CSV has no usable rows");

  // Pass 2: fit the normalizer on ALL rows, then transform each one.
  // (Two passes because the tempo percentiles need every row first.)
  std::vector<RawFeatures> raws;
  raws.reserve(rows.size());
  for (const auto& r : rows) raws.push_back(r.raw);
  Normalizer normalizer = Normalizer::fit(raws, weights);

  FeatureStore store;
  store.reserve(rows.size());
  for (auto& r : rows) store.add(std::move(r.meta), normalizer.transform(r.raw));
  stats.songs = store.size();

  return LoadedDataset{std::move(store), normalizer, stats};
}

LoadedDataset load_spotify_csv(const std::filesystem::path& path, FeatureWeights weights) {
  std::ifstream in(path);  // RAII: file closes automatically when `in` goes out of scope
  if (!in) throw std::runtime_error("cannot open " + path.string());
  return load_spotify_csv(in, weights);
}

}  // namespace plgen
