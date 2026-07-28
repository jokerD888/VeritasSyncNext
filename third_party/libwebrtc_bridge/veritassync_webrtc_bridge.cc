#include "veritassync_webrtc_bridge.h"

#include "api/data_channel_interface.h"

extern "C" uint32_t VeritasSyncWebRtcBridgeAbiVersion(void) {
  return VSYNC_WEBRTC_BRIDGE_ABI_VERSION;
}

extern "C" uint64_t VeritasSyncWebRtcBridgeMaxQueuedBytes(void) {
  return webrtc::DataChannelInterface::MaxSendQueueSize();
}
