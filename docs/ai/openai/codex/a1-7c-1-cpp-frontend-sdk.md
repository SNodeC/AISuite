# A1.7c-1 C++ Frontend SDK

## Scope

A1.7c-1 adds the reusable transport-neutral C++ implementation of Frontend
Protocol v1 and migrates `codex-backend-client` to it. It does not modify
`codex-ui`, add Qt or TypeScript/browser code, introduce provider abstraction,
or change the protocol identity, version, message kinds, method inventory,
scope policy, journal, or information ceiling. The unreleased expanded
projection adds the reviewed `threadList`/`threadList.updated` correction.

The normative architecture is
[`a1-7c-1-cpp-frontend-sdk-architecture.md`](a1-7c-1-cpp-frontend-sdk-architecture.md).
The shared target is `AISuite::OpenAICodexFrontendClient`, the output is
`libaisuite-openai-codex-frontend-client.so`, and the namespace is
`ai::openai::codex::frontend::client`.

## Ownership and execution

`Client` is application-owned, non-copyable, non-movable, and PIMPL-backed.
One Client represents one logical backend deployment and survives sequential
physical attachments. `Connection` is a move-only control-block handle for one
attachment and detaches on destruction. `State` is an immutable cheap copy over
a shared implementation; older snapshots remain unchanged after later reducer
commits and remain valid after Client destruction. Client-owned façade objects
have stable addresses.

The SDK creates no thread, worker, polling loop, future, promise, or coroutine.
Calls and callbacks for one Client use one serialized execution context.
Transport and application callback exceptions are contained. Reentrant
submissions are deferred until the current dispatch frame has completed.

## Transport boundary

The transport accepts and emits complete compact JSON objects. It owns physical
framing, sockets, endpoints, reconnect timing, and its finite writer queue.
`Accepted` transfers an outbound object to that bounded transport; every other
send result fails only the current SDK connection. The SDK does not inspect or
recalculate a socket writer queue.

JSONL adapters append one newline and bound fragmented inbound records.
WebSocket/WSS uses SNode.C's client upgrade/framing implementation, requests
the exact `codex` subprotocol, sends one text object per message, and rejects
binary input. No SNode.C network, HTTP, WebSocket, TLS, Bluetooth, Qt, or curses
target appears in the SDK library's link interface.

## Lifecycle and authentication

The state machine is `Disconnected`, `Connecting`, `Authenticating`,
`Synchronizing`, `Ready`, `Closing`, and `Closed`. The SDK constructs and sends
Hello exactly once after `transportConnected()`; applications do not construct
Hello. Welcome begins synchronization and a valid `sync.complete` is required
before Ready.

CredentialProvider runs after physical connection and before Hello encoding.
Credentials are not placed in State, SessionInfo, diagnostics, request IDs,
capability metadata, or continuity metadata. `continuityKey` is bounded,
application-provided, non-secret, and never sent. Missing or changed continuity
requests a fresh snapshot. The reference CLI reuses the protected bearer-token
file validation used by the backend; verified local Unix use supplies
NoCredential and an effective-UID continuity identity.

## Capabilities and projection continuity

Hello requests exactly the five Frontend Protocol v1 representation selectors:

- `complete_backend_domains`;
- `dedicated_pending_requests`;
- `dedicated_notification_events`;
- `complete_thread_items`;
- `scope_projected_state`.

Required non-representation capabilities are validated after Welcome and are
not added to that request. SessionInfo separates requested/selected
representation, observed static mechanisms, observed conditional topology,
observed products, available/permitted methods, and permitted scopes.

The language-independent projection fingerprint canonicalizes requested and
selected representation capabilities, continuityKey, permitted scopes,
permitted methods, available methods, and explicit projection metadata. It
does not include observed mechanism, topology, or product truth. If continuity
cannot be proved, replay is consumed without mixing retained richer state and
the SDK requests a replacement snapshot before Ready.

Capability accounting has three independent categories: 13 static service
mechanisms; conditional topology `multi_transport`; and products.
`multi_transport` is true only with more than one declared transport family.
`cpp_client_sdk` is true only when the SDK product is enabled and built.
`typescript_client_sdk`, `browser_ui`, and `qt_ui` remain false. The primary
build total is `13 + topology(0|1) + 1`, not an unconditional mechanism count.

## Correlation, façades, and errors

Client-generated request IDs include connection generation and a monotonic
counter, remain unique while pending, and do not wrap. Every accepted operation
gets exactly one terminal callback. Disconnect and close fail pending work once;
reconnect does not restore or retry it. Unsolicited, duplicate, mismatched, or
malformed responses are protocol failures. Normal command failures do not close
the connection.

Native façades own seven methods: Controller two, Provider three, and
Synchronization two. Requests owns all 12 reverse response/rejection methods.
The remaining 18 domain façades own all 86 provider-backed operations. The
reviewed binding authority and generator enforce the exact 7/86/12/105 split.
The advanced `submit` accepts only the generated 105-alternative tagged
parameter variant; there is no string-plus-arbitrary-JSON method API and no
caller-supplied request ID.

Every ordinary façade signature names its exact public parameter and result
type. The binding authority records that type pair, encoder, decoder, category,
owner, and sensitivity class for every MethodId; compile-time guards reject
missing types or generic generated JSON wrappers. Those wrappers remain public
only for the restricted generated `submit` API.

The server-side `account.read` refresh-token check now consumes the normalized,
schema-validated parameter object used for dispatch. This removes the deferred
cross-translation-unit coupling without changing wire shape, scopes, results,
method counts, or controller policy.

## State, synchronization, and replay

State replacement and EventBatch application use private candidates. A whole
message is decoded, validated, reduced, and size-checked before one immutable
state is committed and callbacks run. Malformed or oversized updates leave the
previous State unchanged. Projection absence differs from known empty.
Conversation state provides ordered threads, turns, items, and pending requests
with lookup; item content updates replace the server's accumulated bounded
channel value rather than appending a presumed delta.

Stable state records are typed for provider, controller, sessions, thread-list metadata, threads,
turns, items, pending requests, accounts, models, configuration, processes,
filesystem watches, fuzzy searches, permission profiles, reviews, apps,
external agents, hooks, marketplace, plugins, skills, MCP, Windows sandbox,
platform/remote control, notices, activities, capacity, truncation, and
diagnostics. JSON remains only in protocol-defined opaque configuration or
provider details, unknown-request results, injected Responses API values,
bounded error details, and bounded compatibility extensions. Known changes use
typed alternatives and stable IDs rather than a stringly domain/id pair.

The reducer has a dedicated path for each of the 26 expanded event families,
including the compact typed `threadList.updated` family.
Singular process, watch, search, notice, activity, and diagnostic events preserve
unrelated records; content is replaced with its truncation metadata; malformed
second events and over-capacity candidates roll back the whole transaction.

`visibleSequence` is the last fully applied visible global occurrence.
`synchronizedThrough` is the authoritative global journal cursor from a
completed synchronization. Visible sequences may jump or repeat within an
adjacent expanded occurrence group; regression is rejected. A replay containing
only hidden occurrences may emit no EventBatch and advance synchronizedThrough
beyond visibleSequence at `sync.complete`.

Initial snapshot is Welcome, Snapshot, SyncComplete. Initial replay is Welcome,
zero or more EventBatch messages, SyncComplete. The modes never mix. Explicit
snapshot/replay is complete only after the command response, following stream,
and SyncComplete. Overlapping replay events are validated without duplicating
already represented append-oriented state. Reconnect authenticates again,
creates a new observer session, restores neither pending commands nor controller
ownership, and resumes only when continuity permits.

A bare Snapshot received while Ready is handled as one live snapshot barrier.
The reducer performs one transactional complete-State replacement, preserves
the current physical SessionInfo, projection fingerprint, and pending command
map, remains Ready, and sets both visibleSequence and synchronizedThrough to
the snapshot sequence. Lower sequences fail as StateDivergence, equal
sequences replace idempotently without another cursor callback, and higher
sequences advance the reconnect cursor once. The update uses
`SnapshotFallback`; it does not enter Synchronizing, wait for SyncComplete, or
emit `onSynchronized`.

The first live failure exposed by the real Unix reference applications was an
oversized same-occurrence expansion of `thread.list.updated`: the server
re-expanded every retained thread, selected a bounded bare Snapshot fallback,
and the SDK incorrectly routed that Ready-state Snapshot into its
synchronization-only handler, which reported `synchronization message outside
synchronization` and asked the physical transport to close. The backend had
already sent its complete queued output; stdin remained active and was not the
cause. The corrected server keeps page-thread upserts independent and emits one
compact thread-list metadata event. The reference client also preserves the
first typed SDK error and transport close reason, while intentional quit and
framework-signal shutdown no longer masquerade as remote disconnects.
Nonblocking stdin still treats only `read() == 0` as EOF; `EAGAIN` remains “no
bytes available now.” The stdin event receiver also marks framework shutdown
before pending connect or WebSocket-upgrade callbacks, so the pre-transport
interval is classified consistently with an attached socket.

## Reference CLI migration

`codex-backend-client` now retains parsing, presentation, pre-Ready input
queuing, EOF drain, physical transport selection, and the two-operation `new`
workflow. The SDK owns Hello, request IDs, correlation, synchronization,
replay, sparse sequences, and result validation. `--json` observes decoded
ServerMessage values through the read-only callback. `raw` accepts only a
known generated command and passes through normal SDK validation/correlation.

The application composes named SNode.C clients for Unix, IPv4, IPv6, IPv4/IPv6
TLS, RFCOMM, RFCOMM TLS, WebSocket, and WSS. Unix is enabled by default; every
other instance is disabled by default and exactly one effective outgoing
transport is required. RFCOMM receives configuration/build coverage without
requiring physical hardware.

## Reference application lifetime contract

The CLI treats command, physical-connection, and application lifetimes as
independent. A local parse error, pre-acceptance command-local SDK rejection,
or ordinary `Response(ok=false)` completes only that command, records batch
failure when applicable, releases pending capacity, and continues reading
input. Transport/send rejection remains connection-level, while pending-limit
and active-synchronization conditions are bounded deferrals. A rejected `snapshot`,
`replay`, `thread.start`, or `turn.start` follows the same rule. A failed
compound `new` clears only its active two-stage workflow; a successfully
created thread is not rolled back, the turn is not retried, and later queued
commands continue in order.

Its application state is `Disconnected`, `Connecting`, `Synchronizing`,
`Ready`, `ShuttingDown`, or `Closed`; `Disconnected` is deliberately a running,
nonterminal state and is not the SDK's terminal `Closed` state.

A malformed protocol stream, SDK state divergence, authentication failure, or
transport loss still closes that physical attachment and fails accepted
operations exactly once. It does not stop the interactive process. The
application enters `Disconnected`, retains stale SDK State, keeps stdin active,
and permits local `help`, `watch`, `reconnect`, and `quit`. Remote commands are
rejected with reconnect guidance instead of being queued for later surprise
execution.

`reconnect` starts one fresh physical attempt using the selected SNode.C
transport configuration and attaches a new generation to the same SDK Client.
There is no automatic reconnect, command retry, or controller reacquisition.
No operation or unsubmitted command from the failed attachment is carried into
the new one.

The physical-attempt gate gives each native or WebSocket attempt an immutable
generation. Native context factories capture it directly. WebSocket HTTP
begin/end callbacks capture it per attempt and bind it to the exact originating
socket connection before upgrade; subprotocol construction accepts only that
generation/socket pair. A late callback or factory from a retired transport
therefore cannot attach under, fail, or retire a later reconnect attempt.

The application queue is bounded by `--maximum-queued-commands` (default 256)
and `--maximum-queued-command-bytes` (default 16 MiB). Zero means zero queue
capacity. Checked accounting rejects the newest overflowing input without
evicting old work. Connecting/Synchronizing input and temporary pre-acceptance
deferrals may use the queue; Disconnected remote commands may not. One active
`new` workflow is represented separately from later queued `new` entries, so a
second command cannot overwrite the first workflow's stage.

True stdin EOF drains the complete existing queue, accepted SDK operations,
active `new`, and explicit synchronization even after individual commands
fail. It exits nonzero only after the full drain when any line/command failed;
an all-success drain exits zero. Connection loss during drain accounts for and
discards unsubmitted work without waiting for a manual reconnect. Interactive
`quit` and framework termination signals remain intentional process shutdown;
`EAGAIN`/`EWOULDBLOCK` remain temporary no-input conditions rather than EOF.

## Exact expanded-event identity

The projection authority resolves identity through reviewed family-specific
paths. Thread upsert uses `data.thread.id`, thread removal uses
`data.threadId`, turn upsert uses `data.turn.id` and
`data.turn.threadId`, item upsert/content uses the exact item and parent IDs,
and process/watch/search/activity updates use their stable wrapped handle or
key. Notice addition carries the triggering canonical notice occurrence.
Equivalent extension mappings use explicit reviewed `params` paths; projection
does not recursively guess an arbitrary nested `id`.

`thread/deleted` maps to `thread.removed`. The only notification methods that
map to accumulated `item.content.updated` replacement are
`item/agentMessage/delta`, `item/commandExecution/outputDelta`,
`item/fileChange/outputDelta`, `item/reasoning/summaryTextDelta`, and
`item/reasoning/textDelta`. Other item lifecycle/progress/plan/summary-part
notifications map to `item.upserted`.

The shared `commandOutput` field is filtered semantically by its exact item
type—command execution requires command-execution scope and file change
requires filesystem-write scope. The implementation walks items directly, so
the generic 128-rule bound cannot expose later entries; unknown or conflicting
types require both scopes or omit the field.

The live identity defect returned 25 distinct thread-list page IDs but emitted
25 `thread.upserted` events for one retained tail thread because
`data.thread.id` was not read and `snapshot.threads.back()` was substituted.
All identity-bearing first/last fallbacks are removed. The corrected page emits
the exact 25 unique page IDs with matching content and one compact
`threadList.updated`; unrelated retained threads remain unchanged. Missing or
unresolvable identity selects bounded Snapshot fallback instead of fabricating
or substituting another entity. Live and replay use the same canonical record.

This correction does not change `snodec.codex-frontend`, protocol version 1,
the eight message kinds, 105 methods, or 26 expanded event families. The SDK
public API and ABI remain unchanged.

## Repair closure and resource policy

`allowLegacyV1`, inbound message bytes, decoded-state bytes, pending operations,
and retained diagnostics all have enforced finite behavior; zero means zero
capacity. Every receive overload routes through equivalent serialized-size
admission. Callback submissions enter a bounded FIFO and are sent only after
the outer dispatch frame completes. The reviewed binding authority classifies
secret-bearing operations; all reverse responses are conservatively sensitive.

The state-size serializer uses explicit portable `Json::object()` and
`Json::array()` construction, conditional optional assignment, and element-wise
array insertion. It therefore builds with the supported GCC 15.3 and
nlohmann-json 3.11.3 baseline without relying on newer implicit conversions.

The CLI's normal commands use typed façades. Its `new` workflow receives a
typed `ThreadStartResult`, carries the typed `ThreadId` into typed
`TurnStartParams`, and never extracts the ID from response JSON. Only the
diagnostic `raw` command uses the restricted generated variant.

## Decoded-State accounting performance

`maximumDecodedStateBytes` retains its original language-independent meaning:
the compact canonical JSON encoding produced by the private `encodeState()`
authority, plus one `sizeof(frontend::SequenceNumber)` contribution for each
present internal-only replay boundary. A private, fixed-size State-size ledger
implements that same metric incrementally; allocator capacity, C++ object
layout, and derived implementation data are not counted.

The ledger caches the encoded contribution of every canonical top-level State
section. Ordered entity collections additionally retain an element count and
the sum of compact encoded element sizes, so delimiters and commas remain
exact. A singular event recalculates only its changed entity, directly changed
parent-order record, and affected section metadata. Cursor and synchronization
transitions update only their small metadata sections. Checked addition and
subtraction fail closed, and the ledger is copied with the immutable reducer
candidate, so a malformed or oversized candidate cannot change the committed
State or its accounting.

Complete ledger construction remains appropriate for the empty initial State,
fresh synchronization staging, and complete snapshot replacement. Whole
collection replacement walks and rebuilds only that collection's contribution.
Ordinary singular live/replay events and cursor-only commits neither encode nor
traverse the complete State for capacity admission. In Debug builds, every
successful candidate is compared before commit with the canonical full encoder;
that reference assertion is compiled out under `NDEBUG`. The 54 reducer fixtures
also compare the incremental and reference metrics after each successful
protocol input, while focused edge coverage fixes exact `actual - 1`, `actual`,
and `actual + 1` boundaries, zero capacity, arithmetic failure, escaped/UTF-8
strings, large numbers, opaque JSON, append/eviction, and rollback behavior.

A focused diagnostic benchmark evaluated the complete reducer path at 100,
1,000, and 10,000 retained items. At 10,000 items, immutable candidate copying
took 4.684 ms, canonical whole-State accounting took 98.411 ms, a ledger read
took 0.013 µs, and a last-item linear lookup took 35.4 µs. With incremental
accounting, representative ordinary updates took 5.65–7.50 ms instead of
104.0–105.7 ms. Candidate copying was 62–83% of the new complete update cost;
linear lookup was 0.47–0.63%.

The steering addendum's permitted no-index outcome was therefore selected.
Adding copied hash indexes would increase the dominant immutable candidate-copy
and rollback work for less than a 0.7% complete-path benefit in this model;
synthetic 10,000-entry map copy/rebuild cost 392/454 µs versus a 35.4 µs scan.
Ordered vectors and existing linear public/reducer lookup consequently remain
unchanged. A meaningful ID-index optimization should be designed together with
future structural sharing or persistent entity storage; that redesign is
outside A1.7c-1. Overall reducer mutation still scales with retained State even
though its capacity admission is now constant-time after changed-section
bookkeeping.

## Packaging and verification inventory

The new target uses project version 0.1.0 and SOVERSION 2. It installs 33
public SDK headers. The complete installed Codex header inventory is therefore
29 main + 7 backend + 9 frontend + 33 frontend-client = 78. Isolated installed
headers compile independently, the installed consumer links only
`AISuite::OpenAICodexFrontendClient`, and the exported SDK target has no SNode.C
network dependency.

Reducer conformance fixtures are stored under
`tests/component/codex/fixtures/frontend-client-reducer/` for reuse by A1.7d.
The version-3 JSON authority contains 54 executable cases, each with literal
input messages plus exact normalized state, typed changes, cursors,
synchronization result, connection disposition, and expected error. The C++
tests cover legacy/expanded snapshots, all 26 expanded event families,
sparse sequences, repeated occurrence groups, hidden suffixes, zero-visible
replay, snapshot fallback, overlapping replay, content replacement, capacity,
projection continuity, and transactional rollback.

Final server-projection boundary validation used the established `build-a14c`
tree on a 28-CPU host with 62 GiB RAM, build parallelism 26, and ordinary
CTest parallelism 2.
`cmake --build build-a14c --target all --parallel 26` completed successfully.
The existing protocol and C++ binding authorities were current. Reducer fixture
conformance, incremental/reference accounting equivalence, Debug invariant and
no-hot-path-rebuild guards, public-header self-containment, installed typed
consumer, SDK symbol visibility, dependency policy, binary package, and source
package checks all passed. Changed C++ formatting, CMake reconfiguration,
workflow syntax, and `git diff --check` passed. The focused accounting/reducer
checks passed before the final suite. Focused live-snapshot, compact thread-list
projection, exact identity, command/connection/process lifetime, bounded queue,
close-reason, stdin, native reconnect, and WebSocket reconnect regressions also
passed. The focused projection/schema/service/currentness set passed 14/14 in
11.38 seconds, and the Unix, IPv4/IPv6, TLS, WebSocket, and WSS reconnect set
passed 9/9 in 1.79 seconds under GCC 15 Debug.

The final cross-layer FrontendService-to-SDK regression completed in 5.25
seconds. It exercises typed `thread.start` and `turn.start`, backend
`user_message` and `agent_message` item lifecycles, an `agentText` delta, turn
completion, serialized generated-schema validation, SDK reduction, journal
replay, and a second typed command on the same Ready physical connection. The
raw-occurrence regression proves the complete serialized `item.upserted`
contains `userMessage` rather than `user_message`; the full 18-kind mapping,
unknown-item containment, exact thread/turn/item lookup, bounded discriminator
diagnostic, and producer rejection-before-transport cases all passed.

Mechanical manifest verification before and after this repair produced the
same authority: protocol identity `snodec.codex-frontend`, version 1, eight
message kinds, 105 methods, 26 expanded event families, and 18 ThreadItem
discriminators. No public client-SDK source, API, ABI, SOVERSION, or generated
protocol vocabulary changed, and no `unknown` ThreadItem discriminator was
added.
The final ordinary suite command was:

```console
ctest --test-dir build-a14c --output-on-failure --parallel 2
```

The final corrected run registered 190 tests: 189 passed, one skipped, and zero
failed in 435.39 seconds. The single skip was
`CodexTypedAppServerIntegrationTest` because
`SNODEC_RUN_CODEX_TYPED_INTEGRATION=1` was intentionally not set; that optional
real App Server test may consume configured credentials and quota. The ordinary
suite included successful installed-consumer (50.84 seconds), binary-package
(23.20 seconds), source-package (6.23 seconds), public-header
self-containment (140.47 seconds), generated-authority, and SDK binding
currentness coverage. Physical RFCOMM validation was not attempted; its
hardware-independent configuration/build coverage passed.

The hosted Debug runner exposed that public-header self-containment still
started its nested 78-translation-unit build with 26 jobs even though ordinary
CTest itself had been restored to two concurrent tests. That nested build was
constrained to two jobs and its timeout raised from 300 to 600 seconds without
changing any header assertion. The corrected local run above exercises that
same path; build parallelism remains 26 while test and nested validation
parallelism are independently bounded.

The SDK-enabled single-family matrix advertises 13 static mechanisms plus the
`cpp_client_sdk` product for 14 total. The primary multi-family topology
acceptance also advertises topology-derived `multi_transport`, for 15 total.
Injected SDK-disabled truth covers the corresponding 13- and 14-capability
cases without a second build tree.

The owner-approved P0–P3 architecture reduction follows A1.7c-1. P1 provides
generic SNode.C prerequisites while P2 builds the complete greenfield server
and client beside the executable oracle; P3 cuts production over and deletes
the legacy frontend. A1.7c-2 owns `codex-ui` only after P3 is merged and targets
the reduced canonical frontend; TypeScript/browser work remains A1.7d and
provider abstraction remains later.
