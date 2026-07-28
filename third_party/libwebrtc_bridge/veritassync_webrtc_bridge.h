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
typedef void(__cdecl* VsyncWebRtcBridgeSdpCallback)(void* context, const char* sdp, uint32_t length);

VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeAbiVersion(void);
VSYNC_WEBRTC_BRIDGE_EXPORT uint64_t VeritasSyncWebRtcBridgeMaxQueuedBytes(void);
VSYNC_WEBRTC_BRIDGE_EXPORT void* VeritasSyncWebRtcBridgeCreateFactory(void);
VSYNC_WEBRTC_BRIDGE_EXPORT void VeritasSyncWebRtcBridgeDestroyFactory(void* factory);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeCreateProtocolChannels(void* factory);
VSYNC_WEBRTC_BRIDGE_EXPORT void VeritasSyncWebRtcBridgeSetOfferCallback(
    void* factory, VsyncWebRtcBridgeSdpCallback callback, void* context);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeCreateOffer(void* factory);

#ifdef __cplusplus
}
#endif
