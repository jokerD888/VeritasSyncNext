#include "engine/signaling/signaling_session.h"
#include "engine/signaling/tracker_contract.h"
#include "engine/transport/webrtc_bridge_loader.h"
#include "engine/transport/webrtc_transport.h"
#include "tests/test_framework.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path BridgeLibraryPath() {
  const auto required = GetEnvironmentVariableW(L"VERITASSYNC_WEBRTC_BRIDGE_LIBRARY", nullptr, 0);
  if (required == 0) {
    throw std::runtime_error("VERITASSYNC_WEBRTC_BRIDGE_LIBRARY is required for this test");
  }

  std::vector<wchar_t> path(required);
  if (GetEnvironmentVariableW(L"VERITASSYNC_WEBRTC_BRIDGE_LIBRARY", path.data(), required) == 0) {
    throw std::runtime_error("cannot read VERITASSYNC_WEBRTC_BRIDGE_LIBRARY");
  }
  return path.data();
}

class RecordingRelay final : public veritassync::signaling::SignalingRelay {
 public:
  RecordingRelay() : room_("task-1", veritassync::signaling::Topology::kBidirectional) {}
  void Join(const veritassync::signaling::JoinRequest& request) { room_.Join(request); }
  void Forward(const veritassync::signaling::RelayMessage& message) override {
    ++counts_[static_cast<std::size_t>(message.kind)];
    room_.Forward(message);
  }
  [[nodiscard]] std::vector<veritassync::signaling::RelayMessage> DrainInbox(
      const std::string& device_id) override {
    return room_.DrainInbox(device_id);
  }
  [[nodiscard]] std::size_t Count(const veritassync::signaling::MessageKind kind) const {
    return counts_[static_cast<std::size_t>(kind)];
  }

 private:
  veritassync::signaling::TrackerRoom room_;
  std::array<std::size_t, 4> counts_{};
};

}  // namespace

VSYNC_TEST(WebRtcBridgeUsesStableCAbi) {
  const auto bridge_path = BridgeLibraryPath();
  const auto max_queued_bytes =
      veritassync::transport::WebRtcBridgeLoader::VerifyAndReadMaxQueuedBytes(bridge_path);
  VSYNC_CHECK(max_queued_bytes == 16U * 1024U * 1024U);
  veritassync::transport::WebRtcBridgeLoader::VerifyFactoryLifecycle(bridge_path);
}

VSYNC_TEST(WebRtcTransportRelaysLocalOffer) {
  std::mutex mutex;
  std::condition_variable offer_ready;
  std::string offer;

  veritassync::transport::WebRtcTransport transport(BridgeLibraryPath());
  transport.SetOfferCallback([&](std::string local_offer) {
    {
      std::scoped_lock lock(mutex);
      offer = std::move(local_offer);
    }
    offer_ready.notify_one();
  });
  transport.CreateOffer();

  std::unique_lock lock(mutex);
  VSYNC_CHECK(offer_ready.wait_for(lock, std::chrono::seconds(10), [&] { return !offer.empty(); }));
  VSYNC_CHECK(offer.find("m=application") != std::string::npos);
}

VSYNC_TEST(WebRtcSignalingSessionRelaysTwoLocalPeerDescriptionsAndCandidates) {
  using namespace veritassync;
  const auto bridge_path = BridgeLibraryPath();
  transport::WebRtcTransport offerer(bridge_path);
  transport::WebRtcTransport answerer(bridge_path, false);
  RecordingRelay room;
  room.Join({"task-1", "sync-key", "node-a", "fingerprint-a", protocol::Role::kPeer, "token-a"});
  room.Join({"task-1", "sync-key", "node-b", "fingerprint-b", protocol::Role::kPeer, "token-b"});
  signaling::SignalingSession a_session(room, "node-a", "node-b", offerer);
  signaling::SignalingSession b_session(room, "node-b", "node-a", answerer);
  a_session.StartOffer();

  const auto relay_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < relay_deadline &&
         (room.Count(signaling::MessageKind::kOffer) != 1 ||
          room.Count(signaling::MessageKind::kAnswer) != 1 ||
          room.Count(signaling::MessageKind::kIceCandidate) < 2)) {
    a_session.Pump();
    b_session.Pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  VSYNC_CHECK(room.Count(signaling::MessageKind::kOffer) == 1);
  VSYNC_CHECK(room.Count(signaling::MessageKind::kAnswer) == 1);
  VSYNC_CHECK(room.Count(signaling::MessageKind::kIceCandidate) >= 2);
}
