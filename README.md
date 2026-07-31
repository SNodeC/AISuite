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

Native A1.4 is complete. Its final runtime/platform slice adds exactly ten
identities: two Windows sandbox client requests and eight server notifications.
The live registry is:

- 336 Complete
- 3 Partial
- 0 NotImplemented
- 48 NotApplicable

Native A1.4 is 56 Complete / 0 Partial / 0 NotImplemented. The remaining
Partial identities are `initialize`, `initialized`, and `error`, owned by
Common/A1.0 and deferred to final-A1 completion. All 48 InventoryOnly
identities remain NotApplicable, and Codex SOVERSION remains 1. See the
[runtime and platform report](docs/ai/openai/codex/a1-4-runtime-and-platform-long-tail.md)
for the exact surface and lifecycle behavior.

AISuite validates current build and runtime compatibility with the installed
SNode.C package. CI builds the current SNode.C `master` branch once, installs
it, and configures AISuite only against that prefix. No SNode.C source checkout
is required by AISuite tests.
