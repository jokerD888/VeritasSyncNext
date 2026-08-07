#include "engine/sync/bidirectional_sync.h"

#include "engine/common/content_hash.h"
#include "engine/common/uuid.h"
#include "engine/storage/ignore_rules.h"
#include "engine/storage/manifest_scanner.h"
#include "engine/sync/download_receiver.h"
#include "engine/sync/upload_session.h"
#include "engine/sync/version_resolution.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace veritassync::sync {
namespace {
constexpr std::size_t kMaxPendingUploadBytes = 16U * 1024U * 1024U;
constexpr std::size_t kResumeBelowBytes = 1U * 1024U * 1024U;
constexpr std::size_t kPersistBatchBytes = 8U * 1024U * 1024U;

[[nodiscard]] std::int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
[[nodiscard]] char HexDigit(const std::uint8_t value) { return "0123456789abcdef"[value & 0x0fU]; }
[[nodiscard]] std::string EncodeHash(const std::span<const std::uint8_t> hash) {
  if (hash.size() != common::ContentHash{}.size()) throw std::invalid_argument("content hash length is invalid");
  std::string value(hash.size() * 2U, '0');
  for (std::size_t index = 0; index < hash.size(); ++index) {
    value[index * 2U] = HexDigit(static_cast<std::uint8_t>(hash[index] >> 4U));
    value[index * 2U + 1U] = HexDigit(hash[index]);
  }
  return value;
}
[[nodiscard]] std::uint8_t DecodeHexDigit(const char value) {
  if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
  if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
  if (value >= 'A' && value <= 'F') return static_cast<std::uint8_t>(value - 'A' + 10);
  throw std::invalid_argument("manifest content hash is not hexadecimal");
}
[[nodiscard]] common::ContentHash DecodeHash(const std::string_view value) {
  common::ContentHash hash{};
  if (value.size() != hash.size() * 2U) throw std::invalid_argument("manifest content hash length is invalid");
  for (std::size_t index = 0; index < hash.size(); ++index) {
    hash[index] = static_cast<std::uint8_t>((DecodeHexDigit(value[index * 2U]) << 4U) |
                                            DecodeHexDigit(value[index * 2U + 1U]));
  }
  return hash;
}
[[nodiscard]] std::uint64_t ChunkCount(const std::uint64_t size) {
  return size == 0 ? 0 : (size + protocol::kLogicalChunkSize - 1U) / protocol::kLogicalChunkSize;
}
[[nodiscard]] bool IsConflictPath(const std::string_view path) {
  for (const auto& component : std::filesystem::path(path)) {
    if (component.string().find(".conflict.") != std::string::npos) return true;
  }
  return false;
}
[[nodiscard]] protocol::VersionedEntryKind ToProtocolKind(const storage::FileKind kind) {
  switch (kind) {
    case storage::FileKind::kFile: return protocol::VersionedEntryKind::kFile;
    case storage::FileKind::kDirectory: return protocol::VersionedEntryKind::kDirectory;
    case storage::FileKind::kTombstone: return protocol::VersionedEntryKind::kTombstone;
  }
  throw std::invalid_argument("unknown local file kind");
}
[[nodiscard]] storage::FileKind ToStorageKind(const protocol::VersionedEntryKind kind) {
  switch (kind) {
    case protocol::VersionedEntryKind::kFile: return storage::FileKind::kFile;
    case protocol::VersionedEntryKind::kDirectory: return storage::FileKind::kDirectory;
    case protocol::VersionedEntryKind::kTombstone: return storage::FileKind::kTombstone;
  }
  throw std::invalid_argument("unknown remote file kind");
}
[[nodiscard]] storage::FileRecord ToRecord(const std::string& task_id,
                                           const protocol::VersionedManifestEntry& entry) {
  storage::FileRecord record{task_id, entry.relative_path, ToStorageKind(entry.kind), entry.size, 0, {},
                             entry.version_id, entry.origin_device_id, entry.logical_clock, std::nullopt};
  if (entry.kind == protocol::VersionedEntryKind::kFile) {
    const auto hash = DecodeHash(entry.content_hash);
    record.content_hash.assign(hash.begin(), hash.end());
  }
  if (entry.deleted_at_ms.has_value()) {
    if (*entry.deleted_at_ms > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
      throw std::invalid_argument("tombstone timestamp is too large");
    }
    record.deleted_at_ms = static_cast<std::int64_t>(*entry.deleted_at_ms);
  }
  return record;
}
[[nodiscard]] bool SameSnapshot(const storage::SnapshotEntry& entry, const storage::FileRecord& record) {
  const auto kind = entry.kind == storage::SnapshotKind::kFile ? storage::FileKind::kFile : storage::FileKind::kDirectory;
  if (kind != record.kind || entry.size != record.size) return false;
  if (kind == storage::FileKind::kDirectory) return true;
  return entry.content_hash.has_value() && record.content_hash.size() == entry.content_hash->size() &&
         std::equal(entry.content_hash->begin(), entry.content_hash->end(), record.content_hash.begin());
}
}

struct BidirectionalSyncNode::SourceFile { std::filesystem::path absolute_path; common::ContentHash hash; };
struct BidirectionalSyncNode::PendingUpload { storage::TransferId transfer_id; std::unique_ptr<UploadSession> session; };
struct BidirectionalSyncNode::ActiveDownload {
  protocol::VersionedManifestEntry entry;
  std::string destination_path;
  bool apply_formal_record = false;
  common::ContentHash hash;
  storage::TransferId transfer_id;
  std::uint64_t chunk_count = 0;
  std::unique_ptr<DownloadReceiver> receiver;
  std::size_t durable_chunks = 0;
  std::size_t dirty_bytes = 0;
  std::vector<std::uint64_t> dirty_chunks;
};

BidirectionalSyncNode::BidirectionalSyncNode(BidirectionalSyncConfig config, transport::Transport& transport)
    : config_(std::move(config)), transport_(transport), writer_(config_.task_root) {
  if (config_.task_id.empty() || config_.device_id.empty() || config_.peer_device_id.empty() ||
      config_.device_fingerprint.empty() || config_.peer_device_fingerprint.empty() ||
      config_.authorization_digest.empty()) throw std::invalid_argument("bidirectional identity is incomplete");
  transport_.SetReceiveCallback([this](const protocol::Channel channel, std::vector<std::uint8_t> wire) {
    Receive(channel, std::move(wire));
  });
}
BidirectionalSyncNode::~BidirectionalSyncNode() { transport_.SetReceiveCallback({}); }

void BidirectionalSyncNode::Start() {
  std::scoped_lock lock(mutex_);
  if (started_) return;
  started_ = true;
  RefreshLocal();
  Send(protocol::Channel::kControl, protocol::FrameType::kHello,
       protocol::EncodeHello({config_.task_id, protocol::Role::kPeer, config_.device_id,
                              config_.device_fingerprint, config_.authorization_digest}));
}

void BidirectionalSyncNode::RefreshLocal() {
  std::scoped_lock lock(mutex_);
  storage::IgnoreRules rules;
  rules.LoadFile(config_.task_root);
  auto snapshot = storage::ManifestScanner(std::move(rules)).Scan(config_.task_root);
  snapshot.erase(std::remove_if(snapshot.begin(), snapshot.end(), [](const storage::SnapshotEntry& entry) {
    return IsConflictPath(entry.relative_path);
  }), snapshot.end());
  const auto known = config_.database.ListFileRecords(config_.task_id);
  std::size_t known_index = 0;
  for (const auto& entry : snapshot) {
    while (known_index < known.size() && known[known_index].relative_path < entry.relative_path) {
      ++known_index;
    }
    const auto* previous = known_index < known.size() &&
                                   known[known_index].relative_path == entry.relative_path
                               ? &known[known_index]
                               : nullptr;
    if (previous != nullptr && SameSnapshot(entry, *previous)) continue;
    const auto clock = config_.database.AdvanceLogicalClock(config_.task_id);
    storage::FileRecord record{config_.task_id, entry.relative_path,
                               entry.kind == storage::SnapshotKind::kFile ? storage::FileKind::kFile : storage::FileKind::kDirectory,
                               entry.size, entry.mtime_ns, {}, common::NewUuidV4(), config_.device_id, clock, std::nullopt};
    if (entry.content_hash.has_value()) record.content_hash.assign(entry.content_hash->begin(), entry.content_hash->end());
    config_.database.RecordVersionLineage({config_.task_id, record.version_id,
                                           previous == nullptr ? std::nullopt : std::optional<std::string>{previous->version_id}});
    config_.database.UpsertFileRecord(record);
  }
  std::size_t snapshot_index = 0;
  for (const auto& record : known) {
    while (snapshot_index < snapshot.size() &&
           snapshot[snapshot_index].relative_path < record.relative_path) {
      ++snapshot_index;
    }
    const bool exists = snapshot_index < snapshot.size() &&
                        snapshot[snapshot_index].relative_path == record.relative_path;
    if (record.kind == storage::FileKind::kTombstone || exists) continue;
    const auto clock = config_.database.AdvanceLogicalClock(config_.task_id);
    const auto version = common::NewUuidV4();
    config_.database.RecordVersionLineage({config_.task_id, version, record.version_id});
    config_.database.RecordTombstone(config_.task_id, record.relative_path, version, config_.device_id,
                                     clock, NowMilliseconds());
  }
  source_files_.clear();
  for (const auto& entry : snapshot) {
    if (entry.kind == storage::SnapshotKind::kFile && entry.content_hash.has_value()) {
      source_files_.push_back({config_.task_root / std::filesystem::path(entry.relative_path), *entry.content_hash});
    }
  }
  std::ranges::sort(source_files_, {}, &SourceFile::hash);
  if (received_hello_) SendManifest();
}

void BidirectionalSyncNode::Pump() {
  std::scoped_lock lock(mutex_);
  for (auto upload = uploads_.begin(); upload != uploads_.end();) {
    const auto next = upload->session->NextForTransport(transport_.BufferedAmount(protocol::Channel::kBulk));
    if (next.has_value()) transport_.Send(next->channel, std::move(next->wire));
    if (!upload->session->HasPending()) upload = uploads_.erase(upload); else ++upload;
  }
  for (auto& download : downloads_) {
    if (download.dirty_bytes >= kPersistBatchBytes && !download.dirty_chunks.empty()) {
      download.receiver->PersistAcceptedChunks(download.dirty_chunks, NowMilliseconds());
      download.durable_chunks += download.dirty_chunks.size(); download.dirty_chunks.clear(); download.dirty_bytes = 0;
    }
  }
}

bool BidirectionalSyncNode::HandshakeComplete() const { std::scoped_lock lock(mutex_); return started_ && received_hello_ && !last_error_.has_value(); }
bool BidirectionalSyncNode::IsConverged() const { std::scoped_lock lock(mutex_); return HandshakeComplete() && received_manifest_ && downloads_.empty(); }
std::optional<std::string> BidirectionalSyncNode::LastError() const { std::scoped_lock lock(mutex_); return last_error_; }
std::size_t BidirectionalSyncNode::PendingDownloadCount() const { std::scoped_lock lock(mutex_); return downloads_.size(); }

void BidirectionalSyncNode::Receive(const protocol::Channel channel, std::vector<std::uint8_t> wire) {
  std::scoped_lock lock(mutex_);
  try {
    const auto frame = protocol::DecodeFrameView(wire);
    if (!protocol::IsAllowedOn(channel, frame.type)) throw std::invalid_argument("wrong_channel");
    if (frame.type == protocol::FrameType::kHello) {
      const auto hello = protocol::DecodeHello(frame.payload);
      if (hello.task_id != config_.task_id || hello.role != protocol::Role::kPeer ||
          hello.device_id != config_.peer_device_id || hello.device_fingerprint != config_.peer_device_fingerprint ||
          hello.authorization_digest != config_.authorization_digest) throw std::invalid_argument("unauthorized_peer");
      const bool first_hello = !received_hello_;
      received_hello_ = true;
      if (first_hello) {
        Send(protocol::Channel::kControl, protocol::FrameType::kHello,
             protocol::EncodeHello({config_.task_id, protocol::Role::kPeer, config_.device_id,
                                    config_.device_fingerprint, config_.authorization_digest}));
        SendManifest();
      }
      return;
    }
    if (!started_ || !received_hello_) throw std::invalid_argument("unauthorized_peer");
    switch (frame.type) {
      case protocol::FrameType::kVersionManifest: ApplyManifest(protocol::DecodeVersionedManifest(frame.payload)); break;
      case protocol::FrameType::kFileRequest: HandleFileRequest(protocol::DecodeFileRequest(frame.payload)); break;
      case protocol::FrameType::kChunk: AcceptChunk(protocol::DecodeChunk(frame.payload)); break;
      case protocol::FrameType::kCancel: HandleCancel(protocol::DecodeCancel(frame.payload)); break;
      default: throw std::invalid_argument("unexpected_frame");
    }
  } catch (const std::exception& error) { last_error_ = error.what(); }
}

void BidirectionalSyncNode::Send(const protocol::Channel channel, const protocol::FrameType type,
                                 std::vector<std::uint8_t> payload) {
  transport_.Send(channel, protocol::EncodeFrame({type, next_request_id_++, std::move(payload)}));
}

void BidirectionalSyncNode::SendManifest() {
  protocol::VersionedManifest manifest{++manifest_revision_, {}};
  for (const auto& record : config_.database.ListFileRecords(config_.task_id)) {
    const auto lineage = config_.database.FindVersionLineage(config_.task_id, record.version_id);
    if (!lineage.has_value()) throw std::runtime_error("local record has no version lineage");
    manifest.entries.push_back({record.relative_path, ToProtocolKind(record.kind), record.size,
                                record.kind == storage::FileKind::kFile ? EncodeHash(std::span(record.content_hash)) : std::string{},
                                record.version_id, record.origin_device_id, record.logical_clock,
                                lineage->parent_version_id.value_or(""),
                                record.deleted_at_ms.has_value() ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(*record.deleted_at_ms)} : std::nullopt});
  }
  Send(protocol::Channel::kControl, protocol::FrameType::kVersionManifest,
       protocol::EncodeVersionedManifest(manifest));
}

void BidirectionalSyncNode::ApplyManifest(const protocol::VersionedManifest& manifest) {
  std::vector<std::string> paths;
  paths.reserve(manifest.entries.size());
  for (const auto& entry : manifest.entries) {
    (void)storage::ResolveTaskPath(config_.task_root, entry.relative_path);
    if (entry.kind == protocol::VersionedEntryKind::kFile) (void)DecodeHash(entry.content_hash);
    paths.push_back(entry.relative_path);
  }
  std::ranges::sort(paths);
  if (std::adjacent_find(paths.begin(), paths.end()) != paths.end()) throw std::invalid_argument("manifest contains duplicate paths");
  received_manifest_ = true;
  for (const auto& entry : manifest.entries) ApplyRemoteEntry(entry);
}

void BidirectionalSyncNode::ApplyRemoteEntry(const protocol::VersionedManifestEntry& entry) {
  const auto remote = ToRecord(config_.task_id, entry);
  config_.database.RecordVersionLineage({config_.task_id, remote.version_id,
                                         entry.parent_version_id.empty() ? std::nullopt : std::optional<std::string>{entry.parent_version_id}});
  (void)config_.database.AdvanceLogicalClock(config_.task_id, remote.logical_clock);
  const auto local = config_.database.FindFileRecord(config_.task_id, entry.relative_path);
  const auto resolution = VersionResolver::Resolve(config_.database, config_.task_id, local,
                                                    {remote, entry.parent_version_id.empty() ? std::nullopt : std::optional<std::string>{entry.parent_version_id}});
  if (resolution.action == VersionResolutionAction::kKeepLocal) return;
  if (resolution.action == VersionResolutionAction::kConflict) {
    const auto& winner = resolution.remote_wins ? remote : *local;
    config_.database.RecordConflict({common::NewUuidV4(), config_.task_id, entry.relative_path,
                                     winner.version_id, resolution.conflict_path, "unresolved", NowMilliseconds()});
    if (resolution.remote_wins && local->kind == storage::FileKind::kFile) {
      writer_.MoveFileToConflict(entry.relative_path, resolution.conflict_path);
      RelocateSourceFile(entry.relative_path, resolution.conflict_path);
    }
    if (!resolution.remote_wins) {
      if (entry.kind == protocol::VersionedEntryKind::kFile) BeginDownload(entry, resolution.conflict_path, false);
      return;
    }
  }
  if (entry.kind == protocol::VersionedEntryKind::kFile) {
    BeginDownload(entry, entry.relative_path, true);
  } else if (entry.kind == protocol::VersionedEntryKind::kDirectory) {
    const auto path = storage::ResolveTaskPath(config_.task_root, entry.relative_path);
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) && !error) writer_.RemoveFile(entry.relative_path);
    writer_.EnsureDirectory(entry.relative_path);
    UpsertRemoteRecord(entry);
  } else {
    const auto path = storage::ResolveTaskPath(config_.task_root, entry.relative_path);
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) && !error) writer_.RemoveFile(entry.relative_path);
    else if (std::filesystem::is_directory(path, error) && !error) writer_.RemoveEmptyDirectory(entry.relative_path);
    UpsertRemoteRecord(entry);
  }
}

void BidirectionalSyncNode::BeginDownload(const protocol::VersionedManifestEntry& entry,
                                          std::string destination_path, const bool apply_formal_record) {
  const auto hash = DecodeHash(entry.content_hash);
  const auto existing = std::ranges::find_if(downloads_, [&](const ActiveDownload& active) {
    return active.destination_path == destination_path && active.hash == hash;
  });
  if (existing != downloads_.end()) return;
  const auto formal_destination = storage::ResolveTaskPath(config_.task_root, destination_path);
  std::error_code error;
  if (std::filesystem::is_directory(formal_destination, error) && !error) writer_.RemoveEmptyDirectory(destination_path);
  if (entry.size == 0) {
    writer_.WriteAtomically(destination_path, {});
    if (apply_formal_record) UpsertRemoteRecord(entry);
    return;
  }
  const auto active = config_.database.FindActiveDownloadTransfer(config_.task_id, config_.peer_device_id,
                                                                   destination_path, std::span<const std::uint8_t>(hash));
  const auto transfer_id = active.has_value() ? active->transfer_id : common::NewTransferId();
  if (!active.has_value()) {
    writer_.DiscardPartial(destination_path);
    config_.database.CreateTransfer({transfer_id, config_.task_id, config_.peer_device_id, "download",
                                     {hash.begin(), hash.end()}, "active", NowMilliseconds(), NowMilliseconds(), destination_path});
  }
  const auto count = ChunkCount(entry.size);
  auto receiver = std::make_unique<DownloadReceiver>(config_.database, transfer_id, writer_, destination_path,
                                                     entry.size, hash, count);
  const auto request = receiver->ResumeRequest();
  if (request.missing_ranges.empty()) {
    receiver->Commit(NowMilliseconds());
    if (apply_formal_record) UpsertRemoteRecord(entry);
    return;
  }
  const auto durable = config_.database.CompletedTransferChunks(transfer_id).size();
  downloads_.push_back({entry, std::move(destination_path), apply_formal_record, hash, transfer_id, count,
                        std::move(receiver), durable});
  Send(protocol::Channel::kControl, protocol::FrameType::kFileRequest, protocol::EncodeFileRequest(request));
}

void BidirectionalSyncNode::AcceptChunk(const protocol::Chunk& chunk) {
  const auto download = std::ranges::find_if(downloads_, [&](const ActiveDownload& active) {
    return active.transfer_id == chunk.transfer_id && active.hash == chunk.file_hash;
  });
  if (download == downloads_.end()) throw std::invalid_argument("chunk does not belong to active download");
  const auto index = chunk.offset / protocol::kLogicalChunkSize;
  download->receiver->AcceptChunk(index, chunk.offset, chunk.bytes, chunk.chunk_hash, NowMilliseconds(), false);
  if (std::ranges::find(download->dirty_chunks, index) == download->dirty_chunks.end()) {
    download->dirty_chunks.push_back(index); download->dirty_bytes += chunk.bytes.size();
  }
  if (download->dirty_bytes >= kPersistBatchBytes || download->durable_chunks + download->dirty_chunks.size() == download->chunk_count) {
    download->receiver->PersistAcceptedChunks(download->dirty_chunks, NowMilliseconds());
    download->durable_chunks += download->dirty_chunks.size(); download->dirty_chunks.clear(); download->dirty_bytes = 0;
  }
  if (download->durable_chunks != download->chunk_count) return;
  CommitDownload(*download);
  downloads_.erase(download);
}

void BidirectionalSyncNode::CommitDownload(ActiveDownload& download) {
  download.receiver->Commit(NowMilliseconds());
  if (download.apply_formal_record) UpsertRemoteRecord(download.entry);
}

void BidirectionalSyncNode::HandleFileRequest(const protocol::FileRequest& request) {
  const auto source = std::ranges::lower_bound(source_files_, request.file_hash, {}, &SourceFile::hash);
  if (source == source_files_.end() || source->hash != request.file_hash) {
    Send(protocol::Channel::kControl, protocol::FrameType::kCancel,
         protocol::EncodeCancel({request.transfer_id, "source_missing"}));
    return;
  }
  auto session = std::make_unique<UploadSession>(ChunkSource(source->absolute_path, request.transfer_id, request.file_hash),
                                                 kMaxPendingUploadBytes, kResumeBelowBytes);
  try { session->QueueRequested(request); }
  catch (const std::exception&) {
    Send(protocol::Channel::kControl, protocol::FrameType::kCancel,
         protocol::EncodeCancel({request.transfer_id, "source_changed"}));
    return;
  }
  uploads_.push_back({request.transfer_id, std::move(session)});
}

void BidirectionalSyncNode::HandleCancel(const protocol::Cancel& cancel) {
  uploads_.erase(std::remove_if(uploads_.begin(), uploads_.end(), [&](const PendingUpload& upload) {
    return upload.transfer_id == cancel.transfer_id;
  }), uploads_.end());
  const auto download = std::ranges::find_if(downloads_, [&](const ActiveDownload& active) {
    return active.transfer_id == cancel.transfer_id;
  });
  if (download != downloads_.end()) { download->receiver->Cancel(cancel, NowMilliseconds()); downloads_.erase(download); }
}

void BidirectionalSyncNode::UpsertRemoteRecord(const protocol::VersionedManifestEntry& entry) {
  config_.database.UpsertFileRecord(ToRecord(config_.task_id, entry));
}

void BidirectionalSyncNode::RelocateSourceFile(const std::string_view old_path, const std::string_view new_path) {
  const auto old_absolute = config_.task_root / std::filesystem::path(old_path);
  for (auto& source : source_files_) {
    if (source.absolute_path == old_absolute) source.absolute_path = config_.task_root / std::filesystem::path(new_path);
  }
}

}  // namespace veritassync::sync
