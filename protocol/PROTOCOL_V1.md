# VeritasSync application protocol v1

This document defines the Phase 0 application boundary. Its binary encoding is
little-endian for all multi-byte integers. It is independent of the eventual
DataChannel implementation.

## Shared frame envelope

Every control and bulk message is one binary frame:

```text
magic[2] = 'V' 'S' | protocol_version:u8 | type:u8 |
request_id:u64 | payload_length:u32 | payload[payload_length]
```

`protocol_version` is `1`; receivers reject unknown versions. The maximum encoded
frame is 16 MiB, including its 16-byte envelope. A decoder must reject bad magic,
unknown type, oversized payloads, and trailing/truncated bytes before dispatch.

Control frames use `HELLO (1)`, `MANIFEST (2)`, `ERROR (3)`, `HEARTBEAT (4)`,
`FILE_REQUEST (5)`.
Bulk frames use `CHUNK (64)`, `CHUNK_ACK (65)`, `WINDOW_UPDATE (66)`. A transport
must not accept a control type on the bulk channel or vice versa.

## HELLO v1 payload

```text
task_id:string | role:u8 | device_id:string | device_fingerprint:string |
authorization_digest:string
```

Strings have a `u16` byte length. Before any manifest or block is accepted, a node
checks protocol version, task id, expected topology role, and authorization digest.
Phase 0 treats the digest as an opaque test credential; later phases replace it with
device signatures and group authorization.

## MANIFEST v1 payload

```text
revision:u64 | entry_count:u32 |
  relative_path:string | size:u64 | content_hash:string   (repeated)
```

The Phase 0 manifest is a snapshot test payload, not yet a file scanner or a
version/conflict model.

## CHUNK v1 payload

```text
transfer_id[16] | file_hash[32] | offset:u64 | chunk_length:u32 |
chunk_hash[32] | bytes[chunk_length]
```

The initial logical chunk target is 256 KiB. A future DataChannel adapter may slice
an encoded chunk to meet its message limit, but must reconstruct this application
frame before handing it to the engine. A receiver verifies that `chunk_length` equals
the remaining payload size and that the chunk hash matches its bytes before writing.

## FILE_REQUEST v1 payload

```text
transfer_id[16] | file_hash[32] | range_count:u32 |
  first_chunk:u64 | chunk_count:u32  (repeated)
```

This requests missing logical chunk ranges after reconnect. Ranges are ordered,
non-overlapping, and non-empty.

## Error codes

`malformed_frame`, `unsupported_version`, `wrong_channel`, `unauthorized_peer`,
`task_mismatch`, `role_mismatch`, `invalid_manifest`, `invalid_chunk`, and
`internal_error` are stable v1 error-code strings. `request_id` allows an error to be
matched with the request that caused it and is reserved for replay de-duplication in
later phases.
