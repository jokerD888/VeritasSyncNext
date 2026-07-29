#include "engine/storage/ignore_rules.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>

VSYNC_TEST(IgnoreRulesApplyDefaultsAndNormalizeWindowsPaths) {
  veritassync::storage::IgnoreRules rules;
  VSYNC_CHECK(rules.IsIgnored("download.part"));
  VSYNC_CHECK(rules.IsIgnored(".git/config"));
  VSYNC_CHECK(rules.IsIgnored("nested\\download.part"));
  VSYNC_CHECK(!rules.IsIgnored("keep.txt"));
  VSYNC_CHECK_THROWS(rules.IsIgnored("../outside.txt"));
}

VSYNC_TEST(IgnoreRulesSupportGitIgnoreStyleOrderingAndDirectories) {
  veritassync::storage::IgnoreRules rules;
  rules.Load("# generated output\n*.log\nlogs/\n!important.log\nkeep.txt\n");
  VSYNC_CHECK(rules.IsIgnored("logs/debug.txt"));
  VSYNC_CHECK(rules.IsIgnored("nested/error.log"));
  VSYNC_CHECK(!rules.IsIgnored("important.log"));
  VSYNC_CHECK(rules.IsIgnored("keep.txt"));
}

VSYNC_TEST(IgnoreRulesSupportRootedDoubleStarAndCharacterClassPatterns) {
  veritassync::storage::IgnoreRules rules;
  rules.Load("/root-only.txt\nsrc/**/generated?.[ch]\n[!a]*.tmp\n");
  VSYNC_CHECK(rules.IsIgnored("root-only.txt"));
  VSYNC_CHECK(!rules.IsIgnored("nested/root-only.txt"));
  VSYNC_CHECK(rules.IsIgnored("src/generated1.c"));
  VSYNC_CHECK(rules.IsIgnored("src/lib/generated2.h"));
  VSYNC_CHECK(!rules.IsIgnored("src/lib/generated2.cpp"));
  VSYNC_CHECK(rules.IsIgnored("build.tmp"));
  VSYNC_CHECK(!rules.IsIgnored("apple.tmp"));
}

VSYNC_TEST(IgnoreRulesLoadTaskRootFile) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("veritassync-ignore-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  { std::ofstream stream(root / ".veritasignore"); stream << "*.generated\n"; }
  veritassync::storage::IgnoreRules rules;
  rules.LoadFile(root);
  VSYNC_CHECK(rules.IsIgnored("build.generated"));
  std::filesystem::remove_all(root);
}

VSYNC_TEST(IgnoreRulesTreatEscapedCommentAndBangAsLiteralPatterns) {
  veritassync::storage::IgnoreRules rules;
  rules.Load("\\!important.txt\n\\#private.txt\n");
  VSYNC_CHECK(rules.IsIgnored("!important.txt"));
  VSYNC_CHECK(rules.IsIgnored("#private.txt"));
}
