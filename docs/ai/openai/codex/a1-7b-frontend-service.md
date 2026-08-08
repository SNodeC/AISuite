# A1.7b Codex FrontendService

A1.7b completes the transport-neutral Codex frontend server runtime while
preserving Frontend Protocol identity `snodec.codex-frontend`, version `1`, its
eight message kinds, and the 105-method catalog frozen by A1.7a. It replaces
the former `BackendAdapter` public name with the PIMPL-backed
`FrontendService`, authenticates each connection before creating a BackendCore
frontend session, activates all 105 approved handlers, applies mandatory
per-principal projection, and lets several listener families borrow the same
service.

The runtime still has exactly one provider context:

```text
Codex App Server
        ↓
typed AppServerClient
        ↓
one BackendCore
        ↓
one FrontendService
        ├─ authentication and admission accounting
        ├─ authorization and deployment policy
        ├─ one controller
        ├─ one global frontend sequence
        └─ one canonical EventJournal
              ↓
       per-principal projection
              ↓
Unix / TCP / TLS / WebSocket / WSS / RFCOMM listeners
```

Listener factories borrow `FrontendService&`. An accepted transport owns one
move-only `FrontendConnection`; it does not own another service, journal,
controller, provider generation, or BackendCore. Transport code is limited to
framing, peer metadata, TLS and Origin metadata, bounded writing, and close.
Authentication, method policy, controller checks, BackendCommand mapping,
snapshot/replay selection, and projection remain service-owned.

## Public service boundary

The installed header is
`<ai/openai/codex/frontend/FrontendService.h>`.
`BackendAdapter.h` and a compatibility alias are deliberately absent from the
unreleased SOVERSION-2 surface. The reusable shared library and imported target
remain `libaisuite-openai-codex-frontend.so.2` and
`AISuite::OpenAICodexFrontend`.

`FrontendService` accepts any `BackendCore<ClientT>` through its template
constructor and keeps its non-template implementation behind PIMPL. Its
transport-neutral entry point is:

```cpp
frontend::FrontendService service(backend, options);
frontend::FrontendConnection connection =
    service.openConnection(peer, callbacks);
```

`FrontendConnection` is move-only and accepts typed messages, JSON values, or
compact JSON text. Its close and callback paths contain exceptions. Safe
service diagnostics report connection/authentication counts, controller ID,
defined/implemented/available methods, declared bound transport kinds, and
implemented capabilities; they do not expose credentials, private keys,
command parameters, journal internals, file descriptors, BackendState
mutators, or the App Server client.
An authenticated `FrontendConnection::principal()` reports the safe principal
ID, profile, scopes, and local-trust classification; it never reports the
credential that established them. The insecure override uses a distinguishable
principal ID rather than masquerading as verified peer identity.

The installed header inventory stays 29 main + 7 backend + 9 frontend = 45.
Project version `0.1.0` and `AISUITE_CODEX_SOVERSION = 2` are unchanged.
The nine frontend headers are `Codec.h`, `EventCoalescer.h`, `EventJournal.h`,
`FrontendService.h`, `GeneratedProtocol.h`, `Messages.h`, `Protocol.h`,
`Security.h`, and `UpdateBatch.h`.

## Authentication handshake

Hello retains its original valid form and additively accepts one optional
credential object:

```json
{
  "protocol": "snodec.codex-frontend",
  "version": 1,
  "kind": "hello",
  "authentication": {
    "scheme": "bearer",
    "token": "secret bytes"
  }
}
```

Authentication completes before `FrontendService` opens a BackendCore
`FrontendSession`. A failed attempt creates no session, controller state,
command correlation, replay state, or journal entry. Each connection receives
one authentication attempt. Failure accounting is additionally bounded per
peer/address in an event-loop time window; its defaults are three failures in
60 seconds. The generic pre-authentication errors are
`authentication_required`, `authentication_failed`, `origin_rejected`,
`transport_security_required`, and `rate_limited`.

The default service limits are 128 total connections, 16 unauthenticated
connections, a 10-second handshake deadline, a 1 MiB inbound message, 50
messages per second with a burst of 100, 256 outstanding commands per
connection, and the three-per-minute failed-authentication budget. Zero means
zero capacity. Scheduling and expiry use the SNode.C event loop; the service
adds no thread, future, coroutine lifecycle, sleep, or polling loop.

### Verified Unix trust

Unix transport identity alone is never trust. The Unix adapter asks SNode.C
2.0's `net::un::peerCredentials()` for the accepted socket's peer facts. It sets
`localPeer` and `unixUserId` only after successful credential extraction. The
reference policy grants `local_trusted` only when local trust is enabled, the
filesystem socket is owned by the service effective user with owner-only mode,
and the peer effective UID matches that user.

The default socket mode is `0600`. Missing peer credentials do not silently
become local trust; the reference application then requires bearer
authentication. The insecure local-trust bypass is a separate explicit
operator option and emits a structural warning without credential material.

### Reference bearer policy

Remote and untrusted connections authenticate through the optional Hello
bearer credential. `codex-backend` accepts the reference token only from a
protected file, read once at startup. It rejects a missing, unreadable,
non-regular, empty, overlarge, group/world-accessible, or NUL-containing file.
Exactly one final LF or CRLF is removed; all other token bytes are preserved.
Comparison is constant-time over the maximum candidate length, and retained
token storage is erased when released. A literal bearer token is not a command
line option and is never placed in a URL, cookie, query, WebSocket path or
subprotocol, Welcome, snapshot, event, journal, diagnostic, or generated
configuration dump.

The reference remote principal defaults to profile `default_remote` with
exactly `observe` and `control`. The verified local profile is
`local_trusted`, containing all 12 frozen scopes.

## Runtime method policy

A1.7b distinguishes four stable layers:

- **defined**: present in the v1 contract;
- **implemented**: backed by a complete handler in this build;
- **deployment-enabled**: admitted by the application gates;
- **permitted**: deployment-enabled and allowed by the authenticated
  principal's static scope policy.

Controller ownership, provider readiness, parameter-sensitive policy, and
capacity are invocation-time checks, not inputs to `permittedMethods`.
Welcome reports `availableMethods` as implemented and deployment-enabled, and
reports `permittedMethods` as that set filtered by the principal's static
scopes.

The frozen arithmetic is:

```text
defined methods                         105
implemented handlers                    105
conditional/default-disabled             15
default available                         90

default_remote permitted              53 / 90
local_trusted permitted                90 / 90

default_remote exclusions:
  privileged provider operations           22
  reverse response/rejection methods        12
  provider-lifecycle methods                 3
                                             --
                                             37
```

The 105 handlers are seven frontend-native methods, 86 provider operations,
and 12 reverse response/rejection methods. The native set is
`controller.acquire`, `controller.release`, `snapshot.get`, `events.replay`,
`provider.start`, `provider.stop`, and `provider.restart`. Provider operations
and reverse responses use the generated exact method/target metadata and the
existing 101-alternative `BackendCommand`; lifecycle handlers call the
existing `BackendCore::start()`, `stop()`, and `restart()` API instead of
adding lifecycle command alternatives.

The exact 15 conditional methods remain implemented but absent from the
default available set:

```text
fs.copy                    fs.createDirectory
fs.getMetadata             fs.readDirectory
fs.readFile                fs.remove
fs.unwatch                 fs.watch
fs.writeFile               fuzzyFileSearch
command.exec               command.exec.resize
command.exec.terminate     command.exec.write
thread.shellCommand
```

Their deployment gates are independent from scopes. Filesystem methods also
pass the configured canonical-path invocation policy, and command methods pass
the configured execution allow-list policy. Enabling a gate does not grant a
scope or controller ownership, while holding all scopes does not enable a
deployment gate.

`account.read` remains parameter-sensitive after schema validation:

- omitted or false `refreshToken` requires `observe` and no controller;
- `refreshToken: true` requires `control`, `account_management`, and current
  controller ownership.

Because the observer form is available, `account.read` appears in the
`default_remote` permitted catalog. The stronger branch is enforced only at
invocation time.

The current implementation is safe because the codec rejects defined-field
and extension collisions before this policy reads the tagged parameters; this
is not considered an exploitable condition. A1.7c-1 removes the
cross-translation-unit coupling by deriving the `account.read` `refreshToken`
policy from the already validated normalized params object rather than
rereading the original tagged parameter value.

Every command follows this order: frame bound, structural envelope decoding,
completed authentication, exact MethodId lookup, handler and deployment
checks, generated parameter validation, parameter-sensitive policy, required
scopes, controller ownership, provider readiness, capacity, exact service
action or BackendCommand construction, and asynchronous submission. An
unauthenticated client cannot probe whether a privileged method exists, is
enabled, has valid parameters, or has a ready provider.

Runtime dispatch does not add a second 105-method list. Owner exposure and
security decisions remain in the production protocol registry; generation
continues in one direction from `app_server_surface.py` through the committed
frontend-registry export into the manifest, Draft 2020-12 schema, and
`GeneratedProtocol.h`. A1.7b runtime metadata marks all 105 handlers
implemented, preserves the 15 conditional set, and preserves the 234 reviewed
identities with zero unresolved decisions. Exact MethodId lookup, generated
scope/controller/readiness policy, and generated provider/reverse targets are
the service's dispatch authority.

## Controller, provider, and reconnect semantics

Controller ownership is global across all transports. Many authenticated
observers may connect, but exactly one connection may explicitly acquire the
controller with `control` scope. Scope possession does not acquire control;
controller ownership grants no scope. Only the owner may release it, and
disconnect or protocol-failure closure releases it without promoting another
connection. Pending reverse requests and the provider survive controller
loss. There is no lease, forced takeover, automatic promotion, or
multi-controller mode.

`provider.start`, `provider.stop`, and `provider.restart` require `control`,
`provider_lifecycle`, and controller ownership. They reuse BackendCore's
lifecycle and generation machinery. Stopping the provider does not stop
`codex-backend`, its listeners, frontend sessions, or the frontend journal.

Reconnect creates a new authenticated frontend session. `resumeAfter` is only
a cursor into the shared EventJournal; it does not restore controller
ownership, request correlation, prior identity, scopes, or a durable session
token. Authentication is repeated, retained records are projected for the new
principal, and a replay gap falls back to a projected snapshot before
`sync.complete`.

## Canonical journal and mandatory projection

FrontendService owns one global sequence and one bounded canonical
EventJournal. A backend transition is first normalized into a bounded,
transport-neutral canonical record, then assigned one sequence and stored
once. Before retention, AISuite removes known structured authentication
material, known credential/token/password/private-key/API-key/cookie fields,
reviewed secret-response fields, and unsafe raw provider envelopes. The same
record is projected separately for each connection.

The installed `EventJournal` surface exposes no legacy public event append or
replay path. FrontendService's private canonical append/replay path is the only
journal authority, so a caller cannot bypass canonicalization and projection
with a second record form.

Arbitrary user, model, reasoning, notice, diagnostic, process-output, and
command-output text remains potentially sensitive. AISuite does not attempt
heuristic token detection in such text: bounded privileged content may remain
canonical when needed to construct a complete authorized local projection and
is protected through mandatory per-principal scope filtering. A
`local_trusted` connection can therefore receive authorized
filesystem/process/configuration state while `default_remote` receives an
omitted or redacted projection of the same record with the same occurrence and
sequence.

Projection order is fixed:

1. known structured-secret and unsafe-envelope removal plus bounding;
2. unconditional principal scope filtering;
3. legacy or expanded representation selection;
4. optional omission/redaction metadata;
5. encoding and bounded enqueue.

Scope filtering never depends on requested capabilities. The
`scope_projected_state` capability advertises explicit omission/redaction
metadata only; omitting that capability cannot reveal more information.
Initial snapshot, `snapshot.get`, live events, and replay use the same security
rules. The legacy and expanded views retain the same information ceiling for
equal scopes and never duplicate one provider occurrence.

The complete mapping covers 68 stable notifications, 18 `ThreadItem`
alternatives, ten pending-request kinds, and 25 expanded event families.
Legacy clients retain the original 15-method response bytes and existing
normalized/redacted projections after mandatory filtering. Expanded clients
may negotiate complete backend domains, dedicated requests/events/items, and
explicit omission metadata without receiving a second legacy copy.

### Sparse visible sequences and replay cursors

Every canonical occurrence owns one number from the single global frontend
sequence. Each authenticated principal sees an ordered subset of those global
numbers, and mandatory projection may make the visible subset sparse. A jump
between visible event sequences does not itself mean loss, and the service
does not renumber per principal or emit filler, tombstone, hidden-event, or
omission-count records that would disclose a privileged occurrence.

`resumeAfter` is a global journal cursor. Replay uses the reconnecting
principal's current scopes and negotiated representation capabilities, so it
may return sparse visible events or no event batch for a nonempty canonical
suffix. `EventBatch.fromSequence` and `toSequence` are the first and last
visible sequences in that batch; individual event sequences remain the global
occurrence identifiers. After applying all preceding synchronization
messages, the client persists `sync.complete.sequence` as the authoritative
cursor. It may exceed the last visible event when a suffix contains only
hidden occurrences. Replay availability is determined by the journal floor;
only a server-reported gap selects snapshot fallback, and replay and snapshot
are not mixed.

The C++ Frontend SDK tracks the global replay cursor, accepts sparse visible
sequences, and persists `sync.complete.sequence` as the synchronization
boundary.

## Capabilities and bound-listener topology

A complete A1.7b service implements these 13 mechanism/build capabilities:

```text
method_discovery                 security_scopes
complete_provider_operations     complete_reverse_requests
complete_backend_domains         conditional_filesystem
conditional_command_execution    dedicated_pending_requests
dedicated_notification_events    complete_thread_items
authenticated_frontend           scope_projected_state
provider_lifecycle
```

`conditional_filesystem` and `conditional_command_execution` describe working
mechanisms and remain implemented when their 15 methods are deployment-off.
Product capabilities remain a separate category from these 13 static
mechanisms. A1.7c-1 makes `cpp_client_sdk` build-derived; TypeScript, browser,
and Qt products remain false.

`multi_transport` is a separate conditional topology capability. It is false
for one declared transport family and true for more than one. SNode.C owns
listener configuration and lifecycle; the service-side declaration fact exists
only to derive protocol topology truth and is not a listener registry.

## Transport composition

Stream listeners use compact JSONL with fragmentation, multi-line reads, CRLF
tolerance, bounded framing, parse containment, and per-connection writer
backpressure. WebSocket uses one complete JSON object per text message and no
newline; binary messages are rejected. Every form opens the same
transport-neutral `FrontendConnection`.

`main.cpp` constructs the Unix, IPv4, IPv6, IPv4/IPv6 TLS, RFCOMM, and
RFCOMM-TLS SNode.C server templates explicitly. Their instance configuration
lambdas are visible at each construction site; AISuite provides no listener
constructor wrapper that could hide or duplicate native SNode.C configuration.

SNode.C 2.0 owns HTTP parsing and resource admission. The reference application
configures its parser for 8 KiB start and header lines, 64 KiB aggregate
headers, 128 fields, and the existing body ceiling. Server configuration allows
one pending request and disables chunked transfer and pipelining. Express
middleware then applies AISuite's method, endpoint, Origin, credential-channel,
and request semantics. WebSocket upgrade and framing remain SNode.C concerns;
the dynamically loaded `codex` server subprotocol opens the FrontendConnection
only after a successful upgrade.

The application uses the verified installed SNode.C target families:

```text
snodec::core
snodec::net-un-stream-legacy
snodec::net-in-stream-legacy
snodec::net-in6-stream-legacy
snodec::net-in-stream-tls
snodec::net-in6-stream-tls
snodec::net-rc-stream-legacy
snodec::net-rc-stream-tls
snodec::http-server
snodec::http-server-express
snodec::http-server-express-legacy-in
snodec::http-server-express-legacy-in6
snodec::http-server-express-tls-in
snodec::http-server-express-tls-in6
snodec::websocket-server
snodec::websocket-client
```

`snodec::websocket-client` is test-only. The transport implementation neither
uses Node.js/browser automation nor modifies SNode.C. TLS, WebSocket, and
RFCOMM builds are controlled by
`AISUITE_ENABLE_CODEX_FRONTEND_TLS`,
`AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET`, and
`AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM`; optional application transport targets
do not leak into library-only AISuite consumers. RFCOMM composition and
framing can be validated without claiming physical Bluetooth hardware.

Plain IPv4/IPv6 listeners default to loopback and off. A non-loopback
plaintext bind needs the explicit insecure-remote override but still requires
bearer authentication. TLS and WSS require configured certificate and key
files; no downgrade is attempted. Bluetooth pairing is not frontend
authentication.

### Origin and static-root policy

The WebSocket endpoint is exactly `/frontend` by default. Browser Origin
admission normalizes scheme, host, and effective port and defaults to same
origin; an explicit allow-list may add normalized origins. Native clients may
omit Origin but must still authenticate in Hello. Credentials are rejected in
URL/query, cookie, Authorization header, and WebSocket subprotocol channels.
Non-loopback browser deployment requires WSS unless the explicit insecure
override is present.

The default WebSocket message ceiling is exactly 1 MiB (1,048,576 bytes).
Each accepted message must be one complete text JSON value; SNode.C enforces
frame/message limits and the AISuite subprotocol rejects binary messages. An
upgrade requires exactly the WebSocket subprotocol token `codex`, which is
distinct from the Frontend Protocol identity `snodec.codex-frontend`.
SNode.C owns the finite 13 MiB writer queue and connection-local backpressure.

An optional static root serves only `GET` and `HEAD`. The root is canonicalized
once, retained as an open directory descriptor, and may not be the filesystem
root. Request targets have a fixed byte bound and reject malformed, repeated
percent-encoded, dot-segment, backslash, NUL, directory, and symlink escape
paths. Files are opened relative to that retained descriptor, component by
component with `openat()` and `O_NOFOLLOW`; replacing the configured pathname
after startup cannot redirect the service to another tree. Assets must be
regular, stay within the configured maximum asset size, and use an explicit
MIME allow-list.

For GET, SNode.C 2.0's `FileReader::adopt()` takes ownership of the already
authorized final descriptor without reopening its pathname, and
`Response::pipe()` connects it to the framework's descriptor-streaming and
backpressure lifecycle. The complete asset is neither allocated by AISuite nor
required to fit in the instantaneous writer queue. Until `pipe()` succeeds, an
application-private guard owns the obligation to stop the adopted reader; a
false return or exception stops it exactly once. Successful `pipe()` releases
that guard and transfers the source lifecycle to SNode.C. EOF, read error,
response failure, abort, and disconnect are then handled by the SNode.C
source/pipe chain. `HEAD` performs the same secure resolution, emits the exact
GET representation length without a body, and releases the descriptor after
metadata is established. Directory listing is not provided.

SNode.C bounds a decoded HTTP request body at one byte because zero means
unlimited in the framework configuration. AISuite's common application policy
rejects that one byte, and therefore every non-empty static or WebSocket
upgrade request. A declared body larger than one byte receives HTTP 413 in the
SNode.C parser before Express route dispatch. Chunked requests remain disabled.
Static responses include a CSP with `frame-ancestors 'none'`,
`X-Content-Type-Options: nosniff`, and `Referrer-Policy: no-referrer`.
AISuite never logs bearer tokens, credentials, raw Hello messages, or
Authorization values. Operators must keep framework payload tracing disabled
in production because framework tracing is configured and owned by SNode.C.

## Backpressure and failure isolation

The default retained journal is bounded at 4,096 records and 8 MiB. The
service queue is bounded at 512 messages and 11 MiB per connection, and the
reference stream/WebSocket writer is bounded at 13 MiB. This preserves the
required relationship:

```text
retained replay bytes <= service outbound bytes <= transport writer bytes
```

A slow or malformed client closes only its own connection. Closing suppresses
stale completion delivery, releases its request-correlation state and
controller role, and leaves BackendCore, the provider, other listeners, and
other clients intact. BackendCore continues to own provider-operation
lifecycle; FrontendService owns only frontend correlation.

Static bodies use SNode.C's HTTP response pipe rather than the FrontendService
or WebSocket message queue. Framework source and SocketWriter backpressure do
not require an entire asset to enter a transport writer at once. Static
transfer failure remains local to that HTTP connection.

## Deterministic web-security guards

The focused A1.7b tests use SNode.C's own HTTP and WebSocket clients rather
than Node.js, browser automation, or a custom socket test client:

- `CodexFrontendWebSocketIntegrationTest` exercises SNode.C's configured HTTP
  parser/server limits and dynamic `codex` subprotocol loading. Its live IPv4
  cases cover wrong subprotocol, missing/bad/good bearer Hello, same Origin,
  binary input, a configured oversized text message, post-failure reuse, 90
  available methods, `default_remote` 53/90, and the 13 advertised mechanisms.
- `CodexFrontendWebHttpIntegrationTest` exercises live static-root and
  rootless HTTP, traversal and symlink denial, MIME selection, exact endpoint,
  query/Authorization/cookie credential-channel rejection, Origin failure
  budgeting, security headers, exact streamed GET bytes, body-free HEAD with
  the exact representation length, and synthetic-secret absence. It also
  streams an asset larger than the instantaneous writer capacity and observes
  that the unauthenticated service reservation remains held while the HTTP
  response is written and that no BackendCore session, controller, or journal
  entry is created.
- `CodexFrontendWebSocketTlsIntegrationTest` uses a deterministic synthetic
  certificate and `snodec::websocket-client` to complete an authenticated WSS
  Hello/Welcome/snapshot/sync exchange. It verifies certificate-backed TLS,
  encrypted peer metadata, same Origin, 90 available methods,
  `default_remote` 53/90, text-only output, and bearer absence from server
  output.
- `CodexFrontendWebSecurityTest` and
  `CodexFrontendWebConfigurationTest` independently guard normalized Origin,
  static-root descriptor retention, body-free `HEAD`, maximum asset size,
  security headers, safe defaults, and all listener/configuration gates.
- Static HTTP integration proves that the secure resolver returns descriptor
  ownership rather than a body, `FileReader::adopt()` and `Response::pipe()`
  stream byte-exact GET responses, HEAD is body-free with the same length, and
  pathname traversal, encoded traversal, symlink escape, MIME, and size guards
  remain intact.

These are deterministic contract/security assertions, not wall-clock load
benchmarks and not claims of browser-product or physical-Bluetooth coverage.

## Roadmap boundary

A1.7b does not add a frontend client library, migrate
`codex-backend-client`, modify `codex-ui`, or provide browser assets.

The public C++ protocol payload model remains the A1.7a method-tagged,
schema-validated JSON layer: generated parameter/result tags distinguish all
105 methods, while payload values remain `nlohmann::json`. A1.7b consumes that
wire contract in the server; it does not pretend to be the ergonomic,
domain-typed application SDK.

A1.7c-1 implements the C++ Frontend SDK plus `codex-backend-client`
migration. A1.7c-2 immediately follows and migrates the
existing `codex-ui` into the canonical standalone AI IDE. No additional PR is
inserted before `codex-ui`. A1.7d owns the TypeScript Frontend SDK and browser
frontend.
