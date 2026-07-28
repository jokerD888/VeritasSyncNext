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

This validation deliberately does **not** include WebRTC C++ headers in the MSVC-built
engine. The static library is produced by WebRTC's pinned GN/clang-cl toolchain and
uses its own C++ runtime settings; consuming its C++ API directly from the engine
would create an unsupported ABI boundary. The next adapter target is therefore built
by GN with the same toolchain and exposes a narrow C ABI / PImpl boundary to
`engine/transport`. It will create `control-v1` and `bulk-v1` DataChannels and forward
their bytes to the existing `Transport` contract.

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
