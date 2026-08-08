#pragma once

#include "engine/storage/database.h"
#include "engine/storage/file_watcher.h"
#include "engine/sync/snapshot_reconciler.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace veritassync::runtime {

struct TaskRuntimeOptions {
  std::chrono::milliseconds debounce{350};
  std::chrono::milliseconds error_retry{5000};
  std::chrono::milliseconds periodic_verification{std::chrono::hours(6)};
};

class TaskRuntimeManager {
 public:
  using ScanCompleted = std::function<void(const std::string&)>;

  TaskRuntimeManager(storage::Database& database, std::string device_id,
                     TaskRuntimeOptions options = {});
  ~TaskRuntimeManager();
  TaskRuntimeManager(const TaskRuntimeManager&) = delete;
  TaskRuntimeManager& operator=(const TaskRuntimeManager&) = delete;

  void Start();
  void Stop();
  void TaskCreated(const std::string& task_id);
  void TaskDeleting(const std::string& task_id);
  void PauseTask(const std::string& task_id);
  void ResumeTask(const std::string& task_id);
  [[nodiscard]] sync::ReconcileResult ScanNow(const std::string& task_id);
  void SetScanCompletedCallback(ScanCompleted callback);

 private:
  struct Entry {
    storage::TaskDefinition task;
    bool enabled = true;
    bool dirty = true;
    bool scanning = false;
    std::uint64_t generation = 1;
    std::chrono::steady_clock::time_point dirty_deadline;
    std::chrono::steady_clock::time_point periodic_deadline;
    std::unique_ptr<storage::FileWatcher> watcher;
  };

  void LoadTask(const std::string& task_id);
  void MarkDirty(const std::string& task_id);
  void WorkerLoop();
  [[nodiscard]] sync::ReconcileResult PerformScan(const std::string& task_id,
                                                  std::uint64_t generation);
  void RecordFailure(const std::string& task_id, const std::string& error);

  storage::Database& database_;
  std::string device_id_;
  TaskRuntimeOptions options_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool started_ = false;
  bool stopping_ = false;
  std::thread worker_;
  std::unordered_map<std::string, std::unique_ptr<Entry>> entries_;
  ScanCompleted scan_completed_;
};

}  // namespace veritassync::runtime
