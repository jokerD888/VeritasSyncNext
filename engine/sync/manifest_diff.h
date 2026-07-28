#pragma once

#include "engine/storage/database.h"
#include "engine/storage/manifest_scanner.h"

#include <string>
#include <vector>

namespace veritassync::sync {

struct ManifestDiff {
  std::vector<storage::SnapshotEntry> created_or_changed;
  std::vector<std::string> deleted_paths;
};

// Compares an on-disk scan with durable records. Deleted paths are limited to live
// file/directory records; existing tombstones are never tombstoned again.
[[nodiscard]] ManifestDiff DiffManifest(const std::vector<storage::SnapshotEntry>& snapshot,
                                        const std::vector<storage::FileRecord>& known_records);

}  // namespace veritassync::sync
