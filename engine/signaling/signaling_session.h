#pragma once

#include "engine/signaling/tracker_contract.h"
#include "engine/transport/peer_transport.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace veritassync::signaling {

// Bridges asynchronous libwebrtc callbacks to a serialized Tracker relay pump.
// Call Pump from the engine event loop after StartOffer and until the session closes.
class SignalingSession {
 public:
  SignalingSession(SignalingRelay& relay, std::string local_device_id, std::string remote_device_id,
                   transport::PeerTransport& transport);
  void StartOffer();
  void Pump();

 private:
  void Queue(MessageKind kind, std::string payload);
  void QueueIce(transport::PeerTransport::IceCandidate candidate);
  void ApplyMessage(const RelayMessage& message);
  void ApplyReadyCandidates();

  SignalingRelay& relay_;
  std::string local_device_id_;
  std::string remote_device_id_;
  transport::PeerTransport& transport_;
  std::mutex mutex_;
  std::vector<RelayMessage> outbound_;
  std::vector<transport::PeerTransport::IceCandidate> pending_ice_;
  std::optional<bool> remote_description_result_;
};

}  // namespace veritassync::signaling
