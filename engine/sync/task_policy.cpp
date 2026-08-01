#include "engine/sync/task_policy.h"
namespace veritassync::sync {

bool CanApplyLocalWatcherChange(const storage::TaskDefinition& task) {
  return task.mode == "bidirectional" || task.role == "source";
}

bool CanScanLocalChanges(const storage::TaskDefinition& task) {
  return CanApplyLocalWatcherChange(task);
}

}  // namespace veritassync::sync
