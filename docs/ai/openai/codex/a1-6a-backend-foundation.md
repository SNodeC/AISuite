# A1.6a backend foundation

A1.6a hardens the reusable Codex backend without expanding provider-operation
coverage. `BackendCore<ClientT>` still directly owns one provider client and
one non-template runtime, reducer, canonical state, operation bookkeeper, and
recovery timer. Frontend sessions and observers borrow that service; they do
not own or duplicate the App Server lifecycle.

The scope boundary is exact:

- stable application provider operations: 86;
- implemented provider commands: 6;
- missing provider commands reserved for A1.6b: 80;
- backend/state denominator: 198;
- A1.6a disposition: 32 Implemented, 16 reasoned NotApplicable, and 150
  NotImplemented, for 48 resolved entries.

A1.6a adds none of the 80 missing provider commands and does not claim
198-entry closure. A1.6b owns 86/86 provider commands, complete stable request,
notification, and `ThreadItem` handling, and the final reasoned denominator.
A1.7 owns the multi-transport frontend service and SDK/UI work. A2 remains the
provider-neutral phase.

## Ownership and service lifetime

The canonical composition remains:

```text
BackendCore<ClientT>
    ├── owns one ClientT
    └── owns one BackendCoreRuntime
            ├── borrows AppServerClient&
            ├── owns one BackendState and Reducer
            ├── owns sessions and observers
            ├── owns active-operation bookkeeping
            └── owns recovery bookkeeping and one timer
```

There is still one SNode.C event loop, transport, JSON-RPC engine, provider
generation, typed event path, raw path, pending-operation map, reverse-request
occurrence registry, callback scheduler, and cancellation lifecycle. A
provider connection can fail or restart without destroying `BackendCore`,
closing frontend sessions, or releasing controller ownership.

## Provider lifecycle and the Failed restart fix

`BackendState::provider` is the canonical provider model:

```cpp
struct ProviderState {
    ProviderLifecycle lifecycle;
    std::uint64_t generation;
    bool desiredRunning;
    std::optional<Error> lastError;
    RecoveryState recovery;
    std::optional<typed::InitializeResponse> initialization;
};
```

`ProviderLifecycle` distinguishes `Stopped`, `Starting`, `Initializing`,
`Ready`, `Stopping`, `Failed`, and `Recovering`. This state is separate from
the lifetime of the backend service and its frontend sessions.

The former backend attempted to recover by calling `AppServerClient::start()`
while the client was still Failed, although that client accepts startup only
from Stopped. A1.6a makes the legal transition explicit:

```text
Failed -> stop() -> Stopping -> Stopped -> start()
```

- `start()` records desired-running intent and starts only an underlying
  Stopped client. It is idempotent in other lifecycle states and never performs
  `Failed -> start()`. If the shared SNode.C loop is already stopping, startup
  is rejected before generation, lifecycle, callback, refresh, or recovery
  state changes; the provider remains coherently Stopped.
- `stop()` clears desired-running intent, cancels recovery, invalidates current
  operation callbacks and provider-scoped state, cancels attached commands
  exactly once, and requests provider stop. Sessions and controller survive.
- `restart()` sets desired-running intent, supersedes an automatic recovery
  wait, requests stop when needed, waits for Stopped, then starts exactly once.
  It is safe from Ready, Failed, Recovering, Starting, Initializing, Stopping,
  and Stopped.

## Recovery policy

Embedded `BackendCore` instances default to recovery disabled. Recovery is
eligible only while the provider is desired-running, recovery is enabled, a
classified error exists, and the category is `Transport` or `Process`.
Launch, protocol, initialization, invalid-state, capacity, cancellation,
enqueue, and unclassified failures require operator correction and manual
restart.

```cpp
struct RecoveryOptions {
    bool enabled = false;
    std::uint32_t maximumAttempts = 0;
    std::uint64_t initialDelayMs = 1000;
    std::uint64_t maximumDelayMs = 30000;
    std::uint32_t multiplier = 2;
};
```

Zero maximum attempts means unlimited retries. Attempt N waits the saturating
minimum of `initialDelayMs * multiplier^(N - 1)` and `maximumDelayMs`.
Multipliers below one normalize to one, the initial delay is capped by the
maximum, and a zero delay still schedules asynchronously. One ordinary SNode.C
single-shot timer performs the wait; no thread, sleep, future, coroutine, or
polling loop is introduced. Stop and destruction cancel it, restart supersedes
it, and token plus weak-lifetime checks make stale callbacks harmless.

The `codex-backend` reference application opts in by default with unlimited
attempts and the 1,000/30,000/2 delay policy. Its SNode.C/CLI configuration can
change the enabled flag, attempt limit, initial delay, maximum delay, and
multiplier. Reaching Ready clears the last error, resets attempts and delay,
and records safe initialization metadata. Finite-attempt exhaustion remains
visible as `RecoveryStatus::Exhausted`.

## Generation, freshness, and invalidation

The public provider generation begins at zero and increments once immediately
before each accepted underlying start attempt. It does not change for sessions,
controllers, snapshots, failure observation, stop requests, or retry
scheduling. Each operation captures the generation and a private callback
epoch, so an old completion cannot mutate a new provider generation.

Provider-derived threads, turns, items, and thread-list state carry:

```cpp
struct SourceStamp {
    std::uint64_t generation;
    Freshness freshness; // Unknown, Current, or Stale
};
```

A current-generation operation result or typed event marks only the confirmed
entity Current. In particular, an item update confirms only that item; a turn
update confirms the turn without promoting its parent thread. Missing parents
created to locate a child are explicitly marked backend placeholders rather
than copies of stale metadata. Provider connection invalidation passes through
`ProviderConnectionInvalidated` and the reducer: retained conversation cache
becomes Stale, active turns and items become `connectionInvalidated` without a
fabricated terminal state, pending server requests are cleared, and
provider-scoped handles become invalid. Sessions, controller, revision, and
bounded diagnostics/extensions remain.

On Ready, at most one bounded initial thread-list request runs for that
generation when initial hydration is enabled. It consumes the same global
provider-operation capacity as a frontend-submitted command. At zero or
exhausted capacity, hydration is skipped with one rejection and bounded
diagnostic while the provider remains Ready and list freshness remains
Unknown or Stale. Only returned entities become Current. Unconfirmed cached
entities remain Stale, and the backend does not follow pagination cursors or
claim complete history automatically.

## Capacity and bounded snapshots

`BackendCapacityOptions` defaults are:

| Limit | Default |
|---|---:|
| Sessions | 128 |
| Observers | 16 |
| Active operations | 4,096 |
| Pending server requests | 1,024 |
| Retained threads | 2,048 |
| Retained turns | 16,384 |
| Retained items | 65,536 |
| Accumulated visible content | 64 MiB |
| Final snapshot | 8 MiB |

Zero means zero capacity. These global bounds coexist with the established
per-session/observer queues and per-item, extension, and diagnostic bounds.
Capacity counters record rejected sessions, observers, and operations;
provider-request overflows; evicted threads, turns, and items; dropped content;
and snapshot omissions. Accounting saturates rather than wrapping and changes
through reducer-visible capacity events.

Session and observer exhaustion rejects only the new handle. The active-
operation limit is global across session-attached provider commands and
internal initial hydration. Exhaustion completes an accepted frontend command
asynchronously with the existing `local_submission_failure` response; denied
hydration is not submitted. Every typed server-request occurrence consumes a
pending slot, including the attestation, dynamic-tool, and MCP-elicitation
requests whose response commands remain reserved for A1.6b. Pending requests
are never silently discarded or evicted: overflow fails closed by recording
capacity failure, invalidating and stopping the provider, and retaining
frontend sessions/controller. No request is approved, rejected, or moved
outside canonical bounded state.

Conversation limits use deterministic oldest-first order. Active/nonterminal
entities and entities referenced by pending requests are protected. If no
candidate is evictable, optional retention is omitted and accounted without
changing the exact provider command result or corrupting order vectors. Global
presentation content is trimmed from the oldest inactive terminal items first,
then from remaining oldest content only when necessary. The canonical typed
item value remains unchanged and newest content is preferred.

Retained thread, turn, item, and visible-content counts are canonical,
incremental state. Ordinary under-limit events perform four constant-time
limit comparisons and return without rebuilding pending-reference indexes or
walking retention order. Reference indexes and deterministic eviction walks
are constructed only when a corresponding structural limit is exceeded;
content traversal occurs only when the global content limit is exceeded.

Snapshot creation remains deterministic, redacted, exception-contained, and by
value. It omits oldest inactive projections before active entities, pending
request summaries, provider/recovery/error state, controller/session summaries,
and capacity metadata. If the mandatory core itself exceeds the configured
ceiling, the result contains only that valid core and explicitly sets both
`truncated` and `mandatoryCoreExceedsLimit`; zero therefore permits no optional
snapshot payload and is never interpreted as unlimited capacity. Representable
ceilings are enforced against the measured safe projection.

## Replay ownership

BackendCore no longer exposes `ReplayAfter`, `ReplayResult`, or
`Snapshot::replayRange`. Its `sequence` remains a monotonic canonical-state
revision. Frontend Protocol v1 `events.replay` remains byte- and
semantics-compatible because `frontend::EventJournal` and the shared
`BackendAdapter` remain the sole replay authority, including replay gaps,
snapshot fallback, `sync.complete`, backpressure, and the frontend sequence.

The A1.7 ownership requirement is one `BackendCore`, one shared
`BackendAdapter` and frontend journal, and multiple future Unix, IPv4, IPv6,
RFCOMM, and WebSocket factories. Those transports must share one replay
sequence; A1.6a does not implement them.

## Reasoned backend/state disposition

Every NotApplicable registry layer status now carries an explicit
`LayerDispositionReason`: `InternalProtocolLifecycle`, `TypeModelOnly`,
`ActionOnlyNoPersistentState`, `NoRuntimeBackendStatePath`, or
`ExperimentalInventory`. Implemented and NotImplemented entries require
`None`. Mechanical validation prevents a stable application operation,
notification, `ThreadItem`, or server request from being marked NotApplicable
to manufacture closure. Each of the 16 stable `ResponseItem` alternatives is
NotApplicable specifically because no runtime backend-state path exists.

Coverage is always reported in separate columns, never merely as “198/198”:

| Backend/state disposition | Count |
|---|---:|
| Implemented | 32 |
| NotApplicable | 16 |
| NotImplemented | 150 |
| Resolved | 48 |
| Total | 198 |

BackendCore stable operation commands are separately **6/86 Implemented**.
A1.6b, not A1.6a, is responsible for eliminating the remaining 80 command gaps
and resolving the remaining backend/state entries.

## Installed SNode.C module composition

AISuite uses installed SNode.C packages and canonical targets. The generic
extraction-era `core` and `net-un-stream-legacy` forwarding targets are gone.
Reusable libraries require `snodec::core`; Unix transport is requested only
when applications or tests need `snodec::net-un-stream-legacy`. The installed
AISuite package therefore does not impose Unix transport on a library-only
consumer.

The staged module-consumer test is a separate CMake tree using only installed
prefixes and public headers. It finds SNode.C `core` and
`net-un-stream-legacy`, finds AISuite, then composes a minimal Unix-domain
SNode.C server with `BackendCore<stdio::Client>` and the public frontend
adapter. It links only:

```text
AISuite::OpenAICodex
AISuite::OpenAICodexBackend
AISuite::OpenAICodexFrontend
snodec::core
snodec::net-un-stream-legacy
```

The composition requires no real Codex credentials and no AISuite private or
source-relative include path. `codex-backend` follows the same public module
boundary and remains Unix-domain only in A1.6a.

## Preserved protocol and ABI boundary

A1.6a does not modify Codex schemas, registry implementation status, typed
codecs, descriptors, method strings, variant order, request correlation,
Frontend Protocol v1, or A1.5's public application façade. Typed protocol state
remains 339 Complete / 0 Partial / 0 NotImplemented / 48 NotApplicable.
Codex SOVERSION remains 2.
