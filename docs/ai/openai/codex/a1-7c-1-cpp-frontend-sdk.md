# A1.7c-1 C++ Frontend SDK

## Scope

A1.7c-1 adds the reusable transport-neutral C++ implementation of Frontend
Protocol v1 and migrates `codex-backend-client` to it. It does not modify
`codex-ui`, add Qt or TypeScript/browser code, introduce provider abstraction,
or change the protocol identity, version, message kinds, method inventory,
scope policy, journal, or projection semantics.

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

Stable state records are typed for provider, controller, sessions, threads,
turns, items, pending requests, accounts, models, configuration, processes,
filesystem watches, fuzzy searches, permission profiles, reviews, apps,
external agents, hooks, marketplace, plugins, skills, MCP, Windows sandbox,
platform/remote control, notices, activities, capacity, truncation, and
diagnostics. JSON remains only in protocol-defined opaque configuration or
provider details, unknown-request results, injected Responses API values,
bounded error details, and bounded compatibility extensions. Known changes use
typed alternatives and stable IDs rather than a stringly domain/id pair.

The reducer has a dedicated path for each of the 25 expanded event families.
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

## Packaging and verification inventory

The new target uses project version 0.1.0 and SOVERSION 2. It installs 33
public SDK headers. The complete installed Codex header inventory is therefore
29 main + 7 backend + 9 frontend + 33 frontend-client = 78. Isolated installed
headers compile independently, the installed consumer links only
`AISuite::OpenAICodexFrontendClient`, and the exported SDK target has no SNode.C
network dependency.

Reducer conformance fixtures are stored under
`tests/component/codex/fixtures/frontend-client-reducer/` for reuse by A1.7d.
The version-3 JSON authority contains 53 executable cases, each with literal
input messages plus exact normalized state, typed changes, cursors,
synchronization result, connection disposition, and expected error. The C++
tests cover legacy/expanded snapshots, all 25 expanded event families,
sparse sequences, repeated occurrence groups, hidden suffixes, zero-visible
replay, snapshot fallback, overlapping replay, content replacement, capacity,
projection continuity, and transactional rollback.

Final validation used the established `build-a17b` tree and parallelism 26.
`cmake --build build-a17b --target all --parallel 26` completed successfully.
The existing protocol authority, C++ binding authority, reducer fixtures,
public-header self-containment, installed typed consumer, SDK symbol visibility,
and dependency-policy checks passed together, 8/8. Changed-hunk C++ formatting,
new-file CMake formatting, generated currentness, and `git diff --check` passed.
The focused repair set passed 32/32; the subsequently discovered real-backend
typed-CLI acceptance drift was corrected and its focused acceptance passed.
The ordinary suite command was run once:

```console
ctest --test-dir build-a17b --output-on-failure --parallel 26
```

It registered 188 tests: 187 passed, one skipped, and zero failed in 317.94
seconds. The single skip was `CodexTypedAppServerIntegrationTest` because
`SNODEC_RUN_CODEX_TYPED_INTEGRATION=1` was intentionally not set; that optional
real App Server test may consume configured credentials and quota. The ordinary
suite included successful installed-consumer (50.10 seconds), binary-package
(22.41 seconds), source-package (7.72 seconds), public-header
self-containment (25.62 seconds), generated-authority, and SDK binding
currentness coverage. Physical RFCOMM validation was not attempted; its
hardware-independent configuration/build coverage passed.

The SDK-enabled single-family matrix advertises 13 static mechanisms plus the
`cpp_client_sdk` product for 14 total. The primary multi-family topology
acceptance also advertises topology-derived `multi_transport`, for 15 total.
Injected SDK-disabled truth covers the corresponding 13- and 14-capability
cases without a second build tree.

A1.7c-2 remains the next PR and owns `codex-ui`; TypeScript/browser work remains
A1.7d and provider abstraction remains later.
