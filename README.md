# AISuite

AISuite is the home of reusable C++ AI integrations built on SNode.C. Its
initial provider is a typed, asynchronous client, backend, and frontend protocol
for the OpenAI Codex App Server.

AISuite consumes the current installed SNode.C package while keeping its Codex
protocol implementation and versioned protocol sources in this repository.

AISuite now owns the three remaining Codex-specific source-policy
responsibilities: installed public-header policy, backend logging API surface
policy, and parameterless semantic-logger classification policy. Four new
functional tests implement those responsibilities, while the unchanged
pre-existing synthetic-secret guard remains a separate fifth functional policy
test.

## Build

AISuite consumes an installed SNode.C package; it never includes a sibling
SNode.C source checkout.

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="/path/to/snodec/prefix"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## CMake consumption

```cmake
find_package(snodec CONFIG REQUIRED COMPONENTS core net-un-stream-legacy)
find_package(AISuite CONFIG REQUIRED)

target_link_libraries(my_app PRIVATE AISuite::OpenAICodex)
```

The public C++ namespace remains `ai::openai::codex` and public includes retain
forms such as `<ai/openai/codex/AppServerClient.h>`. The typed client now
groups user-facing integrations behind the installed `Apps`,
`ExternalAgents`, `Feedback`, `Hooks`, `Marketplace`, `Plugins`, and `Skills`
facades. MCP operations use `Mcp`, and the cross-platform Codex App Server
Windows protocol uses `WindowsSandbox`.

## Current protocol status

The stable Codex A1 typed protocol surface is complete. Final A1a completes
the Common handshake and error identities after native A1.4. The live registry
is:

- 339 Complete
- 0 Partial
- 0 NotImplemented
- 48 NotApplicable

Native A1.4 remains 56 Complete / 0 Partial / 0 NotImplemented. All 48
InventoryOnly identities remain NotApplicable. Final A1b removes the frozen
deferred compatibility layer and moves all three Codex libraries to SOVERSION
2. See the
[Final A1a protocol report](docs/ai/openai/codex/a1-final-protocol-completion.md)
for initialization and canonical error behavior and the
[Final A1b ABI transition](docs/ai/openai/codex/a1-final-abi-transition.md)
for the exact source-compatibility boundary.

AISuite validates current build and runtime compatibility with the installed
SNode.C package. CI builds the current SNode.C `master` branch once, installs
it, and configures AISuite only against that prefix. No SNode.C source checkout
is required by AISuite tests.
