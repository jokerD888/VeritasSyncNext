#include "engine/storage/safe_file_writer.h"
#include "engine/common/content_hash.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("veritassync-safe-writer-" + std::to_string(nonce));
    std::filesystem::create_directory(path_);
  }
  ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
  [[nodiscard]] const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

}  // namespace

VSYNC_TEST(SafeFileWriterCommitsOnlyFinalFileUnderTaskRoot) {
  TemporaryDirectory directory;
  veritassync::storage::SafeFileWriter writer(directory.Path());
  const std::vector<std::uint8_t> content{9, 8, 7, 6};
  writer.WriteAtomically("nested/notes.txt", content);

  const auto final_path = directory.Path() / "nested" / "notes.txt";
  VSYNC_CHECK(std::filesystem::exists(final_path));
  VSYNC_CHECK(ReadBytes(final_path) == std::vector<std::uint8_t>({9, 8, 7, 6}));
  for (const auto& entry : std::filesystem::directory_iterator(final_path.parent_path())) {
    VSYNC_CHECK(entry.path().extension() != ".part");
  }
}

VSYNC_TEST(SafeFileWriterRejectsEscapingPaths) {
  TemporaryDirectory directory;
  veritassync::storage::SafeFileWriter writer(directory.Path());
  const std::vector<std::uint8_t> content{1};
  VSYNC_CHECK_THROWS(writer.WriteAtomically("../outside.txt", content));
  VSYNC_CHECK_THROWS(writer.WriteAtomically("C:\\outside.txt", content));
  VSYNC_CHECK_THROWS(writer.WriteAtomically("nested/../outside.txt", content));
}

VSYNC_TEST(SafeFileWriterAtomicallyReplacesCompletedDestination) {
  TemporaryDirectory directory;
  veritassync::storage::SafeFileWriter writer(directory.Path());
  const std::vector<std::uint8_t> old_content{1, 2};
  const std::vector<std::uint8_t> new_content{3, 4, 5};
  writer.WriteAtomically("notes.txt", old_content);
  writer.WriteAtomically("notes.txt", new_content);
  VSYNC_CHECK(ReadBytes(directory.Path() / "notes.txt") == new_content);
}

VSYNC_TEST(SafeFileWriterResumesChunksThenVerifiesBeforeCommit) {
  TemporaryDirectory directory;
  veritassync::storage::SafeFileWriter writer(directory.Path());
  const std::vector<std::uint8_t> expected{'a', 'b', 'c', 'd', 'e', 'f'};
  writer.WritePartialChunk("nested/file.bin", 3, std::span(expected).subspan(3));
  VSYNC_CHECK_THROWS(writer.CommitPartial("nested/file.bin", expected.size(), veritassync::common::Blake3(expected)));
  writer.WritePartialChunk("nested/file.bin", 0, std::span(expected).first(3));
  writer.CommitPartial("nested/file.bin", expected.size(), veritassync::common::Blake3(expected));
  VSYNC_CHECK(ReadBytes(directory.Path() / "nested" / "file.bin") == expected);
  VSYNC_CHECK(!std::filesystem::exists(directory.Path() / "nested" / "file.bin.part"));
}

VSYNC_TEST(SafeFileWriterRejectsOverflowingChunkRange) {
  TemporaryDirectory directory;
  veritassync::storage::SafeFileWriter writer(directory.Path());
  const std::vector<std::uint8_t> bytes{1, 2};
  VSYNC_CHECK_THROWS(writer.WritePartialChunk("file.bin", (std::numeric_limits<std::uint64_t>::max)() - 1, bytes));
}

VSYNC_TEST(SafeFileWriterRemovesOnlyRegularSynchronizedFiles) {
  TemporaryDirectory directory;
  veritassync::storage::SafeFileWriter writer(directory.Path());
  const std::vector<std::uint8_t> bytes{1, 2, 3};
  writer.WriteAtomically("nested/file.bin", bytes);
  writer.RemoveFile("nested/file.bin");
  VSYNC_CHECK(!std::filesystem::exists(directory.Path() / "nested/file.bin"));
  writer.RemoveFile("nested/file.bin");
  VSYNC_CHECK_THROWS(writer.RemoveFile("nested"));
}
