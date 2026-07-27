#pragma once

#include "engine/common/protocol.h"

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
};

// Contract model used by the engine and tracker implementations. The Phase 1 test
// double validates the same admission and forwarding rules as the network service.
class TrackerRoom {
 public:
  TrackerRoom(std::string task_id, Topology topology);
  void Join(const JoinRequest& request);
  void Forward(const RelayMessage& message);
  [[nodiscard]] std::vector<std::string> Members() const;
  [[nodiscard]] std::vector<RelayMessage> DrainInbox(const std::string& device_id);

 private:
  [[nodiscard]] bool HasMember(const std::string& device_id) const;
  std::string task_id_;
  Topology topology_;
  std::unordered_map<std::string, JoinRequest> members_;
  std::unordered_map<std::string, std::vector<RelayMessage>> inboxes_;
};

}  // namespace veritassync::signaling
