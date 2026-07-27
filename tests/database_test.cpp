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
    VSYNC_CHECK(database.CountRows("tasks") == 1);
    VSYNC_CHECK(database.CountRows("file_records") == 0);
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-shm");
  std::filesystem::remove(path.string() + "-wal");
}
