#include "engine/runtime/network_session_manager.h"
#include "engine/security/device_identity.h"
#include "tests/test_framework.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace {
struct TrackerFixture {
  std::string local_device;
  std::string local_fingerprint;
  std::string remote_device = std::string(32, 'b');
  std::string remote_fingerprint = std::string(64, 'c');
  std::atomic_size_t offers{0};
};

class SessionHttp final : public veritassync::signaling::TrackerHttpTransport {
 public:
  explicit SessionHttp(std::shared_ptr<TrackerFixture> fixture) : fixture_(std::move(fixture)) {}
  veritassync::signaling::TrackerHttpResponse Post(std::string_view, std::string_view path,
                                                   const std::map<std::string, std::string>&,
                                                   std::string_view) override {
    if (path == "/v1/rooms/join") {
      return {200, "OK\troom-1\t" + std::string(64, 'a') +
                       "\tsession\t9999999999999\tMEMBERS\nMEMBER\t" + fixture_->local_device +
                       "\t" + fixture_->local_fingerprint + "\tsource\nMEMBER\t" +
                       fixture_->remote_device + "\t" + fixture_->remote_fingerprint +
                       "\ttarget\nEND\n"};
    }
    if (path == "/v1/signals/send") {
      ++fixture_->offers;
      return {200, "OK\n"};
    }
    if (path == "/v1/signals/drain") return {200, "OK\nEND\n"};
    return {404, "missing"};
  }

 private:
  std::shared_ptr<TrackerFixture> fixture_;
};

struct TransportFixture {
  std::atomic_size_t frames{0};
};
class ReadyPeerTransport final : public veritassync::transport::PeerTransport {
 public:
  explicit ReadyPeerTransport(std::shared_ptr<TransportFixture> fixture)
      : fixture_(std::move(fixture)) {}
  void Send(veritassync::protocol::Channel, std::vector<std::uint8_t>) override {
    ++fixture_->frames;
  }
  std::size_t BufferedAmount(veritassync::protocol::Channel) const override { return 0; }
  void SetReceiveCallback(ReceiveCallback callback) override { receive_ = std::move(callback); }
  void SetOfferCallback(SdpCallback callback) override { offer_ = std::move(callback); }
  void SetAnswerCallback(SdpCallback) override {}
  void SetIceCallback(IceCallback) override {}
  void SetRemoteDescriptionCallback(RemoteDescriptionCallback) override {}
  void CreateOffer() override {
    ready_ = true;
    if (offer_) offer_("v=0");
  }
  void ApplyRemoteOffer(std::string) override {}
  void ApplyRemoteAnswer(std::string) override { ready_ = true; }
  void ApplyRemoteIceCandidate(const IceCandidate&) override {}
  bool IsReady() const override { return ready_; }

 private:
  std::shared_ptr<TransportFixture> fixture_;
  ReceiveCallback receive_;
  SdpCallback offer_;
  bool ready_ = false;
};

bool WaitUntil(const std::function<bool()>& condition) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return condition();
}
}  // namespace

VSYNC_TEST(NetworkSessionManagerConnectsPairedSourceToSyncNode) {
  const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto root = std::filesystem::temp_directory_path() / ("veritassync-network-root-" + stamp);
  const auto database_path =
      std::filesystem::temp_directory_path() / ("veritassync-network-db-" + stamp + ".db");
  std::filesystem::create_directories(root);
  {
    veritassync::storage::Database database(database_path);
    database.ApplyMigrations();
    database.CreateTask({"network", "one_way", "source", root.string()});
    database.ConfigureTaskConnection(
        {"network", "https://tracker.example", "room-1", std::string(64, 'a'), 1});
    auto identity = veritassync::security::DeviceIdentity::Generate();
    auto tracker_fixture = std::make_shared<TrackerFixture>();
    tracker_fixture->local_device = identity.DeviceId();
    tracker_fixture->local_fingerprint = identity.Fingerprint();
    auto http_factory = [tracker_fixture] {
      return std::make_unique<SessionHttp>(tracker_fixture);
    };
    veritassync::security::PairingService pairing(database, std::move(identity), http_factory);
    auto transport_fixture = std::make_shared<TransportFixture>();
    veritassync::runtime::NetworkSessionOptions options;
    options.pump_interval = std::chrono::milliseconds(20);
    options.tracker_poll_interval = std::chrono::milliseconds(40);
    options.membership_refresh = std::chrono::seconds(10);
    options.retry_delay = std::chrono::milliseconds(100);
    veritassync::runtime::NetworkSessionManager manager(
        database, pairing,
        [transport_fixture](
            bool initiator) -> std::unique_ptr<veritassync::transport::PeerTransport> {
          VSYNC_CHECK(initiator);
          return std::make_unique<ReadyPeerTransport>(transport_fixture);
        },
        options);
    manager.Start();
    VSYNC_CHECK(WaitUntil([&] {
      std::scoped_lock lock(database.AccessMutex());
      return database.RuntimeState("network").network_status == "online";
    }));
    VSYNC_CHECK(tracker_fixture->offers.load() > 0);
    VSYNC_CHECK(transport_fixture->frames.load() > 0);
    manager.Stop();
  }
  std::filesystem::remove_all(root);
  std::filesystem::remove(database_path);
  std::filesystem::remove(database_path.string() + "-shm");
  std::filesystem::remove(database_path.string() + "-wal");
}
