# `@snodec/codex-frontend`

Framework-neutral TypeScript frontend SDK for the AISuite `codex-bridge`.

The generated protocol declarations come from the same pinned Codex schema
and Rust operation bindings as AISuite's C++ views. Regenerate both outputs in
one invocation:

```sh
node tools/generate-codex-protocol.mjs \
  /path/to/codex_app_server_protocol.schemas.json \
  /path/to/app-server-protocol/src/protocol/common.rs \
  src/ai/openai/codex/protocol/generated/ProtocolTypes.h \
  src/ai/openai/codex/protocol/generated/manifest.json \
  packages/codex-frontend/src/protocol/generated.ts
```

`npm test --prefix packages/codex-frontend` builds the declarations and proves
that the checked-in C++ and TypeScript type names, operation bindings,
required-parameter flags, counts, and source hashes remain equal.

## Frontend proxy

`CodexBridgeClient` mirrors the observable routing and lifecycle contract of
AISuite's C++ frontend proxy while exposing generated method typing:

```ts
import {CodexBridgeClient} from "@snodec/codex-frontend";

const client = new CodexBridgeClient((message) => transport.send(message));

client.onServerNotification("thread/started", (notification) => {
    console.log(notification.params?.thread);
});

const response = await client.requestPromise("thread/list", {
    limit: 20,
    archived: false,
});
```

The proxy retains only pending JSON-RPC callbacks and bridge connection state.
It does not retain threads, turns, items, settings, or other Codex-domain data.

`ClientConnection` and `WebSocketTransport` provide the browser equivalent of
the native client connection and WebSocket binding:

```ts
import {
    ClientConnection,
    CodexBridgeClient,
    WebSocketTransport,
} from "@snodec/codex-frontend";

const client = new CodexBridgeClient();
const connection = new ClientConnection(client);
const transport = new WebSocketTransport(
    connection,
    "ws://localhost:8080/codex",
);
```

The transport negotiates the `codex` subprotocol, accepts JSON text messages,
and applies the same 64 MiB default message bound as the C++ client. It does
not reconnect automatically; application intent owns construction of a new
transport after a completed detach.
