# VeritasSyncNext desktop shell

The desktop application is a Tauri 2 shell around the independent C++ engine.
It never opens the engine SQLite database itself. On launch it connects to the
versioned `VSYNC_IPC/1` named pipe and starts a detached engine server only if
that pipe is unavailable. Closing or restarting the UI therefore does not stop
an already-running engine.

## Development prerequisites

- Rust stable plus the MSVC build tools (`cargo` must be on `PATH`)
- Node.js is only needed for optional linting; the included UI is plain static
  HTML/CSS/JavaScript and has no npm dependency
- Build the C++ engine first with `cmake --build --preset default`

Stage the engine sidecar and run Tauri:

```powershell
./scripts/stage-desktop-engine.ps1
cd desktop/src-tauri
cargo tauri dev
```

Release packaging, Windows signing, and updater publication are described in
[`deploy/windows/README.md`](../deploy/windows/README.md). They require the
release owner to provide a certificate, Tauri update signing key, and HTTPS
release endpoint; no development key is committed to this repository.
