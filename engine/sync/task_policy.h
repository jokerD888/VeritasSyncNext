#pragma once
#include "engine/storage/database.h"
namespace veritassync::sync {

// The filesystem watcher must use this gate before turning a local change into
// a scan or outbound manifest revision.  A one-way target only applies source
// manifests and is never allowed to advertise its own local modification.
[[nodiscard]] bool CanApplyLocalWatcherChange(const storage::TaskDefinition& task);
[[nodiscard]] bool CanScanLocalChanges(const storage::TaskDefinition& task);

}  // namespace veritassync::sync
