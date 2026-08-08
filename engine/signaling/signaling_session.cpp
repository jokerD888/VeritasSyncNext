#include "engine/signaling/signaling_session.h"

#include <stdexcept>
#include <utility>

namespace veritassync::signaling {

SignalingSession::SignalingSession(SignalingRelay& relay, std::string local_device_id,
                                   std::string remote_device_id,
                                   transport::PeerTransport& transport)
    : relay_(relay),
      local_device_id_(std::move(local_device_id)),
      remote_device_id_(std::move(remote_device_id)),
      transport_(transport) {
  if (local_device_id_.empty() || remote_device_id_.empty() ||
      local_device_id_ == remote_device_id_) {
    throw std::invalid_argument("signaling session requires two distinct device ids");
  }
  transport_.SetOfferCallback(
      [this](std::string sdp) { Queue(MessageKind::kOffer, std::move(sdp)); });
  transport_.SetAnswerCallback(
      [this](std::string sdp) { Queue(MessageKind::kAnswer, std::move(sdp)); });
  transport_.SetIceCallback(
      [this](transport::PeerTransport::IceCandidate candidate) { QueueIce(std::move(candidate)); });
  transport_.SetRemoteDescriptionCallback([this](const bool success) {
    std::scoped_lock lock(mutex_);
    remote_description_result_ = success;
  });
}

void SignalingSession::StartOffer() { transport_.CreateOffer(); }

void SignalingSession::Pump() {
  std::vector<RelayMessage> outbound;
  {
    std::scoped_lock lock(mutex_);
    outbound.swap(outbound_);
  }
  for (const auto& message : outbound) relay_.Forward(message);

  for (const auto& message : relay_.DrainInbox(local_device_id_)) ApplyMessage(message);
  ApplyReadyCandidates();
}

void SignalingSession::Queue(const MessageKind kind, std::string payload) {
  if (payload.empty()) return;
  std::scoped_lock lock(mutex_);
  outbound_.push_back({kind, local_device_id_, remote_device_id_, std::move(payload)});
}

void SignalingSession::QueueIce(transport::PeerTransport::IceCandidate candidate) {
  if (candidate.mid.empty() || candidate.candidate.empty()) return;
  std::scoped_lock lock(mutex_);
  outbound_.push_back({MessageKind::kIceCandidate, local_device_id_, remote_device_id_,
                       std::move(candidate.candidate), std::move(candidate.mid),
                       candidate.mline_index});
}

void SignalingSession::ApplyMessage(const RelayMessage& message) {
  if (message.sender_device_id != remote_device_id_) {
    throw std::invalid_argument("signal sender does not match session peer");
  }
  switch (message.kind) {
    case MessageKind::kOffer:
    case MessageKind::kAnswer:
      if (message.payload.empty()) throw std::invalid_argument("empty session description");
      {
        std::scoped_lock lock(mutex_);
        remote_description_result_.reset();
      }
      if (message.kind == MessageKind::kOffer)
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
        std::scoped_lock lock(mutex_);
        pending_ice_.push_back(
            {message.candidate_mid, message.candidate_mline_index, message.payload});
      }
      break;
    case MessageKind::kIceRestart:
      throw std::invalid_argument("ICE restart is not implemented by this bridge revision");
  }
}

void SignalingSession::ApplyReadyCandidates() {
  std::vector<transport::PeerTransport::IceCandidate> candidates;
  {
    std::scoped_lock lock(mutex_);
    if (!remote_description_result_.has_value()) return;
    if (!*remote_description_result_)
      throw std::runtime_error("WebRTC rejected remote description");
    candidates.swap(pending_ice_);
  }
  for (const auto& candidate : candidates) transport_.ApplyRemoteIceCandidate(candidate);
}

}  // namespace veritassync::signaling
