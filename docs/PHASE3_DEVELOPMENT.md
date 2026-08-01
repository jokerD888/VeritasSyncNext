# Phase 3: one authoritative source to multiple targets

Phase 3 is implemented in `sync::MultiTargetSource`.  It owns the source task
root and creates one scanned manifest revision for all connected targets.  It is
deliberately separate from `OneWaySyncNode`, which remains the target-side
session implementation and the Phase 2 one-source/one-target compatibility path.

## Ownership and scheduling

- `RefreshSource()` scans and reconciles the authoritative source exactly once,
  then sends the same immutable manifest revision to every hello-complete target.
- Every target has its own `Transport`, request-id sequence, `UploadSession`
  collection, error state, byte statistics, and buffered-amount check.
- `Pump()` takes at most one eligible frame from each pending upload session per
  target.  A target whose bulk `BufferedAmount` is above the session threshold
  pauses only that target's queue; it does not prevent frames for another peer.
- Every target independently uses the existing durable target workflow:
  manifest diff, resume request, hash verification, atomic replacement, and
  tombstones.

This preserves a single source scan while avoiding a shared bulk queue that
would allow a slow peer to cause head-of-line blocking for the other targets.

## One-way target policy

The policy is enforced on the three Phase 3 boundaries.

| Boundary | Enforcement |
|---|---|
| Tracker contract | A one-way room accepts one `source` and any number of `target` members. A target may answer a source offer and send ICE candidates, but cannot create an offer or ICE restart, or signal another target. |
| Control/bulk protocol | The source accepts only HELLO, file requests, and cancellation from a target. A target manifest or chunk is rejected as `target_write_forbidden`. The existing target-side node also rejects outbound-source-only frame types. |
| Local watcher/scanner | `CanApplyLocalWatcherChange` and the CLI scan gate return false for a `one_way` task with role `target`; local modifications therefore cannot create an outbound source revision. |

## Automated acceptance evidence

`MultiTargetSourceSharesOneSnapshotAndIsolatesSlowTarget` creates one source and
two local targets over independent mock transports. It proves all of the
following in one execution:

1. Exactly one source scan serves both targets.
2. Both targets receive the shared manifest and request the same source file.
3. A forced bulk-channel backpressure condition on Target A leaves it pending.
4. Target B still receives, verifies, and atomically commits the file.
5. Target A converges after its own backpressure clears.

`MultiTargetSourceRejectsTargetManifestWrites`, `TrackerContractKeepsOneWayTargetsReadOnly`, and `TaskPolicyForbidsOneWayTargetLocalScanning` cover the protocol, Tracker, and local-watcher read-only boundaries respectively.

Run the complete suite with:

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default --output-on-failure
```

Phase 3 does not add a network Tracker service, coturn deployment, or a
cross-network libwebrtc acceptance test. Those remain Phase 1 production
integration work, outside the one-way multi-target synchronization semantics.
