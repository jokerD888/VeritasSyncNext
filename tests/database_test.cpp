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
    VSYNC_CHECK(database.SchemaVersion() == 5);
    database.CreateTask({"task-1", "one_way", "source", "C:/sync"});
    const auto task = database.FindTask("task-1");
    VSYNC_CHECK(task.has_value());
    VSYNC_CHECK(task->root_path == "C:/sync");
    VSYNC_CHECK(!database.FindTask("missing").has_value());
    VSYNC_CHECK_THROWS(database.CreateTask({"invalid-one-way", "one_way", "peer", "C:/sync"}));
    VSYNC_CHECK_THROWS(database.CreateTask({"invalid-two-way", "bidirectional", "source", "C:/sync"}));
    VSYNC_CHECK(database.CountRows("tasks") == 1);
    VSYNC_CHECK(database.CountRows("file_records") == 0);
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-shm");
  std::filesystem::remove(path.string() + "-wal");
}

VSYNC_TEST(DatabasePersistsVersionedIgnorePoliciesAndDeletesTheirHistoryWithTask) {
  const auto path = std::filesystem::temp_directory_path() / ("veritassync-ignore-history-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  {
    veritassync::storage::Database database(path);
    database.ApplyMigrations();
    database.CreateTask({"task-1", "one_way", "source", "C:/sync"});
    const auto first = database.RecordIgnorePolicyRevision("task-1", "*.log\n", std::string(64, '1'), "manual", 100);
    const auto second = database.RecordIgnorePolicyRevision("task-1", "*.log\nbuild/\n", std::string(64, '2'), "ai", 101);
    VSYNC_CHECK(first.revision == 1);
    VSYNC_CHECK(second.revision == 2);
    const auto current = database.CurrentIgnorePolicyRevision("task-1");
    VSYNC_CHECK(current.has_value());
    VSYNC_CHECK(current->content == "*.log\nbuild/\n");
    const auto history = database.ListIgnorePolicyRevisions("task-1");
    VSYNC_CHECK(history.size() == 2);
    VSYNC_CHECK(history.front().source == "ai");
    VSYNC_CHECK_THROWS(database.RecordIgnorePolicyRevision("task-1", "x", "short", "ai", 102));
    database.DeleteTask("task-1");
    VSYNC_CHECK(database.CountRows("ignore_policy_revisions") == 0);
    VSYNC_CHECK(database.CountRows("ignore_policy_state") == 0);
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-shm");
  std::filesystem::remove(path.string() + "-wal");
}

VSYNC_TEST(DatabaseListsDeletesTasksAndPersistsEngineEvents) {
  const auto path = std::filesystem::temp_directory_path() / ("veritassync-events-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  {
    veritassync::storage::Database database(path);
    database.ApplyMigrations();
    database.CreateTask({"task-a", "one_way", "source", "C:/a"});
    database.CreateTask({"task-b", "bidirectional", "peer", "C:/b"});
    VSYNC_CHECK(database.ListTasks().size() == 2);
    database.RecordEngineEvent({0, std::optional<std::string>{"task-a"}, "info", "task created", 100});
    database.RecordEngineEvent({0, std::nullopt, "warning", "engine restarted", 101});
    const auto events = database.ListEngineEvents();
    VSYNC_CHECK(events.size() == 2);
    VSYNC_CHECK(events[0].message == "engine restarted");
    database.DeleteTask("task-a");
    VSYNC_CHECK(!database.FindTask("task-a").has_value());
    VSYNC_CHECK(database.ListTasks().size() == 1);
    VSYNC_CHECK(database.ListEngineEvents().size() == 2);
    VSYNC_CHECK(!database.ListEngineEvents()[1].task_id.has_value());
    VSYNC_CHECK_THROWS(database.DeleteTask("task-a"));
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-shm");
  std::filesystem::remove(path.string() + "-wal");
}

VSYNC_TEST(DatabasePersistsVersionLineageLamportClockAndConflicts) {
  const auto path = std::filesystem::temp_directory_path() / ("veritassync-versions-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  {
    veritassync::storage::Database database(path);
    database.ApplyMigrations();
    database.CreateTask({"task-1", "bidirectional", "peer", "C:/sync"});
    database.RecordVersionLineage({"task-1", "v1", std::nullopt});
    database.RecordVersionLineage({"task-1", "v2", std::optional<std::string>{"v1"}});
    database.RecordVersionLineage({"task-1", "v3", std::optional<std::string>{"v2"}});
    const auto lineage = database.ListVersionLineage("task-1");
    VSYNC_CHECK(lineage.size() == 3);
    VSYNC_CHECK(lineage[2].version_id == "v3");
    VSYNC_CHECK(lineage[2].parent_version_id == std::optional<std::string>{"v2"});
    VSYNC_CHECK(database.IsVersionAncestor("task-1", "v1", "v3"));
    VSYNC_CHECK(!database.IsVersionAncestor("task-1", "v3", "v1"));
    VSYNC_CHECK_THROWS(database.RecordVersionLineage({"task-1", "v2", std::nullopt}));
    VSYNC_CHECK(database.AdvanceLogicalClock("task-1") == 1);
    VSYNC_CHECK(database.AdvanceLogicalClock("task-1", 7) == 8);
    VSYNC_CHECK(database.AdvanceLogicalClock("task-1", 3) == 9);
    database.RecordConflict({"conflict-1", "task-1", "notes.txt", "v2",
                             "notes.conflict.device-b.8.txt", "unresolved", 1234});
    const auto conflicts = database.ListConflicts("task-1");
    VSYNC_CHECK(conflicts.size() == 1);
    VSYNC_CHECK(database.ListConflicts().size() == 1);
    VSYNC_CHECK(conflicts[0].conflict_path == "notes.conflict.device-b.8.txt");
    database.UpdateConflictState("conflict-1", "resolved");
    VSYNC_CHECK(database.ListConflicts("task-1")[0].state == "resolved");
    VSYNC_CHECK_THROWS(database.UpdateConflictState("missing", "resolved"));
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
    database.CreateTransfer({transfer_id, "task-1", "device-b", "download", std::vector<std::uint8_t>(32, 7), "active", 100, 100, "nested/file.bin"});
    VSYNC_CHECK_THROWS(database.CreateTransfer({{}, "task-1", "device-b", "download", std::vector<std::uint8_t>(32, 7), "unknown", 100, 100}));
    VSYNC_CHECK_THROWS(database.CreateTransfer({transfer_id, "task-1", "device-b", "download", {1}, "active", 100, 100}));
    database.MarkTransferChunkCompleted(transfer_id, 3, 101);
    database.MarkTransferChunkCompleted(transfer_id, 0, 102);
    const std::vector<std::uint64_t> batch{3, 5, 5};
    database.MarkTransferChunksCompleted(transfer_id, batch, 103);
    VSYNC_CHECK(database.CompletedTransferChunks(transfer_id) == std::vector<std::uint64_t>({0, 3, 5}));
    const auto active = database.FindActiveDownloadTransfer("task-1", "device-b", "nested/file.bin", std::vector<std::uint8_t>(32, 7));
    VSYNC_CHECK(active.has_value());
    VSYNC_CHECK(active->transfer_id == transfer_id);
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-shm");
  std::filesystem::remove(path.string() + "-wal");
}
