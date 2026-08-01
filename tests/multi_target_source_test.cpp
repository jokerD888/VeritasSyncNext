#include "engine/common/protocol.h"
#include "engine/storage/database.h"
#include "engine/sync/multi_target_source.h"
#include "engine/sync/one_way_sync.h"
#include "engine/transport/mock_transport.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class TemporaryMultiTargetDirectories {
 public:
  TemporaryMultiTargetDirectories() {
    const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    root_ = std::filesystem::temp_directory_path() / ("veritassync-multi-target-" + nonce);
    source_ = root_ / "source";
    target_a_ = root_ / "target-a";
    target_b_ = root_ / "target-b";
    std::filesystem::create_directories(source_);
    std::filesystem::create_directories(target_a_);
    std::filesystem::create_directories(target_b_);
  }
  ~TemporaryMultiTargetDirectories() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }
  [[nodiscard]] const std::filesystem::path& Source() const { return source_; }
  [[nodiscard]] const std::filesystem::path& TargetA() const { return target_a_; }
  [[nodiscard]] const std::filesystem::path& TargetB() const { return target_b_; }
  [[nodiscard]] std::filesystem::path DatabasePath(const std::string& name) const {
    return root_ / (name + ".db");
  }

 private:
  std::filesystem::path root_;
  std::filesystem::path source_;
  std::filesystem::path target_a_;
  std::filesystem::path target_b_;
};

void WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

veritassync::sync::OneWaySyncConfig TargetConfig(
    const std::string& target_id, const std::filesystem::path& root,
    veritassync::storage::Database& database) {
  return {"task-multi", veritassync::protocol::Role::kTarget, target_id, "source",
          target_id + "-fingerprint", "shared-auth", root, database};
}

void InitializeTask(veritassync::storage::Database& database, const std::string& role,
                    const std::filesystem::path& root) {
  database.ApplyMigrations();
  database.CreateTask({"task-multi", "one_way", role, root.string()});
}

}  // namespace

VSYNC_TEST(MultiTargetSourceSharesOneSnapshotAndIsolatesSlowTarget) {
  using namespace veritassync;
  TemporaryMultiTargetDirectories directories;
  const std::vector<std::uint8_t> bytes{'m', 'u', 'l', 't', 'i'};
  WriteBytes(directories.Source() / "payload.bin", bytes);

  storage::Database source_db(directories.DatabasePath("source"));
  storage::Database target_a_db(directories.DatabasePath("target-a"));
  storage::Database target_b_db(directories.DatabasePath("target-b"));
  InitializeTask(source_db, "source", directories.Source());
  InitializeTask(target_a_db, "target", directories.TargetA());
  InitializeTask(target_b_db, "target", directories.TargetB());

  transport::MockNetwork network_a;
  transport::MockNetwork network_b;
  auto pair_a = network_a.CreatePair();
  auto pair_b = network_b.CreatePair();
  sync::MultiTargetSource source({"task-multi", "source", "source-fingerprint",
                                  directories.Source(), source_db});
  source.AddTarget({"target-a", "target-a-fingerprint", "shared-auth"}, *pair_a.first);
  source.AddTarget({"target-b", "target-b-fingerprint", "shared-auth"}, *pair_b.first);
  sync::OneWaySyncNode target_a(TargetConfig("target-a", directories.TargetA(), target_a_db),
                                *pair_a.second);
  sync::OneWaySyncNode target_b(TargetConfig("target-b", directories.TargetB(), target_b_db),
                                *pair_b.second);

  source.Start();
  target_a.Start();
  target_b.Start();
  network_a.PumpUntilIdle();
  network_b.PumpUntilIdle();
  VSYNC_CHECK(source.TargetCount() == 2);
  VSYNC_CHECK(source.SnapshotScanCount() == 1);
  VSYNC_CHECK(source.HandshakeComplete("target-a"));
  VSYNC_CHECK(source.HandshakeComplete("target-b"));
  VSYNC_CHECK(target_a.PendingDownloadCount() == 1);
  VSYNC_CHECK(target_b.PendingDownloadCount() == 1);

  pair_a.first->SetBufferedAmount(protocol::Channel::kBulk, 1024U * 1024U);
  source.Pump();
  network_b.PumpUntilIdle();
  VSYNC_CHECK(target_b.TargetIsConverged());
  VSYNC_CHECK(!target_a.TargetIsConverged());
  VSYNC_CHECK(ReadBytes(directories.TargetB() / "payload.bin") == bytes);
  const auto slow_stats = source.Statistics("target-a");
  const auto fast_stats = source.Statistics("target-b");
  VSYNC_CHECK(slow_stats.has_value() && slow_stats->backpressure_pauses == 1);
  VSYNC_CHECK(fast_stats.has_value() && fast_stats->chunks_sent == 1);

  pair_a.first->SetBufferedAmount(protocol::Channel::kBulk, 0);
  source.Pump();
  network_a.PumpUntilIdle();
  VSYNC_CHECK(target_a.TargetIsConverged());
  VSYNC_CHECK(ReadBytes(directories.TargetA() / "payload.bin") == bytes);

  source.RefreshSource();
  network_a.PumpUntilIdle();
  network_b.PumpUntilIdle();
  VSYNC_CHECK(source.SnapshotScanCount() == 2);
  VSYNC_CHECK(target_a.TargetIsConverged());
  VSYNC_CHECK(target_b.TargetIsConverged());
}

VSYNC_TEST(MultiTargetSourceRejectsTargetManifestWrites) {
  using namespace veritassync;
  TemporaryMultiTargetDirectories directories;
  storage::Database source_db(directories.DatabasePath("source"));
  storage::Database target_db(directories.DatabasePath("target-a"));
  InitializeTask(source_db, "source", directories.Source());
  InitializeTask(target_db, "target", directories.TargetA());
  transport::MockNetwork network;
  auto pair = network.CreatePair();
  sync::MultiTargetSource source({"task-multi", "source", "source-fingerprint",
                                  directories.Source(), source_db});
  source.AddTarget({"target-a", "target-a-fingerprint", "shared-auth"}, *pair.first);
  sync::OneWaySyncNode target(TargetConfig("target-a", directories.TargetA(), target_db), *pair.second);
  source.Start();
  target.Start();
  network.PumpUntilIdle();

  const protocol::Manifest forged{1, {}};
  pair.second->Send(protocol::Channel::kControl,
                    protocol::EncodeFrame({protocol::FrameType::kManifest, 99,
                                           protocol::EncodeManifest(forged)}));
  network.PumpUntilIdle();
  VSYNC_CHECK(source.LastError("target-a") ==
              std::optional<std::string>{"target_write_forbidden"});
}
