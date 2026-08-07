# Tracker protocol v1

The Tracker exposes a deliberately narrow HTTPS API. It stores no file names,
manifests, hashes, or chunks. Request and response bodies are UTF-8 tab-separated
fields with percent escaping and content type `application/x-veritassync-v1`.

Every POST request carries:

```text
X-VeritasSync-Device-Id
X-VeritasSync-Public-Key
X-VeritasSync-Timestamp
X-VeritasSync-Nonce
X-VeritasSync-Signature
```

The signature is Ed25519 over:

```text
POST\n<path>\n<unix-seconds>\n<nonce>\n<lowercase-blake3-body-hash>
```

The device id is the lowercase hex encoding of the first 16 bytes of
`BLAKE3(public_key)`. Timestamps have a five-minute window and a nonce may only
be used once. Signaling calls additionally require a short-lived
`X-VeritasSync-Session` token.

## Endpoints

- `POST /v1/rooms/create`: create a room and single-use ten-minute invitation.
- `POST /v1/invitations/redeem`: authorize the signed device and consume the invitation.
- `POST /v1/rooms/join`: renew a session for an existing, non-revoked member.
- `POST /v1/signals/send`: enqueue one Offer, Answer, ICE candidate, or ICE restart message.
- `POST /v1/signals/drain`: atomically receive and remove up to 256 queued messages.

Sessions expire after fifteen minutes and are renewed through `rooms/join`.
One-way rooms accept one Source and any number of Targets; only the Source may
initiate Offer/ICE restart. Bidirectional rooms accept exactly two Peers.
