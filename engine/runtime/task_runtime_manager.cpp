#include "engine/runtime/task_runtime_manager.h"

#include "engine/common/uuid.h"
#include "engine/storage/ignore_rules.h"
#include "engine/storage/manifest_scanner.h"
#include "engine/sync/task_policy.h"

#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

namespace veritassync::runtime {
namespace {

std::int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

TaskRuntimeManager::TaskRuntimeManager(storage::Database& database, std::string device_id,
                                       TaskRuntimeOptions options)
    : database_(database), device_id_(std::move(device_id)), options_(options) {
  if (device_id_.empty() || options_.debounce.count() < 0 || options_.error_retry.count() <= 0 ||
      options_.periodic_verification.count() <= 0) {
    throw std::invalid_argument("task runtime configuration is invalid");
  }
}

TaskRuntimeManager::~TaskRuntimeManager() { Stop(); }

void TaskRuntimeManager::Start() {
  {
    std::scoped_lock lock(mutex_);
    if (started_) return;
    stopping_ = false;
    started_ = true;
  }
  std::vector<std::string> task_ids;
  {
    std::scoped_lock database_lock(database_.AccessMutex());
    for (const auto& task : database_.ListTasks()) task_ids.push_back(task.task_id);
  }
  try {
    for (const auto& task_id : task_ids) LoadTask(task_id);
    worker_ = std::thread([this] { WorkerLoop(); });
  } catch (...) {
    Stop();
    throw;
  }
}

void TaskRuntimeManager::Stop() {
  std::vector<std::unique_ptr<Entry>> entries;
  {
    std::scoped_lock lock(mutex_);
    if (!started_) return;
    stopping_ = true;
    wake_.notify_all();
  }
  if (worker_.joinable()) worker_.join();
  {
    std::scoped_lock lock(mutex_);
    for (auto& [_, entry] : entries_) entries.push_back(std::move(entry));
    entries_.clear();
    started_ = false;
  }
  for (auto& entry : entries)
    if (entry->watcher) entry->watcher->Stop();
}

void TaskRuntimeManager::LoadTask(const std::string& task_id) {
  storage::TaskDefinition task;
  storage::TaskRuntimeState state;
  {
    std::scoped_lock database_lock(database_.AccessMutex());
    const auto found = database_.FindTask(task_id);
    if (!found.has_value()) throw std::invalid_argument("task does not exist");
    task = *found;
    state = database_.RuntimeState(task_id);
  }
  auto entry = std::make_unique<Entry>();
  entry->task = task;
  entry->enabled = state.enabled;
  entry->dirty = state.dirty;
  const auto now = std::chrono::steady_clock::now();
  entry->dirty_deadline = now + options_.debounce;
  entry->periodic_deadline = now + options_.periodic_verification;
  if (entry->enabled && sync::CanScanLocalChanges(task)) {
    entry->watcher = std::make_unique<storage::FileWatcher>(
        task.root_path, [this, task_id] { MarkDirty(task_id); });
    try {
      entry->watcher->Start();
    } catch (const std::exception& error) {
      entry->watcher.reset();
      entry->dirty = true;
      std::scoped_lock database_lock(database_.AccessMutex());
      database_.UpdateTaskRuntime(task_id, "error", true, state.last_scan_at_ms, error.what());
    }
  }
  std::unique_ptr<Entry> old;
  {
    std::scoped_lock lock(mutex_);
    auto existing = entries_.find(task_id);
    if (existing != entries_.end()) {
      old = std::move(existing->second);
      existing->second = std::move(entry);
    } else {
      entries_.emplace(task_id, std::move(entry));
    }
    wake_.notify_all();
  }
  if (old && old->watcher) old->watcher->Stop();
}

void TaskRuntimeManager::TaskCreated(const std::string& task_id) { LoadTask(task_id); }

void TaskRuntimeManager::TaskDeleting(const std::string& task_id) {
  std::unique_ptr<Entry> removed;
  {
    std::unique_lock lock(mutex_);
    auto entry = entries_.find(task_id);
    while (entry != entries_.end() && entry->second->scanning) {
      wake_.wait(lock);
      entry = entries_.find(task_id);
    }
    if (entry != entries_.end()) {
      removed = std::move(entry->second);
      entries_.erase(entry);
    }
  }
  if (removed && removed->watcher) removed->watcher->Stop();
}

void TaskRuntimeManager::PauseTask(const std::string& task_id) {
  std::unique_ptr<storage::FileWatcher> watcher;
  {
    std::unique_lock lock(mutex_);
    auto entry = entries_.find(task_id);
    if (entry == entries_.end()) throw std::invalid_argument("task runtime does not exist");
    while (entry->second->scanning) {
      wake_.wait(lock);
      entry = entries_.find(task_id);
      if (entry == entries_.end()) throw std::invalid_argument("task runtime does not exist");
    }
    entry->second->enabled = false;
    entry->second->dirty = false;
    watcher = std::move(entry->second->watcher);
  }
  if (watcher) watcher->Stop();
  std::scoped_lock database_lock(database_.AccessMutex());
  database_.SetTaskEnabled(task_id, false);
  database_.RecordEngineEvent({0, task_id, "info", "Paused task " + task_id, NowMilliseconds()});
}

void TaskRuntimeManager::ResumeTask(const std::string& task_id) {
  {
    std::scoped_lock database_lock(database_.AccessMutex());
    database_.SetTaskEnabled(task_id, true);
    database_.RecordEngineEvent({0, task_id, "info", "Resumed task " + task_id, NowMilliseconds()});
  }
  LoadTask(task_id);
}

void TaskRuntimeManager::MarkDirty(const std::string& task_id) {
  std::scoped_lock lock(mutex_);
  const auto entry = entries_.find(task_id);
  if (entry == entries_.end() || !entry->second->enabled) return;
  entry->second->dirty = true;
  ++entry->second->generation;
  entry->second->dirty_deadline = std::chrono::steady_clock::now() + options_.debounce;
  wake_.notify_all();
}

sync::ReconcileResult TaskRuntimeManager::ScanNow(const std::string& task_id) {
  std::uint64_t generation = 0;
  {
    std::scoped_lock lock(mutex_);
    const auto entry = entries_.find(task_id);
    if (entry == entries_.end()) throw std::invalid_argument("task runtime does not exist");
    if (!entry->second->enabled) throw std::invalid_argument("task is paused");
    if (!sync::CanScanLocalChanges(entry->second->task)) {
      throw std::invalid_argument("target task cannot scan local changes");
    }
    if (entry->second->scanning) throw std::runtime_error("task scan is already running");
    entry->second->scanning = true;
    generation = entry->second->generation;
  }
  try {
    return PerformScan(task_id, generation);
  } catch (const std::exception& error) {
    RecordFailure(task_id, error.what());
    throw;
  }
}

sync::ReconcileResult TaskRuntimeManager::PerformScan(const std::string& task_id,
                                                      const std::uint64_t generation) {
  storage::TaskDefinition task;
  {
    std::scoped_lock lock(mutex_);
    task = entries_.at(task_id)->task;
  }
  storage::IgnoreRules rules;
  rules.LoadFile(task.root_path);
  const auto snapshot = storage::ManifestScanner(std::move(rules)).Scan(task.root_path);
  const auto current = NowMilliseconds();
  sync::ReconcileResult result;
  {
    std::scoped_lock database_lock(database_.AccessMutex());
    result = sync::SnapshotReconciler(common::NewUuidV4)
                 .Apply(database_, snapshot, {task_id, device_id_, 0, current});
  }
  bool remains_dirty = false;
  {
    std::scoped_lock lock(mutex_);
    const auto entry = entries_.find(task_id);
    if (entry == entries_.end()) throw std::runtime_error("task was removed during scan");
    remains_dirty = entry->second->generation != generation;
    entry->second->dirty = remains_dirty;
    entry->second->scanning = false;
    entry->second->periodic_deadline =
        std::chrono::steady_clock::now() + options_.periodic_verification;
    if (remains_dirty)
      entry->second->dirty_deadline = std::chrono::steady_clock::now() + options_.debounce;
    wake_.notify_all();
  }
  {
    std::scoped_lock database_lock(database_.AccessMutex());
    database_.UpdateTaskRuntime(task_id, "watching", remains_dirty, current);
    database_.RecordEngineEvent(
        {0, task_id, "info",
         "Automatically scanned " + std::to_string(snapshot.size()) + " entries", current});
  }
  ScanCompleted callback;
  {
    std::scoped_lock lock(mutex_);
    callback = scan_completed_;
    wake_.notify_all();
  }
  if (callback) callback(task_id);
  return result;
}

void TaskRuntimeManager::RecordFailure(const std::string& task_id, const std::string& error) {
  std::optional<std::int64_t> last_scan;
  {
    std::scoped_lock lock(mutex_);
    const auto entry = entries_.find(task_id);
    if (entry != entries_.end()) {
      entry->second->scanning = false;
      entry->second->dirty = true;
      entry->second->dirty_deadline = std::chrono::steady_clock::now() + options_.error_retry;
    }
  }
  {
    std::scoped_lock database_lock(database_.AccessMutex());
    try {
      last_scan = database_.RuntimeState(task_id).last_scan_at_ms;
    } catch (...) {
    }
    database_.UpdateTaskRuntime(task_id, "error", true, last_scan, error);
    database_.RecordEngineEvent(
        {0, task_id, "error", "Automatic scan failed: " + error, NowMilliseconds()});
  }
  wake_.notify_all();
}

void TaskRuntimeManager::WorkerLoop() {
  std::unique_lock lock(mutex_);
  while (!stopping_) {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, std::uint64_t>> scans;
    auto next_wake = now + std::chrono::seconds(30);
    for (auto& [task_id, entry] : entries_) {
      if (!entry->enabled || entry->scanning || !sync::CanScanLocalChanges(entry->task)) continue;
      const auto due = entry->dirty ? entry->dirty_deadline : entry->periodic_deadline;
      if (due <= now) {
        entry->scanning = true;
        scans.emplace_back(task_id, entry->generation);
      } else if (due < next_wake) {
        next_wake = due;
      }
    }
    if (scans.empty()) {
      wake_.wait_until(lock, next_wake);
      continue;
    }
    lock.unlock();
    for (const auto& [task_id, generation] : scans) {
      try {
        static_cast<void>(PerformScan(task_id, generation));
      } catch (const std::exception& error) {
        RecordFailure(task_id, error.what());
      }
    }
    lock.lock();
  }
}

void TaskRuntimeManager::SetScanCompletedCallback(ScanCompleted callback) {
  std::scoped_lock lock(mutex_);
  scan_completed_ = std::move(callback);
}

}  // namespace veritassync::runtime
