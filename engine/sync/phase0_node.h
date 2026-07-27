#pragma once

#include "engine/common/protocol.h"
#include "engine/transport/mock_transport.h"

#include <optional>
#include <string>
#include <vector>

namespace veritassync::sync {

struct NodeConfig {
  std::string task_id;
  protocol::Role role;
  std::string device_id;
  std::string device_fingerprint;
  std::string authorization_digest;
};

class Phase0Node {
 public:
  Phase0Node(NodeConfig config, transport::Transport& transport, protocol::Manifest local_manifest);
  void Start();
  void SendFakeBlock(std::vector<std::uint8_t> bytes);
  [[nodiscard]] bool HandshakeComplete() const;
  [[nodiscard]] const std::optional<protocol::Manifest>& ReceivedManifest() const;
  [[nodiscard]] const std::vector<protocol::Chunk>& ReceivedChunks() const;
  [[nodiscard]] const std::optional<std::string>& LastError() const;

 private:
  void Receive(protocol::Channel channel, std::vector<std::uint8_t> wire);
  void Send(protocol::Channel channel, protocol::FrameType type, std::vector<std::uint8_t> payload);
  void SendManifest();
  [[nodiscard]] bool RolesCompatible(protocol::Role peer_role) const;

  NodeConfig config_;
  transport::Transport& transport_;
  protocol::Manifest local_manifest_;
  bool started_ = false;
  bool received_hello_ = false;
  bool manifest_sent_ = false;
  std::uint64_t next_request_id_ = 1;
  std::optional<protocol::Manifest> received_manifest_;
  std::vector<protocol::Chunk> received_chunks_;
  std::optional<std::string> last_error_;
};
}  // namespace veritassync::sync
