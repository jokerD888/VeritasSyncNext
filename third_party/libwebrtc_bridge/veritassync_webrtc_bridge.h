#pragma once

#include <stdint.h>

#if defined(_WIN32)
#define VSYNC_WEBRTC_BRIDGE_EXPORT __declspec(dllexport)
#else
#define VSYNC_WEBRTC_BRIDGE_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum { VSYNC_WEBRTC_BRIDGE_ABI_VERSION = 1 };

VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeAbiVersion(void);
VSYNC_WEBRTC_BRIDGE_EXPORT uint64_t VeritasSyncWebRtcBridgeMaxQueuedBytes(void);

#ifdef __cplusplus
}
#endif
