#include "engine/sync/version_resolution.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace veritassync::sync {
namespace {

[[nodiscard]] bool IsDirectory(const storage::FileRecord& record) {
  return record.kind == storage::FileKind::kDirectory;
}

[[nodiscard]] bool RemoteSortsBeforeLocal(const storage::FileRecord& remote,
                                          const storage::FileRecord& local) {
  if (remote.logical_clock != local.logical_clock) {
    return remote.logical_clock < local.logical_clock;
  }
  return remote.origin_device_id < local.origin_device_id;
}

[[nodiscard]] std::string SafeDeviceId(const std::string& value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    if (std::isalnum(character) || character == '-' || character == '_') {
      result.push_back(static_cast<char>(character));
    } else {
      result.push_back('_');
    }
  }
  return result.empty() ? "unknown" : result;
}

}  // namespace

VersionResolution VersionResolver::Resolve(const storage::Database& database,
                                           const std::string& task_id,
                                           const std::optional<storage::FileRecord>& local,
                                           const VersionedRecord& remote) {
  if (task_id.empty() || remote.record.task_id != task_id ||
      remote.record.relative_path.empty() || remote.record.version_id.empty()) {
    throw std::invalid_argument("version resolution input is invalid");
  }
  if (!local.has_value()) return {VersionResolutionAction::kApplyRemote, true, {}};
  if (local->task_id != task_id || local->relative_path != remote.record.relative_path) {
    throw std::invalid_argument("version records do not identify the same path");
  }
  if (local->version_id == remote.record.version_id) {
    return {VersionResolutionAction::kKeepLocal, false, {}};
  }
  if (database.IsVersionAncestor(task_id, local->version_id, remote.record.version_id)) {
    return {VersionResolutionAction::kApplyRemote, true, {}};
  }
  if (database.IsVersionAncestor(task_id, remote.record.version_id, local->version_id)) {
    return {VersionResolutionAction::kKeepLocal, false, {}};
  }

  // File-vs-directory conflicts always retain the directory at the formal path.
  // That avoids a recursive deletion merely to replace a directory with a file.
  const bool remote_wins = IsDirectory(remote.record) != IsDirectory(*local)
                               ? IsDirectory(remote.record)
                               : RemoteSortsBeforeLocal(remote.record, *local);
  const auto& losing = remote_wins ? *local : remote.record;
  return {VersionResolutionAction::kConflict, remote_wins,
          ConflictPath(remote.record.relative_path, losing)};
}

std::string VersionResolver::ConflictPath(const std::string& original_path,
                                          const storage::FileRecord& losing_record) {
  if (original_path.empty() || losing_record.origin_device_id.empty()) {
    throw std::invalid_argument("conflict path identity is invalid");
  }
  const std::filesystem::path original(original_path);
  const auto filename = original.filename();
  if (filename.empty() || filename == "." || filename == "..") {
    throw std::invalid_argument("conflict path is invalid");
  }
  const auto stem = filename.stem().string();
  const auto extension = filename.extension().string();
  const auto conflict_name = stem + ".conflict." + SafeDeviceId(losing_record.origin_device_id) +
                             "." + std::to_string(losing_record.logical_clock) + extension;
  const auto result = original.parent_path() / conflict_name;
  return result.generic_string();
}

}  // namespace veritassync::sync
