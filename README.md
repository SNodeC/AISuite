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

## CMake consumption

```cmake
find_package(snodec CONFIG REQUIRED COMPONENTS core net-un-stream-legacy)
find_package(AISuite CONFIG REQUIRED)

target_link_libraries(my_app PRIVATE AISuite::OpenAICodex)
```

The public C++ namespace remains `ai::openai::codex` and public includes retain
forms such as `<ai/openai/codex/AppServerClient.h>`.

## Current protocol status

The extraction intentionally preserves the post-A1.3/A1.4-audit state:

- 280 Complete
- 4 Partial
- 55 NotImplemented
- 48 NotApplicable

No A1.4 production implementation is part of the extraction.
