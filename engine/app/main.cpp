#include "engine/storage/database.h"
#include "engine/storage/ignore_rules.h"
#include "engine/storage/manifest_scanner.h"
#include "engine/common/uuid.h"
#include "engine/sync/snapshot_reconciler.h"
#include "engine/sync/task_policy.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Usage() {
  std::cout << "Usage: veritassync-engine --headless --db <path> [--init-task <id> --mode <one_way|bidirectional> --role <source|target|peer> --root <path>] [--scan-task <id> --device-id <id>]\n";
}

void RequireDatabaseOutsideTaskRoot(const std::filesystem::path& database_path,
                                    const std::filesystem::path& task_root) {
  std::error_code error;
  const auto database = std::filesystem::weakly_canonical(database_path, error);
  if (error) throw std::invalid_argument("cannot resolve database path");
  const auto root = std::filesystem::weakly_canonical(task_root, error);
  if (error) throw std::invalid_argument("cannot resolve task root");
  const auto relative = database.lexically_relative(root);
  bool inside = database == root || !relative.empty();
  for (const auto& component : relative) {
    if (component == "..") inside = false;
  }
  if (inside) throw std::invalid_argument("database path must be outside the task root");
}
}
int main(int argc, char** argv) {
  try {
    bool headless = false;
    std::string db_path;
    std::string init_task_id, scan_task_id, device_id, mode, role, root;
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--help") { Usage(); return 0; }
      if (argument == "--headless") { headless = true; continue; }
      if (i + 1 >= argc) throw std::invalid_argument("missing value for " + argument);
      const std::string value = argv[++i];
      if (argument == "--db") db_path = value;
      else if (argument == "--init-task") init_task_id = value;
      else if (argument == "--scan-task") scan_task_id = value;
      else if (argument == "--device-id") device_id = value;
      else if (argument == "--mode") mode = value;
      else if (argument == "--role") role = value;
      else if (argument == "--root") root = value;
      else throw std::invalid_argument("unknown option: " + argument);
    }
    if (!headless || db_path.empty()) { Usage(); return 2; }
    veritassync::storage::Database database(db_path);
    database.ApplyMigrations();
    if (!init_task_id.empty()) {
      if (mode.empty() || role.empty() || root.empty()) throw std::invalid_argument("--init-task requires --mode, --role, and --root");
      database.CreateTask({init_task_id, mode, role, root});
      std::cout << "Created task " << init_task_id << " in schema v" << database.SchemaVersion() << "\n";
    }
    if (!scan_task_id.empty()) {
      if (device_id.empty()) throw std::invalid_argument("--scan-task requires --device-id");
      const auto task = database.FindTask(scan_task_id);
      if (!task.has_value()) throw std::invalid_argument("scan task does not exist");
      if (!veritassync::sync::CanScanLocalChanges(*task)) throw std::invalid_argument("one-way target tasks cannot scan local changes");
      RequireDatabaseOutsideTaskRoot(std::filesystem::path(db_path), std::filesystem::path(task->root_path));
      veritassync::storage::IgnoreRules rules;
      rules.LoadFile(std::filesystem::path(task->root_path));
      veritassync::storage::ManifestScanner scanner(std::move(rules));
      const auto snapshot = scanner.Scan(std::filesystem::path(task->root_path));
      const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      veritassync::sync::SnapshotReconciler reconciler(veritassync::common::NewUuidV4);
      const auto result = reconciler.Apply(database, snapshot, {scan_task_id, device_id, 0, now});
      std::cout << "Scanned " << snapshot.size() << " entries; " << result.created_or_changed
                << " created or changed, " << result.tombstoned << " tombstoned\n";
    } else {
      if (init_task_id.empty()) std::cout << "VeritasSyncNext headless engine ready; schema v" << database.SchemaVersion() << "\n";
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << "error: " << error.what() << "\n"; return 1; }
}
