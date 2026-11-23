#include "plgen/groundtruth.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace plgen {
namespace {

constexpr std::array<char, 4> kMagic{'P', 'L', 'G', 'T'};
constexpr std::uint32_t kVersion = 1;

void write_u32(std::ofstream& out, std::uint32_t v) {
  out.write(reinterpret_cast<const char*>(&v), sizeof v);
}

std::uint32_t read_u32(std::ifstream& in) {
  std::uint32_t v = 0;
  in.read(reinterpret_cast<char*>(&v), sizeof v);
  if (!in) throw std::runtime_error("groundtruth: truncated file");
  return v;
}

}  // namespace

void write_groundtruth(const std::filesystem::path& path, const GroundTruth& gt) {
  if (gt.queries.size() != gt.answers.size()) {
    throw std::invalid_argument("groundtruth: queries/answers size mismatch");
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("cannot write " + path.string());

  out.write(kMagic.data(), kMagic.size());
  write_u32(out, kVersion);
  write_u32(out, static_cast<std::uint32_t>(gt.queries.size()));
  write_u32(out, gt.k);
  for (std::size_t i = 0; i < gt.queries.size(); ++i) {
    write_u32(out, gt.queries[i]);
    write_u32(out, static_cast<std::uint32_t>(gt.answers[i].size()));
    for (SongId id : gt.answers[i]) write_u32(out, id);
  }
  if (!out) throw std::runtime_error("groundtruth: write failed");
}

GroundTruth read_groundtruth(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path.string());

  std::array<char, 4> magic{};
  in.read(magic.data(), magic.size());
  if (!in || magic != kMagic) throw std::runtime_error("groundtruth: bad magic");
  if (read_u32(in) != kVersion) throw std::runtime_error("groundtruth: unsupported version");

  GroundTruth gt;
  const std::uint32_t n = read_u32(in);
  gt.k = read_u32(in);
  gt.queries.reserve(n);
  gt.answers.reserve(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    gt.queries.push_back(read_u32(in));
    const std::uint32_t count = read_u32(in);
    std::vector<SongId> ids(count);
    for (auto& id : ids) id = read_u32(in);
    gt.answers.push_back(std::move(ids));
  }
  return gt;
}

double recall_at_k(const std::vector<SongId>& approx, const std::vector<SongId>& exact) {
  if (exact.empty()) return 1.0;
  const std::unordered_set<SongId> truth(exact.begin(), exact.end());
  const auto hits = std::count_if(approx.begin(), approx.end(),
                                  [&](SongId id) { return truth.contains(id); });
  return static_cast<double>(hits) / static_cast<double>(exact.size());
}

}  // namespace plgen
