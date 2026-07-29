#include "engine/transport/webrtc_bridge_loader.h"
#include "engine/transport/webrtc_transport.h"
#include "tests/test_framework.h"

#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
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
