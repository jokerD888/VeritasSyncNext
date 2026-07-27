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

Enable the adapter only with the resulting exact checkout and artifact:

```powershell
cmake --preset default -DVERITASSYNC_ENABLE_WEBRTC=ON -DVERITASSYNC_WEBRTC_ROOT=D:\deps\webrtc\src -DVERITASSYNC_WEBRTC_LIBRARY=D:\deps\webrtc\src\out\veritassync\webrtc.lib
```

The current adapter probe verifies the documented PeerConnection header is available.
The following implementation step binds the connection state machine to the factory,
creates `control-v1` and `bulk-v1` DataChannels, and forwards their bytes to the
existing `Transport` contract.

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
