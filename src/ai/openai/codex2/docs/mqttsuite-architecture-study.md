# MQTTSuite Architecture Study for codex2

## Purpose and Status

This report is the MQTTSuite companion to
`snodec-architecture-study.md`. It records a detailed source study of how
MQTTSuite embeds the SNode.C `iot::mqtt` subframework in real applications,
with special attention to:

- `mqttbroker`;
- `mqttintegrator`;
- `mqttbridge`;
- `mqttcli`;
- `mqttstore`;
- raw MQTT over SNode.C stream connections;
- MQTT over HTTP-upgraded WebSocket connections;
- application configuration, factories, callbacks, lifecycle, persistence,
  and shared coordinators.

This is an architecture report, not an implementation change. The local
MQTTSuite repository was inspected read-only.

The local MQTTSuite worktree had five pre-existing CMake modifications. Each
changes only the required SNode.C package version from `1.0.0` to `2.0.0` in
one application. Those edits were inspected but not modified, staged, or
committed by this study.

The corresponding SNode.C source is always the current `master`/HEAD. The
implemented codex2 build and CI use that branch directly.

## Executive Conclusions

MQTTSuite demonstrates that SNode.C's internal template and CMake composition
produces a deliberately small application-facing API. An application developer
primarily implements or selects a `SocketContextFactory`, supplies the concrete
application `SocketContext`, and uses `SocketConnection` only when direct
transport interaction is needed. WebSocket integration adds the parallel
`SubProtocolFactory` and `SubProtocol` classes. The practical learning surface
is these objects, their callbacks, and their public methods; the framework
carries the address-family and protocol-stack composition underneath. Ease of
application use, rather than internal implementation simplicity, is the
fulfilled design goal.

1. MQTTSuite validates the intended SNode.C protocol integration model. An
   application provides a concrete subclass of `iot::mqtt::client::Mqtt` or
   `iot::mqtt::server::Mqtt`; a transport adapter owns it; and a factory creates
   one adapter per connection.

2. Raw stream and WebSocket operation reuse the same application MQTT class.
   The stream path wraps it in `iot::mqtt::SocketContext`; the WebSocket path
   wraps it in `iot::mqtt::{client,server}::SubProtocol`. Application MQTT
   semantics do not depend on IPv4, IPv6, Unix, TLS, HTTP, or WebSocket.

3. Network configuration remains instance-owned. Each application composes
   native SNode.C client/server instances and adds only its own MQTT or domain
   `SubCommand` sections.

4. A `SocketContextFactory` or WebSocket `SubProtocolFactory` is intentionally
   small. It reads configuration from the connection instance, constructs the
   application protocol object, and transfers it to the SNode.C adapter.

5. `mqttbroker` demonstrates a shared application-owned coordinator injected
   into every accepted connection. Raw and WebSocket clients participate in
   the same broker because their factories obtain the same broker instance.

6. `mqttintegrator` demonstrates one shared configuration/mapping service used
   by multiple simultaneous transport instances and a deliberate hot-reload
   versus reconnect decision.

7. `mqttbridge` demonstrates dynamic construction of many heterogeneous
   outbound connections from validated domain configuration and graceful
   replacement after all old flow controllers finish. Its semantic bridge
   store is MQTT-specific and is not a template for caching Codex state.

8. `mqttcli` demonstrates how application sections can be attached beneath
   every native network instance while preserving one common protocol class.

9. `mqttstore` demonstrates callback-driven integration with another
   asynchronous subsystem: MQTT publish callbacks produce storage operations,
   while transport/session and database configuration remain separate.

10. The principal codex2 lesson is the shape of composition, not reuse of MQTT
    domain state. codex2 should copy factories, adapters, shared-coordinator
    injection, and configuration placement. It should not copy MQTT sessions,
    broker trees, retained messages, mapping history, or SSE replay as Codex
    authority.

## 1. Repository and Build Composition

### 1.1 Top-level products

The top-level CMake project builds a shared support library and five products:

```text
lib
mqttbroker
mqttintegrator
mqttbridge
mqttcli
mqttstore
```

Every product has a similar physical layout:

```text
application main
SocketContextFactory.{h,cpp}
lib/Mqtt.{h,cpp}
lib/CMakeLists.txt
websocket/SubProtocolFactory.{h,cpp}
websocket/CMakeLists.txt
```

Additional domain classes live beside `lib/Mqtt` where required. This layout
keeps the application protocol behavior in a reusable shared library while the
executable is responsible for endpoint composition and startup.

### 1.2 Feature-controlled transport dependencies

Each application has CMake options for IPv4, IPv6, Unix domain, TLS variants,
WebSocket, and WSS. Enabled features contribute only the corresponding SNode.C
components to `find_package` and link dependencies. Examples include:

- `net-in-stream-legacy` and `net-in-stream-tls`;
- `net-in6-stream-legacy` and `net-in6-stream-tls`;
- `net-un-stream-legacy` and `net-un-stream-tls`;
- `http-client` for WebSocket clients;
- Express HTTP server components for server dashboards and upgrades;
- `mqtt-client`, `mqtt-server`, or their WebSocket variants.

This is compile-time availability combined with runtime SNode.C instance
configuration. It avoids linking every possible transport into every binary.
The template and CMake complexity is therefore functional modularity: it is the
mechanism that keeps each resulting application slim while preserving a common
architecture. Its cost is discoverability for contributors, not failure of the
slim design.

### 1.3 WebSocket subprotocol libraries

Each application's WebSocket directory builds a shared subprotocol library.
The exported C entry point returns the role-correct base factory type:

```text
mqttClientSubProtocolFactory()
mqttServerSubProtocolFactory()
```

Client applications install an app-specific library whose output name is
`snodec-websocket-mqtt-client`; the broker installs
`snodec-websocket-mqtt-server`. Install directories distinguish applications
whose output names are otherwise equal.

The code also supports static-link configurations through guarded factory and
selector includes. Dynamic and static selection therefore change deployment,
not application MQTT behavior.

## 2. SNode.C MQTT Integration Contract

### 2.1 Protocol engine and transport adapter are separate

`iot::mqtt::Mqtt` is the protocol engine. It owns:

- incremental fixed-header decoding;
- role-specific control-packet deserializer selection;
- packet delivery;
- packet serialization;
- MQTT session and QoS state;
- packet identifier allocation;
- keepalive handling;
- protocol/session lifecycle callbacks.

`iot::mqtt::MqttContext` is the transport contract. It supplies:

- `recv()` and `send()` byte operations;
- access to the underlying `SocketConnection`;
- graceful end and protocol-error close;
- connected, received, disconnected, and signal forwarding.

The context constructor links the protocol engine back to the context. The
context destructor deletes the application MQTT object. Factory code therefore
allocates the application MQTT object and transfers ownership exactly once.

### 2.2 Incremental MQTT decoding

Incoming bytes first feed `FixedHeader`. Once complete, the role-specific MQTT
implementation creates the correct control-packet deserializer. That object
continues incrementally until the packet is complete or malformed.

On completion the packet is delivered, the deserializer is deleted, parser
state returns to the fixed header, and the keepalive timer is restarted. An
unknown packet type, malformed fixed header, malformed control packet, or
invalid QoS transition closes through `MqttContext`.

The segmentation of TCP reads or WebSocket payload buffers is therefore not
visible to application callbacks. Applications receive typed complete packets
such as `Connect`, `Connack`, `Publish`, `Subscribe`, and acknowledgements.

### 2.3 Session and QoS behavior

The base engine persists sender and receiver QoS state in `iot::mqtt::Session`:

- outgoing publishes awaiting acknowledgement;
- PUBREL identifiers awaiting PUBCOMP;
- inbound QoS 2 publishes awaiting PUBREL;
- completed inbound QoS 2 identifiers needed for duplicate suppression.

Session initialization resends outstanding protocol state. QoS 1 sends
PUBACK. QoS 2 records and suppresses duplicate delivery across the
PUBLISH/PUBREC/PUBREL/PUBCOMP flow. Packet identifiers avoid outstanding IDs.

After MQTT session initialization, the generic socket timeout is set to zero
and MQTT's keepalive timer becomes the protocol liveness mechanism. The server
uses the MQTT 1.5 multiplier before closing an expired connection.

This state is correct in the MQTT layer because QoS and sessions are defined by
MQTT. It does not justify a second semantic state authority in codex2.

## 3. Raw Stream Integration

### 3.1 Factory shape

For a raw stream, each application defines a small subclass of
`core::socket::stream::SocketContextFactory` and overrides:

```cpp
core::socket::stream::SocketContext* create(
    core::socket::stream::SocketConnection* socketConnection);
```

The implementation reads application sections through
`socketConnection->getConfigInstance()` or shared application configuration,
constructs its concrete `lib::Mqtt`, then returns:

```cpp
new iot::mqtt::SocketContext(socketConnection, new ApplicationMqtt(...));
```

No MQTT operation is added to `SocketConnection`. The generic connection stays
transport-neutral.

### 3.2 Stream adapter behavior

`iot::mqtt::SocketContext` combines SNode.C's stream `SocketContext` with
`MqttContext`:

- `readFromPeer()` implements MQTT receive;
- `sendToPeer()` implements MQTT send;
- `shutdownWrite()` implements graceful MQTT end;
- stream close implements protocol-error close;
- socket lifecycle callbacks forward to the MQTT engine.

The same factory can be instantiated by IPv4, IPv6, and Unix client/server
templates and by their legacy or TLS connection implementations.

### 3.3 Application endpoint construction

Applications use `core::socket::stream::Client` or the family-specific
`Server` helper. The configurator sets defaults such as port, Nagle control,
retry base, reconnect, local/remote address, and disabled state. Application
sections are attached to the resulting `ConfigInstance` while each network
instance is constructed. In MQTTSuite this commonly occurs after
`SNodeC::init()` has performed its initial root parse but before
`SNodeC::start()` performs the final configuration bootstrap. Shared
application-wide subcommands needed during construction are instead registered
before `init()`.

The returned client handle is retained only when the application needs its
flow controller or other explicit control. Runtime connection ownership stays
inside SNode.C.

## 4. WebSocket Integration

### 4.1 HTTP client upgrade path

MQTT client applications instantiate a SNode.C HTTP client instead of a raw
socket client. On an HTTP session they:

1. read the configured WebSocket target, normally `/ws`;
2. set `Sec-WebSocket-Protocol: mqtt`;
3. request an upgrade to `websocket`;
4. observe upgrade start, response, parse error, and session-end callbacks;
5. let the selected WebSocket subprotocol factory create MQTT after the
   context switch.

Retry and reconnect belong to the HTTP client's native network configuration.
WSS uses the TLS HTTP client with the same upgrade and MQTT subprotocol logic.

### 4.2 Server upgrade path

`mqttbroker` exposes Express HTTP routes. Its `/ws` route checks that the
requested subprotocol contains `mqtt`, then calls `Response::upgrade`. The
server WebSocket selector creates the upgraded context and invokes the MQTT
server subprotocol factory. Unsupported subprotocols receive an HTTP error.

The HTTP parser stops being the data protocol after upgrade. MQTT bytes are
not carried as HTTP request bodies.

### 4.3 MQTT WebSocket adapter

The shared `iot::mqtt::SubProtocol<WSRole>` combines a client or server
WebSocket subprotocol with `MqttContext`.

It enforces the MQTT-over-WebSocket data contract:

- text messages are rejected with a protocol close;
- binary message fragments are accumulated by WebSocket callbacks;
- on message completion, bytes are appended to an MQTT input buffer;
- a dedicated event receiver schedules incremental MQTT consumption;
- if bytes remain after one decoder pass, the receiver is spanned again;
- outgoing MQTT packets become WebSocket binary messages;
- MQTT end becomes WebSocket close;
- MQTT protocol error becomes close code 1002.

The MQTT engine sees a byte source in both raw-stream and WebSocket modes. The
adapter is the only layer aware that a completed WebSocket message supplied
those bytes.

### 4.4 Factory parity

Every application WebSocket factory mirrors its raw factory:

```cpp
new iot::mqtt::client::SubProtocol(
    subProtocolContext,
    "mqtt",
    new ApplicationMqtt(...));
```

The broker uses the server role. Configuration arguments and shared domain
objects are intentionally equivalent between the two factories. This parity is
the clearest reusable pattern for codex2 stream and WebSocket frontends.

## 5. Shared MQTTSuite Mapping Services

### 5.1 `ConfigApplication`

`mqtt::lib::ConfigApplication` is a `utils::SubCommand`. It owns:

- a shared `MqttMapper`;
- a mapping-file option;
- an MQTT session-store option;
- load, replace, retrieve, and persist operations.

`ConfigMqttBroker` and `ConfigMqttIntegrator` specialize it. The mapper is
shared with every protocol instance created by the application's factories.

### 5.2 `MqttMapper`

The mapper validates JSON against an embedded schema and applies schema
defaults as a JSON patch. It separates connection settings from topic mapping
rules and reports whether replacing a mapping changed connection parameters,
which determines whether active clients must reconnect.

Mapping can:

- derive subscriptions from a topic-level tree;
- match input publishes by MQTT topic levels;
- produce static mapped publishes;
- render topic and message templates through inja;
- emit immediate or timer-delayed publishes;
- load shared libraries that register inja value or void callbacks.

Plugin handles are closed only after the inja environment using their
callbacks is destroyed. Replacing a mapping rebuilds the environment and
plugin set.

### 5.3 Administrative configuration lifecycle

`MappingAdminRouter` provides authenticated Express endpoints for schema,
active configuration, draft patch/replace, validation, deployment, rollback,
and version history. Draft deployment:

- stamps metadata;
- preserves up to 50 timestamped prior active files;
- validates against the current schema;
- installs the mapping in the shared mapper;
- persists it;
- invokes an application reload callback.

This is application-domain configuration management. It is useful evidence for
`SubCommand` placement and callback coordination, but it is unrelated to Codex
thread/history recovery.

## 6. `mqttbroker`

### 6.1 Role and shared broker

`mqttbroker` is an MQTT server plus HTTP administration/dashboard service. It
creates one shared `iot::mqtt::server::broker::Broker` using the configured
maximum QoS and session-store file. The same broker is supplied to raw stream
factories and obtained by WebSocket factories, so clients on all listeners
share subscriptions, retained messages, and sessions.

Raw listeners include enabled IPv4, IPv6, and Unix legacy/TLS combinations.
HTTP/HTTPS listeners over the same families expose WebSocket MQTT plus REST,
SSE, and static dashboard content.

### 6.2 Per-connection server class

`mqttbroker::lib::Mqtt` subclasses `iot::mqtt::server::Mqtt`. It adds:

- connection registration in `MqttModel`;
- dashboard notifications for connect/disconnect and subscription changes;
- mapper-generated immediate publishes;
- timer-scheduled mapped publishes;
- administrative subscribe/unsubscribe methods.

Delayed publishes use a stable priority queue ordered by absolute due time and
insertion sequence. One single-shot timer is armed for the earliest item. Due
publishes enter the shared broker and are recursively observed by the model and
mapping path.

### 6.3 Broker authority and persistence

The SNode.C broker owns MQTT domain authority:

- active and retained client sessions;
- subscription tree;
- retained-message tree;
- queued session delivery;
- MQTT session persistence on shutdown/startup.

`Broker::instance()` returns a process-wide shared broker. On startup it loads
the configured JSON session store and removes the consumed store file. On
destruction it writes current sessions and trees back.

### 6.4 HTTP and SSE model

`MqttModel` tracks active application MQTT objects for administrative actions
and publishes dashboard events. A fresh SSE response receives initialization,
current clients, subscriptions, and retained messages. Weak response pointers
and disconnect callbacks remove receivers. A periodic timer emits SSE
keepalive comments.

REST routes can disconnect a client, subscribe or unsubscribe it, and release
a retained message. These operations deliberately call broker/application
domain APIs rather than writing MQTT bytes manually.

### 6.5 codex2 lesson

Transferable: inject one shared coordinator into every listener factory and
keep per-connection objects small. Not transferable: broker session trees,
retained messages, dashboard reconstruction, and process-wide semantic state.

## 7. `mqttintegrator`

### 7.1 Role

`mqttintegrator` is an MQTT client that subscribes according to a mapping,
transforms received publishes, and publishes immediate or delayed results. It
can connect simultaneously through raw, TLS, WebSocket, and WSS clients over
IPv4, IPv6, or Unix sockets.

### 7.2 Per-connection protocol behavior

Its `lib::Mqtt` subclasses `iot::mqtt::client::Mqtt`. Construction derives
client ID, keepalive, connection payload, initial subscriptions, and session
store from the shared mapper/configuration.

On transport connection it sends CONNECT. On accepted CONNACK without a
present session it subscribes. On PUBLISH it obtains immediate and scheduled
mapped publishes. A priority queue and single-shot timer dispatch delayed
results.

### 7.3 Live mapping changes

All live integrator MQTT instances register in a static set. A deployed mapping
calls `updateSubscriptions(mustReconnect)`:

- if connection settings changed, each instance sends DISCONNECT and native
  reconnect policy establishes a new session;
- otherwise the instance computes old/new subscription differences, sends
  only required SUBSCRIBE and UNSUBSCRIBE packets, and replaces its local
  subscription list.

The result reports reload mode, instance count, subscribed count, and
unsubscribed count through the admin API.

### 7.4 codex2 lesson

Transferable: distinguish configuration changes that can update a live
protocol object from changes requiring a controlled reconnect, and use native
client reconnect. Not transferable: a global set of semantic clients as an
authoritative Codex state store.

## 8. `mqttbridge`

### 8.1 Role and configuration model

`mqttbridge` connects to multiple external brokers and forwards publishes
between members of a named bridge. `ConfigBridge`, a `utils::SubCommand`, owns
the JSON definition file and dashboard directory. `BridgeStore` validates the
definition against an embedded schema and materializes:

- named bridges;
- bridge-level topic prefixes and disabled state;
- broker endpoints;
- transport, address family, encryption, and address;
- MQTT session/connect options;
- subscriptions, broker prefixes, and loop-prevention options.

The full SNode.C instance name combines bridge and broker identity and becomes
the lookup key used by both raw and WebSocket factories.

### 8.2 Dynamic endpoint construction

`startBridges()` walks the validated bridge model. According to each broker's
transport/protocol/encryption tuple, it creates the matching SNode.C client:

- raw IPv4, IPv6, or Unix;
- TLS IPv4, IPv6, or Unix;
- WebSocket over plain HTTP for those families;
- WebSocket over TLS for those families.

Compile-time feature guards reject unsupported combinations. Definition-owned
instance and remote address sections are marked non-configurable so command
line input cannot contradict the validated topology. Generic connection
settings still use the native client configuration.

### 8.3 Per-broker MQTT behavior

The application `Mqtt` obtains its `Broker` definition from `BridgeStore`,
sends CONNECT from those fields, subscribes after successful CONNACK, and
registers with the owning `Bridge`. On incoming PUBLISH, the bridge forwards to
every connected destination except the origin. The outgoing topic is composed
from bridge prefix, origin prefix, destination prefix, and original topic.

The no-reflection rule prevents immediate origin echo. MQTT's optional
loop-prevention connect extension addresses wider bridge loops.

### 8.4 Staged replacement and flow control

Configuration patching creates a validated staged JSON document. Activation
does not overwrite active connections immediately. `closeBridges()`:

- marks a restart pending;
- emits lifecycle SSE events;
- asks MQTT clients to disconnect;
- terminates every registered `ClientFlowController`;
- waits for all flow-completed callbacks.

Only after the flow-controller map is empty does it activate staged data,
construct replacement clients, and reparse configuration. This is a concrete
example of coordinating asynchronous teardown before replacing an object graph.

### 8.5 SSE telemetry

`SSEDistributor` emits bridge/broker starting, connected, disconnecting,
stopped, and disabled events. It retains lifecycle events for replay to a fresh
dashboard and uses weak response pointers plus heartbeat timers.

This replay is dashboard telemetry owned by mqttbridge. It is not equivalent
to app-server authoritative thread recovery and should not be copied as a
Codex snapshot.

### 8.6 codex2 lesson

Transferable: validated topology, protocol-neutral client selection, instance
identity lookup, shared coordinator injection, and flow-completion barriers.
Not transferable: semantic publish routing, topic prefixing, staged Codex state,
or SSE replay as protocol authority.

## 9. `mqttcli`

### 9.1 Role and configuration

`mqttcli` is a configurable publisher/subscriber client. Every enabled SNode.C
network instance receives three application subcommands:

- `session`: client ID, default QoS, retained session, keepalive, will,
  username, and password;
- `sub`: one or more subscription topic specifications;
- `pub`: topic, message, and retain flag.

A require callback enforces that an enabled instance performs at least one of
publish or subscribe. The WebSocket variant adds a configurable HTTP target.
Transport, remote address, TLS, retry, reconnect, queue, and timeout options
remain native instance configuration.

All compiled instances start disabled, allowing configuration to select the
desired transport explicitly.

### 9.2 Runtime behavior

The raw and WebSocket factories read identical instance application sections
and construct the same `mqttcli::lib::Mqtt`. On connection it sends CONNECT.
After successful CONNACK it:

- parses optional `##qos` suffixes on subscription topics;
- sends subscriptions;
- parses an optional `##qos` suffix on the publish topic;
- publishes the configured message;
- disconnects immediately for work that needs no acknowledgement;
- waits for PUBACK or PUBCOMP before disconnecting when required;
- remains connected while subscriptions are active.

Incoming publishes are formatted for terminal output; protocol acknowledgements
are still handled by the base MQTT engine.

### 9.3 codex2 lesson

Transferable: attach protocol-specific `SubCommand` sections to each native
connection instance and have both transport factories read the same typed
configuration. The facade/client behavior stays outside generic socket classes.

## 10. `mqttstore`

### 10.1 Role and configuration tree

`mqttstore` subscribes to MQTT topics and writes every incoming publish to
MariaDB. Each network instance receives:

- `session` for MQTT connection/session fields and session-store path;
- `sub` for topic filters;
- `db` for MariaDB host, port, Unix socket, database, user, password, and flags;
- nested `storage` for raw table, automatic raw-table creation, and optional
  projection file.

Raw and WebSocket factories read the same tree. They load and validate a
`StoragePlan`, construct the same application MQTT class, and differ only in
whether ownership is transferred to `SocketContext` or client `SubProtocol`.
Startup exceptions are reported and fail the affected construction.

### 10.2 MQTT behavior

After connection the client sends CONNECT. A rejected CONNACK or missing
subscription configuration causes DISCONNECT. Accepted connections parse topic
`##qos` suffixes and subscribe. Each typed PUBLISH callback creates an
`MqttMessage` containing source connection, topic, original payload, QoS,
retain/duplicate flags, and packet identifier, then calls storage.

### 10.3 Raw storage

`MariaDbStorage` uses the asynchronous SNode.C MariaDB client. It can ensure a
configured raw table exists and inserts:

- receive timestamp;
- source instance;
- topic and MQTT delivery fields;
- original payload bytes;
- safe text form where applicable;
- parsed JSON where applicable;
- payload classification as JSON, text, or binary.

SQL identifiers are validated before quoting. Values are SQL-escaped or
serialized by type. Database errors are reported through asynchronous result
callbacks.

### 10.4 Typed projections

`StoragePlan` validates a projection JSON document against an embedded schema.
Projections match MQTT topic filters and map columns from literals, topic
levels, or JSON Pointers. Projection tables are not automatically created;
they remain externally managed domain schemas. Only valid JSON payloads enter
projection inserts, while raw storage remains independent.

### 10.5 codex2 lesson

Transferable: a typed protocol callback may dispatch into another asynchronous
SNode.C subsystem without polluting the transport adapter. Not transferable:
database persistence in the bridge. codex2 must leave retained Codex data in
app-server and expose it through native queries.

## 11. Cross-Application Comparison

| Application | MQTT role | Shared object | Raw adapter | WebSocket adapter | Domain persistence/state |
| --- | --- | --- | --- | --- | --- |
| `mqttbroker` | Server | Shared broker and mapper | Server `SocketContext` | Server `SubProtocol` | Sessions, subscriptions, retained messages, dashboard model |
| `mqttintegrator` | Client | Shared mapper | Client `SocketContext` | Client `SubProtocol` | Mapping files, versions, optional MQTT session |
| `mqttbridge` | Client per external broker | Bridge store and bridge coordinators | Client `SocketContext` | Client `SubProtocol` | Validated topology, MQTT sessions, SSE lifecycle replay |
| `mqttcli` | Client | Instance configuration | Client `SocketContext` | Client `SubProtocol` | Optional MQTT session only |
| `mqttstore` | Client | Instance configuration and storage plan | Client `SocketContext` | Client `SubProtocol` | MQTT session and MariaDB messages/projections |

Across all five applications, the stable invariant is:

```text
native SNode.C connection
    -> transport-specific MQTT context
    -> application-specific MQTT subclass
    -> typed MQTT callbacks
    -> application domain object
```

## 12. Detailed codex2 Guidance

### 12.1 Patterns to adopt

1. Create one thin codex2 protocol adapter per provider or frontend endpoint,
   while keeping one application-owned `CodexBridge` backend SDK.

2. Create thin raw-stream and WebSocket adapters that own that object and map
   transport lifecycle into one common protocol contract.

3. Keep generic `SocketConnection`, HTTP, and WebSocket classes free of Codex
   methods.

4. Make raw and WebSocket factories consume the same Codex-specific
   configuration and coordinator references.

5. Inject the single application-owned `CodexBridge` into every frontend
   stream context. For WebSocket listeners, keep the statically linked
   subprotocol factory stateless and pass the bridge through a scoped,
   connection-validated binding around synchronous `Response::upgrade()`.
   Do not turn the bridge into a singleton merely because the selector's
   factory callback is context-free.

6. Use native SNode.C instance configuration for address families, TLS,
   reconnect, retry, queue limits, timeouts, HTTP, and WebSocket behavior.

7. Use explicit instance and connection identities to correlate callbacks and
   coordinator registrations across transport types.

8. Treat context replacement during HTTP upgrade as an adapter transition,
   not an application disconnect.

9. Wait for asynchronous flow completion before replacing a live endpoint
   graph when configuration truly requires replacement.

10. Keep application telemetry separate from native app-server payloads.

### 12.2 Patterns not to adopt

1. Do not create a Codex equivalent of MQTT `Session`, `Broker`,
   `SubscriptionTree`, or `RetainTree` in AISuite.

2. Do not use mqttbridge's staged JSON model as a retained Codex thread model.

3. Do not use SSE event replay as a substitute for native app-server
   `thread/list` or `thread/read` recovery.

4. Do not persist app-server requests, responses, turns, items, command output,
   approvals, or pending state in the bridge.

5. Do not infer send success merely because a protocol serializer or
   WebSocket convenience method returned. codex2 still requires explicit
   bounded queue admission and failure diagnostics at every delivery boundary.

6. Do not copy process-wide singletons unless the lifetime is truly one per
   application and explicit dependency injection cannot express it cleanly.

### 12.3 Recommended codex2 object correspondence

```text
MQTTSuite                           codex2
--------------------------------   ---------------------------------------
application lib::Mqtt              Codex protocol endpoint adapter
MqttContext                        common endpoint transport contract
SocketContext                      JSONL stream frontend/provider adapter
client/server SubProtocol          Codex WebSocket frontend/provider adapter
SocketContextFactory               Codex stream context factory
SubProtocolFactory                 Codex WebSocket subprotocol factory
shared Broker injected in factory  application-owned CodexBridge SDK reference
typed packet callbacks             typed app-server JSON-RPC callbacks
raw packet serializer              mandatory raw app-server JSON path
native client instance config      native codex endpoint instance config
```

The correspondence ends at composition. MQTT broker/session semantics are not
Codex bridge semantics.

## 13. Final Assessment

MQTTSuite is strong evidence that the SNode.C architecture supports the slim
codex2 design with a stateless frontend proxy SDK rather than another stateful
frontend projection layer. It already demonstrates:

- one protocol engine over raw, TLS, WebSocket, and WSS transports;
- IPv4, IPv6, and Unix endpoint composition;
- client and server roles;
- per-connection factories and callbacks;
- shared application coordinators;
- typed protocol events;
- dynamic or static WebSocket subprotocol deployment;
- native configuration hierarchies;
- graceful asynchronous reconfiguration.

The correct adaptation is narrower than MQTTSuite's applications: codex2 needs
transport adapters, typed/raw app-server access, multi-client registration,
one-controller routing, and telemetry. It must continue to query app-server for
Codex authority and must not grow MQTT-like semantic storage merely because the
framework makes such state possible.

Against the codex2-relevant criteria of modular transport composition,
protocol/transport separation, callback-driven lifecycle consistency, and a
small application-facing learning surface, this study assesses the underlying
SNode.C architecture at **9.5/10**. MQTTSuite supplies concrete evidence for
that assessment by implementing several materially different applications over
the same small factory/context/subprotocol boundary.

The rating is deliberately scoped; it is not a claim about every aspect of
framework quality or application maturity. The remaining cost appears when
developers extend the framework itself, where template navigation, CMake
feature composition, and compiler diagnostics demand deeper knowledge. That is
a documentation and internal-development burden rather than a failure of the
application-facing design.
