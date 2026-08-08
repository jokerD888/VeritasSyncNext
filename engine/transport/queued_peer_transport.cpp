#include "engine/transport/queued_peer_transport.h"

#include <stdexcept>
#include <utility>

namespace veritassync::transport {

QueuedPeerTransport::QueuedPeerTransport(std::unique_ptr<PeerTransport> inner)
    : inner_(std::move(inner)) {
  if (inner_ == nullptr)
    throw std::invalid_argument("queued peer transport requires an inner transport");
  inner_->SetReceiveCallback(
      [this](const protocol::Channel channel, std::vector<std::uint8_t> wire) {
        std::scoped_lock lock(mutex_);
        received_.push_back({channel, std::move(wire)});
      });
}

QueuedPeerTransport::~QueuedPeerTransport() { inner_->SetReceiveCallback({}); }
void QueuedPeerTransport::Send(const protocol::Channel channel, std::vector<std::uint8_t> wire) {
  inner_->Send(channel, std::move(wire));
}
std::size_t QueuedPeerTransport::BufferedAmount(const protocol::Channel channel) const {
  return inner_->BufferedAmount(channel);
}
void QueuedPeerTransport::SetReceiveCallback(ReceiveCallback callback) {
  std::scoped_lock lock(mutex_);
  callback_ = std::move(callback);
}
void QueuedPeerTransport::SetOfferCallback(SdpCallback callback) {
  inner_->SetOfferCallback(std::move(callback));
}
void QueuedPeerTransport::SetAnswerCallback(SdpCallback callback) {
  inner_->SetAnswerCallback(std::move(callback));
}
void QueuedPeerTransport::SetIceCallback(IceCallback callback) {
  inner_->SetIceCallback(std::move(callback));
}
void QueuedPeerTransport::SetRemoteDescriptionCallback(RemoteDescriptionCallback callback) {
  inner_->SetRemoteDescriptionCallback(std::move(callback));
}
void QueuedPeerTransport::CreateOffer() { inner_->CreateOffer(); }
void QueuedPeerTransport::ApplyRemoteOffer(std::string sdp) {
  inner_->ApplyRemoteOffer(std::move(sdp));
}
void QueuedPeerTransport::ApplyRemoteAnswer(std::string sdp) {
  inner_->ApplyRemoteAnswer(std::move(sdp));
}
void QueuedPeerTransport::ApplyRemoteIceCandidate(const IceCandidate& candidate) {
  inner_->ApplyRemoteIceCandidate(candidate);
}
bool QueuedPeerTransport::IsReady() const { return inner_->IsReady(); }

void QueuedPeerTransport::PumpReceived() {
  std::deque<Received> received;
  ReceiveCallback callback;
  {
    std::scoped_lock lock(mutex_);
    received.swap(received_);
    callback = callback_;
  }
  if (!callback) return;
  for (auto& item : received) callback(item.channel, std::move(item.wire));
}

}  // namespace veritassync::transport
