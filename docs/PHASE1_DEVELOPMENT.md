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
cmake --preset default -DVERITASSYNC_ENABLE_WEBRTC=ON -DVERITASSYNC_WEBRTC_ROOT=D:\deps\webrtc\src -DVERITASSYNC_WEBRTC_BRIDGE_LIBRARY=D:\deps\webrtc\src\out\veritassync\veritassync_webrtc_bridge.dll
```

Build the matching C ABI bridge with the same GN/clang-cl toolchain:

```powershell
./scripts/build-webrtc-bridge.ps1 -CheckoutRoot D:\deps\webrtc
cmake --preset default -DVERITASSYNC_ENABLE_WEBRTC=ON -DVERITASSYNC_WEBRTC_ROOT=D:\deps\webrtc\src -DVERITASSYNC_WEBRTC_BRIDGE_LIBRARY=D:\deps\webrtc\src\out\veritassync\veritassync_webrtc_bridge.dll
ctest --test-dir build\default -C Debug --output-on-failure
```

This deliberately keeps WebRTC C++ headers out of the MSVC-built engine. The bridge
is a narrow C ABI boundary and its runtime test calls the real DataChannel send-queue
API, creates/destroys a PeerConnectionFactory, and creates ordered `control-v1` plus
unordered `bulk-v1` channels. It also produces a real SDP offer containing the data
channel media section, applies it to a second local PeerConnection, receives the SDP
answer, applies that answer back to the initiator, then forwards the gathered ICE
candidates in both directions. The additional native acceptance test opens both
DataChannels, verifies fragmented bulk/control delivery, renegotiates ICE with
changed credentials, and then synchronizes a 12 MiB binary, a Unicode filename,
an empty file and a deletion through production one-way sync nodes.
The 2026-10-08 verification identified missing Windows Winsock initialization,
not an established virtual-adapter limitation. The bridge now balances
WSAStartup/WSACleanup per factory. Native tests pass with normal network defaults
and with optional loopback diagnostics; full WebRTC-enabled Debug and Release
regressions also pass. Signaling in the native test is in-memory; the separate
loopback TCP diagnostic tests are not evidence of WebRTC connectivity. A combined
live-Tracker/desktop visual acceptance run and later two-host/network validation
remain required before public deployment. The 2026-10-09 follow-up passed separate
formal-engine processes with a live Tracker, bidirectional and multi-target native
sync, crash/resume, Source/Tracker restart, and local forced TURN/UDP and TURN/TCP
relay. This uses test-only loopback diagnostics for a loopback TURN endpoint.

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

## ICE configuration and recovery

Configure the engine process before launch (credentials are not persisted or logged):

```powershell
$env:VERITASSYNC_ICE_URLS = 'stun:stun.example.net:3478;turn:turn.example.net:3478?transport=udp'
$env:VERITASSYNC_TURN_USERNAME = '<temporary username>'
$env:VERITASSYNC_TURN_CREDENTIAL = '<temporary credential>'
$env:VERITASSYNC_ICE_RELAY_ONLY = '0'
```

Up to 16 STUN/STUNS/TURN/TURNS URLs are accepted. TURN requires a username and
credential; relay-only mode (`1`) requires at least one TURN URL. Empty configuration
uses host candidates only. A disconnected initiator requests an SDP ICE-restart
offer; unsuccessful or timed-out connections are rebuilt with bounded backoff.
Targets connect only to the Source, and an unavailable Target does not block the
other Source sessions. Bridge ABI 2 adds ICE configuration and restart exports;
ABI 1 DLLs fail explicitly and must be rebuilt with the matching source. Configuration
and restart signaling have automated tests; local native DataChannel transfer,
restart renegotiation, process-crash recovery and local TURN/UDP/TCP also pass.
Actual network-outage recovery, cross-network behavior and TURN/TLS are not yet
accepted. See `LOCAL_VERIFICATION.md` for the current results.
