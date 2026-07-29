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
typedef void(__cdecl* VsyncWebRtcBridgeIceCallback)(void* context, const char* mid, uint32_t mid_length,
                                                     int32_t mline_index, const char* candidate,
                                                     uint32_t candidate_length);
typedef void(__cdecl* VsyncWebRtcBridgeDataCallback)(void* context, uint32_t channel,
                                                      const uint8_t* bytes, uint32_t length);
typedef void(__cdecl* VsyncWebRtcBridgeCompletionCallback)(void* context, uint32_t success);

VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeAbiVersion(void);
VSYNC_WEBRTC_BRIDGE_EXPORT uint64_t VeritasSyncWebRtcBridgeMaxQueuedBytes(void);
VSYNC_WEBRTC_BRIDGE_EXPORT void* VeritasSyncWebRtcBridgeCreateFactory(void);
VSYNC_WEBRTC_BRIDGE_EXPORT void VeritasSyncWebRtcBridgeDestroyFactory(void* factory);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeCreateProtocolChannels(void* factory);
VSYNC_WEBRTC_BRIDGE_EXPORT void VeritasSyncWebRtcBridgeSetOfferCallback(
    void* factory, VsyncWebRtcBridgeSdpCallback callback, void* context);
VSYNC_WEBRTC_BRIDGE_EXPORT void VeritasSyncWebRtcBridgeSetAnswerCallback(
    void* factory, VsyncWebRtcBridgeSdpCallback callback, void* context);
VSYNC_WEBRTC_BRIDGE_EXPORT void VeritasSyncWebRtcBridgeSetIceCallback(
    void* factory, VsyncWebRtcBridgeIceCallback callback, void* context);
VSYNC_WEBRTC_BRIDGE_EXPORT void VeritasSyncWebRtcBridgeSetDataCallback(
    void* factory, VsyncWebRtcBridgeDataCallback callback, void* context);
VSYNC_WEBRTC_BRIDGE_EXPORT void VeritasSyncWebRtcBridgeSetRemoteDescriptionCallback(
    void* factory, VsyncWebRtcBridgeCompletionCallback callback, void* context);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeCreatePeerConnection(void* factory);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeCreateOffer(void* factory);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeApplyRemoteOffer(
    void* factory, const char* sdp, uint32_t length);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeApplyRemoteAnswer(
    void* factory, const char* sdp, uint32_t length);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeApplyRemoteIceCandidate(
    void* factory, const char* mid, uint32_t mid_length, int32_t mline_index,
    const char* candidate, uint32_t candidate_length);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeSendControl(
    void* factory, const uint8_t* bytes, uint32_t length);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeSendBulk(
    void* factory, const uint8_t* bytes, uint32_t length);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeControlChannelState(void* factory);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeConnectionState(void* factory);
VSYNC_WEBRTC_BRIDGE_EXPORT uint32_t VeritasSyncWebRtcBridgeIsReady(void* factory);

#ifdef __cplusplus
}
#endif
