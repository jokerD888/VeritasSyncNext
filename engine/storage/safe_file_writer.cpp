#include "engine/storage/safe_file_writer.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace veritassync::storage {
namespace {

[[nodiscard]] bool IsDescendant(const std::filesystem::path& root,
                                const std::filesystem::path& candidate) {
  const auto relative = candidate.lexically_relative(root);
  if (relative.empty()) {
    return candidate == root;
  }
  for (const auto& component : relative) {
    if (component == "..") {
      return false;
    }
  }
  return !relative.is_absolute();
}

void EnsureSafeDirectory(const std::filesystem::path& directory) {
  std::filesystem::path current = directory.root_path();
  for (const auto& component : directory.relative_path()) {
    current /= component;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(current, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      throw std::runtime_error("cannot inspect destination directory");
    }
    if (!std::filesystem::exists(status)) {
      std::filesystem::create_directory(current, error);
      if (error) {
        throw std::runtime_error("cannot create destination directory");
      }
      continue;
    }
    if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
      throw std::invalid_argument("destination path traverses a non-directory or symlink");
    }
  }
}

[[nodiscard]] std::filesystem::path MakePartPath(const std::filesystem::path& destination) {
  static std::atomic_uint64_t serial{0};
  const auto suffix = std::to_wstring(::GetCurrentProcessId()) + L"." +
                      std::to_wstring(serial.fetch_add(1, std::memory_order_relaxed));
  return destination.parent_path() /
         (destination.filename().wstring() + L".veritassync." + suffix + L".part");
}

[[nodiscard]] std::filesystem::path ResumablePartPath(const std::filesystem::path& destination) {
  return destination.parent_path() / (destination.filename().wstring() + L".part");
}

void FlushPartFile(const std::filesystem::path& part) {
  const HANDLE handle = ::CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("cannot reopen partial download for flush");
  }
  const BOOL flushed = ::FlushFileBuffers(handle);
  ::CloseHandle(handle);
  if (!flushed) {
    throw std::runtime_error("cannot flush partial download");
  }
}

void RejectSymlink(const std::filesystem::path& path, const char* const message) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("cannot inspect download path");
  }
  if (std::filesystem::is_symlink(status)) {
    throw std::invalid_argument(message);
  }
}

void CommitPart(const std::filesystem::path& part, const std::filesystem::path& destination) {
  if (!::MoveFileExW(part.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::runtime_error("cannot atomically commit download");
  }
}

}  // namespace

std::filesystem::path ResolveTaskPath(const std::filesystem::path& task_root,
                                      const std::string_view relative_path) {
  if (task_root.empty() || relative_path.empty()) {
    throw std::invalid_argument("task root and relative path are required");
  }
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(task_root, error);
  if (error || !std::filesystem::is_directory(root)) {
    throw std::invalid_argument("task root must be an existing directory");
  }
  const std::filesystem::path relative{relative_path};
  if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
    throw std::invalid_argument("absolute paths are not allowed");
  }
  for (const auto& component : relative) {
    if (component.empty() || component == "." || component == "..") {
      throw std::invalid_argument("path traversal is not allowed");
    }
  }
  const auto candidate = (root / relative).lexically_normal();
  if (!IsDescendant(root, candidate) || candidate == root) {
    throw std::invalid_argument("path resolves outside task root");
  }
  return candidate;
}

SafeFileWriter::SafeFileWriter(std::filesystem::path task_root)
    : task_root_(std::move(task_root)) {
  // Validate at construction so an invalid task cannot begin a download.
  (void)ResolveTaskPath(task_root_, ".veritassync-validation");
}

void SafeFileWriter::WriteAtomically(const std::string_view relative_path,
                                     const std::span<const std::uint8_t> bytes) const {
  const auto destination = ResolveTaskPath(task_root_, relative_path);
  EnsureSafeDirectory(destination.parent_path());
  RejectSymlink(destination, "destination file must not be a symlink");

  const auto part = MakePartPath(destination);
  std::error_code error;
  try {
    std::ofstream stream(part, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("cannot create partial download");
    }
    if (!bytes.empty()) {
      stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    stream.flush();
    if (!stream) {
      throw std::runtime_error("cannot write partial download");
    }
    stream.close();
    FlushPartFile(part);
    CommitPart(part, destination);
  } catch (...) {
    std::filesystem::remove(part, error);
    throw;
  }
}

void SafeFileWriter::WritePartialChunk(const std::string_view relative_path, const std::uint64_t offset,
                                       const std::span<const std::uint8_t> bytes, const bool flush) const {
  if (offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
    throw std::invalid_argument("chunk offset is too large");
  }
  if (bytes.size() > (std::numeric_limits<std::uint64_t>::max)() - offset) {
    throw std::invalid_argument("chunk range overflows");
  }
  const auto destination = ResolveTaskPath(task_root_, relative_path);
  EnsureSafeDirectory(destination.parent_path());
  RejectSymlink(destination, "destination file must not be a symlink");
  const auto part = ResumablePartPath(destination);
  RejectSymlink(part, "partial file must not be a symlink");
  if (!std::filesystem::exists(part)) {
    std::ofstream create(part, std::ios::binary);
    if (!create) throw std::runtime_error("cannot create partial download");
  }
  std::fstream stream(part, std::ios::binary | std::ios::in | std::ios::out);
  if (!stream) throw std::runtime_error("cannot open partial download");
  stream.seekp(static_cast<std::streamoff>(offset));
  if (!bytes.empty()) stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  stream.flush();
  if (!stream) throw std::runtime_error("cannot write partial download");
  stream.close();
  if (flush) FlushPartFile(part);
}

void SafeFileWriter::FlushPartial(const std::string_view relative_path) const {
  const auto destination = ResolveTaskPath(task_root_, relative_path);
  const auto part = ResumablePartPath(destination);
  RejectSymlink(part, "partial file must not be a symlink");
  std::error_code error;
  if (!std::filesystem::is_regular_file(part, error) || error) {
    throw std::runtime_error("partial download does not exist for flush");
  }
  FlushPartFile(part);
}

void SafeFileWriter::DiscardPartial(const std::string_view relative_path) const {
  const auto destination = ResolveTaskPath(task_root_, relative_path);
  const auto part = ResumablePartPath(destination);
  RejectSymlink(part, "partial file must not be a symlink");
  std::error_code error;
  const auto status = std::filesystem::status(part, error);
  if (error == std::errc::no_such_file_or_directory) return;
  if (error || !std::filesystem::is_regular_file(status)) {
    throw std::runtime_error("cannot discard unsafe partial download");
  }
  if (!std::filesystem::remove(part, error) || error) {
    throw std::runtime_error("cannot discard partial download");
  }
}

void SafeFileWriter::CommitPartial(const std::string_view relative_path, const std::uint64_t expected_size,
                                   const common::ContentHash& expected_hash) const {
  const auto destination = ResolveTaskPath(task_root_, relative_path);
  const auto part = ResumablePartPath(destination);
  RejectSymlink(destination, "destination file must not be a symlink");
  RejectSymlink(part, "partial file must not be a symlink");
  std::error_code error;
  if (!std::filesystem::is_regular_file(part, error) || error) {
    throw std::runtime_error("partial download does not exist");
  }
  if (std::filesystem::file_size(part, error) != expected_size || error) {
    throw std::invalid_argument("partial download size does not match");
  }
  if (common::Blake3File(part) != expected_hash) {
    throw std::invalid_argument("partial download hash does not match");
  }
  FlushPartFile(part);
  CommitPart(part, destination);
}

void SafeFileWriter::RemoveFile(const std::string_view relative_path) const {
  const auto destination = ResolveTaskPath(task_root_, relative_path);
  RejectSymlink(destination, "destination file must not be a symlink");
  std::error_code error;
  const auto status = std::filesystem::status(destination, error);
  if (error == std::errc::no_such_file_or_directory) return;
  if (error) throw std::runtime_error("cannot inspect destination file for deletion");
  if (!std::filesystem::is_regular_file(status)) {
    throw std::invalid_argument("destination deletion requires a regular file");
  }
  if (!std::filesystem::remove(destination, error) || error) {
    throw std::runtime_error("cannot delete synchronized file");
  }
}

void SafeFileWriter::MoveFileToConflict(const std::string_view relative_path,
                                        const std::string_view conflict_relative_path) const {
  const auto source = ResolveTaskPath(task_root_, relative_path);
  const auto destination = ResolveTaskPath(task_root_, conflict_relative_path);
  if (source == destination) throw std::invalid_argument("conflict destination must differ from source");
  RejectSymlink(source, "conflict source file must not be a symlink");
  RejectSymlink(destination, "conflict destination file must not be a symlink");
  std::error_code error;
  if (!std::filesystem::is_regular_file(source, error) || error) {
    throw std::invalid_argument("conflict source must be a regular file");
  }
  if (std::filesystem::exists(destination, error) || error) {
    throw std::invalid_argument("conflict destination already exists");
  }
  EnsureSafeDirectory(destination.parent_path());
  if (!::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
    throw std::runtime_error("cannot preserve conflict file");
  }
}

void SafeFileWriter::EnsureDirectory(const std::string_view relative_path) const {
  const auto directory = ResolveTaskPath(task_root_, relative_path);
  std::error_code error;
  const auto status = std::filesystem::symlink_status(directory, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("cannot inspect synchronized directory");
  }
  if (std::filesystem::exists(status)) {
    if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
      throw std::invalid_argument("synchronized directory path is unsafe");
    }
    return;
  }
  EnsureSafeDirectory(directory);
}

void SafeFileWriter::RemoveEmptyDirectory(const std::string_view relative_path) const {
  const auto directory = ResolveTaskPath(task_root_, relative_path);
  RejectSymlink(directory, "synchronized directory must not be a symlink");
  std::error_code error;
  const auto status = std::filesystem::status(directory, error);
  if (error == std::errc::no_such_file_or_directory) return;
  if (error) throw std::runtime_error("cannot inspect synchronized directory for deletion");
  if (!std::filesystem::is_directory(status)) {
    throw std::invalid_argument("synchronized directory deletion requires a directory");
  }
  if (!std::filesystem::remove(directory, error) || error) {
    throw std::runtime_error("cannot delete non-empty synchronized directory");
  }
}

}  // namespace veritassync::storage
