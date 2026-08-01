# VeritasSyncNext desktop shell

The desktop application is a Tauri 2 shell around the independent C++ engine.
It never opens the engine SQLite database itself. On launch it connects to the
versioned `VSYNC_IPC/1` named pipe and starts a detached engine server only if
that pipe is unavailable. Closing or restarting the UI therefore does not stop
an already-running engine.

## Development prerequisites

- Rust stable plus the MSVC build tools (`cargo` must be on `PATH`)
- Node.js 20+ and `pnpm` for the React + TypeScript + Vite workspace
- Build the C++ engine first with `cmake --build --preset default`

Stage the engine sidecar and run Tauri:

```powershell
./scripts/stage-desktop-engine.ps1
cd desktop/ui
pnpm install
pnpm lint
pnpm test
cd ../src-tauri
cargo tauri dev
```

The Tauri development command starts Vite automatically at `127.0.0.1:1420`.
For a production frontend artifact, run `pnpm build` in `desktop/ui`; Tauri
does this automatically before a package build.

## Desktop integration

The React UI uses Radix primitives, shadcn-style local components, and Lucide
icons. It retains the existing Rust IPC bridge and the independent C++ engine:
the UI never reads the SQLite database or sync roots directly.

- `window-state` restores the main window's size, position, and maximized state.
- `single-instance` focuses the existing main window on a second launch.
- `dialog` supplies the native folder picker used by task creation.
- `notification` delivers optional Windows notifications for completed scans,
  created tasks, and engine availability failures.
- `store` persists desktop-only preferences such as the notification toggle.
- `log` writes desktop-shell and frontend diagnostics outside the sync database.

The window uses the Windows 11 Mica backdrop only at the shell level. Content
panels are deliberately opaque and high contrast, so sync data remains readable
and the visual effect does not obscure operational state.

Release packaging, Windows signing, and updater publication are described in
[`deploy/windows/README.md`](../deploy/windows/README.md). They require the
release owner to provide a certificate, Tauri update signing key, and HTTPS
release endpoint; no development key is committed to this repository.

The shell checks for updates only when the user selects **检查更新**. A found
release is downloaded and verified against the embedded Tauri public key before
Windows starts its installer and exits the current shell.
