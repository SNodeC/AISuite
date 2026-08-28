# AISuite

AISuite provides a typed, asynchronous C++ integration for the OpenAI Codex
app-server, built on SNode.C. The canonical implementation is a stateless
multi-client bridge: app-server remains the semantic and persistence authority,
while AISuite owns transport adaptation, controller routing, typed facades, and
bounded telemetry.

## Components

- `AISuite::OpenAICodex`: backend SDK, frontend proxy SDK, bridge routing, and
  provider/frontend transport adapters.
- `@snodec/codex-frontend`: framework-neutral TypeScript frontend proxy and
  generated protocol declarations for browser and Node clients.
- `codex-bridge`: one app-server provider connection exposed to multiple
  controller or observer clients.
- `codex-bridge-client`: interactive SNode.C client using the frontend proxy SDK.
- `tests/codex`: focused routing, framing, callback, provider, and frontend
  transport tests.

The generated protocol datatypes cover every JSON-RPC message represented by
the selected app-server schema. Typed values expose direct field access and
`getRaw()` for lossless access to the original `nlohmann::json`. Raw JSON-RPC
submission remains available alongside the typed API.

The TypeScript declarations are generated from the same pinned schema and
operation bindings as the C++ views. Their focused equality suite builds the
package and compares source hashes, type names, operation bindings, and counts:

```sh
npm test --prefix packages/codex-frontend
```

## Integrated browser listener

`codex-bridge` serves a built CodexWebUI and the frontend WebSocket from one
HTTP listener. The installed defaults are:

- `/` and `/assets/...`: static files below
  `${CMAKE_INSTALL_FULL_DATADIR}/codexui/web`;
- `/codex`: WebSocket upgrade using the `codex` subprotocol.

Enable the IPv4 listener with, for example:

```sh
codex-bridge codex-bridge-websocket-ipv4 --disabled=false \
  local --host 127.0.0.1 --port 8080
```

`--bridge-web-root PATH` overrides the static directory. An empty value
disables static delivery without changing WebSocket behavior. No Node process
is required after the web artifact has been built and installed.

## Build

AISuite consumes an installed SNode.C `master`/HEAD package.

```sh
cmake -S . \
  -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="/path/to/snodec/prefix" \
  -DAISUITE_BUILD_APPS=ON \
  -DAISUITE_BUILD_CODEX_TESTS=ON
cmake --build "${BUILD_DIR}" --parallel 8
ctest --test-dir "${BUILD_DIR}" \
  -L codex --output-on-failure --parallel 8
```

## CMake Consumption

```cmake
find_package(AISuite CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE AISuite::OpenAICodex)
```

Public headers are installed below `aisuite/ai/openai/codex`, and the public
namespace is `ai::openai::codex`.

## Architecture

The complete runtime object graph, protocol contract, configuration rules,
transport matrix, public APIs, implementation report, and focused test design
are documented in
[`src/ai/openai/codex/docs/architecture.md`](src/ai/openai/codex/docs/architecture.md).
