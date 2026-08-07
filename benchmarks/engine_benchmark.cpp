#include "engine/common/protocol.h"
#include "engine/storage/database.h"
#include "engine/storage/ignore_rules.h"
#include "engine/storage/manifest_scanner.h"
#include "engine/sync/manifest_diff.h"
#include "engine/sync/snapshot_reconciler.h"
#include "engine/transport/mock_transport.h"
#include "engine/transport/send_scheduler.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("veritassync-benchmark-" + std::to_string(Clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
  [[nodiscard]] const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

template <typename Operation>
double MeasureMilliseconds(Operation&& operation) {
  const auto begin = Clock::now();
  operation();
  const auto elapsed = Clock::now() - begin;
  return std::chrono::duration<double, std::milli>(elapsed).count();
}

void Report(const std::string_view name, const std::size_t items, const double milliseconds) {
  const auto rate = milliseconds > 0.0 ? static_cast<double>(items) * 1000.0 / milliseconds : 0.0;
  std::cout << std::left << std::setw(28) << name << " items=" << std::setw(8) << items
            << " ms=" << std::fixed << std::setprecision(3) << std::setw(12) << milliseconds
            << " items_per_s=" << std::setprecision(0) << rate << '\n';
}

veritassync::common::ContentHash HashFor(const std::size_t index) {
  veritassync::common::ContentHash hash{};
  for (std::size_t offset = 0; offset < hash.size(); ++offset) {
    hash[offset] = static_cast<std::uint8_t>((index + offset) & 0xffU);
  }
  return hash;
}

std::string PathFor(const std::size_t index) {
  return "tree/dir-" + std::to_string(index / 100U) + "/file-" + std::to_string(index) + ".bin";
}

std::vector<veritassync::storage::SnapshotEntry> MakeSnapshot(const std::size_t count) {
  std::vector<veritassync::storage::SnapshotEntry> entries;
  entries.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    entries.push_back({PathFor(index), veritassync::storage::SnapshotKind::kFile,
                       1024U + index, static_cast<std::int64_t>(1000U + index), HashFor(index)});
  }
  std::ranges::sort(entries, {}, &veritassync::storage::SnapshotEntry::relative_path);
  return entries;
}

std::vector<veritassync::storage::FileRecord> MakeRecords(const std::size_t count) {
  std::vector<veritassync::storage::FileRecord> records;
  records.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto hash = HashFor(index);
    records.push_back({"benchmark", PathFor(index), veritassync::storage::FileKind::kFile,
                       1024U + index, static_cast<std::int64_t>(1000U + index),
                       {hash.begin(), hash.end()}, "version-" + std::to_string(index),
                       "device-a", 1, std::nullopt});
  }
  std::ranges::sort(records, {}, &veritassync::storage::FileRecord::relative_path);
  return records;
}

void BenchmarkManifestDiff(const std::size_t count) {
  const auto snapshot = MakeSnapshot(count);
  const auto records = MakeRecords(count);
  std::size_t changes = 0;
  const auto milliseconds = MeasureMilliseconds([&] {
    const auto result = veritassync::sync::DiffManifest(snapshot, records);
    changes = result.created_or_changed.size() + result.deleted_paths.size();
  });
  if (changes != 0) throw std::runtime_error("unchanged manifest benchmark produced changes");
  Report("manifest_diff_unchanged", count, milliseconds);
}

void BenchmarkSnapshotReconcile(const std::size_t count) {
  TemporaryDirectory temporary;
  veritassync::storage::Database database(temporary.Path() / "state.db");
  database.ApplyMigrations();
  database.CreateTask({"benchmark", "one_way", "source", temporary.Path().string()});
  const auto records = MakeRecords(count);
  database.InTransaction([&] {
    for (const auto& record : records) database.UpsertFileRecord(record);
  });
  const auto snapshot = MakeSnapshot(count);
  std::size_t version = count;
  const auto milliseconds = MeasureMilliseconds([&] {
    const auto result = veritassync::sync::SnapshotReconciler(
        [&] { return "new-version-" + std::to_string(++version); })
                            .Apply(database, snapshot, {"benchmark", "device-a", 1, 1000});
    if (result.created_or_changed != 0 || result.tombstoned != 0) {
      throw std::runtime_error("unchanged reconciliation benchmark produced changes");
    }
  });
  Report("snapshot_reconcile_unchanged", count, milliseconds);
}

void BenchmarkScheduler(const std::size_t count) {
  veritassync::transport::SendScheduler scheduler(count * 32U + 1U, 1U);
  for (std::size_t index = 0; index < count; ++index) {
    scheduler.Enqueue({veritassync::protocol::Channel::kBulk,
                       veritassync::transport::SendPriority::kBulk,
                       std::vector<std::uint8_t>(32U, static_cast<std::uint8_t>(index))});
  }
  std::size_t removed = 0;
  const auto milliseconds = MeasureMilliseconds([&] {
    while (scheduler.Next(0).has_value()) ++removed;
  });
  if (removed != count) throw std::runtime_error("scheduler benchmark lost work");
  Report("scheduler_dequeue", count, milliseconds);
}

void BenchmarkMockTransport(const std::size_t count) {
  veritassync::transport::MockNetwork network;
  auto pair = network.CreatePair();
  std::size_t received = 0;
  pair.second->SetReceiveCallback(
      [&](const veritassync::protocol::Channel, std::vector<std::uint8_t>) { ++received; });
  for (std::size_t index = 0; index < count; ++index) {
    pair.first->Send(veritassync::protocol::Channel::kBulk, {static_cast<std::uint8_t>(index)});
  }
  const auto milliseconds = MeasureMilliseconds([&] { network.PumpUntilIdle(); });
  if (received != count) throw std::runtime_error("mock transport benchmark lost frames");
  Report("mock_transport_pump", count, milliseconds);
}

void BenchmarkProtocolManifest(const std::size_t count) {
  veritassync::protocol::Manifest manifest{1, {}};
  manifest.entries.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    manifest.entries.push_back({PathFor(index), 1024U + index, std::string(64U, 'a')});
  }
  std::vector<std::uint8_t> encoded;
  const auto encode_ms = MeasureMilliseconds([&] { encoded = veritassync::protocol::EncodeManifest(manifest); });
  std::size_t decoded_count = 0;
  const auto decode_ms = MeasureMilliseconds([&] {
    decoded_count = veritassync::protocol::DecodeManifest(encoded).entries.size();
  });
  if (decoded_count != count) throw std::runtime_error("protocol benchmark lost manifest entries");
  Report("manifest_encode", count, encode_ms);
  Report("manifest_decode", count, decode_ms);
}

void BenchmarkChunkCodec(const std::size_t count) {
  veritassync::protocol::Chunk chunk{};
  chunk.bytes.resize(veritassync::protocol::kLogicalChunkSize, 0x5aU);
  chunk.chunk_hash = veritassync::protocol::TestHash(chunk.bytes);
  std::uint64_t decoded_bytes = 0;
  const auto milliseconds = MeasureMilliseconds([&] {
    for (std::size_t index = 0; index < count; ++index) {
      chunk.offset = index * veritassync::protocol::kLogicalChunkSize;
      auto wire = veritassync::protocol::EncodeFrame(
          {veritassync::protocol::FrameType::kChunk, index,
           veritassync::protocol::EncodeChunk(chunk)});
      auto frame = veritassync::protocol::DecodeFrame(wire);
      decoded_bytes += veritassync::protocol::DecodeChunk(frame.payload).bytes.size();
    }
  });
  if (decoded_bytes != count * veritassync::protocol::kLogicalChunkSize) {
    throw std::runtime_error("chunk codec benchmark lost bytes");
  }
  Report("chunk_codec_256k", count, milliseconds);
}

void BenchmarkManifestScan(const std::size_t count) {
  TemporaryDirectory temporary;
  for (std::size_t index = 0; index < count; ++index) {
    const auto directory = temporary.Path() / ("dir-" + std::to_string(index / 100U));
    std::filesystem::create_directories(directory);
    std::ofstream stream(directory / ("file-" + std::to_string(index) + ".txt"), std::ios::binary);
    stream << "benchmark-" << index;
  }
  veritassync::storage::ManifestScanner scanner(veritassync::storage::IgnoreRules{});
  std::size_t scanned = 0;
  const auto milliseconds = MeasureMilliseconds([&] { scanned = scanner.Scan(temporary.Path()).size(); });
  if (scanned < count) throw std::runtime_error("scanner benchmark lost files");
  Report("manifest_scan_tiny_files", count, milliseconds);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::size_t count = 5000;
    if (argc == 3 && std::string_view(argv[1]) == "--items") {
      count = std::stoull(argv[2]);
    } else if (argc != 1) {
      throw std::invalid_argument("usage: veritassync_benchmarks [--items N]");
    }
    if (count < 100) throw std::invalid_argument("benchmark requires at least 100 items");
    std::cout << "VeritasSyncNext deterministic engine benchmark\n";
    BenchmarkManifestDiff(count);
    BenchmarkSnapshotReconcile(count);
    BenchmarkScheduler(count * 4U);
    BenchmarkMockTransport(count * 4U);
    BenchmarkProtocolManifest(count * 10U);
    BenchmarkChunkCodec(256U);
    BenchmarkManifestScan(count);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "benchmark error: " << error.what() << '\n';
    return 1;
  }
}
