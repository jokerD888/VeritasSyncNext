#include "engine/signaling/signaling_session.h"
#include "engine/signaling/tracker_contract.h"
#include "engine/transport/webrtc_bridge_loader.h"
#include "engine/transport/webrtc_transport.h"
#include "engine/transport/queued_peer_transport.h"
#include "engine/common/path.h"
#include "engine/common/content_hash.h"
#include "engine/storage/database.h"
#include "engine/sync/one_way_sync.h"
#include "tests/test_framework.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <filesystem>
#include <fstream>
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
    if (message.kind == veritassync::signaling::MessageKind::kOffer)
      initial_offer_ = message.payload;
    if (message.kind == veritassync::signaling::MessageKind::kIceRestart)
      restart_offer_ = message.payload;
    room_.Forward(message);
  }
  [[nodiscard]] std::vector<veritassync::signaling::RelayMessage> DrainInbox(
      const std::string& device_id) override {
    return room_.DrainInbox(device_id);
  }
  [[nodiscard]] std::size_t Count(const veritassync::signaling::MessageKind kind) const {
    return counts_[static_cast<std::size_t>(kind)];
  }
  [[nodiscard]] bool RestartUsesNewCredentials() const {
    const auto credential = [](const std::string& sdp, const char* field) {
      const auto start = sdp.find(field);
      if (start == std::string::npos) return std::string{};
      return sdp.substr(start, sdp.find('\n', start) - start);
    };
    const auto initial_user = credential(initial_offer_, "a=ice-ufrag:");
    const auto initial_password = credential(initial_offer_, "a=ice-pwd:");
    const auto restart_user = credential(restart_offer_, "a=ice-ufrag:");
    const auto restart_password = credential(restart_offer_, "a=ice-pwd:");
    return !initial_user.empty() && !initial_password.empty() && !restart_user.empty() &&
           !restart_password.empty() && initial_user != restart_user &&
           initial_password != restart_password;
  }

 private:
  veritassync::signaling::TrackerRoom room_;
  std::array<std::size_t, 4> counts_{};
  std::string initial_offer_, restart_offer_;
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

VSYNC_TEST(WebRtcDataChannelsTransferLargeFramesAndRestartIce) {
  using namespace veritassync;
  const auto bridge_path = BridgeLibraryPath();
  auto a_native = std::make_unique<transport::WebRtcTransport>(bridge_path, true);
  auto b_native = std::make_unique<transport::WebRtcTransport>(bridge_path, false);
  const auto* a_diagnostics = a_native.get();
  const auto* b_diagnostics = b_native.get();
  transport::QueuedPeerTransport a(std::move(a_native));
  transport::QueuedPeerTransport b(std::move(b_native));
  RecordingRelay room;
  room.Join({"task-1", "sync-key", "node-a", "fingerprint-a", protocol::Role::kPeer, "token-a"});
  room.Join({"task-1", "sync-key", "node-b", "fingerprint-b", protocol::Role::kPeer, "token-b"});
  signaling::SignalingSession a_session(room, "node-a", "node-b", a);
  signaling::SignalingSession b_session(room, "node-b", "node-a", b);
  auto pump = [&] {
    a_session.Pump(); b_session.Pump(); a.PumpReceived(); b.PumpReceived();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  };
  auto until = [&](const char* stage, const auto& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!condition() && std::chrono::steady_clock::now() < deadline) pump();
    if (!condition()) throw std::runtime_error(std::string("native WebRTC timeout: ") + stage +
        "; offers=" + std::to_string(room.Count(signaling::MessageKind::kOffer)) +
        "; answers=" + std::to_string(room.Count(signaling::MessageKind::kAnswer)) +
        "; candidates=" + std::to_string(room.Count(signaling::MessageKind::kIceCandidate)) +
        "; a=" + a_diagnostics->DiagnosticState() + "; b=" + b_diagnostics->DiagnosticState());
  };
  a_session.StartOffer();
  until("initial DataChannel connection", [&] { return a.IsReady() && b.IsReady(); });

  protocol::Chunk chunk{};
  chunk.bytes.assign(protocol::kLogicalChunkSize, 0xA5);
  chunk.chunk_hash = protocol::TestHash(chunk.bytes);
  const auto bulk = protocol::EncodeChunkFrame(chunk, 42);
  const auto control = protocol::EncodeFrame({protocol::FrameType::kHeartbeat, 43, {1, 2, 3}});
  unsigned bulk_received = 0, control_received = 0;
  b.SetReceiveCallback([&](auto channel, auto wire) {
    VSYNC_CHECK(channel == protocol::Channel::kBulk && wire == bulk); ++bulk_received;
  });
  a.SetReceiveCallback([&](auto channel, auto wire) {
    VSYNC_CHECK(channel == protocol::Channel::kControl && wire == control); ++control_received;
  });
  a.Send(protocol::Channel::kBulk, bulk);
  b.Send(protocol::Channel::kControl, control);
  until("initial frame transfer", [&] { return bulk_received == 1 && control_received == 1; });

  a_session.RestartIce();
  until("ICE restart", [&] { return room.Count(signaling::MessageKind::kIceRestart) == 1 &&
                    room.Count(signaling::MessageKind::kAnswer) == 2 && a.IsReady() && b.IsReady(); });
  VSYNC_CHECK(room.RestartUsesNewCredentials());
  a.Send(protocol::Channel::kBulk, bulk);
  b.Send(protocol::Channel::kControl, control);
  until("post-restart frame transfer", [&] { return bulk_received == 2 && control_received == 2; });

  struct Files {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("veritassync-native-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Files() { std::filesystem::create_directories(root / "source"); std::filesystem::create_directories(root / "target"); }
    ~Files() { std::error_code error; std::filesystem::remove_all(root, error); }
  } files;
  const auto source_root = files.root / "source";
  const auto target_root = files.root / "target";
  const auto relative = common::Utf8Path("照片-你好🙂.bin");
  const auto source_file = source_root / relative;
  const auto target_file = target_root / relative;
  {
    std::vector<std::uint8_t> bytes(12U * 1024U * 1024U + 7U);
    for (std::size_t index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<std::uint8_t>(index % 251);
    std::ofstream output(source_file, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  { std::ofstream empty(source_root / "empty.txt", std::ios::binary); }
  const auto expected_hash = common::Blake3File(source_file);
  storage::Database source_db(files.root / "source.db"), target_db(files.root / "target.db");
  source_db.ApplyMigrations(); target_db.ApplyMigrations();
  source_db.CreateTask({"task-1", "one_way", "source", common::PathUtf8(source_root)});
  target_db.CreateTask({"task-1", "one_way", "target", common::PathUtf8(target_root)});
  sync::OneWaySyncNode source({"task-1", protocol::Role::kSource, "node-a", "node-b",
      "fingerprint-a", "native-test-auth", source_root, source_db}, a);
  sync::OneWaySyncNode target({"task-1", protocol::Role::kTarget, "node-b", "node-a",
      "fingerprint-b", "native-test-auth", target_root, target_db}, b);
  source.Start(); target.Start();
  until("12 MiB native file synchronization", [&] {
    source.Pump(); target.Pump();
    if (source.LastError()) throw std::runtime_error(*source.LastError());
    if (target.LastError()) throw std::runtime_error(*target.LastError());
    return std::filesystem::exists(target_file) && std::filesystem::exists(target_root / "empty.txt") &&
           target.TargetIsConverged();
  });
  VSYNC_CHECK(common::Blake3File(target_file) == expected_hash);
  VSYNC_CHECK(std::filesystem::file_size(target_root / "empty.txt") == 0);
  VSYNC_CHECK(std::filesystem::file_size(target_file) == std::filesystem::file_size(source_file));
  std::filesystem::remove(source_file);
  source.RefreshSource();
  until("native file deletion", [&] { source.Pump(); target.Pump(); return !std::filesystem::exists(target_file); });
}
