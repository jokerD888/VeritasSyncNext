#include "engine/sync/download_receiver.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>

VSYNC_TEST(DownloadReceiverMarksOnlyVerifiedWrittenChunksAndCommits) {
  const auto root = std::filesystem::temp_directory_path() / ("veritassync-download-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  const auto db_path = root.parent_path() / (root.filename().string() + ".db");
  { veritassync::storage::Database db(db_path); db.ApplyMigrations(); db.CreateTask({"task", "one_way", "target", root.string()});
    veritassync::storage::TransferId id{}; id[0] = 1;
    const std::vector<std::uint8_t> content{'a','b','c','d'};
    const auto hash = veritassync::common::Blake3(content);
    db.CreateTransfer({id, "task", "peer", "download", std::vector<std::uint8_t>(hash.begin(), hash.end()), "active", 1, 1});
    veritassync::storage::SafeFileWriter writer(root);
    veritassync::sync::DownloadReceiver receiver(db, id, writer, "file.bin", 4, hash, 2);
    const std::vector<std::uint8_t> first{'a','b'}, second{'c','d'};
    VSYNC_CHECK_THROWS(receiver.AcceptChunk(0, 0, first, veritassync::common::Blake3(second), 2));
    receiver.AcceptChunk(1, 2, second, veritassync::common::Blake3(second), 3);
    VSYNC_CHECK_THROWS(receiver.Commit());
    receiver.AcceptChunk(0, 0, first, veritassync::common::Blake3(first), 4);
    receiver.Commit();
    VSYNC_CHECK(std::filesystem::exists(root / "file.bin"));
  }
  std::filesystem::remove_all(root); std::filesystem::remove(db_path); std::filesystem::remove(db_path.string()+"-wal"); std::filesystem::remove(db_path.string()+"-shm");
}
