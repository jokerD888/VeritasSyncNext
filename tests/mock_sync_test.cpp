#include "engine/sync/phase0_node.h"
#include "tests/test_framework.h"

VSYNC_TEST(MockTransportCompletesHelloManifestAndFakeBlockTransfer) {
  using namespace veritassync;
  transport::MockNetwork network;
  auto endpoints = network.CreatePair();
  protocol::Manifest source_manifest{7, {{"notes.txt", 12, "fake-content-hash"}}};
  protocol::Manifest target_manifest{3, {}};
  sync::Phase0Node source({"task-1", protocol::Role::kSource, "source", "source-fp", "shared-auth"}, *endpoints.first, source_manifest);
  sync::Phase0Node target({"task-1", protocol::Role::kTarget, "target", "target-fp", "shared-auth"}, *endpoints.second, target_manifest);
  source.Start();
  target.Start();
  network.PumpUntilIdle();
  VSYNC_CHECK(source.HandshakeComplete());
  VSYNC_CHECK(target.HandshakeComplete());
  VSYNC_CHECK(target.ReceivedManifest().has_value());
  VSYNC_CHECK(target.ReceivedManifest()->revision == 7);
  VSYNC_CHECK(target.ReceivedManifest()->entries.size() == 1);
  source.SendFakeBlock({9, 8, 7, 6, 5});
  network.PumpUntilIdle();
  VSYNC_CHECK(target.ReceivedChunks().size() == 1);
  VSYNC_CHECK(target.ReceivedChunks()[0].bytes == std::vector<std::uint8_t>({9, 8, 7, 6, 5}));
  VSYNC_CHECK(!target.LastError().has_value());
}

VSYNC_TEST(MockTransportRejectsMismatchedAuthorization) {
  using namespace veritassync;
  transport::MockNetwork network;
  auto endpoints = network.CreatePair();
  sync::Phase0Node source({"task-1", protocol::Role::kSource, "source", "source-fp", "expected"}, *endpoints.first, {1, {}});
  sync::Phase0Node target({"task-1", protocol::Role::kTarget, "target", "target-fp", "wrong"}, *endpoints.second, {1, {}});
  source.Start();
  target.Start();
  network.PumpUntilIdle();
  VSYNC_CHECK(target.LastError().has_value());
  VSYNC_CHECK(!target.HandshakeComplete());
}
