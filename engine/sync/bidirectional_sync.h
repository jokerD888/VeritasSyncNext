#pragma once

#include "engine/common/protocol.h"
#include "engine/storage/database.h"
#include "engine/storage/safe_file_writer.h"
#include "engine/transport/transport.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace veritassync::sync {

struct BidirectionalSyncConfig {
  std::string task_id;
  std::string device_id;
  std::string peer_device_id;
  std::string device_fingerprint;
  std::string peer_device_fingerprint;
  std::string authorization_digest;
  std::filesystem::path task_root;
  storage::Database& database;
};

// A two-peer, version-aware session. Both peers may create local revisions;
// all incoming revisions are resolved through VersionResolver before any
// filesystem mutation or transfer request is made.
class BidirectionalSyncNode {
 public:
  BidirectionalSyncNode(BidirectionalSyncConfig config, transport::Transport& transport);
  ~BidirectionalSyncNode();
  void Start();
  void RefreshLocal();
  void Pump();
  [[nodiscard]] bool HandshakeComplete() const;
  [[nodiscard]] bool IsConverged() const;
  [[nodiscard]] std::optional<std::string> LastError() const;
  [[nodiscard]] std::size_t PendingDownloadCount() const;

 private:
  struct SourceFile;
  struct PendingUpload;
  struct ActiveDownload;

  void Receive(protocol::Channel channel, std::vector<std::uint8_t> wire);
  void Send(protocol::Channel channel, protocol::FrameType type,
            std::vector<std::uint8_t> payload);
  void SendManifest();
  void ApplyManifest(const protocol::VersionedManifest& manifest);
  void ApplyRemoteEntry(const protocol::VersionedManifestEntry& entry);
  void BeginDownload(const protocol::VersionedManifestEntry& entry,
                     std::string destination_path, bool apply_formal_record);
  void AcceptChunk(const protocol::Chunk& chunk);
  void CommitDownload(ActiveDownload& download);
  void HandleFileRequest(const protocol::FileRequest& request);
  void HandleCancel(const protocol::Cancel& cancel);
  void UpsertRemoteRecord(const protocol::VersionedManifestEntry& entry);
  void RelocateSourceFile(std::string_view old_path, std::string_view new_path);

  BidirectionalSyncConfig config_;
  transport::Transport& transport_;
  storage::SafeFileWriter writer_;
  bool started_ = false;
  bool received_hello_ = false;
  bool received_manifest_ = false;
  std::uint64_t next_request_id_ = 1;
  std::uint64_t manifest_revision_ = 0;
  std::vector<SourceFile> source_files_;
  std::vector<PendingUpload> uploads_;
  std::vector<ActiveDownload> downloads_;
  std::optional<std::string> last_error_;
  mutable std::recursive_mutex mutex_;
};

}  // namespace veritassync::sync
