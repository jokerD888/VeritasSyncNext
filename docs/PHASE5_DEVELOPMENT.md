# Phase 5: desktop product shell

The desktop product is intentionally split into two independently upgradeable
processes:

```text
Tauri desktop shell ── VSYNC_IPC/1 named pipe ── veritassync-engine.exe
```

The shell never opens the engine database and never scans or writes a sync root.
The engine owns SQLite migrations, task state, conflict state, and events.

## Desktop workflow

`desktop/ui` is a React + TypeScript Tauri webview interface with:

- task overview plus a new-task wizard for one-way and bidirectional modes;
- an explicit per-task scan/recover control that delegates local reconciliation
  to the engine;
- native folder selection;
- task deletion (without deleting the selected local root);
- persisted engine status/event log polling;
- conflict review and a controlled “mark resolved” action;
- a per-task ignore-policy editor with deterministic dry-run, revision undo,
  tracked-file deletion warnings, and optional AI-assisted proposals;
- AI Provider settings whose API Key remains in Windows Credential Manager;
- tray actions to show the window or exit only the desktop shell.

The UI uses `ipc_request` for Engine operations and narrow Rust commands for the
AI Provider and updater. It does not access files, SQLite, credentials, or the
sidecar process directly from JavaScript.

## Versioned local IPC and lifecycle

`engine/ipc/IpcService` accepts one bounded UTF-8, percent-escaped
`VSYNC_IPC/1` request per Windows message-mode named-pipe connection. Supported
commands are `ping`, `status`, `list_tasks`, `create_task`, `delete_task`,
`scan_task`, `ignore_get`, `ignore_context`, `ignore_preview`, `ignore_apply`,
`ignore_undo`, `list_conflicts`, `resolve_conflict`, `list_events`, and `shutdown`.

On launch, the Rust shell pings the per-user pipe. If unavailable, it starts
`veritassync-engine --ipc-serve` as a detached process and waits for its ping.
Subsequent shell launches reuse the server. A request failure attempts one
engine re-discovery/restart; no UI restart deletes or migrates state itself.
The message-mode pipe uses a protected owner-only Windows DACL, so another local
account cannot connect merely by knowing its name.

Migration 4 adds `engine_events`, a durable, bounded-query event source for UI
status/log rendering. Task deletion removes task-owned transfer/version/conflict
state in one transaction, leaves sync-root files untouched, and preserves prior
events with their task reference cleared.

Migration 5 adds immutable `ignore_policy_revisions` plus per-task
`ignore_policy_state`. See [`IGNORE_RULES.md`](IGNORE_RULES.md) for the security,
privacy, preview, topology, and atomic-write contract.

## Packaging, signing, and updates

`scripts/stage-desktop-engine.ps1` stages the C++ executable and its adjacent
runtime DLLs as a Tauri runtime resource; the full resource directory is the
sidecar's preferred launch location. `deploy/windows/build-release.ps1` first
builds/tests the engine, generates an updater config only from CI environment
secrets, stages the sidecar, invokes `cargo tauri build`, and Authenticode-signs
and verifies the engine plus MSI/NSIS artifacts.

The updater endpoint/public key and certificate material are deliberately not
in source control. See [`deploy/windows/README.md`](../deploy/windows/README.md)
for the required CI secrets and publication ordering.

## Current verification

- C++ engine build and test suite pass.
- `IpcService` unit tests cover protocol rejection, task lifecycle, and ignore-policy
  get/context/preview/apply/topology enforcement.
- A Windows named-pipe smoke test starts `--ipc-serve`, receives `ping`, and
  performs a controlled `shutdown`.
- Vitest covers IPC frame parsing and deterministic AI-candidate merging; TypeScript
  and Vite validate and build the production UI.
- Rust stable MSVC (`rustc/cargo 1.97.1`) builds the desktop shell with
  `cargo check`; Tauri creates both MSI and NSIS release bundles.
- A desktop smoke test verifies sidecar startup from the packaged resource
  directory, `VSYNC_IPC/1` ping, and watchdog recovery after the engine is
  forcibly terminated.
- Trusted signing and a live updater publish additionally require the
  release-owner credentials and HTTPS endpoint.
