#include "engine/sync/phase0_node.h"

#include <algorithm>
#include <stdexcept>

namespace veritassync::sync {
Phase0Node::Phase0Node(NodeConfig config, transport::Transport& transport, protocol::Manifest local_manifest)
    : config_(std::move(config)), transport_(transport), local_manifest_(std::move(local_manifest)) {
  transport_.SetReceiveCallback([this](protocol::Channel channel, std::vector<std::uint8_t> wire) { Receive(channel, std::move(wire)); });
}
void Phase0Node::Start() {
  if (started_) return;
  started_ = true;
  Send(protocol::Channel::kControl, protocol::FrameType::kHello, protocol::EncodeHello({config_.task_id, config_.role, config_.device_id, config_.device_fingerprint, config_.authorization_digest}));
}
void Phase0Node::SendFakeBlock(std::vector<std::uint8_t> bytes) {
  if (!HandshakeComplete()) throw std::logic_error("cannot send block before HELLO completes");
  protocol::Chunk chunk{};
  std::fill(chunk.transfer_id.begin(), chunk.transfer_id.end(), 0xA1);
  std::fill(chunk.file_hash.begin(), chunk.file_hash.end(), 0xB2);
  chunk.offset = 0;
  chunk.bytes = std::move(bytes);
  chunk.chunk_hash = protocol::TestHash(chunk.bytes);
  Send(protocol::Channel::kBulk, protocol::FrameType::kChunk, protocol::EncodeChunk(chunk));
}
bool Phase0Node::HandshakeComplete() const { return started_ && received_hello_ && !last_error_.has_value(); }
const std::optional<protocol::Manifest>& Phase0Node::ReceivedManifest() const { return received_manifest_; }
const std::vector<protocol::Chunk>& Phase0Node::ReceivedChunks() const { return received_chunks_; }
const std::optional<std::string>& Phase0Node::LastError() const { return last_error_; }
bool Phase0Node::RolesCompatible(protocol::Role peer_role) const {
  if (config_.role == protocol::Role::kPeer || peer_role == protocol::Role::kPeer) return config_.role == protocol::Role::kPeer && peer_role == protocol::Role::kPeer;
  return (config_.role == protocol::Role::kSource && peer_role == protocol::Role::kTarget) || (config_.role == protocol::Role::kTarget && peer_role == protocol::Role::kSource);
}
void Phase0Node::Send(protocol::Channel channel, protocol::FrameType type, std::vector<std::uint8_t> payload) { transport_.Send(channel, protocol::EncodeFrame({type, next_request_id_++, std::move(payload)})); }
void Phase0Node::SendManifest() { if (!manifest_sent_) { manifest_sent_ = true; Send(protocol::Channel::kControl, protocol::FrameType::kManifest, protocol::EncodeManifest(local_manifest_)); } }
void Phase0Node::Receive(protocol::Channel channel, std::vector<std::uint8_t> wire) {
  try {
    const auto frame = protocol::DecodeFrameView(wire);
    if (!protocol::IsAllowedOn(channel, frame.type)) throw std::invalid_argument("wrong_channel");
    if (frame.type == protocol::FrameType::kHello) {
      const auto hello = protocol::DecodeHello(frame.payload);
      if (hello.task_id != config_.task_id) throw std::invalid_argument("task_mismatch");
      if (hello.authorization_digest != config_.authorization_digest) throw std::invalid_argument("unauthorized_peer");
      if (!RolesCompatible(hello.role)) throw std::invalid_argument("role_mismatch");
      received_hello_ = true;
      SendManifest();
      return;
    }
    if (!HandshakeComplete()) throw std::invalid_argument("unauthorized_peer");
    if (frame.type == protocol::FrameType::kManifest) { received_manifest_ = protocol::DecodeManifest(frame.payload); return; }
    if (frame.type == protocol::FrameType::kChunk) { received_chunks_.push_back(protocol::DecodeChunk(frame.payload)); return; }
  } catch (const std::exception& error) { last_error_ = error.what(); }
}
}  // namespace veritassync::sync
