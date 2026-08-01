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

The public C++ namespace is `ai::openai::codex`. Ordinary applications can
include the complete API and access its typed domains directly:

```cpp
#include <ai/openai/codex/Api.h>

namespace codex = ai::openai::codex;
namespace typed = ai::openai::codex::typed;

void configure(codex::stdio::Client& client) {
    client.events().setOnEvent([](const typed::Event&) {});
    client.threads().start(
        [](const typed::OperationResult<typed::ThreadStartResponse>&) {});
    client.models().list(
        [](const typed::OperationResult<typed::ModelListResponse>&) {});
}
```

The 20 direct domains cover accounts, applications, commands, configuration,
events, external agents, feedback, filesystem, hooks, marketplace, MCP,
models, permission profiles, plugins, reverse requests, reviews, skills,
threads, turns, and the cross-platform Windows sandbox protocol. `raw()` is
the explicit low-level escape hatch. See the
[Codex application API](docs/ai/openai/codex/api.md) for lifecycle and result
handling.

## Current protocol status

The stable Codex A1 typed protocol surface is complete. Final A1a completed
the Common handshake and error identities after native A1.4. The live registry
is:

- 339 Complete
- 0 Partial
- 0 NotImplemented
- 48 NotApplicable

Native A1.4 remains 56 Complete / 0 Partial / 0 NotImplemented. All 48
InventoryOnly identities remain NotApplicable. Final A1b removed the frozen
deferred compatibility layer and moved all three Codex libraries to SOVERSION
2. A1.5 completes the still-unreleased `.so.2` application façade with direct
domain access; backend, frontend-protocol, and provider-neutral redesign remain
separate A1.6, A1.7, and A2 work. See the
[Final A1a protocol report](docs/ai/openai/codex/a1-final-protocol-completion.md)
for initialization and canonical error behavior and the
[Final A1b ABI transition](docs/ai/openai/codex/a1-final-abi-transition.md)
for the exact source-compatibility boundary.

AISuite validates current build and runtime compatibility with the installed
SNode.C package. CI builds the current SNode.C `master` branch once, installs
it, and configures AISuite only against that prefix. No SNode.C source checkout
is required by AISuite tests.
