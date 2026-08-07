#include "engine/ipc/ipc_service.h"
#include "engine/storage/database.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {
class TemporaryIpcDatabase {
 public:
  TemporaryIpcDatabase() : path_(std::filesystem::temp_directory_path() / ("veritassync-ipc-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db")), database_(std::make_unique<veritassync::storage::Database>(path_)) { database_->ApplyMigrations(); }
  ~TemporaryIpcDatabase() { database_.reset(); std::filesystem::remove(path_); std::filesystem::remove(path_.string() + "-shm"); std::filesystem::remove(path_.string() + "-wal"); }
  veritassync::storage::Database& Database() { return *database_; }
 private:
  std::filesystem::path path_; std::unique_ptr<veritassync::storage::Database> database_;
};

std::string ResponseField(const std::string& response, const std::size_t index) {
  std::size_t begin = 0;
  for (std::size_t field = 0; field < index; ++field) {
    begin = response.find('\t', begin);
    if (begin == std::string::npos) return {};
    ++begin;
  }
  const auto end = response.find_first_of("\t\n", begin);
  return response.substr(begin, end == std::string::npos ? response.size() - begin : end - begin);
}
}

VSYNC_TEST(IpcServiceCreatesListsDeletesTasksAndRejectsBadVersions) {
  TemporaryIpcDatabase temporary;
  veritassync::ipc::IpcService service(temporary.Database());
  VSYNC_CHECK(service.Handle("VSYNC_IPC/1\tping") == "OK\tpong\n");
  VSYNC_CHECK(service.Handle("VSYNC_IPC/0\tping").starts_with("ERR\tunsupported IPC protocol"));
  VSYNC_CHECK(service.Handle("VSYNC_IPC/1\tcreate_task\tdemo\tone_way\tsource\tC:/sync") == "OK\n");
  const auto tasks = service.Handle("VSYNC_IPC/1\tlist_tasks");
  VSYNC_CHECK(tasks.find("ROW\tdemo\tone_way\tsource\tC:/sync") != std::string::npos);
  const auto dashboard = service.Handle("VSYNC_IPC/1\tdashboard\t10");
  VSYNC_CHECK(dashboard.starts_with("OK\t5\t1\n"));
  VSYNC_CHECK(dashboard.find("TASK\tdemo\tone_way\tsource\tC:/sync") != std::string::npos);
  VSYNC_CHECK(dashboard.find("EVENT\t") != std::string::npos);
  VSYNC_CHECK(service.Handle("VSYNC_IPC/1\tlist_conflicts") == "END\n");
  VSYNC_CHECK(service.Handle("VSYNC_IPC/1\tdelete_task\tdemo") == "OK\n");
  const auto events = service.Handle("VSYNC_IPC/1\tlist_events\t10");
  VSYNC_CHECK(events.find("Deleted task demo") != std::string::npos);
}

VSYNC_TEST(IpcServicePreviewsAndAppliesVersionedIgnorePoliciesForSourcesOnly) {
  TemporaryIpcDatabase temporary;
  const auto root = std::filesystem::temp_directory_path() /
      ("veritassync-ipc-ignore-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  { std::ofstream stream(root / "debug.log"); stream << "log"; }
  { std::ofstream stream(root / "keep.txt"); stream << "keep"; }
  temporary.Database().CreateTask({"source", "one_way", "source", root.string()});
  temporary.Database().UpsertFileRecord({"source", "debug.log", veritassync::storage::FileKind::kFile,
      3, 1, {1}, "v1", "device-a", 0, std::nullopt});
  veritassync::ipc::IpcService service(temporary.Database());

  const auto current = service.Handle("VSYNC_IPC/1\tignore_get\tsource");
  VSYNC_CHECK(current.starts_with("OK\t1\t"));
  const auto hash = ResponseField(current, 2);
  VSYNC_CHECK(hash.size() == 64);
  const auto preview = service.Handle("VSYNC_IPC/1\tignore_preview\tsource\t*.log%0A");
  VSYNC_CHECK(preview.starts_with("OK\t" + hash));
  VSYNC_CHECK(preview.find("DELETE\tdebug.log") != std::string::npos);
  const auto applied = service.Handle(
      "VSYNC_IPC/1\tignore_apply\tsource\t" + hash + "\t*.log%0A\tai");
  VSYNC_CHECK(applied.starts_with("OK\t2\t"));
  VSYNC_CHECK(std::filesystem::is_regular_file(root / ".veritasignore"));
  VSYNC_CHECK(service.Handle(
      "VSYNC_IPC/1\tignore_apply\tsource\t" + hash + "\tbuild/%0A\tai")
      .starts_with("ERR\tignore policy changed"));

  temporary.Database().CreateTask({"target", "one_way", "target", root.string()});
  const auto target = service.Handle("VSYNC_IPC/1\tignore_get\ttarget");
  const auto target_hash = ResponseField(target, 2);
  VSYNC_CHECK(service.Handle("VSYNC_IPC/1\tignore_apply\ttarget\t" + target_hash +
      "\t*.tmp%0A\tmanual").starts_with("ERR\tone-way target ignore policy is read-only"));
  temporary.Database().CreateTask({"peer", "bidirectional", "peer", root.string()});
  const auto peer = service.Handle("VSYNC_IPC/1\tignore_get\tpeer");
  const auto peer_hash = ResponseField(peer, 2);
  VSYNC_CHECK(service.Handle("VSYNC_IPC/1\tignore_apply\tpeer\t" + peer_hash +
      "\t*.tmp%0A\tmanual").starts_with("ERR\tbidirectional ignore policy requires peer negotiation"));
  std::filesystem::remove_all(root);
}
