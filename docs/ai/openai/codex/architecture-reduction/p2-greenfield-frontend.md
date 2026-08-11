# P2 greenfield Codex frontend architecture and closure

Status: the exact-head source-audit correction is implemented and has focused
local validation. Full closure and hosted-CI evidence for earlier heads remain
historical; the new exact-head hosted-CI status is recorded in the mutable
draft-PR body after publication. P2 does not become a cutover: production
remains on the P0 implementations.

P2 builds the permanent transport-neutral frontend server and client cores
beside the P0 production implementation. It does not cut over an application or
transport, change the public frontend server/client Pimpls, delete legacy code,
or begin P3.

## Provenance and fixed borders

| Item | Recorded value |
| --- | --- |
| AISuite repository | `SNodeC/AISuite` |
| `origin/master` at branch creation | `fa93297df89cd555ae08914fe33f82d8c7017ec6` |
| P2 base | `fa93297df89cd555ae08914fe33f82d8c7017ec6` |
| PR #15 merge commit | `fa93297df89cd555ae08914fe33f82d8c7017ec6` |
| PR #15 final source head | `e25fa5f05bdefb8b5d24f18b4f490663afcde408` |
| PR #15 merge method | Two-parent merge commit (non-fast-forward) |
| P2 branch | `codex/p2-greenfield-codex-frontend` |
| Accepted SNode.C commit | `6ae8fafcd50052a9d86932b0be721ef39cce7a44` |
| Accepted SNode.C version | `2.0.0` |
| Frontend protocol | `snodec.codex-frontend`, version `1` |
| AISuite project version | `0.1.0` |
| Codex SOVERSION | `2` |

The owner treats P1 as complete for this roadmap. P2 uses the accepted SNode.C
installation only for AISuite validation. It does not modify SNode.C, add a
composition API, add `connectOnce()` or a public `reconnect()`, add a templated
HTTP/WebSocket upgrade path, or repair the deferred static-factory lifetime
issue. P3 remains responsible for transport composition using the existing
SNode.C static-link helpers.

The frozen generated authority has eight message kinds, 105 methods (seven
frontend-native, 86 provider, and 12 reverse-response), 26 expanded event
families, 18 `ThreadItem` discriminators, ten pending-request kinds, 12 scopes,
and 18 capabilities (13 static-mechanism, one conditional-topology, and four
product capabilities). P2 consumes these authorities without changing their
external values.

## Permanent target graph

```text
                           nlohmann_json
                                 |
                                 v
             AISuite::OpenAICodexFrontendProtocol
                                 |
                                 v
             ai-openai-codex-frontend-model
                       /                     \
                      v                       v
       ai-openai-codex-frontend-      ai-openai-codex-frontend-
             server-core                    client-core
              |       |                          |
              v       v                          v
    OpenAICodexBackend OpenAICodex          OpenAICodex
```

`AISuite::OpenAICodexFrontendProtocol` is the only additive installed public
target. The model and both cores are permanent internal static libraries. Their
headers remain below `frontend/internal/` and are not installed.

The required dependency direction is:

- `ai-openai-codex-frontend-protocol` has no AISuite production, Backend,
  SNode.C, transport, TLS, Bluetooth, HTTP, WebSocket, Qt, or Curses dependency;
- `ai-openai-codex-frontend-model` depends on the protocol target and no
  Backend, old frontend service, server core, client core, or transport;
- `ai-openai-codex-frontend-server-core` depends on the model, protocol,
  `OpenAICodexBackend`, and `OpenAICodex`, and not on either client or any
  transport;
- `ai-openai-codex-frontend-client-core` depends on the model, protocol, and
  `OpenAICodex`, and not on Backend, either server, SNode.C, or any transport.

CMake File API and source-include closure policy inspect both direct and
transitive target edges. They also prove that `codex-backend` and
`codex-backend-client` still resolve through the old production server/client
targets rather than either new core.

## Shared Frontend Protocol boundary

The new shared library has these identities:

| Property | Value |
| --- | --- |
| Build target | `ai-openai-codex-frontend-protocol` |
| Exported target | `AISuite::OpenAICodexFrontendProtocol` |
| `EXPORT_NAME` | `OpenAICodexFrontendProtocol` |
| `OUTPUT_NAME` | `aisuite-openai-codex-frontend-protocol` |
| `VERSION` | `${PROJECT_VERSION}` |
| `SOVERSION` | `${AISUITE_CODEX_SOVERSION}` |
| Public link interface | `nlohmann_json::nlohmann_json` |

It owns the existing protocol-neutral implementations `Codec.cpp`,
`Messages.cpp`, and `detail/GeneratedSchemaValidator.cpp`. It reuses the
existing `Codec.h`, `GeneratedProtocol.h`, `Messages.h`, `Protocol.h`, and
`Security.h` public paths and namespaces. Generated schema data, event
representation support, and schema-validator internals remain private inputs.
There is one codec, schema, method registry, capability registry, and item
discriminator authority.

The old frontend server directly links the protocol DSO, and the old client
resolves that shared authority through its existing frontend dependency, so
protocol symbols have one runtime owner. The old frontend DSO must directly
`NEEDED` the new protocol DSO. A protocol-only installed consumer exercises
constants, generated method metadata, client/server message codecs, and
expanded snapshot/event codecs while checking that its runtime closure does not
load Backend, either old frontend DSO, or SNode.C transport libraries.

The ELF ABI gate preserves a consumer linked against only the exact P0
installed `AISuite::OpenAICodexFrontend` target, does not relink it, and runs it
against the P2 installation. It covers client-message and server-message
encoding/decoding, enum/string conversions, and generated method metadata. This
is Linux ELF evidence only and is not a cross-platform ABI claim.

## Canonical typed model

The internal model uses strong identifiers for sessions, controllers, threads,
turns, items, pending requests, process handles, projection/source stamps, and
occurrence groups, plus a dedicated frontend sequence type. Known state is held
in typed records and closed variants rather than arbitrary JSON maps.

Forward-safe detail is admitted only through `SafeDetail`. It has hard byte,
depth, and member limits and rejects secret-key material. The model separately
represents freshness, truncation, and the information states present, omitted,
redacted, truncated, unavailable, stale, unknown, absent, and explicit null.
This keeps those states distinguishable through snapshot, occurrence,
projection, and client reduction.

The model never uses `void*`, `std::any`, or an untyped shared pointer as its
semantic authority. Authentication credentials, reverse-response answers, and
provider secrets are not retained in model state, projection metadata, or safe
extensions.

## Typed snapshot

`CanonicalSnapshot` is the single typed snapshot authority. It covers provider,
controller, sessions, thread-list state, threads, turns, all item variants,
pending requests, accounts, models, configuration, processes, filesystem
watches, fuzzy searches, permission profiles, reviews, apps, external agents,
hooks, marketplace, plugins, skills, MCP, Windows sandbox, platform,
remote-control, integrations, notices, activities, diagnostics, capacity,
truncation, projection metadata, and backend cursor metadata.

The same model is decoded from and encoded to legacy-v1 and expanded-v1
snapshots. Server snapshots, live occurrences, replay, and client reduction do
not maintain a separate snapshot-only semantic implementation.

## Typed occurrence and journal

`CanonicalOccurrence` owns common sequence and group identity, group index and
count, source and optional projection stamps, and the relevant optional
session/controller/thread/turn/item/pending-request/process identities. Its
closed `OccurrencePayload` variant contains the 26 expanded event-family
representations. The 18 `ThreadItem` and ten pending-request discriminators are
closed typed variants in the same model.

Legacy and expanded wire forms are encodings of one canonical occurrence:

```text
legacy-v1 event ----\
                     > typed canonical occurrence -> client reducer
expanded-v1 event --/

typed canonical occurrence -> expanded-v1 events
                           \-> legacy-v1 compatibility event
```

An occurrence can expand into multiple events with one sequence. Explicit
group identity, index, and count enforce the only valid equal-sequence grouping
semantics. Known discriminators remain closed typed alternatives. Future item
discriminators and generic unknown pending requests are retained only in a
bounded private legacy-compatibility sidecar. Legacy-v1 projection merges those
records back into source order; expanded projection omits and accounts them for
that connection without inventing an expanded discriminator.

For an unknown item, that sidecar retains the exact item, parent-thread, and
parent-turn identities; source index and order; a valid UTF-8 discriminator
bounded to the identity limit; approved visible status, accumulated agent and
reasoning text, reasoning summary, command output, timestamps, generation,
freshness, connection-invalidated state, and existing content-truncation and
dropped-content facts. It unconditionally omits opaque unknown `data`, opaque
unknown `extensions`, raw provider envelopes, and speculative future-schema
fields, even when their current member names look harmless. Each nonempty
discarded object adds its exact compatibility-local `/data` or `/extensions`
omission path and sets truncation monotonically. Empty, malformed, or otherwise
unrepresentable discriminators use the truncated `unknown` fallback. Expanded
projection still accounts the whole unknown item exactly once at its source
path; compatibility-local payload omissions do not inflate that count.

`TypedOccurrenceJournal` retains canonical occurrences directly. It advances a
monotonic frontend sequence, accounts retained entries and canonical encoded
bytes, evicts by configured entry/byte limits, preserves deterministic group
order, reports the oldest replayable boundary, rejects future replay cursors,
reports gaps, supports explicit replay invalidation, and reports sequence
exhaustion. Serialization is used for byte accounting, not to recover semantic
type.

A pending authoritative Snapshot has one closed sequencing mode. A
`ReuseCommittedSequence` barrier is valid only when every semantic value in the
Snapshot is already represented by the current committed frontend sequence; it
invalidates replay at that exact cursor without advancing it. Any unsequenced
canonical-state loss selects `AdvanceSequence` and invalidates replay through
the ordinary advancing journal operation. `AdvanceSequence` dominates a prior
`ReuseCommittedSequence` request and is never downgraded merely because a later
occurrence commits. Consequently, a Snapshot never carries semantic state newer
than its frontend sequence. The pending mode returns to `None` only when that
Snapshot suffix is consumed or during terminal cleanup.

Every bounded user-visible string retained in the canonical frontend model is
validated as complete UTF-8 before retention. Valid text within its bound is
preserved exactly; oversized valid text keeps the longest complete UTF-8 prefix
within the byte bound and records the existing deterministic truncation path.
Malformed UTF-8 fails closed before it can enter the canonical model or a
serialized frontend message.

## Corrective implementation-audit disposition

The post-implementation source audit was resolved without changing the P2
architecture or starting P3. The focused regressions named below are subcases
of the listed component or policy tests.

| Finding | Disposition | Production change | Enforced invariant and evidence | Owning commit |
| --- | --- | --- | --- | --- |
| Identity parse failure, wildcard, or last/first lookup | Confirmed correctness defect | Yes | Entity families require a valid, unique strong identity and exact parentage; missing, malformed, incomplete, conflicting, or ambiguous identities select nothing and require Snapshot containment. `CodexFrontendServerCoreSynchronizationTest` and `CodexFrontendCoreDependencyPolicyTest` cover the negative cases and source closure. | 3 |
| Unknown item legacy-v1 compatibility and projection poisoning | Confirmed correctness defect | Yes | The private sidecar retains exact item/thread/turn identity, source position/order, a bounded valid discriminator, approved visible accumulated fields, timestamps, generation/freshness, connection-invalidated state, and truncation/dropped-content facts. Opaque unknown `data` and `extensions`, raw provider envelopes, and speculative future-schema fields are always omitted; nonempty objects add exact local omission paths and cannot have their truncation fact overwritten. Legacy-v1 Snapshot/live/replay preserve the bounded item in order, CompleteThreadItems omits it once, client PublishedState contains no opaque key/value, UTF-8 truncation is boundary-safe, known neighbors and unrelated state remain, and malformed known kinds stay strict. `CodexFrontendServerCoreSynchronizationTest` and `CodexFrontendClientDifferentialTest` name the focused second-audit evidence. | 3, 5 |
| Generic unknown pending request poisons projection | Confirmed correctness defect | Yes | A bounded, sanitized private sidecar retains the stable request identity, source position, safe generic method/parameters, diagnostics, and truncation facts. Legacy-v1 retains it and the existing `request.unknown.respond`/`reject` paths target its exact identity; DedicatedPendingRequests omits it once and uses per-connection Snapshot containment. Known requests and unrelated state remain, secrets/raw envelopes do not escape, and malformed known kinds remain strict. Model, server synchronization/command, client synchronization, compatibility-adapter, and differential tests cover the behavior. | 2, 3, 4, 5 |
| Recursive mutex and implicit server reentrancy | Confirmed architectural defect | Yes | `ServerCore` is owner-event-loop confined and not thread-safe. Dispatch depth, deferred/coalesced flush state, stable connection generations, expected lifecycle checks, bounded deferred live-Snapshot placement, and provider-action generations replace locking. Lifecycle, synchronization, command, and backpressure tests cover synchronous schedulers, clocks, backend snapshots/actions, completion, and stale callbacks; dependency policy forbids mutex tokens and unscoped backend-snapshot callback boundaries. | 3 |
| Oversized journal group clears prior records | Disproven as an external correctness defect | No | The unretained sequence creates a replay hole: cursors before it report Gap, its own cursor can replay later retained groups, the current cursor has an empty available replay, and future cursors remain FutureSequence. `CodexFrontendTypedJournalTest` proves the exact sequence 1/2/3 behavior. | 2 |
| Server-wide versus per-connection Snapshot fallback | Disproven for representation overflow | No | Canonical-stream loss remains global, while representation-specific batch overflow falls back only for the affected connection; the fitting representation remains live/replayable and later groups do not inherit fallback. `CodexFrontendServerCoreSynchronizationTest` measures and exercises both representations. | 5 |
| `PublishResult` conflates canonical commit with delivery fallback | Confirmed correctness/API-contract defect | Yes | Canonical journal commit success is distinct from live delivery mode. Rejection before commit returns `accepted=false`, an error, and `None`; a normally staged commit returns `accepted=true`, no error, and `Occurrences`; a commit covered by an existing, bound-triggered, or post-commit-exception Snapshot fallback returns `accepted=true`, no error, its committed sequence, and `SnapshotFallback`. A committed group is never reported rejected, so callers are never told to retry it. One non-throwing transition clears uncertain pending occurrences while preserving the current result's committed sequence, and committed delivery fallback invalidates replay at that sequence before stamping the authoritative Snapshot. Ordinary explicit Snapshot barriers retain their existing advancing invalidation contract. `CodexFrontendTypedJournalTest`, `CodexFrontendServerCoreSynchronizationTest`, and `CodexFrontendServerCoreBackpressureTest` cover same-sequence invalidation, normal commit, invalid/closed rejection, zero-bound fallback, already-pending fallback, authoritative Snapshot/replay invalidation, and the complete result-invariant matrix. | 2, 3 |
| Pending Snapshot cause conflation | Confirmed correctness defect | Yes | `PendingSnapshotSequenceMode` makes `None`, `ReuseCommittedSequence`, and `AdvanceSequence` mutually exclusive and auditable. Committed-delivery-only fallback may reuse N, but later unsequenced dirty/coalescing loss upgrades the same pending Snapshot to an advancing barrier at N+1. The stronger mode cannot be overwritten by another committed fallback. The focused backpressure regression proves one accepted commit at N, no duplicate occurrence delivery, final backend state in the single Snapshot at N+1, Gap after N, and Available/empty replay after N+1; the existing server backpressure and journal regressions continue to prove committed-only reuse, and the journal regression proves the next ordinary append at N+1. | 3 |
| Pending user-input presentation uses byte truncation | Confirmed correctness defect | Yes | Question id/header/prompt and option label/description validate the entire source string as UTF-8, then share the existing complete-code-point prefix helper. Oversized multibyte values record their exact truncation paths without retaining partial code points; isolated continuation bytes, truncated multibyte leads, and malformed suffixes beyond the byte bound fail projection before canonical or wire retention. `CodexFrontendServerCoreSynchronizationTest` covers every affected field and all three malformed forms. | 3 |
| Contradictory `publishSnapshot` result | Confirmed correctness defect | Yes | Frozen Path B applies: a same-sequence terminal Snapshot is accepted with no error and carries sequence-exhausted cursor metadata; `accepted` and `error` are never both set. Because the barrier invalidates replay at that same sequence, a later replay request correctly performs Snapshot synchronization rather than claiming empty replay continuity. Boundary, recipient, replay, and client-consumption cases are covered by server/client synchronization tests. | 3 |
| Client callbacks continue after lifecycle invalidation | Confirmed correctness defect | Yes | Every continuation after a user callback checks the captured physical generation and expected lifecycle; retired-generation publication, cursor, synchronization, operation, error, and deferred-send continuations stop. Accepted synchronous direct sends may legitimately complete their synchronization before `send()` returns, and deferred commands continue draining only while their physical generation remains owned. Idempotent detach/disconnect during failure fanout cannot send old work. The documented terminal ProtocolError observation remains the sole explicit exception. Client lifecycle, synchronization, operation, and differential tests cover the rule. | 4 |
| Measure-by-serialization | Confirmed deferred optimization | No | Canonical serialization remains the bounded byte-accounting mechanism. No correctness defect was established and no size-ledger redesign belongs in this pass. | Deferred |
| Projection linear scans | Confirmed deferred optimization | No | Exact unique scans are retained; no permanent index or snapshot redesign belongs in this pass. | Deferred |
| Sequence-derived occurrence-group identity | Accepted deterministic implementation | No | No concrete collision or behavioral defect was established; the current group identity remains deterministic and covered by grouping/replay tests. | None |

`SafeDetail` remains fail-closed: recursive structural inspection,
byte/depth/member bounds, secret-key rejection, the boolean `isSecret`
classification allowance, and whole-detail rejection are unchanged. Opaque
unknown item `data` and `extensions` are omitted before any attempt to preserve
them as safe detail; this correction neither weakens nor expands the
`SafeDetail` vocabulary. Differential normalization remains `none`.
The fail-closed server behavior intentionally differs from the legacy server
oracle, so it is not mislabeled as a matched server-ledger identity and no
normalization is added. Corrected-wire client containment remains a focused,
unledgered differential regression; the generated ledger and coverage fixture
remain unchanged.

## Projection and information ceilings

`ProjectionAuthority` receives the authenticated principal, selected
capabilities, peer/transport facts, continuity fingerprint, controller fact,
and generated method metadata. It projects both `CanonicalSnapshot` and
`CanonicalOccurrence` and applies bounded detail rules with explicit metadata.

Scopes and controller ownership remain independent: ownership grants no scope,
and the control scope grants no ownership. Method admission is derived from the
generated method security/readiness/controller classifications rather than a
second hand-maintained matrix. Projection forbids secrets from snapshots,
events, replay, client state, fingerprints, diagnostics, and fixtures.

## Transport-neutral server core

`ServerCore` is driven by a narrow typed `BackendPort`, connection callbacks,
an injected scheduler/timer scheduler, and an injected monotonic clock. It has
no socket, HTTP, WebSocket, TLS, RFCOMM, or SNode.C transport type.

The core owns service and connection lifecycle, Hello/version/authentication
handling, pre-authentication failure limits, authorization and method policy,
controller/session state, capability and method discovery, generated command
dispatch, provider readiness, duplicate/outstanding request bounds, inbound and
rate bounds, backend completion/error mapping, snapshot/replay/live delivery,
typed journal publication, bounded outbound queues, fairness/backpressure,
projection, close policy, replay invalidation, and sequence exhaustion.

Generated method metadata is the exhaustive dispatch authority for all 105
methods. Backend projection builds the canonical snapshot and occurrence model;
projection and the journal then feed snapshot, replay, and live delivery through
the same protocol encoders.

## Transport-neutral client core

`ClientCore` attaches a transport through send/close callbacks and receives
typed or encoded Frontend Protocol messages. It owns physical-generation
lifecycle, Hello/authentication, Welcome and capability validation,
representation choice, initial and explicit synchronization, request-ID
allocation, operation correlation and bounds, generated operation dispatch,
typed result decoding, reverse-response submission, immutable state
publication, typed snapshot and occurrence reduction, callback ordering,
bounded diagnostics, capacity enforcement, and protocol/transport error
propagation.

The core retains the P0 lifecycle invariants: commands from an old physical
generation are never retried; controller ownership and reverse responses are
never restored or resubmitted; ordinary command failure does not close the
client; physical disconnect retains the client object and marks published state
stale; a later attachment starts a new physical generation; stale callbacks
cannot affect that generation; and ordinary commands are not ready until
synchronization completes.

The reducer publishes existing frozen SDK-compatible typed values where
practical. Private compatibility adapters exercise the mapping from existing
`FrontendServiceOptions`, `FrontendConnectionCallbacks`, and public
`client::ClientOptions`, state/result/error/submission/callback types into the
new cores. P2 does not switch the public Pimpls or install the adapters.

## Production oracle isolation

Throughout P2:

- `codex-backend` links and runs the old `FrontendService` implementation;
- `codex-backend-client` links and runs the old public client implementation;
- all native, WebSocket, and WSS transports remain wired to the old code;
- public `EventCoalescer`, `EventJournal`, `UpdateBatchBuilder`,
  `FrontendService`, and `FrontendConnection` behavior remains unchanged;
- old source, transports, tests, runtime bridges, CLI behavior, reconnect
  behavior, and defaults remain present;
- `codex-ui` is untouched.

Old production changes are limited to protocol-target linking and symbol
ownership needed by extraction; no old production `.cpp` file has a semantic
change. The new cores have no source or target dependency on the old
implementations. New-core development exercises old code only through
differential and compatibility tests, while production applications remain
wired to the old implementations.

## Differential border and exhaustive coverage

The deterministic server harness drives equivalent backend state,
notifications, messages, and commands through old `FrontendService` and the
new server core. It compares receive disposition, exact protocol messages and
order, request IDs, errors, close behavior, synchronization, sequences and
groups, snapshots, replay/live delivery, controller/session behavior,
capabilities/methods, projection metadata, and terminal queue/backpressure
behavior.

The deterministic client harness feeds identical legacy-v1 and expanded-v1
messages into the old public client oracle and new client core and drives
equivalent submissions. It compares request IDs, operation values/errors,
connection and session state, visible/synchronized sequence, pending counts,
capability/method status, immutable typed state and indices, revisions,
callbacks, diagnostics, stale transition, and close behavior.

The coverage fixture is rendered from the generated registry and protocol
authority. The guard re-derives it, checks it into source control for review,
and requires runtime execution ledgers written only after a successful actual
old/new comparison. The required matrix includes:

- all 105 methods and every parameter/result schema reference, ownership,
  security, readiness, and controller classification;
- all 26 expanded event families at the canonical legacy/expanded projection
  and reduction borders; only authority-defined families have live backend
  notification mappings;
- all 18 item and ten pending-request kinds;
- all 12 scopes, all 18 capabilities, and all eight message kinds;
- snapshot and replay synchronization, live delivery, and both wire
  representations.

Focused border mutations must be detected for event and item discriminators,
sequence/grouping, redaction/secret/omission paths, controller and scope
requirements, result type, replay gaps, reducer omission, retry/restoration,
queue bounds, and stale callbacks.

### Normalization

Normalization: none. No field or path is ignored. Deterministic schedulers,
clocks, backend fixtures, connection order, and request IDs make the comparison
exact. Canonical public-State serialization and source-shape conversion are
comparison inputs, not output normalization. If closure identifies an
unavoidable nondeterministic field, this section and a test that proves no
adjacent field is ignored must be added before P2 can close.

## Logical commit ledger

P2 has exactly these six logical commit subjects. The final SHA ledger belongs
in the draft PR body because this document is itself owned by commit 6.

1. `Add the shared Codex frontend protocol boundary`
2. `Add the typed Codex frontend model and occurrence journal`
3. `Implement the greenfield Codex frontend server core`
4. `Implement the greenfield Codex frontend client core`
5. `Add differential Codex frontend conformance`
6. `Close P2 greenfield frontend architecture` (containing commit/HEAD)

## Installed, API, and ABI compatibility

The four P0 public targets, library identities, public headers, project version,
SOVERSION, protocol identity, and protocol version remain required. The
protocol target and DSO are additive; internal core/model headers are not
installed.

Closure requires all of the following evidence:

- public headers compile individually and comply with installation policy;
- existing source consumers build without source changes;
- protocol symbols resolve for the preserved, unrelinked P0 ELF consumer;
- the old frontend DSO directly needs the protocol DSO;
- protocol-only and existing server/client installed consumers pass;
- protocol-only runtime dependencies exclude Backend, old frontend/client, and
  SNode.C transports;
- binary and source packages contain the required old artifacts plus the
  additive protocol DSO/target and exclude internal headers.

## P0 comparison and architecture measurements

The committed `p0-baseline.json` remains immutable. The committed P0 tool's
self-test, source-only validation, full build/install/executable verification,
deterministic capture, and hard comparison all pass. `externalContract` has
zero blocking drift: `changedExternalContracts`, missing required public
targets, and missing required public headers are all empty; all nine P0
external-contract sections are unchanged. SNode.C remains at the accepted
commit and version with `changed=false` and `blocking=false`.

Expected nonblocking architecture-measurement changes are reported separately:

- one additive installed protocol target and shared DSO;
- three additive permanent internal static targets;
- protocol symbol ownership and corresponding target/ELF `NEEDED` edges;
- added typed model, occurrence, journal, projection, server, and client source;
- added focused, differential, compatibility, policy, installed-consumer, and
  mutation tests;
- changed binary sizes and build/test duration.

No composite score is used. The relevant nonblocking P0-to-P2 measurements are:

- production/build-support targets increase from 13 to 17: the permanent
  protocol, model, server-core, and client-core targets;
- `libaisuite-openai-codex-frontend-protocol.so` is additive and measures
  384,383,832 bytes in the Debug install; the old frontend DSO decreases from
  699,502,840 to 408,769,912 bytes as protocol ownership moves;
- production-reduction source increases from 255 to 269 files, including seven
  headers and seven sources, by 16,855 physical lines and 863,098 bytes;
- the ordinary suite increases from 191 to 215 registered tests, with the P2
  run reporting 214 passes, one expected skip, and zero failures;
- CMake and ELF dependency-edge changes record the new protocol authority and
  the permitted core dependencies individually;
- all seven configured P0 legacy-oracle source identities remain present,
  tracked, and unchanged.

The comparison report separately records every remaining target, dependency,
binary-size, symbol-fingerprint, source, support, test-count, and duration
change.

## Historical validation evidence for the superseded closure head

The table below records the original P2 closure evidence for superseded head
`040d19c62341fc8d4a369f05eea9eec7dc4ff635`. It remains historical evidence and
must not be read as validation of the second-audit rewritten head. Mutable
publication and hosted evidence is recorded in the draft PR body.

| Gate | Final evidence |
| --- | --- |
| Exact six commits and subjects | Passed: six linear commits from the exact P0 base with the required subjects; the final SHA ledger is recorded in the draft PR body |
| Clean tree and `git diff --check` | Passed after the final closure amendment |
| Feature-complete configure and all-target build | Passed: Debug Ninja configuration and one incremental `all` build, 51 required rebuild steps plus 30 continuation steps after restoring a test-only namespace seam, `--parallel 4` |
| Protocol and client generated-authority currentness | Passed |
| Protocol isolation | Passed |
| Five typed-model tests | Passed |
| Five server-core tests | Passed |
| Five client-core tests | Passed |
| Server differential | Passed |
| Client differential | Passed |
| Exhaustive execution-ledger coverage guard | Passed |
| Differential mutation test | Passed |
| Compatibility-adapter test | Passed |
| Corrected focused P2 suite | Passed: 21/21, 0 skipped, 0 failed, 8.53 s; three dependency/protocol-isolation gates also passed |
| Selected closure gate suite | Passed: all 16 gates, 0 skipped, 0 failed. Fifteen passed in the 416.32 s aggregate run; the installed-consumer gate was rerun alone in 59.25 s after moving its isolation workspace outside the repository, eliminating a test-environment path-policy failure. |
| Public-header policy and self-containment | Passed; self-containment completed in 143.47 s during ordinary CTest |
| CMake File API/source-closure policy | Passed |
| Symbol visibility | Passed |
| Installed consumers | Passed in 61.48 s during ordinary CTest; the isolated protocol-only consumer loaded no forbidden runtime library |
| Unrelinked P0 ELF binary against P2 install | Passed in 133.66 s during ordinary CTest; Linux ELF scope only |
| Binary package | Passed in 26.13 s during ordinary CTest |
| Source package | Passed in 7.72 s during ordinary CTest |
| P0 source-only validation | Passed: baseline-tool self-test and source-only validation |
| P0 full build/install-backed verification | Passed; required external contract matches P0, additive protocol target observed, accepted SNode.C unchanged |
| P2 capture and P0 comparison | Passed: deterministic byte-identical captures and zero blocking external-contract drift |
| Complete ordinary CTest | Passed: 215 registered; 214 passed; 1 skipped; 0 failed; 649.82 s; `--parallel 2` |
| Local environment skips and reasons | One expected skip: `CodexTypedAppServerIntegrationTest` — `SNODEC_RUN_CODEX_TYPED_INTEGRATION=1` was not set because the real integration may use configured credentials and quota |
| Local execution limits | Temporary isolation used `/home/voc/p2t`; the repository retained its single `build-p2-complete` tree. The all-target build used `--parallel 4` and complete CTest used `--parallel 2`. |
| Draft PR number/head | Recorded in the mutable draft PR body after publication |
| Exact-head hosted workflow/job IDs and result | Recorded in the mutable draft PR body after exact-head CI |
| Hosted skips and reasons | Recorded in the mutable draft PR body after exact-head CI |

## Exact-head source-audit correction validation

This correction intentionally uses only the affected Debug Ninja targets and
the ten requested focused tests. It does not rerun the complete ordinary CTest
suite, installed consumer/package/ABI gates, P0 capture or comparison, real App
Server integration, live transport acceptance, SNode.C, or a fresh all-target
build. Before selecting parallelism, the host reported 28 processors, 62 GiB
RAM with 45 GiB available, and 460 GiB free in the workspace filesystem. The
affected build used `--parallel 4`; CTest used `--parallel 2`.

Before either production file changed, the two tests-only regressions failed on
exact head `b2644b771e32b5fc235d4ee3cb232a199def993c`. The backpressure test showed
the mixed authoritative Snapshot remaining at N and replay after N+1 being
treated as future/Snapshot. The synchronization test showed partial multibyte
pending-request presentation truncation and malformed UTF-8 surviving the
projection boundary.

The affected Debug Ninja targets were built with:

```text
cmake --build build-p2-complete --target CodexFrontendTypedJournalTest CodexFrontendTypedSnapshotTest CodexFrontendServerCoreSynchronizationTest CodexFrontendServerCoreBackpressureTest CodexFrontendProjectionSecurityTest CodexFrontendServerDifferentialTest CodexFrontendClientDifferentialTest CodexFrontendDifferentialMutationProbe --parallel 4
```

`ctest -N` resolved all ten requested registrations. All ten passed, with zero
failed and zero skipped, in 7.50 seconds real time. Per-test times were: server
differential 6.77 s; client differential 3.10 s; server synchronization 0.84 s;
mutation 0.63 s; server backpressure 0.18 s; typed Snapshot 0.10 s; coverage
guard 0.09 s; typed journal 0.08 s; dependency policy 0.07 s; and projection
security 0.01 s. `git diff --check` passed. The rewritten six-commit ledger and
hosted-workflow status are recorded in the draft PR body after publication.

## P3 cutover map

P3 is a wiring and deletion phase after P1 and P2 merge, not another semantic
rewrite:

1. Bind the existing public server options and connection callbacks to
   `ServerCore`, preserving `FrontendService` and `FrontendConnection` API/ABI.
2. Bind the public client Pimpl and every generated facade to `ClientCore`,
   preserving state, results, errors, submissions, callbacks, and reconnect
   behavior.
3. Switch `codex-backend` and `codex-backend-client` to those bindings.
4. Bind all eleven native/WebSocket/WSS transports to the new callback borders.
5. For WebSocket/WSS, statically link/register the existing HTTP upgrade and
   Codex subprotocol factories with the accepted SNode.C helpers; add no new
   SNode.C composition API.
6. Retain compatible implementations or wrappers for `EventCoalescer`,
   `EventJournal`, `UpdateBatchBuilder`, `FrontendService`, and
   `FrontendConnection` where public API/ABI requires them.
7. Re-run full protocol, API/ABI, transport, package, P0, and owner-live gates.
8. Only after those gates pass, delete the old server/client implementations,
   obsolete transport adapters/runtime bridges, and temporary oracle support.

P3 must preserve all eleven transport contracts and the separate test-only
in-memory path. It must not alter SNode.C compatibility mechanisms, and it does
not include the later `codex-ui` migration.
