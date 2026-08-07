#include "engine/sync/multi_target_source.h"

#include "engine/common/content_hash.h"
#include "engine/common/uuid.h"
#include "engine/storage/ignore_rules.h"
#include "engine/storage/manifest_scanner.h"
#include "engine/sync/chunk_source.h"
#include "engine/sync/snapshot_reconciler.h"
#include "engine/sync/upload_session.h"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <stdexcept>
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

}  // namespace

struct MultiTargetSource::SourceFile {
  std::filesystem::path absolute_path;
  common::ContentHash hash;
};

struct MultiTargetSource::PendingUpload {
  storage::TransferId transfer_id;
  std::unique_ptr<UploadSession> session;
};

struct MultiTargetSource::Peer {
  MultiTargetPeerConfig config;
  transport::Transport* transport = nullptr;
  bool received_hello = false;
  std::uint64_t next_request_id = 1;
  std::vector<PendingUpload> uploads;
  std::optional<std::string> last_error;
  MultiTargetPeerStatistics statistics;
};

MultiTargetSource::MultiTargetSource(MultiTargetSourceConfig config) : config_(std::move(config)) {
  if (config_.task_id.empty() || config_.device_id.empty() || config_.device_fingerprint.empty()) {
    throw std::invalid_argument("multi-target source identity is incomplete");
  }
}

MultiTargetSource::~MultiTargetSource() {
  std::scoped_lock lock(mutex_);
  for (auto& peer : peers_) peer->transport->SetReceiveCallback({});
}

void MultiTargetSource::AddTarget(MultiTargetPeerConfig config, transport::Transport& transport) {
  std::scoped_lock lock(mutex_);
  if (started_) throw std::logic_error("targets must be added before the source starts");
  if (config.device_id.empty() || config.device_fingerprint.empty() ||
      config.authorization_digest.empty() || FindPeer(config.device_id) != nullptr) {
    throw std::invalid_argument("target identity is invalid or duplicate");
  }
  auto peer = std::make_unique<Peer>();
  peer->config = std::move(config);
  peer->transport = &transport;
  auto* peer_pointer = peer.get();
  transport.SetReceiveCallback([this, peer_pointer](const protocol::Channel channel,
                                                     std::vector<std::uint8_t> wire) {
    Receive(*peer_pointer, channel, std::move(wire));
  });
  peers_.push_back(std::move(peer));
}

void MultiTargetSource::Start() {
  std::scoped_lock lock(mutex_);
  if (started_) return;
  if (peers_.empty()) throw std::logic_error("multi-target source requires at least one target");
  started_ = true;
  RefreshSource();
  for (auto& peer : peers_) SendHello(*peer);
}

void MultiTargetSource::RefreshSource() {
  std::scoped_lock lock(mutex_);
  storage::IgnoreRules rules;
  rules.LoadFile(config_.task_root);
  const auto snapshot = storage::ManifestScanner(std::move(rules)).Scan(config_.task_root);
  ++snapshot_scan_count_;
  [[maybe_unused]] const auto reconciled = SnapshotReconciler(common::NewUuidV4).Apply(
      config_.database, snapshot,
      {config_.task_id, config_.device_id, manifest_revision_ + 1U, NowMilliseconds()});

  source_manifest_ = {++manifest_revision_, {}};
  source_files_.clear();
  for (const auto& entry : snapshot) {
    if (entry.kind == storage::SnapshotKind::kDirectory) {
      source_manifest_.entries.push_back({entry.relative_path, 0, {}});
      continue;
    }
    if (!entry.content_hash.has_value()) throw std::logic_error("source file snapshot has no hash");
    source_manifest_.entries.push_back({entry.relative_path, entry.size, EncodeHash(*entry.content_hash)});
    source_files_.push_back({config_.task_root / std::filesystem::path(entry.relative_path),
                             *entry.content_hash});
  }
  std::ranges::sort(source_files_, {}, &SourceFile::hash);
  for (auto& peer : peers_) {
    if (peer->received_hello) SendManifest(*peer);
  }
}

void MultiTargetSource::Pump() {
  std::scoped_lock lock(mutex_);
  for (auto& peer : peers_) {
    for (auto upload = peer->uploads.begin(); upload != peer->uploads.end();) {
      const auto next = upload->session->NextForTransport(
          peer->transport->BufferedAmount(protocol::Channel::kBulk));
      if (next.has_value()) {
        ++peer->statistics.chunks_sent;
        peer->statistics.bulk_bytes_sent += next->wire.size();
        peer->transport->Send(next->channel, std::move(next->wire));
      } else if (upload->session->HasPending()) {
        ++peer->statistics.backpressure_pauses;
      }
      if (!upload->session->HasPending()) upload = peer->uploads.erase(upload);
      else ++upload;
    }
  }
}

std::size_t MultiTargetSource::TargetCount() const {
  std::scoped_lock lock(mutex_);
  return peers_.size();
}

std::size_t MultiTargetSource::SnapshotScanCount() const {
  std::scoped_lock lock(mutex_);
  return snapshot_scan_count_;
}

bool MultiTargetSource::HandshakeComplete(const std::string& target_device_id) const {
  std::scoped_lock lock(mutex_);
  const auto* peer = FindPeer(target_device_id);
  return peer != nullptr && started_ && peer->received_hello && !peer->last_error.has_value();
}

std::optional<MultiTargetPeerStatistics> MultiTargetSource::Statistics(
    const std::string& target_device_id) const {
  std::scoped_lock lock(mutex_);
  const auto* peer = FindPeer(target_device_id);
  if (peer == nullptr) return std::nullopt;
  return peer->statistics;
}

std::optional<std::string> MultiTargetSource::LastError(const std::string& target_device_id) const {
  std::scoped_lock lock(mutex_);
  const auto* peer = FindPeer(target_device_id);
  if (peer == nullptr) return std::nullopt;
  return peer->last_error;
}

void MultiTargetSource::Receive(Peer& peer, const protocol::Channel channel,
                                std::vector<std::uint8_t> wire) {
  std::scoped_lock lock(mutex_);
  try {
    if (channel == protocol::Channel::kControl) peer.statistics.control_bytes_received += wire.size();
    const auto frame = protocol::DecodeFrameView(wire);
    if (!protocol::IsAllowedOn(channel, frame.type)) throw std::invalid_argument("wrong_channel");
    if (frame.type == protocol::FrameType::kHello) {
      const auto hello = protocol::DecodeHello(frame.payload);
      if (hello.task_id != config_.task_id || hello.device_id != peer.config.device_id ||
          hello.device_fingerprint != peer.config.device_fingerprint ||
          hello.authorization_digest != peer.config.authorization_digest ||
          hello.role != protocol::Role::kTarget) {
        throw std::invalid_argument("unauthorized_peer");
      }
      peer.received_hello = true;
      SendHello(peer);
      SendManifest(peer);
      return;
    }
    if (!started_ || !peer.received_hello) throw std::invalid_argument("unauthorized_peer");
    switch (frame.type) {
      case protocol::FrameType::kFileRequest:
        HandleFileRequest(peer, protocol::DecodeFileRequest(frame.payload));
        break;
      case protocol::FrameType::kCancel:
        HandleCancel(peer, protocol::DecodeCancel(frame.payload));
        break;
      default:
        throw std::invalid_argument("target_write_forbidden");
    }
  } catch (const std::exception& error) {
    peer.last_error = error.what();
  }
}

void MultiTargetSource::Send(Peer& peer, const protocol::Channel channel,
                             const protocol::FrameType type, std::vector<std::uint8_t> payload) {
  auto wire = protocol::EncodeFrame({type, peer.next_request_id++, std::move(payload)});
  if (channel == protocol::Channel::kControl) peer.statistics.control_bytes_sent += wire.size();
  else peer.statistics.bulk_bytes_sent += wire.size();
  peer.transport->Send(channel, std::move(wire));
}

void MultiTargetSource::SendHello(Peer& peer) {
  Send(peer, protocol::Channel::kControl, protocol::FrameType::kHello,
       protocol::EncodeHello({config_.task_id, protocol::Role::kSource, config_.device_id,
                              config_.device_fingerprint, peer.config.authorization_digest}));
}

void MultiTargetSource::SendManifest(Peer& peer) {
  Send(peer, protocol::Channel::kControl, protocol::FrameType::kManifest,
       protocol::EncodeManifest(source_manifest_));
}

void MultiTargetSource::HandleFileRequest(Peer& peer, const protocol::FileRequest& request) {
  const auto source = std::ranges::lower_bound(source_files_, request.file_hash, {}, &SourceFile::hash);
  if (source == source_files_.end() || source->hash != request.file_hash) {
    Send(peer, protocol::Channel::kControl, protocol::FrameType::kCancel,
         protocol::EncodeCancel({request.transfer_id, "source_missing"}));
    return;
  }
  peer.uploads.erase(std::remove_if(peer.uploads.begin(), peer.uploads.end(),
                                    [&](const PendingUpload& upload) {
                                      return upload.transfer_id == request.transfer_id;
                                    }),
                     peer.uploads.end());
  auto session = std::make_unique<UploadSession>(
      ChunkSource(source->absolute_path, request.transfer_id, request.file_hash),
      kMaxPendingUploadBytes, kResumeBelowBytes);
  try {
    session->QueueRequested(request);
  } catch (const std::exception&) {
    Send(peer, protocol::Channel::kControl, protocol::FrameType::kCancel,
         protocol::EncodeCancel({request.transfer_id, "source_changed"}));
    return;
  }
  peer.uploads.push_back({request.transfer_id, std::move(session)});
}

void MultiTargetSource::HandleCancel(Peer& peer, const protocol::Cancel& cancel) {
  peer.uploads.erase(std::remove_if(peer.uploads.begin(), peer.uploads.end(),
                                    [&](const PendingUpload& upload) {
                                      return upload.transfer_id == cancel.transfer_id;
                                    }),
                     peer.uploads.end());
}

MultiTargetSource::Peer* MultiTargetSource::FindPeer(const std::string& target_device_id) {
  const auto peer = std::ranges::find_if(peers_, [&](const std::unique_ptr<Peer>& candidate) {
    return candidate->config.device_id == target_device_id;
  });
  return peer == peers_.end() ? nullptr : peer->get();
}

const MultiTargetSource::Peer* MultiTargetSource::FindPeer(const std::string& target_device_id) const {
  const auto peer = std::ranges::find_if(peers_, [&](const std::unique_ptr<Peer>& candidate) {
    return candidate->config.device_id == target_device_id;
  });
  return peer == peers_.end() ? nullptr : peer->get();
}

}  // namespace veritassync::sync
