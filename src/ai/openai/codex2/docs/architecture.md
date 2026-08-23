# codex2 / codex-bridge Architecture

## Goal

`codex2` is a new slim Codex integration for AISuite. It returns to the original SNode.C idea: make AI systems easy to access from SNode.C applications through transports, framing, typed facades, and simple routing.

The existing `ai::openai::codex` implementation remains on disk as reference material, but is disabled from the default CMake build on this branch. The new implementation starts from scratch under `src/ai/openai/codex2`, with the application entry point under `src/apps/codex-bridge`.

## Runtime Shape

```text
CodexUI / CodexWebUI / SNode.C apps
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

## SNode.C Shape

The bridge should follow the existing SNode.C protocol style used by protocol implementations such as MQTT:

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
```

`CodexBridge` is the shared intermediary. It registers frontend clients, tracks controller/observer roles, routes client messages to app-server, fans out app-server messages to clients, and emits bridge-level telemetry.

## Bridge Protocol

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
  "kind": "bridge.diagnostic",
  "connectionId": "frontend-3",
  "frameBytes": 4812,
  "queueDepth": 2
}
```

Unknown app-server methods, fields, events, and result members must pass through without loss.

## AISuite-Owned Responsibilities

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

## Explicit Non-Goals

`codex2` must not reintroduce Codex semantic authority:

- no semantic cache
- no frontend `State`
- no AISuite snapshot authority
- no timeline reducer
- no command-history reconstruction
- no pending-request truth
- no merge/replace semantics for thread items
- no interpretation of omitted app-server fields

Recovery is performed by querying app-server again, for example through `thread/read`, not from AISuite-retained semantic state.

## Controller Model

Only one frontend client is authoritative at a time.

- the controller may submit turns, steer, and respond to app-server server-requests
- observers receive events and may optionally perform allowed read-only requests
- controller handoff is explicit
- selected-thread auto-switching is not a bridge policy
- mutating observer requests are rejected or ignored with diagnostics

## Transport Model

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

Stdio is a single ordered stream. App-server Unix/WebSocket modes can accept multiple clients, but `codex-bridge` owns the intended frontend multi-client controller/observer policy.

## SDK / Facade

Generic SNode.C socket classes must not be polluted with Codex-specific methods. Use Codex-specific wrapper/facade objects around SNode.C transport and protocol contexts.

```cpp
SocketClient socket(...);
CodexBridgeClient client(socket);
```

Typed facade methods are mandatory:

```cpp
client.threadList([](ThreadList& result) {
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
client.sendRawJson(json);
client.onRawJson(...);
```

Every typed result/event object wraps native app-server JSON, exposes `getRaw()`, provides typed accessors, preserves unknown fields, has `operator bool()` for success/error state, and does not cache or reconstruct broader Codex state.

## Configuration

Use the existing SNode.C configuration subsystem whenever command-line options are required.

Each SNode.C server/client instance already has a broad set of command-line options. Do not duplicate existing transport semantics in `codex-bridge`.

Valid Codex-specific configuration includes:

- app-server executable path
- app-server listen mode / provider transport
- child-specific `CODEX_HOME`
- controller policy
- bridge protocol limits
- telemetry/log verbosity
- optional app-server restart policy

Do not duplicate existing SNode.C instance semantics such as Unix path binding, IPv4/IPv6 binding, TLS certificate/key handling, RFCOMM adapter/channel handling, or WebSocket transport configuration.

## Initial Implementation Scope

The initial branch prepares the new architecture only:

- disable legacy Codex CMake targets by default
- disable all existing tests in CMake/CI for the bootstrap phase
- add `src/ai/openai/codex2`
- add `src/apps/codex-bridge`
- add this architecture contract
- do not delete old files
- do not modify the OpenAI app-server
- do not implement tests initially

## Build Directory

All incremental local development builds for this branch must use the canonical build directory:

```text
/home/voc/projects/drafts/AISuite-extraction/build/codex-build
```

Do not create ad hoc sibling build directories for ordinary codex2/codex-bridge iteration unless a task explicitly requires an isolated disposable build.
