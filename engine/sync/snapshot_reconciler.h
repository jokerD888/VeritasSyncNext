#pragma once

#include "engine/storage/database.h"
#include "engine/storage/manifest_scanner.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace veritassync::sync {

struct ReconcileConfig {
  std::string task_id;
  std::string origin_device_id;
  std::uint64_t logical_clock = 0;
  std::int64_t now_ms = 0;
};

struct ReconcileResult {
  std::size_t created_or_changed = 0;
  std::size_t tombstoned = 0;
};

// Reconciles one completed scan with SQLite in one short write transaction.
class SnapshotReconciler {
 public:
  using VersionIdGenerator = std::function<std::string()>;

  explicit SnapshotReconciler(VersionIdGenerator version_id_generator);
  [[nodiscard]] ReconcileResult Apply(storage::Database& database,
                                      const std::vector<storage::SnapshotEntry>& snapshot,
                                      const ReconcileConfig& config) const;

 private:
  VersionIdGenerator version_id_generator_;
};

}  // namespace veritassync::sync
