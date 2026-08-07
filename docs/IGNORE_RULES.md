# Ignore rules

Each task root may contain `.veritasignore`. Rules are evaluated by the scanner before
hashing or transfer scheduling. The last matching user rule wins; a rule beginning
with `!` restores a path ignored by an earlier user rule.

Supported patterns are Git-ignore style: `*`, `?`, `**`, `[abc]`, `[a-z]`, and
`[!abc]`. A leading `/` anchors a rule to the task root; a trailing `/` matches a
directory and all descendants. Rules without a slash match a name at every depth.

The engine always excludes `.git/`, `.veritasignore`, and `*.part`. User negation
cannot re-include them: partial downloads and engine metadata must never be announced
as source files.

## Editing, preview, and revisions

The desktop rule workbench never writes `.veritasignore` directly. The C++ Engine
owns the complete policy lifecycle through versioned `VSYNC_IPC/1` commands:

- `ignore_get` synchronizes the on-disk file into revision history and returns its
  content hash;
- `ignore_preview` validates a proposed policy and performs one metadata-only tree
  inventory to compare current and proposed results;
- `ignore_apply` requires the previously observed content hash, records a revision,
  and uses a flushed same-directory temporary file plus atomic replacement;
- `ignore_undo` restores the preceding revision with the same optimistic concurrency
  check.

A preview reports newly ignored and newly included paths separately. It also compares
the proposal with the durable file manifest. If an already tracked path becomes
ignored, its next source scan will create a deletion record; the desktop therefore
requires an explicit second confirmation. Inventories and sample lists are bounded,
and validation rejects more than 128 rules, more than 16 KiB total content, malformed
character classes, control characters, and oversized lines.

One-way Source tasks may edit policy. One-way Targets are read-only because their
policy belongs to the authoritative Source. A bidirectional task is also read-only
until a peer protocol provides `PROPOSE / ACK / COMMIT` policy negotiation; silently
changing one peer would otherwise produce divergent manifests.

## AI-assisted proposals

AI assistance is a proposal layer, not a synchronization authority:

```text
React request
  -> C++ Engine builds a bounded context
  -> Rust sends HTTPS to an OpenAI-compatible provider
  -> Rust accepts strict JSON only
  -> React merges the candidate into a draft
  -> C++ Engine validates and previews
  -> explicit user confirmation
  -> C++ Engine atomically applies a revision
```

The Provider endpoint, model, and JSON-mode preference are stored by the Tauri store
plugin. The API Key is stored only as a generic Windows Credential Manager secret
under `VeritasSyncNext/AIProvider`; it is never returned to JavaScript and is not
written to SQLite, a JSON settings file, diagnostic logs, or the synchronization
root. Non-loopback endpoints must use HTTPS and redirects are disabled.

Two context modes are available:

- **Private** sends the user request plus aggregated depth/extension metadata; it
  does not send existing rule text because those rules may contain private names.
- **Precise** additionally sends existing rule text and bounded relative-path
  samples selected by the Engine. It still never sends file contents or absolute
  paths.

Directory names and sample paths are treated as untrusted prompt data. Provider
output must be one JSON object containing exactly `rules` and `explanation`; markdown,
extra fields, multiline rules, oversized output, and unsupported rules are rejected.
AI output is never applied automatically.
