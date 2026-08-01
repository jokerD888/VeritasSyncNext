#pragma once

#include "engine/common/protocol.h"
#include "engine/storage/database.h"
#include "engine/transport/transport.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace veritassync::sync {

// Identity shared by every outbound one-way session owned by this source.
struct MultiTargetSourceConfig {
  std::string task_id;
  std::string device_id;
  std::string device_fingerprint;
  std::filesystem::path task_root;
  storage::Database& database;
};

struct MultiTargetPeerConfig {
  std::string device_id;
  std::string device_fingerprint;
  std::string authorization_digest;
};

struct MultiTargetPeerStatistics {
  std::uint64_t control_bytes_sent = 0;
  std::uint64_t bulk_bytes_sent = 0;
  std::uint64_t control_bytes_received = 0;
  std::uint64_t chunks_sent = 0;
  std::uint64_t backpressure_pauses = 0;
};

// A single authoritative source shared by independent target sessions.  A scan
// creates one immutable manifest revision, then every connected target receives
// that same revision.  Each peer has its own upload queue and transport budget,
// so a congested peer cannot consume another peer's bulk-send opportunity.
class MultiTargetSource {
 public:
  explicit MultiTargetSource(MultiTargetSourceConfig config);
  ~MultiTargetSource();

  MultiTargetSource(const MultiTargetSource&) = delete;
  MultiTargetSource& operator=(const MultiTargetSource&) = delete;

  void AddTarget(MultiTargetPeerConfig config, transport::Transport& transport);
  void Start();
  void RefreshSource();
  void Pump();

  [[nodiscard]] std::size_t TargetCount() const;
  [[nodiscard]] std::size_t SnapshotScanCount() const;
  [[nodiscard]] bool HandshakeComplete(const std::string& target_device_id) const;
  [[nodiscard]] std::optional<MultiTargetPeerStatistics> Statistics(
      const std::string& target_device_id) const;
  [[nodiscard]] std::optional<std::string> LastError(
      const std::string& target_device_id) const;

 private:
  struct SourceFile;
  struct PendingUpload;
  struct Peer;

  void Receive(Peer& peer, protocol::Channel channel, std::vector<std::uint8_t> wire);
  void Send(Peer& peer, protocol::Channel channel, protocol::FrameType type,
            std::vector<std::uint8_t> payload);
  void SendHello(Peer& peer);
  void SendManifest(Peer& peer);
  void HandleFileRequest(Peer& peer, const protocol::FileRequest& request);
  void HandleCancel(Peer& peer, const protocol::Cancel& cancel);
  [[nodiscard]] Peer* FindPeer(const std::string& target_device_id);
  [[nodiscard]] const Peer* FindPeer(const std::string& target_device_id) const;

  MultiTargetSourceConfig config_;
  bool started_ = false;
  std::uint64_t manifest_revision_ = 0;
  std::size_t snapshot_scan_count_ = 0;
  protocol::Manifest source_manifest_;
  std::vector<SourceFile> source_files_;
  std::vector<std::unique_ptr<Peer>> peers_;
  mutable std::recursive_mutex mutex_;
};

}  // namespace veritassync::sync
