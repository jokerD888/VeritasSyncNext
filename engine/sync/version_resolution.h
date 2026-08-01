#pragma once

#include "engine/storage/database.h"

#include <optional>
#include <string>

namespace veritassync::sync {

struct VersionedRecord {
  storage::FileRecord record;
  std::optional<std::string> parent_version_id;
};

enum class VersionResolutionAction { kApplyRemote, kKeepLocal, kConflict };

struct VersionResolution {
  VersionResolutionAction action;
  // For a concurrent conflict, true means the remote version is retained at the
  // formal path. The other version is preserved under conflict_path when it has
  // filesystem content (file or directory).
  bool remote_wins = false;
  std::string conflict_path;
};

// Decides a single logical path without looking at wall-clock time. The caller
// records received lineage before resolving so the graph can distinguish a
// successor from a concurrent branch.
class VersionResolver {
 public:
  [[nodiscard]] static VersionResolution Resolve(const storage::Database& database,
                                                 const std::string& task_id,
                                                 const std::optional<storage::FileRecord>& local,
                                                 const VersionedRecord& remote);
  [[nodiscard]] static std::string ConflictPath(const std::string& original_path,
                                                const storage::FileRecord& losing_record);
};

}  // namespace veritassync::sync
