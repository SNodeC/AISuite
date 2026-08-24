# AISuite

AISuite provides a typed, asynchronous C++ integration for the OpenAI Codex
app-server, built on SNode.C. The canonical implementation is a stateless
multi-client bridge: app-server remains the semantic and persistence authority,
while AISuite owns transport adaptation, controller routing, typed facades, and
bounded telemetry.

The previous stateful implementation is preserved on the dedicated
`legacy-codex` Git branch. It is not part of the canonical source tree or build.

## Components

- `AISuite::OpenAICodex`: backend SDK, frontend proxy SDK, bridge routing, and
  provider/frontend transport adapters.
- `codex-bridge`: one app-server provider connection exposed to multiple
  controller or observer clients.
- `codex-bridge-client`: interactive SNode.C client using the frontend proxy SDK.
- `tests/codex`: focused routing, framing, callback, provider, and frontend
  transport tests.

The generated protocol datatypes cover every JSON-RPC message represented by
the selected app-server schema. Typed values expose direct field access and
`getRaw()` for lossless access to the original `nlohmann::json`. Raw JSON-RPC
submission remains available alongside the typed API.

## Build

AISuite consumes an installed SNode.C `master`/HEAD package. The canonical
incremental build directory is:

```text
/home/voc/projects/drafts/AISuite-extraction/build/codex-build
```

```sh
cmake -S . \
  -B /home/voc/projects/drafts/AISuite-extraction/build/codex-build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="/path/to/snodec/prefix" \
  -DAISUITE_BUILD_APPS=ON \
  -DAISUITE_BUILD_CODEX_TESTS=ON
cmake --build /home/voc/projects/drafts/AISuite-extraction/build/codex-build \
  --parallel 8
ctest --test-dir /home/voc/projects/drafts/AISuite-extraction/build/codex-build \
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
