# VeritasSync Tracker

The Tracker is an independent Rust service. It stores room membership,
single-use invitations, short-lived sessions, replay nonces, and queued
Offer/Answer/ICE messages in SQLite. It never accepts file manifests or chunks.

```powershell
cd tracker
$env:VERITASSYNC_TRACKER_BIND = "127.0.0.1:8787"
$env:VERITASSYNC_TRACKER_DB = "$PWD/dev-tracker.db"
cargo run
```

`GET /healthz` returns `ok`. Signed protocol requests use
`application/x-veritassync-v1`; the wire contract is documented in
`docs/TRACKER_PROTOCOL_V1.md`.

The built-in listener is HTTP. It deliberately defaults to loopback. For any
non-loopback deployment, terminate HTTPS at a reverse proxy and forward only to
the loopback listener. The Engine client rejects plain HTTP for non-loopback
hosts.
