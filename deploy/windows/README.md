# Windows release process

Phase 5 ships the Tauri shell and the independently runnable C++ engine as a
single MSI/NSIS bundle. The repository intentionally contains neither a code
signing certificate nor a Tauri updater private key.

## Release-owner inputs

Before producing a trusted public release, set these secret values in CI:

- `WINDOWS_CERTIFICATE_PFX_BASE64` and `WINDOWS_CERTIFICATE_PASSWORD`
- `TAURI_SIGNING_PRIVATE_KEY` and `TAURI_SIGNING_PRIVATE_KEY_PASSWORD`
- `VERITASSYNC_UPDATE_ENDPOINT`, an HTTPS URL serving Tauri update manifests
- `VERITASSYNC_UPDATER_PUBKEY`, the corresponding public key

The release pipeline must fail closed when any of them is missing. A self-signed
certificate is suitable only for a local developer install and must never be
substituted for a trusted release certificate.

## Build order

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default --output-on-failure
./scripts/stage-desktop-engine.ps1
cd desktop/src-tauri
cargo tauri build
```

The bundle includes the engine as a sidecar. The first shell launch starts the
named-pipe server; later shell launches reuse it. SQLite migrations remain owned
by the engine, so a UI upgrade cannot mutate the sync database directly.

## Signing and updates

Generate a release-only Tauri config from the environment in CI, adding the
updater endpoint and public key. Sign both `veritassync-engine.exe` and the MSI/
NSIS output with the release certificate, verify Authenticode signatures, then
publish the signed update artifacts and manifest atomically. Do not publish an
update manifest before every referenced signed artifact is available.
