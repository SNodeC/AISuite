# Codex Frontend Protocol v1

Codex Frontend Protocol v1 is the stable, transport-independent boundary
between a stateful SNode.C Codex backend and a UI, CLI, editor integration, or
test client. Its protocol identity is exactly `snodec.codex-frontend` and its
version is the JSON integer `1`. The public constants are
`ai::openai::codex::frontend::ProtocolIdentity` and `ProtocolVersion`.

The protocol deliberately does not expose the Codex App Server protocol. The
raw protocol is highly granular: one displayed message may arrive as hundreds
or thousands of token-sized notifications. Forwarding those notifications
directly made earlier UI implementations spend excessive time decoding
messages, updating models, and repainting. It would also require every
frontend to rebuild the App Server state machine.

The reusable frontend layer instead performs this pipeline:

```text
typed App Server events
        ↓
BackendCore reducer updates canonical state immediately
        ↓
FrontendService marks a visible entity/channel dirty
        ↓
one guarded next-tick flush
        ↓
one bounded canonical record with known structured secrets and unsafe raw
provider envelopes removed, plus one global sequence
        ↓
one bounded canonical replay journal
        ↓
mandatory per-principal projection and independent bounded queues
```

This gives consumers complete snapshots, semantic updates, exact accumulated
content, replay, and command correlation without raw App Server JSON. The
frontend target depends on `ai-openai-codex-backend`, which depends on
`ai-openai-codex`; neither reusable target depends on Unix sockets or JSONL.
See [Codex BackendCore](backend-core.md) for the canonical reducer and
ownership model. The machine-readable contract is
[`frontend-protocol-v1.schema.json`](frontend-protocol-v1.schema.json).

## Additive v1 contract and generation authority

A1.7a extends the definition of Protocol v1 additively; it does not change
`ProtocolIdentity`, `ProtocolVersion`, any existing field meaning, or the eight
message kinds. The complete method catalog is exactly:

- 15 original methods plus 90 additive definitions = 105 methods;
- seven frontend-native methods: controller acquire/release, snapshot, replay,
  and provider start/stop/restart; and
- 98 non-native mappings: all 86 stable provider operations and all 12 reverse
  response/rejection commands.

Definition and runtime availability remain separate. A1.7b implements all 105
handlers, while deployment policy keeps the exact 15 filesystem/command
methods disabled by default. The default available set is therefore 90. An
unavailable method is not a raw JSON escape hatch.

The review denominator is also fixed independently of the generated
percentage. It is 148 formerly unresolved exposure decisions plus 86 existing
compatibility contracts, for 234 reviewed identities and zero final unresolved
decisions. The 148 comprise 86 stable application operations, ten stable server
requests, 35 experimental client requests, one experimental server request,
and 16 stable `ResponseItem` alternatives. The 86 compatibility contracts are
the 68 stable notifications and 18 stable `ThreadItem` alternatives. All 36
experimental requests remain denied frontend exposure; the 16 `ResponseItem`
alternatives remain genuinely not applicable because they have no runtime
backend-state path.

Generation flows in one direction:

```text
owner-approved frontend policy in app_server_surface.py
        + production protocol registry and pinned schema evidence
        -> tools/frontend/frontend-registry-source.json
        + tools/frontend/frontend-protocol-v1.schema.template.json
        -> tools/frontend/generate_frontend_protocol.py
             |-> frontend-protocol-v1.manifest.json
             |-> frontend-protocol-v1.schema.json
             `-> frontend/GeneratedProtocol.h
```

The App Server surface tool is the upstream policy authority and emits the
committed frontend-registry export. The downstream frontend generator consumes
that export; it never reparses vendored Rust, TypeScript, schemas, or a local
Codex executable to invent policy. The schema template preserves the legacy v1
contract while generation adds reviewed definitions. The manifest, complete
schema, and C++ header are generated outputs, not policy inputs, and their
currentness guard rejects hand-edited drift.

A1.7a adds exactly two installed public headers:
`frontend/GeneratedProtocol.h` for the complete generated method/contract
metadata and `frontend/Security.h` for scopes and profiles. The installed
inventory is 29 main, seven backend, and nine frontend headers, or 45 total.
A1.7b replaces `frontend/BackendAdapter.h` with
`frontend/FrontendService.h` and installs no compatibility alias, so that
inventory does not change.
Project version is `0.6.0`; all Codex libraries use SOVERSION 7 after adding
the explicit authoritative `thread.read` state effect to the public C++
surface. The protocol identity and protocol version remain unchanged;
negotiated append-v2 projection carries complete
retained command output in bounded base64 chunks without enlarging the frozen
scalar fields or depending on JSON escape expansion. After the backend's
bounded command-output window fills, append-v2 advances it with a UTF-8-safe
discard-prefix plus append delta instead of repeatedly replacing the whole
window. “Complete
retained” is still subject to BackendCore's aggregate
content and snapshot capacities; clients are never promised output the backend
has already discarded.

BackendCore's accumulated-content capacity covers both retained item text
channels and typed turn-plan explanation/step/status text. Plan replacement,
turn/thread removal, and capacity trimming update the same ledger.

Canonical client `ThreadState` now carries the authoritative current
`ExecutionConfiguration` and typed ephemeral/archive lifecycle when available.
`TurnState` carries the effective configuration captured while an accepted turn
is known to this backend process. Filesystem paths and collaboration developer
instructions remain authority-projected, so they are absent for clients without
the corresponding scopes. Historical effective settings are deliberately not
invented after a complete backend restart when the app-server cannot supply
them.

Complete upstream inventory registration, typed implementation, BackendCore
support, canonical-state support, frontend definition, runtime availability,
deployment enablement, and per-connection permission remain separate facts.
Registry presence alone never makes an App Server operation remotely callable.
See the generated [coverage report](app-server-api-coverage.md), frozen
[security decisions](app-server-security-decisions.md), and focused
[A1.7a report](a1-7a-frontend-contract.md), and runtime
[A1.7b report](a1-7b-frontend-service.md).

## C++ tagged-JSON model

A1.7a's complete C++ protocol model is a method-tagged, schema-validated JSON
contract layer. `generated::MethodParameters<Id>` and
`generated::MethodResult<Id>` distinguish all 105 frontend methods at compile
time, while their payload is retained in `nlohmann::json`. The tags provide
exact method correlation, generated metadata, schema validation, and wire
conformance. They are not yet the ergonomic, domain-typed C++ application API.

A1.7c-1 introduces `AISuite::OpenAICodexFrontendClient` with domain-oriented
façades, callback-last asynchronous operations, typed client-side state,
replay/reconnection, and no raw-JSON requirement for stable application
workflows. A1.7a therefore supplies method-tagged schema-validated protocol
types; A1.7c-1 supplies the domain-typed Frontend SDK.

## Runtime schema validation profile

The published protocol schema is a complete JSON Schema Draft 2020-12
artifact. The C++ runtime validator is not a general-purpose Draft 2020-12
implementation. It implements the exact assertion and numeric-format subset
mechanically audited as reachable from generated runtime schemas. Generation
fails if that subset gains an unsupported assertion keyword, malformed
supported assertion, unsupported format, external/non-local reference, or
unreviewed `x-aisuite-*` assertion. Local references must also use the strict
RFC 6901 escape and array-index forms accepted by the C++ runtime.

The exact supported assertion set is `$ref`, `allOf`, `anyOf`, `oneOf`, `not`,
`if`, `then`, `else`, `type`, `const`, `enum`, `properties`, `propertyNames`,
`additionalProperties`, `required`, `minProperties`, `maxProperties`, `items`,
`minItems`, `maxItems`, `uniqueItems`, `minLength`, `maxLength`, `pattern`,
`minimum`, `maximum`, `format`, `x-aisuite-sensitiveFieldNamesForbidden`, and
`x-aisuite-forbiddenNormalizedPropertyNames`. `$defs` is structural. The
mechanically present annotation-only set is `$id`, `$schema`, `default`,
`description`, `title`, `x-aisuite-frontend-contract`, and
`x-aisuite-redactionClass`.

Numeric formats are exactly `int32`, `int64`, `uint`, `uint16`, `uint32`, and
`uint64`. The current graph uses 27 of 29 supported assertions (`else` and
`minProperties` are unused), two distinct patterns, and nine bounded
`uniqueItems` sites. The maximum unique cardinality is 105, bounding the
generated pair-comparison count at 5,460. Generated regex syntax is checked,
and both exact current patterns are compiled and exercised with the C++
runtime. AISuite makes no general ECMA-262 or Draft-2020-12 regex-conformance
claim beyond that reviewed pattern set.

The generated schema is parsed once. Production validation has deterministic
limits of 128 schema-recursion levels and 4,000,000 node visits, and exceptions
are contained at the `Codec` boundary. A generated `uniqueItems: true` array is
accepted only when generation can prove a finite maximum cardinality and bound
the resulting pair comparisons.

The fixed regression corpus contains 559 generated validations. Its observed
maxima are 3,549 visits, depth 23, 1,896 resolved references, 65 evaluated
alternatives, 28 discriminator fast paths, zero unique-item comparisons, and 11
regular-expression evaluations. A valid 2,000-item expanded snapshot is
739,169 bytes and consumes 225,533 visits at depth 23, with 2,047 alternatives
and 2,010 discriminator fast paths. The generated `$defs` graph is currently
acyclic; the private test seam therefore exercises the exact 128/129 depth
boundary with a synthetic schema, while generated snapshots are tested at their
exact measured depth and again through the public codec. The generated
sensitive-field guard is separately driven past depth 128 through a real method
result; the public codec rejects it with the bounded complexity error and
remains reusable.

Unknown non-conflicting fields are deliberately accepted for additive v1
compatibility. Known fields retain full validation, while unknown values retain
safe-property-name, sensitive-field, nesting, size, and nested-value checks.
AISuite intentionally does not use `additionalProperties: false` to reject such
safe extensions at runtime; this is a compatibility policy, not generic Draft
2020-12 behavior. A1.7b supplies the separate network admission, frame bounds,
rate limiting, authentication, and connection-scope enforcement.

## Common envelope and compatibility

Every complete wire message is one JSON object containing:

```json
{
  "protocol": "snodec.codex-frontend",
  "version": 1,
  "kind": "command"
}
```

The inner entries of an `events` array are not separate wire messages and do
not repeat the common fields. Sequence numbers are unsigned 64-bit JSON
integers. IDs supplied by the backend (`sessionId` and `pendingRequestId`) are
non-empty decimal strings; frontend `requestId` values are arbitrary non-empty
strings.

Decoders tolerate unknown, non-conflicting fields and retain them as extension
fields where the public C++ message type provides an extension object. A v1
sender must not change the meaning or type of a known field. An unknown
top-level `kind`, command `method`, or input discriminator is an error rather
than an implicit raw operation.

The message kinds are:

- client to server: `hello`, `command`;
- server to client: `welcome`, `sync.complete`, `snapshot`, `events`,
  `response`, and `protocol.error`.

## Handshake and synchronization

The first client message must be `hello`. The backend creates no
`FrontendSession` before a successfully authenticated hello. The original
Hello remains schema-valid and authenticates without a credential only when
the transport policy supplies verified local trust. Other connections use the
additive bearer object:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"hello","authentication":{"scheme":"bearer","token":"secret bytes"}}
```

Authentication material is permitted only in this inbound Hello object. It is
never returned in Welcome, snapshots, events, replay, diagnostics, or the
journal, and it is not a URL, cookie, query, path, or WebSocket-subprotocol
credential. One authentication attempt is allowed per transport connection.
A failed attempt creates no BackendCore session or frontend state and closes
after one bounded generic protocol error.

An authenticated hello without `resumeAfter` requests a snapshot:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"hello"}
```

The normal response is a welcome, exactly one snapshot, then a sync marker:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"welcome","sessionId":"7","role":"observer","currentSequence":140,"syncMode":"snapshot"}
{"protocol":"snodec.codex-frontend","version":1,"kind":"snapshot","sequence":140,"state":{"backendRevision":318,"lifecycle":"ready","diagnostics":{"received":0,"recent":[]},"threads":[],"pendingRequests":[],"sessions":[{"sessionId":"7","role":"observer"}],"codexExtensions":[],"omittedCodexExtensions":0,"threadList":{"hasLoadedPage":true,"complete":false,"pagesLoaded":1,"nextCursor":"page-2"},"journal":{"oldestReplayableAfter":118,"currentSequence":140,"oldestRetainedSequence":119,"newestRetainedSequence":140},"sequenceExhausted":false}}
{"protocol":"snodec.codex-frontend","version":1,"kind":"sync.complete","sequence":140}
```

To reconnect, authenticate again and include the last fully applied frontend
sequence:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"hello","resumeAfter":140}
```

When every event strictly after 140 remains available and fits the current
batch bounds, synchronization uses replay:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"welcome","sessionId":"8","role":"observer","currentSequence":143,"syncMode":"replay"}
{"protocol":"snodec.codex-frontend","version":1,"kind":"events","fromSequence":141,"toSequence":143,"events":[{"sequence":141,"type":"turn.updated","data":{"turn":{"id":"turn-1","threadId":"thread-1","status":"inProgress","active":true,"terminal":false,"items":[],"extensions":{}}}},{"sequence":142,"type":"item.content.updated","data":{"threadId":"thread-1","turnId":"turn-1","itemId":"item-1","channel":"agentText","content":"Hello","contentTruncated":false,"droppedContentBytes":0}},{"sequence":143,"type":"item.updated","data":{"threadId":"thread-1","turnId":"turn-1","item":{"id":"item-1","type":"agent_message","status":"completed","agentText":"Hello","reasoningText":"","reasoningSummary":"","commandOutput":"","droppedContentBytes":0,"contentTruncated":false,"data":{},"extensions":{}}}}]}
{"protocol":"snodec.codex-frontend","version":1,"kind":"sync.complete","sequence":143}
```

If `resumeAfter` predates the replay floor, a retained event is oversized, or
the journal was invalidated, `syncMode` is `snapshot` and a fresh snapshot is
sent. A hello with a future sequence also falls back to a snapshot. No replay
and snapshot are mixed in one synchronization. The sequence in
`sync.complete` is the synchronization boundary the client may persist after
it has applied all preceding messages. `resumeAfter` restores only replay
position; it does not restore the old session ID, principal, scopes,
controller ownership, command correlation, or command responses.

Sequence numbers identify occurrences in the one global canonical journal.
Each authenticated connection observes an ordered subset of those occurrence
numbers, and mandatory principal filtering may make that visible subset sparse.
A forward jump therefore does not by itself prove message loss: a missing
number may belong to an occurrence outside that principal's information
ceiling. `resumeAfter` is a global journal cursor, not a per-principal visible
event index. Replay uses the reconnecting principal's current scopes and the
capabilities negotiated by that connection, so a nonempty canonical interval
may produce sparse visible events or no visible event batch at all.

After applying every preceding synchronization message, a client persists
`sync.complete.sequence` as the authoritative global cursor. That boundary may
be greater than the last visible event when the replay suffix contained only
hidden occurrences. During ordinary live delivery, a client may persist the
last fully applied visible global sequence and must accept a later forward
jump. On reconnect, the server processes any intervening hidden occurrences
and advances the durable cursor through `sync.complete`; replay availability
comes from the server's replay-floor contract, not from testing whether
`candidateSequence == previousSequence + 1`. A replay gap still uses the
existing snapshot fallback, and replay and snapshot are never mixed in one
synchronization. No filler event, tombstone, hidden-event type, omission count,
or privileged occurrence marker is exposed merely to make a visible sequence
contiguous.

The C++ Frontend SDK tracks the global replay cursor, accepts sparse visible
sequences, and persists `sync.complete.sequence` as the synchronization
boundary.

A client can synchronize again with `snapshot.get` or `events.replay`. The
server sends the command response first, then snapshot or event batches, then
`sync.complete`. Explicit replay from a future sequence fails with
`invalid_command`.

After initial synchronization, FrontendService may send one bare `snapshot`
to a connection that is already Ready when one atomic canonical occurrence
cannot fit a bounded event batch. This live snapshot barrier has no preceding
`welcome` and no following `sync.complete`. It transactionally replaces the
projected state while preserving the physical session and pending command
correlation. Its sequence becomes both the last visible sequence and the
durable reconnect cursor: a lower sequence is invalid, an equal sequence is an
authoritative idempotent replacement, and a higher sequence advances the
cursor. This barrier is not an explicit synchronization and does not produce a
synchronization-completed callback.

Before hello, any other message receives one bounded `protocol.error` and the
connection closes. Malformed JSON, a wrong identity, or an unsupported version
also closes only that frontend connection. An unsupported-version response
includes `supportedVersions: [1]`. Protocol errors never stop `BackendCore`,
the App Server, or another frontend.

### Capability and method discovery

A hello may add a `capabilities` array to request additive behavior and a
positive `capabilityVocabularyVersion` declaring the newest closed capability
name vocabulary the client can decode. A welcome may add `capabilities`,
`availableMethods`, `permittedMethods`, `serverVersion`, and
`maximumInboundMessageBytes`. All discovery fields remain optional. An older
server ignores the additive vocabulary marker under v1's unknown-field rule;
an unmarked older client receives no capability name introduced after the
original vocabulary, so both upgrade directions retain the legacy path.
The positive byte limit is the server's effective command-ingress limit for
that connection; a client uses its configured conservative fallback when an
older server omits it. Capability
advertisement contains three independent arrays:

- `defined`: the server understands the contract name;
- `implemented`: the running server has executable support; and
- `permitted`: the authenticated connection may negotiate the implemented
  mechanism.

The 19 defined capability names are:

```text
method_discovery                 security_scopes
complete_provider_operations     complete_reverse_requests
complete_backend_domains         conditional_filesystem
conditional_command_execution    dedicated_pending_requests
dedicated_notification_events    complete_thread_items
thread_read_state_effects        authenticated_frontend
scope_projected_state
provider_lifecycle               multi_transport
cpp_client_sdk                   typescript_client_sdk
browser_ui                       qt_ui
```

A1.7b implements these 13 mechanism capabilities:

```text
method_discovery                 security_scopes
complete_provider_operations     complete_reverse_requests
complete_backend_domains         conditional_filesystem
conditional_command_execution    dedicated_pending_requests
dedicated_notification_events    complete_thread_items
authenticated_frontend           scope_projected_state
provider_lifecycle
```

The invocation-policy capabilities remain implemented when their methods are
deployment-disabled; method activation is represented by `availableMethods`.
Capability category and current truth are independent. A1.7c adds
`thread_read_state_effects` to the 13 A1.7b entries, so the current runtime has
14 static service mechanisms. `multi_transport` is the one conditional
topology capability: one declared transport family yields false and more than
one yields true. Product capabilities are a third category. `cpp_client_sdk`
is build-derived true when the AISuite C++ SDK product is enabled and built;
`typescript_client_sdk`, `browser_ui`, and `qt_ui` remain false.

The implemented total is therefore `14 + topology(0|1) + product(0|1)`, not
one unconditional mechanism count. The SDK requests only the five v1
representation selectors: `complete_backend_domains`,
`dedicated_pending_requests`, `dedicated_notification_events`,
`complete_thread_items`, and `scope_projected_state`. It observes mechanism,
topology, and product facts without requesting them as representation choices.
The current SDK sends `capabilityVocabularyVersion: 1`, observes
`thread_read_state_effects` in Welcome, and opts in per eligible command instead
of adding the mechanism name to its requested representation capabilities.

For methods, `availableMethods` means implemented and deployment-enabled.
`permittedMethods` further filters that set by the authenticated principal's
static scopes. It deliberately does not depend on current controller
ownership, transient provider readiness, capacity, or a parameter-sensitive
branch. Those are invocation-time checks. With default gates, the counts are
105 defined, 105 implemented, and 90 available. `default_remote` is permitted
53/90 and `local_trusted` 90/90; the 37 remote exclusions are 22 privileged
provider operations, 12 reverse methods, and three lifecycle methods.

### Scope profiles and controller ownership

Frontend scopes have exact stable string spellings. The default remote profile
contains only `observe` and `control`. The local trusted profile contains all
12 scopes, in order:

```text
observe                   control
provider_lifecycle        account_management
configuration_write       command_execution
filesystem_read           filesystem_write
extension_management      mcp_invoke
sensitive_response        unknown_request_response
```

Scope possession and controller ownership are independent checks. Possessing
`control` does not acquire the single controller role, and acquiring that role
does not grant any scope. A method must satisfy every declared scope,
controller, deployment-enable, and provider-lifecycle requirement.

All filesystem methods and arbitrary command-execution methods are conditional
and default-disabled. This includes filesystem metadata/directory/file reads,
fuzzy search and watches, filesystem mutations, `command.exec` and its
resize/terminate/write family, and `thread.shellCommand`. Trusted BackendCore
read policy is not remote authorization. FrontendService enforces explicit
deployment enablement, required scopes, controller requirements, provider
readiness, and configured path/execution policy before any such method can
run.

`account.read` is parameter-sensitive. With `refreshToken` absent or `false`,
it is an observer read requiring `observe`. With `refreshToken: true`, it
requires `control`, `account_management`, and current controller ownership.
The sensitive result remains subject to projection and redaction policy.

## Commands and correlation

A command has this envelope:

```json
{
  "protocol": "snodec.codex-frontend",
  "version": 1,
  "kind": "command",
  "requestId": "client-42",
  "method": "turn.start",
  "params": {}
}
```

`requestId` is opaque, non-empty, and scoped to one frontend session. A second
command with the same still-pending ID receives `duplicate_request_id`.
Accepted asynchronous commands produce exactly one response while the session
remains open. Closing a session suppresses its later response but does not
cancel an already accepted App Server operation merely because the frontend
went away.

Every session starts as an observer. The following table is the exact original
15-method legacy-compatibility subset, not the complete 105-method runtime
catalog. A1.7b preserves these parameter/result bytes while activating the
remaining generated handlers. Observer commands are marked **O**; all other
legacy commands require the controller role (**C**).

| Role | Method | `params` fields |
| --- | --- | --- |
| O | `controller.acquire` | empty object |
| C | `controller.release` | empty object |
| O | `snapshot.get` | empty object |
| O | `events.replay` | required `after` sequence |
| C | `thread.start` | optional `cwd`, `model`, `modelProvider`, `approvalPolicy`, `sandboxMode`, `ephemeral` |
| C | `thread.resume` | required `threadId`; optional `cwd`, `model`, `modelProvider`, `approvalPolicy`, `sandboxMode` |
| O | `thread.list` | optional `cursor`, unsigned-32 `limit`, `archived`, string `searchTerm` |
| O | `thread.read` | required `threadId`; optional `includeTurns` |
| C | `turn.start` | required `threadId` and non-empty `input`; optional `cwd`, `model`, `reasoningEffort`, `approvalPolicy`, `sandboxPolicy` |
| C | `turn.interrupt` | required `threadId`, `turnId` |
| C | `request.approval.respond` | required `pendingRequestId`, `decision` |
| C | `request.userInput.respond` | required `pendingRequestId`, `answers` |
| C | `request.authentication.respond` | required `pendingRequestId`, `accessToken`, `chatgptAccountId`; optional `chatgptPlanType` |
| C | `request.unknown.respond` | required `pendingRequestId`, arbitrary JSON `result` |
| C | `request.unknown.reject` | required `pendingRequestId`, signed-64 `code`, `message`; optional arbitrary JSON `data` |

### Authoritative `thread.read` results

The additive static capability `thread_read_state_effects` makes one narrow
command result an explicit source of synchronized state. A capable client does
not put this mechanism in its Hello capability request, because that array
selects representation behavior. It positively marks
`capabilityVocabularyVersion: 1`; an older server ignores that additive field,
while a new server omits the new capability name from Welcome for an unmarked
older client. After observing the mechanism in Welcome, the client adds the
command-envelope extension
`threadReadStateEffectVersion: 1` only to `thread.read` with
`includeTurns: true`. Old servers therefore see the old command, and old
clients continue to receive the legacy result path.

A full read is requester-local in both modes. It never publishes the returned
turns or items into BackendCore's shared State, never emits a global
`thread.upserted`, and never advances another frontend's journal. Without the
per-command extension, its bounded body is raw operation result data only and
does not mutate synchronized client State.

An opted-in success may include `stateEffect` with `scope: "thread"`, one of
the authorities `merge`, `replace`, or `absent`, and bounded truncation
metadata. Before sending that response, the server materializes every pending
visible event in the same dispatch transaction. The ordered transport is the
barrier: the client applies the effect transactionally to the immutable State
that is current when it processes the response, publishes the replacement
State, and only then invokes the operation completion callback. Applying the
result does not advance the journal cursor, including when the first useful
full read arrives while that cursor is still zero. The response is valid only
at its exact observer fence. If an ordinary observer suffix overtakes the
captured body, or capacity recovery must publish a newer Snapshot while
materializing the pending prefix, the newer synchronized State remains
authoritative and the server fails only the negotiated read instead of
returning an older effect. A legacy raw result remains usable because it has no
state effect. Ordinary observer overtake reports `conflict`; a Snapshot forced
by bounded projection reports `capacity_exceeded`.

- `replace` requires a structurally complete body with `fullyLoaded: true` and
  atomically replaces the thread and all of its turns and items. Descendants
  absent from the body are deleted.
- `merge` requires an incomplete body with `fullyLoaded: false` and only
  upserts the supplied structure. It never deletes an absent descendant, but
  it does downgrade an older complete local cache to `fullyLoaded: false` so
  omitted middle descendants cannot be mistaken for complete history.
- `absent` carries no thread body and removes the thread. The server emits it
  only when the provider reports the exact recognized authoritative
  missing-thread signature for the requested thread ID. Generic or ambiguous
  provider errors and retention-driven omission must never be interpreted as
  absence.

`truncation.sourcePartial` records that the captured source was already partial,
whether because the provider omitted descendants or requester-local retention
limits reduced the capture before wire-size bounding.
`truncation.responseTruncated` is true exactly when bounding the response
omitted turns or items, whose counts are carried separately. Either condition
requires `merge`. Content-only truncation does not make the topology partial
and therefore does not prevent `replace`; the affected item metadata remains
responsible for exposing its own dropped-byte count and truncation. A result
without `stateEffect` remains observable but cannot mutate the synchronized
client State. For an opted-in complete read, the client validates the wire
result without copying its full body, consumes that body exactly once into
canonical State, and completes the public `ThreadReadResult` with `threadId`
and `stateEffect`; callers read the published body from `Client::state()`.
Legacy and non-authoritative reads retain their nested result body unchanged.

An approval `decision` is exactly one of `accept`, `acceptForSession`,
`decline`, or `cancel`. A user-input `answers` array contains objects with a
non-empty `questionId` and an `answers` string array, which may be empty.
`pendingRequestId` must be a non-zero unsigned decimal integer encoded as a
string.

Turn input uses explicit discriminators:

- `{"type":"text","text":"..."}`;
- `{"type":"image","url":"...","detail":"..."}`;
- `{"type":"localImage","path":"...","detail":"..."}`;
- `{"type":"skill","name":"...","path":"..."}`;
- `{"type":"mention","name":"...","path":"..."}`.

The optional turn `sandboxPolicy.type` is `dangerFullAccess`, `readOnly`,
`externalSandbox`, or `workspaceWrite`. `dangerFullAccess` accepts only its
type. `networkAccess` is Boolean for `readOnly` and `workspaceWrite`, and a
non-empty string for `externalSandbox`. Only `workspaceWrite` accepts
`writableRoots`, `excludeTmpdirEnvVar`, and `excludeSlashTmp`.

Controller acquisition is explicit:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"command","requestId":"role-1","method":"controller.acquire","params":{}}
{"protocol":"snodec.codex-frontend","version":1,"kind":"response","requestId":"role-1","ok":true,"result":{"controllerSessionId":"7","role":"controller"}}
{"protocol":"snodec.codex-frontend","version":1,"kind":"events","fromSequence":144,"toSequence":144,"events":[{"sequence":144,"type":"controller.changed","data":{"controllerSessionId":"7"}}]}
```

Acquisition succeeds when there is no controller and is idempotent for the
current controller. A different session receives `conflict`. Release succeeds
only for the controller. When a controller disconnects, its role is released
and remaining clients receive controller/session updates; the App Server keeps
running and pending requests remain unanswered for a later controller. There
is no forced takeover and no automatic first-controller policy.

A complete turn command and response can look like:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"command","requestId":"turn-9","method":"turn.start","params":{"threadId":"thread-1","input":[{"type":"text","text":"Summarize the changes."}],"cwd":"/work/project","approvalPolicy":"on-request","sandboxPolicy":{"type":"workspaceWrite","networkAccess":false,"writableRoots":["/work/project"],"excludeSlashTmp":true}}}
{"protocol":"snodec.codex-frontend","version":1,"kind":"response","requestId":"turn-9","ok":true,"result":{"turn":{"id":"turn-1","threadId":"thread-1","status":"inProgress","active":true,"terminal":false,"items":[],"extensions":{}}}}
```

## Responses and stable errors

A successful response contains `result` and no `error`:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"response","requestId":"client-42","ok":true,"result":{}}
```

A failed response contains `error` and no `result`:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"response","requestId":"client-42","ok":false,"error":{"code":"permission_denied","message":"The controller role is required."}}
```

`ok:false` is the terminal result of that command, not a request to close the
Frontend Protocol connection. The peer removes that request's correlation
exactly once and may submit later commands on the same connection. Connection
closure occurs only through a separate transport failure or a closing
`protocol.error`/protocol-state failure. In particular, controller denial,
ordinary invalid parameters, not-found/conflict, cancellation, provider
unavailability, and rate limiting do not by themselves terminate a valid
session.

The stable v1 error codes are:

```text
permission_denied          invalid_command
not_found                  conflict
local_submission_failure  typed_decoding_failure
remote_app_server_error    cancelled
backend_unavailable        duplicate_request_id
malformed_json             wrong_protocol
unsupported_version        missing_field
invalid_field              unknown_kind
unknown_method             frame_too_large
capacity_exceeded          sequence_overflow
replay_gap                 internal_error
authentication_required    authentication_failed
origin_rejected            transport_security_required
rate_limited
```

`details` may carry bounded, structured context. A remote App Server error may
include its signed numeric `remoteCode`, but consumers must branch on the
stable string `code`, not on `errno` or an App Server implementation detail.

`protocol.error` is for message-level failures and includes `code`, `message`,
`supportedVersions`, and `closeConnection`; it may also include `requestId`
and `details`:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"protocol.error","code":"unsupported_version","message":"unsupported frontend protocol version 2","supportedVersions":[1],"closeConnection":true}
```

## Snapshot state

The `snapshot` envelope is:

```json
{
  "protocol": "snodec.codex-frontend",
  "version": 1,
  "kind": "snapshot",
  "sequence": 140,
  "state": {}
}
```

Its deterministic `state` contains:

- `backendRevision`, `lifecycle`, optional `lastLifecycleError`, and
  `sequenceExhausted` for BackendCore;
- a bounded `diagnostics` summary;
- ordered `threads`, each with ordered turns and items;
- each item's lifecycle plus complete currently retained `agentText`,
  `reasoningText`, `reasoningSummary`, and `commandOutput`;
- `contentTruncated` and `droppedContentBytes` when the backend's configurable
  accumulated-visible-content bound discarded an old prefix;
- pending request summaries, controller ownership, and connected sessions;
- thread-list completeness, loaded-page count, and available cursors; and
- frontend journal `oldestReplayableAfter`, `currentSequence`, and optional
  oldest/newest retained sequences.

Lifecycle strings are `stopped`, `starting`, `initializing`, `ready`,
`stopping`, and `failed`. Session roles are `observer` and `controller`. Item
status is `unknown`, `started`, `completed`, or `failed`; turn status remains a
stable Codex status string with independent `active` and `terminal` Booleans.
Known item `type` strings are `user_message`, `agent_message`, `reasoning`,
`command_execution`, `file_change`, `tool_call`, and `web_search`; a future
unknown item retains its provided Codex type or uses `unknown`. An expanded
user-message item's normalized `data` contains a nullable string `clientId`,
the complete retained textual presentation in `text`, and `textTruncated`.
Text input parts remain in source order and are separated by `\n\n`, including
empty parts. The projection accepts Codex's aggregate 1,048,576-Unicode-scalar
text limit without applying the generic frontend-detail bounds. Non-text input
and additional content-entry metadata are not exposed. The frozen legacy
representation carries the same retained text as normalized `content` text
entries for compatibility.

User-message `data` also contains `contentTruncated`,
`originalContentBytes`, `retainedContentBytes`, `originalContentItems`, and
`retainedContentItems`. The byte counts are the compact serialized sizes of
the original content array and the retained normalized text-entry array,
including their array delimiters; an empty retained array is therefore two
bytes. The item counts report the corresponding array lengths.
`contentTruncated` is true when non-text content or entry metadata was omitted;
`textTruncated` independently reports incomplete textual retention. These
payload-specific fields do not change the top-level item `contentTruncated`
and `droppedContentBytes`, which continue to describe old prefixes discarded
from accumulated visible text and command-output channels.

Snapshots do not contain callbacks, pointers, internal request-occurrence
tokens, App Server client request IDs, authentication access tokens, or secret
user-input answers. Known item and request data is normalized. Bounded future
information may appear under deliberately named `extensions` or `details`
objects rather than as a raw ordinary App Server envelope.

### Capability-gated expanded state

A1.7a defines the scope-projectable expanded snapshot model and A1.7b activates
it for connections that negotiate the relevant capabilities. Its mandatory
core is `provider`, `controller`, `sessions`, `threadList`, `capacity`, and
`truncation`. `threadList` carries `hasLoadedPage`, `complete`, `pagesLoaded`,
optional forward/backward cursors, and source generation/freshness when
available. Optional authorized domains are
`threads`, `turns`, `items`, `pendingRequests`, `accounts`, `models`,
`configuration`, `processes`, `filesystemWatches`, `fuzzySearches`,
`permissionProfiles`, `reviews`, `apps`, `externalAgents`, `hooks`,
`marketplace`, `plugins`, `skills`, `mcp`, `windowsSandbox`, `remoteControl`,
`notices`, and `activities`. Omission is meaningful: a domain may be absent
because the capability is not implemented, the deployment disabled it, the
connection lacks scope, or the snapshot bound omitted optional data. Mandatory
truncation metadata remains visible.

The expanded safe `ThreadItem` discriminator covers all 18 stable alternatives:

```text
agentMessage          collabAgentToolCall  commandExecution
contextCompaction     dynamicToolCall      enteredReviewMode
exitedReviewMode      fileChange           hookPrompt
imageGeneration       imageView            mcpToolCall
plan                  reasoning            sleep
subAgentActivity      userMessage          webSearch
```

Each expanded item retains reviewed IDs/location, bounded status and summary,
generation/freshness, connection invalidation, and explicit
truncation/omission metadata. It never exposes raw provider JSON, binary image
or audio payloads, unbounded prompts, or known structured secret fields.

Expanded pending requests use exactly ten safe kinds:

```text
command_execution_approval  file_change_approval  user_input
authentication              apply_patch_approval  exec_command_approval
permissions_approval        attestation           dynamic_tool_call
mcp_elicitation
```

They retain only the backend-generated pending ID, safe associations, bounded
summary/details, and truncation state. Provider occurrence tokens, JSON-RPC
request IDs, authentication tokens, secret answers, and unbounded raw payloads
remain excluded.

## Normalized events

An `events` message contains a non-empty occurrence-ordered visible sequence.
Sequence numbers increase strictly between visible canonical occurrences but
need not be contiguous because mandatory projection may omit an occurrence.
Recognized expanded event families projected from one occurrence may repeat
that occurrence's global sequence as one atomic group; legacy events remain
strictly increasing. The outer range exactly matches the first and last
visible event, while each inner `event.sequence` remains the authoritative
global occurrence identifier.

```json
{
  "protocol": "snodec.codex-frontend",
  "version": 1,
  "kind": "events",
  "fromSequence": 145,
  "toSequence": 146,
  "events": [
    {"sequence":145,"type":"item.content.updated","data":{"threadId":"thread-1","turnId":"turn-1","itemId":"item-1","channel":"commandOutput","content":"done\n","contentTruncated":false,"droppedContentBytes":0}},
    {"sequence":146,"type":"turn.updated","data":{"turn":{"id":"turn-1","threadId":"thread-1","status":"completed","active":false,"terminal":true,"items":[],"extensions":{}}}}
  ]
}
```

`fromSequence` and `toSequence` equal the first and last inner event sequence.
The stable normalized event names and their principal data are:

| Event type | Data |
| --- | --- |
| `backend.lifecycle.changed` | `lifecycle`, optional `error` |
| `diagnostics.updated` | total `received`, bounded `recent` strings |
| `thread.updated` | complete current `thread` |
| `thread.list.updated` | page/completeness flags and optional cursors |
| `turn.updated` | complete current `turn` |
| `item.updated` | `threadId`, `turnId`, complete current `item` |
| `item.content.updated` | IDs, `channel`, latest accumulated `content`, truncation fields |
| `request.pending` | sanitized current `request` |
| `request.resolved` | `pendingRequestId`, `reason` |
| `controller.changed` | optional `controllerSessionId` |
| `session.changed` | `sessionId`, `connected`, `role` |
| `codex.extension` | bounded `method`, sanitized bounded `params`, optional bounded `decodingError`, optional `truncation` |

The additive contract also defines 26 capability-gated expanded event
families:

```text
provider.updated         controller.updated       sessions.updated
threadList.updated       thread.upserted           thread.removed
turn.upserted            item.upserted             item.content.updated
pendingRequests.updated  account.updated           models.updated
configuration.updated    process.updated           filesystemWatch.updated
fuzzySearch.updated      reviews.updated           integrations.updated
plugins.updated          skills.updated            mcp.updated
platform.updated         notice.added              activity.updated
capacity.updated         diagnostics.updated
```

`threadList.updated` contains one required `threadList` wrapper with the same
stable shape used by expanded snapshots. A legacy `thread.list.updated`
occurrence projects to exactly that one compact expanded event when dedicated
notification events are selected; it does not repeat every retained thread or
fabricate a thread for an empty page. Threads returned by the page are carried
by their own ordinary `thread.upserted` occurrences. Legacy connections retain
the original `thread.list.updated` representation, and one connection never
receives both forms for one occurrence.

### Expanded-event identity

An identity-bearing expanded event takes its identity from the exact canonical
occurrence. If the projection resolves richer data through the captured
snapshot, the selected entity must carry that same identity. It must never
substitute another retained entity merely because that entity is first, last,
newest, or nonempty, and it must not recursively guess an arbitrary nested
field named `id`.

The stable normalized identity paths are:

| Expanded family | Canonical identity/parent path |
| --- | --- |
| `thread.upserted` | `data.thread.id` |
| `thread.removed` | `data.threadId` |
| `turn.upserted` | `data.turn.id`, parent `data.turn.threadId` |
| `item.upserted` | `data.item.id`, parents `data.threadId` and `data.turnId` |
| `item.content.updated` | `data.itemId`, parents `data.threadId` and `data.turnId` |
| `process.updated` | `data.process.processHandle` |
| `filesystemWatch.updated` | `data.filesystemWatch.watchId` |
| `fuzzySearch.updated` | `data.fuzzySearch.sessionId` |
| `activity.updated` | `data.activity.key` |
| `notice.added` | the exact `data.notice` occurrence |

Reviewed notification-extension mappings use the corresponding explicit
`params` path. They do not turn recursive identity discovery into a wire
contract. When a proven identity has no resolvable target, a family may emit a
contract-approved same-ID minimum only where its schema permits that state;
otherwise projection selects bounded Snapshot fallback. An unproven identity
cannot become a fabricated `"unavailable"` upsert or an unrelated entity.

The remaining families are aggregate/singleton projections and may carry their
complete reviewed projected domain. Live delivery and replay project the same
canonical record through the same scope filter, so exact identity and data are
equivalent. A page of 25 distinct threads therefore yields at most 25 exact,
unique `thread.upserted` occurrences plus one `threadList.updated`; it cannot
yield 25 copies of one retained tail thread.

Notification mapping follows the stable transition semantics. In particular,
`thread/deleted` maps to `thread.removed`. Only
`item/agentMessage/delta`, `item/commandExecution/outputDelta`,
`item/fileChange/outputDelta`, `item/reasoning/summaryTextDelta`, and
`item/reasoning/textDelta` map to accumulated `item.content.updated`
replacement. Item lifecycle, terminal-interaction, patch, progress, plan, and
summary-part notifications map to `item.upserted`.

`commandOutput` is projected according to the enclosing stable item type:
`commandExecution` requires `command_execution`, and `fileChange` requires
`filesystem_write`. This semantic item walk is not limited by the generic
projection-rule budget. Missing, unknown, or conflicting discriminators require
both scopes or cause `commandOutput` to be omitted and reported.

Compatibility is explicit and mechanically complete. All 68 stable server
notifications retain one legacy path: 14 already use normalized state/events
and 54 use bounded, recursively redacted `codex.extension`. All 18 stable
`ThreadItem` alternatives retain one legacy path: eight have normalized item
contracts and ten retain bounded metadata-only compatibility. The expanded
mapping covers all 68 notifications and all 18 items. For one provider
occurrence, a connection receives either its legacy projection or its
capability-gated expanded projection, never both. A1.7a defines and tests that
mapping; A1.7b activates it through the same mandatory scope-projection path
used by snapshot, live events, and replay.

The content channel is one of `agentText`, `reasoningText`,
`reasoningSummary`, or `commandOutput`. Consumers replace their visible value
for that channel with `content`; they do not append an assumed raw delta.
Several backend transitions can become one event, and an internal transition
that does not change visible frontend state can produce none.
Current request-resolution reasons are `response_enqueued` and
`app_server_connection_invalidated`.

`codex.extension` is the explicit forward-compatibility escape hatch. A client
may safely ignore it. It is not permission to treat arbitrary raw App Server
envelopes as stable v1 events, and it never makes an unknown command method a
raw operation.

Both live `codex.extension` events and snapshot `codexExtensions` records use
one deterministic sanitizer. A snapshot carries at most the newest 64 records
and reports any additional records omitted from the current canonical retained
set in `omittedCodexExtensions`. A method retains at most 1 KiB, serialized
`params` at most 32 KiB, and a decoding error at most 2 KiB. Oversized content
uses explicit byte-counted `truncation` metadata; a normalized extension event
is capped at 64 KiB and therefore fits the default 256 KiB batch. Credential,
authorization, password, token, answer, and secret-value fields are recursively
redacted before serialization. `sensitiveFieldsRedacted: true` makes that loss
explicit. The same parameter sanitizer applies to unknown pending requests, so
snapshot fallback cannot reintroduce access tokens, secret answers, or internal
request occurrence tokens.

## Delta accumulation, dirty entities, and flushes

Every App Server delta updates canonical BackendCore state immediately. The
service marks the tuple `(thread, turn, item, channel)` dirty and stores the
latest accumulated visible content. Repeated marks for the same key replace
the pending normalized update while preserving the key's first insertion
order. Item, turn, thread, pending-request, controller, session, lifecycle,
diagnostic, and extension updates have their own keys; content from different
items, turns, or channels is never merged.

The first deferred mark schedules one `core::EventReceiver::atNextTick()`
flush. A guard prevents the next 999 raw deltas from scheduling 999 callbacks.
The flush drains all dirty keys in deterministic insertion order, allocates one
frontend sequence per normalized update, journals it, builds bounded batches,
and broadcasts those batches. Thus a 1,000-token burst normally yields a small
number of frontend messages while the final content remains exact.

Interactive and terminal transitions flush immediately: lifecycle/controller
changes, session changes, pending-request arrival/removal, terminal item
updates, turn completion, non-retrying turn failure, and capacity/snapshot
fallback. An immediate flush drains every already-dirty key, so accumulated
item content is visible before the terminal turn update. Flush reentrancy is
guarded; work marked during a flush is drained again without recursively
entering the service.

The default dirty-set maximum is 4,096 entity/channel keys. Exhausting it,
failing normalization, or producing a single event larger than a legal batch
invalidates replay and broadcasts a fresh snapshot instead of growing memory
or emitting an oversized event.

## Batching, replay, and bounded memory

Default protocol-layer bounds are:

| Resource | Default bound |
| --- | ---: |
| dirty entity/channel keys | 4,096 |
| events per batch | 64 |
| serialized bytes per batch | 256 KiB |
| replay-journal entries | 4,096 |
| serialized replay-journal bytes | 8 MiB |
| queued messages per FrontendService connection | 512 |
| queued serialized bytes per FrontendService connection | 138.125 MiB |
| messages delivered per event-loop callback | 64 |

Batch size is measured from the compact serialized envelope, not estimated
from payload count. A burst is split before either batch bound is exceeded.
One normalized event that cannot fit triggers snapshot fallback. The fallback
advances a sequence barrier and invalidates replay from every earlier sequence,
so a stale client cannot mistake the changed snapshot state for an empty replay.
An explicit snapshot of unchanged state does not advance that barrier. Latency is
bounded by the next event-loop tick unless an immediate transition flushes it
earlier.

For an already Ready connection this fallback is delivered as the bare live
snapshot barrier described above. The atomic occurrence is not split, limits
are not raised, and the client remains Ready after the replacement.

The journal stores only bounded post-coalescing canonical records. Before
canonical retention, AISuite removes known structured authentication material,
known credential/token/password/private-key/API-key/cookie fields, reviewed
secret-response fields, and unsafe raw provider envelopes. It never stores one
record per raw token merely because the App Server emitted one, and a
connection-specific serialized projection is not its authority. Arbitrary
user, model, reasoning, notice, diagnostic, process-output, and command-output
text remains potentially sensitive; it may remain in bounded canonical state
when required for an authorized projection and is protected by mandatory
per-principal scope projection. AISuite does not claim that arbitrary text is
semantically free of credentials and does not apply heuristic token scanning.

Scope filtering is unconditional. Live and replay project the same canonical
record for the current principal, and snapshots use the same domain policy.
The projection order is known structured-secret and unsafe-envelope removal,
scope filtering, legacy/expanded selection, optional omission/redaction
metadata, then encoding. Thus
`scope_projected_state` changes metadata only and omitting capabilities cannot
increase the information ceiling. Different principals may receive richer or
redacted representations of the same occurrence and sequence without a
second journal record.

Sequence numbers start at the configured initial value (zero by default), are
strictly monotonic across eviction, and never get reused. Replay considers
canonical occurrences strictly after the requested global sequence, then
projects them for the current principal; the visible result may therefore be
sparse or empty. `EventBatch.fromSequence` and `toSequence` bound the first and
last visible events in that batch. The oldest entries are evicted until both
entry and exact serialized-byte bounds hold. A request below
`oldestReplayableAfter` has a server-reported journal gap and receives a
snapshot; a visible sequence jump alone is not a replay gap. Requesting the
current sequence is a valid empty replay, as is a projected empty suffix that
ends with `sync.complete` at the current global sequence. Unsigned-64
exhaustion reports `sequence_overflow`, invalidates replay, and never wraps.

Journal byte accounting covers the compact event objects. A replay also needs
the `events` envelopes, commas between events, `welcome`, and `sync.complete`.
The default service therefore reserves explicit downstream headroom instead of
using the same 8 MiB limit twice: 512 bytes per possible retained entry (a
conservative upper bound for the v1 batch-envelope contribution), 64 KiB for
control envelopes, and room for one maximum server message (a 128 MiB decoded
State plus its 64 KiB snapshot envelope, or a provider-derived response), for a
138.125 MiB service limit.
This makes every replay that fits the default 4,096-entry/8 MiB journal fit the
default service queue as well. The queue remains bounded and slow-client
isolation is unchanged.

These relationships apply to the defaults. Applications that independently
raise journal entries, journal bytes, batch bounds, service bounds, or a
transport writer bound must preserve corresponding downstream headroom. If a
custom limit cannot contain a replay or a complete snapshot, that frontend is
closed locally by backpressure; the implementation does not make snapshots or
queues unbounded to mask an inconsistent deployment configuration.

The service gives every frontend its own bounded asynchronous queue. A queue
overflow, a throwing callback, or a transport that declines an outbound
message closes only that frontend and releases all of its queued memory. A
slow observer therefore cannot grow the controller queue or delay another
observer. If the slow connection was the controller, normal disconnect policy
releases its role but retains pending requests.

## Pending requests and secrets

`request.pending` and snapshots expose backend-generated request IDs, not
App Server occurrence tokens. Supported summaries include:

- `command_approval` and `file_change_approval`, with relevant IDs and safe
  command/path/reason fields;
- `user_input`, with question IDs, prompts, option descriptions, and secret
  flags, but never submitted answers;
- `authentication`, with reason and optional previous account ID, but no
  access token; and
- `unknown`, with bounded original method/params and optional decoding error.

The exact typed request remains inside BackendCore so existing occurrence-token
protection still handles App Server request-ID reuse. A request is removed only
after its response was successfully enqueued, or when App Server connection
ownership is invalidated. Local enqueue failure retains it. Controller
disconnect neither approves nor rejects it. A later controller can answer it.

A command-approval flow uses the frontend-generated pending ID throughout.
The correlated response and broadcast update are independently ordered streams,
so clients must not assume which of the last two messages is delivered first:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"events","fromSequence":201,"toSequence":201,"events":[{"sequence":201,"type":"request.pending","data":{"request":{"id":"31","type":"command_approval","threadId":"thread-1","turnId":"turn-1","itemId":"command-1","details":{"command":"make check","cwd":"/work/project","reason":"Needs approval"}}}}]}
{"protocol":"snodec.codex-frontend","version":1,"kind":"command","requestId":"approval-31","method":"request.approval.respond","params":{"pendingRequestId":"31","decision":"decline"}}
{"protocol":"snodec.codex-frontend","version":1,"kind":"response","requestId":"approval-31","ok":true,"result":{}}
{"protocol":"snodec.codex-frontend","version":1,"kind":"events","fromSequence":202,"toSequence":202,"events":[{"sequence":202,"type":"request.resolved","data":{"pendingRequestId":"31","reason":"response_enqueued"}}]}
```

`request.authentication.respond` necessarily carries an access token from the
local frontend to the backend. That value is command-only: it is never echoed
in a response, snapshot, normalized event, diagnostic, or replay entry. Secret
user-input answers have the same rule. Transport implementations should still
treat all command traffic as sensitive.

## Transport independence and A1.7 boundaries

Protocol v1 defines JSON values, not record framing or a socket. The reference
application uses compact newline-delimited JSON, documented in
[Codex backend reference application](codex-backend-reference-app.md). An
in-process consumer or WebSocket transport uses the same `Codec` and
`FrontendService` without inheriting JSONL. Stream transports use one compact
JSON object plus newline; WebSocket uses one complete JSON object per text
message and rejects binary protocol messages. The reference WebSocket maximum
is the same exact 8 MiB (8,388,608-byte) inbound bound as stream framing. This
admits a maximum Codex text input even when every Unicode scalar requires a
six-byte JSON escape.

The reference HTTP/WebSocket transport configures SNode.C 2.0's HTTP parser
and server with 8 KiB start/header lines, 64 KiB aggregate headers, 128 fields,
a one-byte decoded-body ceiling, one pending request, and disabled chunked
transfer and pipelining. Zero is not used because SNode.C defines it as
unlimited. Express middleware rejects every non-empty static or WebSocket
upgrade body; requests larger than the parser boundary receive 413 before
route dispatch. It then applies AISuite's endpoint, method, Origin,
credential-channel, and request semantics. A BackendCore session is still
created only after authenticated Hello.

WebSocket bearer material remains legal only in that first protocol Hello.
The reference upgrade rejects query, cookie, Authorization, Proxy-Authorization,
and credential-shaped subprotocol channels. The required WebSocket subprotocol
token is `codex`, distinct from the Frontend Protocol identity
`snodec.codex-frontend`. This is reference transport policy, not an additional
Frontend Protocol v1 message or field.

The original frontend slice did not implement provider recovery. A1.6a added
event-loop-native provider recovery to the reference backend without changing
Protocol v1, A1.6b completed provider-to-backend semantics, and A1.7a froze the
additive contract and owner-approved security decisions. A1.7b completes the
authenticated, scope-projecting `FrontendService`, all approved handlers,
provider lifecycle exposure, and Unix/TCP/TLS/WebSocket/WSS/RFCOMM
composition. The default application remains Unix-only; optional transport
support does not alter the protocol identity or method catalog.

A1.7c-1 supplies the C++ Frontend SDK and migrated
`codex-backend-client`. The owner-approved P0–P3 architecture reduction follows
A1.7c-1: P1 provides generic SNode.C prerequisites, P2 builds the complete
greenfield frontend beside the old executable oracle, and P3 cuts over and
deletes the legacy implementation. `codex-ui` remains untouched throughout it.
A1.7c-2 begins after P3 and migrates the UI against the reduced canonical
frontend architecture. A1.7d owns the TypeScript Frontend SDK and browser
frontend.
Persistence, multiple controllers, forced takeover, and provider-neutral
architecture are not implied by any capability name; provider neutrality
remains A2. Every later phase must preserve v1 identity, bounded coalescing,
replay, redaction, and per-connection slow-client isolation.
