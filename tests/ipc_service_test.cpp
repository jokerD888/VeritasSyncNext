#include "engine/ipc/ipc_service.h"
#include "engine/storage/database.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>
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
}

VSYNC_TEST(IpcServiceCreatesListsDeletesTasksAndRejectsBadVersions) {
  TemporaryIpcDatabase temporary;
  veritassync::ipc::IpcService service(temporary.Database());
  VSYNC_CHECK(service.Handle("VSYNC_IPC/1\tping") == "OK\tpong\n");
  VSYNC_CHECK(service.Handle("VSYNC_IPC/0\tping").starts_with("ERR\tunsupported IPC protocol"));
  VSYNC_CHECK(service.Handle("VSYNC_IPC/1\tcreate_task\tdemo\tone_way\tsource\tC:/sync") == "OK\n");
  const auto tasks = service.Handle("VSYNC_IPC/1\tlist_tasks");
  VSYNC_CHECK(tasks.find("ROW\tdemo\tone_way\tsource\tC:/sync") != std::string::npos);
  VSYNC_CHECK(service.Handle("VSYNC_IPC/1\tdelete_task\tdemo") == "OK\n");
  const auto events = service.Handle("VSYNC_IPC/1\tlist_events\t10");
  VSYNC_CHECK(events.find("Deleted task demo") != std::string::npos);
}
