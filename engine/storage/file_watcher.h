#pragma once

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

namespace veritassync::storage {

// Recursive Windows directory watcher. Notifications are intentionally reduced
// to a dirty signal: correctness comes from the subsequent deterministic scan,
// while ReadDirectoryChangesW only avoids repeatedly scanning an unchanged tree.
class FileWatcher {
 public:
  using ChangeCallback = std::function<void()>;

  FileWatcher(std::filesystem::path root, ChangeCallback callback);
  ~FileWatcher();
  FileWatcher(const FileWatcher&) = delete;
  FileWatcher& operator=(const FileWatcher&) = delete;

  void Start();
  void Stop();
  [[nodiscard]] bool Running() const { return running_.load(); }

 private:
  void Watch();

  std::filesystem::path root_;
  ChangeCallback callback_;
  std::atomic_bool stopping_{false};
  std::atomic_bool running_{false};
  mutable std::mutex handle_mutex_;
  HANDLE directory_ = INVALID_HANDLE_VALUE;
  std::thread thread_;
};

}  // namespace veritassync::storage
