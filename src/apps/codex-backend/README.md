# Codex backend reference server

`codex-backend` owns BackendCore, the Codex App Server process, and the
application-private SNode.C transport adapters. FrontendService remains the
single Frontend Protocol authority for every enabled Unix, TCP, TLS, RFCOMM,
WebSocket, and WSS listener.

When a complete same-occurrence event projection exceeds the bounded batch
limits, FrontendService sends one bare Snapshot to an already Ready connection.
That message is the Frontend Protocol v1 live snapshot barrier: it replaces
client state atomically, has no following `sync.complete`, and does not stop the
backend or provider. Expanded thread-list metadata uses the compact
`threadList.updated` family; the server does not re-expand all retained threads
or fabricate an empty-page thread.

Expanded exact-entity projection resolves only the identity carried by the
canonical occurrence. Thread/turn/item paths use their reviewed nested IDs and
parents; process, filesystem-watch, fuzzy-search, and activity paths use their
stable wrapped handles/keys; notice projection carries the triggering notice
occurrence. No branch may substitute a first/last retained entity or discover
identity through an arbitrary recursive `id` search. If exact identity cannot
be proven or its target cannot be safely resolved, FrontendService requests the
existing bounded Snapshot fallback instead of fabricating another entity.

This rule repairs the observed page projection in which 25 distinct
`thread/list` IDs became 25 copies of `snapshot.threads.back()`. A 25-thread
page now produces at most 25 exact unique `thread.upserted` occurrences plus
one compact `threadList.updated`; unrelated retained threads are not repeated.
Live and replay pass the same canonical records through the same scope filter.
The protocol remains `snodec.codex-frontend` version 1 with eight message kinds,
105 methods, and 26 expanded event families.

Notification mapping is transition-specific: `thread/deleted` becomes
`thread.removed`; only the five true accumulated-content delta notifications
(agent message, command output, file-change output, reasoning-summary text,
and reasoning text) become `item.content.updated`. Item lifecycle, patch,
progress, plan, terminal-interaction, and summary-part notifications become
`item.upserted` instead of fabricating a content-channel replacement.

The `commandOutput` ceiling follows the exact item type: command-execution
items require command-execution scope and file-change items require
filesystem-write scope. This semantic walk is independent of the bounded
generic rule count, so later items cannot escape projection when a snapshot is
large; unknown or conflicting types fail conservatively.

A normal command `response` with `ok:false` completes only that request. It is
not a FrontendService close request and does not stop BackendCore or the Codex
App Server. Physical closure remains separately driven by transport failure or
a closing protocol/state error.

Native JSONL and WebSocket adapters preserve a bounded, control-character-safe
FrontendService close reason for diagnostics before closing only that physical
connection. They do not log complete protocol objects, credentials, command
parameters, or secret reverse-response data.

The complete configuration, authentication, transport, and lifecycle guide is
in [the Codex backend reference-app documentation](../../../docs/ai/openai/codex/codex-backend-reference-app.md).
