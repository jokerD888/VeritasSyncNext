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
