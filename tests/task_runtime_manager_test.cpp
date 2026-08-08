#include "engine/runtime/task_runtime_manager.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
bool WaitUntil(const std::function<bool()>& condition, const std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  return condition();
}
}  // namespace

VSYNC_TEST(TaskRuntimeManagerAutomaticallyScansWatcherChangesAndHonorsPause) {
  const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto root = std::filesystem::temp_directory_path() / ("veritassync-runtime-root-" + stamp);
  const auto database_path =
      std::filesystem::temp_directory_path() / ("veritassync-runtime-db-" + stamp + ".db");
  std::filesystem::create_directories(root);
  {
    veritassync::storage::Database database(database_path);
    database.ApplyMigrations();
    database.CreateTask({"watch", "one_way", "source", root.string()});
    veritassync::runtime::TaskRuntimeOptions options;
    options.debounce = std::chrono::milliseconds(80);
    options.error_retry = std::chrono::milliseconds(200);
    options.periodic_verification = std::chrono::hours(1);
    veritassync::runtime::TaskRuntimeManager runtime(database, "device-local", options);
    runtime.Start();
    VSYNC_CHECK(WaitUntil(
        [&] {
          std::scoped_lock lock(database.AccessMutex());
          return database.RuntimeState("watch").last_scan_at_ms.has_value();
        },
        std::chrono::seconds(5)));

    {
      std::ofstream stream(root / "first.txt");
      stream << "first";
    }
    VSYNC_CHECK(WaitUntil(
        [&] {
          std::scoped_lock lock(database.AccessMutex());
          return database.FindFileRecord("watch", "first.txt").has_value();
        },
        std::chrono::seconds(5)));

    runtime.PauseTask("watch");
    {
      std::ofstream stream(root / "paused.txt");
      stream << "paused";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    {
      std::scoped_lock lock(database.AccessMutex());
      VSYNC_CHECK(!database.FindFileRecord("watch", "paused.txt").has_value());
      VSYNC_CHECK(!database.RuntimeState("watch").enabled);
    }
    runtime.ResumeTask("watch");
    VSYNC_CHECK(WaitUntil(
        [&] {
          std::scoped_lock lock(database.AccessMutex());
          return database.FindFileRecord("watch", "paused.txt").has_value();
        },
        std::chrono::seconds(5)));
    runtime.Stop();
  }
  std::filesystem::remove_all(root);
  std::filesystem::remove(database_path);
  std::filesystem::remove(database_path.string() + "-shm");
  std::filesystem::remove(database_path.string() + "-wal");
}
