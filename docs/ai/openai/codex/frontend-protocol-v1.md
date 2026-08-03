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
frontend adapter marks a visible entity/channel dirty
        ↓
one guarded next-tick flush
        ↓
latest visible entity state is normalized and coalesced
        ↓
bounded event batches and bounded replay journal
        ↓
independent bounded queue for each frontend
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

Definition and runtime availability are separate. The production runtime still
accepts exactly the original 15 methods. The 90 additive methods are described
by generated metadata and JSON Schema but remain unavailable until A1.7b adds
the authenticated, scope-enforcing service boundary. An unavailable additive
method is not a raw JSON escape hatch.

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
Project version `0.1.0`, all three Codex libraries' SOVERSION 2, protocol
identity, and protocol version remain unchanged.

Complete upstream inventory registration, typed implementation, BackendCore
support, canonical-state support, frontend definition, runtime availability,
deployment enablement, and per-connection permission remain separate facts.
Registry presence alone never makes an App Server operation remotely callable.
See the generated [coverage report](app-server-api-coverage.md), frozen
[security decisions](app-server-security-decisions.md), and focused
[A1.7a report](a1-7a-frontend-contract.md).

## C++ tagged-JSON model

A1.7a's complete C++ protocol model is a method-tagged, schema-validated JSON
contract layer. `generated::MethodParameters<Id>` and
`generated::MethodResult<Id>` distinguish all 105 frontend methods at compile
time, while their payload is retained in `nlohmann::json`. The tags provide
exact method correlation, generated metadata, schema validation, and wire
conformance. They are not yet the ergonomic, domain-typed C++ application API.

A1.7c will introduce `AISuite::OpenAICodexFrontendClient` with domain-oriented
façades, callback-last asynchronous operations, typed client-side state,
replay/reconnection, and no raw-JSON requirement for stable application
workflows. A1.7a therefore supplies method-tagged schema-validated protocol
types; A1.7c supplies the domain-typed Frontend SDK.

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

The fixed regression corpus contains 558 generated validations. Its observed
maxima are 3,323 visits, depth 16, 1,806 resolved references, 38 evaluated
alternatives, 28 discriminator fast paths, zero unique-item comparisons, and 11
regular-expression evaluations. A valid 2,000-item expanded snapshot consumes
225,307 visits at depth 12. The generated `$defs` graph is currently acyclic;
the private test seam therefore exercises the exact 128/129 depth boundary with
a synthetic schema, while generated snapshots are tested at their exact
measured depth and again through the public codec. The generated sensitive-field
guard is separately driven past depth 128 through a real method result; the
public codec rejects it with the bounded complexity error and remains reusable.

Unknown non-conflicting fields are deliberately accepted for additive v1
compatibility. Known fields retain full validation, while unknown values retain
safe-property-name, sensitive-field, nesting, size, and nested-value checks.
AISuite intentionally does not use `additionalProperties: false` to reject such
safe extensions at runtime; this is a compatibility policy, not generic Draft
2020-12 behavior. A1.7b still owns network admission, frame bounds, rate
limiting, authentication, and connection-scope enforcement.

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
`FrontendSession` before a valid hello. A hello without `resumeAfter` requests
a snapshot:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"hello"}
```

The normal response is a welcome, exactly one snapshot, then a sync marker:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"welcome","sessionId":"7","role":"observer","currentSequence":140,"syncMode":"snapshot"}
{"protocol":"snodec.codex-frontend","version":1,"kind":"snapshot","sequence":140,"state":{"backendRevision":318,"lifecycle":"ready","diagnostics":{"received":0,"recent":[]},"threads":[],"pendingRequests":[],"sessions":[{"sessionId":"7","role":"observer"}],"codexExtensions":[],"omittedCodexExtensions":0,"threadList":{"hasLoadedPage":true,"complete":false,"pagesLoaded":1,"nextCursor":"page-2"},"journal":{"oldestReplayableAfter":118,"currentSequence":140,"oldestRetainedSequence":119,"newestRetainedSequence":140},"sequenceExhausted":false}}
{"protocol":"snodec.codex-frontend","version":1,"kind":"sync.complete","sequence":140}
```

To reconnect, include the last fully applied frontend sequence:

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
it has applied all preceding messages.

A client can synchronize again with `snapshot.get` or `events.replay`. The
server sends the command response first, then snapshot or event batches, then
`sync.complete`. Explicit replay from a future sequence fails with
`invalid_command`.

Before hello, any other message receives one bounded `protocol.error` and the
connection closes. Malformed JSON, a wrong identity, or an unsupported version
also closes only that frontend connection. An unsupported-version response
includes `supportedVersions: [1]`. Protocol errors never stop `BackendCore`,
the App Server, or another frontend.

### Capability and method discovery

A hello may add a `capabilities` array to request additive behavior. A welcome
may add `capabilities`, `availableMethods`, `permittedMethods`, and
`serverVersion`. All four welcome fields and the hello field are optional, so
an original v1 peer retains its existing bytes and behavior. Capability
advertisement contains three independent arrays:

- `defined`: the server understands the contract name;
- `implemented`: the running server has executable support; and
- `permitted`: the authenticated connection, deployment configuration, and
  current authorization permit its use.

The 18 defined capability names are:

```text
method_discovery                 security_scopes
complete_provider_operations     complete_reverse_requests
complete_backend_domains         conditional_filesystem
conditional_command_execution    dedicated_pending_requests
dedicated_notification_events    complete_thread_items
authenticated_frontend           scope_projected_state
provider_lifecycle               multi_transport
cpp_client_sdk                   typescript_client_sdk
browser_ui                       qt_ui
```

A1.7a's generated metadata marks only `method_discovery` and `security_scopes`
as implemented by the current runtime. Future capability names are defined so
clients can negotiate without a version fork, but their presence in `defined`
must never be interpreted as implementation or permission. Likewise,
`availableMethods` is deployment/runtime availability while `permittedMethods`
is connection-specific authorization. The current runtime method set remains
the original 15 listed below.

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
read policy is not remote authorization. A1.7b must enforce explicit
deployment enablement before any such frontend method can run.

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

Every session starts as an observer. The following table is the exact 15-method
current runtime subset, not the complete 105-method additive catalog. Observer
commands are marked **O**; all other commands require the controller role
(**C**).

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
`externalSandbox`, or `workspaceWrite`. `networkAccess` is Boolean except for
`externalSandbox`, where it is a non-empty string. Only `workspaceWrite`
accepts `writableRoots`, `excludeTmpdirEnvVar`, and `excludeSlashTmp`.

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
unknown item retains its provided Codex type or uses `unknown`. A user-message
item's normalized `data` is an object with a nullable string `clientId` and a
`content` array. The typed layer retains the complete opaque App Server content
array, while a snapshot or `item.updated` event retains an ordered prefix of
complete entries whose complete normalized `data` serialization is at most
65,536 bytes. Retained entries remain exact JSON values, including unknown
future content-entry variants; no partial JSON serialization is exposed as an
entry.

User-message `data` also contains `contentTruncated`,
`originalContentBytes`, `retainedContentBytes`, `originalContentItems`, and
`retainedContentItems`. The byte counts are the compact serialized sizes of
the original and retained content arrays, including their array delimiters;
an empty retained array is therefore two bytes. The item counts report the
corresponding array lengths. If even the first entry cannot fit with the
client ID and metadata, `content` is empty and `contentTruncated` is true.
These user-message fields describe normalized payload truncation. They do not
change the top-level item `contentTruncated` and `droppedContentBytes`, which
continue to describe old prefixes discarded from accumulated visible text and
command-output channels.

Snapshots do not contain callbacks, pointers, internal request-occurrence
tokens, App Server client request IDs, authentication access tokens, or secret
user-input answers. Known item and request data is normalized. Bounded future
information may appear under deliberately named `extensions` or `details`
objects rather than as a raw ordinary App Server envelope.

### Capability-gated expanded state

A1.7a defines a scope-projectable expanded snapshot model without activating it
for current connections. Its mandatory core is `provider`, `controller`,
`sessions`, `capacity`, and `truncation`. Optional authorized domains are
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

Each expanded item retains safe IDs/location, bounded status and summary,
generation/freshness, connection invalidation, and explicit
truncation/omission metadata. It never exposes raw provider JSON, binary image
or audio payloads, unbounded prompts, or secrets.

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

An `events` message contains a strictly increasing, non-empty sequence:

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

The additive contract also defines 25 capability-gated expanded event
families:

```text
provider.updated         controller.updated       sessions.updated
thread.upserted          thread.removed            turn.upserted
item.upserted            item.content.updated      pendingRequests.updated
account.updated          models.updated            configuration.updated
process.updated          filesystemWatch.updated   fuzzySearch.updated
reviews.updated          integrations.updated      plugins.updated
skills.updated           mcp.updated               platform.updated
notice.added             activity.updated          capacity.updated
diagnostics.updated
```

Compatibility is explicit and mechanically complete. All 68 stable server
notifications retain one legacy path: 14 already use normalized state/events
and 54 use bounded, recursively redacted `codex.extension`. All 18 stable
`ThreadItem` alternatives retain one legacy path: eight have normalized item
contracts and ten retain bounded metadata-only compatibility. The expanded
mapping covers all 68 notifications and all 18 items. For one provider
occurrence, a connection receives either its legacy projection or its
capability-gated expanded projection, never both. A1.7a defines and tests that
mapping but does not activate the expanded event families in the current
runtime.

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
adapter marks the tuple `(thread, turn, item, channel)` dirty and stores the
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
entering the adapter.

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
| queued messages per frontend adapter connection | 512 |
| queued serialized bytes per frontend adapter connection | 11 MiB |
| messages delivered per event-loop callback | 64 |

Batch size is measured from the compact serialized envelope, not estimated
from payload count. A burst is split before either batch bound is exceeded.
One normalized event that cannot fit triggers snapshot fallback. The fallback
advances a sequence barrier and invalidates replay from every earlier sequence,
so a stale client cannot mistake the changed snapshot state for an empty replay.
An explicit snapshot of unchanged state does not advance that barrier. Latency is
bounded by the next event-loop tick unless an immediate transition flushes it
earlier.

The journal stores only normalized, post-coalescing frontend events. It never
stores one record per raw token merely because the App Server emitted one.
Sequence numbers start at the configured initial value (zero by default), are
strictly monotonic across eviction, and never get reused. Replay returns
events strictly after the requested sequence. The oldest entries are evicted
until both entry and exact serialized-byte bounds hold. A request below
`oldestReplayableAfter` has a gap and receives a snapshot; requesting the
current sequence is a valid empty replay. Unsigned-64 exhaustion reports
`sequence_overflow`, invalidates replay, and never wraps.

Journal byte accounting covers the compact event objects. A replay also needs
the `events` envelopes, commas between events, `welcome`, and `sync.complete`.
The default adapter therefore reserves explicit downstream headroom instead of
using the same 8 MiB limit twice: 512 bytes per possible retained entry (a
conservative upper bound for the v1 batch-envelope contribution), 64 KiB for
control envelopes, and additional bounded margin, for an 11 MiB adapter limit.
This makes every replay that fits the default 4,096-entry/8 MiB journal fit the
default adapter queue as well. The queue remains bounded and slow-client
isolation is unchanged.

These relationships apply to the defaults. Applications that independently
raise journal entries, journal bytes, batch bounds, adapter bounds, or a
transport writer bound must preserve corresponding downstream headroom. If a
custom limit cannot contain a replay or a complete snapshot, that frontend is
closed locally by backpressure; the implementation does not make snapshots or
queues unbounded to mask an inconsistent deployment configuration.

The adapter gives every frontend its own bounded asynchronous queue. A queue
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
in-process consumer, future WebSocket adapter, or other transport can use the
same `Codec` and `BackendAdapter` without inheriting JSONL.

The original frontend slice did not implement provider recovery. A1.6a added
event-loop-native provider recovery to the reference backend without changing
Protocol v1, and A1.6b completed provider-to-backend semantics. A1.7a only
freezes the additive contract, generated metadata, and owner-approved security
decisions. It does not add a listener, TLS, remote authentication, scope
enforcement, or another transport.

A1.7b owns the authenticated, scope-projecting `FrontendService`, runtime
activation of approved additive methods and event/state projections, provider
lifecycle exposure, and multi-transport service composition. A1.7c owns the
C++ client SDK and Qt UI. A1.7d owns the TypeScript client SDK and browser UI.
Persistence, multiple controllers, forced takeover, and provider-neutral
architecture are not implied by any capability name; provider neutrality
remains A2. Every later phase must preserve v1 identity, bounded coalescing,
replay, redaction, and per-connection slow-client isolation.
