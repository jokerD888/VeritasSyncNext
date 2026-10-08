# Local verification — updated 2026-10-09

Cross-network testing is explicitly deferred at the user's request. This record
distinguishes verified behavior from implementation that still needs its native
runtime or release credentials. Passing these checks is not a public-release claim.

## Completed implementation

- Bidirectional watcher scans create causal versions and durable Lamport clocks;
  startup scans detect offline edits, concurrent remote writes cause a rescan, and
  conflict copies are not re-announced as normal files.
- Bidirectional ignore apply/undo negotiates a durable peer transaction with
  retries after reconnect/restart. Divergent initial policies and concurrent
  proposals fail safely. Ignored local copies are retained.
- ICE server configuration, restart signaling, connection timeout/backoff, and
  Source-only Target topology are implemented. WebRTC frames are fragmented and
  bounded for unordered-channel reassembly and send backpressure (bridge ABI 2).
- Task metrics include rates, current-session byte totals, connected peers,
  pending files, queued bytes, and actual active-download progress. Duplicate
  chunks do not inflate progress.
- UTF-8 paths are preserved through Windows CLI/IPC/scans/conflict paths. Safe
  writes reject traversal, reserved device names, alternate streams, and unsafe
  trailing characters. Tasks cannot include their own state database.
- Windows login startup is opt-in, with hidden startup and normal tray operation.
  Build/CI/release scripts include the bridge and local diagnostic integration test.
  Updater artifacts are re-signed after Authenticode modifies the installers.

## Executed checks

| Check | Result and scope |
| --- | --- |
| CMake Debug build and `ctest --preset default --output-on-failure` | Passed with native WebRTC enabled, including four bridge tests and engine regressions for policy restart phases, causal scans, topology, fragmentation, IPC, path safety and metrics |
| CMake Release build and `ctest --preset default -C Release --output-on-failure` | Passed with native WebRTC enabled, including real DataChannel file transfer; earlier two-process TCP integration with `-BuildDirectory build/default/Release` also passed |
| `./scripts/test-local-sync.ps1` | Passed two-process loopback TCP integration: one-way/bidirectional changes, deletions, 12 MiB binary forcibly interrupted mid-transfer and resumed, empty file, Chinese/emoji roots and filenames, pause/resume, restart recovery, offline concurrent conflicts, ignore apply/undo |
| Tracker `cargo test --locked` | 3 tests passed |
| Local live Tracker plus C++ pairing round-trip test | Passed real loopback HTTP create/join/admission/relay flow |
| Desktop `cargo test --locked` | 12 tests passed, including Unicode IPC, AI request validation, audit/production isolation, and a temporary-registry autostart round-trip without modifying the real Windows Run key |
| UI `pnpm lint`, `pnpm test`, `pnpm build` | Passed TypeScript checks, 16 tests, and production build; interaction tests additionally cover policy edit permissions, risk confirmation, undo and unapplied private AI drafts |
| PowerShell parsing and `git diff --check` | Passed |

## Native follow-up — 2026-10-08

The pinned checkout is now available at
`D:\tmp\veritassync-native-tools\webrtc\src` and ABI 2 bridge compilation succeeded.
The build script now discovers non-default Visual Studio locations using vswhere.
Native compilation caught and fixed an inaccessible thread-status API and unsafe
raw array indexing at the C ABI boundary. Four native tests passed: factory/ABI
lifecycle, SDP offer generation, two-peer SDP/ICE relay, and real DataChannel
connection/transfer with ICE-restart renegotiation.

The initial real-connection failure was traced to missing Windows Winsock
initialization in the bridge (`WSANOTINITIALISED`, 0x276D). Candidate exchange
alone had concealed UDP/TCP socket creation failures. Each factory now balances
`WSAStartup`/`WSACleanup`. A VPN/virtual adapter was not established as the cause.

`WebRtcDataChannelsTransferLargeFramesAndRestartIce` now passes with normal
production network defaults and also with optional
`VERITASSYNC_WEBRTC_ALLOW_LOOPBACK=1` diagnostics. It runs two real native
PeerConnections in one process, exchanges signaling through an in-memory relay,
and sends actual bytes over libwebrtc DataChannels (not the TCP diagnostic adapter).
It verifies ordered control frames and a fragmented 256 KiB bulk frame in opposite
directions, ICE-restart SDP with changed credentials and subsequent frame delivery,
then production one-way synchronization of a 12 MiB + 7 byte binary file with a
Chinese/emoji filename, an empty file, BLAKE3 equality, and deletion propagation.
This is not an induced network-outage recovery test or a combined live-Tracker/
desktop end-to-end acceptance run (the separate formal-engine run below covers the
live Tracker and connection manager). Full WebRTC-enabled Debug and Release suites
passed after the socket-initialization fix.
The final Release native transfer test passed five consecutive runs with normal
network defaults and one additional run with loopback diagnostics enabled.

To reproduce the native check using the installed checkout:

```powershell
cmake --preset default -DVERITASSYNC_ENABLE_WEBRTC=ON -DVERITASSYNC_WEBRTC_ROOT=D:/tmp/veritassync-native-tools/webrtc/src -DVERITASSYNC_WEBRTC_BRIDGE_LIBRARY=D:/tmp/veritassync-native-tools/webrtc/src/out/veritassync/veritassync_webrtc_bridge.dll
cmake --build --preset default --parallel 4
ctest --preset default --output-on-failure
```

The 100,000-small-file Release benchmark also passed after separating its database
from the sync root: scan/hash took 9.25 seconds, unchanged reconciliation 97.96 ms,
and changed reconciliation 659.00 ms. One-million-entry codec checks passed, but
these do not establish million-file network synchronization or framed transfer.

## Reproduce local integration

```powershell
cmake --preset default
cmake --build --preset default --parallel 4
ctest --preset default --output-on-failure
./scripts/test-local-sync.ps1
```

The integration script starts two `veritassync-local-peer.exe` processes with
separate temporary roots, databases, pipes and logs. It cleans up its processes
but retains diagnostic files in the reported temporary directory. This executable
is test-only and is not staged into the desktop release. It uses production
sync/storage/runtime/IPC code with a real TCP stream adapter, not native WebRTC,
the production Tracker session manager, or two complete desktop application windows.

The latest successful integration run retained its logs at
`C:\Users\joker\AppData\Local\Temp\veritassync-e2e-c592e279d0254928be78068496145dcb` (Release).

## Remaining environment-dependent validation

- Cross-host/network, real NAT traversal, TURN/TLS, network switching and induced
  packet loss remain deferred. Local TURN/UDP and TURN/TCP results below do not
  establish behavior across routers, firewalls, public servers or different ISPs.
- Trusted signing and real installation/upgrade/updater download still require
  release credentials, an endpoint, and an agreed installation environment. The
  unsigned MSI/NSIS builds below were not installed on this machine.
- Logout/login startup and a complete two-window desktop visual acceptance remain
  unverified. Computer-use interaction was stopped by the user and has not resumed.
  Registry round-trips and component interaction tests do not replace those checks.
- The bounded 10-minute native run and 100,000-file benchmark are not evidence of
  multi-day production stability or million-file network synchronization.

## Formal-engine acceptance — 2026-10-09

`scripts/test-native-sync.ps1` launches a real local Rust Tracker and two or three
separate **production** `veritassync-engine.exe` processes, each with isolated
roots, databases, pipes and persistent test identities. It drives the same IPC
used by the desktop, signed pairing, WinHTTP relay, NetworkSessionManager, native
WebRTC and real filesystem watchers. It does not replace networking with a mock
or TCP adapter and does not launch or control desktop windows.

Passed scenarios:

- Bidirectional Unicode copy and reverse watcher edit, pause/resume with channel
  reconnection, negotiated ignore apply/undo, ignored-file retention, simultaneous
  offline edits after both engines restart, conflict copies and deletion.
- Source plus two Targets, with source peer count 2 and each Target peer count 1.
  The second target joins after the first is connected. An empty file and ignored
  file checks pass; a 64 MiB + 7 byte Unicode-named binary is SHA-256 identical.
- Target forcibly killed after at least 16 MiB of partial-file writes, while the
  other Target continues receiving both the binary and a new file. The restarted
  Target reuses durable chunks rather than downloading the entire file again.
  In the latest host-only run its resumed-session receive count was 50,353,333
  bytes for a 67,108,871-byte file; no `.part` remains after commit.
- Source-process crash/restart and Tracker-process restart recover automatically,
  preserve identity/room membership, and deliver newly created files to both targets.
- Release bounded stability run: **600 seconds, 500 successive file revisions**,
  both targets converge after every revision with no unexpected error event during
  the monitored interval. Latest full-code Debug and Release short runs also pass.

Integration exposed and fixed peer lifecycle issues: membership updates and a
single target's connection timeout now preserve healthy peer sessions; issuing
another invitation does not rebuild an already paired task. Changed remote DTLS
certificates replace that individual native connection instead of reusing stale
channels. Detaching the last target and later reconnecting refreshes the source
snapshot. Native signaling callbacks retain independent state during teardown.

The production identity target is unchanged by default. `--identity-target`
permits isolated local instances; differently packaged desktop apps also isolate
their identity, pipe and autostart value. Synthetic test credentials are removed
after each run, but fixture databases, files and logs are retained for diagnosis.

```powershell
./scripts/test-native-sync.ps1 -BuildDirectory build/default/Release -WebRtcBridge D:/tmp/veritassync-native-tools/webrtc/src/out/veritassync/veritassync_webrtc_bridge.dll -SoakSeconds 600
```

The 600-second run retained artifacts at
`C:\Users\joker\AppData\Local\Temp\veritassync-native-e2e-0e83690c311e40dc89d9e662ba4aa071`.
The subsequent full-code host-only run retained artifacts at
`C:\Users\joker\AppData\Local\Temp\veritassync-native-e2e-ef17642a98424df99bc18bfe02475d7f`
(final callback-lifetime implementation).

## Same-machine TURN acceptance — 2026-10-09

The existing Ubuntu-22.04 WSL Docker daemon allowed local coturn testing without
installing a new runtime or changing firewall/WSL settings. The temporary coturn
4.6.3 container binds only loopback and is removed at completion; unrelated
containers are untouched. The image remains cached for repeat tests.

Both TURN/UDP and TURN/TCP **relay-only** transfer passed, including multi-target
64 MiB files, interrupted-transfer resume and Source/Tracker restart recovery.
The TURN/UDP run also passed the complete bidirectional scenarios. Initial local
attempts failed when WebRTC bound a non-loopback source interface to a loopback
TURN endpoint; enabling the bridge's existing loopback diagnostic mode resolved
that mismatch. Normal production defaults are unchanged. No TURN/TLS certificate,
public relay or actual cross-network traversal was tested.

```powershell
./scripts/test-local-turn.ps1 -WebRtcBridge D:/tmp/veritassync-native-tools/webrtc/src/out/veritassync/veritassync_webrtc_bridge.dll -IncludeBidirectional
```

Successful diagnostic TCP-relay artifacts:
`C:\Users\joker\AppData\Local\Temp\veritassync-native-e2e-9a10b299840d4a1c84bac040fd217611`.
Successful full UDP-relay artifacts:
`C:\Users\joker\AppData\Local\Temp\veritassync-native-e2e-6fc8104422cf45a1aa3c25345f5af2a3`.
Final-code Release relay-only matrix also passed using the reusable wrapper:
UDP artifacts `C:\Users\joker\AppData\Local\Temp\veritassync-native-e2e-3bfec2b5c3584f1caccbd7907b54dac2`,
TCP artifacts `C:\Users\joker\AppData\Local\Temp\veritassync-native-e2e-b9bd53b449dc4e9787f7e79ccbe2b130`.

## Unsigned package build — 2026-10-09

The latest desktop Release/custom-protocol binary and both Tauri bundles build.
The CLI runs hooks from the detected frontend workspace; duplicated `--dir ui`
was removed from the hooks. Staging now supports `-SkipDevelopmentResources` so
release packaging does not overwrite a running Debug fixture's engine.
The MSI file table contains the desktop executable, engine, ABI 2 bridge and
all three runtime libraries (BLAKE3, libsodium, SQLite). Both artifacts are
explicitly **NotSigned**, were not installed, and are not public-release builds.
MSI `MsiFileHash` records additionally match both latest staged engine copies and
the native bridge, so the file table is not merely evidence of matching names.
The trusted-release script now runs formal-engine native acceptance before packaging;
that full signed script still requires the release-owner inputs and was not executed.

- `desktop/src-tauri/target/release/bundle/nsis/VeritasSync Next_0.1.0-2_x64-setup.exe`
- `desktop/src-tauri/target/release/bundle/msi/VeritasSync Next_0.1.0-2_x64_zh-CN.msi`
