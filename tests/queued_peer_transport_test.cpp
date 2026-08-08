#include "engine/transport/queued_peer_transport.h"
#include "tests/test_framework.h"

#include <memory>
#include <utility>

namespace {
class FakePeerTransport final : public veritassync::transport::PeerTransport {
 public:
  void Send(veritassync::protocol::Channel, std::vector<std::uint8_t>) override {}
  std::size_t BufferedAmount(veritassync::protocol::Channel) const override { return 0; }
  void SetReceiveCallback(ReceiveCallback callback) override { receive = std::move(callback); }
  void SetOfferCallback(SdpCallback callback) override { offer = std::move(callback); }
  void SetAnswerCallback(SdpCallback callback) override { answer = std::move(callback); }
  void SetIceCallback(IceCallback callback) override { ice = std::move(callback); }
  void SetRemoteDescriptionCallback(RemoteDescriptionCallback callback) override {
    description = std::move(callback);
  }
  void CreateOffer() override {
    if (offer) offer("offer");
  }
  void ApplyRemoteOffer(std::string) override {
    if (answer) answer("answer");
  }
  void ApplyRemoteAnswer(std::string) override {
    if (description) description(true);
  }
  void ApplyRemoteIceCandidate(const IceCandidate&) override {}
  bool IsReady() const override { return true; }
  ReceiveCallback receive;
  SdpCallback offer, answer;
  IceCallback ice;
  RemoteDescriptionCallback description;
};
}  // namespace

VSYNC_TEST(QueuedPeerTransportDispatchesDataOnlyWhenEnginePumps) {
  auto inner = std::make_unique<FakePeerTransport>();
  auto* fake = inner.get();
  veritassync::transport::QueuedPeerTransport queued(std::move(inner));
  std::size_t received = 0;
  queued.SetReceiveCallback(
      [&](veritassync::protocol::Channel channel, std::vector<std::uint8_t> wire) {
        VSYNC_CHECK(channel == veritassync::protocol::Channel::kControl);
        received += wire.size();
      });
  fake->receive(veritassync::protocol::Channel::kControl, {1, 2, 3});
  VSYNC_CHECK(received == 0);
  queued.PumpReceived();
  VSYNC_CHECK(received == 3);
}
