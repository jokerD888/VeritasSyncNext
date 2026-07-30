#include "engine/common/content_hash.h"
#include "engine/storage/database.h"
#include "engine/sync/one_way_sync.h"
#include "engine/transport/mock_transport.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

class TemporarySyncDirectories {
 public:
  TemporarySyncDirectories() {
    const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    root_ = std::filesystem::temp_directory_path() / ("veritassync-one-way-" + nonce);
    source_ = root_ / "source";
    target_ = root_ / "target";
    std::filesystem::create_directories(source_);
    std::filesystem::create_directories(target_);
  }
  ~TemporarySyncDirectories() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }
  [[nodiscard]] const std::filesystem::path& Source() const { return source_; }
  [[nodiscard]] const std::filesystem::path& Target() const { return target_; }
  [[nodiscard]] std::filesystem::path SourceDb() const { return root_ / "source.db"; }
  [[nodiscard]] std::filesystem::path TargetDb() const { return root_ / "target.db"; }

 private:
  std::filesystem::path root_;
  std::filesystem::path source_;
  std::filesystem::path target_;
};

void WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

veritassync::sync::OneWaySyncConfig Config(
    const veritassync::protocol::Role role, const std::filesystem::path& root,
    veritassync::storage::Database& database) {
  const bool source = role == veritassync::protocol::Role::kSource;
  return {"task-1", role, source ? "source" : "target", source ? "target" : "source",
          source ? "source-fingerprint" : "target-fingerprint", "shared-auth", root, database};
}

}  // namespace

VSYNC_TEST(OneWaySyncResumesValidatedDownloadThenAppliesSourceTombstone) {
  using namespace veritassync;
  TemporarySyncDirectories directories;
  std::vector<std::uint8_t> source_bytes(protocol::kLogicalChunkSize + 17U, 0x42);
  source_bytes.back() = 0x7f;
  WriteBytes(directories.Source() / "nested/file.bin", source_bytes);

  storage::Database source_db(directories.SourceDb());
  storage::Database target_db(directories.TargetDb());
  source_db.ApplyMigrations();
  target_db.ApplyMigrations();
  source_db.CreateTask({"task-1", "one_way", "source", directories.Source().string()});
  target_db.CreateTask({"task-1", "one_way", "target", directories.Target().string()});

  transport::MockNetwork network;
  auto endpoints = network.CreatePair();
  sync::OneWaySyncNode source(Config(protocol::Role::kSource, directories.Source(), source_db),
                              *endpoints.first);
  auto target = std::make_unique<sync::OneWaySyncNode>(
      Config(protocol::Role::kTarget, directories.Target(), target_db), *endpoints.second);
  source.Start();
  target->Start();
  network.PumpUntilIdle();
  source.Pump();
  VSYNC_CHECK(network.PumpOne());
  VSYNC_CHECK(target->PendingDownloadCount() == 1);

  target.reset();
  target = std::make_unique<sync::OneWaySyncNode>(
      Config(protocol::Role::kTarget, directories.Target(), target_db), *endpoints.second);
  target->Start();
  network.PumpUntilIdle();
  source.Pump();
  network.PumpUntilIdle();

  VSYNC_CHECK(target->TargetIsConverged());
  VSYNC_CHECK(!target->LastError().has_value());
  VSYNC_CHECK(ReadBytes(directories.Target() / "nested/file.bin") == source_bytes);
  const auto record = target_db.FindFileRecord("task-1", "nested/file.bin");
  VSYNC_CHECK(record.has_value() && record->kind == storage::FileKind::kFile);
  VSYNC_CHECK(target_db.CountRows("transfer_chunks") == 2);

  std::filesystem::remove(directories.Source() / "nested/file.bin");
  source.RefreshSource();
  network.PumpUntilIdle();
  VSYNC_CHECK(!std::filesystem::exists(directories.Target() / "nested/file.bin"));
  const auto tombstone = target_db.FindFileRecord("task-1", "nested/file.bin");
  VSYNC_CHECK(tombstone.has_value() && tombstone->kind == storage::FileKind::kTombstone);
  const auto source_tombstone = source_db.FindFileRecord("task-1", "nested/file.bin");
  VSYNC_CHECK(source_tombstone.has_value() && source_tombstone->kind == storage::FileKind::kTombstone);
}

VSYNC_TEST(OneWaySyncCancelsStaleSourceThenRetriesFreshManifest) {
  using namespace veritassync;
  TemporarySyncDirectories directories;
  const std::vector<std::uint8_t> initial{'o', 'l', 'd'};
  const std::vector<std::uint8_t> replacement{'n', 'e', 'w', '!'};
  WriteBytes(directories.Source() / "file.bin", initial);

  storage::Database source_db(directories.SourceDb());
  storage::Database target_db(directories.TargetDb());
  source_db.ApplyMigrations();
  target_db.ApplyMigrations();
  source_db.CreateTask({"task-1", "one_way", "source", directories.Source().string()});
  target_db.CreateTask({"task-1", "one_way", "target", directories.Target().string()});
  transport::MockNetwork network;
  auto endpoints = network.CreatePair();
  sync::OneWaySyncNode source(Config(protocol::Role::kSource, directories.Source(), source_db),
                              *endpoints.first);
  sync::OneWaySyncNode target(Config(protocol::Role::kTarget, directories.Target(), target_db),
                              *endpoints.second);
  source.Start();
  target.Start();

  while (network.PumpOne()) {
    if (target.PendingDownloadCount() == 1) break;
  }
  WriteBytes(directories.Source() / "file.bin", replacement);
  network.PumpUntilIdle();
  VSYNC_CHECK(target.LastError() == std::optional<std::string>{"source_changed"});
  VSYNC_CHECK(!target.TargetIsConverged());

  source.RefreshSource();
  network.PumpUntilIdle();
  source.Pump();
  network.PumpUntilIdle();
  if (target.LastError().has_value()) {
    throw std::runtime_error("retry failed: " + *target.LastError());
  }
  VSYNC_CHECK(target.PendingDownloadCount() == 0);
  VSYNC_CHECK(target.HandshakeComplete());
  VSYNC_CHECK(target.TargetIsConverged());
  VSYNC_CHECK(ReadBytes(directories.Target() / "file.bin") == replacement);
}

VSYNC_TEST(OneWaySyncAppliesMultipleFilesIncludingEmptyFile) {
  using namespace veritassync;
  TemporarySyncDirectories directories;
  const std::vector<std::uint8_t> text{'h', 'e', 'l', 'l', 'o'};
  const std::vector<std::uint8_t> empty;
  WriteBytes(directories.Source() / "notes.txt", text);
  WriteBytes(directories.Source() / "nested/empty.bin", empty);
  std::filesystem::create_directories(directories.Source() / "empty-directory");
  storage::Database source_db(directories.SourceDb());
  storage::Database target_db(directories.TargetDb());
  source_db.ApplyMigrations(); target_db.ApplyMigrations();
  source_db.CreateTask({"task-1", "one_way", "source", directories.Source().string()});
  target_db.CreateTask({"task-1", "one_way", "target", directories.Target().string()});
  transport::MockNetwork network;
  auto endpoints = network.CreatePair();
  sync::OneWaySyncNode source(Config(protocol::Role::kSource, directories.Source(), source_db), *endpoints.first);
  sync::OneWaySyncNode target(Config(protocol::Role::kTarget, directories.Target(), target_db), *endpoints.second);
  source.Start(); target.Start(); network.PumpUntilIdle();
  for (int attempt = 0; attempt < 8 && !target.TargetIsConverged(); ++attempt) {
    source.Pump();
    network.PumpUntilIdle();
  }
  VSYNC_CHECK(target.TargetIsConverged());
  VSYNC_CHECK(ReadBytes(directories.Target() / "notes.txt") == text);
  VSYNC_CHECK(std::filesystem::is_regular_file(directories.Target() / "nested/empty.bin"));
  VSYNC_CHECK(std::filesystem::file_size(directories.Target() / "nested/empty.bin") == 0);
  VSYNC_CHECK(std::filesystem::is_directory(directories.Target() / "empty-directory"));
  VSYNC_CHECK(source.Statistics().chunks_sent == 1);
  VSYNC_CHECK(source.Statistics().bulk_bytes_sent > 0);
  VSYNC_CHECK(target.Statistics().chunks_received == 1);
  VSYNC_CHECK(target.Statistics().files_committed == 2);
  VSYNC_CHECK(target.Statistics().control_bytes_received > 0);

  std::filesystem::remove(directories.Source() / "empty-directory");
  source.RefreshSource();
  network.PumpUntilIdle();
  VSYNC_CHECK(!std::filesystem::exists(directories.Target() / "empty-directory"));
  const auto directory_tombstone = target_db.FindFileRecord("task-1", "empty-directory");
  VSYNC_CHECK(directory_tombstone.has_value() && directory_tombstone->kind == storage::FileKind::kTombstone);
}

VSYNC_TEST(OneWaySyncStreamsManyLogicalChunksWithoutRestart) {
  using namespace veritassync;
  TemporarySyncDirectories directories;
  std::vector<std::uint8_t> bytes(protocol::kLogicalChunkSize * 32U + 31U, 0x5a);
  bytes.back() = 0x33;
  WriteBytes(directories.Source() / "large.bin", bytes);
  storage::Database source_db(directories.SourceDb());
  storage::Database target_db(directories.TargetDb());
  source_db.ApplyMigrations(); target_db.ApplyMigrations();
  source_db.CreateTask({"task-1", "one_way", "source", directories.Source().string()});
  target_db.CreateTask({"task-1", "one_way", "target", directories.Target().string()});
  transport::MockNetwork network;
  auto endpoints = network.CreatePair();
  sync::OneWaySyncNode source(Config(protocol::Role::kSource, directories.Source(), source_db), *endpoints.first);
  sync::OneWaySyncNode target(Config(protocol::Role::kTarget, directories.Target(), target_db), *endpoints.second);
  source.Start(); target.Start(); network.PumpUntilIdle();
  for (std::size_t attempt = 0; attempt < 40 && !target.TargetIsConverged(); ++attempt) {
    source.Pump();
    network.PumpUntilIdle();
  }
  VSYNC_CHECK(target.TargetIsConverged());
  VSYNC_CHECK(source.Statistics().chunks_sent == 33);
  VSYNC_CHECK(target.Statistics().chunks_received == 33);
  VSYNC_CHECK(common::Blake3File(directories.Target() / "large.bin") == common::Blake3(bytes));
}

VSYNC_TEST(OneWaySyncWaitsForTransportBackpressureToClear) {
  using namespace veritassync;
  TemporarySyncDirectories directories;
  const std::vector<std::uint8_t> bytes{'b', 'a', 'c', 'k', 'p', 'r', 'e', 's', 's', 'u', 'r', 'e'};
  WriteBytes(directories.Source() / "file.bin", bytes);
  storage::Database source_db(directories.SourceDb());
  storage::Database target_db(directories.TargetDb());
  source_db.ApplyMigrations(); target_db.ApplyMigrations();
  source_db.CreateTask({"task-1", "one_way", "source", directories.Source().string()});
  target_db.CreateTask({"task-1", "one_way", "target", directories.Target().string()});
  transport::MockNetwork network;
  auto endpoints = network.CreatePair();
  sync::OneWaySyncNode source(Config(protocol::Role::kSource, directories.Source(), source_db), *endpoints.first);
  sync::OneWaySyncNode target(Config(protocol::Role::kTarget, directories.Target(), target_db), *endpoints.second);
  source.Start(); target.Start(); network.PumpUntilIdle();
  endpoints.first->SetBufferedAmount(protocol::Channel::kBulk, 1024U * 1024U);
  source.Pump();
  VSYNC_CHECK(!network.PumpOne());
  VSYNC_CHECK(source.Statistics().backpressure_pauses == 1);
  endpoints.first->SetBufferedAmount(protocol::Channel::kBulk, 0);
  source.Pump(); network.PumpUntilIdle();
  VSYNC_CHECK(target.TargetIsConverged());
  VSYNC_CHECK(ReadBytes(directories.Target() / "file.bin") == bytes);
}
