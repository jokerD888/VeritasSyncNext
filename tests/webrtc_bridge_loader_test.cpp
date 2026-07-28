#include "engine/transport/webrtc_bridge_loader.h"
#include "tests/test_framework.h"

#include <Windows.h>

#include <stdexcept>
#include <string>
#include <vector>

VSYNC_TEST(WebRtcBridgeUsesStableCAbi) {
  const auto required = GetEnvironmentVariableW(L"VERITASSYNC_WEBRTC_BRIDGE_LIBRARY", nullptr, 0);
  if (required == 0) throw std::runtime_error("VERITASSYNC_WEBRTC_BRIDGE_LIBRARY is required for this test");
  std::vector<wchar_t> path(required);
  if (GetEnvironmentVariableW(L"VERITASSYNC_WEBRTC_BRIDGE_LIBRARY", path.data(), required) == 0) {
    throw std::runtime_error("cannot read VERITASSYNC_WEBRTC_BRIDGE_LIBRARY");
  }
  const auto max_queued_bytes = veritassync::transport::WebRtcBridgeLoader::VerifyAndReadMaxQueuedBytes(path.data());
  VSYNC_CHECK(max_queued_bytes == 16U * 1024U * 1024U);
}
