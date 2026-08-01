# VeritasSyncNext

VeritasSyncNext is a C++20, headless synchronization-engine implementation. Its
current milestones provide the versioned protocol, durable SQLite state, a mock
transport, the Phase 1 libwebrtc bridge/signaling boundary, and Phase 2/3 one-way
synchronization. One authoritative source can share one scanned manifest revision
with multiple independent targets; each target has its own transfer queue and
backpressure budget. A production Tracker and cross-network WebRTC DataChannel
validation remain pending.

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
./build/default/veritassync-engine --headless --db state.db --scan-task demo --device-id device-a
```

The scan command applies `.veritasignore`, computes BLAKE3 content hashes, and
reconciles durable file records. The database path must stay outside the task root,
so its WAL and temporary files can never be synchronized. See
[`protocol/PROTOCOL_V1.md`](protocol/PROTOCOL_V1.md) for the wire contract.

See [`docs/PHASE3_DEVELOPMENT.md`](docs/PHASE3_DEVELOPMENT.md) for the multi-target
source ownership, slow-peer isolation, and one-way target policy.
