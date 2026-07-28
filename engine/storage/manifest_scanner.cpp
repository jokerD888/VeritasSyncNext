#include "engine/storage/manifest_scanner.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

namespace veritassync::storage {
namespace {

[[nodiscard]] std::string ToUtf8Path(const std::filesystem::path& path) {
  const auto value = path.generic_u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::int64_t TimestampNanoseconds(const std::filesystem::file_time_type time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

}  // namespace

ManifestScanner::ManifestScanner(IgnoreRules rules) : rules_(std::move(rules)) {}

std::vector<SnapshotEntry> ManifestScanner::Scan(const std::filesystem::path& task_root) const {
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(task_root, error);
  if (error || !std::filesystem::is_directory(root)) {
    throw std::invalid_argument("scan root must be an existing directory");
  }
  std::vector<SnapshotEntry> snapshot;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  if (error) {
    throw std::runtime_error("cannot enumerate task root");
  }
  const std::filesystem::recursive_directory_iterator end;
  while (iterator != end) {
    const auto entry = *iterator;
    const auto link_status = entry.symlink_status(error);
    if (error) {
      throw std::runtime_error("cannot inspect scanned path");
    }
    if (std::filesystem::is_symlink(link_status)) {
      if (entry.is_directory(error)) {
        iterator.disable_recursion_pending();
      }
      if (error) {
        throw std::runtime_error("cannot inspect symlinked path");
      }
      ++iterator;
      continue;
    }
    const auto relative = ToUtf8Path(entry.path().lexically_relative(root));
    const bool ignored = rules_.IsIgnored(relative);
    if (std::filesystem::is_directory(link_status)) {
      if (!ignored) {
        snapshot.push_back({relative, SnapshotKind::kDirectory, 0,
                            TimestampNanoseconds(entry.last_write_time(error)), std::nullopt});
        if (error) {
          throw std::runtime_error("cannot read directory timestamp");
        }
      }
    } else if (std::filesystem::is_regular_file(link_status) && !ignored) {
      const auto size = entry.file_size(error);
      if (error) {
        throw std::runtime_error("cannot read file size");
      }
      const auto modified = entry.last_write_time(error);
      if (error) {
        throw std::runtime_error("cannot read file timestamp");
      }
      snapshot.push_back({relative, SnapshotKind::kFile, size, TimestampNanoseconds(modified),
                          common::Blake3File(entry.path())});
    }
    ++iterator;
  }
  std::ranges::sort(snapshot, {}, &SnapshotEntry::relative_path);
  return snapshot;
}

}  // namespace veritassync::storage
