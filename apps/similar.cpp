// Look up a song by name and print its most similar songs.
//
//   plgen_similar "hey ya"                       # name search (case-insensitive)
//   plgen_similar "hey ya" --artist outkast      # narrow by artist
//   plgen_similar "hey ya" --pick 2 --k 15       # use the 2nd match, show 15 results
//   plgen_similar "hey ya" --no-rerank           # vector distance only, for comparison
//   Tuning: --genre-penalty X --pop-bonus X --candidates N --per-artist N --related-full X
//
// Shows two lists side by side so you can see what the re-ranking stage changes.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "plgen/brute_force.hpp"
#include "plgen/dataset.hpp"
#include "plgen/recommender.hpp"

namespace {

struct Options {
  std::string query;
  std::string artist;
  std::string csv = "data/dataset.csv";
  std::size_t k = 10;
  std::size_t pick = 1;
  bool rerank = true;
  plgen::RerankOptions rr;
};

[[noreturn]] void usage() {
  std::cerr << "usage: plgen_similar \"song name\" [--artist NAME] [--pick N] [--k K] "
               "[--csv PATH] [--no-rerank]\n";
  std::exit(2);
}

Options parse_args(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) usage();
      return argv[++i];
    };
    if (arg == "--artist") o.artist = value();
    else if (arg == "--csv") o.csv = value();
    else if (arg == "--k") o.k = std::stoul(value());
    else if (arg == "--pick") o.pick = std::stoul(value());
    else if (arg == "--no-rerank") o.rerank = false;
    else if (arg == "--genre-penalty") o.rr.genre_penalty = std::stof(value());
    else if (arg == "--pop-bonus") o.rr.popularity_bonus = std::stof(value());
    else if (arg == "--candidates") o.rr.candidates = std::stoul(value());
    else if (arg == "--per-artist") o.rr.max_per_artist = std::stoul(value());
    else if (arg == "--related-full") o.rr.related_full = std::stof(value());
    else if (arg.starts_with("--")) usage();
    else if (o.query.empty()) o.query = arg;
    else usage();
  }
  if (o.query.empty() || o.pick == 0) usage();
  return o;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string join(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& s : v) out += (out.empty() ? "" : ", ") + s;
  return out;
}

void print_song(const plgen::SongMeta& m) {
  std::cout << m.name << " - " << m.artists << "  [" << join(m.genres) << "]  pop "
            << m.popularity << '\n';
}

}  // namespace

int main(int argc, char** argv) try {
  const Options opt = parse_args(argc, argv);
  const auto ds = plgen::load_spotify_csv(opt.csv);
  const auto& store = ds.store;

  // Find matching songs, most popular first (usually the version you mean).
  const std::string q = lower(opt.query), a = lower(opt.artist);
  std::vector<plgen::SongId> matches;
  for (plgen::SongId id = 0; id < store.size(); ++id) {
    const auto& m = store.meta(id);
    if (lower(m.name).find(q) == std::string::npos) continue;
    if (!a.empty() && lower(m.artists).find(a) == std::string::npos) continue;
    matches.push_back(id);
  }
  if (matches.empty()) {
    std::cout << "No song matching \"" << opt.query << "\"" << (a.empty() ? "" : " by " + opt.artist)
              << ". It may not be in the dataset; try part of the title.\n";
    return 1;
  }
  std::stable_sort(matches.begin(), matches.end(), [&](plgen::SongId x, plgen::SongId y) {
    return store.meta(x).popularity > store.meta(y).popularity;
  });
  if (opt.pick > matches.size()) {
    std::cerr << "--pick " << opt.pick << " but only " << matches.size() << " matches\n";
    return 1;
  }

  if (matches.size() > 1) {
    std::cout << matches.size() << " matches (use --pick N or --artist to choose):\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(matches.size(), 8); ++i) {
      std::cout << (i + 1 == opt.pick ? " > " : "   ") << i + 1 << ". ";
      print_song(store.meta(matches[i]));
    }
    std::cout << '\n';
  }

  const plgen::SongId query = matches[opt.pick - 1];
  std::cout << "Songs similar to: ";
  print_song(store.meta(query));
  std::cout << std::fixed << std::setprecision(4);

  if (opt.rerank) {
    const auto genres = plgen::GenreGraph::build(store);
    std::cout << "\nRe-ranked (what the API will return)    score / distance / genre match\n";
    int rank = 1;
    for (const auto& r : plgen::similar_songs(store, genres, query, opt.k, opt.rr)) {
      std::cout << std::setw(3) << rank++ << ". " << r.score << " / " << r.distance << " / "
                << std::setprecision(2) << r.relatedness << std::setprecision(4) << "  ";
      print_song(store.meta(r.id));
    }
  }

  std::cout << "\nAudio features only (before re-ranking)    distance\n";
  int rank = 1;
  for (const auto& nb : plgen::brute_force_knn(store, store.vector(query), opt.k, query)) {
    std::cout << std::setw(3) << rank++ << ". " << nb.dist << "  ";
    print_song(store.meta(nb.id));
  }
  return 0;
} catch (const std::exception& e) {
  std::cerr << "error: " << e.what() << '\n';
  return 1;
}
