# AISuite

AISuite is the home of reusable C++ AI integrations built on SNode.C. Its
initial provider is a typed, asynchronous client, backend, and frontend protocol
for the OpenAI Codex App Server.

This repository was extracted additively from `SNodeC/snode.c` at commit
`d18b231a1d2ec2235fd6f204786b0a761cc24ff5`. The original SNode.C Codex
implementation remains untouched until a later, independently reviewed cutover.

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

Test-enabled builds must also set
`AISUITE_TEST_SNODEC_SOURCE_REPOSITORY=/absolute/path/to/snode.c` to a clean
clone containing the pinned extraction dependency; the installed-consumer gate
exports that pinned tree and rebuilds both packages in disjoint directories.

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
facades.

## Current protocol status

The A14-UserIntegrations milestone completes exactly 33 A1.4 identities:
23 client requests, six server notifications, and four `PluginSource`
alternatives. The live registry is:

- 313 Complete
- 4 Partial
- 22 NotImplemented
- 48 NotApplicable

Native A1.4 remains in progress at 33 Complete, 1 Partial, and 22
NotImplemented. PR B, PR C, the inherited A1.0 Partials, and InventoryOnly
identities remain untouched. See the
[user-facing integrations report](docs/ai/openai/codex/a1-4-user-facing-integrations.md)
for the exact scope and verification boundary.
