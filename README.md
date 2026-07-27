# VeritasSyncNext

Phase 0 is a C++20, headless synchronization-engine skeleton. It deliberately has no
libwebrtc, Tauri, Tracker, or coturn dependency. The only concrete transport is an
in-memory mock used to verify the protocol handshake and transfer boundary.

## Build and test

Install vcpkg, set `VCPKG_ROOT`, then run:

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default
```

## Headless CLI

```powershell
./build/default/veritassync-engine --headless --db state.db --init-task demo --mode one_way --role source --root C:\sync
```

The CLI only initializes durable state in this phase. See
[`protocol/PROTOCOL_V1.md`](protocol/PROTOCOL_V1.md) for the wire contract.
