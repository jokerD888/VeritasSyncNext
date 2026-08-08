#include "engine/security/device_identity.h"
#include "engine/signaling/tracker_client.h"
#include "tests/test_framework.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
struct RequestCapture {
  std::string path;
  std::map<std::string, std::string> headers;
  std::string body;
};

class FakeHttp final : public veritassync::signaling::TrackerHttpTransport {
 public:
  std::vector<veritassync::signaling::TrackerHttpResponse> responses;
  std::vector<RequestCapture> requests;
  veritassync::signaling::TrackerHttpResponse Post(
      std::string_view, std::string_view path, const std::map<std::string, std::string>& headers,
      std::string_view body) override {
    requests.push_back({std::string(path), headers, std::string(body)});
    if (responses.empty()) throw std::runtime_error("unexpected fake Tracker request");
    auto response = responses.front();
    responses.erase(responses.begin());
    return response;
  }
};
}  // namespace

VSYNC_TEST(TrackerClientCreatesSignedInvitationAndRelaysSignals) {
  auto identity = veritassync::security::DeviceIdentity::Generate();
  auto fake = std::make_unique<FakeHttp>();
  auto* capture = fake.get();
  capture->responses.push_back({200, "INVITE\tABCD-EFGH\nOK\troom-1\t" + std::string(64, 'a') +
                                         "\tsession-1\t9999999999999\tMEMBERS\nMEMBER\t" +
                                         identity.DeviceId() + "\t" + identity.Fingerprint() +
                                         "\tsource\nEND\n"});
  capture->responses.push_back({200, "INVITE\tIJKL-MNOP\n"});
  capture->responses.push_back({200, "OK\n"});
  capture->responses.push_back(
      {200, "OK\nSIGNAL\tanswer\tpeer-b\t" + identity.DeviceId() + "\tv%3D0%0A\tdata\t0\nEND\n"});
  veritassync::signaling::TrackerClient client("https://tracker.example", identity,
                                               std::move(fake));
  const auto invitation =
      client.CreateRoom("photos", veritassync::signaling::Topology::kOneWay,
                        veritassync::protocol::Role::kSource, veritassync::protocol::Role::kTarget);
  VSYNC_CHECK(invitation.invitation_code == "ABCD-EFGH");
  VSYNC_CHECK(invitation.members.size() == 1);
  VSYNC_CHECK(capture->requests[0].headers.contains("X-VeritasSync-Signature"));
  VSYNC_CHECK(capture->requests[0].headers.at("X-VeritasSync-Device-Id") == identity.DeviceId());

  VSYNC_CHECK(client.CreateInvitation("room-1", veritassync::protocol::Role::kTarget) ==
              "IJKL-MNOP");
  VSYNC_CHECK(capture->requests[1].path == "/v1/invitations/create");
  VSYNC_CHECK(capture->requests[1].headers.at("X-VeritasSync-Session") == "session-1");

  client.Forward({veritassync::signaling::MessageKind::kOffer, identity.DeviceId(), "peer-b",
                  "v=0\n", "", -1});
  VSYNC_CHECK(capture->requests[2].headers.at("X-VeritasSync-Session") == "session-1");
  const auto messages = client.DrainInbox(identity.DeviceId());
  VSYNC_CHECK(messages.size() == 1);
  VSYNC_CHECK(messages[0].kind == veritassync::signaling::MessageKind::kAnswer);
  VSYNC_CHECK(messages[0].sender_device_id == "peer-b");
  VSYNC_CHECK(messages[0].payload == "v=0\n");
}

VSYNC_TEST(TrackerClientRejectsPlainHttpExceptLoopbackAndMalformedResponses) {
  auto identity = veritassync::security::DeviceIdentity::Generate();
  auto fake = std::make_unique<FakeHttp>();
  fake->responses.push_back({401, "invalid signature"});
  veritassync::signaling::TrackerClient client("https://tracker.example", identity,
                                               std::move(fake));
  VSYNC_CHECK_THROWS(client.RedeemInvitation("code", "task", veritassync::protocol::Role::kPeer));

  VSYNC_CHECK(veritassync::signaling::TrackerClient::DecodeField(
                  veritassync::signaling::TrackerClient::EncodeField("a\tb\nc")) == "a\tb\nc");
  VSYNC_CHECK_THROWS(veritassync::signaling::TrackerClient::DecodeField("%zz"));
}
