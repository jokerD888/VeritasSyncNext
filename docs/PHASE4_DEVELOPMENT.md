# Phase 4: two-node bidirectional synchronization

`sync::BidirectionalSyncNode` is the two-peer state machine for a task with
`mode=bidirectional` and role `peer`. Both endpoints scan their local roots,
create versioned snapshots, exchange `VERSION_MANIFEST` frames, and use the
existing resume-capable chunk transport for any required file content.

## Durable version semantics

SQLite migration 3 adds two Phase 4 tables:

- `version_lineage(task_id, version_id, parent_version_id)` records each
  immutable parent edge.
- `task_clocks(task_id, logical_clock)` persists the Lamport clock across an
  engine restart.

Every new local write or tombstone advances the durable clock and records the
version's predecessor. Receiving a remote record also observes its clock before
any later local write can be allocated. The graph determines whether an incoming
version is a successor, an ancestor, or a concurrent branch.

For a concurrent branch, the lower `(logical_clock, origin_device_id)` owns the
formal path. The other file is retained under the deterministic sibling name
`name.conflict.<device-id>.<logical-clock>.ext`; a conflict row records the
original path, winning version, conflict path, and unresolved state. Generated
conflict copies are intentionally excluded from future local scans, so they can
never re-enter the formal-path competition. A directory wins a same-path
file/directory collision, avoiding unsafe recursive removal.

## Safety and transfer behavior

- A remote successor does not replace a file until its complete BLAKE3-verified
  download has been atomically committed.
- A losing local file is moved within the task root using the safe writer before
  a remote winner can occupy the formal path. The move rejects symlinks and never
  overwrites an existing conflict copy.
- A missing transfer resumes through the same `transfers` and `transfer_chunks`
  persistence used by Phase 2. Reconnecting peers rebuild their manifest and
  version decision from the database rather than trusting lost in-memory frames.
- Tombstones remain first-class versioned entries. In a concurrent delete/edit,
  the deterministic order decides whether the formal path is deleted or the edit
  remains; the non-winning version is still represented in the conflict log, and
  file content is preserved when applicable.

## Automated Phase 4 acceptance

`BidirectionalSyncConvergesInitialReplicaAndOfflineConcurrentEdits` verifies an
initial replica, disconnected concurrent edits, matching formal files, matching
conflict copies, and one conflict record on each peer.

`BidirectionalSyncPreservesDeleteModifyAndFileDirectoryConflicts` verifies that
a concurrent delete/edit has a deterministic winner and that a same-path
file/directory collision retains the directory and synchronizes its child.

`BidirectionalSyncConvergesOfflineBranchesAfterBothPeersRestart` records
different offline edits, destroys both sessions without delivering queued frames,
then constructs a fresh mock connection. It verifies final convergence and the
same conflict filename on both peers.

Run all checks with:

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default --output-on-failure
```
