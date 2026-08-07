# Performance audit and benchmark

This document records the August 2026 whole-project performance pass. It is a
measurement companion to `FROM_SCRATCH_IMPLEMENTATION_PLAN.md`, not a replacement
architecture. Correctness rules such as content-hash equality, crash-safe SQLite
state, and transport backpressure were not relaxed to improve a number.

## Reproduce

Configure once, build the Release benchmark, then run it at least three times:

```powershell
cmake --preset default
cmake --build --preset benchmark
.\build\default\Release\veritassync_benchmarks.exe --items 5000
```

The benchmark creates isolated temporary databases and file trees and removes
them after each case. The reported result below is the median of three warm runs
on Windows 11 build 26200, an AMD Ryzen 7 7745HX (8 cores / 16 threads), 31.2 GiB
RAM, NVMe storage, CMake 3.31.6, and MSVC 19.44.35219.

| Case | Work | Before (ms) | After median (ms) | Result |
| --- | ---: | ---: | ---: | ---: |
| Unchanged manifest diff | 5,000 entries | 5.427 | 0.311 | 17.5x faster |
| Unchanged snapshot reconcile | 5,000 entries | 134.820 | 4.456 | 30.3x faster |
| Fully changed snapshot reconcile | 5,000 entries | 91.432 | 45.409 | 2.0x faster |
| Scheduler dequeue | 20,000 frames | 339.880 | 0.901 | 377x faster |
| Mock transport pump | 20,000 frames | 327.811 | 1.051 | 312x faster |
| Manifest encode | 50,000 entries | 6.314 | 4.650 | 1.36x faster |
| Manifest decode | 50,000 entries | 14.536 | 13.302 | 1.09x faster |
| 256 KiB chunk encode/decode | 256 chunks | 51.307 | 61.093 | noisy; see below |
| Scan and hash tiny files | 5,000 files | 1,040.632 | 1,112.254 | I/O-noise range |

The last two rows are intentionally not presented as wins. Chunk timing includes
BLAKE3 verification and varied from 46.8 to 61.9 ms in the final samples; the
implementation still removes one full payload copy in each direction. Tiny-file
scanning is dominated by filesystem metadata and file-open latency. Parallelizing
it without a device-aware I/O policy can make HDD and busy-system behavior worse.

## Changes made

- Manifest comparison and unchanged snapshot reconciliation now merge sorted
  scanner/database views in linear time instead of repeatedly searching vectors.
- Scheduler and mock-network FIFO queues use constant-time front removal.
- SQLite reuses the file-record upsert statement for large reconciliations and
  skips the transaction entirely when a scan found no changes.
- Bulk receive paths decode a non-owning frame view. Chunk sending writes the
  envelope and payload into one reserved buffer instead of constructing and
  copying an intermediate payload.
- Source file hashes are sorted once per scan and file requests use binary search,
  avoiding an entry-by-entry source scan for every requested file.
- Bidirectional local refresh uses sorted merge cursors. Manifest emission loads
  all version lineage in one query instead of one query per file.
- Desktop polling performs the real IPC command as its health probe and only
  enters the restart path after a failure. Status, tasks, events, and all conflict
  rows are returned as one consistent dashboard snapshot. A normal refresh
  therefore uses one named-pipe connection instead of `2N + 7` for `N` tasks
  (27 to 1 connection at ten tasks). Responses larger than 64 KiB are read in
  bounded chunks instead of being rejected.
- Windows packaging now explicitly builds, tests, and stages the Release C++
  sidecar. Previously the generic Debug preset path could place an unoptimized
  engine inside an otherwise Release Tauri bundle.
- Tauri's frontend hooks now run pnpm against the actual `desktop/ui` workspace;
  the former working-directory mismatch prevented an integrated Release build.
- The Rust desktop Release profile uses size optimization, ThinLTO, one codegen
  unit, abort-on-panic, and symbol stripping. On the audit machine this reduced
  the standalone shell from 16.92 MB to 7.33 MB without changing the C++ engine.
- The production React bundle is 420.10 kB JavaScript / 131.00 kB gzip and
  35.76 kB CSS / 8.10 kB gzip. At this size, code splitting would add complexity
  without addressing the measured engine/IPC bottlenecks.

## Complexity and resource boundaries

| Path | Current behavior | Bound |
| --- | --- | --- |
| Manifest diff | sorted merge | O(files) time, O(1) extra for sorted inputs |
| Snapshot reconcile | sorted merge + changed-row writes | O(files + changed rows) |
| File request lookup | hash-sorted binary search | O(log files) per request |
| Send scheduling | deque FIFO | O(1) dequeue |
| Mock delivery | deque FIFO | O(1) delivery removal |
| Desktop polling | one dashboard snapshot | O(1) IPC connections, plus returned rows |
| Full local scan | metadata walk + BLAKE3 of every included file | O(total bytes) |

The full scan must still hash content: the architecture explicitly forbids using
only size and modification time as file identity. The planned filesystem watcher
can later reduce how often unchanged trees need a full walk, while periodic full
verification preserves correctness.

## Not measured in this environment

Real libwebrtc DataChannel throughput, congestion response, packet loss, TURN
relay cost, and two-physical-node end-to-end latency were not measured in this
pass. The current machine did not provide the accepted second-node environment,
and substituting mock transport numbers for real networking would be misleading.
Before a production performance claim, run the same large-file, many-small-file,
resume, and slow-peer scenarios on two machines and record p50/p95 throughput,
CPU, peak working set, and retransmission/relay state.

## Regression policy

- Run the deterministic benchmark in Release; Debug results are not comparable.
- Compare medians from at least three runs on an otherwise idle machine.
- Treat a sustained regression above 15% in manifest diff, reconciliation,
  scheduler, or mock transport as requiring investigation.
- Treat scanner and chunk-codec changes as significant only with more controlled
  I/O and CPU sampling because their run-to-run variance is higher.
