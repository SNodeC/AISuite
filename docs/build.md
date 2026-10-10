# Build and install AISuite from source

<!-- snodec:begin menu -->
<p>
  <a href="../README.md#quick-start" title="Start"><img src="media/menu/snodec-start-108.svg" alt="Start" width="108" height="24"></a>
  <a href="../README.md#install" title="Install"><img src="media/menu/snodec-install-108.svg" alt="Install" width="108" height="24"></a>
  <a href="build.md" title="Build"><img src="media/menu/snodec-build-108.svg" alt="Build" width="108" height="24"></a>
  <a href="../README.md#first-success" title="Use"><img src="media/menu/snodec-use-108.svg" alt="Use" width="108" height="24"></a>
  <a href="../README.md#configuration" title="Configure"><img src="media/menu/snodec-configure-108.svg" alt="Configure" width="108" height="24"></a>
  <a href="../README.md#architecture" title="Architecture"><img src="media/menu/snodec-architecture-108.svg" alt="Architecture" width="108" height="24"></a>
  <a href="../README.md#contributing" title="Contribute"><img src="media/menu/snodec-contribute-108.svg" alt="Contribute" width="108" height="24"></a>
</p>

<!-- snodec:end menu -->

## Prerequisites

- A C++20 toolchain, CMake 3.18+, and a build tool such as Make.
- Python 3 for the documented app-server integration tests.
- An installed [SNode.C](https://github.com/SNodeC/snode.c#project-overview) `master`/HEAD package satisfying the project's SNode.C 2.0 requirement.
- nlohmann-json headers; optional transports require their corresponding SNode.C components.
- For the default Anthropic-enabled build: OpenSSL and SNode.C HTTP client/server plus IPv4 TLS support.
- A configured Codex executable to run an owned app-server; Node.js 22+ for the frontend SDK development workflow.

## Configure, build and verify

Replace the SNode.C installation prefix:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DCMAKE_PREFIX_PATH="/path/to/snodec/prefix" \
  -DAISUITE_BUILD_APPS=ON \
  -DAISUITE_BUILD_CODEX_TESTS=ON
cmake --build build --parallel 14
ctest --test-dir build -L codex --output-on-failure --parallel 14
```

Set `-DAISUITE_ENABLE_ANTHROPIC=OFF` for the direct Codex/OpenAI path without the model-provider HTTP/TLS dependencies. Optional frontend transports are selected from available SNode.C components and can be configured with `AISUITE_ENABLE_CODEX_FRONTEND_TLS`, `AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET` and `AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM`.

## Install the build

Run these commands from the same checkout and build directory used for the source build.

The example installs into `/usr/local` and needs administrator privileges. To use a writable non-system prefix instead, set `CMAKE_INSTALL_PREFIX` at configure time and omit `sudo`; configure consumers and runtime library lookup for that prefix.

```sh
sudo cmake --install build
sudo ldconfig
```

For frontend SDK development, see the [TypeScript SDK guide](../packages/codex-frontend/README.md). For a first working bridge connection, return to [First success](../README.md#first-success).
