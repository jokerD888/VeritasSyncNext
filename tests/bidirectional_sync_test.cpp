#include "engine/storage/database.h"
#include "engine/sync/bidirectional_sync.h"
#include "engine/transport/mock_transport.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

class TemporaryBidirectionalDirectories {
 public:
  TemporaryBidirectionalDirectories() {
    const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    root_ = std::filesystem::temp_directory_path() / ("veritassync-bidirectional-" + nonce);
    left_ = root_ / "left";
    right_ = root_ / "right";
    std::filesystem::create_directories(left_);
    std::filesystem::create_directories(right_);
  }
  ~TemporaryBidirectionalDirectories() { std::error_code error; std::filesystem::remove_all(root_, error); }
  [[nodiscard]] const std::filesystem::path& Left() const { return left_; }
  [[nodiscard]] const std::filesystem::path& Right() const { return right_; }
  [[nodiscard]] std::filesystem::path Db(const std::string& name) const { return root_ / (name + ".db"); }

 private:
  std::filesystem::path root_, left_, right_;
};

void WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

veritassync::sync::BidirectionalSyncConfig Config(const std::string& local,
                                                   const std::string& remote,
                                                   const std::filesystem::path& root,
                                                   veritassync::storage::Database& database) {
  return {"task-peer", local, remote, local + "-fingerprint", remote + "-fingerprint",
          "shared-auth", root, database};
}

void Initialize(veritassync::storage::Database& database, const std::filesystem::path& root) {
  database.ApplyMigrations();
  database.CreateTask({"task-peer", "bidirectional", "peer", root.string()});
}

void Drive(veritassync::transport::MockNetwork& network,
           veritassync::sync::BidirectionalSyncNode& left,
           veritassync::sync::BidirectionalSyncNode& right) {
  for (int iteration = 0; iteration < 64; ++iteration) {
    network.PumpUntilIdle();
    left.Pump();
    right.Pump();
    network.PumpUntilIdle();
    if (left.IsConverged() && right.IsConverged()) return;
  }
  throw std::runtime_error("bidirectional mock session did not converge");
}

}  // namespace

VSYNC_TEST(BidirectionalSyncConvergesInitialReplicaAndOfflineConcurrentEdits) {
  using namespace veritassync;
  TemporaryBidirectionalDirectories directories;
  WriteBytes(directories.Left() / "shared.txt", {'b', 'a', 's', 'e'});
  storage::Database left_db(directories.Db("left"));
  storage::Database right_db(directories.Db("right"));
  Initialize(left_db, directories.Left()); Initialize(right_db, directories.Right());
  transport::MockNetwork network;
  auto endpoints = network.CreatePair();
  sync::BidirectionalSyncNode left(Config("device-a", "device-b", directories.Left(), left_db), *endpoints.first);
  sync::BidirectionalSyncNode right(Config("device-b", "device-a", directories.Right(), right_db), *endpoints.second);
  left.Start(); right.Start(); Drive(network, left, right);
  VSYNC_CHECK(ReadBytes(directories.Right() / "shared.txt") == std::vector<std::uint8_t>({'b', 'a', 's', 'e'}));

  WriteBytes(directories.Left() / "shared.txt", {'a', 'l', 'p', 'h', 'a'});
  WriteBytes(directories.Right() / "shared.txt", {'b', 'r', 'a', 'v', 'o'});
  left.RefreshLocal();
  right.RefreshLocal();
  Drive(network, left, right);

  const auto expected_formal = std::vector<std::uint8_t>({'a', 'l', 'p', 'h', 'a'});
  const auto expected_conflict = std::vector<std::uint8_t>({'b', 'r', 'a', 'v', 'o'});
  const auto conflict = "shared.conflict.device-b.3.txt";
  VSYNC_CHECK(ReadBytes(directories.Left() / "shared.txt") == expected_formal);
  VSYNC_CHECK(ReadBytes(directories.Right() / "shared.txt") == expected_formal);
  VSYNC_CHECK(ReadBytes(directories.Left() / conflict) == expected_conflict);
  VSYNC_CHECK(ReadBytes(directories.Right() / conflict) == expected_conflict);
  VSYNC_CHECK(left_db.ListConflicts("task-peer").size() == 1);
  VSYNC_CHECK(right_db.ListConflicts("task-peer").size() == 1);
}

VSYNC_TEST(BidirectionalSyncPreservesDeleteModifyAndFileDirectoryConflicts) {
  using namespace veritassync;
  TemporaryBidirectionalDirectories directories;
  WriteBytes(directories.Left() / "delete.txt", {'b', 'a', 's', 'e'});
  WriteBytes(directories.Left() / "node", {'f', 'i', 'l', 'e'});
  storage::Database left_db(directories.Db("left"));
  storage::Database right_db(directories.Db("right"));
  Initialize(left_db, directories.Left()); Initialize(right_db, directories.Right());
  transport::MockNetwork network;
  auto endpoints = network.CreatePair();
  sync::BidirectionalSyncNode left(Config("device-a", "device-b", directories.Left(), left_db), *endpoints.first);
  sync::BidirectionalSyncNode right(Config("device-b", "device-a", directories.Right(), right_db), *endpoints.second);
  left.Start(); right.Start(); Drive(network, left, right);

  std::filesystem::remove(directories.Left() / "delete.txt");
  WriteBytes(directories.Right() / "delete.txt", {'r', 'i', 'g', 'h', 't'});
  std::filesystem::remove(directories.Left() / "node");
  std::filesystem::create_directory(directories.Left() / "node");
  WriteBytes(directories.Left() / "node" / "child.txt", {'c', 'h', 'i', 'l', 'd'});
  WriteBytes(directories.Right() / "node", {'r', 'i', 'g', 'h', 't'});
  left.RefreshLocal(); right.RefreshLocal(); Drive(network, left, right);

  // The concurrent edit's clock is lower in this run, so it deterministically
  // wins the formal path while the deletion is retained as a conflict record.
  VSYNC_CHECK(ReadBytes(directories.Left() / "delete.txt") == std::vector<std::uint8_t>({'r', 'i', 'g', 'h', 't'}));
  VSYNC_CHECK(ReadBytes(directories.Right() / "delete.txt") == std::vector<std::uint8_t>({'r', 'i', 'g', 'h', 't'}));
  VSYNC_CHECK(std::filesystem::is_directory(directories.Left() / "node"));
  VSYNC_CHECK(std::filesystem::is_directory(directories.Right() / "node"));
  VSYNC_CHECK(ReadBytes(directories.Left() / "node" / "child.txt") == std::vector<std::uint8_t>({'c', 'h', 'i', 'l', 'd'}));
  VSYNC_CHECK(ReadBytes(directories.Right() / "node" / "child.txt") == std::vector<std::uint8_t>({'c', 'h', 'i', 'l', 'd'}));
}

VSYNC_TEST(BidirectionalSyncConvergesOfflineBranchesAfterBothPeersRestart) {
  using namespace veritassync;
  TemporaryBidirectionalDirectories directories;
  WriteBytes(directories.Left() / "restart.txt", {'b', 'a', 's', 'e'});
  storage::Database left_db(directories.Db("left"));
  storage::Database right_db(directories.Db("right"));
  Initialize(left_db, directories.Left()); Initialize(right_db, directories.Right());

  {
    transport::MockNetwork network;
    auto endpoints = network.CreatePair();
    auto left = std::make_unique<sync::BidirectionalSyncNode>(
        Config("device-a", "device-b", directories.Left(), left_db), *endpoints.first);
    auto right = std::make_unique<sync::BidirectionalSyncNode>(
        Config("device-b", "device-a", directories.Right(), right_db), *endpoints.second);
    left->Start(); right->Start(); Drive(network, *left, *right);
    WriteBytes(directories.Left() / "restart.txt", {'l', 'e', 'f', 't'});
    WriteBytes(directories.Right() / "restart.txt", {'r', 'i', 'g', 'h', 't'});
    left->RefreshLocal(); right->RefreshLocal();
    // Deliberately discard the queued frames: the next connection must rebuild
    // the same conclusion using only directories and durable SQLite state.
  }

  transport::MockNetwork reconnected_network;
  auto reconnected = reconnected_network.CreatePair();
  sync::BidirectionalSyncNode left(Config("device-a", "device-b", directories.Left(), left_db),
                                   *reconnected.first);
  sync::BidirectionalSyncNode right(Config("device-b", "device-a", directories.Right(), right_db),
                                    *reconnected.second);
  left.Start(); right.Start(); Drive(reconnected_network, left, right);
  VSYNC_CHECK(ReadBytes(directories.Left() / "restart.txt") == std::vector<std::uint8_t>({'l', 'e', 'f', 't'}));
  VSYNC_CHECK(ReadBytes(directories.Right() / "restart.txt") == std::vector<std::uint8_t>({'l', 'e', 'f', 't'}));
  const auto conflict = "restart.conflict.device-b.3.txt";
  VSYNC_CHECK(ReadBytes(directories.Left() / conflict) == std::vector<std::uint8_t>({'r', 'i', 'g', 'h', 't'}));
  VSYNC_CHECK(ReadBytes(directories.Right() / conflict) == std::vector<std::uint8_t>({'r', 'i', 'g', 'h', 't'}));
}
