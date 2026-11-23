#include <gtest/gtest.h>

#include <filesystem>

#include "plgen/groundtruth.hpp"

using namespace plgen;

TEST(GroundTruth, RoundTrip) {
  const GroundTruth gt{.k = 3, .queries = {5, 9}, .answers = {{1, 2, 3}, {4, 5}}};
  const auto path = std::filesystem::temp_directory_path() / "plgen_gt_test.bin";
  write_groundtruth(path, gt);
  const GroundTruth back = read_groundtruth(path);
  std::filesystem::remove(path);

  EXPECT_EQ(back.k, 3u);
  EXPECT_EQ(back.queries, gt.queries);
  EXPECT_EQ(back.answers, gt.answers);
}

TEST(GroundTruth, Recall) {
  EXPECT_DOUBLE_EQ(recall_at_k({1, 2, 3, 4}, {1, 2, 3, 4}), 1.0);
  EXPECT_DOUBLE_EQ(recall_at_k({1, 2, 9, 8}, {1, 2, 3, 4}), 0.5);
  EXPECT_DOUBLE_EQ(recall_at_k({}, {1, 2}), 0.0);
}
