# Phase 1 development notes

Phase 1 introduces the connection boundary only: PeerConnection/DataChannels,
Tracker signaling, and a development TURN service. Sync, storage, and protocol code
must continue to depend solely on `engine/transport`, not libwebrtc headers.

## Pinned WebRTC build

`third_party/libwebrtc.lock` pins the official `lkgr` commit. Install depot_tools,
place it on `PATH`, and run:

```powershell
./scripts/bootstrap-webrtc.ps1 -CheckoutRoot D:\deps\webrtc
cd D:\deps\webrtc\src
gn gen out\veritassync --args='is_debug=false is_component_build=false rtc_include_tests=false rtc_build_examples=false rtc_enable_protobuf=false'
autoninja -C out\veritassync
```

On Windows the bootstrap uses the installed Visual Studio toolchain
(`DEPOT_TOOLS_WIN_TOOLCHAIN=0`), so it does not require access to Google's internal
Chrome Windows toolchain bucket.

Verify the pinned checkout and its build artifact from the CMake project:

```powershell
cmake --preset default -DVERITASSYNC_ENABLE_WEBRTC=ON -DVERITASSYNC_WEBRTC_ROOT=D:\deps\webrtc\src -DVERITASSYNC_WEBRTC_LIBRARY=D:\deps\webrtc\src\out\veritassync\obj\webrtc.lib
```

Build the matching C ABI bridge with the same GN/clang-cl toolchain:

```powershell
./scripts/build-webrtc-bridge.ps1 -CheckoutRoot D:\deps\webrtc
cmake --preset default -DVERITASSYNC_ENABLE_WEBRTC=ON -DVERITASSYNC_WEBRTC_ROOT=D:\deps\webrtc\src -DVERITASSYNC_WEBRTC_LIBRARY=D:\deps\webrtc\src\out\veritassync\obj\webrtc.lib -DVERITASSYNC_WEBRTC_BRIDGE_LIBRARY=D:\deps\webrtc\src\out\veritassync\veritassync_webrtc_bridge.dll
ctest --test-dir build\default -C Debug --output-on-failure
```

This deliberately keeps WebRTC C++ headers out of the MSVC-built engine. The bridge
is a narrow C ABI boundary and its runtime test calls the real DataChannel send-queue
API, creates/destroys a PeerConnectionFactory, and creates ordered `control-v1` plus
unordered `bulk-v1` channels. It also produces a real SDP offer containing the data
channel media section, applies it to a second local PeerConnection, receives the SDP
answer, and applies that answer back to the initiator. ICE candidate relay and
DataChannel payload forwarding still remain before this is a complete signaling
implementation.

The bridge script makes one local, reproducible build-graph change to the otherwise
pinned checkout so GN emits the bridge target. Its source and patch both live in this
repository; `third_party/libwebrtc.lock` remains the source revision authority.

## Tracker and TURN

`engine/signaling/tracker_contract.*` is the exact topology/admission and relay
contract for the independently deployed Tracker. It permits only Offer, Answer, ICE
candidate, and ICE restart relay messages; it never accepts file data.

For a local coturn instance, set a temporary `TURN_SHARED_SECRET` and run:

```powershell
docker compose -f deploy/coturn/docker-compose.yml up
```

This repository does not store TURN credentials or TLS certificates. The development
configuration opens UDP/TCP 3478, TLS 5349, and a restricted relay range only.
