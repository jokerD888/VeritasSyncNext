# VeritasSyncNext

VeritasSyncNext is a C++20, headless synchronization-engine implementation. Its
current milestones provide the versioned protocol, durable SQLite state, a mock
transport, and the Phase 1 libwebrtc bridge/signaling boundary. A production Tracker
and cross-network WebRTC DataChannel validation remain pending.

The storage layer also exposes a Phase 2 safety primitive: received files are written
as same-directory `*.part` files, flushed, then atomically replaced under the task
root. It rejects absolute paths and traversal before writing.

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
