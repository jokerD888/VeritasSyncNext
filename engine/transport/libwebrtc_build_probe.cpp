#include "api/peer_connection_interface.h"

// This translation unit deliberately includes only the documented public API boundary.
// It proves that an explicitly supplied libwebrtc checkout is compatible with the
// engine build; the DataChannel adapter is introduced behind this boundary, never in
// sync or storage code.
namespace veritassync::transport {
void VerifyPinnedLibWebRtcHeaders() { (void)sizeof(webrtc::PeerConnectionInterface*); }
}  // namespace veritassync::transport
