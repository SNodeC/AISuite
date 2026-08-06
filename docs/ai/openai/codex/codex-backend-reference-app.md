# Codex backend reference application

`codex-backend` is the first canonical AISuite-on-SNode.C server composition
and a reference application for
[Codex Frontend Protocol v1](frontend-protocol-v1.md). It owns one local Codex
App Server client, one BackendCore, one FrontendService, and a configured set of
Unix/TCP/TLS/WebSocket/WSS/RFCOMM listeners. It is a composition and reference
policy, not a second backend implementation. The companion
[`codex-backend-client`](../../../../src/apps/codex-backend-client/README.md) is a
small terminal and diagnostic client for that frontend protocol.

The concrete transport is intentionally confined to
`src/apps/codex-backend`:

```text
ai::openai::codex::stdio::Client
                  ↓
          one BackendCore
                  ↓
       one FrontendService
                  ↓
       mandatory projection
       ├─ Unix JSONL
       ├─ IPv4/IPv6 JSONL and TLS JSONL
       ├─ WebSocket and WSS JSON
       └─ RFCOMM and RFCOMM-TLS JSONL
```

Reusable `ai-openai-codex-backend` code has no socket dependency, and reusable
`ai-openai-codex-frontend` code has no SNode.C transport or JSONL dependency.
Application-private factories link only the exact installed SNode.C components
for their enabled transports. A library-only AISuite consumer requires
`snodec::core` but does not inherit Unix, IP, TLS, HTTP, WebSocket, or RFCOMM.

The owned BackendCore implements all 86 stable Codex application operations,
all ten stable provider requests, and canonical state for all 68 stable
notifications and 18 `ThreadItem` alternatives. FrontendService implements the
complete 105-method v1 catalog through the A1.7a generated mappings, but
authentication, deployment gates, scopes, controller ownership, provider
readiness, and capacity still determine whether one invocation may proceed.
See the
[A1.6b backend completion](a1-6b-backend-completeness.md) for the exact
provider/backend boundary.

## A1.7b runtime policy

A1.7a freezes an additive v1 catalog of 105 methods: the original 15 plus 90
additive definitions. Seven are frontend-native and 98 map to BackendCore (86
provider operations plus 12 reverse response/rejection commands). A1.7b
implements all 105 handlers. The 15 conditional filesystem and command methods
remain off by default, so the default available catalog is 90.

The frozen review population is 148 formerly unresolved decisions plus 86
existing notification/item compatibility contracts, or 234 identities, with
zero final unresolved decisions. Generated percentages cannot shrink that
denominator.

Optional hello/welcome discovery fields distinguish capabilities that are
defined, implemented, and permitted, and distinguish available methods from
connection-permitted methods. The service advertises 13 static mechanism
capabilities. `multi_transport` is separate conditional topology truth: one
declared transport family yields false and more than one yields true. This
application keeps no duplicate listener registry; SNode.C owns listener
configuration and lifecycle.

The default remote scope profile is exactly `observe` plus `control`. The local
trusted profile contains those and the ten additional scopes
`provider_lifecycle`, `account_management`, `configuration_write`,
`command_execution`, `filesystem_read`, `filesystem_write`,
`extension_management`, `mcp_invoke`, `sensitive_response`, and
`unknown_request_response`. Scope possession and controller ownership are
independent. A controller still needs every method scope; a principal with
`control` scope does not become controller automatically. With default gates,
`default_remote` is permitted 53/90 and `local_trusted` 90/90. The remote
exclusion is exactly 22 privileged provider operations, 12 reverse methods,
and three provider-lifecycle methods.

Filesystem access and arbitrary command execution are conditional and
default-disabled. `account.read` is an observer operation only when
`refreshToken` is absent or false; `refreshToken=true` additionally requires
`control`, `account_management`, and current controller ownership.
FrontendService enforces this after schema validation.

Compatibility remains complete and duplicate-free: all 68 stable
notifications retain either their existing normalized path or bounded/redacted
`codex.extension`, and all 18 stable `ThreadItem` alternatives retain their
existing normalized or metadata-only path. Expanded mappings cover those
families, ten pending-request kinds, and 26 event families. Mandatory scope
filtering occurs before legacy/expanded selection, and one provider occurrence
uses one representation for a connection, never both.

## Composition and lifetime

The application owns the service above every listener factory:

```cpp
ai::openai::codex::backend::BackendCore<
    ai::openai::codex::stdio::Client
> backend;

ai::openai::codex::frontend::FrontendServiceOptions options;
ai::openai::codex::frontend::FrontendService service(backend, options);
apps::codex_backend::FrontendStreamSocketContextFactoryOptions factoryOptions;

auto socketServer =
    net::un::stream::legacy::Server<
        apps::codex_backend::FrontendStreamSocketContextFactory
    >("codex-backend", configure, service, factoryOptions);
```

Every stream/WebSocket factory borrows this same service. Each accepted context
owns one move-only `FrontendConnection`. No factory constructs a BackendCore,
FrontendService, EventJournal, controller registry, or authorization policy.
FrontendService subscribes to BackendCore once and owns one global controller,
sequence, canonical journal, connection universe, and projection policy.

`main()` performs this ordering:

1. initialize `core::SNodeC`;
2. load and validate AISuite policy configuration and the protected
   bearer-token file while SNode.C owns listener configuration;
3. construct `backend::BackendCore<stdio::Client>`;
4. construct one `frontend::FrontendService` borrowing it;
5. install the application-private non-owning WebSocket runtime bridge;
6. construct named SNode.C listeners and Express routes borrowing that service;
7. call each listener's normal `listen()` lifecycle;
8. start `BackendCore`, then enter the SNode.C event loop;
9. close accepted frontend connections and uninstall the runtime bridge;
10. destroy FrontendService;
11. destroy `BackendCore`, whose non-template runtime stops before its directly
   owned App Server client is destroyed; and
12. perform the final idempotent `core::SNodeC::free()` cleanup.

This construction order prevents an accepted context from outliving the
backend. Frontend callbacks additionally use weak lifetime gates. Disconnecting
one frontend closes its transport-neutral `FrontendConnection` and backend
`FrontendSession`; it does not call `BackendCore::stop()` and does not stop the
Codex App Server. Provider failure or restart likewise retains frontend
sessions and controller ownership. The explicit `start()`, `stop()`, and
`restart()` API controls the provider independently of backend-service
lifetime.

SNode.C instance configuration is the sole listener configuration authority:
disabled/enabled state, host, port, Unix path, TLS material, RFCOMM address and
channel, connection settings, and write-queue limits live there. Listener
callbacks report success, disabled state, or error. AISuite maintains neither a
cross-listener startup transaction nor transport bind bookkeeping. The Unix,
IPv4, IPv6, IPv4/IPv6 TLS, RFCOMM, and RFCOMM-TLS server templates are all
constructed explicitly in `main.cpp`, with their SNode.C configuration lambdas
at the call sites; no AISuite listener-construction wrapper remains.

## Build and install

The executable follows the repository's `src/apps` convention and is built
only when `AISUITE_BUILD_APPS` is enabled:

```sh
cmake -S . -B build \
  -DAISUITE_BUILD_APPS=ON \
  -DAISUITE_BUILD_TESTS=ON \
  -DCMAKE_PREFIX_PATH=/path/to/installed/snodec
cmake --build build --parallel 26 --target codex-backend codex-backend-client
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
development boundary; A1.7b does not change project version `0.1.0`.
`FrontendService.h` replaces `BackendAdapter.h` without changing the installed
inventory: 29 main, seven backend, and nine frontend headers, or 45 total.
The executable also requires the `codex` command expected by the existing
stdio client. Authentication and quota for that provider are separate from
frontend authentication.

Optional application transport support is selected with
`AISUITE_ENABLE_CODEX_FRONTEND_TLS`,
`AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET`, and
`AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM`. The implementation consumes the exact
installed SNode.C targets below (subject to the corresponding build option and
platform support):

```text
snodec::core
snodec::net-un-stream-legacy
snodec::net-in-stream-legacy
snodec::net-in6-stream-legacy
snodec::net-in-stream-tls
snodec::net-in6-stream-tls
snodec::net-rc-stream-legacy
snodec::net-rc-stream-tls
snodec::http-server
snodec::http-server-express
snodec::http-server-express-legacy-in
snodec::http-server-express-legacy-in6
snodec::http-server-express-tls-in
snodec::http-server-express-tls-in6
snodec::websocket-server
snodec::websocket-client
```

`snodec::websocket-client` is used by the deterministic WebSocket integration
tests.
None of those application-only targets leaks into the reusable frontend
library.

Start the server and client in separate terminals. The C++ Frontend SDK sends
Hello after the physical transport connects and provides synchronization,
controller, and domain façades as documented in the client README:

```sh
codex-backend
codex-backend-client
```

The staged installed consumers separately exercise `FrontendService` and
`AISuite::OpenAICodexFrontendClient` using only installed headers and imported
AISuite targets. The SDK consumer links no concrete SNode.C transport target
and uses no source-tree or private header.

The separate installed module-server consumer continues to exercise the
application-only `snodec::net-un-stream-legacy` composition boundary.

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

These remain the reference CLI's concise workflow subset. A1.7c-1 migrates its
protocol lifecycle and correlation to the SDK, whose generated binding
authority covers all 105 methods. For legacy compatibility FrontendService
projects the exact
`ThreadStartResponse`, `ThreadResumeResponse`, and `ThreadReadResponse` wrappers
back to the existing `result.thread` JSON, `TurnStartResponse` to the existing
`result.turn`, `ThreadListResponse` to the existing page JSON, and
`typed::Unit` to the existing empty object. Thus completing exact BackendCore
results does not change current reference-client response bytes or fields.

Trusted in-process BackendCore observers may invoke a reviewed set of
read-only operations, including filesystem metadata, directory, and file
reads. That policy does not authorize a frontend invocation. A1.7b keeps those
frontend methods conditional and default-off; an enabled invocation still
requires generated scopes, provider readiness, controller policy where
applicable, and the configured safe path policy.

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

`new` is solely a `codex-backend-client` compound command. It submits
`thread.start` through the SDK, waits for the typed completion, validates the
returned thread ID, and submits `turn.start` through the SDK with that ID and
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

EOF draining uses the SDK pending-operation count and waits for both stages of
`new`. If thread creation succeeds but
the initial turn cannot be submitted or returns a failure, the client exits
unsuccessfully, reports that the thread was created, and preserves its ID in
the diagnostic. Disconnects and send failures during either stage likewise
produce a deterministic failure rather than silently completing the compound
command.

## Frontend authentication and listener configuration

The default listener set is Unix JSONL only. It uses mode `0600` and grants
`local_trusted` only after the accepted socket descriptor reports the service
effective UID and the bound socket path is an owner-only socket owned by that
UID. Unix transport type by itself does not grant trust. If verified peer
credentials are unavailable or disabled, a protected bearer-token file is
required unless the operator explicitly enables the separately warned insecure
local-trust override.

Remote and untrusted listeners always require bearer authentication in Hello.
`--frontend-bearer-token-file` names the protected file; no option accepts the
token bytes directly. `--frontend-remote-principal-id` and
`--frontend-remote-scope-profile` configure the reference principal. The
default profile is `default_remote` (`observe`, `control`). Token-file errors
and diagnostics report structure only, never token material.

Native listener options are grouped under `--frontend-unix-*`,
`--frontend-ipv4-*`, `--frontend-ipv6-*`, `--frontend-tls-ipv4-*`,
`--frontend-tls-ipv6-*`, `--frontend-rfcomm-*`, and
`--frontend-rfcomm-tls-*`. Plain IP defaults to loopback and disabled. TLS
listeners require certificate and private-key paths. RFCOMM is optional and
Bluetooth pairing is not frontend authentication.

Web options are grouped under `--frontend-websocket-ipv4-*`,
`--frontend-websocket-ipv6-*`, `--frontend-wss-ipv4-*`, and
`--frontend-wss-ipv6-*`. `--frontend-websocket-endpoint` defaults to the exact
path `/frontend`; `--frontend-websocket-allowed-origins` adds normalized Origin
entries, and `--frontend-static-root` optionally enables the bounded static
file policy. The default browser policy is same-origin. A native WebSocket
client may omit Origin but still authenticates in Hello. Non-loopback
plaintext requires `--frontend-allow-insecure-remote` and remains
authenticated.

Deployment gates are `--frontend-filesystem-read-enabled`,
`--frontend-filesystem-write-enabled`, and
`--frontend-command-execution-enabled`. Filesystem access additionally uses
`--frontend-filesystem-root`; command policy uses explicit executable/shell
allow-lists. Connection, unauthenticated, handshake, message-size/rate/burst,
outstanding-command, and failed-authentication bounds have corresponding
`--frontend-max-*`, `--frontend-maximum-message-bytes`, and
`--frontend-failed-authentication-window-ms` options. Zero remains zero
capacity.

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

## WebSocket and static HTTP policy

SNode.C's HTTP parser and server enforce the request resource profile: 8 KiB
start and header lines, 64 KiB aggregate headers, 128 fields, a one-byte
decoded-body ceiling, one pending request, and disabled chunked transfer and
pipelining. Zero means unlimited in SNode.C and is intentionally not used.
Express rejects the one-byte boundary and every other non-empty static or
WebSocket-upgrade body; larger bodies receive 413 before route dispatch.
Middleware retains AISuite's method, Origin,
credential-channel, endpoint, and request semantics. No BackendCore frontend
session exists before Hello authentication.

WebSocket listeners use one complete compact JSON object per text message,
without JSONL newline framing. SNode.C owns upgrade, fragmentation, configured
message limits, framing, and transport writer backpressure. The dynamically
loaded AISuite server subprotocol is exactly `codex`, separate from the
Frontend Protocol identity `snodec.codex-frontend`; it opens a
FrontendConnection only after a successful upgrade, forwards text JSON, and
rejects binary messages. Credential-bearing URL/query/cookie/Authorization
channels are rejected before upgrade, and bearer material remains confined to
the first protocol Hello.

The default endpoint is exactly `/frontend`. Browser Origin comparison uses
normalized scheme, host, and effective port and defaults to same-origin. A
configured allow-list may add origins; `*` is not a valid default. Native
clients without Origin continue only to bearer authentication. Plaintext
non-loopback browser admission is rejected unless the operator explicitly
enables insecure remote transport, and WSS never downgrades to plaintext.

Without a configured static root, ordinary HTTP requests return 404 while the
WebSocket endpoint remains usable. A configured root is canonicalized and
retained as an open directory descriptor; files are served only through a
bounded component-by-component no-follow open relative to that descriptor.
Replacing the configured pathname after startup cannot redirect the service.
Repeated encoded traversal, dot segments, backslashes, NULs, directories,
symlink escape, unrecognized MIME types, and files beyond the configured
maximum asset size are denied. GET passes the final descriptor returned by the
secure walk to SNode.C 2.0's `FileReader::adopt()` and attaches it with
`Response::pipe()`. It never buffers the complete asset or reopens the
authorized pathname. Before a successful pipe, an application guard stops the
reader on a false return or exception; after success, SNode.C owns descriptor
streaming, disposal, and connection-local backpressure. The descriptor is
closed exactly once on either path.
`HEAD` sends the exact representation length without a body and closes its
temporary descriptor after establishing the response metadata. Static responses
send a CSP containing `frame-ancestors 'none'`,
`X-Content-Type-Options: nosniff`, and `Referrer-Policy: no-referrer`.
AISuite never logs bearer tokens, credentials, raw Hello messages, or
Authorization values. Framework logging remains SNode.C configuration;
operators must disable payload tracing in production.

## Handshake and commands

The context opens a transport-neutral `FrontendConnection` when the socket is
accepted, but no backend `FrontendSession` exists until authentication
succeeds. A verified local Unix client may retain the original Hello:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"hello","resumeAfter":41}
```

An untrusted or remote client sends the bearer only in Hello:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"hello","authentication":{"scheme":"bearer","token":"secret bytes"},"resumeAfter":41}
```

The server sends `welcome`, replay batches or a snapshot, and
`sync.complete`. Every new session is an observer. Controller acquisition is a
separate correlated command:

```json
{"protocol":"snodec.codex-frontend","version":1,"kind":"command","requestId":"role-1","method":"controller.acquire","params":{}}
```

Other observers remain connected and receive the same globally sequenced
canonical occurrences projected for their own scopes. An observer can
list/read threads and synchronize, but receives `permission_denied` for a
controller-required invocation. See the
[v1 protocol document](frontend-protocol-v1.md) and its
[JSON Schema](frontend-protocol-v1.schema.json) for every envelope and method.

Messages before authenticated Hello, malformed JSON, unsupported protocol
versions, unknown kinds, and command validation failures never reach the raw
Codex protocol. Pre-authentication failures reveal no privileged method,
parameter, provider, or deployment detail and close that client after a
bounded `protocol.error`. Post-handshake command errors normally keep the
connection open.

## Coalescing and outbound backpressure

The application-owned `FrontendService` subscribes once to BackendCore. Raw text,
reasoning, and command-output deltas have already accumulated in canonical
state before the service sees their backend transition. The service marks the
specific entity/channel dirty, schedules only one next-tick flush, replaces
obsolete intermediate state for the same key, and emits normalized events in
batches of at most 64 events and 256 KiB by default. Terminal and interactive
updates flush immediately. The replay journal holds at most 4,096 bounded
canonical records and 8 MiB, never the raw token stream or a
connection-specific serialized projection. Before retention, AISuite removes
known structured authentication and credential-bearing fields, reviewed
secret-response fields, and unsafe raw provider envelopes. Arbitrary bounded
user/model/tool/process/command-output text remains potentially sensitive; it
may remain canonical and is filtered per principal for snapshot, live, and
replay rather than subjected to unreliable heuristic secret scanning.

There are two independent per-client backpressure boundaries:

- `FrontendService` allows at most 512 queued protocol messages and 11 MiB of
  compact serialized JSON per connection, delivering at most 64 messages in
  one event-loop callback;
- each reference stream/WebSocket context allows at most 13 MiB outstanding in
  that connection's writer, including transport framing where applicable.

The limits intentionally have headroom in dependency order: the 8 MiB journal
counts canonical records, the 11 MiB service queue also accommodates bounded
replay batch and synchronization envelopes, and the 13 MiB writer additionally
accommodates framing and data already handed off by the service. Thus a
new Unix connection can replay a full default journal without being rejected
solely because downstream accounting includes envelope overhead. All three
limits remain finite. Deployments that customize one limit must adjust the
downstream limits consistently; a complete snapshot is one separate message
and a configured writer too small for it closes only that frontend.

When one atomic live occurrence cannot fit the event-batch limit,
FrontendService sends a bare live Snapshot barrier to the already Ready
connection. The client replaces its projected state without another Welcome or
SyncComplete. Expanded thread-list metadata is represented by one compact
`threadList.updated` family rather than by re-emitting every retained thread;
page threads remain their own ordinary upserts and an empty page fabricates no
thread.

If FrontendService requests a connection-local close, the native JSONL and
WebSocket adapters retain and log one bounded, control-character-safe lifecycle
reason. Complete protocol payloads, credentials, command parameters, and secret
reverse-response values are not logged.

No unbounded application queue is added. If either boundary cannot accept the
next message, the service closes that frontend, clears its queued data, and
detaches its session. A throwing send callback has the same local effect. One
slow observer cannot add data to the controller's queue, delay another client,
stop BackendCore, or stop the App Server. A disconnected controller releases
its role; pending approvals, user-input prompts, authentication requests, and
unknown requests remain pending and are never automatically answered.

All receive, delivery, coalescing, timer, and cleanup work runs through the
SNode.C event loop. The application adds no `std::thread`, sleep, polling loop,
future, or coroutine lifecycle.

HTTP admission adds no third output queue. WebSocket frames use the transport
writer's existing finite accounting; static responses use a descriptor source
with one bounded chunk and the HTTP pipe's suspend/resume lifecycle. The
accepted HTTP socket keeps its service-owned unauthenticated reservation until
transport teardown (or transfers it to the upgraded context), so slow
pre-Hello connections remain inside the same total, unauthenticated, and
handshake-timeout limits as every other frontend.

## Shutdown and operational limits

Stopping the global SNode.C loop closes accepted contexts and listeners before
FrontendService and BackendCore are destroyed. The Unix listener removes its
own socket and lock nodes on clean shutdown, subject to inode-identity checks.
BackendCore then stops the stdio App Server client, whose existing nonblocking
lifecycle owns child termination and reaping.

The default deployment is deliberately conservative: only Unix JSONL is
enabled, verified same-user trust is required for credential-free Hello, all
remote listeners are off, and all 15 conditional methods are off. TLS,
WebSocket, WSS, RFCOMM, Origin, static-root, bearer, and runtime-limit policies
are available only through explicit configuration and build support. The
service does not add durable authenticated-session or snapshot/journal disk
persistence.

No credentialed real-backend integration is registered in this milestone:
`SNODEC_RUN_CODEX_BACKEND_INTEGRATION=1` is therefore documented as a proposed
gate, not an implemented test switch. The ordinary deterministic suite uses a
fake App Server and requires no credentials, quota, network service, or real
model turn.

A1.7b implements authenticated Unix, IPv4/IPv6 JSONL, TLS JSONL, WebSocket,
WSS, optional RFCOMM composition, provider lifecycle, and scope-projected
state. A1.7c-1 adds the transport-neutral C++ Frontend SDK and migrates the
reference client while leaving the UI and browser unchanged.

A1.7c-2 immediately follows and migrates the
existing `codex-ui` into the canonical standalone AI IDE. No additional PR is
inserted before `codex-ui`. A1.7d owns the TypeScript Frontend SDK and browser
frontend. Provider-neutral architecture remains A2.
