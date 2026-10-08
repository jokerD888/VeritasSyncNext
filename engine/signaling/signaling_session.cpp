#include "engine/signaling/signaling_session.h"

#include <stdexcept>
#include <utility>

namespace veritassync::signaling {

struct SignalingSession::CallbackState {
  std::mutex mutex;
  std::vector<RelayMessage> outbound;
  std::optional<bool> remote_description_result;
  bool restart_offer = false;
  std::string local_device_id, remote_device_id;
};

SignalingSession::SignalingSession(SignalingRelay& relay, std::string local_device_id,
                                   std::string remote_device_id,
                                   transport::PeerTransport& transport)
    : relay_(relay),
      local_device_id_(std::move(local_device_id)),
      remote_device_id_(std::move(remote_device_id)),
      transport_(transport), callback_state_(std::make_shared<CallbackState>()) {
  if (local_device_id_.empty() || remote_device_id_.empty() ||
      local_device_id_ == remote_device_id_) {
    throw std::invalid_argument("signaling session requires two distinct device ids");
  }
  const auto state = callback_state_;
  state->local_device_id = local_device_id_;
  state->remote_device_id = remote_device_id_;
  // A callback already copied by a native thread can finish after Set*Callback({}).
  // Capture independent shared state, never a soon-to-be-destroyed session pointer.
  transport_.SetOfferCallback(
      [state](std::string sdp) {
        if (sdp.empty()) return;
        std::scoped_lock lock(state->mutex);
        state->outbound.push_back({state->restart_offer ? MessageKind::kIceRestart : MessageKind::kOffer,
                                  state->local_device_id, state->remote_device_id, std::move(sdp)});
        state->restart_offer = false;
      });
  transport_.SetAnswerCallback(
      [state](std::string sdp) {
        if (sdp.empty()) return;
        std::scoped_lock lock(state->mutex);
        state->outbound.push_back({MessageKind::kAnswer, state->local_device_id,
                                  state->remote_device_id, std::move(sdp)});
      });
  transport_.SetIceCallback(
      [state](transport::PeerTransport::IceCandidate candidate) {
        if (candidate.mid.empty() || candidate.candidate.empty()) return;
        std::scoped_lock lock(state->mutex);
        state->outbound.push_back({MessageKind::kIceCandidate, state->local_device_id,
            state->remote_device_id, std::move(candidate.candidate), std::move(candidate.mid), candidate.mline_index});
      });
  transport_.SetRemoteDescriptionCallback([state](const bool success) {
    std::scoped_lock lock(state->mutex);
    state->remote_description_result = success;
  });
}

void SignalingSession::StartOffer() { transport_.CreateOffer(); }
SignalingSession::~SignalingSession() {
  transport_.SetOfferCallback({});
  transport_.SetAnswerCallback({});
  transport_.SetIceCallback({});
  transport_.SetRemoteDescriptionCallback({});
}
void SignalingSession::RestartIce() {
  {
    std::scoped_lock lock(callback_state_->mutex);
    callback_state_->restart_offer = true;
    pending_ice_.clear();
    callback_state_->remote_description_result.reset();
  }
  transport_.RestartIce();
}

void SignalingSession::Pump() {
  std::vector<RelayMessage> outbound;
  {
    std::scoped_lock lock(callback_state_->mutex);
    outbound.swap(callback_state_->outbound);
  }
  for (const auto& message : outbound) relay_.Forward(message);

  for (const auto& message : relay_.DrainInbox(local_device_id_)) {
    if (!reset_messages_.empty()) reset_messages_.push_back(message);
    else ApplyMessage(message);
  }
  if (reset_messages_.empty()) ApplyReadyCandidates();
}

std::vector<RelayMessage> SignalingSession::TakeResetMessages() {
  return std::exchange(reset_messages_, {});
}

void SignalingSession::ApplyMessage(const RelayMessage& message) {
  if (message.sender_device_id != remote_device_id_) {
    throw std::invalid_argument("signal sender does not match session peer");
  }
  switch (message.kind) {
    case MessageKind::kOffer:
    case MessageKind::kIceRestart:
    case MessageKind::kAnswer:
      if (message.payload.empty()) throw std::invalid_argument("empty session description");
      {
        const auto start = message.payload.find("a=fingerprint:");
        if (start != std::string::npos) {
          const auto end = message.payload.find_first_of("\r\n", start);
          const auto certificate = message.payload.substr(start, end - start);
          if (!remote_certificate_.empty() && remote_certificate_ != certificate) {
            reset_messages_.push_back(message);
            return;
          }
          remote_certificate_ = certificate;
        }
      }
      {
        std::scoped_lock lock(callback_state_->mutex);
        callback_state_->remote_description_result.reset();
      }
      if (message.kind != MessageKind::kAnswer)
        transport_.ApplyRemoteOffer(message.payload);
      else
        transport_.ApplyRemoteAnswer(message.payload);
      break;
    case MessageKind::kIceCandidate:
      if (message.payload.empty() || message.candidate_mid.empty() ||
          message.candidate_mline_index < -1) {
        throw std::invalid_argument("invalid ICE candidate relay message");
      }
      {
        std::scoped_lock lock(callback_state_->mutex);
        pending_ice_.push_back(
            {message.candidate_mid, message.candidate_mline_index, message.payload});
      }
      break;
  }
}

void SignalingSession::ApplyReadyCandidates() {
  std::vector<transport::PeerTransport::IceCandidate> candidates;
  {
    std::scoped_lock lock(callback_state_->mutex);
    if (!callback_state_->remote_description_result.has_value()) return;
    if (!*callback_state_->remote_description_result)
      throw std::runtime_error("WebRTC rejected remote description");
    candidates.swap(pending_ice_);
  }
  for (const auto& candidate : candidates) transport_.ApplyRemoteIceCandidate(candidate);
}

}  // namespace veritassync::signaling
