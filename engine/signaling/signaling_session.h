#pragma once

#include "engine/signaling/tracker_contract.h"
#include "engine/transport/peer_transport.h"

#include <mutex>
#include <memory>
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
  ~SignalingSession();
  void StartOffer();
  void RestartIce();
  void Pump();
  // A changed DTLS certificate means a new remote PeerConnection, not an ICE
  // restart. The owner must replace this peer before applying its description.
  [[nodiscard]] std::vector<RelayMessage> TakeResetMessages();

 private:
  void ApplyMessage(const RelayMessage& message);
  void ApplyReadyCandidates();

  SignalingRelay& relay_;
  std::string local_device_id_;
  std::string remote_device_id_;
  transport::PeerTransport& transport_;
  struct CallbackState;
  std::shared_ptr<CallbackState> callback_state_;
  std::vector<transport::PeerTransport::IceCandidate> pending_ice_;
  std::string remote_certificate_;
  std::vector<RelayMessage> reset_messages_;
};

}  // namespace veritassync::signaling
