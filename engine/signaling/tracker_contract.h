#pragma once

#include "engine/common/protocol.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace veritassync::signaling {

enum class Topology { kOneWay, kBidirectional };
enum class MessageKind { kOffer, kAnswer, kIceCandidate, kIceRestart };

struct JoinRequest {
  std::string task_id;
  std::string sync_key;
  std::string device_id;
  std::string device_fingerprint;
  protocol::Role role;
  std::string authorization_token;
};
struct RelayMessage {
  MessageKind kind;
  std::string sender_device_id;
  std::string recipient_device_id;
  std::string payload;
  std::string candidate_mid;
  std::int32_t candidate_mline_index = -1;
};

// The engine depends on this relay boundary; a WSS Tracker client can replace the
// in-memory contract model without leaking its protocol into transport code.
class SignalingRelay {
 public:
  virtual ~SignalingRelay() = default;
  virtual void Forward(const RelayMessage& message) = 0;
  [[nodiscard]] virtual std::vector<RelayMessage> DrainInbox(const std::string& device_id) = 0;
};

// Contract model used by the engine tests. It validates the same admission and
// forwarding rules required from the future Tracker network service.
class TrackerRoom final : public SignalingRelay {
 public:
  TrackerRoom(std::string task_id, Topology topology);
  void Join(const JoinRequest& request);
  void Forward(const RelayMessage& message) override;
  [[nodiscard]] std::vector<std::string> Members() const;
  [[nodiscard]] std::vector<RelayMessage> DrainInbox(const std::string& device_id) override;

 private:
  [[nodiscard]] bool HasMember(const std::string& device_id) const;
  std::string task_id_;
  Topology topology_;
  std::unordered_map<std::string, JoinRequest> members_;
  std::unordered_map<std::string, std::vector<RelayMessage>> inboxes_;
};

}  // namespace veritassync::signaling
