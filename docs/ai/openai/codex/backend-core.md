# Codex BackendCore

`ai::openai::codex::backend::BackendCore<ClientT>` is the reusable, stateful
layer above the typed Codex App Server API. It directly owns one concrete
`AppServerClient`, reduces typed operation results, notifications, lifecycle
changes, diagnostics, and server requests into canonical state, and exposes
deterministic snapshots and transport-neutral frontend sessions.

The dependency direction is deliberately one way:

```text
frontend protocol or another in-process consumer
                    ↓
        ai-openai-codex-backend
                    ↓
             ai-openai-codex
                    ↓
              snodec::core
```

The backend library has no dependency on Unix sockets, `net/un`, socket
contexts, JSONL framing, Qt, WebSocket, or browser code. In particular, a Unix
socket path is not backend state. Concrete listener and framing code belongs in
`src/apps/codex-backend`.

## A1.6 completion and coverage boundary

Phase A0 pins the Codex CLI 0.144.6 stable and experimental App Server schemas
and registers every mechanically discovered protocol entry in the private
production `ProtocolSurfaceRegistry`. The generated
[coverage report](app-server-api-coverage.md) measures registry inventory,
typed wire support, BackendCore commands, canonical reducer state, Frontend
Protocol exposure, and owner security decisions independently. A registered
entry or a raw-preservation disposition is not BackendCore command or state
support.

A1.6a hardens lifecycle, recovery, freshness, capacity, snapshot, replay, and
module-consumer behavior. A1.6b completes all 86 stable application provider
commands, all 68 stable notifications, all ten stable server requests, and all
18 stable `ThreadItem` alternatives. The mechanically derived relevant-layer
denominator is therefore 86 + 68 + 10 + 18 + 16 = 198. Its final disposition
is 182 Implemented, 16 reasoned NotApplicable, and 0 NotImplemented.

`LayerDispositionReason` makes every NotApplicable state explicit. Internal
handshake identities use `InternalProtocolLifecycle`, type-only rows use
`TypeModelOnly`, the exact 13 action/result-only operations use
`ActionOnlyNoPersistentState`, the 16 `ResponseItem` alternatives use
`NoRuntimeBackendStatePath`, and experimental inventory uses
`ExperimentalInventory`. The canonical-state applicable denominator is frozen
independently at 169: 73 stateful operations, 68 notifications, 18
`ThreadItem` alternatives, and ten server requests. All 169 are Implemented.
The generator and C++ validator share the authoritative action-only identity
set; count and mutation guards reject a fourteenth action-only identity or any
attempt to improve coverage by shrinking the denominator. See the
[A1.6b completion report](a1-6b-backend-completeness.md) for the exact ledger.
A1.7 owns the multi-transport frontend service, while A2 remains
provider-neutral architecture.

## Ownership and construction

`BackendCore<ClientT>` directly owns exactly one concrete client:

```cpp
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/stdio/Client.h"

ai::openai::codex::backend::BackendCore<
    ai::openai::codex::stdio::Client
> backend;

backend.start();
```

`stdio::Client` is only one possible composition. `ClientT` must derive from
`AppServerClient`. A default-constructible client enables the zero-argument
form, and other client constructor arguments are perfectly forwarded. Backend
options are the first argument when both options and client arguments are
present:

```cpp
BackendCoreOptions options;
options.initialThreadListLimit = 2;

BackendCore<FakeAppServerClient> backend(options, fakeTransportState);
```

This lets deterministic tests directly own an `AppServerClient` backed by a
fake transport while using the same typed API and correlation registry. No heap
allocation or caller-managed lifetime is required merely to own the client.

The backend installs the client's lifecycle, diagnostic, typed-event, and
typed-server-request handlers. It does not instantiate a raw protocol engine,
does not allocate App Server client request IDs, and does not duplicate the
typed client's request-correlation registry. The small public template
delegates to a non-template runtime retained in `BackendCore.cpp`. That runtime
borrows the owned client through the existing `AppServerClient` abstraction.
Members are ordered so destruction first invalidates backend callbacks, stops
the client, and suppresses queued frontend callbacks; only then is the concrete
client destroyed.

The main API is:

```cpp
template <typename ClientT>
    requires std::derived_from<ClientT, AppServerClient>
class BackendCore {
public:
    BackendCore();
    explicit BackendCore(BackendCoreOptions options);

    template <typename... ClientArgs>
    explicit BackendCore(ClientArgs&&... clientArgs);

    template <typename... ClientArgs>
    BackendCore(BackendCoreOptions options, ClientArgs&&... clientArgs);

    void start();
    void stop();
    void restart();

    BackendState state() const;
    Snapshot snapshot() const;
    bool isReady() const noexcept;

    FrontendSession openSession(FrontendSessionCallbacks callbacks);
    BackendObserverSubscription subscribe(BackendObserverCallbacks callbacks);
};
```

`state()` is intended for trusted in-process diagnostics and tests. It includes
the exact typed pending request needed to preserve occurrence ownership.
Transport and UI integrations should consume `snapshot()`, which deliberately
removes occurrence tokens and sensitive implementation data.

## Canonical state and identifiers

`BackendState` uses the strong `SessionId`, `PendingRequestId`, and
`SequenceNumber` types. Session and pending-request IDs reserve zero as an
invalid value. They increase monotonically and do not silently wrap.

The state contains:

- `ProviderState`, including provider lifecycle, desired-running intent,
  generation, initialization metadata, recovery state, and last provider
  error;
- capacity limits, saturating rejection/eviction/drop counters, and retained
  state accounting;
- a bounded diagnostic summary;
- metadata-only provider-operation records and per-domain result summaries,
  containing method, result alternative, bounded subject/page metadata, and a
  source stamp rather than duplicate complete results;
- 42 named trusted replacement caches containing bounded typed copies for account
  login/read/rate/usage/messages, model list/capabilities, configuration/
  requirements/features/write/enablement, thread goal get/clear/set and
  unsubscribe/loaded threads, permission profiles/reviews, apps/external
  agents/hooks/marketplace, plugin install/catalog/detail/share/skill and
  skills list/configuration, MCP OAuth/status, and Windows readiness; wire-only
  raw data and diagnostics are discarded, while extra roots and large stream/
  search payloads use dedicated bounded state;
- bounded typed-notification markers grouped into account, model,
  configuration, conversation, filesystem, review, integration,
  plugin/skill, MCP, and platform domains;
- threads in deterministic first-seen order;
- each thread's typed summary and its known turns;
- each turn's typed status, deterministic item order, terminal/failure state,
  token usage, and a bounded model-reroute history;
- each item's typed representation, lifecycle timestamps, and separately
  accumulated agent text, reasoning text, reasoning summary, and command
  output;
- exact pending typed server requests, indexed by backend-generated
  `PendingRequestId`;
- bounded process, filesystem-watch, fuzzy-search, review/import/hook/OAuth,
  sandbox-setup, and other activity records;
- bounded notices for warnings, deprecations, configuration/security notices,
  and Windows world-writable warnings;
- connected sessions and controller ownership;
- thread-list pagination and completeness information;
- generation/freshness source stamps across provider-derived conversation and
  domain state, plus connection-invalidation markers for active turns, items,
  processes, watches, and searches; and
- the current backend revision plus bounded unknown-extension records.

Maps provide ID-based upsert semantics while explicit order vectors preserve
the server's deterministic first-seen order. An operation result and a later
notification for the same thread, turn, or item update the same entity; they do
not create duplicate state. The metadata ledgers do not serve as authoritative
typed caches and do not promote an entire domain's freshness. Each named cache
or domain entity carries the stamp of the result or event that actually
confirmed it.

The default reducer retains 64 diagnostics and 64 Codex extensions.
Individual diagnostic messages are capped at 16 KiB. Canonical extension
records cap the method at 4 KiB, the serialized payload at 64 KiB, and a
decoding error at 16 KiB. Structured decode-diagnostic surface, field path, and
message text use the corresponding method/error bounds and retain saturated
original-size accounting when truncated. Model reroutes are capped at 64 per turn, and each
accumulated item-content stream at 4 MiB. When accumulated content exceeds its bound, the reducer retains the
newest suffix and increments `droppedContentBytes`. Snapshots expose both the
dropped byte count and `contentTruncated`, so a consumer never mistakes a
bounded suffix for complete output. These bounds are configurable through
`ReducerOptions`; ordinary 1,000-delta test bursts remain below the defaults
and reconstruct exactly.

The typed `UserMessageThreadItem` separately retains its complete opaque content
array. Its normalized snapshot/event `data` object has a dedicated 65,536-byte
compact-serialization bound: `content` remains an array containing an ordered
prefix of complete, unmodified entries, and the adjacent
`contentTruncated`, `originalContentBytes`, `retainedContentBytes`,
`originalContentItems`, and `retainedContentItems` fields describe that
projection. The byte counts are compact serialized array sizes, so an empty
retained array counts as two bytes. This payload-specific truncation does not
alter the top-level `contentTruncated` or `droppedContentBytes`; those fields
retain their accumulated-visible-content meaning for every item type.

## Reducer semantics

All ordinary domain transitions pass through `Reducer::apply()`. Typed Codex
events first pass through `Reducer::translate()` and become deliberately named
backend events:

- `ProviderLifecycleChanged`, `ProviderConnectionInvalidated`,
  `CapacityConfigured`, `CapacityChanged`, and `DiagnosticReceived`;
- `ProviderOperationCompleted` plus process/watch/search admission and release
  transitions;
- `ThreadUpserted`, `ThreadListUpdated`, and `ThreadStatusUpdated`;
- `TurnUpserted`, `TurnCompleted`, `TurnFailed`, and `TurnErrorUpdated`;
- `ItemUpserted`, `ItemContentChanged`, and `FileChangeUpdated`;
- `TokenUsageUpdated` and `ModelRerouted`;
- `PendingRequestAdded` and `PendingRequestRemoved`;
- `ControllerChanged` and `SessionChanged`; and
- `CodexExtensionReceived`.

Every one of the 68 stable typed notifications produces at least one backend
event. Lasting account, configuration, model, conversation, process,
filesystem/search, review/security, integration, MCP, platform, realtime, and
notice meaning is reduced into the corresponding canonical domain. The
existing `error` notification remains on exactly one `TurnErrorUpdated` path.
The generic extension projection remains bounded and redacted and is used only
to preserve occurrence information where no additional durable meaning
exists; the exact typed notification is an ephemeral reducer input, while
session and observer queues receive its precomputed safe projection. A
provider notification is not delivered twice.

User messages and unknown typed items with a stable ID and envelope location
remain canonical items. Unknown items retain their common ID, thread, and turn
metadata along with raw JSON and any item-local decoding error. An item with a
valid location but no stable ID remains observable through the bounded
`codex/item-without-id` extension fallback. Unknown events, or malformed future
item events that cannot identify an owning thread and turn, remain observable
as bounded `CodexExtensionReceived` records with their original method or
deliberate extension name, payload, optional decoding error, and structured
forward-compatibility or protocol-warning classification. They do not fail the
backend and are not silently discarded.

The immutable public snapshot retains the newest 64 extension records under
stricter frontend-safe bounds: 1 KiB of UTF-8 method, 32 KiB of serialized
parameters, and 2 KiB of decoding error. Oversized parameters become an
explicit omission record with their original serialized byte count. Obvious
credential, authorization, password, token, answer, and secret-value keys are
redacted recursively (case-insensitively). The same sanitizer protects unknown
pending-request parameters. Thus neither an extension snapshot nor an unknown
request snapshot exposes App Server occurrence tokens, access tokens, or secret
answers.

The reducer updates canonical content immediately for every text or output
delta. Backend events describe transitions that have already been applied:
when an observer receives an event, an immediately obtained snapshot contains
that transition. Terminal turns, failed turns, completed items, pending
interactive requests, controller changes, and lifecycle failures are marked as
immediate-flush transitions for the frontend normalization layer.

## Snapshots and sequence semantics

`Snapshot` is an immutable-by-value view assembled deterministically from the
canonical maps and explicit order vectors. Two snapshots of unchanged state
compare equal. It contains the current backend revision, safe provider state,
recovery and initialization metadata, capacity/truncation accounting, source
freshness, diagnostics, ordered threads, turns and items, accumulated bounded
content, pending request summaries, controller, connected sessions,
thread-list completeness, bounded domain/process/watch/search/activity/notice
projections, and sequence-exhaustion state.

Snapshot creation never exposes pointers, callbacks, App Server client request
IDs, server-request occurrence tokens, authentication access tokens, or
user-input answers. Raw typed envelopes are not copied into normal snapshot
data. Known pending requests expose only the fields needed to render and answer
them. Unknown request details are explicitly labelled and bounded to 64 KiB.

The backend sequence starts at zero and increases once for each visible
backend-domain transition. It is an in-process revision, not the Codex
Frontend Protocol replay sequence. The frontend layer allocates its own
sequence only after normalization and coalescing and owns the bounded replay
journal. Backend snapshots therefore contain no replay range and BackendCore
accepts no replay command. Exhaustion fails explicitly through provider error
state without destroying the backend service; the number never wraps.

## Provider lifecycle and recovery

Provider lifecycle is not backend-service lifetime. `BackendCore`, frontend
sessions, controller ownership, observer subscriptions, the backend revision,
and retained conversation cache remain alive while the owned App Server client
stops, fails, or recovers. `ProviderState` exposes `Stopped`, `Starting`,
`Initializing`, `Ready`, `Stopping`, `Failed`, and `Recovering` separately from
the desired-running intent.

A1.6a fixes the former Failed-state restart defect. The underlying
`AppServerClient::start()` is now called only from its `Stopped` state:

```text
Failed -> stop() -> Stopping -> Stopped -> start()
```

`start()` sets the desired-running intent and starts only an already stopped
provider. It is idempotent in every active, stopping, failed, or recovering
state and never attempts a direct `Failed -> start()` transition. When SNode.C
is already stopping, admission fails before generation, lifecycle, callback,
refresh, or recovery bookkeeping changes, so the already-stopped provider
cannot become stranded in `Starting`. `stop()`
clears desired-running intent, cancels recovery, invalidates provider-scoped
callbacks and handles, completes attached commands as cancelled once, and
requests provider stop without closing sessions or releasing the controller.
`restart()` sets desired-running intent, supersedes an automatic recovery wait,
requests stop when necessary, and performs exactly one start after `Stopped`.

`BackendCoreOptions::recovery` is disabled by default for embedded consumers.
When enabled, only unexpected `Transport` and `Process` failures with a
classified error are retried. Launch, protocol, initialization, invalid-state,
capacity, cancellation, enqueue, and unclassified failures remain explicit
operator errors. Attempt zero means unlimited retries. Delay N is the
saturating, deterministic minimum of
`initialDelayMs * multiplier^(N - 1)` and `maximumDelayMs`; multipliers below
one normalize to one and the initial delay is capped by the maximum. Zero delay
still schedules asynchronously through the ordinary SNode.C event-loop timer.
Stop and destruction cancel the timer, manual restart supersedes it, and stale
queued callbacks are harmless. Exhaustion is represented by
`RecoveryStatus::Exhausted`; reaching Ready resets recovery and the last error.

## Generation, freshness, and hydration

The public provider generation starts at zero and increments exactly once
immediately before each accepted underlying start attempt. Session activity,
controller changes, failures, stops, and scheduling a retry do not increment
it. Provider operations capture that generation and a private callback epoch,
so a late completion from an invalidated connection cannot mutate a newer
generation.

All 86 provider operations use one common generation/epoch-guarded execution
path. Successful stateful operations publish reducer events before their exact
typed command completion is delivered. Conversation examples include:

- thread start and resume upsert the returned thread summary;
- thread list merges the returned page by ID and retains its cursors;
- thread read upserts the returned thread, turns, and items, marking it fully
  loaded when turns were explicitly requested;
- turn start upserts the returned turn; and
- turn interrupt completes from its typed result while later authoritative
  events determine terminal turn state.

On each `Ready` generation, the backend submits at most one initial thread-list
request. Its default limit is 50 threads. It never walks subsequent cursors
automatically, so startup cannot load unbounded history. Set
`initialThreadListLimit` to zero to disable this refresh or to another bounded
value for an application-specific policy. Initial hydration uses the same
global active-provider-operation capacity as frontend commands. If capacity is
zero or exhausted, the request is not submitted, one rejection and bounded
diagnostic are retained, list freshness remains Unknown or Stale, and the
provider stays Ready.

Provider-derived thread, turn, item, and list state carries a `SourceStamp`.
Only a result or typed event from the current generation marks the entity it
actually confirms `Current`. Item events do not promote stale parent turn or
thread metadata, and turn events do not promote a stale parent thread. Missing
parents created by a child event are identifiable current-generation backend
placeholders. Connection invalidation retains durable cache as `Stale`, marks
active turns and items `connectionInvalidated` without inventing a terminal
completed, failed, or cancelled state, clears pending server requests, and
invalidates provider-scoped handles. A reconnect alone does not make cached
data current. The bounded initial list refresh marks only entities actually
confirmed by that generation; unconfirmed cache remains stale and cursors are
not followed automatically. Backend and session callbacks use weak lifetime
guards so queued work cannot enter a destroyed backend.

## Capacity and snapshot bounds

`BackendCapacityOptions` has independent defaults of 128 sessions, 16
observers, 4,096 active operations, 1,024 pending server requests, 2,048
threads, 16,384 turns, 65,536 items, 64 MiB accumulated visible content, and an
8 MiB final snapshot. Domain-resource defaults additionally retain at most 256
notices, 256 processes, 4 MiB of output per process, 16 MiB of process output
globally, 1,024 filesystem watches, 256 fuzzy-search sessions, and 512 activity
records. Zero means zero capacity for deterministic boundary tests. For
snapshots, zero permits no optional payload; the mandatory valid summary
envelope is still returned and explicitly reports when it cannot fit. These
global limits supplement, rather than replace, the existing per-session,
observer-queue, extension, diagnostic, item-content, and realtime bounds.

Session or observer exhaustion rejects only the new handle. Active-operation
capacity is global across every one of the 86 provider commands and internal
initial hydration. Exhaustion completes an accepted backend command
asynchronously with `local_submission_failure`; denied hydration records a
rejection and diagnostic without provider submission. Every typed
server-request occurrence uses one pending slot, including attestation,
dynamic-tool, and MCP-elicitation requests. All ten stable request types now
have exact response semantics.
Pending typed server requests are never evicted or silently dropped: overflow
records a capacity event, invalidates and stops the provider connection, clears
provider-scoped occurrence ownership, and retains sessions and controller
ownership. The capacity error requires operator correction and an explicit
restart.

Process execution, filesystem watch creation, and fuzzy search reserve their
resource capacity before provider submission. Active resources are protected;
terminal processes and completed searches are evicted deterministically when
needed, while active watches are never evicted merely to admit another watch.
Each reservation is bound to the expected process, watch, or search identifier.
An early provider notification promotes only its matching reserved slot into
the concrete resource record without double counting or reporting a false
overflow; an unrelated notification cannot steal another pending operation's
reservation.
Unsolicited provider resources that cannot be represented fail the provider
closed. The per-process output ceiling applies to stdout and stderr combined;
process output then uses the global newest-suffix bound and saturating
dropped-byte accounting.

Conversation retention uses deterministic oldest-first scans over the explicit
thread, turn, and item order vectors. Active/nonterminal entities and entities
referenced by a pending request are protected. If no candidate can be evicted,
the exact provider operation may still complete while optional canonical
retention is omitted and accounted. Global accumulated presentation content is
trimmed from the oldest inactive terminal items first, then from remaining
oldest content only when necessary, while preserving the typed item value and
preferring newest content. All rejection, eviction, dropped-byte, overflow,
and snapshot-omission counters saturate rather than wrap and change only
through reducer-visible capacity events.

Canonical retained thread, turn, item, and accumulated-content counts are
updated incrementally at mutation points, as are notice, process,
process-output, watch, fuzzy-search, and activity counts. The ordinary reducer
path performs only O(1) counter comparisons and returns immediately while all
limits are satisfied. Pending-reference indexes and the relevant deterministic
order walk are used only after a structural limit is exceeded; content or
output is traversed only when its accumulated limit is exceeded.

Snapshot construction first creates the bounded, redacted projection and then
enforces `maxSnapshotBytes` deterministically. It omits oldest inactive state
before active entities, pending-request summaries, provider/recovery state,
controller/session summaries, error state, and capacity metadata. If that
mandatory core alone is too large, the result is a minimal valid truncated
snapshot rather than an exception. Its `mandatoryCoreExceedsLimit` flag makes
that structurally unavoidable condition explicit; for every representable
ceiling, the measured safe projection does not exceed `maxSnapshotBytes`.

## Sessions, observers, and callback ordering

`FrontendSession` is a move-only RAII handle. Destruction calls `close()` and
detaches the session. Every new session begins as an observer; opening a
session never grants controller authority implicitly.

```cpp
auto session = backend.openSession({
    .onEvents = [](const std::vector<SequencedBackendEvent>& events) {
        // Events are ordered backend-domain transitions.
    },
    .onSnapshot = [](const Snapshot& snapshot) {},
    .onCommandCompleted = [](const CommandCompletion& completion) {},
    .onClosed = [](const std::string& reason) {}
});

session.submit("client-1", ControllerAcquire{});
session.requestSnapshot();
```

Outbound session callbacks are scheduled through the SNode.C event loop by
default, are ordered, and never run inline as command completion callbacks.
One scheduled-drain guard collects a burst of backend events rather than
scheduling one callback per token delta. Consecutive events are delivered in
batches of at most 512 by default. Public callback exceptions are caught at the
boundary. A callback may submit another command, close its session, stop the
backend, destroy an adapter, or throw without unwinding into the event loop or
invalidating another session.

`BackendObserverSubscription` provides the same ordered, batched event feed to
one transport-neutral normalization adapter without creating controller state.
It is also move-only RAII. If its bounded queue overflows, intermediate backend
events are dropped and `onResynchronize` receives a current snapshot. This is
safe because the frontend layer derives normalized events from already reduced
state.

The default per-session queue bound is 4,096 entries and 8 MiB. The observer
queue has the same defaults. Both entry and approximate serialized-byte bounds
are configurable. These queue bounds are independent from the global defaults
of 128 live sessions and 16 observer subscriptions; exceeding a global count
returns only an invalid new handle and leaves existing consumers unchanged. A
session that exceeds either queue limit is closed and all its
queued data is released; its controller role is released if necessary. That
does not stop `BackendCore`, the App Server, another observer, or the
controller. Observer-subscription overflow uses snapshot resynchronization
instead of allowing unbounded growth.

Tests can replace the default `core::EventReceiver::atNextTick()` scheduler via
`BackendCoreOptions::scheduler`. A deterministic scheduler can append callbacks
to a deque and advance one event-loop tick explicitly. Production code should
retain event-loop scheduling rather than introduce threads or blocking waits.

## Controller policy and commands

There is at most one controller. Admitted sessions may act as observers within
the configured session and observer-subscription limits. `BackendCommand` is a
101-alternative C++ value variant independent of JSON: three control commands,
all 86 stable provider operations, and 12 exact response/rejection commands.
`ProviderOperationValue` contains 65 exact typed result alternatives;
`CommandValue` contains the three control values followed by those alternatives
as one flat 68-alternative variant. Exact thread/turn operation wrappers are not
flattened to their nested entity.

One exhaustive visitor assigns every command a `CommandPolicy`; there is no
default policy for a future alternative. Any connected trusted in-process
session may acquire/release the controller, obtain a snapshot, or submit these
read-only provider operations while the provider is Ready:

```text
account/rateLimits/read       account/read
account/usage/read            account/workspaceMessages/read
config/read                   configRequirements/read
experimentalFeature/list     model/list
modelProvider/capabilities/read
thread/list                   thread/loaded/list
thread/read                   thread/goal/get
fs/getMetadata                fs/readDirectory
fs/readFile                   fuzzyFileSearch
permissionProfile/list        app/list
externalAgentConfig/detect    externalAgentConfig/import/readHistories
hooks/list                    plugin/installed
plugin/list                   plugin/read
plugin/share/list             plugin/skill/read
skills/list                   mcpServer/resource/read
mcpServerStatus/list          windowsSandbox/readiness
```

`account/read` is observer-readable only when `refreshToken` is absent or
false. A true value intentionally refreshes authentication material and is
controller-only. Every other provider mutation and all request responses or
rejections require the controller and a Ready provider.

The filesystem read operations above are observer-readable only at the
trusted in-process BackendCore authorization boundary. This is not permission
to expose them through an out-of-process protocol. Frontend Protocol v1 does
not map them, and A1.7 must perform a separate transport/frontend security
review before any remote exposure.

Frontend replay remains handled entirely by the frontend journal. Controller
acquisition succeeds when there is no controller or idempotently for the same
session. A different session receives `conflict`; release succeeds only for
the current controller.

When the controller disconnects, ownership is released and remaining sessions
are notified. The App Server keeps running. Pending requests remain pending,
are neither approved nor rejected, and can be answered by a later controller.

Each submitted command has a nonempty, session-local request ID. A duplicate ID
is rejected while its earlier command is pending. IDs in different sessions do
not conflict. An accepted command produces exactly one asynchronous
`CommandCompletion` unless that session closes first. Closing a session
suppresses later completions and retires its session-attached active-operation
bookkeeping. A later provider result is ignored by BackendCore's guarded
callback and cannot mutate shared state.

The stable command error categories are:

```text
permission_denied
invalid_command
not_found
conflict
local_submission_failure
typed_decoding_failure
remote_app_server_error
cancelled
backend_unavailable
```

They preserve an optional remote code and bounded details without reducing the
contract to `errno`.

## Pending request ownership

Each incoming typed server request receives a monotonically increasing
`PendingRequestId`. Canonical state retains the exact typed request, including
its `ServerRequestToken`, so the existing typed layer continues to protect
against App Server JSON-RPC ID reuse. The token is never a frontend identifier
and never appears in a snapshot.

A pending request is removed only after the typed `respond`, `respondRaw`, or
`reject` call successfully enqueues the response. BackendCore first verifies
that the reducer has sequence capacity to publish that removal. If sequence is
exhausted, it returns `backend_unavailable` without sending a response and
retains the occurrence. Validation, encoding, or enqueue failure returns
`local_submission_failure` and also retains the request for retry. A successful
enqueue immediately publishes `PendingRequestRemoved` before completing the
backend command. App Server stop, failure, or other connection invalidation
clears all pending requests because their typed occurrence ownership is no
longer valid.

All ten stable typed request alternatives have an exact BackendCore response
path. The application-friendly approval, user-input, and authentication
commands accept both their convenience values and their complete typed schema
response alternatives. Dedicated commands cover apply-patch approval,
exec-command approval, permissions approval, attestation, dynamic tool calls,
and MCP elicitation. `KnownRequestReject` is limited to the four known request
types whose typed façade supports `reject()`; raw response/rejection commands
remain restricted to `UnknownServerRequest`. No response payload containing
authentication or elicitation data is copied into `CommandValue`, snapshots,
diagnostics, or logs.

`serverRequest/resolved` removes a matching current-generation pending request
once. Duplicate, unknown, or stale-generation notifications are idempotent. A
conflicting thread association retains the occurrence and records a bounded,
redacted extension marker rather than removing the wrong request. The provider
request ID and typed occurrence token remain absent from public snapshots.

Controller disconnect is not connection invalidation. It does not remove,
approve, decline, reject, or otherwise answer any request. BackendCore contains
no automatic approval or rejection policy.

## Frontend Protocol v1 boundary

Completing BackendCore does not add provider methods to Frontend Protocol v1.
Its identity, version, message kinds, command subset, JSON fields, replay,
coalescing, batching, controller rules, and error codes remain unchanged. The
adapter projects `ThreadStartResponse`, `ThreadResumeResponse`, and
`ThreadReadResponse` through their `.thread` member and `TurnStartResponse`
through `.turn`, preserving the existing v1 JSON. `ThreadListResponse` and
`typed::Unit` keep their existing page and empty-object projections.

The other 59 provider result alternatives are unreachable from the v1 command
set. The adapter exhaustively recognizes them but reports an internal adapter
error if one arrives unexpectedly; it neither invents a v1 representation nor
exposes raw typed state. Stable notifications continue through the existing
dedicated normalized events or the single bounded `codex.extension` contract,
without duplicate delivery. Frontend protocol expansion, new snapshot fields,
additional transports, SDKs, and UI security decisions remain A1.7.

## Installed module consumption

AISuite consumes the installed SNode.C CMake package as a module. Reusable
Codex libraries link the canonical `snodec::core` target directly; the removed
generic `core` and `net-un-stream-legacy` forwarding wrappers are not part of
the build. A library-only `find_package(AISuite CONFIG REQUIRED)` consumer does
not inherit the Unix transport dependency.

A staged Unix server composition explicitly finds both packages and links only
public targets:

```cmake
find_package(snodec CONFIG REQUIRED COMPONENTS
    core
    net-un-stream-legacy
)
find_package(AISuite CONFIG REQUIRED)

target_link_libraries(module_server PRIVATE
    AISuite::OpenAICodex
    AISuite::OpenAICodexBackend
    AISuite::OpenAICodexFrontend
    snodec::core
    snodec::net-un-stream-legacy
)
```

The installed module-consumer test instantiates
`BackendCore<stdio::Client>`, the public frontend adapter, and a minimal
Unix-domain SNode.C server using only staged headers and packages. It uses no
AISuite source include path, private header, real Codex credential, or fixed
dependency commit.

A1.6b remains inside the intentionally unreleased SOVERSION-2 development ABI
boundary. Project version remains `0.1.0`, the installed public-header inventory
remains 29 main, 7 backend, and 7 frontend headers, and no compatibility copy
of the former flattened command-result API is installed.
