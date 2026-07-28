#include "engine/sync/manifest_diff.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace veritassync::sync {
namespace {

[[nodiscard]] bool IsSameContent(const storage::SnapshotEntry& entry,
                                 const storage::FileRecord& record) {
  if ((entry.kind == storage::SnapshotKind::kFile) != (record.kind == storage::FileKind::kFile)) {
    return false;
  }
  if (entry.kind == storage::SnapshotKind::kDirectory) {
    return record.kind == storage::FileKind::kDirectory;
  }
  if (!entry.content_hash.has_value()) {
    throw std::invalid_argument("file snapshots require a content hash");
  }
  return entry.size == record.size &&
         std::equal(entry.content_hash->begin(), entry.content_hash->end(), record.content_hash.begin(), record.content_hash.end());
}

}  // namespace

ManifestDiff DiffManifest(const std::vector<storage::SnapshotEntry>& snapshot,
                          const std::vector<storage::FileRecord>& known_records) {
  std::map<std::string, const storage::SnapshotEntry*, std::less<>> scanned;
  for (const auto& entry : snapshot) {
    if (!scanned.emplace(entry.relative_path, &entry).second) {
      throw std::invalid_argument("snapshot contains duplicate paths");
    }
  }
  std::map<std::string, const storage::FileRecord*, std::less<>> known;
  for (const auto& record : known_records) {
    if (!known.emplace(record.relative_path, &record).second) {
      throw std::invalid_argument("known records contain duplicate paths");
    }
  }

  ManifestDiff result;
  for (const auto& [path, entry] : scanned) {
    const auto existing = known.find(path);
    if (existing == known.end() || existing->second->kind == storage::FileKind::kTombstone ||
        !IsSameContent(*entry, *existing->second)) {
      result.created_or_changed.push_back(*entry);
    }
  }
  for (const auto& [path, record] : known) {
    if (record->kind != storage::FileKind::kTombstone && !scanned.contains(path)) {
      result.deleted_paths.push_back(path);
    }
  }
  return result;
}

}  // namespace veritassync::sync
