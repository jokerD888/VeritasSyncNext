#include "engine/sync/manifest_diff.h"
#include "tests/test_framework.h"

#include <array>

namespace {

veritassync::storage::SnapshotEntry File(std::string path, const std::uint8_t marker) {
  veritassync::common::ContentHash hash{};
  hash.front() = marker;
  return {std::move(path), veritassync::storage::SnapshotKind::kFile, 10, 1, hash};
}

veritassync::storage::FileRecord Known(std::string path, veritassync::storage::FileKind kind,
                                       const std::uint8_t marker = 0) {
  std::vector<std::uint8_t> hash(32, 0);
  hash.front() = marker;
  return {"task-1", std::move(path), kind, 10, 1, std::move(hash), "version", "device", 0, std::nullopt};
}

}  // namespace

VSYNC_TEST(ManifestDiffFindsContentChangesAndTombstonesOnlyLivePaths) {
  using namespace veritassync;
  const std::vector<storage::SnapshotEntry> snapshot{
      File("changed.txt", 2), File("new.txt", 3), {"empty", storage::SnapshotKind::kDirectory, 0, 1, std::nullopt}};
  const std::vector<storage::FileRecord> known{
      Known("changed.txt", storage::FileKind::kFile, 1), Known("same.txt", storage::FileKind::kFile, 0),
      Known("gone.txt", storage::FileKind::kFile, 4), Known("already-deleted.txt", storage::FileKind::kTombstone)};
  const auto diff = sync::DiffManifest(snapshot, known);
  VSYNC_CHECK(diff.created_or_changed.size() == 3);
  VSYNC_CHECK(diff.created_or_changed[0].relative_path == "changed.txt");
  VSYNC_CHECK(diff.created_or_changed[1].relative_path == "empty");
  VSYNC_CHECK(diff.created_or_changed[2].relative_path == "new.txt");
  VSYNC_CHECK(diff.deleted_paths == std::vector<std::string>({"gone.txt", "same.txt"}));
}

VSYNC_TEST(ManifestDiffDoesNotCreateVersionForIdenticalFileContent) {
  using namespace veritassync;
  const std::vector<storage::SnapshotEntry> snapshot{File("same.txt", 7)};
  const std::vector<storage::FileRecord> known{Known("same.txt", storage::FileKind::kFile, 7)};
  const auto diff = sync::DiffManifest(snapshot, known);
  VSYNC_CHECK(diff.created_or_changed.empty());
  VSYNC_CHECK(diff.deleted_paths.empty());
}
