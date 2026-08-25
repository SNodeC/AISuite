# codex / codex-bridge Architecture

> **Implementation provenance:** This architecture and its initial implementation
> were created with Codex using GPT-5.6-Sol at high reasoning effort.

## Goal

`codex` is the canonical slim Codex integration for AISuite. It makes Codex
available to SNode.C applications through transports, framing, typed facades,
and explicit routing.

## Runtime Shape

```text
CodexUI / CodexWebUI / codex-bridge-client / SNode.C apps
        |
        | slim bridge JSON protocol over SNode.C transports
        v
codex-bridge
        |
        | native app-server JSON-RPC
        v
OpenAI Codex app-server
```

The Codex app-server is the semantic authority. `codex-bridge` is not a second app-server and must not own Codex thread, turn, item, command, or pending-request state.

## `codex-bridge` Architecture

`codex-bridge` is the server-side SNode.C composition application. It owns one
application `CodexBridge`, exactly one provider endpoint to one Codex app-server
session, and any number of enabled frontend listener instances. It is a routing
and transport process, not a retained Codex service.

The application sources belong in:

```text
src/apps/codex-bridge/
    main.cpp
    Configuration.{h,cpp}
    ProviderApplication.{h,cpp}
    WebSocketApplication.{h,cpp}
```

Reusable implementation belongs in:

```text
src/ai/openai/codex/
    bridge/
        CodexBridge
        Endpoint
    provider/
        StdioAppServer
        WebSocketAppServer
    frontend/
        StreamSocketContext
        StreamSocketContextFactory
        WebSocketSubProtocol
        WebSocketUpgrade
    protocol/
        Envelope
        JsonLineFramer
        generated/ProtocolTypes
```

The concrete runtime object graph is:

```text
main
  |
  +-- Configuration : utils::SubCommand
  +-- CodexBridge                         application-owned SDK/router
  +-- ProviderApplication
  |     |
  |     +-- exactly one provider endpoint
  |           +-- StdioAppServer
  |           |     -> owned app-server child and JSONL pipes
  |           |
  |           +-- WebSocketAppServer
  |                 -> one SNode.C Unix/IPv4/IPv6 client
  |                 -> independently managed app-server
  |
  +-- zero or more enabled frontend listeners
        +-- raw SocketServer
        |     -> StreamSocketContextFactory(CodexBridge&)
        |     -> StreamSocketContext(CodexBridge&)
        |
        +-- WebApp / WebSocket listener
              -> HTTP route and upgrade
              -> WebSocket SubProtocolFactory
              -> WebSocketSubProtocol(CodexBridge&)
```

`main` owns the bridge and all provider/listener handles. Provider and frontend
contexts borrow `CodexBridge&`; they do not own it. The bridge outlives every
provider endpoint, listener, accepted connection, and WebSocket subprotocol.
The provider endpoint is registered explicitly. Installing a second live
provider endpoint is an error.

### Provider session

Provider transport selection is an application-specific `SubCommand` option.
Address, Unix path, retry, reconnect, queue, timeout, and other transport
settings remain on the selected native SNode.C client instance. Provider modes
are limited to transports currently exposed by app-server: stdio JSONL, Unix
WebSocket, IPv4 WebSocket, and IPv6 WebSocket.

Stdio mode owns the app-server child directly and connects its standard streams
to bounded JSONL readers/writers. Network modes connect one SNode.C client to
an independently managed app-server started with the corresponding `--listen`
URL. Transport selection therefore determines ownership implicitly; no
separate ownership option exists.

Every bridge-owned app-server child is observed through a Linux pidfd registered
as a SNode.C read event receiver. Pidfd readability identifies exit of that
exact process; the callback then reaps it once and reports its exit status.
There is no periodic process polling timer or polling fallback. If pidfd cannot
be opened or registered, owned-child startup fails cleanly. Stream EOF,
provider-transport closure, and process exit remain distinct lifecycle events.
An externally managed app-server needs no process observer because the bridge
does not own that process.

Normal process-exit handling is event driven while the SNode.C loop is active.
During application shutdown, `ProviderApplication::stop()` runs only after
`core::SNodeC::start()` has returned: it detaches the pidfd receiver, sends
`SIGTERM` to the owned process group, waits once on a pidfd exit event for a
bounded one-second grace period, escalates with `SIGKILL` only if the child does
not exit, and performs the final child reap outside an event-loop callback. This
is not a recurring poll or timer. No signal or socket callback performs a
blocking child wait.

Every new provider generation performs exactly one app-server
`initialize`/`initialized` handshake. `codex-bridge` supplies its client
identity and capabilities because all frontends share this one app-server
transport session. Frontends must not independently initialize the shared
provider. Ordinary frontend requests are rejected until the handshake has
completed, and readiness is emitted as bridge telemetry.

Provider disconnect completes every outstanding local and forwarded callback
exactly once, clears only ephemeral request ownership, increments generation on
the next connection, and emits bounded lifecycle diagnostics. It does not
retain or rebuild app-server domain data.

App-server requests create only transient response-ownership records. A normal
response, frontend disconnect, provider disconnect, or matching
`serverRequest/resolved` notification retires that ownership exactly once.
The resolution notification is still forwarded unchanged. This accounting is
routing state, not a retained pending-request model or Codex-domain cache.

### Frontend listeners

Raw frontend listeners use one common JSONL `SocketContextFactory` over Unix,
IPv4, IPv6, TLS, RFCOMM, and RFCOMM TLS SNode.C server aliases. WebSocket and
WSS listeners use the same Codex text-message subprotocol over their respective
HTTP/TLS transports. Transport adapters perform framing, bounds, queue
admission, lifecycle callbacks, and diagnostics only.

The default Unix listener uses a private per-user runtime directory: a valid
private `XDG_RUNTIME_DIR`, or `/tmp/codex-bridge-<uid>` created with mode
`0700`. Its default socket path is shared with CodexUI. Frontend count is
bounded (16 by default). Network and RFCOMM listeners remain disabled unless
explicitly configured and require the deployment's own identity policy.

Each accepted frontend endpoint registers once and receives a bridge connection
identity and role. One frontend is controller according to configured policy;
the others are observers. Control transfer is explicit. Disconnecting the
controller leaves the role vacant rather than silently selecting another
frontend.

### Message routing

Frontend-to-provider routing is:

```text
bridge envelope
    -> bounded transport decode
    -> envelope and JSON-RPC classification
    -> controller/observer policy
    -> per-request upstream ID translation
    -> unchanged app-server method and params
    -> provider transport
```

JSON-RPC request IDs are local to each frontend. `CodexBridge` therefore assigns
a collision-free temporary upstream ID for every forwarded request and retains
only the connection ID plus original request ID until its response arrives. It
restores the original ID before returning the response to that frontend. This
ephemeral correlation is routing state, not semantic caching.

Provider-to-frontend routing is:

```text
app-server response
    -> route to its local/backend callback or originating frontend

app-server notification
    -> invoke an optional local typed handler
    -> fan out unchanged payload to every current frontend

app-server server-request
    -> invoke a registered local backend handler when present
    -> otherwise route only to the current controller
    -> accept a response only from the recorded owner
```

Unknown valid app-server messages and unknown fields remain available through
the raw path. Bridge telemetry uses separate `kind` values and never enters the
native app-server payload.

### Lifecycle

The executable follows the standard SNode.C application lifecycle:

```text
core::SNodeC::init(argc, argv)
    -> construct provider and listener graph
    -> schedule provider startup on the event loop
    -> initiate configured listeners
    -> core::SNodeC::start()
    -> stop provider and terminate owned child internally
```

Help and configuration-only invocations must not spawn app-server. There is no
`core::SNodeC::free()` call in `main`. Process signals, socket shutdown, TLS
shutdown, retry/reconnect, and writer backpressure remain governed by SNode.C
and the provider process supervisor rather than parallel application loops.

### WebSocket bridge injection

`CodexBridge` remains owned by the application and is not a process-wide
singleton. SNode.C's WebSocket `SubProtocolFactorySelector::link()` callback is
context-free, so the statically linked Codex subprotocol factory is itself
static and stateless.

The bridge reference is transferred to a new server subprotocol through a
scoped upgrade binding:

1. immediately around synchronous `Response::upgrade()`, the HTTP application
   creates a stack binding containing the exact `SocketConnection*` and a
   `CodexBridge&`;
2. `SubProtocolFactory::create()` obtains the connection from its
   `SubProtocolContext` and consumes the active binding only when the pointers
   match;
3. the binding can be consumed only once and constructs the subprotocol with a
   non-owning `CodexBridge&`;
4. the binding is removed when `Response::upgrade()` returns, including on
   failure and nested/reentrant upgrade paths.

This is a synchronous event-loop handoff, not global bridge lookup or threading
infrastructure. The application must outlive every listener, upgraded socket,
and subprotocol. A static factory is acceptable; a singleton `CodexBridge` is
unnecessary and must not be introduced for WebSocket composition.


### AISuite-owned responsibilities

AISuite owns only bridge-level concerns:

- SNode.C transports
- connection lifecycle
- frontend registration
- controller vs observer role
- request routing
- app-server process supervision
- JSON-RPC / JSONL framing
- delivery sequence
- frame limits
- diagnostics
- close reasons
- telemetry


## `codex-bridge-client` Architecture

`codex-bridge-client` is a thin interactive SNode.C application built on the
required codex frontend proxy SDK. Its command-line interaction maps directly
to current typed app-server and bridge operations.

The implementation follows the ordinary SNode.C client application boundary
demonstrated by `mqttcli`, `mqttintegrator`, and `mqttbridge`. Application
developers compose `SocketClient`, `SocketContextFactory`, `SocketContext`, and,
for WebSocket transport, `SubProtocolFactory` and `SubProtocol`. Generic socket
classes remain unaware of Codex. The internal template and component machinery
provides modular transport reuse; the application-facing object graph stays
small.

```text
stdin
  |
  v
CommandParser
  |
  v
ClientSession
  |
  v
ai::openai::codex::frontend::CodexBridge
  |
  v
selected SNode.C transport binding
  |
  v
codex-bridge
  |
  v
Codex app-server
```

The concrete runtime graph is:

```text
main
  |
  +-- Configuration : utils::SubCommand
  +-- Presenter
  +-- CommandParser
  +-- ClientSession
  +-- frontend::CodexBridge            application-owned SDK
  +-- ClientConnection                 SDK/transport mediator
  |
  +-- exactly one enabled SNode.C client instance
        |
        +-- raw stream
        |     SocketClient
        |       -> StreamSocketContextFactory(ClientConnection&)
        |       -> StreamSocketContext(ClientConnection&)
        |
        +-- WebSocket
              HTTP SocketClient
                -> HTTP upgrade
                -> Codex SubProtocolFactory(ClientConnection&)
                -> Codex SubProtocol(ClientConnection&)
```

Ownership is explicit: `main` owns the SDK, session, mediator, and transport
handles. Factories and per-connection contexts borrow those application
objects. A socket context or WebSocket subprotocol never owns the frontend SDK
and never outlives the application graph that supplied it.

The application sources belong in:

```text
src/apps/codex-bridge-client/
    main.cpp
    Configuration.{h,cpp}
    CommandParser.{h,cpp}
    Presenter.{h,cpp}
    ClientSession.{h,cpp}
    StdinReader.{h,cpp}
```

The responsibilities are deliberately narrow:

- `Configuration` is an SNode.C `SubCommand` containing application-specific
  options only.
- `CommandParser` exposes the supported interactive command grammar where the
  app-server or bridge has an equivalent operation.
- `ClientSession` coordinates parsed commands, typed SDK calls, callbacks,
  controller role, and reconnect lifecycle.
- `Presenter` implements concise human output and JSONL machine output.
- `StdinReader` integrates nonblocking standard input with the event loop.
- `main.cpp` is the composition root for the SDK and selected SNode.C client
  transport.

The executable lifecycle is the standard application lifecycle:

```text
core::SNodeC::init(argc, argv)
    -> construct/configure application and transport graph
    -> connect enabled client instance(s)
    -> core::SNodeC::start()
    -> inner shutdown and transport-flow termination
```

There is no `core::SNodeC::free()` call in `main`.

### Frontend SDK boundary

The application uses:

```cpp
ai::openai::codex::frontend::CodexBridge
```

Requests are asynchronous typed SDK operations:

```cpp
client.threadList(params, [](ThreadListResponse& result) {
    if (!result) {
        return;
    }

    for (const ThreadSummary& thread : result.threads()) {
        // typed access
    }

    const nlohmann::json& raw = result.getRaw();
});
```

The frontend SDK owns only outstanding callback correlation, local connection
identity, current controller role, typed handlers, and raw JSON hooks. It owns
no thread history, reconstructed activity, retained pending request, account or
configuration cache, model cache, or other Codex domain state.

### Reusable client transport binding

Codex client transport adapters belong to the codex library so CodexUI and
other remote SNode.C applications can reuse them:

```text
ai/openai/codex/frontend/client/
    ClientConnection
    StreamSocketContext
    StreamSocketContextFactory
    WebSocketSubProtocol
    WebSocketUpgrade
```

`ClientConnection` binds one selected transport to one frontend SDK instance:

```text
socket envelope received
    -> frontendSdk.receive(envelope)

frontendSdk sends envelope
    -> ClientConnection sends through the active transport

socket disconnects
    -> frontendSdk.transportDisconnected(reason)
```

This mirrors the MQTTSuite division between a reusable protocol engine and its
raw-stream/WebSocket adapters. The raw `StreamSocketContextFactory` receives a
non-owning `ClientConnection&` as a normal SNode.C factory argument and creates
one context for the accepted physical connection. The context performs bounded
JSONL framing and delegates complete bridge envelopes to `ClientConnection`.

The WebSocket route carries the same logical `ClientConnection` binding through
the HTTP socket context into the selected client subprotocol factory. HTTP
exists only for the upgrade. After upgrade, a Codex WebSocket subprotocol
accepts text messages, enforces frame/message bounds, and delegates exactly one
bridge envelope per completed WebSocket message. Raw stream and WebSocket
transports therefore differ only in framing; they drive the same SDK and
session callbacks.

Codex methods do not live on a generic SNode.C `SocketClient`. Stream and
WebSocket contexts perform framing and lifecycle integration only. WebSocket
client construction carries a shared upgrade binding in the HTTP socket
context until the negotiated subprotocol is created; neither SDK nor mediator
becomes a singleton.

The client supports every frontend transport enabled by `codex-bridge` and the
available SNode.C build:

- Unix JSONL
- IPv4 and IPv6 JSONL
- IPv4 and IPv6 TLS JSONL
- RFCOMM and RFCOMM TLS JSONL
- IPv4 and IPv6 WebSocket
- IPv4 and IPv6 WSS

The bridge protocol has no bearer-token layer. The default Unix connection is
scoped by its private per-user runtime directory; explicitly enabled remote
transports require an appropriate deployment trust policy.

Like `mqttcli`, every compiled transport instance starts disabled. The user
enables exactly one through the ordinary SNode.C instance configuration. This
keeps the complete transport matrix visible and configurable without adding an
application-level duplicate for host, port, Unix path, TLS, Bluetooth, retry,
reconnect, timeout, or writer-queue semantics. Application configuration may
add only genuinely Codex-specific sections, such as the WebSocket request path
or controller preference.

Native `ClientFlowController` policy owns initial retry, retry backoff,
post-disconnect reconnect, and stale-cycle suppression. `ClientSession` does
not implement another reconnect timer. An explicit `reconnect` command first
terminates the selected flow, waits for its asynchronous completion boundary,
and then starts a new connection cycle using the same configured instance.

TLS remains below the Codex protocol context. The same raw Codex context is
used over plain and TLS stream clients, and the same Codex WebSocket subprotocol
is used over WS and WSS. Certificate, SNI, cipher, handshake, and shutdown
configuration remain native SNode.C instance sections.

Outbound delivery is successful only when the SNode.C writer accepts the
bounded frame. The binding records `QueueResult`, queued/sent/outstanding byte
accounting, and the configured writer limit when delivery fails. It must not
report success merely because JSON serialization or WebSocket framing
succeeded.

### CLI behavior

The client provides local commands such as `help`, `quit`, `reconnect`, and
`watch on|off`, plus thread, turn, and read commands for which the current
app-server exposes a direct operation. Commands are translated to typed
frontend SDK calls.

`watch on` presents future app-server notifications. It does not activate or
subscribe to a bridge-owned State publication. `read <thread-id>` invokes the
current app-server `thread/read(includeTurns=true)` operation and presents
exactly the history view returned by app-server. The word `full` in an
app-server `itemsView` is scoped to that provider history mode; it does not mean
that every item previously emitted on the live event stream is reconstructable.

The `snapshot` command means a transient client-side report produced from fresh
app-server queries,
such as `thread/list` followed by selected `thread/read` calls. It must not
create, request, or imply an AISuite-owned snapshot, and the transient report is
discarded after presentation.

Human mode presents concise results and live events. In `--json` mode stdout is
reserved for one JSON object per line; unchanged app-server payloads, bridge
telemetry, and local diagnostics remain explicitly distinguishable. Logs and
human diagnostics go to stderr. The presenter does not construct a retained
domain projection.

### App-server history reconstruction limitation

The no-cache architecture has an unavoidable provider-dependent limitation.
A live test with Codex CLI/app-server `0.144.6` established the following
sequence through independent IPv4 WebSocket app-server, bridge, and client
processes:

1. app-server emitted `item/started` and `item/completed` for a real
   `commandExecution` with a stable item ID, command, working directory,
   aggregated output, completed status, duration, and exit code;
2. codex-bridge forwarded both events unchanged and the frontend client
   observed the complete live item;
3. the isolated rollout JSONL retained the underlying custom tool call and
   custom tool output;
4. an immediate `thread/read(includeTurns=true)` for the same configured-history
   thread returned `itemsView: "full"` but omitted the command execution and
   contained only user and agent messages with reconstructed synthetic item
   IDs;
5. requesting `thread/start` with `historyMode: "paginated"` was rejected by
   the same runtime with JSON-RPC error `-32601` and the message
   `paginated_threads is not supported yet`.

The first loss boundary in this sequence is the app-server `thread/read`
projection. It is not a codex-bridge routing, transport, SDK, or
frontend-protocol loss. The persisted rollout contains richer raw tool data,
but that file is an app-server implementation detail and is not a replacement
for a supported app-server protocol operation.

Consequently, with this provider version a stateless bridge can guarantee
lossless live forwarding but cannot guarantee reconstruction of every prior
live activity for a fresh or reconnected client. Schema presence alone is not
proof that paginated history is available; runtime acceptance is authoritative.
Clients and applications must not interpret `itemsView: "full"` as
proof that omitted live command activity never existed.

This limitation does not authorize an implicit AISuite cache. Caching,
persistence, replay authority, bounds, and eviction remain deferred design
topics. Until either app-server exposes exhaustive history or an explicit cache
contract is adopted, codex-bridge forwards the provider's live events and read
responses without claiming stronger retention semantics.

### Controller and lifecycle behavior

The client observes the bridge's explicit controller policy:

- the controller may steer, mutate Codex state, and answer app-server requests;
- observers may issue only bridge-approved read operations;
- claim, release, and transfer are explicit bridge control operations;
- no client automatically takes control from another active controller;
- no newly created or discovered thread automatically replaces the selected
  thread.

Every SDK operation returns immediately and completes asynchronously through
its typed callback. On transport loss, every outstanding callback completes
exactly once with a transport error, ephemeral request correlation and role
telemetry are cleared, and reconnect establishes a new bridge connection
identity. History recovery always uses fresh app-server queries; the client and
bridge do not recover from cached semantic state. Recovery is therefore limited
to the history representation that the active app-server runtime can actually
reconstruct.

`ClientConnection` is the single physical-attachment mediator. A context calls
it when attached, connected, receiving, failing, and disconnected. It enforces
one active context, routes SDK output only to that context, and detaches exactly
once. Context destruction, WebSocket closure, configured reconnect, explicit
reconnect, and application shutdown all converge through this lifecycle rather
than independently mutating SDK state.

Application shutdown terminates the selected `ClientFlowController` and lets
SNode.C close active contexts. Dynamic replacement of a configured transport,
if introduced later, must use the `mqttbridge` pattern: terminate and await the
old flow before constructing or activating the replacement. No current feature
requires a process-global registry of client sessions.

## Shared Application Contract

The following rules apply to both `codex-bridge` and `codex-bridge-client`. They keep the two applications on one lossless protocol and SDK contract while allowing each application to compose its own SNode.C server or client transports.

### SNode.C composition

Both applications follow the existing SNode.C protocol style used by protocol implementations such as MQTT:

```text
SocketContext/SubProtocol
    -> protocol/framing object
        -> shared intermediary object
```

For Codex this maps to:

```text
AppServerSocketContext / AppServerSubProtocol
    -> JsonRpcFraming
    -> CodexAppServerClient
    -> CodexBridge

FrontendSocketContext / FrontendWebSocketSubProtocol
    -> BridgeJsonFraming
    -> FrontendConnection
    -> CodexBridge

ClientStreamSocketContext / ClientWebSocketSubProtocol
    -> BridgeJsonFraming
    -> ClientConnection
    -> frontend::CodexBridge
```

On the server, `bridge::CodexBridge` is the shared intermediary. It registers
frontend clients, tracks controller/observer roles, routes client messages to
app-server, fans out app-server messages, and emits bridge-level telemetry. On
the client, `frontend::CodexBridge` is the application-owned proxy SDK and
`ClientConnection` is its single selected transport mediator.

### Bridge envelope protocol

Use JSON-RPC as the real app-server protocol. The bridge protocol is only a thin JSON wrapper around native app-server JSON-RPC messages.

```json
{
  "kind": "appserver",
  "connectionId": "frontend-3",
  "role": "controller",
  "seq": 42,
  "payload": {
    "jsonrpc": "2.0",
    "id": 17,
    "method": "thread/list",
    "params": {}
  }
}
```

`payload` is the native app-server JSON-RPC message. It must be preserved as-is and exposed directly as `nlohmann::json`.

The app-server wire format is JSON-RPC-shaped but does not consistently emit
the optional `"jsonrpc": "2.0"` member. Classification and generated typed
views therefore recognize requests/notifications by `method` plus optional
`params`, and responses by non-null `id` plus exactly one of `result` or
`error`. Typed accessors unwrap `params` or successful `result` for either
wire shape while `getRaw()` continues to return the complete untouched native
message. Presence or absence of the optional version member must never change
routing, callback correlation, or typed field access.

Bridge-owned messages are separate:

```json
{
  "kind": "bridge.connection",
  "event": "opened",
  "connectionId": "frontend-3"
}
```

```json
{
  "kind": "bridge.controller",
  "controllerConnectionId": "frontend-3"
}
```

```json
{
  "kind": "bridge.provider",
  "state": "ready",
  "providerGeneration": 1,
  "seq": 43
}
```

`bridge.provider` reports `connected`, `ready`, and `disconnected` with a
provider generation that is independent of the frontend connection identity.
Frontend requests are admitted only in `ready`; generation loss retires their
outstanding callbacks exactly once.

Unknown app-server methods, fields, events, and result members must pass through without loss.

### Shared non-goals

`codex` must not reintroduce Codex semantic authority:

- no semantic cache
- no frontend `State`
- no AISuite snapshot authority
- no timeline reducer
- no command-history reconstruction
- no pending-request truth
- no merge/replace semantics for thread items
- no interpretation of omitted app-server fields

Recovery is performed by querying app-server again, for example through
`thread/read`, not from AISuite-retained semantic state. Such a query is not
assumed to be exhaustive beyond the active app-server history mode and runtime
capabilities.

### Controller model

Only one frontend client is authoritative at a time.

- the controller may submit turns, steer, and respond to app-server server-requests
- observers receive events and may optionally perform allowed read-only requests
- controller handoff is explicit
- selected-thread auto-switching is not a bridge policy
- mutating observer requests are rejected or ignored with diagnostics

### Transport model

Frontend transports are SNode.C transport concerns and may include:

- Unix sockets
- IPv4
- IPv6
- TLS
- WebSocket
- WSS when supported by the SNode.C layer
- RFCOMM
- RFCOMM TLS

Provider-side app-server connectivity uses what the app-server supports:

- `stdio://`
- `unix://`
- `unix://PATH`
- `ws://IP:PORT`

Stdio is JSONL over one ordered stream. Current app-server Unix and IP modes are
WebSocket transports: `unix://` performs a WebSocket handshake over the Unix
stream, while `ws://IP:PORT` performs the handshake over TCP. They are not
JSONL socket modes. App-server Unix/WebSocket listeners can accept multiple
clients, but `codex-bridge` uses exactly one provider connection and owns the
intended frontend multi-client controller/observer policy.

### Shared SDK / facade contract

Generic SNode.C socket classes must not be polluted with Codex-specific
methods. The application-owned `CodexBridge` owns the generated backend SDK and
is the Codex-specific object used by arbitrary AI-enabled SNode.C applications.
The active provider endpoint supplies its transport to that bridge.

```cpp
CodexBridge bridge;
StdioAppServer provider(bridge, options);
```

Typed facade methods are mandatory:

```cpp
bridge.threadList([](ThreadList& result) {
    if (!result) {
        return;
    }

    for (const ThreadSummary& thread : result.threads()) {
        // typed access
    }

    const nlohmann::json& raw = result.getRaw();
});
```

Raw JSON access is mandatory too:

```cpp
bridge.sendRawJson(json);
bridge.onRawJson(...);
```

Frontend listeners are optional users of the same `CodexBridge`. A SNode.C
application that needs only direct typed/raw Codex access uses the bridge SDK
without configuring any frontend listener.

Remote clients use a required frontend proxy SDK. It exposes the same generated
datatype names, request/notification methods, callback signatures, and
server-request response operations as the `CodexBridge` backend SDK. Its
implementation only wraps outgoing native JSON-RPC in bridge envelopes and
unwraps incoming envelopes before invoking the same callbacks.

The proxy may additionally expose bridge connection, role, controller handoff,
and transport telemetry. It contains no reducer, snapshots, reconciliation
rules, retained Codex objects, or additional authority model. Backend SDK and
frontend proxy SDK differ in transport, not in the Codex API presented to their
users.

Both SDKs provide typed responses to app-server-initiated requests in addition
to typed handlers. A response takes the stable JSON-RPC ID from the typed
request object and serializes the typed response payload without introducing a
bridge-owned request identity. Explicit typed error responses are available as
well.

Transport disconnection completes every outstanding callback exactly once with
a synthetic transport error and clears only ephemeral request correlation and
connection-role telemetry. It does not retain or reconstruct any Codex domain
state. Requests submitted through an unconnected frontend proxy fail locally
instead of creating IDs that could collide after connection establishment.

Every typed result/event object wraps native app-server JSON, exposes `getRaw()`, provides typed accessors, preserves unknown fields, has `operator bool()` for success/error state, and does not cache or reconstruct broader Codex state.

Typed protocol coverage is complete, not selective. Every JSON-RPC message
defined by the Codex app-server protocol must have a concrete C++ datatype,
including:

- client requests and their parameter and response types
- client notifications
- app-server requests and their parameter and response types
- app-server notifications
- every nested object, enum, union, collection, identifier, and error type
  referenced by those messages

The complete set is generated deterministically from the app-server's exported
protocol schema. Generated C++ objects retain the complete native JSON value,
preserve unknown fields, expose `getRaw()`, and provide typed accessors. The raw
send/receive path remains mandatory so a newer app-server message can still pass
through an older bridge build without loss.

The app-server repository is a read-only schema source. AISuite must not modify
or patch the OpenAI app-server to produce these datatypes.

### Configuration

Both applications use the existing SNode.C configuration subsystem whenever command-line options are required.

Each SNode.C server/client instance already has a broad set of command-line options. Neither application duplicates existing transport semantics in application configuration.

Valid bridge-specific configuration includes:

- app-server executable path
- app-server listen mode / provider transport
- child-specific `CODEX_HOME`
- controller policy
- bridge protocol limits
- telemetry/log verbosity
- optional app-server restart policy

Valid client-specific configuration is limited to behavior not already owned
by its selected SNode.C instance, such as:

- JSON versus human presentation
- WebSocket request path
- bridge protocol frame limit
- optional initial controller claim policy

Do not duplicate existing SNode.C instance semantics such as Unix path binding, IPv4/IPv6 binding, TLS certificate/key handling, RFCOMM adapter/channel handling, or WebSocket transport configuration.

Codex connections are legitimately quiet while users inspect work or while a
turn waits elsewhere in the system. Production provider and frontend stream
instances therefore default their SNode.C connection read and write inactivity
timeouts to zero (unlimited). These are instance defaults configured through
the existing `Connection` config section, not new application options. SNode.C
accepts zero for both timeout options and converts explicitly between numeric
CLI seconds and `utils::Timeval`; finite values remain available as normal
per-instance CLI/config overrides. Maximum frames, queue bounds, connect
timeouts, and explicit lifecycle control remain active.

## Canonical Implementation Scope

The canonical source tree contains the implementation under
`src/ai/openai/codex`, the `codex-bridge` and `codex-bridge-client`
applications, this architecture contract, and the focused tests described
below. It does not modify the OpenAI app-server.

## Implemented System Report

The initial codex implementation is complete for the stateless bridge scope
defined by this document. The implementation consists of one reusable library,
two applications, generated complete protocol facades for the imported
app-server schema, production transport adapters, focused tests, installation
metadata, and CI integration. It does not include a reducer, snapshot store,
history cache, persistence layer, or implicit controller switching.

### Build products and source layout

The reusable static library is built as `ai-openai-codex`, exported and
installed as `AISuite::OpenAICodex`. It contains:

- the backend SDK and multi-client router in `bridge/`;
- the frontend proxy SDK and server/client transport adapters in `frontend/`;
- bridge envelopes, JSON-RPC classification, JSONL framing, and generated
  app-server datatypes in `protocol/`;
- owned-stdio and external-WebSocket provider endpoints in `provider/`.

The two installed executables are:

- `codex-bridge`, which composes one backend `CodexBridge`, one provider
  endpoint, and all configured frontend listeners;
- `codex-bridge-client`, which composes the frontend proxy SDK, one selected
  SNode.C client transport, asynchronous command handling, nonblocking stdin,
  and human or JSONL presentation.

The generated protocol header is produced by
`tools/generate-codex-protocol.mjs`. Its adjacent manifest records the exact
schema inputs and generated surface. The currently imported schema generates
1,920 datatypes, including 81 canonical root types, 585 canonical v2 types, 95
client requests, 10 app-server requests, one client notification, and 76
app-server notifications. This is complete coverage of the imported schema,
not a hand-selected method subset. Each facade retains native JSON through
`getRaw()`, and the bridge also preserves a raw JSON path for forward
compatibility.

### Important public APIs

The principal local-application API is
`ai::openai::codex::bridge::CodexBridge`. Its important hand-written entry
points are:

```cpp
CodexBridge(CodexBridgeOptions options = {});

void setAppServer(AppServerEndpoint* endpoint);
void appServerConnected();
void setAppServerReady();
void appServerDisconnected(std::string_view reason);
void onProviderLifecycle(ProviderLifecycleHandler handler);

bool sendRawJson(const nlohmann::json& message);
void onRawJson(RawHandler handler);

std::string registerFrontend(FrontendEndpoint& endpoint);
void unregisterFrontend(std::string_view connectionId);
void receiveFromFrontend(std::string_view connectionId,
                         const nlohmann::json& envelope);
void receiveFromAppServer(const nlohmann::json& message);
```

Local applications normally use the generated convenience methods rather than
the generic plumbing:

```cpp
using namespace ai::openai::codex;

generated::v2::ThreadListParams params({{"limit", 25}});
bridge.threadList(params, [](generated::client_requests::ThreadList::Response& result) {
    if (!result) {
        return;
    }

    const auto threads = result.data().items();
    const nlohmann::json& nativeResponse = result.getRaw();
});

bridge.onTurnPlanUpdated(
    [](generated::server_notifications::TurnPlanUpdated::Params& event) {
        const auto threadId = event.threadId();
        const auto steps = event.plan().items();
    });
```

The generated backend API provides one named method for every imported client
request, including `threadStart`, `threadResume`, `threadRead`, `threadList`,
`turnStart`, `turnSteer`, `turnInterrupt`, `modelList`, `configRead`, account,
MCP, plugin, app, skill, filesystem, command-exec, and all other schema-defined
operations. Parameterless requests have direct overloads. `initialized()` is
the generated client-notification method.

Every app-server request has a generated `on...` handler and matching
`respondTo...` operation, plus the generic `respond()` and `respondError()`
forms. This includes command/file-change approvals, user input, MCP elicitation,
permission approval, dynamic tool calls, authentication token refresh,
attestation, apply-patch approval, and exec-command approval. Every app-server
notification has a generated `on...` handler, including thread, turn, item,
plan, command-output, process, agent, account, configuration, and lifecycle
events.

The remote-client API is
`ai::openai::codex::frontend::CodexBridge`. It intentionally mirrors all of
the generated request, notification, event-handler, response, typed-error, and
raw JSON methods above. Its additional bridge-hop API is:

```cpp
CodexBridge(Sender sender);
void setSender(Sender sender);
bool receive(const nlohmann::json& bridgeEnvelope);
void transportDisconnected(std::string_view reason);

void onBridgeEvent(BridgeEventHandler handler);
bool claimController();
bool releaseController();
bool transferController(std::string targetConnectionId);

const std::optional<std::string>& connectionId() const;
const std::optional<std::string>& controllerConnectionId() const;
std::optional<protocol::Role> role() const;
bool isController() const;
```

The small transport contracts are `bridge::AppServerEndpoint`,
`bridge::FrontendEndpoint`, and `frontend::client::TransportEndpoint`. Each
contains only `send`, connection-state where relevant, and `close` where the
owning side may terminate a connection. `frontend::client::ClientConnection`
provides `attach`, `connected`, `receive`, `failed`, `detach`, `disconnect`, and
`shutdown` to mediate exactly one physical client transport.

`protocol::JsonLineFramer` provides bounded `consume`, `encode`, and `reset`.
It separates JSON parsing from application dispatch, so a consumer exception
does not poison later valid frames. A framing failure is terminal for the
owning transport.
`protocol::classifyJsonRpc`, `jsonRpcIdKey`, and `jsonRpcMethod` classify native
messages without requiring the optional `jsonrpc` member. The production
`StreamSocketContextFactory` classes and WebSocket factories/subprotocols are
the intended SNode.C composition APIs; generic `SocketClient`, `SocketServer`,
and `SocketContext` classes receive no Codex-specific methods.

Generated datatype behavior is uniform:

- constructors accept native `nlohmann::json` payloads;
- `operator bool()` distinguishes successful responses from JSON-RPC errors;
- `getRaw()` returns the complete original JSON value;
- `getPayload()` returns the operation payload used for serialization;
- `jsonRpcId()` and JSON-RPC error accessors are available where applicable;
- object, array, scalar, enum, optional, and union accessors expose concrete C++
  types while preserving unknown JSON fields.

### Backend SDK and routing implementation

`bridge::CodexBridge` is both the backend SDK presented to local SNode.C
applications and the router used by `codex-bridge`. It implements:

- generated asynchronous request methods and typed response callbacks;
- generated notification methods, app-server request handlers, app-server
  notification handlers, typed responses, and typed errors;
- raw bidirectional JSON observation and raw request/notification forwarding;
- exactly one registered app-server endpoint;
- frontend registration with stable bridge connection identities;
- explicit controller, observer, claim, release, and transfer behavior;
- optional observer access to a deliberately classified read-only method set;
- frontend request-ID translation and exact response-ID restoration;
- provider notification fanout to every connected frontend;
- app-server request routing to a local handler or the current controller;
- ownership validation for app-server request responses;
- bounded bridge sequence telemetry and structured diagnostics;
- exactly-once cleanup of ephemeral callbacks and ownership records on
  resolution or disconnect.

The only retained bridge records are connection roles and outstanding routing
correlations. No thread, turn, item, command output, account, model, plan,
agent, approval, or pending-request domain object is cached.

Bridge messages are JSON objects. Native app-server JSON-RPC remains unchanged
under `kind: "appserver"` in `payload`. Bridge-owned metadata is separated into
`bridge.connection`, `bridge.controller`, and `bridge.diagnostic` messages.
Connection ID, role, and process-local delivery sequence are metadata about the
bridge hop; they are not injected into the native app-server payload.

### Provider implementation

`StdioAppServer` owns a `codex app-server` child only in stdio mode. It uses
SNode.C pipe sources/sinks, bounded JSONL framing and write admission, separate
stderr handling, process-group shutdown, and a Linux pidfd event receiver for
exact child-exit observation. There is no periodic `waitpid(WNOHANG)` polling
timer. Shutdown waits once for a bounded grace period outside event-loop
callbacks and escalates only when required.

`WebSocketAppServer` connects to an independently managed app-server through
the app-server's native WebSocket listener. The application composes Unix,
IPv4, or IPv6 SNode.C HTTP clients according to `--app-server-transport` and
performs the required HTTP upgrade. Network provider modes never spawn or own
another app-server. Provider ownership is therefore implicit and unambiguous:
stdio owns a child; every non-stdio mode connects externally.

`ProviderApplication` performs one `initialize`/`initialized` handshake for
each provider generation and marks the shared backend SDK ready only after the
handshake succeeds. Provider loss fails outstanding callbacks and transient
routing ownership exactly once without fabricating retained Codex state.

### Frontend server and proxy implementation

Raw frontend listeners use the production `StreamSocketContextFactory` and
`StreamSocketContext`. They enforce bounded JSONL framing, register and
unregister one frontend endpoint per physical connection, and forward complete
bridge envelopes without semantic reduction.

WebSocket listeners use the production HTTP upgrade application,
`WebSocketUpgrade`, and `WebSocketSubProtocol`. The scoped upgrade binding
passes the application-owned bridge reference into the newly created
subprotocol without a singleton. HTTP parser, WebSocket frame/message, and
fragment limits are configured before listening.

The bridge application composes the following frontend server instances when
their SNode.C components are available:

- Unix, IPv4, and IPv6 JSONL;
- IPv4 and IPv6 TLS JSONL;
- RFCOMM and RFCOMM TLS JSONL;
- IPv4 and IPv6 WebSocket;
- IPv4 and IPv6 WSS.

Only the Unix JSONL listener is enabled by default. Other native SNode.C
instances remain visible in configuration and start disabled. Connection
inactivity timeouts default to unlimited, while frame and writer-queue limits
remain bounded.

The reusable frontend `CodexBridge` proxy exposes the same generated method and
datatype surface as the backend SDK. It wraps outbound native JSON-RPC in a
bridge envelope, unwraps inbound payloads, dispatches typed callbacks and
events, tracks only local outstanding callbacks and current role telemetry,
and fails pending callbacks exactly once on transport loss. `ClientConnection`
ensures that only one physical stream or WebSocket context is attached to a
proxy SDK instance at a time.

### Transport, encryption, and application completeness matrix

The two network boundaries have different capabilities and must not be merged
into one transport claim. The provider boundary is constrained by transports
implemented by Codex app-server. The frontend boundary is controlled by
`codex-bridge` and can use the broader SNode.C transport surface.

In these tables, **complete** means production composition exists in both the
relevant application and reusable codex library. **Conditional** means the
production implementation is complete but is compiled only when its SNode.C
component is available and the matching AISuite option is enabled.

#### App-server to `codex-bridge` provider boundary

| Provider transport | App-server listener | Encoding | Encryption | App-server ownership | `codex-bridge` implementation | Verification |
|---|---|---|---|---|---|---|
| stdio pipes | `stdio://` | JSONL | not applicable | bridge-owned child | Complete, always built | Deterministic provider acceptance, real app-server fixture, and real end-to-end matrix |
| Unix stream | `unix://PATH` | HTTP Upgrade + WebSocket JSON | none | external process | Conditional on WebSocket components | Deterministic provider acceptance |
| IPv4 TCP | `ws://127.0.0.1:PORT` | HTTP Upgrade + WebSocket JSON | none | external process | Conditional on WebSocket components | Deterministic provider acceptance and authenticated live run |
| IPv6 TCP | `ws://[::1]:PORT` | HTTP Upgrade + WebSocket JSON | none | external process | Conditional on WebSocket components | Deterministic provider acceptance |

Only the stdio row launches and supervises a process. Selecting Unix, IPv4, or
IPv6 provider transport implicitly selects an external app-server; there is no
second ownership switch.

#### `codex-bridge` to frontend applications

| Frontend transport | Encoding | Encryption | `codex-bridge` server | `codex-bridge-client` | Reusable proxy/adapter | Automated verification |
|---|---|---|---|---|---|---|
| Unix domain stream | JSONL | none | Complete, enabled by default | Complete | Complete | Deterministic and real app-server end to end |
| IPv4 TCP | JSONL | none | Complete, disabled by default | Complete | Complete | Deterministic and real app-server end to end |
| IPv6 TCP | JSONL | none | Complete, disabled by default | Complete | Complete | Deterministic and real app-server end to end |
| IPv4 TCP TLS | JSONL | TLS | Conditional | Conditional | Complete over native SNode.C TLS stream | Deterministic and real app-server end to end |
| IPv6 TCP TLS | JSONL | TLS | Conditional | Conditional | Complete over native SNode.C TLS stream | Deterministic and real app-server end to end |
| IPv4 WebSocket | WebSocket JSON | none | Conditional | Conditional | Complete | Deterministic and real app-server end to end |
| IPv6 WebSocket | WebSocket JSON | none | Conditional | Conditional | Complete | Deterministic and real app-server end to end |
| IPv4 WSS | WebSocket JSON | TLS | Conditional | Conditional | Complete over native SNode.C TLS/HTTP/WebSocket stack | Deterministic and real app-server end to end |
| IPv6 WSS | WebSocket JSON | TLS | Conditional | Conditional | Complete over native SNode.C TLS/HTTP/WebSocket stack | Deterministic and real app-server end to end |
| RFCOMM | JSONL | none | Conditional | Conditional | Complete over native SNode.C RFCOMM stream | Not automated; requires suitable Bluetooth runtime/hardware |
| RFCOMM TLS | JSONL | TLS | Conditional | Conditional | Complete over native SNode.C RFCOMM TLS stream | Not automated; requires suitable Bluetooth runtime/hardware |

The frontend matrix carries the same bridge envelope, controller/observer
policy, generated API, raw payload, and asynchronous callback behavior over
every row. TLS changes only the SNode.C transport below the Codex context.
WebSocket changes only framing and HTTP upgrade. Neither adds authentication,
state retention, or another protocol authority.

#### Application completeness

| Application/library surface | Provider access | Frontend server access | Frontend client access | Typed app-server API | Raw app-server JSON | Domain cache |
|---|---|---|---|---|---|---|
| `AISuite::OpenAICodex` backend SDK/router | Through `AppServerEndpoint` | Through `FrontendEndpoint` adapters | not applicable | Complete imported schema | Complete | None |
| `codex-bridge` | Exactly one stdio or external WebSocket provider | All enabled rows above, concurrently | not applicable | Complete through backend SDK | Complete, fanout/routing unchanged | None |
| `AISuite::OpenAICodex` frontend proxy SDK | remote through bridge | not applicable | Through `ClientConnection` adapters | Complete imported schema | Complete | None |
| `codex-bridge-client` | remote through bridge | not applicable | Exactly one enabled row above | Complete SDK available; interactive grammar exposes common operations plus `raw` | Complete through `raw` and watch output | None |

#### Test implementation completeness

The test matrix mirrors the application graph rather than introducing a
test-only protocol stack. **Deterministic** cases use a bounded protocol peer
at the far side of the boundary under test. **Real** cases use an actual
`codex app-server` process while retaining production bridge and frontend proxy
components.

| Boundary/application under test | Transport | Deterministic CTest | Real app-server CTest | Current status |
|---|---|---|---|---|
| Protocol utility | JSONL framing | `CodexJsonLineFramerTest` | not applicable | Complete and passing |
| Frontend proxy SDK + `ClientConnection` | in-memory endpoint boundary | `CodexFrontendSdkTest` | covered through every real frontend transport below | Complete and passing |
| Backend SDK/router | in-memory provider/frontend endpoints | `CodexBridgeRoutingTest` | covered through every real frontend transport below | Complete and passing |
| `codex-bridge` provider | owned stdio JSONL | `CodexProvider_stdio` | exercised by all nine `CodexRealAppServer_*` cases | Complete and passing |
| `codex-bridge` provider | Unix WebSocket | `CodexProvider_websocket_unix` | no separate automated external-process case | Production path complete; deterministic case passing |
| `codex-bridge` provider | IPv4 WebSocket | `CodexProvider_websocket_ipv4` | authenticated external app-server run performed manually | Production path complete; deterministic case passing and live path observed |
| `codex-bridge` provider | IPv6 WebSocket | `CodexProvider_websocket_ipv6` | no separate automated external-process case | Production path complete; deterministic case passing |
| Bridge server + proxy client | Unix JSONL | `CodexStream_unix` | `CodexRealAppServer_Stream_unix` | Complete and passing |
| Bridge server + proxy client | IPv4 JSONL | `CodexStream_ipv4` | `CodexRealAppServer_Stream_ipv4` | Complete and passing |
| Bridge server + proxy client | IPv6 JSONL | `CodexStream_ipv6` | `CodexRealAppServer_Stream_ipv6` | Complete and passing |
| Bridge server + proxy client | IPv4 TLS JSONL | `CodexStream_tls_ipv4` | `CodexRealAppServer_Stream_tls_ipv4` | Complete and passing when TLS is enabled |
| Bridge server + proxy client | IPv6 TLS JSONL | `CodexStream_tls_ipv6` | `CodexRealAppServer_Stream_tls_ipv6` | Complete and passing when TLS is enabled |
| Bridge server + proxy client | IPv4 WebSocket | `CodexWebSocket_websocket_ipv4` | `CodexRealAppServer_WebSocket_websocket_ipv4` | Complete and passing when WebSocket is enabled |
| Bridge server + proxy client | IPv6 WebSocket | `CodexWebSocket_websocket_ipv6` | `CodexRealAppServer_WebSocket_websocket_ipv6` | Complete and passing when WebSocket is enabled |
| Bridge server + proxy client | IPv4 WSS | `CodexWebSocket_wss_ipv4` | `CodexRealAppServer_WebSocket_wss_ipv4` | Complete and passing when TLS/WebSocket are enabled |
| Bridge server + proxy client | IPv6 WSS | `CodexWebSocket_wss_ipv6` | `CodexRealAppServer_WebSocket_wss_ipv6` | Complete and passing when TLS/WebSocket are enabled |
| Bridge server + proxy client | RFCOMM JSONL | none | none | Production composition present; deliberately untested without Bluetooth runtime/hardware |
| Bridge server + proxy client | RFCOMM TLS JSONL | none | none | Production composition present; deliberately untested without Bluetooth runtime/hardware |
| TLS/WSS setup | generated loopback certificates | `CodexGenerateTlsCertificates` | shared by encrypted real cases | Complete and passing |

The three external app-server WebSocket provider modes are tested directly
against deterministic native WebSocket peers. They are not duplicated as nine
more real-process frontend cases because the real end-to-end matrix already
tests the full app-server handshake, backend router, each frontend server,
frontend transport adapter, proxy SDK, and typed callback path with the
bridge-owned stdio provider. The independent IPv4 provider mode additionally
completed an authenticated live app-server/bridge/client session. A future
fully automated external-provider process fixture may add Unix and IPv6 live
variants, but its absence is not hidden by the current 26-test count.

The client CLI does not need a handwritten command for every generated method
to be protocol-complete: arbitrary schema-defined JSON-RPC remains reachable
through `raw`, while C++ applications use the complete generated proxy API.
CodexUI uses that proxy API and one selected frontend transport. Other visual
clients use the same boundary; they are client applications, not additional
bridge authorities.

### Application and configuration implementation

Both applications register a `utils::SubCommand` before
`core::SNodeC::init()`. Their application options cover only Codex-specific
semantics. Socket addresses, ports, Unix paths, TLS material, RFCOMM settings,
retry, reconnect, timeouts, and queue behavior remain native SNode.C instance
configuration.

`codex-bridge-client` implements the interactive `help`, `quit`, `reconnect`,
`watch`, `snapshot`, `replay`, `acquire`, `release`, `threads`, `start`,
`resume`, `new`, `read`, `turn`, `interrupt`, and `raw` command grammar.
Commands that map to app-server operations use generated typed SDK calls and
asynchronous callbacks. `raw` provides direct native JSON-RPC access. `watch`
controls notification presentation only. `snapshot` performs a transient fresh
query and `replay` does not claim a bridge-owned retained event log.

Standard input is nonblocking and integrated with the SNode.C event loop.
Explicit reconnect terminates the selected native client flow and waits for
its asynchronous termination before starting the next connection cycle. JSON
mode emits exactly one compact object per line to stdout and explicitly flushes
each line; diagnostics remain on stderr. This permits interactive use through
pipes without requiring `stdbuf` or waiting for process exit.

### Build, install, and CI implementation

`AISUITE_BUILD_CODEX_TESTS` registers only the focused `tests/codex` suite.
There is no compatibility build switch or second Codex implementation in the
canonical source tree.

The library headers, `AISuite::OpenAICodex` target, `codex-bridge`, and
`codex-bridge-client` are installable. The package configuration resolves the
SNode.C components required by the exported static library. CI checks out and
builds SNode.C `master`/HEAD, builds with parallelism eight, prepares the shared
SNode.C runtime directories before parallel process tests, runs the focused
tests, and verifies installation.

### Completion audit result

An architecture-wide completion audit found no remaining blocker against the
agreed stateless bridge contract. It identified and corrected two cleanup
defects:

1. JSONL client output was written with a newline but not explicitly flushed;
2. obsolete network child-supervisor code remained after app-server ownership
   had been restricted to stdio mode.

The corrected client passes a no-`stdbuf` pipe probe, and the obsolete process
supervision files and unused network ownership hooks have been removed. Source
format validation with `git diff --check` is clean. The app-server history
reconstruction limitation documented above remains an explicit
provider limitation, not an incomplete bridge implementation.

## Focused Test Architecture

The codex test suite validates the narrow bridge architecture at its actual
boundaries. It does not introduce snapshots, reducers, retained domain state,
or an additional authentication protocol.

### Build and registration boundary

Only `tests/codex` is registered when `AISUITE_BUILD_CODEX_TESTS` is enabled.
Every codex test target is named explicitly; there is no recursive discovery
that could silently add unrelated or broad tests.

The focused suite is divided by responsibility:

1. Codex app-server to `codex-bridge` communication tests;
2. `codex-bridge` internal contract tests;
3. `codex-bridge` to frontend-client communication tests.

Each test is bounded by a short CTest timeout. A transport test runs as its own
process so SNode.C initialization, configuration, event-loop state, listeners,
connectors, TLS state, and shutdown cannot leak into another transport case.

### Observable communication trace

Tests expose their communication instead of reporting only pass/fail assertions.
Every process emits one compact JSON object per trace line to stdout. Running

```text
ctest --test-dir "${BUILD_DIR}" -V -L codex
```

shows the full focused trace.

Every communication record contains:

- `trace: "codex.communication"` as a stable discriminator;
- a monotonic process-local `sequence`;
- the test or transport name;
- the observation `boundary`;
- a `direction` such as `client-to-bridge`, `bridge-to-provider`, or
  `provider-to-bridge`;
- an event name;
- bounded structured details or message JSON.

The trace records lifecycle and data movement separately. Observable lifecycle
includes listener results, connector results, process creation/exit, HTTP
upgrade, WebSocket activation, controller telemetry, disconnect, and orderly
shutdown. Observable data movement includes JSONL fragments, decoded messages,
bridge envelopes, native app-server JSON-RPC, request-ID remapping, provider
responses, provider notifications, and typed callback delivery.

Trace output is diagnostic evidence, not another protocol or state store. It is
never read back by production code and never determines test behavior. Strings,
objects, arrays, and nesting are bounded before rendering. A truncated value
retains its original byte/item count and a prefix so IPv6 large-payload coverage
remains visible without printing tens of kilobytes. Synthetic tests contain no
credentials or user data, but the bounded trace discipline still matches the
runtime diagnostic requirements.

### App-server to `codex-bridge` communication tests

These tests exercise the production provider endpoints used by
`codex-bridge`. A deterministic app-server protocol peer occupies the remote
boundary. It speaks native app-server JSON-RPC and does not use bridge envelopes,
cache Codex state, or emulate model behavior.

The verified current app-server listener surface is derived from the read-only
app-server source and CLI:

| App-server listener | Bridge provider endpoint | Address family | Wire format | TLS |
|---|---|---|---|---|
| `stdio://` | `StdioAppServer` | child stdio pipes | JSONL | not applicable |
| `unix://PATH` | `WebSocketAppServer` | Unix domain socket | HTTP Upgrade + WebSocket text JSON | unsupported by app-server |
| `ws://127.0.0.1:PORT` | `WebSocketAppServer` | IPv4 | HTTP Upgrade + WebSocket text JSON | unsupported by app-server |
| `ws://[::1]:PORT` | `WebSocketAppServer` | IPv6 | HTTP Upgrade + WebSocket text JSON | unsupported by app-server |

`off` is not a communication transport and therefore has no message-flow test.
The app-server listener does not support `wss://`; provider-side TLS must not be
invented in AISuite or falsely claimed as app-server compatibility. TLS and WSS
are nevertheless tested comprehensively on the independent frontend-listener
boundary described below.

The stdio test uses the production child-process, pipe, JSONL framer, queue, and
shutdown implementation. Its deterministic helper executable is shaped like a
minimal app-server process: it reads native JSON-RPC from stdin, writes native
JSON-RPC to stdout, flushes each JSONL record, and writes its own diagnostic
trace to stderr so protocol stdout remains uncontaminated.

The Unix, IPv4, and IPv6 provider tests use the production SNode.C HTTP client,
`WebSocketAppServer`, and app-server WebSocket upgrade path against a local
deterministic WebSocket server. The Unix case performs the standard HTTP Upgrade
over a Unix stream; it is not treated as raw Unix JSONL.

The real app-server upgrade does not negotiate a `Sec-WebSocket-Protocol`
value. The deterministic SNode.C peer therefore uses a test-local HTTP upgrade
factory that performs the RFC 6455 handshake and creates its WebSocket protocol
context without requiring a named subprotocol. This keeps the peer faithful to
the app-server handshake while leaving the production provider endpoint
unchanged.

Provider scenarios cover, in priority order:

1. connect and disconnect lifecycle publication;
2. a native generated typed request from the backend SDK;
3. exact JSON-RPC request delivery to the protocol peer;
4. response correlation back to the asynchronous typed callback;
5. an unsolicited provider notification delivered to a typed handler;
6. raw bidirectional observation at the backend SDK boundary;
7. orderly transport and owned-child teardown.

IPv6 tests are separate CTest cases and may return the skip code only after an
explicit `::1` socket/bind probe demonstrates that IPv6 loopback is unavailable.
Protocol, HTTP-upgrade, callback, or implementation failures are test failures.

### `codex-bridge` internal tests

Internal tests isolate deterministic framing, SDK correspondence, and routing
policy without introducing a network variable.

The JSONL framing test covers:

- fragmented input accumulated across reads;
- multiple coalesced messages in one read;
- LF and CRLF delimiters;
- empty-line handling;
- invalid JSON as a terminal framing error;
- inbound frame-size enforcement before and at a delimiter;
- outbound frame-size enforcement;
- explicit reset between independent transport lifecycles.

The frontend proxy SDK test covers:

- production `frontend::CodexBridge` plus `client::ClientConnection` binding;
- bridge connection and controller telemetry;
- generated typed request construction;
- raw JSON equality with the typed request and response;
- stable JSON-RPC callback correlation;
- `getRaw()` preservation on typed result objects;
- typed `id/result` and `method/params` access for real app-server messages that
  omit the optional `jsonrpc` member;
- bidirectional raw observation;
- one-shot completion of pending callbacks on transport loss;
- exactly-once connected, disconnected, and failure callback behavior.

The bridge routing test covers:

- first-client controller and later-client observer roles;
- allowed observer reads and rejected observer mutations;
- frontend request-ID remapping and exact restoration;
- provider notification fanout;
- provider server-request delivery only to the controller;
- rejection of responses from a non-owning frontend;
- provider-handshake ownership and frontend `initialize` rejection;
- explicit controller release and claim;
- absence of controller auto-promotion after disconnect;
- deterministic server-request failure when its controller disconnects;
- exactly-once controller-loss cleanup and stale-response rejection;
- ownership retirement on `serverRequest/resolved` and clean request-ID reuse;
- cleanup of transient request ownership and frontend registration.

Mocks in these tests end at the precise boundary under test. They store messages
but do not model Codex state, infer authority, reconstruct history, or replace
the generated datatypes. Their sends, receives, rejection paths, and lifecycle
changes are visible through the same structured communication trace.

### `codex-bridge` to client communication tests

Every real transport test client has the same principal runtime shape as
`codex-bridge-client`:

```text
native SNode.C SocketClient or HTTP/WebSocket client
    -> codex production transport binding
    -> frontend::client::ClientConnection
    -> frontend::CodexBridge proxy SDK
    -> generated typed method and asynchronous typed callback
```

The transport test must not introduce a test-only socket protocol or call the
server-side `CodexBridge` directly from its scenario logic. Its only
test-specific component is an automatic scenario driver. That driver submits
ordinary generated SDK operations after the production connection callback,
waits for their normal asynchronous result/event callbacks, validates the
result, and advances to the next bounded step.

Every enabled frontend transport also has a real app-server end-to-end variant.
That variant replaces only the deterministic provider peer with the production
`StdioAppServer` endpoint and an actual `codex app-server` child. It performs the
real `initialize`/`initialized` handshake, then sends `thread/start` through the
production frontend proxy SDK. It verifies the typed `thread/start` response,
the corresponding typed `thread/started` notification, stable thread identity,
and the raw native messages after they have crossed the complete chain:

```text
real Codex app-server
    -> production stdio provider
    -> backend CodexBridge
    -> selected production frontend transport
    -> ClientConnection
    -> frontend CodexBridge typed callback and event handler
```

The real cases use a fresh temporary `CODEX_HOME` per test and remove it after
the owned app-server has stopped. They create a thread but do not start a model
turn, so transport/schema acceptance does not consume workload-model credits.
The discovered `codex` executable is recorded in the CTest command. If no
executable is available at CMake configuration time, these real-process cases
are not registered; that absence is visible in the test inventory.

The test bridge server likewise uses the production shape:

```text
native SNode.C SocketServer or Express WebApp
    -> codex production stream SocketContext or WebSocket SubProtocol
    -> backend CodexBridge
    -> deterministic mock AppServerEndpoint
```

The mock provider is intentionally stateless. It answers a small set of
JSON-RPC requests and emits selected notifications or server-requests through
`CodexBridge::receiveFromAppServer()`. It neither caches Codex objects nor
substitutes for app-server persistence semantics.

#### Frontend transport matrix

The real loopback matrix covers all requested frontend network forms:

| Transport | Address family | Encoding | Encryption |
|---|---|---|---|
| Unix domain stream | Unix | JSONL | none |
| TCP | IPv4 loopback | JSONL | none |
| TCP | IPv6 loopback | JSONL | none |
| TCP | IPv4 loopback | JSONL | TLS |
| TCP | IPv6 loopback | JSONL | TLS |
| WebSocket | IPv4 loopback | JSON messages | none |
| WebSocket | IPv6 loopback | JSON messages | none |
| WebSocket | IPv4 loopback | JSON messages | TLS/WSS |
| WebSocket | IPv6 loopback | JSON messages | TLS/WSS |

Unix, IPv4, and IPv6 are separate CTest cases even when one executable contains
their shared scenario driver. TLS and WSS cases are registered only when the
corresponding SNode.C targets were found at configuration time. IPv6 cases may
return CTest's skip code only when the host demonstrably lacks usable IPv6
loopback support; protocol or implementation failures are not skips.

The same registration rule applies to the real app-server variants of all nine
frontend transports. TLS and WSS variants reuse the generated certificate
fixture. Each case emits the real initialize response, app-server lifecycle,
native request/response/notification payloads, bridge telemetry, and typed
callback/event delivery through the bounded communication trace.

#### Frontend scenario priority

Not every transport repeats every communication style. Coverage is allocated
by architectural risk and normal production importance:

1. Unix JSONL exercises the richest ordinary SDK lifecycle: typed request and
   response, raw equality, provider event delivery, controller telemetry, and
   orderly shutdown.
2. IPv4 JSONL exercises ordered multiple operations and callback correlation,
   proving that independent in-flight IDs do not cross.
3. IPv6 JSONL exercises the same native bridge framing over the distinct address
   family and validates a bounded nontrivial payload.
4. TLS IPv4 and TLS IPv6 exercise certificate-authenticated connection setup and
   at least one complete typed request/response lifecycle.
5. WebSocket IPv4 exercises HTTP upgrade, `codex` subprotocol selection, typed
   request/response, and provider event delivery through WebSocket framing.
6. WebSocket IPv6 exercises upgrade and a complete typed request/response over
   the IPv6 HTTP authority path.
7. WSS IPv4 and WSS IPv6 combine the WebSocket upgrade with verified TLS and at
   least one complete typed request/response lifecycle.

Controller/observer fanout and handoff are tested deterministically in the
routing test rather than redundantly on every transport. This keeps transport
failures attributable to transport composition while preserving complete
policy coverage.

### TLS fixture lifecycle

TLS material is generated during test setup, never read from committed private
keys. A CTest fixture runs the local OpenSSL executable to create a short-lived
self-signed loopback certificate and private key under the canonical build
tree. Subject Alternative Names cover `localhost`, `127.0.0.1`, and `::1`.

Frontend TLS and WSS server instances receive that generated certificate and
key through their native SNode.C TLS configuration. Clients receive the generated
certificate as their trust anchor, reject unknown certificates, and use
`localhost` as SNI. Every encrypted test declares a fixture dependency, so it
cannot run before successful certificate generation. The generated key is a
disposable build artifact and is neither installed nor committed.

The fixture does not create provider-side TLS coverage because the current
app-server listener has no TLS mode. If a future app-server schema/CLI adds
`wss://`, provider TLS tests must be added using the same generated certificate
fixture and production provider endpoint before that mode is claimed supported.

### Current test inventory and result

The canonical build currently registers 26 codex CTest cases:

| Group | Cases | What crosses the boundary |
|---|---:|---|
| Focused internal contracts | 3 | JSONL framing, frontend SDK/connection behavior, bridge routing and controller policy |
| Deterministic provider acceptance | 4 | owned stdio JSONL plus Unix, IPv4, and IPv6 app-server WebSocket provider endpoints |
| TLS certificate fixture | 1 | generated short-lived loopback certificate and key |
| Deterministic frontend transports | 9 | Unix/IPv4/IPv6 JSONL, IPv4/IPv6 TLS JSONL, IPv4/IPv6 WebSocket, IPv4/IPv6 WSS |
| Real app-server end to end | 9 | actual app-server stdio provider through the bridge and each enabled frontend transport to the production proxy SDK |
| **Total** | **26** | complete enabled focused matrix |

The latest completion-audit run used:

```text
ctest --test-dir "${BUILD_DIR}" \
      -L codex --output-on-failure -j8
```

Result:

```text
100% tests passed, 0 tests failed out of 26
```

All registered Unix, IPv4, IPv6, JSONL, TLS, WebSocket, and WSS cases passed,
including the nine real-app-server end-to-end variants. The build of
`codex-bridge` and `codex-bridge-client` also succeeded. A separate pipe-level
probe verified that rebuilt
`codex-bridge-client --json` emits a response before process exit without
`stdbuf`, confirming the explicit JSONL flush behavior.

A successful run proves the implemented routing, callback, framing, process,
and enabled transport contracts described above. It does not override the
explicit exclusions below and does not prove that the app-server can
reconstruct live items omitted by its own `thread/read` projection.

### Explicit exclusions

These tests do not:

- modify the OpenAI Codex app-server or its source;
- use the user's real `CODEX_HOME`;
- test app-server persistence or model behavior;
- reintroduce bearer tokens or another authentication protocol;
- test RFCOMM unless separately approved with suitable hardware/runtime setup;
- run broad suites, stress loops, sleeps, or timing-based workload simulations;
- create an alternative frontend SDK or bypass the production client adapter.

The suite proves provider transport composition against deterministic native
protocol peers, bridge logic, SDK correspondence, and frontend transport
composition. Its real-process matrix additionally proves the actual app-server
stdio handshake and schema envelope across every enabled frontend transport.
Live model behavior and app-server persistence remain separate integration
concerns because the real app-server is authoritative for those domains.

### SNode.C dependency policy

`codex` always builds against the current `master`/HEAD of the upstream
SNode.C repository. The dependency must not be pinned to a particular commit
in CMake, CI, or release automation. CI performs a shallow, single-branch clone
of SNode.C `master` and records the resolved HEAD in its log for diagnostics
without turning that observation into a dependency constraint.

The installed backend/frontend SDK target is `AISuite::OpenAICodex`. Its
package configuration loads every SNode.C component referenced by the exported
static library before importing the target. CI builds both applications and the
focused codex suite against SNode.C `master`/HEAD, runs the registered focused
tests, and verifies installation.
