# AISuite

AISuite is the home of reusable C++ AI integrations built on SNode.C. Its
initial provider is a typed, asynchronous client, backend, and frontend protocol
for the OpenAI Codex App Server.

AISuite consumes an installed SNode.C 2.0-or-newer package while keeping its Codex
protocol implementation and versioned protocol sources in this repository.

AISuite now owns the three remaining Codex-specific source-policy
responsibilities: installed public-header policy, backend logging API surface
policy, and parameterless semantic-logger classification policy. Four new
functional tests implement those responsibilities, while the unchanged
pre-existing synthetic-secret guard remains a separate fifth functional policy
test.

## Build

AISuite consumes an installed SNode.C 2.0-or-newer package; it never includes a sibling
SNode.C source checkout. Reusable AISuite libraries require the installed
`snodec::core` target. Application-only frontend transports use the exact
installed SNode.C targets for Unix, IPv4/IPv6, TLS, HTTP/Express, WebSocket,
and optional RFCOMM; those transport components are not imposed on a
library-only consumer.

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="/path/to/snodec/prefix"
cmake --build build --parallel 28
ctest --test-dir build --output-on-failure
```

## CMake consumption

```cmake
find_package(snodec 2.0 CONFIG REQUIRED COMPONENTS core)
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

A1.7a freezes an additive Frontend Protocol v1 contract without changing its
identity or version. Its generated catalog contains the original 15 methods
plus 90 additive definitions, for 105 total: seven frontend-native methods and
98 BackendCore mappings. The owner-reviewed denominator remains 148 formerly
unresolved decisions plus 86 notification/item compatibility contracts, or
234 total, with zero unresolved.

A1.7b completes the PIMPL-backed `FrontendService` and all 105 runtime
handlers. Fifteen filesystem/command methods remain implemented but
deployment-disabled by default, leaving 90 default-available methods. The
`default_remote` profile (`observe` + `control`) is permitted 53/90; the
12-scope `local_trusted` profile is permitted 90/90. The remote exclusion is
exactly 22 privileged provider operations, 12 reverse-response methods, and
three provider-lifecycle methods. `account.read` keeps its observer form while
`refreshToken=true` additionally requires `control`, `account_management`, and
the current controller.

Every listener borrows the same service, controller, sequence, and canonical
journal. Authentication finishes before BackendCore session creation; Unix
local trust requires verified same-user peer credentials and an owner-only
socket, while remote/untrusted connections use a protected-file bearer token
in Hello. Scope filtering is unconditional for snapshot, live, and replay
projections. Before canonical retention, AISuite removes known structured
authentication, credential, token, password, private-key, API-key, cookie, and
reviewed secret-response fields together with unsafe raw provider envelopes.
Arbitrary bounded user, model, tool, reasoning, notice, diagnostic,
process-output, and command-output text remains potentially sensitive and is
protected by the same mandatory per-principal projection.

The service implements and advertises 13 mechanism capabilities. The frozen
`multi_transport` identity remains defined for Protocol v1 compatibility, but
A1.7b does not maintain an application transport registry and does not advertise
that capability. Multi-listener operation instead follows naturally because
every SNode.C listener borrows the same application-owned service.

The reference HTTP/WebSocket path uses SNode.C 2.0's configured HTTP parser,
server limits, native upgrade, framing, and transport backpressure. Express
middleware retains AISuite's endpoint, Origin, credential-channel, and request
semantics. The parser bounds decoded bodies at one byte: AISuite rejects that
one-byte boundary and every other non-empty body, while a larger body receives
413 before Express dispatch. Static GET responses retain the descriptor produced by the hardened
`openat()`/`O_NOFOLLOW` walk, pass it to `FileReader::adopt()`, and attach that
source with `Response::pipe()`; they never reopen the authorized pathname or
buffer the whole asset. A failed or throwing pipe setup stops the reader; a
successful pipe transfers source ownership to SNode.C. HEAD returns the same
representation length without a body. See the A1.7b report for the complete
profile and exact installed SNode.C targets.

The installed frontend surface adds the generated contract and security
headers, `GeneratedProtocol.h` and `Security.h`; A1.7b replaces
`BackendAdapter.h` with `FrontendService.h` and provides no public alias.
Installed header inventory is therefore 29 main + 7 backend + 9 frontend = 45
total. Project version `0.1.0` and all three Codex libraries' SOVERSION 2 remain
unchanged. See the
[A1.6a backend foundation](docs/ai/openai/codex/a1-6a-backend-foundation.md), the
[A1.6b backend completion](docs/ai/openai/codex/a1-6b-backend-completeness.md),
the [A1.7a frontend contract](docs/ai/openai/codex/a1-7a-frontend-contract.md),
the [A1.7b FrontendService](docs/ai/openai/codex/a1-7b-frontend-service.md),
the [Final A1a protocol report](docs/ai/openai/codex/a1-final-protocol-completion.md)
for initialization and canonical error behavior, and the
[Final A1b ABI transition](docs/ai/openai/codex/a1-final-abi-transition.md)
for the exact source-compatibility boundary. A1.7c-1 is next and owns the C++
Frontend SDK plus `codex-backend-client` migration. A1.7c-2 immediately follows
and migrates the existing `codex-ui` into the canonical standalone AI IDE; no
additional PR is inserted before it. A1.7d owns the TypeScript Frontend SDK and
browser frontend. Provider-neutral architecture remains A2.

AISuite validates build and runtime compatibility with installed SNode.C 2.0
or newer. CI builds the current SNode.C `master` branch once, installs
it, and configures AISuite only against that prefix. No SNode.C source checkout
is required by AISuite tests.
