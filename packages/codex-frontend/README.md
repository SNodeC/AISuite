<a id="banner"></a>

<!-- snodec:begin page-header -->
<a id="banner"></a><a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../../docs/media/nav/banner-frontend-sdk-compact.svg"><img src="../../docs/media/nav/banner-frontend-sdk.svg" alt="AISuite — Frontend SDK" width="100%"></picture></p>
<p><a href="../../README.md#banner" title="Up to AISuite"><picture><source media="(prefers-color-scheme: dark)" srcset="../../docs/media/nav/up-aisuite-dark.svg"><img src="../../docs/media/nav/up-aisuite-light.svg" alt="Up to AISuite" title="Up to AISuite" width="160" height="28"></picture></a><a href="https://github.com/VolkerChristian#banner" title="Volker Christian account"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../../docs/media/nav/account-icon-dark.svg"><source media="(max-width: 640px)" srcset="../../docs/media/nav/account-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../../docs/media/nav/account-dark.svg"><img align="right" src="../../docs/media/nav/account-light.svg" alt="Volker Christian account" title="Volker Christian account"></picture></a><a href="https://github.com/SNodeC#banner" title="SNode.C organization"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../../docs/media/nav/organization-icon-dark.svg"><source media="(max-width: 640px)" srcset="../../docs/media/nav/organization-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../../docs/media/nav/organization-dark.svg"><img align="right" src="../../docs/media/nav/organization-light.svg" alt="SNode.C organization" title="SNode.C organization"></picture></a><a href="../../README.md#banner" title="AISuite repository"><picture><source media="(max-width: 640px) and (prefers-color-scheme: dark)" srcset="../../docs/media/nav/repository-icon-dark.svg"><source media="(max-width: 640px)" srcset="../../docs/media/nav/repository-icon-light.svg"><source media="(prefers-color-scheme: dark)" srcset="../../docs/media/nav/repository-dark.svg"><img align="right" src="../../docs/media/nav/repository-light.svg" alt="AISuite repository" title="AISuite repository"></picture></a><br clear="all"></p>
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

The package is `@snodec/codex-frontend`; it exports compiled JavaScript and TypeScript declarations. AISuite's normal build installs the npm archive and `AISuiteFrontend` discovery metadata. To build and install only the frontend SDK, without SNode.C or AISuite's C++ libraries:

```sh
cmake -S packages/codex-frontend -B /path/to/build/aisuite-sdk -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build /path/to/build/aisuite-sdk
sudo cmake --install /path/to/build/aisuite-sdk
```

Use a writable custom prefix and omit `sudo` when appropriate. CMake consumers locate the installed archive through `find_package(AISuiteFrontend 1.0 CONFIG REQUIRED)` and `AISuiteFrontend_PACKAGE`; pass a custom prefix through `CMAKE_PREFIX_PATH`. npm consumers can install the archive with `npm install /usr/local/share/aisuite/npm/aisuite-frontend.tgz`. The archive is extracted into their own node_modules, not linked to this source checkout. See [CodexUI's browser build guide](https://github.com/SNodeC/CodexUI/blob/master/docs/guides/build-from-source.md#browser-frontend).
