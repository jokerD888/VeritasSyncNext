#include "engine/signaling/tracker_contract.h"

#include <algorithm>
#include <ranges>
#include <stdexcept>

namespace veritassync::signaling {
namespace {
constexpr std::size_t kMaxSignalPayload = 64U * 1024U;
}
TrackerRoom::TrackerRoom(std::string task_id, Topology topology) : task_id_(std::move(task_id)), topology_(topology) {
  if (task_id_.empty()) throw std::invalid_argument("tracker task id is required");
}
bool TrackerRoom::HasMember(const std::string& device_id) const { return members_.contains(device_id); }
void TrackerRoom::Join(const JoinRequest& request) {
  if (request.task_id != task_id_ || request.sync_key.empty() || request.device_id.empty() || request.device_fingerprint.empty() || request.authorization_token.empty()) throw std::invalid_argument("invalid tracker join");
  if (HasMember(request.device_id)) throw std::invalid_argument("device already joined");
  if (topology_ == Topology::kOneWay) {
    if (request.role != protocol::Role::kSource && request.role != protocol::Role::kTarget) throw std::invalid_argument("one-way rooms require source or target");
    const auto source_exists = std::ranges::any_of(members_, [](const auto& member) { return member.second.role == protocol::Role::kSource; });
    if (request.role == protocol::Role::kSource && source_exists) throw std::invalid_argument("one-way rooms have one source");
  } else {
    if (request.role != protocol::Role::kPeer) throw std::invalid_argument("bidirectional rooms require peer role");
    if (members_.size() >= 2) throw std::invalid_argument("bidirectional rooms have exactly two peers");
  }
  members_.emplace(request.device_id, request);
}
void TrackerRoom::Forward(const RelayMessage& message) {
  if (!HasMember(message.sender_device_id) || !HasMember(message.recipient_device_id) || message.sender_device_id == message.recipient_device_id) throw std::invalid_argument("relay members are invalid");
  if (message.payload.empty() || message.payload.size() > kMaxSignalPayload) throw std::invalid_argument("signal payload is invalid");
  inboxes_[message.recipient_device_id].push_back(message);
}
std::vector<std::string> TrackerRoom::Members() const { std::vector<std::string> result; result.reserve(members_.size()); for (const auto& member : members_) result.push_back(member.first); std::ranges::sort(result); return result; }
std::vector<RelayMessage> TrackerRoom::DrainInbox(const std::string& device_id) { if (!HasMember(device_id)) throw std::invalid_argument("unknown device"); auto messages = std::move(inboxes_[device_id]); inboxes_[device_id].clear(); return messages; }
}  // namespace veritassync::signaling
