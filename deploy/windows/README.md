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
cmake --build --preset default --config Release
ctest --preset default -C Release --output-on-failure
./scripts/stage-desktop-engine.ps1 -BuildDirectory ./build/default/Release
cd desktop/src-tauri
cargo tauri build
```

Release packaging must explicitly stage `build/default/Release`. The default
sidecar staging path remains Debug for `cargo tauri dev` and must not be used by
the installer pipeline.

The bundle includes the engine as a sidecar. The first shell launch starts the
named-pipe server; later shell launches reuse it. SQLite migrations remain owned
by the engine, so a UI upgrade cannot mutate the sync database directly.

## Signing and updates

Generate a release-only Tauri config from the environment in CI, adding the
updater endpoint and public key. The release script signs and verifies both
staged engine copies *before* Tauri bundles them, then signs/verifies the desktop
executable and MSI/NSIS output. It discovers the x64 `signtool.exe` from a
Windows SDK installation when it is not on `PATH`.

Publish the signed update artifacts and manifest atomically. Do not publish an
update manifest before every referenced signed artifact is available.

## GitHub Releases automation

`.github/workflows/release-windows.yml` publishes a release only for a version
tag matching `desktop/src-tauri/Cargo.toml` (for example `v0.1.0`), or when
manually dispatched. It uploads the signed MSI/NSIS artifacts and their Tauri
signatures before publishing `latest.json`. The default endpoint is:

`https://github.com/jokerD888/VeritasSyncNext/releases/latest/download/latest.json`

Configure these repository Actions secrets before triggering it:

- `WINDOWS_CERTIFICATE_PFX_BASE64` and `WINDOWS_CERTIFICATE_PASSWORD` from a
  trusted code-signing certificate;
- `TAURI_SIGNING_PRIVATE_KEY` (the contents of the protected private key) and
  `TAURI_SIGNING_PRIVATE_KEY_PASSWORD`;
- `VERITASSYNC_UPDATER_PUBKEY` (the matching public-key string).

For a small internal beta only, manually dispatch the workflow with both
`prerelease` and `allowUntrustedCertificate` enabled. This allows a self-signed
certificate solely to exercise the installer and Tauri update path. The build
uses the fixed `beta-channel/latest.json` feed, which is a prerelease release
asset and never replaces the stable `latest` release. Tag-triggered releases
always require a trusted certificate.
