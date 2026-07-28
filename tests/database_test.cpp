#include "engine/storage/database.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>

VSYNC_TEST(DatabaseMigrationsAreReplaySafeAndPersistTasks) {
  const auto path = std::filesystem::temp_directory_path() / ("veritassync-phase0-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  {
    veritassync::storage::Database database(path);
    database.ApplyMigrations();
    database.ApplyMigrations();
    VSYNC_CHECK(database.SchemaVersion() == 1);
    database.CreateTask({"task-1", "one_way", "source", "C:/sync"});
    const auto task = database.FindTask("task-1");
    VSYNC_CHECK(task.has_value());
    VSYNC_CHECK(task->root_path == "C:/sync");
    VSYNC_CHECK(!database.FindTask("missing").has_value());
    VSYNC_CHECK(database.CountRows("tasks") == 1);
    VSYNC_CHECK(database.CountRows("file_records") == 0);
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-shm");
  std::filesystem::remove(path.string() + "-wal");
}

VSYNC_TEST(DatabasePersistsFileVersionsAndTombstones) {
  const auto path = std::filesystem::temp_directory_path() / ("veritassync-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  {
    veritassync::storage::Database database(path);
    database.ApplyMigrations();
    database.CreateTask({"task-1", "one_way", "source", "C:/sync"});
    database.UpsertFileRecord({"task-1", "nested/notes.txt", veritassync::storage::FileKind::kFile, 42, 99,
                               {1, 2, 3}, "version-1", "device-a", 0, std::nullopt});
    const auto file = database.FindFileRecord("task-1", "nested/notes.txt");
    VSYNC_CHECK(file.has_value());
    VSYNC_CHECK(file->content_hash == std::vector<std::uint8_t>({1, 2, 3}));
    VSYNC_CHECK(!file->deleted_at_ms.has_value());
    database.RecordTombstone("task-1", "nested/notes.txt", "version-2", "device-a", 0, 1234);
    const auto tombstone = database.FindFileRecord("task-1", "nested/notes.txt");
    VSYNC_CHECK(tombstone.has_value());
    VSYNC_CHECK(tombstone->kind == veritassync::storage::FileKind::kTombstone);
    VSYNC_CHECK(tombstone->deleted_at_ms == std::optional<std::int64_t>{1234});
    VSYNC_CHECK(tombstone->content_hash.empty());
    VSYNC_CHECK_THROWS(database.UpsertFileRecord({"task-1", "../escape", veritassync::storage::FileKind::kFile, 1, 1,
                                                   {1}, "version-3", "device-a", 0, std::nullopt}));
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-shm");
  std::filesystem::remove(path.string() + "-wal");
}

VSYNC_TEST(DatabasePersistsCompletedTransferChunksForResume) {
  const auto path = std::filesystem::temp_directory_path() / ("veritassync-transfer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  {
    veritassync::storage::Database database(path);
    database.ApplyMigrations();
    database.CreateTask({"task-1", "one_way", "source", "C:/sync"});
    veritassync::storage::TransferId transfer_id{};
    transfer_id.front() = 42;
    database.CreateTransfer({transfer_id, "task-1", "device-b", "download", std::vector<std::uint8_t>(32, 7), "active", 100, 100});
    database.MarkTransferChunkCompleted(transfer_id, 3, 101);
    database.MarkTransferChunkCompleted(transfer_id, 0, 102);
    database.MarkTransferChunkCompleted(transfer_id, 3, 103);
    VSYNC_CHECK(database.CompletedTransferChunks(transfer_id) == std::vector<std::uint64_t>({0, 3}));
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-shm");
  std::filesystem::remove(path.string() + "-wal");
}
