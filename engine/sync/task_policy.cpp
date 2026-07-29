#include "engine/sync/task_policy.h"
namespace veritassync::sync { bool CanScanLocalChanges(const storage::TaskDefinition& task) { return task.mode == "bidirectional" || task.role == "source"; } }
