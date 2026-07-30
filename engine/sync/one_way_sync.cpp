#include "engine/sync/one_way_sync.h"

#include "engine/common/content_hash.h"
#include "engine/common/uuid.h"
#include "engine/storage/ignore_rules.h"
#include "engine/storage/manifest_scanner.h"
#include "engine/sync/download_receiver.h"
#include "engine/sync/upload_session.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace veritassync::sync {
namespace {

constexpr std::size_t kMaxPendingUploadBytes = 16U * 1024U * 1024U;
constexpr std::size_t kResumeBelowBytes = 1U * 1024U * 1024U;

[[nodiscard]] std::int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] char HexDigit(const std::uint8_t value) {
  return "0123456789abcdef"[value & 0x0fU];
}

[[nodiscard]] std::string EncodeHash(const common::ContentHash& hash) {
  std::string result(hash.size() * 2U, '0');
  for (std::size_t index = 0; index < hash.size(); ++index) {
    result[index * 2U] = HexDigit(static_cast<std::uint8_t>(hash[index] >> 4U));
    result[index * 2U + 1U] = HexDigit(hash[index]);
  }
  return result;
}

[[nodiscard]] std::uint8_t DecodeHexDigit(const char value) {
  if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
  if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
  if (value >= 'A' && value <= 'F') return static_cast<std::uint8_t>(value - 'A' + 10);
  throw std::invalid_argument("manifest content hash is not hexadecimal");
}

[[nodiscard]] common::ContentHash DecodeHash(const std::string_view value) {
  common::ContentHash result{};
  if (value.size() != result.size() * 2U) {
    throw std::invalid_argument("manifest content hash has invalid length");
  }
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<std::uint8_t>((DecodeHexDigit(value[index * 2U]) << 4U) |
                                              DecodeHexDigit(value[index * 2U + 1U]));
  }
  return result;
}

[[nodiscard]] std::uint64_t ChunkCount(const std::uint64_t size) {
  if (size == 0) return 0;
  return (size + protocol::kLogicalChunkSize - 1U) / protocol::kLogicalChunkSize;
}

}  // namespace

struct OneWaySyncNode::SourceFile {
  std::filesystem::path absolute_path;
  common::ContentHash hash;
};

struct OneWaySyncNode::ActiveDownload {
  protocol::ManifestEntry entry;
  common::ContentHash hash;
  storage::TransferId transfer_id;
  std::uint64_t chunk_count;
  std::unique_ptr<DownloadReceiver> receiver;
};

struct OneWaySyncNode::PendingUpload {
  storage::TransferId transfer_id;
  std::unique_ptr<UploadSession> session;
};

OneWaySyncNode::~OneWaySyncNode() = default;

OneWaySyncNode::OneWaySyncNode(OneWaySyncConfig config, transport::Transport& transport)
    : config_(std::move(config)), transport_(transport), writer_(config_.task_root) {
  if (config_.role != protocol::Role::kSource && config_.role != protocol::Role::kTarget) {
    throw std::invalid_argument("one-way node role must be source or target");
  }
  if (config_.task_id.empty() || config_.device_id.empty() || config_.peer_device_id.empty() ||
      config_.device_fingerprint.empty() || config_.authorization_digest.empty()) {
    throw std::invalid_argument("one-way node identity is incomplete");
  }
  transport_.SetReceiveCallback([this](const protocol::Channel channel,
                                       std::vector<std::uint8_t> wire) {
    Receive(channel, std::move(wire));
  });
}

void OneWaySyncNode::Start() {
  if (started_) return;
  started_ = true;
  if (config_.role == protocol::Role::kSource) RefreshSource();
  Send(protocol::Channel::kControl, protocol::FrameType::kHello,
       protocol::EncodeHello({config_.task_id, config_.role, config_.device_id,
                              config_.device_fingerprint, config_.authorization_digest}));
}

void OneWaySyncNode::RefreshSource() {
  if (config_.role != protocol::Role::kSource) {
    throw std::logic_error("only source nodes may scan local changes");
  }
  storage::IgnoreRules rules;
  rules.LoadFile(config_.task_root);
  storage::ManifestScanner scanner(std::move(rules));
  source_manifest_ = {++manifest_revision_, {}};
  source_files_.clear();
  for (const auto& entry : scanner.Scan(config_.task_root)) {
    if (entry.kind != storage::SnapshotKind::kFile || !entry.content_hash.has_value()) continue;
    source_manifest_.entries.push_back(
        {entry.relative_path, entry.size, EncodeHash(*entry.content_hash)});
    source_files_.push_back({config_.task_root / std::filesystem::path(entry.relative_path),
                             *entry.content_hash});
  }
  if (received_hello_) SendManifest();
}

void OneWaySyncNode::Pump() {
  if (config_.role != protocol::Role::kSource) return;
  for (auto it = uploads_.begin(); it != uploads_.end();) {
    const auto next = it->session->NextForTransport(
        transport_.BufferedAmount(protocol::Channel::kBulk));
    if (next.has_value()) {
      transport_.Send(next->channel, std::move(next->wire));
    }
    if (it->session->PendingBytes() == 0) it = uploads_.erase(it);
    else ++it;
  }
}

bool OneWaySyncNode::HandshakeComplete() const {
  return started_ && received_hello_ && !last_error_.has_value();
}

bool OneWaySyncNode::TargetIsConverged() const {
  return config_.role == protocol::Role::kTarget && HandshakeComplete() &&
         received_manifest_.has_value() && downloads_.empty();
}

std::size_t OneWaySyncNode::PendingDownloadCount() const { return downloads_.size(); }

const std::optional<std::string>& OneWaySyncNode::LastError() const { return last_error_; }

bool OneWaySyncNode::RolesCompatible(const protocol::Role peer_role) const {
  return (config_.role == protocol::Role::kSource && peer_role == protocol::Role::kTarget) ||
         (config_.role == protocol::Role::kTarget && peer_role == protocol::Role::kSource);
}

void OneWaySyncNode::Send(const protocol::Channel channel, const protocol::FrameType type,
                          std::vector<std::uint8_t> payload) {
  transport_.Send(channel, protocol::EncodeFrame({type, next_request_id_++, std::move(payload)}));
}

void OneWaySyncNode::SendManifest() {
  Send(protocol::Channel::kControl, protocol::FrameType::kManifest,
       protocol::EncodeManifest(source_manifest_));
}

void OneWaySyncNode::Receive(const protocol::Channel channel, std::vector<std::uint8_t> wire) {
  try {
    const auto frame = protocol::DecodeFrame(wire);
    if (!protocol::IsAllowedOn(channel, frame.type)) {
      throw std::invalid_argument("wrong_channel");
    }
    if (frame.type == protocol::FrameType::kHello) {
      const auto hello = protocol::DecodeHello(frame.payload);
      if (hello.task_id != config_.task_id || hello.device_id != config_.peer_device_id ||
          hello.authorization_digest != config_.authorization_digest || !RolesCompatible(hello.role)) {
        throw std::invalid_argument("unauthorized_peer");
      }
      received_hello_ = true;
      if (config_.role == protocol::Role::kSource) {
        Send(protocol::Channel::kControl, protocol::FrameType::kHello,
             protocol::EncodeHello({config_.task_id, config_.role, config_.device_id,
                                    config_.device_fingerprint, config_.authorization_digest}));
        SendManifest();
      }
      return;
    }
    if (!started_ || !received_hello_) throw std::invalid_argument("unauthorized_peer");
    switch (frame.type) {
      case protocol::FrameType::kManifest:
        if (config_.role != protocol::Role::kTarget) throw std::invalid_argument("unexpected_manifest");
        ApplyManifest(protocol::DecodeManifest(frame.payload));
        break;
      case protocol::FrameType::kFileRequest:
        if (config_.role != protocol::Role::kSource) throw std::invalid_argument("unexpected_file_request");
        HandleFileRequest(protocol::DecodeFileRequest(frame.payload));
        break;
      case protocol::FrameType::kChunk:
        if (config_.role != protocol::Role::kTarget) throw std::invalid_argument("unexpected_chunk");
        AcceptChunk(protocol::DecodeChunk(frame.payload));
        break;
      case protocol::FrameType::kCancel:
        HandleCancel(protocol::DecodeCancel(frame.payload));
        break;
      default:
        throw std::invalid_argument("unexpected_frame");
    }
  } catch (const std::exception& error) {
    last_error_ = error.what();
  }
}

void OneWaySyncNode::ApplyManifest(const protocol::Manifest& manifest) {
  std::vector<std::string> paths;
  paths.reserve(manifest.entries.size());
  for (const auto& entry : manifest.entries) {
    (void)storage::ResolveTaskPath(config_.task_root, entry.relative_path);
    (void)DecodeHash(entry.content_hash);
    paths.push_back(entry.relative_path);
  }
  std::ranges::sort(paths);
  if (std::adjacent_find(paths.begin(), paths.end()) != paths.end()) {
    throw std::invalid_argument("manifest contains duplicate paths");
  }
  last_error_.reset();
  received_manifest_ = manifest;
  DeleteFilesAbsentFrom(manifest);
  for (const auto& entry : manifest.entries) {
    if (!TargetAlreadyHas(entry)) BeginDownload(entry);
  }
}

void OneWaySyncNode::DeleteFilesAbsentFrom(const protocol::Manifest& manifest) {
  std::vector<std::string> source_paths;
  source_paths.reserve(manifest.entries.size());
  for (const auto& entry : manifest.entries) source_paths.push_back(entry.relative_path);
  std::ranges::sort(source_paths);
  for (const auto& record : config_.database.ListFileRecords(config_.task_id)) {
    if (record.kind != storage::FileKind::kFile ||
        std::binary_search(source_paths.begin(), source_paths.end(), record.relative_path)) {
      continue;
    }
    writer_.RemoveFile(record.relative_path);
    config_.database.RecordTombstone(config_.task_id, record.relative_path, common::NewUuidV4(),
                                     config_.peer_device_id, 0, NowMilliseconds());
  }
}

bool OneWaySyncNode::TargetAlreadyHas(const protocol::ManifestEntry& entry) const {
  const auto hash = DecodeHash(entry.content_hash);
  const auto record = config_.database.FindFileRecord(config_.task_id, entry.relative_path);
  if (!record.has_value() || record->kind != storage::FileKind::kFile || record->size != entry.size ||
      record->content_hash != std::vector<std::uint8_t>(hash.begin(), hash.end())) {
    return false;
  }
  const auto path = storage::ResolveTaskPath(config_.task_root, entry.relative_path);
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error && common::Blake3File(path) == hash;
}

void OneWaySyncNode::BeginDownload(const protocol::ManifestEntry& entry) {
  if (entry.size == 0) {
    const std::vector<std::uint8_t> empty;
    writer_.WriteAtomically(entry.relative_path, empty);
    const auto hash = DecodeHash(entry.content_hash);
    config_.database.UpsertFileRecord({config_.task_id, entry.relative_path, storage::FileKind::kFile,
                                        0, 0, {hash.begin(), hash.end()}, common::NewUuidV4(),
                                        config_.peer_device_id, 0, std::nullopt});
    return;
  }
  const auto hash = DecodeHash(entry.content_hash);
  const auto existing = std::ranges::find_if(downloads_, [&](const ActiveDownload& download) {
    return download.entry.relative_path == entry.relative_path && download.hash == hash;
  });
  if (existing != downloads_.end()) return;
  const auto active = config_.database.FindActiveDownloadTransfer(
      config_.task_id, config_.peer_device_id, entry.relative_path,
      std::span<const std::uint8_t>(hash));
  const auto transfer_id = active.has_value() ? active->transfer_id : common::NewTransferId();
  if (!active.has_value()) {
    config_.database.CreateTransfer({transfer_id, config_.task_id, config_.peer_device_id, "download",
                                     {hash.begin(), hash.end()}, "active", NowMilliseconds(),
                                     NowMilliseconds(), entry.relative_path});
  }
  const auto chunk_count = ChunkCount(entry.size);
  auto receiver = std::make_unique<DownloadReceiver>(config_.database, transfer_id, writer_,
                                                     entry.relative_path, entry.size, hash, chunk_count);
  const auto request = receiver->ResumeRequest();
  if (request.missing_ranges.empty()) {
    receiver->Commit(NowMilliseconds());
    config_.database.UpsertFileRecord({config_.task_id, entry.relative_path,
                                        storage::FileKind::kFile, entry.size, 0,
                                        {hash.begin(), hash.end()}, common::NewUuidV4(),
                                        config_.peer_device_id, 0, std::nullopt});
    return;
  }
  downloads_.push_back({entry, hash, transfer_id, chunk_count, std::move(receiver)});
  Send(protocol::Channel::kControl, protocol::FrameType::kFileRequest,
       protocol::EncodeFileRequest(request));
}

void OneWaySyncNode::HandleFileRequest(const protocol::FileRequest& request) {
  const auto source = std::ranges::find_if(source_files_, [&](const SourceFile& file) {
    return file.hash == request.file_hash;
  });
  if (source == source_files_.end()) {
    Send(protocol::Channel::kControl, protocol::FrameType::kCancel,
         protocol::EncodeCancel({request.transfer_id, "source_missing"}));
    return;
  }
  uploads_.erase(std::remove_if(uploads_.begin(), uploads_.end(), [&](const PendingUpload& upload) {
    return upload.transfer_id == request.transfer_id;
  }), uploads_.end());
  auto session = std::make_unique<UploadSession>(
      ChunkSource(source->absolute_path, request.transfer_id, request.file_hash),
      kMaxPendingUploadBytes, kResumeBelowBytes);
  try {
    session->QueueRequested(request);
  } catch (const std::exception&) {
    Send(protocol::Channel::kControl, protocol::FrameType::kCancel,
         protocol::EncodeCancel({request.transfer_id, "source_changed"}));
    return;
  }
  uploads_.push_back({request.transfer_id, std::move(session)});
}

void OneWaySyncNode::AcceptChunk(const protocol::Chunk& chunk) {
  const auto download = std::ranges::find_if(downloads_, [&](const ActiveDownload& active) {
    return active.transfer_id == chunk.transfer_id && active.hash == chunk.file_hash;
  });
  if (download == downloads_.end()) throw std::invalid_argument("chunk does not belong to an active download");
  const auto index = chunk.offset / protocol::kLogicalChunkSize;
  download->receiver->AcceptChunk(index, chunk.offset, chunk.bytes, chunk.chunk_hash, NowMilliseconds());
  if (config_.database.CompletedTransferChunks(download->transfer_id).size() != download->chunk_count) return;
  CommitDownload(*download);
  downloads_.erase(download);
}

void OneWaySyncNode::CommitDownload(ActiveDownload& download) {
  download.receiver->Commit(NowMilliseconds());
  config_.database.UpsertFileRecord({config_.task_id, download.entry.relative_path,
                                      storage::FileKind::kFile, download.entry.size, 0,
                                      {download.hash.begin(), download.hash.end()}, common::NewUuidV4(),
                                      config_.peer_device_id, 0, std::nullopt});
}

void OneWaySyncNode::HandleCancel(const protocol::Cancel& cancel) {
  if (config_.role == protocol::Role::kTarget) {
    const auto download = std::ranges::find_if(downloads_, [&](const ActiveDownload& active) {
      return active.transfer_id == cancel.transfer_id;
    });
    if (download == downloads_.end()) throw std::invalid_argument("cancel does not match an active download");
    download->receiver->Cancel(cancel, NowMilliseconds());
    downloads_.erase(download);
    last_error_ = cancel.reason;
    return;
  }
  uploads_.erase(std::remove_if(uploads_.begin(), uploads_.end(), [&](const PendingUpload& upload) {
    return upload.transfer_id == cancel.transfer_id;
  }), uploads_.end());
}

}  // namespace veritassync::sync
