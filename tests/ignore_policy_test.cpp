#include "engine/storage/ignore_policy.h"
#include "engine/storage/ignore_rules.h"
#include "tests/test_framework.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

class TemporaryTree {
 public:
  TemporaryTree() : root_(std::filesystem::temp_directory_path() /
      ("veritassync-ignore-policy-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(root_ / "src");
    std::filesystem::create_directories(root_ / "tests");
    std::filesystem::create_directories(root_ / "logs");
    Write("src/main.cpp");
    Write("src/main_test.cpp");
    Write("tests/unit.cpp");
    Write("logs/error.log");
    Write("README.md");
  }
  ~TemporaryTree() { std::filesystem::remove_all(root_); }
  [[nodiscard]] const std::filesystem::path& Root() const { return root_; }

 private:
  void Write(const std::string& relative) const {
    std::ofstream stream(root_ / relative, std::ios::binary);
    stream << relative;
  }
  std::filesystem::path root_;
};

}  // namespace

VSYNC_TEST(IgnoreRulesRejectMalformedOrDangerousGeneratedPatterns) {
  VSYNC_CHECK_THROWS(veritassync::storage::IgnoreRules::Validate("../outside\n"));
  VSYNC_CHECK_THROWS(veritassync::storage::IgnoreRules::Validate("broken[abc\n"));
  VSYNC_CHECK_THROWS(veritassync::storage::IgnoreRules::Validate("folder\\file\n"));
  veritassync::storage::IgnoreRules::Validate("*.log\n!important.log\ntests/**\n");
}

VSYNC_TEST(IgnorePolicyPrivateContextDoesNotExposeRelativePaths) {
  TemporaryTree tree;
  const auto context = veritassync::storage::IgnorePolicy::BuildContext(
      tree.Root(), "ignore test and log files", veritassync::storage::IgnoreContextMode::kPrivate);
  VSYNC_CHECK(context.scanned_files == 5);
  VSYNC_CHECK(context.relevant_paths.empty());
  VSYNC_CHECK(context.comparison_paths.empty());
  VSYNC_CHECK(context.directory_summary.find("main_test.cpp") == std::string::npos);
  VSYNC_CHECK(context.directory_summary.find("tests") == std::string::npos);
  VSYNC_CHECK(context.directory_summary.find(".cpp") != std::string::npos);
}

VSYNC_TEST(IgnorePolicyPreciseContextIncludesBoundedRelevantAndComparisonSamples) {
  TemporaryTree tree;
  const auto context = veritassync::storage::IgnorePolicy::BuildContext(
      tree.Root(), "忽略 test 文件", veritassync::storage::IgnoreContextMode::kPrecise);
  VSYNC_CHECK(context.scanned_files == 5);
  VSYNC_CHECK(std::ranges::find(context.relevant_paths, "src/main_test.cpp") != context.relevant_paths.end());
  VSYNC_CHECK(std::ranges::find(context.comparison_paths, "src/main.cpp") != context.comparison_paths.end());
}

VSYNC_TEST(IgnorePolicyPreviewReportsTrackedTombstoneRiskAndReincludedFiles) {
  TemporaryTree tree;
  const std::vector<std::string> tracked{"src/main_test.cpp", "logs/error.log"};
  const auto preview = veritassync::storage::IgnorePolicy::Preview(
      tree.Root(), "*.log\n", "**/*_test.cpp\n", tracked);
  VSYNC_CHECK(preview.currently_ignored == 1);
  VSYNC_CHECK(preview.proposed_ignored == 1);
  VSYNC_CHECK(preview.newly_ignored == 1);
  VSYNC_CHECK(preview.newly_included == 1);
  VSYNC_CHECK(preview.tracked_newly_ignored == 1);
  VSYNC_CHECK(preview.tracked_deletion_samples.front() == "src/main_test.cpp");
}

VSYNC_TEST(IgnorePolicyInventoryStopsAtConfiguredFileLimit) {
  TemporaryTree tree;
  const auto context = veritassync::storage::IgnorePolicy::BuildContext(
      tree.Root(), "ignore files", veritassync::storage::IgnoreContextMode::kPrivate, 2);
  VSYNC_CHECK(context.scanned_files == 2);
  VSYNC_CHECK(context.truncated);
}
