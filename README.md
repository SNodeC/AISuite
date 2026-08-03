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
SNode.C source checkout. Reusable AISuite libraries require the installed
`snodec::core` target. Unix reference applications additionally require
`snodec::net-un-stream-legacy`; that transport component is not imposed on a
library-only consumer.

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="/path/to/snodec/prefix"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## CMake consumption

```cmake
find_package(snodec CONFIG REQUIRED COMPONENTS core)
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
domain access. A1.6a hardens the reusable backend foundation: provider
lifecycle and recovery are independent of frontend-session lifetime, cached
state carries provider-generation freshness, capacity and snapshot bounds are
explicit, and frontend replay remains owned by the frontend journal. A1.6b
completes all **86/86** stable provider-operation commands while preserving
their exact typed results, all 68 stable notifications, all ten stable server
requests, and all 18 `ThreadItem` alternatives. Its frozen canonical-state
coverage is 169/169 applicable entries: 73 stateful operations plus the 68
notifications, 18 items, and ten requests. Exactly 13 application operations
remain reasoned action-only results, and the 16 `ResponseItem` alternatives
remain reasoned `NoRuntimeBackendStatePath`.

A1.7a now freezes an additive Frontend Protocol v1 contract without changing
its identity or version. The generated catalog contains the original 15
runtime methods plus 90 additive definitions, for 105 total: seven
frontend-native methods and 98 mappings to BackendCore. The owner-reviewed
denominator is fixed at 148 formerly unresolved decisions plus 86 existing
notification/item compatibility contracts, or 234 total, with zero decisions
left unresolved. Capability advertisement distinguishes a method being
defined, implemented by the current runtime, and permitted for a connection;
the current server still accepts exactly the original 15 methods. Filesystem
and arbitrary command-execution methods remain conditional and disabled by
default, and A1.7a does not implement authentication, scope enforcement, or a
new transport. All 68 stable notifications and all 18 stable `ThreadItem`
alternatives retain their existing normalized or bounded/redacted compatibility
paths while expanded projections remain capability-gated.

The installed frontend surface adds the generated contract and security
headers, `GeneratedProtocol.h` and `Security.h`. Installed header inventory is
therefore 29 main + 7 backend + 9 frontend = 45 total. Project version `0.1.0`
and all three Codex libraries' SOVERSION 2 remain unchanged. See the
[A1.6a backend foundation](docs/ai/openai/codex/a1-6a-backend-foundation.md), the
[A1.6b backend completion](docs/ai/openai/codex/a1-6b-backend-completeness.md),
the [A1.7a frontend contract](docs/ai/openai/codex/a1-7a-frontend-contract.md),
the [Final A1a protocol report](docs/ai/openai/codex/a1-final-protocol-completion.md)
for initialization and canonical error behavior, and the
[Final A1b ABI transition](docs/ai/openai/codex/a1-final-abi-transition.md)
for the exact source-compatibility boundary. A1.7b owns runtime authentication,
scope projection, provider lifecycle exposure, and multi-transport service
activation; A1.7c owns the C++ client SDK and Qt UI; A1.7d owns the TypeScript
client SDK and browser UI. Provider-neutral architecture remains A2.

AISuite validates current build and runtime compatibility with the installed
SNode.C package. CI builds the current SNode.C `master` branch once, installs
it, and configures AISuite only against that prefix. No SNode.C source checkout
is required by AISuite tests.
