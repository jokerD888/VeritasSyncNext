#include "engine/signaling/signaling_session.h"
#include "engine/transport/ice_options.h"
#include "tests/test_framework.h"

namespace {
class SignalingTransport final : public veritassync::transport::PeerTransport {
 public:
  SdpCallback offer, answer;
  RemoteDescriptionCallback description;
  int restart_count = 0;
  std::string applied_offer;
  void Send(veritassync::protocol::Channel, std::vector<std::uint8_t>) override {}
  std::size_t BufferedAmount(veritassync::protocol::Channel) const override { return 0; }
  void SetReceiveCallback(ReceiveCallback) override {}
  void SetOfferCallback(SdpCallback callback) override { offer = std::move(callback); }
  void SetAnswerCallback(SdpCallback callback) override { answer = std::move(callback); }
  void SetIceCallback(IceCallback) override {}
  void SetRemoteDescriptionCallback(RemoteDescriptionCallback callback) override { description = std::move(callback); }
  void CreateOffer() override { if (offer) offer("initial-sdp"); }
  void RestartIce() override { ++restart_count; if (offer) offer("new-ice-sdp"); }
  void ApplyRemoteOffer(std::string value) override { applied_offer = std::move(value); if (description) description(true); if (answer) answer("answer-sdp"); }
  void ApplyRemoteAnswer(std::string) override { if (description) description(true); }
  void ApplyRemoteIceCandidate(const IceCandidate&) override {}
  bool IsReady() const override { return true; }
};
}
VSYNC_TEST(SignalingSessionRenegotiatesIceAndReleasesCallbacksOnDestruction) {
  using namespace veritassync;
  signaling::TrackerRoom relay("task", signaling::Topology::kBidirectional);
  relay.Join({"task", "key", "a", "fa", protocol::Role::kPeer, "ta"});
  relay.Join({"task", "key", "b", "fb", protocol::Role::kPeer, "tb"});
  SignalingTransport a, b;
  {
    signaling::SignalingSession left(relay, "a", "b", a), right(relay, "b", "a", b);
    left.StartOffer(); left.Pump(); right.Pump(); right.Pump(); left.Pump();
    VSYNC_CHECK(b.applied_offer == "initial-sdp");
    left.RestartIce(); left.Pump(); right.Pump(); right.Pump(); left.Pump();
    VSYNC_CHECK(a.restart_count == 1);
    VSYNC_CHECK(b.applied_offer == "new-ice-sdp");
  }
  VSYNC_CHECK(!a.offer && !a.description && !b.answer);
}
VSYNC_TEST(IceConfigurationValidatesTurnCredentialsAndRelayPolicy) {
  using namespace veritassync::transport;
  IceOptions options;
  options.servers.push_back({"stun:localhost:3478", "", ""}); options.Validate();
  options.relay_only = true; VSYNC_CHECK_THROWS(options.Validate());
  options.servers.push_back({"turns:localhost:443?transport=tcp", "user", "short-lived-token"}); options.Validate();
  options.servers.back().credential.clear(); VSYNC_CHECK_THROWS(options.Validate());
  options.servers.back() = {"https://localhost", "", ""}; VSYNC_CHECK_THROWS(options.Validate());
}

VSYNC_TEST(SignalingSessionDistinguishesIceRestartFromNewRemoteCertificate) {
  using namespace veritassync;
  signaling::TrackerRoom relay("task", signaling::Topology::kBidirectional);
  relay.Join({"task", "key", "a", "fa", protocol::Role::kPeer, "ta"});
  relay.Join({"task", "key", "b", "fb", protocol::Role::kPeer, "tb"});
  SignalingTransport transport;
  signaling::SignalingSession session(relay, "b", "a", transport);
  const std::string original = "v=0\r\na=fingerprint:sha-256 AA:BB\r\na=ice-ufrag:old\r\n";
  relay.Forward({signaling::MessageKind::kOffer, "a", "b", original});
  session.Pump();
  VSYNC_CHECK(transport.applied_offer == original);
  const std::string restart = "v=0\r\na=fingerprint:sha-256 AA:BB\r\na=ice-ufrag:new\r\n";
  relay.Forward({signaling::MessageKind::kIceRestart, "a", "b", restart});
  session.Pump();
  VSYNC_CHECK(transport.applied_offer == restart);
  VSYNC_CHECK(session.TakeResetMessages().empty());
  const std::string replacement = "v=0\r\na=fingerprint:sha-256 CC:DD\r\n";
  relay.Forward({signaling::MessageKind::kOffer, "a", "b", replacement});
  relay.Forward({signaling::MessageKind::kIceCandidate, "a", "b", "candidate:replacement", "0", 0});
  session.Pump();
  VSYNC_CHECK(transport.applied_offer == restart);
  const auto replay = session.TakeResetMessages();
  VSYNC_CHECK(replay.size() == 2 && replay[0].payload == replacement);
  VSYNC_CHECK(replay[1].kind == signaling::MessageKind::kIceCandidate);
  VSYNC_CHECK(session.TakeResetMessages().empty());
}

VSYNC_TEST(SignalingSessionCopiedNativeCallbacksMayFinishAfterDestruction) {
  using namespace veritassync;
  signaling::TrackerRoom relay("task", signaling::Topology::kBidirectional);
  relay.Join({"task", "key", "a", "fa", protocol::Role::kPeer, "ta"});
  relay.Join({"task", "key", "b", "fb", protocol::Role::kPeer, "tb"});
  SignalingTransport transport;
  transport::PeerTransport::SdpCallback late_offer, late_answer;
  transport::PeerTransport::RemoteDescriptionCallback late_description;
  {
    signaling::SignalingSession session(relay, "a", "b", transport);
    late_offer = transport.offer; late_answer = transport.answer; late_description = transport.description;
  }
  late_offer("late-offer"); late_answer("late-answer"); late_description(true);
  VSYNC_CHECK(relay.DrainInbox("b").empty());
  VSYNC_CHECK(!transport.offer && !transport.answer && !transport.description);
}
