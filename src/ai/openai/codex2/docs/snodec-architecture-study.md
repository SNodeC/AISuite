# SNode.C Architecture Study for codex2

## Purpose and Status

This report records the SNode.C, MQTTSuite, and legacy AISuite architecture
study requested before further `codex2` implementation. It is a design input,
not an implementation report. Implementation was stopped while this study was
performed.

The study focused on:

- the SNode.C configuration hierarchy and `utils::SubCommand` contract;
- event-loop ownership, callback scheduling, and shutdown behavior;
- timer scheduling, cancellation, and callback lifetime;
- asynchronous operating-system pipes, in-process pipe adapters, and files;
- stream server/client construction and per-connection protocol contexts;
- the shared `src/net` architecture and all implemented address families:
  IPv4, IPv6, Unix domain, Bluetooth L2CAP, and Bluetooth RFCOMM;
- legacy streams, TLS, HTTP, WebSocket, and WSS composition;
- HTTP parsing, body streaming, response sequencing, and protocol upgrades;
- WebSocket framing, limits, close behavior, and subprotocol architecture;
- callback, transport-adapter, packet-decoder, session, and broker patterns
  from MQTT and MQTTSuite;
- provider-process and protocol lessons from the legacy AISuite Codex stack;
- consequences for the new slim `codex2` / `codex-bridge` implementation.

The source revisions inspected were:

- SNode.C: `bc43179dbee2b5a0286420a61d8f1ceaef01530d`
- MQTTSuite: `eedded8ab697e44e0b7ac1ed4336929e427fe8c9`
- AISuite `codex2-master` baseline: `1f3410ac9eac6857f0f99c39b8c883ec163f72d5`

The OpenAI Codex repository at `/home/voc/tmp/codex/codex` was treated as a
read-only protocol/schema source. It was not modified.

## Executive Conclusions

The template and CMake composition used by SNode.C is intentional architectural
machinery, not accidental framework weight. It permits applications to select
only the required address families, stream implementation, TLS/HTTP/WebSocket
layers, and protocol components while reusing one coherent lifecycle model.
The resulting source-level learning cost is the tradeoff by which the original
slim, modular architecture is fulfilled.

1. `codex-bridge` configuration must be represented by an application-specific
   `utils::SubCommand` subclass. It must not add application options directly to
   `utils::Config::configRoot`.

2. Each SNode.C network instance already owns its address, connection,
   transport, retry/reconnect, queue, timeout, and TLS configuration. The bridge
   application must instantiate and configure those native instances rather
   than duplicating their options.

3. Generic `SocketClient`, `SocketServer`, and `SocketConnection` classes must
   remain protocol-neutral. Codex operations belong on a Codex protocol/facade
   object associated with a socket context or provider context.

4. A per-connection `SocketContext` or WebSocket `SubProtocol` is the correct
   endpoint adapter. A shared `CodexBridge` object is the correct meeting point
   for one provider endpoint and multiple frontend endpoints.

5. TLS is a transport specialization below the stream context. Plain JSONL and
   TLS JSONL must use the same Codex frontend protocol context/factory.

6. WebSocket is not a newline stream. It is an HTTP upgrade followed by a
   negotiated SNode.C WebSocket subprotocol whose message callbacks deliver
   complete or fragmented WebSocket messages. It needs a Codex WebSocket
   subprotocol adapter, not reuse of a JSONL parser by accident.

7. SNode.C is event-driven and callback-owned. Blocking process waits, blocking
   socket loops, and synchronous assumptions in protocol callbacks are invalid.
   Deferred callbacks must be lifetime-safe and must respect SNode.C shutdown.

8. SNode.C already provides bounded writer queues and backpressure signals.
   Every bridge delivery path must inspect `QueueResult`; a `void` convenience
   transmitter must be used only after bounded admission has been established.

9. The legacy AISuite app-server transport contains valuable process
   supervision and lifecycle lessons. Its `BackendCore`, reducer, snapshot,
   frontend projection, journal, and frontend `State` architecture are exactly
   the semantic middle layer that `codex2` is intended to avoid.

10. Complete typed app-server coverage should be generated from the canonical
    app-server schema. The typed objects are lossless JSON-backed views. They do
    not become a cache, reducer, alternative schema authority, or replacement
    protocol.

## 1. SNode.C Configuration Model

### 1.1 Configuration is a tree of typed objects

`utils::ConfigRoot` is the root of a hierarchy of `utils::SubCommand` objects.
`SubCommand` wraps a CLI11 application node through `utils::AppWithPtr`, which
retains a pointer to the corresponding typed configuration object.

A concrete application configuration follows this shape:

```cpp
class Configuration : public utils::SubCommand {
public:
    constexpr static std::string_view NAME{"codex"};
    constexpr static std::string_view DESCRIPTION{"Codex app-server bridge"};

    explicit Configuration(utils::SubCommand* parent)
        : utils::SubCommand(parent, this, "Applications") {
        // Add Codex-specific options here.
    }
};
```

It is registered and obtained through the root as a typed node:

```cpp
auto* configuration =
    utils::Config::configRoot.newSubCommand<Configuration>();
```

`newSubCommand<T>()` owns child objects created through that API, while
`getSubCommand<T>()` retrieves the typed node. Config-section base subobjects
used by multiple-inheritance network configuration are attached to the CLI
tree but have distinct ownership rules; the `SubCommand` implementation
explicitly avoids deleting those through the child set.

### 1.2 Static identity and hierarchy are part of the contract

Concrete subcommands expose static `NAME` and `DESCRIPTION` values. The
templated `SubCommand` constructor uses these to create the CLI node and stores
the concrete object pointer in `AppWithPtr`. The parent and group name determine
where options appear in help, configuration files, and generated command-line
views.

This is more than cosmetic CLI organization. It keeps application options,
network instances, and nested transport sections addressable as typed parts of
one configuration object graph.

### 1.3 Requirement semantics are richer than CLI11 defaults

SNode.C tracks canonical and effective requirements separately for nodes and
options. Important APIs include:

- `required(...)` for canonical requirements;
- `needs(...)` for dependencies between options or subcommands;
- `forceUnrequired(...)` for contextual suppression;
- disabled and force-unrequired suppression propagation;
- `setRequireCallback(...)` and `finalCallback(...)`;
- `setDefaultValue(...)` for programmatic instance defaults;
- `setConfigurable(...)` for persistence/config-file participation.

Consequently, the bridge should not bypass the config subsystem with ad hoc
argument parsing or environment-only settings. Doing so would lose the native
required/suppression/config-file semantics.

### 1.4 Parsing belongs to the SNode.C lifecycle

SNode.C deliberately uses a two-stage configuration lifecycle.
`core::SNodeC::init(argc, argv)` initializes the event loop and performs the
initial root-option parse. The application can then construct network instances
and register their instance-specific `SubCommand` trees. When
`core::SNodeC::start()` begins, `utils::Config::bootstrap()` performs the final
configuration bootstrap before the event loop processes traffic.

Application-wide subcommands that are needed before instance construction are
registered before `init()` in the existing broker, integrator, and bridge
applications. Per-instance sections in the CLI and store applications are
created after `init()` as part of constructing each network instance. In both
cases, code must not rely on final effective values until bootstrap. The config
layer owns configuration files, help, version, effective-configuration display,
and command-line reconstruction throughout this lifecycle.

### 1.5 Network configuration is instance-owned

Every SNode.C client/server instance owns a `net::config::ConfigInstance` with a
role and instance name. Concrete transport configurations compose sections
through multiple inheritance. Depending on the transport, these include:

- local and remote address sections;
- physical socket options;
- stream connection timeouts and block sizes;
- maximum write-queue bytes and watermarks;
- retry policy and retry limits;
- client reconnect policy;
- TLS certificate, trust, cipher, SNI, and handshake/shutdown settings.

The instance name scopes generated options and configuration. This permits
several listeners or clients of the same transport family without inventing a
second option namespace in `codex-bridge`.

### 1.6 Consequence for codex2 configuration

The `codex2` application `Configuration` should contain only semantics not
already represented by a network instance, such as:

- app-server executable and app-server arguments;
- child-specific `CODEX_HOME`;
- provider connection mode where more than one app-server mode is supported;
- controller selection/handoff policy;
- bridge-envelope limits not equivalent to the socket writer limit;
- bridge telemetry controls;
- optional provider restart policy.

It must not duplicate Unix pathname, IP bind address, port, TLS certificate,
TLS trust, socket timeout, queue watermark, retry, reconnect, RFCOMM, or HTTP
listener options already supplied by SNode.C.

The legacy `codex-backend` configuration frequently added options directly to
`ConfigRoot`. That is a lesson in what not to carry into the new application:
the new configuration must be a proper `SubCommand` subclass from the start.

## 2. Event Loop and Lifetime Model

### 2.1 Global lifecycle

The application lifecycle is:

```text
main:

    core::SNodeC::init()
        |
        v
    core::SNodeC::start()
        or repeated core::SNodeC::tick()
        |
        |  returns after internal code requests termination
        v
    normal process exit

internal runtime/application callback:

    core::SNodeC::stop()
```

The event-loop states progress through `LOADED`, `INITIALIZED`, `RUNNING`, and
`STOPPING`. `start()` repeatedly ticks while registered resources/observers
remain and the loop is running. `tick()` delegates readiness processing to the
event multiplexer while protecting critical signal-state transitions.

`core::SNodeC::stop()` is requested by inner runtime or application code when
termination is needed; it is not a routine sequential call in `main()` after
`start()`. Once the event loop returns, `main()` exits normally. No additional
framework teardown call is made from `main()`.

### 2.2 Event receivers are callback dispatch objects

`core::EventReceiver` is the base dispatch abstraction. Its notable scheduling
operations are:

- `span()` to keep an event receiver active for another event-loop turn;
- `relax()` to stop self-rescheduling;
- `atNextTick()` for deferred execution.

Descriptor receivers add registration state, enable/disable, suspend/resume,
timeouts, observation accounting, and shutdown callbacks. Specialized read,
write, accept, and connect receivers map multiplexer readiness into protocol or
transport callbacks.

### 2.3 No blocking bridge core

The bridge must not create its own blocking read/write loop around SNode.C
descriptors. Provider pipes and frontend sockets must feed the bridge through
callbacks. Likewise, bridge requests should enqueue work and return; responses
arrive asynchronously through correlated callbacks or raw-message handlers.

### 2.4 Callback lifetime rules

Several SNode.C resources become framework-owned after registration. Pipe
sources/sinks and connection contexts can close asynchronously and may delete
themselves after terminal callbacks. Therefore:

- callback code must not delete a framework-owned object directly unless its
  API explicitly requires that ownership action;
- code must not dereference an endpoint after its close/detach callback;
- deferred callbacks should capture a weak lifetime token, not a naked `this`;
- endpoint deregistration from `CodexBridge` must happen exactly once;
- bridge shutdown must invalidate deferred deliveries before contexts vanish;
- callbacks must tolerate provider/frontend closure racing with queued work.

The lifetime-token pattern used in the legacy WebSocket adapter is useful:
deferred timers capture a `weak_ptr`, lock it, and verify that the endpoint
pointer remains attached before invoking it.

### 2.5 Signal and graceful-close behavior

Protocol contexts receive `onSignal(int)` and can initiate protocol-level
shutdown before transport termination. For the bridge this means:

- stop accepting new frontend work;
- reject or leave unresolved work according to explicit bridge policy;
- send a bounded bridge closing diagnostic where possible;
- stop/reap the owned app-server process;
- allow SNode.C to finish queued transport shutdown;
- never wait synchronously inside the signal callback.

## 3. Stream Server and Client Architecture

### 3.1 Template composition

SNode.C stream endpoints are assembled from transport and protocol types:

```text
SocketServer<SocketAcceptor, SocketContextFactory, FactoryArgs...>
SocketClient<SocketConnector, SocketContextFactory, FactoryArgs...>
```

The concrete namespaces provide convenient constructors, for example variants
of `net::un::stream::legacy::Server`, `net::in::stream::tls::Server`, and the
corresponding clients. These choose the physical socket, address type,
connector/acceptor, stream reader/writer, and concrete configuration type.

The protocol factory remains transport-independent where the byte-stream
semantics are identical.

### 3.2 Shared endpoint context versus per-connection context

A server/client endpoint owns a shared internal context containing:

- the typed configuration;
- a shared `SocketContextFactory`;
- connect/connected/disconnect callbacks;
- connection ID allocation;
- flow controller and retry/reconnect state;
- logging scope and aggregate lifecycle information.

For each accepted or established connection, the factory receives the
`SocketConnection*` and returns a new protocol `SocketContext`. That context is
the correct place for per-peer JSONL framing, endpoint identity, role, request
correlation belonging to that connection, and registration with the shared
bridge.

The shared `CodexBridge` is passed into the factory by reference or shared
lifetime, then into each created context. It is not stored in generic socket
classes.

### 3.3 Callback levels

There are distinct callback layers:

1. Listener/connect status callbacks report address plus `core::socket::State`.
2. Endpoint callbacks report physical connection creation, successful
   connection, and disconnection.
3. `SocketContext` callbacks report protocol attachment, received data,
   disconnection, and signals.
4. The Codex facade reports JSON-RPC completion or incoming notification/server
   request through typed callbacks.

These levels must remain separate. A listener becoming ready is not a frontend
protocol handshake. A TCP connection is not a bridge registration. A bridge
registration is not controller ownership.

### 3.4 Client retry and reconnect

`SocketClient` uses a `ClientFlowController` and timers for connect retry and
post-disconnect reconnect. Configuration governs retry enablement, tries,
backoff base/limit/jitter, retry-on-fatal, reconnect enablement, and reconnect
delay. Connection-cycle identifiers prevent stale timers from acting on a new
cycle.

`codex2` should consume this mechanism for network provider/client modes rather
than implementing another reconnect timer. Provider-process restart for stdio
is a separate application concern because it owns a child process, not a
socket-client reconnect.

### 3.5 Reader behavior and backpressure

The stream reader reads into bounded blocks and suspends descriptor readiness
while application data is pending consumption. `SocketContext::readFromPeer()`
advances the consumed count. A protocol parser should consume only complete
units and retain a bounded incomplete suffix.

The stream writer exposes bounded queue accounting and:

```cpp
enum class QueueResult {
    Queued,
    WouldExceedLimit,
    Closed,
    ShutdownInProgress
};
```

It tracks total queued and total sent bytes and can suspend the paired source
at a high watermark until the queue falls below a low watermark. Bridge
delivery must classify every result:

- `Queued`: delivery accepted by this transport layer;
- `WouldExceedLimit`: apply bounded backpressure or close according to policy;
- `Closed`: endpoint is terminal;
- `ShutdownInProgress`: no new application write is valid.

An endpoint must not report successful fanout merely because serialization
succeeded. Acceptance by the SNode.C writer is part of delivery success.

## 4. Transport Composition

### 4.1 Byte-stream transports

Unix-domain, IPv4, IPv6, Bluetooth L2CAP, Bluetooth RFCOMM, and their TLS stream
variants can carry the same bridge JSONL framing when exposed as raw streams. One
`FrontendStreamSocketContextFactory` can therefore be instantiated by multiple
SNode.C server types.

The transport instance supplies addresses, connection IDs, queue limits,
timeouts, retry/reconnect where applicable, and transport-level diagnostics.
The context supplies only bridge framing and routing.

### 4.2 Provider transport modes

Provider-side app-server connectivity and frontend connectivity are different
roles:

- stdio provider mode owns one child process and one ordered JSONL session;
- app-server Unix or WebSocket provider mode uses SNode.C client composition if
  supported by the selected app-server invocation;
- frontend listeners accept many connections and register them with one bridge.

The bridge's one-controller policy applies to frontend endpoints. It must not
be confused with the number of transport connections an app-server mode may
technically accept.

### 4.3 No authentication layer

The agreed `codex2` architecture does not add bearer-token or other
application authentication. This does not remove transport configuration or
TLS functionality. It means `codex2` does not create a parallel frontend
identity/authentication protocol. Connection identity is bridge telemetry and
controller routing state, not an authentication principal.

## 5. TLS Architecture

### 5.1 TLS is below the protocol context

SNode.C TLS aliases replace the plain stream connector/acceptor and
reader/writer with TLS-aware variants while retaining the same
`SocketContextFactory` contract. Therefore:

```text
Codex JSONL SocketContext
    -> TLS SocketConnection/Reader/Writer
    -> physical Unix/IP/L2CAP/RFCOMM socket
```

The Codex JSON protocol layer should not know whether bytes are protected by
TLS. It should receive the same connection and queue abstractions.

### 5.2 TLS configuration composition

`net::config::ConfigTls` is a `ConfigSection` attached to a
`ConfigInstance`. It supplies native options/accessors for:

- certificate chain and private key;
- private-key password;
- CA certificate and CA directory;
- use of the default CA directory;
- acceptance of unknown certificates;
- OpenSSL cipher list and SSL options;
- TLS initialization timeout;
- TLS shutdown timeout;
- handling of missing `close_notify`.

Client TLS adds SNI. Server TLS adds forced SNI and multiple SNI certificate
maps. SSL contexts are lazily created from instance configuration; server
configuration builds SAN/SNI lookup maps and can choose an SSL context from the
client hello.

None of these options belong in the application `Configuration` class.

### 5.3 TLS lifecycle is asynchronous and explicit

The TLS connection maintains a state machine including plaintext, prepared,
handshaking, active, shutdown-in-progress, shutdown-complete-awaiting-release,
closing, fatal, and closed states.

During handshake the ordinary reader/writer is suspended. Success resumes the
stream; timeout or status errors close it. During shutdown, new writes are
blocked, `close_notify` is handled, and completion is callback-driven. A shared
lifecycle control object prevents asynchronous SSL callbacks from touching a
destroyed connection owner.

The implementation also supports a bounded handoff of application bytes found
during TLS shutdown for transitions back to plaintext. The bridge should not
depend on this specialized behavior, but it must not violate it by assuming
that TLS shutdown is equivalent to immediate descriptor close.

### 5.4 WSS consequence

WSS is composition, not a separate Codex protocol:

```text
Codex WebSocket SubProtocol
    -> WebSocket framing
    -> HTTP upgrade
    -> TLS stream
    -> IP/Unix transport
```

The same Codex WebSocket subprotocol implementation should serve WS and WSS.
TLS selection belongs to the containing SNode.C web application/listener.

## 6. WebSocket and Subprotocol Architecture

### 6.1 Upgrade pipeline

SNode.C WebSocket support is layered over HTTP:

```text
HTTP SocketContext
    -> HTTP upgrade selector
    -> WebSocket SocketContextUpgradeFactory
    -> WebSocket SocketContextUpgrade
    -> SubProtocolFactorySelector
    -> selected SubProtocol instance
```

Server and client roles have role-specific upgrade and subprotocol classes but
share framing, receiver, transmitter, context, and factory concepts.

The server examines offered subprotocol names and selects one it can load. The
client requests a named subprotocol. Factories may be linked statically or
loaded dynamically; `onlyLinked` can prohibit dynamic loading. For
`codex-bridge`, statically linking a named Codex subprotocol is the narrow and
predictable choice.

### 6.2 WebSocket subprotocol contract

A concrete subprotocol derives from the role-specific base and implements:

```cpp
void onConnected();
void onMessageStart(int opCode);
void onMessageData(const char* chunk, std::size_t chunkLen);
void onMessageEnd();
void onMessageError(std::uint16_t error);
void onDisconnected();
bool onSignal(int signal);
```

Its outbound facade provides complete and fragmented message APIs, ping/pong,
and close. The common base can maintain a ping interval and closes when too
many pings remain unanswered. It also exposes the underlying
`SocketConnection`, payload totals, and online duration.

The receiver enforces optional maximum frame bytes, maximum message bytes, and
maximum fragment counts. It validates masking and framing and separates text,
binary, continuation, close, ping, and pong opcodes.

### 6.3 Codex WebSocket message rules

The bridge wrapper is JSON, so a Codex WebSocket frontend subprotocol should:

- accept text messages only;
- clear its input buffer at message start;
- append bounded fragments in `onMessageData`;
- parse exactly one bridge envelope at message end;
- reject malformed, oversized, binary, or protocol-invalid messages with an
  appropriate bounded close reason;
- send one bridge envelope as one text message, fragmenting only as a transport
  detail when required;
- deregister from `CodexBridge` on disconnect/destruction exactly once.

JSONL newline framing is unnecessary inside WebSocket messages. The JSON
payload can be identical, but framing semantics are not.

### 6.4 WebSocket transmit boundedness

The convenience WebSocket transmitter currently presents `void` send methods.
The underlying socket writer remains bounded. The legacy AISuite adapter shows
the necessary precaution: calculate bounded WebSocket framing overhead, inspect
the configured writer limit and outstanding bytes, probe terminal writer state,
and only then call the transmitter. If admission is unavailable, return
backpressure or close; do not silently lose an event.

A cleaner future SNode.C API could expose queue admission directly through the
WebSocket transmitter, but `codex2` must work correctly with the current API
without modifying SNode.C as part of this narrow implementation.

### 6.5 Server groups are not the bridge

The server WebSocket base includes `GroupsManager`, subscription, broadcast,
and iteration over group members. These are useful generic facilities, but
they are not sufficient as `CodexBridge` because they do not encode provider
routing, controller ownership, JSON-RPC request policy, or bridge telemetry.
Fanout should remain centralized in `CodexBridge`; a subprotocol is an endpoint
adapter.

## 7. MQTT and MQTTSuite Lessons

### 7.1 Why MQTT is relevant

MQTT demonstrates the intended SNode.C separation:

- transport-specific socket classes establish a byte path;
- a socket context or WebSocket subprotocol adapts that path;
- a protocol object owns MQTT parsing and methods;
- an application-specific object coordinates higher-level behavior;
- configuration is represented as typed `SubCommand`/instance sections.

This is a knowledge source, not a template to copy mechanically.

### 7.2 MQTT over WebSocket adapter

`iot::mqtt::SubProtocol<WSSubProtocolRole>` combines a WebSocket role class with
`MqttContext`. It translates WebSocket messages to the stream-like `recv`/`send`
contract expected by MQTT:

- incoming binary message fragments are accumulated;
- message completion makes bytes available to `MqttContext`;
- a custom `EventReceiver` spans the next event-loop turn while buffered data
  remains;
- outbound MQTT chunks become binary WebSocket messages;
- connected/disconnected/signal callbacks delegate to MQTT lifecycle methods;
- text WebSocket frames are rejected as the wrong opcode.

The useful lesson is adapter placement: transport framing is converted at the
subprotocol boundary, while MQTT remains a protocol object. For Codex, the
native message is already JSON, so the adapter can be simpler and should not
create a fake stream abstraction unless a shared parser truly needs it.

### 7.3 MQTTSuite factories

MQTTSuite `SocketContextFactory` classes construct an application-specific MQTT
object for each connection and return the SNode.C MQTT context around it.
WebSocket `SubProtocolFactory` classes perform the analogous construction after
upgrade negotiation.

For `codex2`, factories should construct only endpoint adapters. The central
`CodexBridge` and provider facade are application-owned and shared; they must
not be recreated per frontend connection.

### 7.4 MQTTSuite application configuration

`mqtt::lib::ConfigApplication` derives from `utils::SubCommand`. Concrete
application configurations derive from it and pass their concrete pointer to
the base. This confirms the required shape for `codex-bridge::Configuration`.

MQTTSuite also demonstrates programmatically setting defaults on native
instance configuration while leaving those settings visible to the config
system. This is preferable to hidden hard-coded socket setup.

### 7.5 Application coordinator lesson

MQTTSuite bridge objects coordinate protocol peers and domain routing outside
the socket implementation. The relevant principle is that an intermediary can
be application-level without becoming transport-level or parser-level.

For `codex2`, `CodexBridge` similarly coordinates endpoints, but its domain is
deliberately narrow:

- endpoint registration/removal;
- explicit controller/observer role;
- routing and fanout of lossless app-server messages;
- request ownership/correlation needed for routing;
- bridge telemetry and terminal diagnostics.

It must not grow into a retained Codex semantic model.

## 8. Legacy AISuite Lessons

### 8.1 What is worth retaining

The legacy stack contains mature operational work that should inform the new
implementation:

- robust app-server process spawning with dedicated stdin/stdout/stderr pipes;
- child environment overrides, including child-specific `CODEX_HOME`;
- descriptor relocation away from standard descriptors;
- rollback when parent-side setup fails after spawn;
- process-group and signal-mask setup;
- pidfd-based child observation with nonblocking `waitpid` fallback;
- bounded stdin queueing;
- incremental bounded JSONL framing of stdout and stderr;
- distinct process, protocol, transport, and diagnostic callbacks;
- exact process-exit reporting;
- weak-lifetime callback protection;
- complete typed and raw protocol access;
- server-request ownership tokens preventing duplicate/wrong responses;
- transport close diagnostics with queue accounting and close reason.

These are lifecycle and losslessness lessons. Reusing or extracting a small,
well-bounded provider transport is reasonable if it does not pull the legacy
semantic stack into `codex2`.

### 8.2 AppServerClient facade shape

The old `AppServerClient` correctly separates:

- transport callbacks;
- raw JSON-RPC request/notify/respond/reject methods;
- typed domain facades;
- asynchronous response handlers;
- notification and server-request dispatch;
- lifecycle state and diagnostics.

The new facade should preserve this useful direction while simplifying the
surface:

```cpp
client.threadList([](ThreadList& result) {
    if (!result) {
        return;
    }

    const auto& raw = result.getRaw();
    // Typed and raw access refer to the same received value.
});
```

The callback argument is the concrete JSON-backed result object, not a second
generic `Result<T>` wrapper. Failure information lives in the concrete result's
status/error accessors and `operator bool()`.

### 8.3 What must not be retained

The old stack evolved into several semantic layers:

```text
app-server events
    -> typed decoding
    -> BackendCore
    -> Reducer / BackendState / Snapshot
    -> frontend model/journal/projection/service
    -> generated frontend protocol
    -> frontend SDK State
    -> CodexUI reconciliation
```

This architecture had to define authority, merge/replace behavior,
completeness, retention, bounded projection, replay, synchronization,
front-end snapshots, pending-request state, and semantic views. The live
investigation showed that these boundaries are difficult to keep equivalent to
the app-server's own state and can create history, pending-state, and reconnect
failure modes.

`codex2` therefore must not include equivalents of:

- `BackendCore`, `BackendState`, or `Reducer` for Codex state;
- AISuite-owned thread/turn/item snapshots;
- event journals for semantic replay;
- a projected canonical frontend `State`;
- merge/preserve/replace rules for provider objects;
- retained command-output aggregation;
- AISuite-owned pending-request truth;
- frontend synchronization barriers over cached semantic state.

### 8.4 Raw pass-through is a compatibility requirement

The old typed protocol required extensive hand-maintained codecs and explicit
surface registration. The new implementation should generate complete types
from the canonical exported app-server schema, while retaining every original
JSON value and unknown field.

An older bridge must still forward a newer unknown notification or field.
Typed decoding failure must not become forwarding failure unless the bridge
cannot classify enough JSON-RPC structure to enforce controller routing safely.

### 8.5 Request correlation is routing, not semantic caching

Some bounded state is unavoidable for JSON-RPC transport correctness:

- frontend connection identity;
- controller identity;
- monotonically assigned bridge sequence;
- mapping of in-flight request IDs to the frontend that originated them when
  response routing requires it;
- ownership of app-server server requests so only the controller responds;
- bounded queue/backpressure accounting.

This is ephemeral routing metadata. It must be removed on response,
disconnection, controller handoff, provider generation change, or terminal
failure. It must never be presented as reconstructable Codex thread state.

## 9. Complete Typed Protocol Contract

### 9.1 Canonical source

The canonical combined schema currently available from the read-only
app-server checkout is:

```text
/home/voc/tmp/codex/codex/codex-rs/app-server-protocol/schema/json/
    codex_app_server_protocol.schemas.json
```

The inspected export contains broad client-request, server-request,
client-notification, server-notification, and nested datatype surfaces. The
exact generated set must come from the schema at generation time rather than a
hard-coded count in bridge source.

### 9.2 Required generated coverage

Every schema-defined JSON-RPC surface must have a concrete C++ datatype:

- all client requests;
- every request parameter and result;
- all client notifications;
- all app-server requests and their response types;
- all app-server notifications;
- all nested records, arrays, maps, identifiers, enums, tagged unions,
  nullable values, and errors referenced by those messages.

Selected/manual coverage is not acceptable.

### 9.3 JSON-backed datatype behavior

Each generated object should own or share the complete received
`nlohmann::json` value and provide:

- `getRaw()` returning the native JSON representation;
- typed read accessors matching schema fields;
- optional/nullable distinctions where the schema distinguishes them;
- enum/union inspection without discarding unknown future alternatives;
- error/status access and `operator bool()` where it represents a callback
  result;
- preservation of unknown object members;
- no implicit mutation of provider data;
- no reference whose lifetime is shorter than the callback contract.

Typed child objects can be lightweight views sharing the owning result's JSON
lifetime. If values are returned beyond a callback, ownership must remain
explicit and safe.

### 9.4 Methods live on a Codex facade

Methods such as `threadList`, `threadRead`, and `turnStart` do not belong on
SNode.C `SocketClient`. A Codex provider client/facade associated with the
provider protocol context exposes them and submits native JSON-RPC over that
context.

Both paths are mandatory:

```cpp
client.threadList(callback);  // generated typed facade
client.sendRawJson(message);  // lossless raw path
```

Notifications and app-server requests likewise have typed callback registration
and a raw observation path.

## 10. Recommended codex2 Runtime Object Graph

```text
codex-bridge application
|
+-- Configuration : utils::SubCommand
|
+-- CodexBridge
|   |-- provider endpoint identity/generation
|   |-- frontend endpoint registry
|   |-- controller connection ID
|   |-- bounded in-flight routing map
|   |-- bridge sequence and telemetry
|   `-- no Codex semantic state
|
+-- Provider endpoint (exactly one active)
|   |-- stdio StdioAppServerTransport, or SNode.C socket client
|   |-- app-server JSONL/WebSocket framing
|   `-- CodexAppServerClient typed/raw facade
|
`-- Frontend listeners (zero or more configured instances)
    |-- Unix JSONL stream server
    |-- IPv4/IPv6 JSONL stream servers
    |-- TLS JSONL stream servers
    |-- L2CAP/RFCOMM JSONL stream servers
    |-- HTTP WebSocket applications
    `-- HTTPS/WSS applications
        |
        `-- per connection:
            FrontendSocketContext or FrontendWebSocketSubProtocol
                -> bridge envelope codec
                -> CodexBridge registration
```

### 10.1 Provider endpoint responsibilities

- establish and supervise the app-server connection/process;
- frame native app-server JSON-RPC without alteration;
- expose raw incoming/outgoing observation hooks;
- correlate typed client method callbacks;
- deliver app-server notifications and server requests asynchronously;
- report provider generation, process exit, transport failure, and diagnostics;
- never publish a reconstructed thread state.

### 10.2 Frontend endpoint responsibilities

- frame/decode one bridge envelope at a time;
- enforce bounded frame/message size;
- register/deregister with `CodexBridge`;
- expose connection identity and current role;
- deliver accepted outbound envelopes with `QueueResult` handling;
- report close cause and queue counters;
- never decide Codex state authority.

### 10.3 CodexBridge responsibilities

- accept provider and frontend endpoint lifecycle callbacks;
- assign bridge-local connection IDs and delivery sequence numbers;
- select one controller explicitly;
- allow observers to receive all provider messages;
- route permitted controller messages to the provider;
- allow explicitly classified read-only observer requests if the contract
  chooses to support them;
- route responses back to request origin where required while preserving
  observer visibility according to the protocol contract;
- route app-server requests to the controller and observe their resolution;
- fan out native provider events without semantic reduction;
- emit separate `bridge.*` lifecycle and telemetry messages;
- clear ephemeral routing data on terminal boundaries.

### 10.4 Recovery behavior

After frontend reconnect, the frontend asks app-server for authoritative data
through native methods such as `thread/list` and `thread/read`. The bridge does
not send an AISuite semantic snapshot.

After provider restart/reconnect:

- increment provider generation telemetry;
- fail or clear in-flight requests from the previous generation;
- report the transition to frontends;
- let clients requery app-server authority;
- do not attempt to merge old bridge-retained Codex objects into new provider
  responses.

## 11. Implications for the Paused Bootstrap Implementation

The uncommitted bootstrap code was intentionally not continued or built during
this study. Before implementation resumes, it needs a source-level audit
against these findings.

Required checks include:

1. Confirm the application `Configuration` is a real `utils::SubCommand`
   subclass and registration follows normal SNode.C ordering.

2. Remove any duplicated network options that belong to instance
   configurations.

3. Ensure frontend raw streams use a SNode.C `SocketContextFactory` and
   per-connection context rather than a standalone socket loop.

4. Add a native WebSocket subprotocol/factory path; do not feed WebSocket
   fragments into JSONL framing.

5. Ensure every send path handles bounded queue admission and terminal writer
   states.

6. Rework stdio provider ownership to follow SNode.C pipe/event-loop lifecycle,
   including child observation and asynchronous shutdown. The legacy transport
   can inform this, but semantic legacy dependencies must not be imported.

7. Keep `CodexBridge` free of thread, turn, item, command-output, pending-state,
   snapshot, and projection storage.

8. Define precise ephemeral request-ID routing and cleanup semantics before
   implementing controller handoff.

9. Generate complete JSON-backed protocol datatypes from the canonical schema;
   do not proceed with a selected hand-written result set.

10. Preserve a raw native JSON path in both directions even when typed decoding
    is available.

## 12. Build and Scope Constraints

The canonical incremental build directory remains:

```text
/home/voc/projects/drafts/AISuite-extraction/build/codex-build
```

The bootstrap phase intentionally has no tests. Existing legacy Codex sources
and tests remain files in the repository but are disabled from default CMake
and CI on this branch. They are a knowledge base only.

No implementation should broaden the agreed architecture without stopping for
explicit user approval. In particular, adding authentication, semantic cache,
snapshot/replay state, frontend SDK state, or a new authority model would be an
architectural extension and is out of scope.

## 13. Final Design Rules

The following rules are binding inputs for continued implementation:

- use `utils::SubCommand` for application configuration;
- use native SNode.C instance configuration for network semantics;
- use SNode.C event-loop callbacks and lifetime rules end to end;
- use per-connection socket contexts or WebSocket subprotocols;
- keep generic socket classes free of Codex methods;
- place typed methods on a Codex-specific provider facade;
- make every typed callback result a concrete lossless JSON-backed C++ object;
- generate every app-server JSON-RPC datatype, not a selected subset;
- retain mandatory raw JSON send/receive access;
- preserve unknown provider messages and fields;
- use one shared bridge for multi-client registration, routing, and fanout;
- enforce one explicit controller and any number of observers;
- add only bridge-owned telemetry metadata outside native payloads;
- handle bounded queue admission and close reasons explicitly;
- query app-server for recovery and retained history;
- never make AISuite a second Codex state authority.

## 14. Timer Subsystem

This section is based on `src/core/Timer*` and `src/core/timer/*` in SNode.C.
The timer subsystem is part of the event loop. It is not a worker-thread
scheduler and it must not be used to hide blocking work.

### 14.1 Public timer handles and receivers have different ownership

`core::timer::Timer` is a move-only handle around a
`core::TimerEventReceiver`. The concrete factories are:

- `singleshotTimer(callback, delay)`;
- `intervalTimer(callback, interval)`;
- stoppable `intervalTimer(callbackWithStop, interval)`.

The receiver is registered with the event-loop-owned
`TimerEventPublisher`. The handle and receiver maintain a backlink so moving a
handle updates the receiver's handle pointer. Destroying the handle only clears
that backlink; it does not implicitly cancel the scheduled receiver. Code that
needs cancellation or restart control must retain the handle and call the
explicit operation.

This split allows fire-and-forget single-shot timers while still supporting a
controlled timer lifetime. It also means a callback must not assume that the
original C++ handle object still exists.

### 14.2 Scheduling and callback order

Receivers are kept in absolute-time order by `TimerEventPublisher`. Its next
deadline contributes directly to the multiplexer timeout. Once the event loop
wakes, due receivers are dispatched and deferred removals are processed.

The concrete behaviors are:

- a single-shot receiver invokes its callback and then cancels itself;
- an interval receiver advances its next absolute deadline before invoking the
  callback;
- a stoppable interval callback receives a stop function that cancels that
  receiver;
- `restart()` removes the receiver from the ordered set, computes a new
  deadline from the current time, and reinserts it;
- regular interval advancement is based on the prior deadline rather than on
  callback completion time, which preserves cadence instead of accumulating
  callback-duration drift.

Cancellation is event-loop-safe and deferred through the publisher's removal
path. The receiver is deleted after it becomes unobserved. During event-loop
shutdown, outstanding timer receivers are cancelled; enabling a new receiver
while the loop is already stopping is rejected and the receiver is disposed.

### 14.3 Callback and lifetime rules

Timer callbacks run on the event-loop thread. They must therefore:

- finish quickly;
- avoid blocking waits and synchronous subprocess management;
- avoid capturing an endpoint by an unprotected raw pointer;
- use a weak lifetime token when delayed work can outlive a socket context;
- treat cancellation and endpoint teardown as ordinary races to resolve.

The existing socket retry paths demonstrate the intended pattern: schedule a
bounded retry, capture weak lifetime state, and revalidate the endpoint before
performing work.

### 14.4 codex2 use

Appropriate codex2 timer uses are provider restart delay, frontend reconnect
delay where a client endpoint owns that policy, request-routing expiry, ping
cadence, and bounded graceful-close deadlines. A timer must not turn a rejected
write into an unbounded retry loop. Queue admission, endpoint generation, and
connection lifetime must be rechecked on every delayed attempt.

## 15. Pipe Subsystem

This section covers `src/core/pipe/*`. There are two related layers: an
operating-system pipe adapter and an in-process source/sink streaming contract.

### 15.1 `Pipe` is a move-only descriptor owner

`core::pipe::Pipe` creates an OS pipe with close-on-exec enabled by default and
owns both descriptors until ownership is explicitly released. It provides:

- explicit read- and write-descriptor release;
- explicit close operations for either end;
- conversion of the read end into an asynchronous `PipeSink`;
- conversion of the write end into an asynchronous `PipeSource`;
- a callback convenience constructor that wires both event receivers.

Before transferring a descriptor into an event receiver, the implementation
ensures nonblocking operation. Construction failures restore or close the
descriptor consistently, so ownership does not become ambiguous halfway
through setup.

### 15.2 `PipeSink` asynchronously consumes an OS read descriptor

`PipeSink` is a self-owned `ReadEventReceiver`. Its event handler reads bounded
chunks until one of the following occurs:

- the per-event byte budget is reached;
- the descriptor reports `EAGAIN`;
- end of file is observed;
- a terminal error occurs.

Interrupted reads are retried. Data, EOF, error, close, and shutdown are
reported through separate callbacks. Disabling the receiver leads through the
event-loop unobserved path, where the descriptor is closed, the close callback
is invoked, and the receiver deletes itself. Callers must not delete the
receiver or access it after its close callback.

The current defaults use bounded work per event and smaller individual read
chunks. This prevents a busy child pipe from monopolizing one event-loop
iteration.

### 15.3 `PipeSource` asynchronously drains an OS write descriptor

`PipeSource` is a self-owned `WriteEventReceiver` with a bounded accepted-data
queue. It begins suspended and is resumed when data is accepted. Its `send()`
return value is an admission result:

- `true` means the bytes were accepted into the source queue;
- `false` means the source is no longer writable or accepting the bytes would
  exceed its configured queue limit.

The source compacts consumed storage when useful, writes in bounded chunks,
and suspends again when the queue is empty. `eof()` stops new admission but
allows accepted bytes to drain before close. `close()` discards queued bytes
and disables immediately. Error, close, and shutdown callbacks are distinct.

Although this API returns a boolean rather than the socket layer's richer
`QueueResult`, the boolean is still a hard delivery boundary. codex2 must not
discard or silently claim delivery for a provider request when `send()`
returns false.

### 15.4 In-process `Source` and `Sink`

`core::pipe::Source` and `core::pipe::Sink` form a one-source/one-sink
in-process stream. A source exposes start, suspend, resume, and stop operations
and forwards data, EOF, or error into its attached sink. Disconnect is
symmetric. Destroying a sink stops its source, and trying to send without an
attached sink becomes an error rather than a successful no-op.

This abstraction is used by HTTP request/response bodies and `FileReader`.
It is not a multi-subscriber fanout mechanism. The codex2 bridge must perform
frontend fanout explicitly at the protocol-envelope level.

### 15.5 codex2 provider consequence

The stdio app-server child should be integrated with these event-loop pipe
receivers:

- app-server stdout is consumed through the asynchronous read side;
- app-server stdin is fed through the bounded write side;
- app-server stderr is captured independently and never mixed into JSON-RPC;
- EOF, write rejection, process exit, and event-loop shutdown remain distinct
  terminal causes;
- provider generation changes invalidate delayed callbacks and request routes.

This provides transport ownership and boundedness. It does not authorize the
bridge to cache or reinterpret app-server semantic state.

## 16. File Subsystem

`src/core/file/*` is intentionally small. `core::file::File` supplies descriptor
ownership, while `FileReader` adapts a regular descriptor to the in-process
pipe source contract.

### 16.1 Opening and adopting descriptors

`FileReader` supports:

- opening a path;
- opening relative to a directory descriptor with `openat`;
- adopting an already-open descriptor;
- a compatibility open callback that receives the descriptor.

The open operations reject flag combinations that require a creation mode,
such as `O_CREAT` and `O_TMPFILE`, because the API has no mode parameter.
Construction uses descriptor guards so an exception cannot leak the opened
file. Adopting transfers ownership and rejects an invalid descriptor.

`openat` has the operating system's normal semantics. It is not a filesystem
sandbox: an absolute path ignores the supplied directory descriptor, and
`AT_FDCWD` remains valid.

### 16.2 Event-loop streaming

`FileReader` is both an event receiver and a `core::pipe::Source`. It reads
sequential chunks and forwards them to the attached sink. Positive reads emit
data, EOF emits source EOF, and read failures emit source error. Starting,
suspending, resuming, and stopping are mapped to event-receiver state. The
object is self-managed and removes itself through the event-loop path.

The primary framework use is streaming files into HTTP request or response
bodies. It is not a cache, random-access abstraction, pathname security layer,
or replacement for child-process pipes.

### 16.3 codex2 use

Codex provider stdio must use the pipe subsystem, not `FileReader`. `FileReader`
is appropriate only if codex2 later needs an explicitly approved streaming
file operation. Any schema generation should remain a build-time operation and
must not create runtime schema authority inside the bridge.

## 17. HTTP Architecture

This section expands the earlier transport overview using `src/web/http/*`.
HTTP consists of incremental parsers, request/response body pipes, role-specific
socket contexts, and an upgrade handoff mechanism.

### 17.1 Incremental parser and decoder chain

The shared parser advances through begin, first-line, header, body, trailer,
finished, and error phases. Request and response parsers specialize first-line
analysis and completion behavior. Input is consumed incrementally from the
socket context, so split TCP reads are normal.

Body framing is selected by protocol metadata:

- a field decoder parses the first line and headers;
- identity decoding handles a declared content length;
- chunked decoding handles chunk sizes, payload, and trailers;
- response-side HTTP/1.0 close-delimited decoding handles bodies terminated by
  connection close.

Header names use a case-insensitive map. Trailer fields are tracked separately.
Parser configuration includes start-line, header-line, total-header, field-count,
body, and related resource limits. Unlimited values are supported in parts of
the generic API, but codex-facing listeners should retain finite limits.

### 17.2 HTTP server connection context

The server `SocketContext` owns one request parser and the per-connection
request/response sequencing state. Parsed requests are delivered as shared
request and response objects. Pending requests and the active response are
tracked so HTTP pipelining cannot serialize responses out of request order.

The response object is a `core::pipe::Sink`. It supports headers, cookies,
trailers, direct body fragments, file streaming, attached sources, chunked
transfer, completion, and protocol upgrade. Response-started,
response-completed, and request-completed transitions are distinct. Keep-alive,
shutdown, server-sent-event mode, and connection close are handled by the
connection context rather than by application code writing raw bytes.

### 17.3 HTTP client connection context

The client side mirrors this arrangement. A master request queues request
commands and streams body content while the response parser incrementally
delivers status, headers, body, and completion. Pending and delivered requests
are tracked for optional pipelining. Queue-aware send paths and `FileReader`
integration preserve asynchronous operation.

This client machinery is useful architectural evidence, but codex2 is not an
HTTP client when it talks to a stdio app-server. The protocol facade belongs
above the relevant provider endpoint, not in HTTP request classes.

### 17.4 Upgrade is a socket-context transition

An HTTP upgrade does not keep parsing HTTP while separately running a second
protocol. The upgrade factory prepares the request/response context and creates
a replacement stream socket context for the selected protocol. Factory
reference counting keeps dynamically selectable upgrade code alive while
connections still use it.

Consequences for codex2 are:

- WebSocket support must use the native HTTP upgrade path;
- detaching the HTTP context during a successful switch must not be diagnosed
  as a remote transport failure;
- the replacement WebSocket context owns subsequent framing and lifecycle;
- plain HTTP response bodies are not an alternative framing for the live
  bridge protocol unless a separate API is explicitly designed later.

## 18. WebSocket Architecture

This section expands `src/web/websocket/*`, including frame parsing,
transmission, subprotocol lifecycle, and server-side grouping.

### 18.1 Receiver state machine

The receiver incrementally parses opcode, base length, extended length,
masking key, and payload. It validates the WebSocket protocol at each boundary:

- reserved bits and unsupported opcodes are rejected;
- control frames cannot be fragmented and have a bounded payload length;
- a continuation frame requires an open fragmented message;
- a new data message cannot begin before the prior fragmented message ends;
- server receivers require client masking and client receivers reject masked
  server frames;
- extended lengths must use canonical encoding;
- configured frame size, message size, and fragment-count limits are enforced.

Payload is unmasked incrementally. Control frames can be processed while a
fragmented data message is in progress. Protocol violations and oversized
messages are differentiated before the receiver enters a terminal state.

### 18.2 Transmitter behavior

The transmitter supports complete messages and explicit start, fragment, and
end operations. It emits network-order length fields, masks client frames with
a generated key, leaves server frames unmasked, and tracks payload totals.
Control frames and close frames use the same serializer with their protocol
constraints.

Framing correctness does not itself prove transport admission. The underlying
socket source still has a finite queue, so codex2 must surface a rejected frame
instead of treating a transmitter call as guaranteed peer delivery.

### 18.3 `SubProtocol` and upgraded context

`web::websocket::SubProtocol` is the application-facing protocol adapter. It
receives connection, disconnection, signal, message-start, message-data,
message-end, and error callbacks and exposes send, ping, pong, and close
operations. Ping cadence and maximum flying pings are timer-driven.

The upgraded socket context combines the HTTP upgrade context, WebSocket
receiver/transmitter, and selected subprotocol. It maps stream reads into frame
parsing and stream writes into framed output. It also:

- attaches and detaches the subprotocol at connection transitions;
- replies to ping and accounts for pong;
- distinguishes active and passive close;
- applies a bounded close timeout;
- maps protocol errors to a close frame and read shutdown;
- installs configured frame/message/fragment limits.

### 18.4 Server groups are not a Codex coordinator

The WebSocket `GroupsManager` groups connected subprotocols and broadcasts
messages, optionally excluding the sender. This is suitable for generic
fanout, but it does not provide Codex request correlation, one-controller
authority, provider generations, or queue-aware per-recipient failure policy.
The codex2 `CodexBridge` therefore remains an application-owned coordinator.

### 18.5 codex2 WebSocket contract

The bridge envelope is carried as one WebSocket text message. JSONL newline
framing applies to raw byte-stream transports only. A fragmented WebSocket
message is reassembled by the WebSocket layer before bridge-envelope parsing;
individual WebSocket fragments are never JSON records.

TLS remains below HTTP and WebSocket. WSS is thus the same codex2 subprotocol
over an upgraded TLS stream, not a separate Codex protocol implementation.

## 19. MQTT Architecture and Transferable Lessons

This section expands the earlier MQTT discussion using `src/iot/mqtt/*` and
MQTTSuite. MQTT is valuable here because it demonstrates one protocol engine
adapted to multiple SNode.C transports without putting protocol methods on a
generic socket.

### 19.1 Layering

The runtime path is:

```text
stream SocketContext or WebSocket SubProtocol
    -> MqttContext transport adapter
    -> Mqtt protocol engine
    -> FixedHeader and packet deserializer
    -> role-specific control packet
    -> client/server callbacks and session behavior
```

`MqttContext` abstracts receiving bytes, sending bytes, obtaining connection
metadata, ending, and closing. The stream adapter maps socket reads and writes
directly. The WebSocket adapter accepts binary messages, places their payload
in the MQTT byte stream, and schedules protocol parsing while bytes remain. It
rejects WebSocket text messages because MQTT-over-WebSocket is binary.

### 19.2 Incremental typed protocol decoding

The fixed header decoder extracts packet type, required flags, and MQTT's
variable-length remaining length. It then selects a role-appropriate control
packet deserializer. Scalar and compound MQTT wire types are themselves
incremental decoders, so arbitrary transport segmentation is supported.

The implementation covers the full MQTT control-packet family: connection,
publish and all QoS acknowledgements, subscription and unsubscription,
ping/pong, and disconnect. Complete packets are delivered to the role engine;
malformed protocol closes the connection. Keepalive timers are refreshed by
protocol activity.

This reinforces the codex2 requirement that every app-server JSON-RPC message
has a typed representation while unknown JSON remains losslessly accessible.
Unlike MQTT binary types, however, app-server authority remains the original
JSON object in the envelope.

### 19.3 Client, server, session, and broker ownership

MQTT legitimately maintains protocol sessions, packet identifiers,
subscriptions, QoS flows, retained delivery behavior, and broker state because
those are MQTT protocol responsibilities. Server factories inject or share a
broker so multiple listener transports can participate in the same MQTT
domain. MQTTSuite assembles those factories through SNode.C configuration.

codex2 should copy the composition pattern, not the semantic ownership:

- one application-owned `CodexBridge` is injected into every frontend factory;
- stream and WebSocket adapters feed the same bridge protocol contract;
- the bridge coordinates connection roles and routing;
- no MQTT-like Codex session or broker cache is created because app-server is
  the Codex state authority.

### 19.4 Queue-admission difference

Some MQTT send interfaces are `void` because their protocol engine and socket
integration were designed around their own assumptions. The observed Codex
failure modes require a stricter contract: every frontend and provider send
must preserve the socket or pipe admission result, connection generation,
close phase, and diagnostic context. This is an intentional codex2 requirement,
not a reason to alter generic MQTT classes.

## 20. Network Core and Address Families

This section covers shared `src/net/*` machinery and every implemented address
family: `in`, `in6`, `un`, `l2`, and `rc`.

### 20.1 Shared typed composition

`net::SocketAddress<SockAddr>` wraps native `sockaddr` storage with its family
and effective length. Each family provides a concrete typed address and
physical socket specialization. Stream client and server templates then bind
that physical socket to:

- common connector or acceptor machinery;
- a role-specific instance configuration;
- a supplied `SocketContextFactory`;
- either the legacy byte stream or TLS connection implementation.

The application protocol context is consequently transport-independent. A
codex2 frontend context/factory should be instantiated for each desired family
instead of reimplementing bridge behavior per family.

### 20.2 Common connection configuration

`net::config::ConfigConnection` already supplies:

- read and write timeouts;
- read and write block sizes;
- maximum queued write bytes, where zero means unlimited;
- write-queue high and low watermarks;
- terminate timeout.

Final validation ensures watermarks are internally consistent and do not
exceed a finite maximum. If no explicit high watermark is set, a legacy
block-size-derived threshold is selected and clamped by the maximum. These
options belong to the SNode.C instance subtree and must not be duplicated in
the codex2 application `SubCommand`.

`ConfigInstance` also owns instance identity, enable/disable state, lifecycle
callbacks, and integration with root help, show-config, and command-line
triggers. Listener creation must honor that lifecycle rather than manually
parsing parallel options.

### 20.3 IPv4 (`net::in`)

The IPv4 family wraps `sockaddr_in` and resolves host/service data through
`getaddrinfo`. Address configuration supports host, port, numeric lookup,
reverse lookup, canonical names, and multiple resolution candidates. Stream
sockets use TCP.

Server configuration adds address and port reuse controls and TCP Nagle
control; clients expose Nagle control and optional local binding through the
shared client configuration. Convenience client/server wrappers set these
typed configuration values before connecting or listening.

For codex2, IPv4 is one frontend listener option. It does not imply a different
protocol, authority model, or bridge instance.

### 20.4 IPv6 (`net::in6`)

The IPv6 family wraps `sockaddr_in6` and parallels IPv4 host, port, candidate,
numeric, reverse, and canonical-name handling. It can request IPv4-mapped IPv6
resolution. Stream sockets use TCP.

The server additionally exposes `IPV6_V6ONLY`. This makes dual-stack behavior
an explicit configured socket property rather than an accidental platform
default. Address/port reuse and Nagle controls parallel IPv4.

codex2 must let the native instance configuration decide whether an IPv6
listener is IPv6-only or dual-stack. It must not secretly create an extra IPv4
listener or duplicate accepted connections.

### 20.5 Unix domain (`net::un`)

The Unix family wraps `sockaddr_un`. Filesystem paths and abstract namespace
addresses are represented; abstract addresses render with an `@` prefix for
diagnostics. The address implementation validates native path capacity.

Filesystem listener ownership is deliberately defensive. The physical-socket
layer uses a recognized lock marker, an exclusive lock, device/inode identity
checks, and an active-endpoint probe before removing a stale socket node. It
refuses to replace:

- a pre-existing path it cannot prove it owns;
- a non-socket filesystem object;
- an active or unverifiable endpoint;
- an object replaced between inspection and removal.

Cleanup similarly verifies identity before unlinking. This behavior is part of
the transport implementation and codex2 must not add its own unconditional
socket-path removal.

`peerCredentials(fd)` can report local UID, GID, and, where supported, PID,
with explicit success, unsupported, and error states. The architecture has no
Codex authentication layer, but peer credentials are valid local transport
telemetry and diagnostics. They must not be converted into a second semantic
authorization protocol without a separate architectural decision.

Unix sockets are the natural local production listener and remain protocol
equivalent to all other frontend transports.

### 20.6 Bluetooth L2CAP (`net::l2`)

The L2CAP family wraps `sockaddr_l2`. Its typed address contains a Bluetooth
device address and a 16-bit protocol/service multiplexer value (PSM), with the
required Bluetooth byte-order conversion. Client/server convenience wrappers
configure local and remote Bluetooth address/PSM values through the same
instance model used by other families.

L2CAP physical sockets are integrated with the common stream connector,
acceptor, connection, context-factory, legacy, and TLS templates. From the
codex2 protocol's perspective, this is another ordered connection carrying
bridge envelopes; Bluetooth-specific addressing ends below the frontend
context.

### 20.7 Bluetooth RFCOMM (`net::rc`)

RFCOMM wraps `sockaddr_rc`. Its typed address contains a Bluetooth device
address and an 8-bit channel. Client/server wrappers configure local and remote
address/channel values and reuse the common stream lifecycle.

Like L2CAP, RFCOMM has legacy and TLS compositions. codex2 must not implement a
special RFCOMM protocol handler. The same byte-stream JSONL frontend context is
instantiated over the selected RFCOMM socket type.

### 20.8 Legacy and TLS variants

Every family composes with the common legacy stream implementation, and the
source tree also provides TLS aliases/configuration for the family wrappers.
TLS changes connection establishment, encryption, certificate policy, and
close behavior below the application protocol. It does not change the bridge
envelope or create a new controller domain.

WebSocket listeners are built above HTTP upgrade on an appropriate underlying
legacy or TLS stream. The complete transport matrix is therefore composition,
not duplicated codex2 logic:

```text
address family physical socket
    -> legacy or TLS stream connection
    -> raw JSONL context

address family physical socket
    -> legacy or TLS stream connection
    -> HTTP parser and upgrade
    -> WebSocket context
    -> codex2 WebSocket subprotocol
```

### 20.9 One bridge across listeners

All configured listener factories receive the same application-owned
`CodexBridge`. The bridge assigns a unique connection identity independent of
file descriptor and address family. Controller selection, observer fanout,
request routing, provider generation, and telemetry therefore remain coherent
when clients arrive through different listener types.

Transport configuration and protocol authority remain separate:

- SNode.C owns addresses, socket options, TLS, queues, timeouts, and connection
  lifecycle;
- frontend contexts own envelope framing and bounded protocol validation;
- `CodexBridge` owns ephemeral multi-client routing;
- app-server owns Codex threads, turns, items, requests, and retained history.

## 21. Consolidated Implementation Consequences

The detailed subsystem study narrows the codex2 implementation as follows:

1. Use SNode.C timers only for bounded asynchronous lifecycle operations and
   revalidate generation/lifetime in every callback.

2. Spawned app-server stdio must use nonblocking `core::pipe` endpoints with
   independent stdout, stdin, and stderr handling. A rejected provider write is
   terminal for that request unless an explicitly bounded retry can still
   prove the same live generation.

3. Use the native stream `SocketContextFactory` pattern for raw JSONL clients
   and the native HTTP-upgrade `SubProtocol` pattern for WebSocket clients.

4. Instantiate that protocol layer over all selected `net::in`, `net::in6`,
   `net::un`, `net::l2`, and `net::rc` legacy/TLS endpoints without copying
   protocol logic.

5. Use SNode.C instance configuration for all address, socket, queue, timeout,
   TLS, HTTP, and WebSocket semantics. Application configuration adds only
   semantics that do not already exist in those subtrees.

6. Preserve queue admission and close cause at every pipe, stream, HTTP
   upgrade, and WebSocket boundary. Transport adapters cannot return apparent
   success after dropping an envelope.

7. Inject one application-owned `CodexBridge` into every frontend factory.
   Generic WebSocket groups and MQTT broker/session machinery do not replace
   controller-aware routing.

8. Keep the complete generated app-server type facade and mandatory raw JSON
   access above the provider protocol endpoint. Do not put Codex methods on
   generic socket, pipe, HTTP, or WebSocket classes.

9. Do not add a semantic state cache, AISuite snapshot, reconstructed history,
   or frontend SDK state. Recovery remains native app-server queries.

10. Preserve transport-specific telemetry such as peer address, Unix peer
    credentials, TLS state, queue accounting, lifecycle phase, and close cause
    outside the native app-server payload.
