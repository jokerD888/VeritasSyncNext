#include "engine/common/content_hash.h"
#include "engine/storage/manifest_scanner.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class ScanFixture {
 public:
  ScanFixture() {
    root_ = std::filesystem::temp_directory_path() /
            ("veritassync-scan-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root_ / "generated");
    std::filesystem::create_directories(root_ / "empty");
  }
  ~ScanFixture() { std::error_code error; std::filesystem::remove_all(root_, error); }
  void Write(const std::filesystem::path& relative_path, const std::string_view content) const {
    std::filesystem::create_directories((root_ / relative_path).parent_path());
    std::ofstream stream(root_ / relative_path, std::ios::binary);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  }
  [[nodiscard]] const std::filesystem::path& Root() const { return root_; }

 private:
  std::filesystem::path root_;
};

}  // namespace

VSYNC_TEST(ManifestScannerHashesFilesAndPreservesEmptyDirectories) {
  ScanFixture fixture;
  fixture.Write("notes.txt", "abc");
  fixture.Write("generated/cache.bin", "ignore me");
  fixture.Write("generated/keep.txt", "keep me");
  fixture.Write("download.part", "partial");
  veritassync::storage::IgnoreRules rules;
  rules.Load("generated/\n!generated/keep.txt\n");
  veritassync::storage::ManifestScanner scanner(std::move(rules));
  const auto snapshot = scanner.Scan(fixture.Root());

  VSYNC_CHECK(snapshot.size() == 3);
  VSYNC_CHECK(snapshot[0].relative_path == "empty");
  VSYNC_CHECK(snapshot[0].kind == veritassync::storage::SnapshotKind::kDirectory);
  VSYNC_CHECK(snapshot[1].relative_path == "generated/keep.txt");
  VSYNC_CHECK(snapshot[2].relative_path == "notes.txt");
  VSYNC_CHECK(snapshot[2].content_hash == std::optional{veritassync::common::Blake3(std::vector<std::uint8_t>{'a', 'b', 'c'})});
}
