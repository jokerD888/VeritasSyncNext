#include "engine/storage/database.h"
#include "engine/sync/version_resolution.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>
#include <memory>

namespace {

veritassync::storage::FileRecord Record(std::string version, std::string origin,
                                        std::uint64_t clock,
                                        veritassync::storage::FileKind kind = veritassync::storage::FileKind::kFile) {
  veritassync::storage::FileRecord record{"task-1", "nested/notes.txt", kind, 4, 0,
                                           kind == veritassync::storage::FileKind::kFile ? std::vector<std::uint8_t>{1} : std::vector<std::uint8_t>{},
                                           std::move(version), std::move(origin), clock, std::nullopt};
  return record;
}

class TemporaryVersionDatabase {
 public:
  TemporaryVersionDatabase()
      : path_(std::filesystem::temp_directory_path() /
              ("veritassync-resolution-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db")),
        database_(std::make_unique<veritassync::storage::Database>(path_)) {
    database_->ApplyMigrations();
    database_->CreateTask({"task-1", "bidirectional", "peer", "C:/sync"});
  }
  ~TemporaryVersionDatabase() {
    database_.reset();
    std::filesystem::remove(path_);
    std::filesystem::remove(path_.string() + "-shm");
    std::filesystem::remove(path_.string() + "-wal");
  }
  veritassync::storage::Database& Database() { return *database_; }

 private:
  std::filesystem::path path_;
  std::unique_ptr<veritassync::storage::Database> database_;
};

}  // namespace

VSYNC_TEST(VersionResolverAppliesKnownSuccessorAndRetainsKnownDescendant) {
  using namespace veritassync;
  TemporaryVersionDatabase temporary;
  auto& database = temporary.Database();
  database.RecordVersionLineage({"task-1", "v1", std::nullopt});
  database.RecordVersionLineage({"task-1", "v2", std::optional<std::string>{"v1"}});
  const auto remote = sync::VersionedRecord{Record("v2", "device-b", 2), "v1"};
  const auto forward = sync::VersionResolver::Resolve(database, "task-1", Record("v1", "device-a", 1), remote);
  VSYNC_CHECK(forward.action == sync::VersionResolutionAction::kApplyRemote);
  const auto backward = sync::VersionResolver::Resolve(database, "task-1", Record("v2", "device-b", 2),
                                                        {Record("v1", "device-a", 1), std::nullopt});
  VSYNC_CHECK(backward.action == sync::VersionResolutionAction::kKeepLocal);
}

VSYNC_TEST(VersionResolverMakesConcurrentConflictsDeterministicAndDirectorySafe) {
  using namespace veritassync;
  TemporaryVersionDatabase temporary;
  auto& database = temporary.Database();
  database.RecordVersionLineage({"task-1", "left", std::nullopt});
  database.RecordVersionLineage({"task-1", "right", std::nullopt});
  const auto conflict = sync::VersionResolver::Resolve(
      database, "task-1", Record("left", "device-z", 8),
      {Record("right", "device-a", 8), std::nullopt});
  VSYNC_CHECK(conflict.action == sync::VersionResolutionAction::kConflict);
  VSYNC_CHECK(conflict.remote_wins);
  VSYNC_CHECK(conflict.conflict_path == "nested/notes.conflict.device-z.8.txt");

  const auto directory_wins = sync::VersionResolver::Resolve(
      database, "task-1", Record("left", "device-a", 1, storage::FileKind::kFile),
      {Record("right", "device-z", 99, storage::FileKind::kDirectory), std::nullopt});
  VSYNC_CHECK(directory_wins.action == sync::VersionResolutionAction::kConflict);
  VSYNC_CHECK(directory_wins.remote_wins);
}
