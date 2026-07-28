#include "engine/sync/snapshot_reconciler.h"

#include "engine/sync/manifest_diff.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace veritassync::sync {
namespace {

[[nodiscard]] storage::FileRecord MakeRecord(const storage::SnapshotEntry& entry,
                                              const ReconcileConfig& config,
                                              std::string version_id) {
  storage::FileRecord record;
  record.task_id = config.task_id;
  record.relative_path = entry.relative_path;
  record.kind = entry.kind == storage::SnapshotKind::kFile ? storage::FileKind::kFile : storage::FileKind::kDirectory;
  record.size = entry.size;
  record.mtime_ns = entry.mtime_ns;
  if (entry.content_hash.has_value()) {
    record.content_hash.assign(entry.content_hash->begin(), entry.content_hash->end());
  }
  record.version_id = std::move(version_id);
  record.origin_device_id = config.origin_device_id;
  record.logical_clock = config.logical_clock;
  return record;
}

[[nodiscard]] bool SamePath(const storage::SnapshotEntry& entry, const std::string& path) {
  return entry.relative_path == path;
}

}  // namespace

SnapshotReconciler::SnapshotReconciler(VersionIdGenerator version_id_generator)
    : version_id_generator_(std::move(version_id_generator)) {
  if (!version_id_generator_) throw std::invalid_argument("version id generator is required");
}

ReconcileResult SnapshotReconciler::Apply(storage::Database& database,
                                          const std::vector<storage::SnapshotEntry>& snapshot,
                                          const ReconcileConfig& config) const {
  if (config.task_id.empty() || config.origin_device_id.empty() || config.now_ms <= 0) {
    throw std::invalid_argument("reconciliation identity and timestamp are required");
  }
  const auto known = database.ListFileRecords(config.task_id);
  const auto diff = DiffManifest(snapshot, known);
  ReconcileResult result{diff.created_or_changed.size(), diff.deleted_paths.size()};
  database.InTransaction([&] {
    for (const auto& entry : snapshot) {
      const auto existing = std::ranges::find_if(known, [&](const storage::FileRecord& record) {
        return SamePath(entry, record.relative_path);
      });
      const bool changed = std::ranges::find_if(diff.created_or_changed, [&](const storage::SnapshotEntry& changed_entry) {
        return SamePath(entry, changed_entry.relative_path);
      }) != diff.created_or_changed.end();
      if (changed || existing == known.end()) {
        database.UpsertFileRecord(MakeRecord(entry, config, version_id_generator_()));
      } else {
        auto refreshed = *existing;
        refreshed.size = entry.size;
        refreshed.mtime_ns = entry.mtime_ns;
        database.UpsertFileRecord(refreshed);
      }
    }
    for (const auto& path : diff.deleted_paths) {
      database.RecordTombstone(config.task_id, path, version_id_generator_(), config.origin_device_id,
                               config.logical_clock, config.now_ms);
    }
  });
  return result;
}

}  // namespace veritassync::sync
