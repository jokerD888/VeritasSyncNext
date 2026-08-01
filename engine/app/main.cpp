#include "engine/storage/database.h"
#include "engine/storage/ignore_rules.h"
#include "engine/storage/manifest_scanner.h"
#include "engine/common/uuid.h"
#include "engine/ipc/ipc_service.h"
#include "engine/ipc/named_pipe_server.h"
#include "engine/sync/snapshot_reconciler.h"
#include "engine/sync/task_policy.h"
#include "engine/sync/one_way_sync.h"
#include "engine/transport/mock_transport.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Usage() {
  std::cout << "Usage: veritassync-engine --headless --db <path> [--init-task <id> --mode <one_way|bidirectional> --role <source|target|peer> --root <path>] [--scan-task <id> --device-id <id>] [--list-conflicts <task-id>] [--resolve-conflict <conflict-id>]\n"
               "       veritassync-engine --ipc-serve --db <path> --pipe <\\\\.\\pipe\\name>\n"
               "       veritassync-engine --headless --mock-one-way --mock-source-root <path> --mock-target-root <path> --mock-source-db <path> --mock-target-db <path> [--mock-task-id <id>]\n";
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

void EnsureTask(veritassync::storage::Database& database,
                const veritassync::storage::TaskDefinition& expected) {
  const auto existing = database.FindTask(expected.task_id);
  if (!existing.has_value()) {
    database.CreateTask(expected);
    return;
  }
  if (existing->mode != expected.mode || existing->role != expected.role ||
      std::filesystem::path(existing->root_path) != std::filesystem::path(expected.root_path)) {
    throw std::invalid_argument("existing mock task does not match requested role or root");
  }
}

void RunMockOneWay(const std::string& task_id, const std::filesystem::path& source_root,
                   const std::filesystem::path& target_root,
                   const std::filesystem::path& source_database_path,
                   const std::filesystem::path& target_database_path) {
  if (task_id.empty()) throw std::invalid_argument("mock task id is required");
  if (!std::filesystem::is_directory(source_root)) throw std::invalid_argument("mock source root must exist");
  std::filesystem::create_directories(target_root);
  RequireDatabaseOutsideTaskRoot(source_database_path, source_root);
  RequireDatabaseOutsideTaskRoot(target_database_path, target_root);
  veritassync::storage::Database source_database(source_database_path);
  veritassync::storage::Database target_database(target_database_path);
  source_database.ApplyMigrations();
  target_database.ApplyMigrations();
  EnsureTask(source_database, {task_id, "one_way", "source", source_root.string()});
  EnsureTask(target_database, {task_id, "one_way", "target", target_root.string()});
  veritassync::transport::MockNetwork network;
  auto endpoints = network.CreatePair();
  veritassync::sync::OneWaySyncNode source(
      {task_id, veritassync::protocol::Role::kSource, "mock-source", "mock-target",
       "mock-source-fingerprint", "mock-shared-auth", source_root, source_database},
      *endpoints.first);
  veritassync::sync::OneWaySyncNode target(
      {task_id, veritassync::protocol::Role::kTarget, "mock-target", "mock-source",
       "mock-target-fingerprint", "mock-shared-auth", target_root, target_database},
      *endpoints.second);
  source.Start();
  target.Start();
  network.PumpUntilIdle();
  constexpr std::size_t kMaximumPumpIterations = 2U * 1024U * 1024U;
  for (std::size_t iteration = 0; iteration < kMaximumPumpIterations && !target.TargetIsConverged(); ++iteration) {
    source.Pump();
    network.PumpUntilIdle();
  }
  if (!target.TargetIsConverged()) {
    throw std::runtime_error("mock one-way sync did not converge" +
                             (target.LastError().has_value() ? ": " + *target.LastError() : ""));
  }
  const auto source_statistics = source.Statistics();
  const auto target_statistics = target.Statistics();
  std::cout << "Mock one-way sync converged: source chunks=" << source_statistics.chunks_sent
            << ", target chunks=" << target_statistics.chunks_received
            << ", committed files=" << target_statistics.files_committed
            << ", deleted files=" << target_statistics.files_deleted << "\n";
}
}
int main(int argc, char** argv) {
  try {
    bool headless = false, ipc_serve = false;
    std::string db_path, pipe_name;
    std::string init_task_id, scan_task_id, list_conflicts_task_id, resolve_conflict_id, device_id, mode, role, root;
    bool mock_one_way = false;
    std::string mock_source_root, mock_target_root, mock_source_db, mock_target_db, mock_task_id = "phase2-mock";
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--help") { Usage(); return 0; }
      if (argument == "--headless") { headless = true; continue; }
      if (argument == "--ipc-serve") { ipc_serve = true; continue; }
      if (argument == "--mock-one-way") { mock_one_way = true; continue; }
      if (i + 1 >= argc) throw std::invalid_argument("missing value for " + argument);
      const std::string value = argv[++i];
      if (argument == "--db") db_path = value;
      else if (argument == "--pipe") pipe_name = value;
      else if (argument == "--init-task") init_task_id = value;
      else if (argument == "--scan-task") scan_task_id = value;
      else if (argument == "--list-conflicts") list_conflicts_task_id = value;
      else if (argument == "--resolve-conflict") resolve_conflict_id = value;
      else if (argument == "--device-id") device_id = value;
      else if (argument == "--mode") mode = value;
      else if (argument == "--role") role = value;
      else if (argument == "--root") root = value;
      else if (argument == "--mock-source-root") mock_source_root = value;
      else if (argument == "--mock-target-root") mock_target_root = value;
      else if (argument == "--mock-source-db") mock_source_db = value;
      else if (argument == "--mock-target-db") mock_target_db = value;
      else if (argument == "--mock-task-id") mock_task_id = value;
      else throw std::invalid_argument("unknown option: " + argument);
    }
    if (ipc_serve) {
      if (db_path.empty() || pipe_name.empty()) throw std::invalid_argument("--ipc-serve requires --db and --pipe");
      veritassync::storage::Database database(db_path);
      database.ApplyMigrations();
      database.RecordEngineEvent({0, std::nullopt, "info", "IPC server started", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()});
      veritassync::ipc::IpcService service(database);
      return veritassync::ipc::RunNamedPipeServer(service, pipe_name);
    }
    if (!headless) { Usage(); return 2; }
    if (mock_one_way) {
      if (mock_source_root.empty() || mock_target_root.empty() || mock_source_db.empty() || mock_target_db.empty()) {
        throw std::invalid_argument("--mock-one-way requires source/target roots and database paths");
      }
      RunMockOneWay(mock_task_id, mock_source_root, mock_target_root, mock_source_db, mock_target_db);
      return 0;
    }
    if (db_path.empty()) { Usage(); return 2; }
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
    }
    if (!list_conflicts_task_id.empty()) {
      for (const auto& conflict : database.ListConflicts(list_conflicts_task_id)) {
        std::cout << conflict.conflict_id << "\t" << conflict.state << "\t" << conflict.original_path
                  << "\t" << conflict.conflict_path << "\t" << conflict.winning_version_id << "\n";
      }
    }
    if (!resolve_conflict_id.empty()) {
      database.UpdateConflictState(resolve_conflict_id, "resolved");
      std::cout << "Resolved conflict " << resolve_conflict_id << "\n";
    }
    if (init_task_id.empty() && scan_task_id.empty() && list_conflicts_task_id.empty() && resolve_conflict_id.empty()) {
      std::cout << "VeritasSyncNext headless engine ready; schema v" << database.SchemaVersion() << "\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
