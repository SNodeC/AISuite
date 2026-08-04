# Codex backend reference application

`codex-backend` is the first canonical AISuite-on-SNode.C server composition
and a small local reference application for
[Codex Frontend Protocol v1](frontend-protocol-v1.md). It owns one local Codex
App Server client and accepts several frontend clients on a Unix-domain stream
socket. It is an adapter and example, not a second backend implementation or a
production daemon. The companion
[`codex-backend-client`](../../../../src/apps/codex-backend-client/README.md) is a
small terminal and diagnostic client for that frontend protocol.

The concrete transport is intentionally confined to
`src/apps/codex-backend`:

```text
ai::openai::codex::stdio::Client
                  ↓
             BackendCore
                  ↓
      frontend::FrontendService
                  ↓
net::un::stream::legacy::SocketServer
                  ↓
       compact JSONL clients
```

Reusable `ai-openai-codex-backend` code has no socket dependency, and reusable
`ai-openai-codex-frontend` code has no Unix or JSONL dependency. Unix-domain
applications and staged server compositions link the canonical
`snodec::net-un-stream-legacy` target. Reusable libraries require
`snodec::core`, while an installed library-only AISuite consumer does not
require the Unix transport component. That boundary keeps future transports
from depending on a Unix socket path or `SocketContext`.

The owned BackendCore implements all 86 stable Codex application operations,
all ten stable provider requests, and canonical state for all 68 stable
notifications and 18 `ThreadItem` alternatives. The reference application's
Frontend Protocol v1 mapping deliberately remains smaller. Backend completion
does not create new socket methods or authorize remote access to the complete
trusted in-process API. See the
[A1.6b backend completion](a1-6b-backend-completeness.md) for the exact
provider/backend boundary.

## A1.7a contract versus current runtime

A1.7a freezes an additive v1 catalog of 105 methods: the original 15 plus 90
new definitions. Seven are frontend-native and 98 map to BackendCore (86
provider operations plus 12 reverse response/rejection commands). This
reference application's runtime dispatch remains exactly the original 15.
Defining an additive method does not make it available on the Unix socket,
enabled by deployment policy, or permitted for a connection.

The frozen review population is 148 formerly unresolved decisions plus 86
existing notification/item compatibility contracts, or 234 identities, with
zero final unresolved decisions. Generated percentages cannot shrink that
denominator.

Optional hello/welcome discovery fields distinguish capabilities that are
defined, implemented, and permitted, and distinguish available methods from
connection-permitted methods. The 18-name capability vocabulary is complete,
but A1.7a runtime metadata marks only method discovery and security scopes as
implemented; it does not claim authenticated frontend, scope-projected state,
provider-lifecycle exposure, or multi-transport support.

The default remote scope profile is exactly `observe` plus `control`. The local
trusted profile contains those and the ten additional scopes
`provider_lifecycle`, `account_management`, `configuration_write`,
`command_execution`, `filesystem_read`, `filesystem_write`,
`extension_management`, `mcp_invoke`, `sensitive_response`, and
`unknown_request_response`. Scope possession and controller ownership are
independent. A controller still needs every method scope; a principal with
`control` scope does not become controller automatically.

Filesystem access and arbitrary command execution are conditional and
default-disabled. `account.read` is an observer operation only when
`refreshToken` is absent or false; `refreshToken=true` additionally requires
`control`, `account_management`, and current controller ownership. These are
frozen contract decisions for A1.7b enforcement, not authentication or
authorization already supplied by this local reference application.

Compatibility remains complete and duplicate-free: all 68 stable
notifications retain either their existing normalized path or bounded/redacted
`codex.extension`, and all 18 stable `ThreadItem` alternatives retain their
existing normalized or metadata-only path. Capability-gated expanded mappings
are defined, but one provider occurrence must use either the legacy or expanded
projection for a connection, never both.

## Composition and lifetime

The listener uses the actual SNode.C legacy helper:

```cpp
ai::openai::codex::backend::BackendCore<
    ai::openai::codex::stdio::Client
> backend;

auto socketServer =
    net::un::stream::legacy::Server<
        apps::codex_backend::CodexFrontendSocketContextFactory
    >("codex-backend", configure, backend);
```

The helper yields a
`net::un::stream::legacy::SocketServer<CodexFrontendSocketContextFactory,
BackendCore<stdio::Client>&>`. The factory derives from
`core::socket::stream::SocketContextFactory`, owns one reusable
`frontend::FrontendService`, and constructs one
`CodexFrontendSocketContext` per accepted socket.

`main()` performs this ordering:

1. initialize `core::SNodeC`;
2. directly construct `backend::BackendCore<stdio::Client>`;
3. create and listen with the Unix socket server;
4. start `BackendCore`, then enter the SNode.C event loop;
5. destroy the socket server and all contexts;
6. destroy `BackendCore`, whose non-template runtime stops before its directly
   owned App Server client is destroyed; and
7. perform the final idempotent `core::SNodeC::free()` cleanup.

This construction order prevents an accepted context from outliving the
backend. Frontend callbacks additionally use weak lifetime gates. Disconnecting
one frontend closes its transport-neutral `FrontendConnection` and backend
`FrontendSession`; it does not call `BackendCore::stop()` and does not stop the
Codex App Server. Provider failure or restart likewise retains frontend
sessions and controller ownership. The explicit `start()`, `stop()`, and
`restart()` API controls the provider independently of backend-service
lifetime.

## Build and install

The executable follows the repository's `src/apps` convention and is built
only when `AISUITE_BUILD_APPS` is enabled:

```sh
cmake -S . -B build \
  -DAISUITE_BUILD_APPS=ON \
  -DAISUITE_BUILD_TESTS=ON \
  -DCMAKE_PREFIX_PATH=/path/to/installed/snodec
cmake --build build --parallel --target codex-backend codex-backend-client
```

With a conventional single-configuration generator, run the in-tree binaries
from `build/src/apps/codex-backend/codex-backend` and
`build/src/apps/codex-backend-client/codex-backend-client`. An ordinary install
places both executables in the configured binary install directory:

```sh
cmake --install build
```

Ordinary library builds may set `-DAISUITE_BUILD_APPS=OFF`; the exported
`AISuite::OpenAICodexBackend` and `AISuite::OpenAICodexFrontend` components
remain independently reusable.
All three Codex libraries remain in the intentionally unreleased SOVERSION-2
development boundary; A1.7a does not change project version `0.1.0`.
A1.7a installs exactly two additional frontend headers,
`GeneratedProtocol.h` and `Security.h`, so installed inventory is 29 main,
seven backend, and nine frontend headers, or 45 total.
The executable also requires the `codex` command expected by the existing
stdio client. Authentication and quota are properties of that local Codex
installation, not of the frontend protocol.

Start the server and client in separate terminals. The client sends `hello`
automatically and provides typed synchronization, controller, thread, and turn
commands as documented in its README:

```sh
codex-backend
codex-backend-client
```

The staged module-consumer test separately finds SNode.C components `core` and
`net-un-stream-legacy` plus AISuite, then builds a minimal Unix server with
`BackendCore<stdio::Client>` and the public frontend adapter. It links only the
three `AISuite::OpenAICodex*` targets plus `snodec::core` and
`snodec::net-un-stream-legacy`, and uses no source-tree or private headers.

## Provider recovery configuration

Embedded `BackendCore` consumers keep automatic recovery disabled by default.
This reference application opts in with deterministic defaults: recovery
enabled, unlimited attempts, a 1,000 ms initial delay, a 30,000 ms maximum,
and multiplier 2. The corresponding SNode.C/CLI configuration options are:

```text
--provider-recovery-enabled=<true|false>
--provider-recovery-maximum-attempts <count>
--provider-recovery-initial-delay-ms <milliseconds>
--provider-recovery-maximum-delay-ms <milliseconds>
--provider-recovery-multiplier <factor>
```

Only unexpected provider `Transport` and `Process` errors are retried.
Recovery always stops the failed client, waits for `Stopped`, and schedules the
next start through the SNode.C event-loop timer. Manual stop cancels a pending
retry; manual restart supersedes it. Frontend sessions, controller ownership,
and the shared frontend replay journal survive provider recovery.

## Reference client thread lifecycle

The reference client exposes the existing Frontend Protocol v1 operations with
the following thread-management syntax:

```text
start [--cwd <path>] [--model <model>]
      [--model-provider <provider>]
      [--approval-policy <policy>]
      [--sandbox-mode <mode>]
      [--ephemeral]

resume <thread-id>
       [--cwd <path>] [--model <model>]
       [--model-provider <provider>]
       [--approval-policy <policy>]
       [--sandbox-mode <mode>]

new [thread-start-options] -- <prompt>
new <prompt>

turn <thread-id> <prompt>
read <thread-id>
threads
acquire
release
```

These remain the existing v1 command subset. A1.6b's additional BackendCore
commands are not mapped into this protocol. The adapter projects the exact
`ThreadStartResponse`, `ThreadResumeResponse`, and `ThreadReadResponse` wrappers
back to the existing `result.thread` JSON, `TurnStartResponse` to the existing
`result.turn`, `ThreadListResponse` to the existing page JSON, and
`typed::Unit` to the existing empty object. Thus completing exact BackendCore
results does not change current reference-client response bytes or fields.

Trusted in-process BackendCore observers may invoke a reviewed set of
read-only operations, including filesystem metadata, directory, and file
reads. That policy does not authorize any corresponding Frontend Protocol v1
method. A1.7a freezes those frontend methods as conditional and default-off;
A1.7b owns authenticated runtime enablement and enforcement.

Frontend sessions begin as observers, and the client does not acquire
controller ownership automatically. `start`, `resume`, `new`, and `turn`
therefore require a prior successful `acquire`; an observer receives the
backend's ordinary `permission_denied` response.

An interactive explicit workflow creates a thread, reports its ID, and then
uses that ID for the first turn:

```text
acquire
start --cwd /home/voc/projects/snode.c
turn <returned-thread-id> Review the repository.
```

An existing persisted thread must be resumed before it can accept a turn in
the running App Server:

```text
acquire
threads
resume <thread-id>
turn <thread-id> Continue the previous task.
```

`threads` may include threads marked `notLoaded`. In particular:

> `read` retrieves thread data but does not resume or load the thread into the
> running Codex App Server. Use `resume <thread-id>` before starting a turn on a
> persisted `notLoaded` thread.

The convenience workflow is:

```text
acquire
new --cwd /home/voc/projects/snode.c -- Review the repository.
```

`new` is solely a `codex-backend-client` compound command. It submits a typed
`ThreadStart`, waits for the matching successful response, validates the
returned `result.thread.id`, and submits a typed `TurnStart` with that ID and
one text input containing the complete prompt. The backend and Frontend
Protocol v1 gain no `new` operation, and the client does not send both requests
before the start response arrives.

Human mode reports concise started/resumed IDs and `new` stage results. In
`--json` mode, stdout remains complete Frontend Protocol JSONL: `new` emits the
real `thread.start` and `turn.start` responses and events, never a synthetic
message. Local diagnostics remain on stderr.

For example, a piped JSON workflow is:

```sh
printf '%s\n' \
  'acquire' \
  'new --cwd /home/voc/projects/snode.c -- Review the repository.' \
  | codex-backend-client --json
```

EOF draining waits for both stages of `new`. If thread creation succeeds but
the initial turn cannot be submitted or returns a failure, the client exits
unsuccessfully, reports that the thread was created, and preserves its ID in
the diagnostic. Disconnects and send failures during either stage likewise
produce a deterministic failure rather than silently completing the compound
command.

## Socket path

The application registers an SNode.C server instance named `codex-backend`.
Its local address exposes the standard `--sun-path` CLI/config-file option.
The option is authoritative when supplied. The exact nested CLI form is:

```sh
build/src/apps/codex-backend/codex-backend \
  codex-backend local \
  --sun-path /run/user/1000/my-codex-backend.sock
```

Use `--help=expanded` at the application root, or
`codex-backend local --help`, to inspect the generated configuration help.

Without an override, the safe application default is:

```text
$XDG_RUNTIME_DIR/snodec-codex-backend.sock
```

when `XDG_RUNTIME_DIR` is non-empty, otherwise:

```text
/tmp/snodec-codex-backend-<numeric-uid>.sock
```

The default never embeds a developer-specific UID. A filesystem socket uses a
locked sidecar containing a versioned SNode.C ownership marker, so competing
listeners cannot both claim it. A new marker is created exclusively. If the
socket path already exists at that point, startup refuses to infer ownership
retroactively and preserves the path, including an unmarked stale socket or a
socket that has been bound but has not begun listening.

The v1 marker payload is the exact UTF-8 byte sequence
`snodec.unix-socket-lock:v1\n`; extra or missing bytes make a sidecar
unrecognized.

Only a pre-existing sidecar whose marker is recognized can authorize crash
recovery. After acquiring its advisory lock, startup validates the marker,
confirms that the existing node is a socket, and probes it with a nonblocking
Unix connection. It removes only an `ECONNREFUSED`/`ENOENT` stale case, after
rechecking the socket type, device, and inode immediately before unlinking.
An active or unverifiable socket is preserved, as is an unrecognized sidecar,
regular file, symlink, replacement inode, or other unrelated filesystem
object. Clean shutdown likewise removes only the socket and marker identities
owned by this listener. Bind/listen failure is reported and stops the
application cleanly.

## Unix stream framing

The Unix transport uses one compact JSON object followed by one newline:

```text
{"protocol":"snodec.codex-frontend","version":1,"kind":"hello"}\n
```

The newline is transport framing and is not part of Frontend Protocol v1.
`Codec` itself consumes and produces JSON values or compact strings, so a
future transport can select another framing scheme.

`JsonLineFramer` accepts a line split over any number of reads and any number
of complete lines in one read. A trailing carriage return before the record
newline is removed. A JSON string containing an escaped newline uses the two
wire characters `\` and `n`, so it does not terminate a frame. Server messages
are serialized compactly and never pretty-printed.

The default maximum frame payload is exactly 1 MiB (1,048,576 bytes), excluding
the terminating newline. A payload of exactly that size is accepted if its
next byte is the newline. A further non-newline byte clears the bounded
accumulator, yields `frame_too_large`, sends a bounded local protocol error
where possible, and closes only that connection. The same isolation applies to
malformed JSON. `SocketFrontendOptions::maximumFrameSize` makes the bound
deterministic and configurable for embedding and tests; this reference
`main()` uses the default.

Example using a Unix-capable client such as `socat` (each input line must be
compact JSON):

```sh
printf '%s\n' \
  '{"protocol":"snodec.codex-frontend","version":1,"kind":"hello"}' \
  | socat - UNIX-CONNECT:"${XDG_RUNTIME_DIR:-/tmp}/snodec-codex-backend.sock"
```

When `XDG_RUNTIME_DIR` is absent, substitute the UID-bearing fallback path;
the shorthand command above intentionally does not guess it.

## Handshake and commands

The context opens a transport-neutral `FrontendConnection` when the socket is
accepted, but no backend `FrontendSession` exists until a valid hello. Send:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"hello","resumeAfter":41}
```

The server sends `welcome`, replay batches or a snapshot, and
`sync.complete`. Every new session is an observer. Controller acquisition is a
separate correlated command:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"command","requestId":"role-1","method":"controller.acquire","params":{}}
```

Other observers remain connected and receive the same ordered normalized
event batches. An observer can list/read threads and synchronize, but receives
`permission_denied` for thread/turn mutation or request answers. See the
[v1 protocol document](frontend-protocol-v1.md) and its
[JSON Schema](frontend-protocol-v1.schema.json) for every envelope and method.

Messages before hello, malformed JSON, unsupported protocol versions, unknown
kinds, and command validation failures never reach the raw Codex protocol. A
pre-hello error and identity/version/framing error close that client after a
bounded `protocol.error`. Post-handshake command errors normally keep the
connection open.

## Coalescing and outbound backpressure

The factory's shared `FrontendService` subscribes once to BackendCore. Raw text,
reasoning, and command-output deltas have already accumulated in canonical
state before the adapter sees their backend transition. The adapter marks the
specific entity/channel dirty, schedules only one next-tick flush, replaces
obsolete intermediate state for the same key, and emits normalized events in
batches of at most 64 events and 256 KiB by default. Terminal and interactive
updates flush immediately. The replay journal holds at most 4,096 normalized
events and 8 MiB, never the raw token stream.

There are two independent per-client backpressure boundaries:

- `FrontendService` allows at most 512 queued protocol messages and 11 MiB of
  compact serialized JSON per connection, delivering at most 64 messages in
  one event-loop callback;
- `CodexFrontendSocketContext` allows at most 13 MiB outstanding in that Unix
  connection's socket writer, including the newline framing byte.

The limits intentionally have headroom in dependency order: the 8 MiB journal
counts event objects, the 11 MiB adapter queue also accommodates bounded replay
batch and synchronization envelopes, and the 13 MiB Unix writer additionally
accommodates JSONL framing and data already handed off by the adapter. Thus a
new Unix connection can replay a full default journal without being rejected
solely because downstream accounting includes envelope overhead. All three
limits remain finite. Deployments that customize one limit must adjust the
downstream limits consistently; a complete snapshot is one separate message
and a configured writer too small for it closes only that frontend.

No unbounded application queue is added. If either boundary cannot accept the
next message, the adapter closes that frontend, clears its queued data, and
detaches its session. A throwing send callback has the same local effect. One
slow observer cannot add data to the controller's queue, delay another client,
stop BackendCore, or stop the App Server. A disconnected controller releases
its role; pending approvals, user-input prompts, authentication requests, and
unknown requests remain pending and are never automatically answered.

All receive, delivery, coalescing, and cleanup work runs through the SNode.C
event loop. The application adds no `std::thread`, blocking descriptor I/O,
direct `fork()`/`vfork()`, or blocking `waitpid()`.

## Shutdown and operational limits

Stopping the global SNode.C loop closes accepted contexts and the listening
socket before BackendCore is destroyed. The listener removes its own socket
and lock nodes on clean shutdown, subject to inode-identity checks. BackendCore
then stops the stdio App Server client, whose existing nonblocking lifecycle
owns child termination and reaping.

This application is deliberately local and minimal. Apart from its explicit
provider-recovery configuration, it adds no daemon or production-service policy
(even though generic framework-wide CLI options may appear in SNode.C help),
frontend-user authentication, TLS, systemd socket activation, or
snapshot/replay persistence. Unix filesystem permissions and the
runtime-directory policy are the local access boundary.

No credentialed real-backend integration is registered in this milestone:
`SNODEC_RUN_CODEX_BACKEND_INTEGRATION=1` is therefore documented as a proposed
gate, not an implemented test switch. The ordinary deterministic suite uses a
fake App Server and requires no credentials, quota, network service, or real
model turn.

A1.7a defines the complete additive contract but intentionally leaves runtime
service expansion deferred. No IPv4, IPv6, RFCOMM, WebSocket, Qt UI, browser
frontend, UI-product migration, remote authentication, multi-controller
policy, or forced controller takeover is implemented here. A1.7b owns the
authenticated, scope-projecting `FrontendService`, approved additive runtime
methods/events/state, provider lifecycle exposure, and multi-transport
composition. A1.7c owns the C++ client SDK and Qt UI. A1.7d owns the TypeScript
client SDK and browser UI.

The frozen ownership remains one `BackendCore`, one shared frontend journal,
and multiple future transport factories. Those factories must share one replay
sequence and retain Protocol v1's state reduction, coalescing, bounded
batching, replay fallback, and slow-client isolation. Provider-neutral
architecture remains A2.
