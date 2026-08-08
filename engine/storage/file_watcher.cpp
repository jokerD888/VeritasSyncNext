#include "engine/storage/file_watcher.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace veritassync::storage {

FileWatcher::FileWatcher(std::filesystem::path root, ChangeCallback callback)
    : root_(std::move(root)), callback_(std::move(callback)) {
  if (root_.empty() || !callback_)
    throw std::invalid_argument("watcher root and callback are required");
}

FileWatcher::~FileWatcher() { Stop(); }

void FileWatcher::Start() {
  if (running_.load()) return;
  if (!std::filesystem::is_directory(root_))
    throw std::invalid_argument("watcher root must be a directory");
  const HANDLE directory = CreateFileW(
      root_.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (directory == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open watcher root");
  {
    std::scoped_lock lock(handle_mutex_);
    directory_ = directory;
  }
  stopping_.store(false);
  running_.store(true);
  try {
    thread_ = std::thread([this] { Watch(); });
  } catch (...) {
    running_.store(false);
    std::scoped_lock lock(handle_mutex_);
    CloseHandle(directory_);
    directory_ = INVALID_HANDLE_VALUE;
    throw;
  }
}

void FileWatcher::Stop() {
  stopping_.store(true);
  {
    std::scoped_lock lock(handle_mutex_);
    if (directory_ != INVALID_HANDLE_VALUE) CancelIoEx(directory_, nullptr);
  }
  if (thread_.joinable()) thread_.join();
  {
    std::scoped_lock lock(handle_mutex_);
    if (directory_ != INVALID_HANDLE_VALUE) {
      CloseHandle(directory_);
      directory_ = INVALID_HANDLE_VALUE;
    }
  }
  running_.store(false);
}

void FileWatcher::Watch() {
  alignas(DWORD) std::array<std::byte, 64U * 1024U> buffer{};
  constexpr DWORD filters = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                            FILE_NOTIFY_CHANGE_CREATION;
  const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) {
    running_.store(false);
    return;
  }
  while (!stopping_.load()) {
    DWORD bytes = 0;
    HANDLE directory;
    {
      std::scoped_lock lock(handle_mutex_);
      directory = directory_;
    }
    ResetEvent(event);
    OVERLAPPED operation{};
    operation.hEvent = event;
    const BOOL started =
        ReadDirectoryChangesW(directory, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
                              filters, nullptr, &operation, nullptr);
    if (started == FALSE && GetLastError() != ERROR_IO_PENDING) {
      if (stopping_.load()) break;
      try {
        callback_();
      } catch (...) {
      }
      continue;
    }
    while (!stopping_.load() && WaitForSingleObject(event, 250) == WAIT_TIMEOUT) {
    }
    if (stopping_.load()) {
      CancelIoEx(directory, &operation);
      WaitForSingleObject(event, 1000);
      break;
    }
    if (GetOverlappedResult(directory, &operation, &bytes, FALSE) == FALSE) {
      const auto error = GetLastError();
      if (error == ERROR_OPERATION_ABORTED || error == ERROR_INVALID_HANDLE) break;
      try {
        callback_();
      } catch (...) {
      }
      continue;
    }
    if (bytes > 0) {
      try {
        callback_();
      } catch (...) {
      }
    }
  }
  CloseHandle(event);
  running_.store(false);
}

}  // namespace veritassync::storage
