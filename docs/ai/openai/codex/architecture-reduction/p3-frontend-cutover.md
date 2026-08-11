# P3 Codex frontend production cutover contract

Status: frozen at the P3 base. This document is the design authority for the
seven-commit P3 cutover. The current Task-A execution is limited to Commits 1–3;
native transport consolidation, WebSocket composition, legacy deletion, and
final closure are future work and must not be inferred from this document's
presence.

## Provenance and compatibility boundary

| Item | Frozen value |
| --- | --- |
| Repository | `SNodeC/AISuite` |
| P3 branch | `codex/p3-frontend-cutover` |
| P3 base (`origin/master` at branch creation) | `7e68847e14753553402e1d468c3af15a148eea80` |
| Merged P2 PR | `#16`, base `master`, state `MERGED` |
| Merged P2 merge commit | `7e68847e14753553402e1d468c3af15a148eea80` |
| AISuite project version | `0.1.0` |
| Codex SOVERSION | `2` |
| Frontend Protocol | `snodec.codex-frontend`, version `1` |
| SNode.C package selected by the reused build | active-cache `snodec_DIR`, observed package version `2.0.0` |
| Accepted SNode.C source authority | dependency checkout selected by `.github/workflows/ci.yml` at the dynamic P3 base |

The base was derived from the fetched repository and GitHub at execution time;
it is not a prompt-supplied P2 SHA. PR #16 is merged, its base is `master`, its
merge commit is reachable from `origin/master`, and local `master` equalled
`origin/master` with a clean worktree before this branch was created.
SNode.C source provenance is resolved from that merged CI authority and the
active build at execution time. P3 records the derived provenance in execution
reports but adds no SNode.C commit pin or machine-local checkout path.

P3 preserves the project version, all SOVERSION values, output-library and
shared-library identities, installed target names, installed header paths,
public class layouts, constructors, methods, callbacks, aliases, generated
facades, enums and value semantics, Frontend Protocol identity/version, CLI
syntax, all eleven transport compositions, configuration hierarchy, defaults,
and security policy. No public semantic transport type, virtual interface,
second protocol target, or installed internal model/core/bridge header may be
introduced. P3 does not modify SNode.C, SNode.C provenance, `codex-ui`, or the
Frontend Protocol schema and generated wire identity.

The installed targets at the base are:

- `AISuite::OpenAICodex`
- `AISuite::OpenAICodexBackend`
- `AISuite::OpenAICodexFrontend`
- `AISuite::OpenAICodexFrontendClient`
- `AISuite::OpenAICodexFrontendProtocol`

The complete public-header inventory and file dispositions are machine-readable
in `p3-frontend-cutover-manifest.json` beside this document.

The five Task-A steering corrections are part of this design authority:

1. the exact future Commit-5 head, with the complete legacy implementation and
   oracle still present, must pass independently verified hosted full closure
   before Commit 6 may begin;
2. the future WebSocket upgrade handoff must be file-private, owner-event-loop
   confined, and RAII-scoped, and must never use `thread_local`;
3. future Commit 4 has a hard source-and-executable proof gate for an explicit
   later `connect()` on the same configured SNode.C client object;
4. every local build and CTest invocation retains exactly 24-way parallelism,
   every CTest retains the 60-second timeout, and prohibited long gates remain
   hosted-only; and
5. `EventCoalescer`, `EventJournal`, and `UpdateBatchBuilder` remain
   server-DSO-only compatibility utilities and cannot create any frontend-client
   header, source, link-interface, or transitive dependency.

These corrections freeze future execution rules; they do not authorize Task A
to start Commits 4–7.

## Starting ownership and dependency graph

P2's permanent authorities exist beside the production implementation. The
direct build-tree target edges at the frozen base are:

```text
AISuite::OpenAICodex
  PUBLIC  -> nlohmann_json::nlohmann_json
  PRIVATE -> snodec::core

AISuite::OpenAICodexBackend
  PUBLIC  -> AISuite::OpenAICodex
  PRIVATE -> snodec::core

AISuite::OpenAICodexFrontendProtocol
  PUBLIC  -> nlohmann_json::nlohmann_json

ai-openai-codex-frontend-model
  PUBLIC  -> AISuite::OpenAICodexFrontendProtocol

ai-openai-codex-frontend-server-core
  PUBLIC  -> ai-openai-codex-frontend-model
          -> AISuite::OpenAICodexFrontendProtocol
          -> AISuite::OpenAICodexBackend
          -> AISuite::OpenAICodex

ai-openai-codex-frontend-client-core
  PUBLIC  -> ai-openai-codex-frontend-model
          -> AISuite::OpenAICodexFrontendProtocol
          -> AISuite::OpenAICodex

AISuite::OpenAICodexFrontend
  PUBLIC  -> AISuite::OpenAICodexBackend
          -> AISuite::OpenAICodexFrontendProtocol
  PRIVATE -> snodec::core

ai-openai-codex-frontend-client-objects
  PRIVATE -> AISuite::OpenAICodexFrontend

AISuite::OpenAICodexFrontendClient
  PUBLIC  -> AISuite::OpenAICodexFrontend
```

`ServerCore`, `BackendProjection`, `ClientCore`, the typed model, typed
occurrence authority, typed journal, the closed pending-Snapshot sequence-mode
correction, UTF-8-safe pending user-input projection, and the P2 differential
fixtures/coverage authority are present. The public production Pimpls still use
the legacy engines at the base:

```text
codex-backend
  -> AISuite::OpenAICodexFrontend
  -> FrontendService.cpp legacy journal/coalescer/replay/projection/session/controller engine

codex-backend-client
  -> AISuite::OpenAICodexFrontendClient
  -> Client.cpp legacy lifecycle/synchronization/correlation engine
  -> State.cpp + detail/StateReducer.h JSON state authority
```

Thus the starting client target's public target closure includes
`AISuite::OpenAICodexFrontend`, `AISuite::OpenAICodexBackend`,
`AISuite::OpenAICodex`, `AISuite::OpenAICodexFrontendProtocol`, and
`nlohmann_json::nlohmann_json`. Its shared-library/runtime closure also reaches
SNode.C through the private `snodec::core` links on `OpenAICodexFrontend`,
`OpenAICodexBackend`, and `OpenAICodex`. A private shared-library edge is not a
propagated compile usage requirement, but it remains part of the starting
runtime dependency closure. P3 must remove the client-to-server/backend/SNode.C
closure. The protocol DSO remains the sole protocol-symbol authority.

The compatibility utilities `EventCoalescer`, `EventJournal`, and
`UpdateBatchBuilder` are frozen server/frontend-library ABI. They remain owned
and implemented by `AISuite::OpenAICodexFrontend`. They are not client SDK
utilities: no public client header or client source may include them, and the
final `AISuite::OpenAICodexFrontendClient` target may not link the server DSO to
obtain them. A dependency-policy assertion must enforce the header, source,
exported-interface, and transitive target boundaries.

## Final semantic ownership

The server cutover is one architecture:

```text
Codex App Server
      |
      v
BackendCore
      |
      v
BackendCoreBridge
      |
      v
ServerCore
      |
      v
FrontendService / FrontendConnection public Pimpls
      |
      +---- bounded JSONL native transports
      +---- WebSocket/WSS text transports
```

`BackendCoreBridge` borrows the existing `BackendCoreRuntime`, owns exactly one
shared `BackendObserverSubscription`, and owns one BackendCore
`FrontendSession` for each authenticated ServerCore frontend session. The
per-session `onEvents` and `onSnapshot` callbacks are empty; command completion
and backend-session closure are their only callbacks. The shared observer
classifies BackendCore topology by private `SessionId`. It suppresses active
bridge-owned echoes and retired bridge-owned late echoes; those BackendCore IDs
never appear on the wire. Independent BackendCore sessions are mapped for their
lifetime to non-reused Frontend Protocol `DecimalId` `SessionIdentity` values
allocated from `UINT64_MAX` downward, while ServerCore frontend identities stay
in the disjoint low range from 1 through `INT64_MAX`.

External open, close, and controller transitions are staged exactly once
through ServerCore and participate in live delivery, journal replay, snapshot
barriers, and `currentController()`. Snapshot composition joins ServerCore-owned
frontend sessions with mapped external BackendCore sessions and exposes one
coherent controller fact. All non-topology events continue through
`BackendProjection::projectOccurrences`, `ServerCore::stageGroup`/
`requireSnapshot`, and observer resynchronization through
`BackendProjection::projectSnapshot` and `ServerCore::publishSnapshot`.

Authentication order is fixed as physical connection, Hello, authentication,
ServerCore session creation, BackendCore session creation, then Welcome and
synchronization. A failed BackendCore session creation closes/rejects the
authenticated frontend without retaining a half-open core session or exposing
the backend session ID.

Controller acquire/release is an asynchronous transaction through the mapped
BackendCore session. ServerCore performs frontend validation and reserves the
correlation first. It does not commit canonical controller state before a
successful backend completion. Failure leaves canonical state unchanged;
success commits once, emits the canonical frontend occurrence once, and sends
the correlated generated result. Connection generation and outstanding-token
checks discard stale/late completions. Closing the controller frontend clears
ServerCore ownership and closes its BackendCore session, which ultimately
releases BackendCore ownership; reconnect never restores it.

Provider and reverse commands retain the generated method identity, use
`BackendCommandMapper`, and map typed completions through
`ProviderResultProjection` plus the frozen legacy-result compatibility cases.
There is no second command/result table and arbitrary backend JSON is not a
protocol authority. Provider lifecycle start/stop/restart retains the P2
generation and reentrant-continuation checks.

The client cutover is one architecture:

```text
persistent configured transport connector
      |
      v
public Client / generation-bearing Connection Pimpl
      |
      v
ClientCore
      |
      v
CanonicalStateBuilder
      |
      v
existing private StateStorage -> immutable public State
```

Every public/generated facade continues through the existing central submission
border, which delegates to one `ClientCore`. One internal prepare/commit seam
builds a candidate public State directly from the candidate canonical typed
publication and enforces public accounting before ClientCore commits. On
success, ClientCore commits, the prepared immutable State is installed without
failure, and callbacks run in their frozen order. On preparation failure,
neither canonical nor public revision/cursor advances; the prior public State is
retained and becomes stale where the existing capacity/connection policy
requires it. No rollback occurs after user callbacks. Production never encodes
the canonical model as Frontend Protocol JSON, reparses it, or calls the legacy
`StateReducer`.

The final frontend client target has only:

- public `AISuite::OpenAICodexFrontendProtocol`;
- `AISuite::OpenAICodex` only if an existing installed typed header actually
  requires that permitted relationship and it can still satisfy the harder
  no-SNode.C transitive-closure rule;
- private P2 typed-model/client-core and `CanonicalStateBuilder` ownership.

It has no direct or transitive edge to `AISuite::OpenAICodexFrontend`,
`AISuite::OpenAICodexBackend`, ServerCore, BackendCore, SNode.C, compatibility
utilities, or a concrete transport.

The frozen compatibility solution for installed typed client headers is a
narrow source-authority reuse, not an `AISuite::OpenAICodex` target edge. The
client DSO may compile the transport-free `typed/Accounts.cpp`,
`typed/Configuration.cpp`, `typed/Models.cpp`, and `typed/Types.cpp` helpers
with public visibility, while all transport-free detail codecs required by the
generated façades remain hidden. This preserves client-only linkage for the
existing out-of-line typed value factories without importing OpenAICodex's
SNode.C runtime closure. It creates no second Frontend Protocol object copy,
no installed target, and no second client state or protocol authority. The
typed definitions are compiled from the same four source authorities as
OpenAICodex; their additional client-DSO exports are ABI-additive and must be
covered by source-link and symbol-policy tests. Hosted platform closure must
still verify non-ELF export behavior. Because the same four public typed
definitions are exported by two DSOs, cross-DSO duplicate-symbol/interposition
behavior and non-ELF export/link behavior remain hosted-platform closure
uncertainties; this Task-A correction does not redesign the accepted solution.

ClientCore mutation-only conformance operations are no longer public members of
the non-installed core class. A private `ClientCoreTestAccess` friend owns the
deterministic overflow, revision, generation, dispatch-failure, and erasure
seams, mirroring the server bridge test-access pattern without changing an
installed header, layout, export, or ABI. Tests-enabled builds intentionally
reuse the production objects, so the friend functions remain hidden local code
in that configuration. Producing a second test-only core/client object variant
solely to remove hidden code from the tests-enabled DSO is deferred as
disproportionate build-topology churn; tests-disabled physical elimination is
not required for this non-public F6 correction.

## Eleven preserved transport contracts

P3 retains these server/client pairs and their names, options, configuration
hierarchy, security defaults, and enabled state:

1. Unix JSONL (`codex-backend` / `codex-backend-client-unix`), enabled by default.
2. IPv4 JSONL (`codex-backend-ipv4` / `codex-backend-client-ipv4`), disabled by default.
3. IPv6 JSONL (`codex-backend-ipv6` / `codex-backend-client-ipv6`), disabled by default.
4. IPv4 TLS JSONL (`codex-backend-tls-ipv4` / `codex-backend-client-tls-ipv4`), disabled by default.
5. IPv6 TLS JSONL (`codex-backend-tls-ipv6` / `codex-backend-client-tls-ipv6`), disabled by default.
6. RFCOMM JSONL (`codex-backend-rfcomm` / `codex-backend-client-rfcomm`), disabled by default.
7. RFCOMM TLS JSONL (`codex-backend-rfcomm-tls` / `codex-backend-client-rfcomm-tls`), disabled by default.
8. WebSocket IPv4 (`codex-backend-websocket-ipv4` / `codex-backend-client-websocket-ipv4`), disabled by default.
9. WebSocket IPv6 (`codex-backend-websocket-ipv6` / `codex-backend-client-websocket-ipv6`), disabled by default.
10. WSS IPv4 (`codex-backend-wss-ipv4` / `codex-backend-client-wss-ipv4`), disabled by default.
11. WSS IPv6 (`codex-backend-wss-ipv6` / `codex-backend-client-wss-ipv6`), disabled by default.

Commits 1–3 do not modify any of these compositions.

## Temporary legacy oracle

Before either public Pimpl changes, the old service/client implementation is
copied below `tests/component/codex/oracle/`. Test-only static targets compile
the copies with private renamed class/namespace identities. The existing server
and client differential executables link those renamed targets as their old
side, so the stable borders remain executable after the production cutover.

The frozen implementation closure is self-contained below that test subtree.
Its exact projection authority is the P3-base byte content of
`detail/BackendProjectionBuilder.cpp`, `detail/BackendProjectionBuilder.h`,
`detail/FrontendCapabilities.cpp`, `detail/FrontendCapabilities.h`,
`detail/FrontendProjection.cpp`, and `detail/FrontendProjection.h`. The private
headers occupy the mirrored canonical root
`tests/component/codex/oracle/include/ai/openai/codex/frontend/detail/` so the
unchanged canonical includes in every direct-copy consumer resolve to frozen
bytes. `Messages.h`, `Security.h`, `GeneratedProtocol.h`, and
`backend/Snapshot.h` remain deliberately shared stable authorities.

One `ai-openai-codex-frontend-legacy-projection-oracle` component compiles the
three frozen projection translation units exactly once and is consumed by both
oracle libraries. The entire six-file projection closure contains zero
occurrences of all four oracle renaming tokens: `FrontendConnection`,
`FrontendService`, `FrontendServiceTestAccess`, and `client`. The shared
component therefore receives none of the rename definitions. Server class and
client namespace coexistence remains compile-time-only through the existing
`target_compile_definitions`; source-level transformation is prohibited. A
short three-translation-unit link test pulls symbols from both oracle consumers
into one executable.

`tests/component/codex/oracle/source-closure.txt` is the single mechanically
consumed target/TU authority for CMake and policy. The manifest records SHA-256
for every frozen implementation file. Direct-copy files match the exact P3-base
source bytes. The eight `State.part*.inc` files concatenate without inserted
bytes to the P3-base `client/State.cpp`; the eight-line `oracle/client/State.cpp`
include shim has its own independent digest.

The oracle isolation policy validates the target/TU authority, complete digest
inventory, direct-copy fidelity when Git history is available, `State`
reconstruction, test-only/non-installed ownership, source-archive inclusion,
and the absence of live `src` implementation TUs or `.cpp`/`.inc` include
smuggling. The frozen include root is added with `BEFORE PRIVATE` to each oracle
target. A separate short dependency-resolution test reads the generator's
compiler dependency records (`ninja -t deps` in CI, Make dependency files for
Makefiles), positively observes all three mirrored frozen headers, and rejects
every reference to their live `src` counterparts. Missing dependency data is a
failure rather than an empty success.

The client oracle also exposes a test-private capture border through
`tests/component/codex/oracle/LegacyFrontendClientCapture.h` and
`tests/component/codex/oracle/client/Capture.cpp`. That wrapper records the old
client's accepted/ready/close facts, immutable serialized states, callback and
diagnostic order, and outbound messages without publishing a new SDK API.

The oracle targets exist only below the `AISUITE_BUILD_TESTS` subtree, are not
exported or installed, are absent from installed/binary runtime packaging, are
not linked by production/application targets, and have a one-way dependency on
production compatibility/protocol utilities. Source archives intentionally
include the oracle subtree because a supported test-enabled source tree refers
to it. Production never depends on the oracle. Commit 6 removes the copied
sources, capture wrappers, targets, and temporary isolation policy only after
every pre-deletion gate below is green. Retargeting a formerly production-facing
test to the oracle is permitted only when its continuing production behavior
has an explicit, reviewed permanent-core or production-adapter coverage map.
The Task-A production-coverage mapping is frozen in
`p3-task-a-f3-coverage-audit.md`; its four approved gaps are covered by focused
production regressions in Commit 2.

## Exact seven-commit ledger

The final branch is seven linear logical commits in this order:

1. `Freeze the P3 cutover contract and preserve the legacy oracle`
2. `Cut the public Codex frontend service over to ServerCore`
3. `Cut the public Codex frontend client over to ClientCore`
4. `Consolidate native Codex frontend transport composition`
5. `Cut WebSocket transports over to static composition`
6. `Remove the legacy Codex frontend implementation`
7. `Close and verify the Codex P3 cutover`

Commit 7 is documentation/test/policy/CI-evidence closure only and contains no
path below `src/`. Final SHAs remain pending until the seven-commit history is
complete and are recorded in Commit 7 and the draft PR body.

## Pre-deletion gates and mandatory hosted checkpoint

The focused local pre-deletion gate is necessary but not sufficient. After
future Commit 5, the complete legacy implementation and temporary oracle must
still exist. The exact Commit-5 head is pushed and becomes a mandatory rollback
and reference checkpoint. Commit 6 is forbidden until all of the following are
true:

1. Commits 1–5 are complete.
2. The explicit focused local Commit-5 gate passes.
3. The exact Commit-5 head is pushed.
4. Hosted CI/full closure runs on that exact SHA.
5. The hosted result is independently verified green.

The checkpoint must record SHA, workflow ID, every relevant job ID, conclusion,
skips, and environment limitations. Hosted closure includes, as applicable,
the complete ordinary CTest suite, installed consumers, source and binary
packages, public-header closure, ABI/ELF compatibility probe, P0
build/install-backed verification, deterministic P0 capture/comparison, and
hosted transport acceptance. Only then may Commit 6 delete legacy/oracle code.
After Commits 6–7 and any autosquash, republishing rewritten history uses only
`git push --force-with-lease`; final hosted closure runs again. Plain
`--force` is forbidden.

## Future Commit-4 SNode.C entry gate

Before native reconnect/physical-client machinery changes, Task B must verify
both accepted SNode.C source and focused executable behavior for an explicit
second `connect()` on the same configured client object. Evidence must separate
automatic retry/reconnect from a later application call and cover:

- successful connect, disconnect, explicit same-object connect again;
- failed/terminated attempt, explicit same-object connect again;
- no stale callback from cycle N affecting N+1;
- no duplicate automatic reconnect enabled by explicit connect;
- configuration remaining on the same client without reconstruction/copying;
- application shutdown.

The accepted source currently exposes relevant implementation under
`core/socket/stream/SocketClient` and focused lifecycle tests, but source
appearance is not proof. If the premise is not proven under the local runtime
policy, Task B stops before removing `PhysicalConnectionAttemptGate`,
per-attempt clients/configuration copies, or application reconnect machinery.
P3 may not add `connectOnce()`, invent another reconnect API, or patch SNode.C.

## Future WebSocket synchronous handoff rule

The WebSocket upgrade handoff is owner-event-loop-confined synchronous
reentrancy/lifetime composition, not threading infrastructure. It must use the
smallest file-private RAII scope state active only around the synchronous HTTP
to WebSocket upgrade call. The state supports safe nesting, strict LIFO
restoration, exception-safe cleanup, and exact `SocketConnection` identity
validation, so a factory for connection A can never consume connection B's
binding.

`thread_local` is prohibited. Mutexes, a global connection map, service
singleton, installed composition API, and a persistent process-wide runtime
registry are also prohibited. A file-static pointer/stack protected solely by
the strict RAII scope is acceptable in the single-owner event-loop model.

## Local and hosted validation policy

Every local CMake build uses exactly:

```text
cmake --build <build-dir> --parallel 24 [--target <focused targets>]
```

Every local CTest invocation uses an explicit focused selection and exactly:

```text
ctest --test-dir <build-dir> --output-on-failure --parallel 24 --timeout 60 <explicit selection>
```

Parallelism is never reduced or increased, and the timeout is never increased.
A timeout is reported rather than hidden or retried under different local
parameters.

Local execution does not run or wait for the complete ordinary CTest suite,
unfiltered CTest, full package closure, installed consumer, source/binary
package tests, cold/relinked or unrelinked ELF ABI probes, complete installed
public-header rebuild, complete P0 build/install verification, deterministic
full P0 capture/comparison, all eleven live transports as one suite, real Codex
App Server integration, credential/quota/account integration, stress, soak,
fuzzing, benchmarks, sanitizer matrices, exhaustive unrelated regressions, a
fresh SNode.C build, a test expected to exceed 60 seconds, or GitHub Actions
completion. Long closure is hosted and is never claimed for an exact head unless
that exact gate actually ran and passed there.

## Deletion and retention contract

Commit 6 is controlled deletion, not a semantic implementation commit. No new
behavior may first appear there. Expected removed categories are the old
service semantic engine, old client lifecycle/JSON reducer engine, duplicate
projection and synchronization authorities, obsolete test access, runtime
bridge/global WebSocket runtime/dynamic plugin targets, physical-attempt gate
and per-attempt client/configuration reconstruction, application reconnect
timers, duplicate JSONL framers, and the temporary oracle.

Permanent retained authorities are the protocol DSO, typed model, occurrence
authority, typed journal, `BackendProjection`, `ServerCore`, `ClientCore`,
`BackendCoreBridge`, `CanonicalStateBuilder`, all frozen public Pimpl classes
and generated facades, server-DSO-only compatibility utilities, the shared
private JSONL framer, final native/static-WebSocket binders, differential
fixtures, canonical expected outputs, coverage authority, mutation guards, and
permanent conformance tests. The manifest is the file-level deletion authority
and is updated to final observed state only in Commit 7.
