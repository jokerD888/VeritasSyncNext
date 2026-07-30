#pragma once

#include "engine/common/protocol.h"
#include "engine/storage/database.h"
#include "engine/storage/safe_file_writer.h"
#include "engine/transport/transport.h"

#include <filesystem>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace veritassync::sync {

struct OneWaySyncConfig {
  std::string task_id;
  protocol::Role role;
  std::string device_id;
  std::string peer_device_id;
  std::string device_fingerprint;
  std::string authorization_digest;
  std::filesystem::path task_root;
  storage::Database& database;
};

struct TransferStatistics {
  std::uint64_t control_bytes_sent = 0;
  std::uint64_t bulk_bytes_sent = 0;
  std::uint64_t control_bytes_received = 0;
  std::uint64_t bulk_bytes_received = 0;
  std::uint64_t chunks_sent = 0;
  std::uint64_t chunks_received = 0;
  std::uint64_t files_committed = 0;
  std::uint64_t files_deleted = 0;
  std::uint64_t backpressure_pauses = 0;
};

// A single-source/single-target session. The owner drives Pump from its event
// loop; Source scanning and Target file writes remain outside the transport API.
class OneWaySyncNode {
 public:
  OneWaySyncNode(OneWaySyncConfig config, transport::Transport& transport);
  ~OneWaySyncNode();
  void Start();
  void RefreshSource();
  void Pump();
  [[nodiscard]] bool HandshakeComplete() const;
  [[nodiscard]] bool TargetIsConverged() const;
  [[nodiscard]] std::size_t PendingDownloadCount() const;
  [[nodiscard]] TransferStatistics Statistics() const;
  [[nodiscard]] std::optional<std::string> LastError() const;

 private:
  struct SourceFile;
  struct ActiveDownload;
  struct PendingUpload;

  void Receive(protocol::Channel channel, std::vector<std::uint8_t> wire);
  void Send(protocol::Channel channel, protocol::FrameType type,
            std::vector<std::uint8_t> payload);
  void SendManifest();
  void ApplyManifest(const protocol::Manifest& manifest);
  void BeginDownload(const protocol::ManifestEntry& entry);
  void AcceptChunk(const protocol::Chunk& chunk);
  void CommitDownload(ActiveDownload& download);
  void HandleFileRequest(const protocol::FileRequest& request);
  void HandleCancel(const protocol::Cancel& cancel);
  void DeleteFilesAbsentFrom(const protocol::Manifest& manifest);
  [[nodiscard]] bool TargetAlreadyHas(const protocol::ManifestEntry& entry) const;
  [[nodiscard]] bool RolesCompatible(protocol::Role peer_role) const;

  OneWaySyncConfig config_;
  transport::Transport& transport_;
  storage::SafeFileWriter writer_;
  bool started_ = false;
  bool received_hello_ = false;
  std::uint64_t next_request_id_ = 1;
  std::uint64_t manifest_revision_ = 0;
  protocol::Manifest source_manifest_;
  std::vector<SourceFile> source_files_;
  std::vector<ActiveDownload> downloads_;
  std::vector<PendingUpload> uploads_;
  std::optional<protocol::Manifest> received_manifest_;
  std::optional<std::string> last_error_;
  TransferStatistics statistics_;
  mutable std::recursive_mutex mutex_;
};

}  // namespace veritassync::sync
