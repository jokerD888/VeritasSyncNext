#pragma once

#include "engine/transport/peer_transport.h"

#include <deque>
#include <atomic>
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
  void RestartIce() override;
  void ApplyRemoteOffer(std::string sdp) override;
  void ApplyRemoteAnswer(std::string sdp) override;
  void ApplyRemoteIceCandidate(const IceCandidate& candidate) override;
  [[nodiscard]] bool IsReady() const override;

  void PumpReceived();
  [[nodiscard]] std::uint64_t BytesSent() const { return bytes_sent_.load(); }
  [[nodiscard]] std::uint64_t BytesReceived() const { return bytes_received_.load(); }

 private:
  struct Received {
    protocol::Channel channel;
    std::vector<std::uint8_t> wire;
  };
  std::unique_ptr<PeerTransport> inner_;
  mutable std::mutex mutex_;
  std::deque<Received> received_;
  ReceiveCallback callback_;
  std::atomic_uint64_t bytes_sent_{0}, bytes_received_{0};
  std::size_t received_bytes_ = 0;
  bool overflow_ = false;
};

}  // namespace veritassync::transport
