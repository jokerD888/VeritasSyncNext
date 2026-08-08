#include "engine/storage/file_watcher.h"
#include "tests/test_framework.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>

VSYNC_TEST(FileWatcherReportsRecursiveDirectoryChangesAndStops) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("veritassync-watcher-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root / "nested");
  std::mutex mutex;
  std::condition_variable changed;
  bool observed = false;
  {
    veritassync::storage::FileWatcher watcher(root, [&] {
      {
        std::scoped_lock lock(mutex);
        observed = true;
      }
      changed.notify_one();
    });
    watcher.Start();
    {
      std::ofstream stream(root / "nested" / "changed.txt");
      stream << "change";
    }
    std::unique_lock lock(mutex);
    VSYNC_CHECK(changed.wait_for(lock, std::chrono::seconds(5), [&] { return observed; }));
    VSYNC_CHECK(watcher.Running());
    lock.unlock();
    watcher.Stop();
    VSYNC_CHECK(!watcher.Running());
  }
  std::filesystem::remove_all(root);
}
