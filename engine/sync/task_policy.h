#pragma once
#include "engine/storage/database.h"
namespace veritassync::sync { [[nodiscard]] bool CanScanLocalChanges(const storage::TaskDefinition& task); }
