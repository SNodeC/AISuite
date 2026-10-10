# Build and install AISuite from source

<!-- snodec:begin back -->
<p>
  <a href="../README.md#project-overview" title="AISuite"><img src="media/menu/back-aisuite.svg" alt="AISuite" width="96" height="24"></a>
</p>
<!-- snodec:end back -->

## Prerequisites

- A C++20 toolchain, CMake 3.18+, and a build tool such as Make.
- Python 3 for the documented app-server integration tests.
- A compatible [SNode.C](https://github.com/SNodeC/snode.c#project-overview) CMake installation satisfying `find_package(snodec 2.0)` and the components requested by the chosen features; use its default-branch source for the documented development build.
- nlohmann-json headers; optional transports require their corresponding SNode.C components.
- For the default Anthropic-enabled build: OpenSSL and SNode.C HTTP client/server plus IPv4 TLS support.
- A configured Codex executable to run an owned app-server. The generated protocol and documented compatibility profile target Codex 0.154.0; [check compatibility](codex-responses-profile.md) before using another version. Node.js 22+ is needed for frontend SDK development.

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

## Verify the installation

Run `codex-bridge --help` and `codex-bridge-client --help` from the installed prefix. If they are not found, check `PATH`; if shared libraries cannot be loaded, check the prefix and runtime library lookup before continuing. Return to [First success](../README.md#first-success) to verify a real bridge connection.

## Verification

The test suites cover routing, framing, callback lifetime, provider behavior and frontend transports. GCC/Clang builds enable warning checks; an optional `AISUITE_ENABLE_ASAN` build enables AddressSanitizer.

For the generated frontend contract:

```sh
npm ci --prefix packages/codex-frontend
npm test --prefix packages/codex-frontend
tools/regenerate-codex-protocol.sh --check
```

Regeneration requires fetching the pinned upstream source. Live provider checks are separately opt-in and may incur API charges; missing prerequisites must not be mistaken for passing coverage. See the [provider verification record](anthropic-verification.md).

## Troubleshooting

- **CMake component missing:** verify the complete SNode.C installation and enabled provider/transport components; use `CMAKE_PREFIX_PATH` for a custom prefix.
- **Executable or shared library missing:** check installation, `PATH`, and runtime library lookup before debugging the protocol.
- **Unix connection fails:** select exactly one client transport and launch bridge and client as the same user with the same runtime environment. If the bridge uses a custom socket, configure the client's `codex-bridge-client-unix remote --sun-path PATH` to match.
- **Provider not ready / protocol error:** inspect the configured Codex executable, operating-system user, authentication and pinned protocol compatibility. A socket connection alone is not app-server readiness.
- **Observer write rejected:** claim an available controller role; changing an envelope's role does not grant permission. Review [access policy](../src/ai/openai/codex/docs/architecture.md#access-boundary).
