#include "engine/sync/task_policy.h"
#include "tests/test_framework.h"
VSYNC_TEST(TaskPolicyForbidsOneWayTargetLocalScanning) {
  VSYNC_CHECK(veritassync::sync::CanScanLocalChanges({"a","one_way","source","C:/"}));
  VSYNC_CHECK(!veritassync::sync::CanScanLocalChanges({"a","one_way","target","C:/"}));
  VSYNC_CHECK(veritassync::sync::CanScanLocalChanges({"a","bidirectional","peer","C:/"}));
}
