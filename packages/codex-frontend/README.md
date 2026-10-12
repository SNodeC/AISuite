<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../../docs/media/nav/banner-frontend-sdk-compact.svg"><img src="../../docs/media/nav/banner-frontend-sdk.svg" alt="AISuite — Frontend SDK" width="100%"></picture></p>
<p><a href="../../README.md#project-overview" title="Up to AISuite"><picture><source media="(prefers-color-scheme: dark)" srcset="../../docs/media/nav/up-aisuite-dark.svg"><img src="../../docs/media/nav/up-aisuite-light.svg" alt="Up to AISuite" title="Up to AISuite" width="160" height="28"></picture></a><a href="https://github.com/SNodeC" title="SNodeC organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../../docs/media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../../docs/media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../../docs/media/nav/organization-dark.svg"><img align="right" src="../../docs/media/nav/organization-light.svg" alt="SNodeC organization" title="SNodeC organization"></picture></a><a href="../../README.md#project-overview" title="AISuite repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../../docs/media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../../docs/media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../../docs/media/nav/repository-dark.svg"><img align="right" src="../../docs/media/nav/repository-light.svg" alt="AISuite repository" title="AISuite repository"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# `@snodec/codex-frontend`

Framework-neutral TypeScript frontend SDK for the AISuite `codex-bridge`.

The generated protocol declarations come from the same pinned Codex schema and Rust operation bindings as AISuite's C++ views. Regenerate both outputs in one pinned invocation:

```sh
tools/regenerate-codex-protocol.sh
```

The release script fetches the exact pinned Codex revision, extracts that release's experimental schema, and regenerates both language surfaces. CI runs the same command with `--check` and fails on any drift.

`npm test --prefix packages/codex-frontend` builds the declarations and proves that the checked-in C++ and TypeScript type names, operation bindings, required-parameter flags, counts, and source hashes remain equal.

## Frontend proxy

`CodexBridgeClient` mirrors the observable routing and lifecycle contract of AISuite's C++ frontend proxy while exposing generated method typing:

In this low-level example, `transport` is the consuming application’s connected message sender. For the SDK-provided browser connection helpers, continue to the next example.

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

The proxy retains only pending JSON-RPC callbacks and bridge connection state. It does not retain threads, turns, items, settings, or other Codex-domain data.

`ClientConnection` and `WebSocketTransport` provide the browser equivalent of the native client connection and WebSocket binding:

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
    "ws://127.0.0.1:8080/codex",
);
```

The transport negotiates the `codex` subprotocol, accepts JSON text messages, and applies the same 64 MiB default message bound as the C++ client. It does not reconnect automatically; application intent owns construction of a new transport after a completed detach. `ClientConnectionCallbacks.onDetached` reports that completion even when a socket failed before it became online; `onDisconnected` remains the online-to-offline lifecycle notification.

## Consume the SDK from source

The package is `@snodec/codex-frontend`; `package.json` exports the compiled `dist/index.js` and `dist/index.d.ts`. This source-consumption route does not require a published npm release. From an AISuite checkout, build the SDK before linking it into a separate application:

```sh
npm ci --prefix packages/codex-frontend
npm run build --prefix packages/codex-frontend
cd /path/to/your/application
npm install --save /absolute/path/to/AISuite/packages/codex-frontend
```

The consumer records a local `file:` dependency. Keep that checkout available and rebuild the SDK when changing its source; record its resolved Git revision with your application. Imports use the package export, not private source paths. CodexUI's [build guide](https://github.com/SNodeC/CodexUI#project-overview) documents its existing local checkout layout (Build → Browser frontend).
