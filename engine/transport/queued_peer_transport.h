#pragma once

#include "engine/transport/peer_transport.h"

#include <deque>
#include <memory>
#include <mutex>

namespace veritassync::transport {

// Queues DataChannel callbacks from libwebrtc threads. PumpReceived dispatches
// them on the Engine session thread, keeping SQLite and sync state single-threaded.
class QueuedPeerTransport final : public PeerTransport {
 public:
  explicit QueuedPeerTransport(std::unique_ptr<PeerTransport> inner);
  ~QueuedPeerTransport() override;

  void Send(protocol::Channel channel, std::vector<std::uint8_t> wire) override;
  [[nodiscard]] std::size_t BufferedAmount(protocol::Channel channel) const override;
  void SetReceiveCallback(ReceiveCallback callback) override;
  void SetOfferCallback(SdpCallback callback) override;
  void SetAnswerCallback(SdpCallback callback) override;
  void SetIceCallback(IceCallback callback) override;
  void SetRemoteDescriptionCallback(RemoteDescriptionCallback callback) override;
  void CreateOffer() override;
  void ApplyRemoteOffer(std::string sdp) override;
  void ApplyRemoteAnswer(std::string sdp) override;
  void ApplyRemoteIceCandidate(const IceCandidate& candidate) override;
  [[nodiscard]] bool IsReady() const override;

  void PumpReceived();

 private:
  struct Received {
    protocol::Channel channel;
    std::vector<std::uint8_t> wire;
  };
  std::unique_ptr<PeerTransport> inner_;
  mutable std::mutex mutex_;
  std::deque<Received> received_;
  ReceiveCallback callback_;
};

}  // namespace veritassync::transport
