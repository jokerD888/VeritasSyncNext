#include "engine/signaling/tracker_contract.h"
#include "tests/test_framework.h"

namespace {
veritassync::signaling::JoinRequest Join(std::string id, veritassync::protocol::Role role) {
  return {"task-1", "sync-key", std::move(id), "fingerprint", role, "token"};
}
}

VSYNC_TEST(TrackerContractForwardsOfferAnswerAndCandidates) {
  using namespace veritassync;
  signaling::TrackerRoom room("task-1", signaling::Topology::kBidirectional);
  room.Join(Join("node-a", protocol::Role::kPeer));
  room.Join(Join("node-b", protocol::Role::kPeer));
  room.Forward({signaling::MessageKind::kOffer, "node-a", "node-b", "v=0\r\n..."});
  room.Forward({signaling::MessageKind::kAnswer, "node-b", "node-a", "v=0\r\n..."});
  room.Forward({signaling::MessageKind::kIceCandidate, "node-a", "node-b", "candidate:1", "0", 0});
  const auto a_messages = room.DrainInbox("node-a");
  const auto b_messages = room.DrainInbox("node-b");
  VSYNC_CHECK(a_messages.size() == 1);
  VSYNC_CHECK(a_messages[0].kind == signaling::MessageKind::kAnswer);
  VSYNC_CHECK(b_messages.size() == 2);
  VSYNC_CHECK(b_messages[0].kind == signaling::MessageKind::kOffer);
  VSYNC_CHECK(b_messages[1].kind == signaling::MessageKind::kIceCandidate);
  VSYNC_CHECK(b_messages[1].candidate_mid == "0");
  VSYNC_CHECK(b_messages[1].candidate_mline_index == 0);
}

VSYNC_TEST(TrackerContractEnforcesTopologyAdmission) {
  using namespace veritassync;
  signaling::TrackerRoom one_way("task-1", signaling::Topology::kOneWay);
  one_way.Join(Join("source-a", protocol::Role::kSource));
  one_way.Join(Join("target-a", protocol::Role::kTarget));
  VSYNC_CHECK_THROWS(one_way.Join(Join("source-b", protocol::Role::kSource)));
  signaling::TrackerRoom bidirectional("task-1", signaling::Topology::kBidirectional);
  bidirectional.Join(Join("peer-a", protocol::Role::kPeer));
  bidirectional.Join(Join("peer-b", protocol::Role::kPeer));
  VSYNC_CHECK_THROWS(bidirectional.Join(Join("peer-c", protocol::Role::kPeer)));
}

VSYNC_TEST(TrackerContractKeepsOneWayTargetsReadOnly) {
  using namespace veritassync;
  signaling::TrackerRoom room("task-1", signaling::Topology::kOneWay);
  room.Join(Join("source", protocol::Role::kSource));
  room.Join(Join("target-a", protocol::Role::kTarget));
  room.Join(Join("target-b", protocol::Role::kTarget));

  room.Forward({signaling::MessageKind::kOffer, "source", "target-a", "offer"});
  room.Forward({signaling::MessageKind::kIceCandidate, "source", "target-b", "candidate"});
  room.Forward({signaling::MessageKind::kAnswer, "target-a", "source", "answer"});
  room.Forward({signaling::MessageKind::kIceCandidate, "target-a", "source", "candidate"});
  VSYNC_CHECK(room.DrainInbox("target-a").size() == 1);
  VSYNC_CHECK(room.DrainInbox("target-b").size() == 1);
  VSYNC_CHECK(room.DrainInbox("source").size() == 2);

  VSYNC_CHECK_THROWS(room.Forward({signaling::MessageKind::kOffer, "target-a", "source", "offer"}));
  VSYNC_CHECK_THROWS(room.Forward({signaling::MessageKind::kIceRestart, "target-a", "source", "restart"}));
  VSYNC_CHECK_THROWS(room.Forward({signaling::MessageKind::kOffer, "source", "source", "offer"}));
  VSYNC_CHECK_THROWS(room.Forward({signaling::MessageKind::kOffer, "target-a", "target-b", "offer"}));
}
