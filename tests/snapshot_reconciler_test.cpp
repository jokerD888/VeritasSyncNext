#include "engine/sync/snapshot_reconciler.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace {

veritassync::storage::SnapshotEntry File(std::string path, const std::uint8_t marker, const std::int64_t modified) {
  veritassync::common::ContentHash hash{};
  hash.front() = marker;
  return {std::move(path), veritassync::storage::SnapshotKind::kFile, 10, modified, hash};
}

std::vector<std::uint8_t> Hash(const std::uint8_t marker) {
  std::vector<std::uint8_t> hash(32);
  hash.front() = marker;
  return hash;
}

}  // namespace

VSYNC_TEST(SnapshotReconcilerWritesNewVersionsMetadataAndTombstonesAtomically) {
  const auto path = std::filesystem::temp_directory_path() / ("veritassync-reconcile-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  {
    veritassync::storage::Database database(path);
    database.ApplyMigrations();
    database.CreateTask({"task-1", "one_way", "source", "C:/sync"});
    database.UpsertFileRecord({"task-1", "same.txt", veritassync::storage::FileKind::kFile, 10, 1,
                               Hash(1), "old-version", "device-a", 0, std::nullopt});
    database.UpsertFileRecord({"task-1", "gone.txt", veritassync::storage::FileKind::kFile, 10, 1,
                               Hash(2), "gone-version", "device-a", 0, std::nullopt});
    int version = 0;
    veritassync::sync::SnapshotReconciler reconciler([&] { return "version-" + std::to_string(++version); });
    const auto result = reconciler.Apply(database, {File("same.txt", 1, 99), File("new.txt", 3, 4)},
                                         {"task-1", "device-a", 0, 1234});
    VSYNC_CHECK(result.created_or_changed == 1);
    VSYNC_CHECK(result.tombstoned == 1);
    const auto same = database.FindFileRecord("task-1", "same.txt");
    const auto created = database.FindFileRecord("task-1", "new.txt");
    const auto tombstone = database.FindFileRecord("task-1", "gone.txt");
    VSYNC_CHECK(same->version_id == "old-version");
    VSYNC_CHECK(same->mtime_ns == 99);
    VSYNC_CHECK(created->version_id == "version-1");
    VSYNC_CHECK(tombstone->kind == veritassync::storage::FileKind::kTombstone);
    VSYNC_CHECK(tombstone->version_id == "version-2");
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-shm");
  std::filesystem::remove(path.string() + "-wal");
}
