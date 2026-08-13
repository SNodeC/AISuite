# Codex architecture-reduction roadmap

This document is the normative, fixed P0–P3 roadmap for reducing the internal
complexity of the AISuite Codex frontend and its reference applications. The
current frontend server and client are a frozen behavioral reference and a
temporary executable differential oracle. They are not the implementation
strategy or internal architecture that later phases must preserve.

P0 freezes observable behavior and records present complexity. P1 provides the
generic SNode.C prerequisites. P2 builds the complete permanent frontend beside
the oracle. P3 cuts production over and deletes the legacy frontend. Later
phases must preserve the frozen external contract while replacing and reducing
the measured architecture deliberately. Architecture measurements are
comparison data, not equality gates.

P3 status: the production cutover and controlled legacy deletion are complete
on the P3 branch. The approved pre-deletion Commit-5 checkpoint and focused
local Commit-6 validation are recorded in
[`p3-frontend-cutover.md`](p3-frontend-cutover.md). Final exact-head hosted
closure and independent review remain pending; this status is not a merge-ready
claim.

## Motivation

The completed Frontend Protocol v1 stack has broad behavior, transport, and SDK
coverage, but its implementation grew across successive delivery phases.
Protocol projection, server and client transport composition, and application
lifecycle supervision now cross boundaries that can be made smaller and more
direct without changing what callers or users observe.

Reduction must proceed from evidence. P0 therefore captures the protocol,
public C++ surface, CLI, transport matrix, security and lifecycle semantics,
installed artifacts, dependency graph, source measurements, and test coverage
before any production replacement begins. P2 uses the current server and client
as executable oracles at stable borders rather than treating either old
implementation as a design dependency. P3 removes those implementations only
after differential and compatibility closure.

## Preserved boundaries

The following external and architectural borders remain authoritative
throughout P1–P3:

- `AppServerClient` remains the typed provider boundary; frontend clients never
  connect directly to the Codex App Server or receive its raw JSON.
- `BackendCore` remains the authoritative typed backend-state boundary,
  including provider lifecycle, frontend sessions, controller facts, bounded
  snapshots, and observations.
- The public `FrontendService` API and ABI and its Frontend Protocol v1 wire
  border remain compatible. Its current internal projection, journal, replay,
  batching, and queue implementation is not frozen as the future design.
- The public C++ frontend SDK API and ABI remain compatible, including its
  immutable `State`, operation results, errors, and lifecycle callbacks. Its
  current internal reduction and JSON traversal are not frozen as the future
  design.
- Frontend Protocol v1 identity, wire behavior, security, lifecycle, CLI, and
  all eleven external transport compositions remain compatible.
- The four existing imported targets, their package identities, and their
  required public headers remain available and compatible. Additive public or
  private targets, headers, and symbols are permitted.

## Diagnosed accidental complexity

The baseline distinguishes necessary domain and protocol complexity from
accidental implementation complexity. The latter currently includes the old
frontend client's transitive dependency on the old frontend server/backend,
multiple semantic JSON representations, legacy and expanded projections,
arbitrary JSON positions, journal/batch/coalescer machinery, duplicated JSONL
framing, separate native and WebSocket lifecycle stacks, application-owned
transport matrices, global WebSocket runtime bridges, reconnect configuration
copying, physical-attempt generation machinery, and broad responsibilities in
both application `main.cpp` files and `CommandDrainController`.

Those observations identify replacement targets. The old dependency graph is a
measured P0 fact, not an intermediate architecture to clean up. There is no
separate AISuite dependency-DAG cleanup phase and no preparatory
protocol-extraction phase. P2 creates the final dependency direction as part of
the greenfield implementation; P3 removes the obsolete old graph during
cutover and deletion.

## Final fixed phases

The phase count, order, purposes, and dependencies below are fixed. Do not add,
remove, merge, split, rename, or reorder phases. There is no P4, P5, P6, or P7.

### P0 — Freeze behavior and architecture baseline

Repository: AISuite

Purpose:

- freeze externally observable behavior;
- freeze protocol, security, lifecycle, transport, API, ABI, package, and CLI
  compatibility obligations;
- record the current implementation and dependency graph as measured facts;
- retain the current frontend implementation as a temporary differential
  oracle;
- capture deterministic architecture measurements;
- make no production change.

P0 does not require the current internal architecture to survive. It does not
implement P1, P2, or P3.

### P1 — Complete reusable SNode.C connect and WebSocket composition

Repository: SNode.C

Purpose:

- provide only the generic SNode.C composition primitives required by the P3
  AISuite cutover;
- preserve existing SNode.C transport capabilities and API compatibility;
- make no AISuite protocol or product-semantic change.

P1 is a separate SNode.C pull request and is not implemented by P0.

SocketClient requirements:

- retain `SocketClient::connect()` as the public explicit connection operation;
- do not add `connectOnce()`;
- do not add a public `reconnect()` method;
- permit a later explicit `connect()` on the same configured `SocketClient`
  after a failed connection cycle, after retries are disabled or exhausted, or
  after an established connection disconnects while automatic reconnect is
  disabled;
- preserve configured automatic retry and automatic reconnect;
- preserve current active-flow and multiple-connect behavior unless an
  independently proven defect requires a separately reviewed correction;
- preserve cumulative connection identities and statistics where currently
  defined;
- prevent stale timers, callbacks, status handlers, or connection receivers
  from an older cycle from affecting a newer explicit cycle;
- preserve permanent framework shutdown semantics;
- preserve the SNode.C ownership rule that a temporary `SocketClient` handle
  may leave scope while an already-started shared connection, retry, or
  reconnect flow continues.

`SocketClient` destruction or ordinary scope exit must not implicitly cancel an
active shared asynchronous flow.

WebSocket composition requirements:

- add direct per-upgrade WebSocket server factory/context composition;
- add direct per-request WebSocket client factory/context composition;
- preserve existing APIs and compatible fallback composition where required;
- enable AISuite to remove global WebSocket runtime bridges in P3.

### P2 — Build the complete greenfield frontend beside the oracle

Repository: AISuite

Prerequisite: P0 merged

P2 may proceed in parallel with P1. It is transport-neutral and does not cut
over production applications, so it does not require P1.

Purpose:

- build the complete permanent frontend architecture largely from scratch
  while the old frontend server and client remain available only as
  differential oracles;
- create one shared Frontend Protocol authority/boundary;
- create one new transport-neutral frontend server core;
- create one new transport-neutral frontend client core;
- establish the permanent typed frontend model/occurrence design and the
  permanent snapshot/live/replay projection authority;
- complete differential validation between the old and new implementations.

The shared protocol boundary consumes the existing frozen Frontend Protocol v1
identity/version, generated method/capability/projection metadata, generated
schema and fixtures, and stable message, event, item, scope, and error
vocabularies. P2 may create a permanent protocol target, but P0 does not freeze
its name or exact implementation. No separate preparatory extraction pull
request exists. P2 must neither create a second wire vocabulary nor fork the
generated authorities, and production new code must not depend on the old
frontend implementation as a library.

New server requirements:

- consume `BackendCore` through a typed boundary;
- implement authentication, authorization, scopes, controller/session
  behavior, snapshots, synchronization, replay, live Snapshot, bounded queues,
  backpressure, capabilities, projection, redaction, information ceilings, and
  command dispatch;
- use one typed frontend model and one typed occurrence/journal authority;
- avoid the current dual semantic JSON authority and repeated transformations;
- not depend on the old frontend server implementation.

New client requirements:

- preserve the existing public C++ SDK behavior and eventual API/ABI;
- implement correlation, synchronization, replay, immutable `State`, typed
  façades, command completion, reverse requests, schema validation, and
  lifecycle handling;
- remain transport-neutral and depend on neither the old nor the new frontend
  server, `BackendCore`, nor SNode.C transport modules;
- not retry failed commands or restore controller ownership automatically;
- preserve stale-state behavior after transport disconnection.

A dependency on existing stable `ai::openai::codex::typed::*` domain values is
permitted where required by the frozen public SDK.

The old implementation remains structurally unchanged except for the smallest
test-only or build wiring needed for differential execution. It is not cleaned
up, converted into the new design, or made a dependency of the new design. Old
and new implementations may both be built in P2, but production applications
continue using the old implementation throughout P2. No old production
implementation is deleted in P2.

### P3 — Cut over all applications and transports, then remove the legacy frontend

Repository: AISuite

Prerequisites: P1 merged and P2 merged

Purpose:

- cut `AISuite::OpenAICodexFrontend`,
  `AISuite::OpenAICodexFrontendClient`, `codex-backend`,
  `codex-backend-client`, all server/client transport composition, and
  installed/package consumers over to the new cores;
- use the merged P1 SNode.C primitives;
- preserve the four required imported targets, output-library names, required
  public-header paths, project VERSION, SOVERSION, public API/ABI, Frontend
  Protocol v1 behavior, security, lifecycle, CLI, and all eleven transports;
- replace application-owned composition with only the final shared framing,
  binders, hosts, connectors, supervision, workflow, and presentation layers
  needed by the product, without freezing exact internal class names;
- delete both old frontend implementations and all temporary oracle build
  paths after the deletion gate closes;
- remove obsolete framers, reconstruction/configuration machinery, runtime
  bridges, lifecycle stacks, adapters, helpers, targets, and duplicate tests;
- retain permanent fixtures and conformance tests;
- perform the final comparison against P0.

The eleven external compositions remain Unix JSONL, IPv4 JSONL, IPv6 JSONL,
IPv4 TLS JSONL, IPv6 TLS JSONL, RFCOMM JSONL, RFCOMM TLS JSONL, WebSocket IPv4,
WebSocket IPv6, WSS IPv4, and WSS IPv6. The in-memory path remains separately
test-only.

The CLI command named `reconnect` remains user-facing UX and may use the
reusable configured SNode.C client through the existing public `connect()`
operation. P3 must not implement a second application-owned reconnect
mechanism.

The old server/client code may be deleted only after the new architecture
passes full differential closure, protocol authority/currentness, schema and
SDK conformance, service-to-SDK acceptance, all eleven transport paths,
authentication and authorization, package/install consumers, public-header and
source/ABI compatibility tests, and owner live acceptance.

## Phase dependencies and repository ownership

```text
                         P0
                          |
               +----------+----------+
               |                     |
               v                     v
              P1                    P2
           SNode.C               AISuite
               |                     |
               +----------+----------+
                          |
                          v
                         P3
                       AISuite
```

- P1 and P2 may proceed in parallel after P0 is merged.
- P2 is transport-neutral and does not require P1.
- P3 requires the merged results of both P1 and P2.
- Only P1 modifies SNode.C.
- P0, P2, and P3 modify AISuite.
- `codex-ui` is not modified by P0–P3.

## Differential-oracle strategy

The old frontend server and client are temporary executable oracles through P2
and until P3 closure. They are not architectural dependencies of their
replacements. P2 performs differential comparison at stable borders:

```text
BackendCore typed inputs/state/occurrences
              |
       +------+------+
       |             |
old frontend     new frontend
server core      server core
       |             |
       +------+------+
              |
canonical Frontend Protocol v1 behavior
```

```text
canonical Frontend Protocol v1 input
              |
       +------+------+
       |             |
old frontend     new frontend
client core      client core
       |             |
       +------+------+
              |
public results / errors / immutable State / callbacks
```

Required differential domains include all 105 methods, all 26 expanded event
families, all 18 `ThreadItem` discriminators, all 12 scopes, capability
negotiation, Hello/Welcome, snapshots, initial synchronization, replay, live
Snapshot, equal-sequence expanded event groups, lower/higher/gapped sequence
handling, controller ownership, sessions, authentication and authorization
failures, scope projection, redaction and information ceilings, pending
reverse requests, command errors, malformed messages, unknown/future-safe
containment, item-content accumulation, truncation, and queue/backpressure
terminal behavior.

The starting oracle corpus is the generated Frontend Protocol/schema fixtures,
the reducer conformance fixture, projection and schema fixtures/tests,
service-to-SDK acceptance, and owner-reported live evidence. Future
differential normalizations must be narrow, reviewed, and documented. Broad
“ignore differing JSON” normalization is forbidden; differences in stable
Frontend Protocol semantics fail.

P2 completes both new cores and their differential validation while production
remains old. P3 cuts production over, retains the permanent fixtures and
conformance tests, deletes both old implementations and temporary oracle
scaffolding, and performs the final P0 comparison.

## A1.7c-2 ordering

The owner-approved order is:

```text
PR #14 / A1.7c-1
        |
        v
P0
        |
        +------------+
        |            |
        v            v
       P1           P2
        |            |
        +------+-----+
               |
               v
              P3
               |
               v
A1.7c-2 codex-ui migration
```

A1.7c-2 follows P3 and targets the reduced canonical frontend architecture.
`codex-ui` remains untouched throughout P0–P3. P0–P3 do not implement the Qt
UI.

## P0 non-goals

P0 does not simplify, refactor, redesign, repair, or otherwise change
production code. It changes neither protocol, public runtime API/ABI,
production target/link/install rule, transport default/timeout, security,
synchronization, replay, controller ownership, reconnect behavior,
command/process lifetime, CLI, output, SNode.C, nor `codex-ui`. It starts none
of P1–P3 and does not implement a shared protocol production target, either
greenfield core, differential replacement tests, application/transport cutover,
or legacy deletion. It does not start A1.7c-2, browser/TypeScript work,
provider abstraction, a line editor, or asynchronous prompt redraw.

A defect discovered during P0 is recorded with a later phase or owner-decision
route; it is not fixed in P0.

## Hard external contracts

The blocking P0 external contract covers:

- Frontend Protocol identity/version, message kinds, exact method IDs and
  ownership, expanded event families, `ThreadItem` discriminators, scopes,
  capabilities, and canonical authority fingerprints;
- the required existing imported targets `AISuite::OpenAICodex`,
  `AISuite::OpenAICodexBackend`, `AISuite::OpenAICodexFrontend`, and
  `AISuite::OpenAICodexFrontendClient`, their output/package identities,
  VERSION/SOVERSION compatibility, export availability, and required installed
  public-header paths;
- the complete `codex-backend-client` command syntax and its command,
  connection, process, queue, EOF-drain, stdin, reconnect, and controller
  semantics;
- the `BackendCore`, public `FrontendService`, and public SDK borders,
  projection and synchronization behavior, security, authentication,
  authorization, redaction, and information ceilings;
- eleven distinct external transports and the separate test-only in-memory
  transport;
- inherited configurable SNode.C read/write inactivity defaults and their
  disconnect behavior;
- the stable server/client comparison borders used by the temporary executable
  oracle strategy.

Public API and ABI compatibility remain hard project requirements. P0
mechanically freezes target/package/header identity and maps dedicated source
and ABI compatibility evidence; symbol fingerprints and binary metadata are
comparison measurements, not a claim of complete ABI proof by this tool.
Additional targets, headers, and ABI-compatible symbols are permitted. A
required target or required header may not disappear.

Changing a hard external contract requires explicit owner approval. A later
phase may replace internal implementations without changing this contract.

## Evidence and metric categories

The machine-readable baseline keeps exactly three top-level categories:

1. `externalContract` is blocking. P1–P3 preserve it unless the owner explicitly
   approves a contract change. Each subsection identifies whether evidence is
   generated-authority-derived, build-or-install-derived,
   executable-observation, named-test-evidence,
   owner-approved-declarative-contract, or inherited-dependency-evidence.
2. `architectureMeasurements` is non-blocking comparison evidence. It reports
   the production/reduction subject, permanent compatibility support, and P0
   baseline infrastructure separately. Target edges, NEEDED sets, binary sizes,
   symbols, total headers, files, tracked physical lines, dependency revisions,
   and test duration may change and are never equality gates.
3. `ownerLiveEvidence` records owner-reported real interactions. It is not
   represented as reproduced by automated tooling and is not rerun when it
   would require credentials, quota, an installed Codex executable, or
   hardware.

Facts in the human report are labeled as observed facts, measured facts,
owner-reported live evidence, architectural judgments, or future targets.
Estimates are not presented as measurements. No composite architecture score
is used.

## Expected final direction

P1 completes only the generic SNode.C prerequisites. In parallel, P2 builds the
complete permanent, transport-neutral server/client architecture beside the
legacy executable oracles, including the shared protocol boundary and full
differential validation. P3 then uses the merged P1 primitives to cut the
applications and all eleven transports over, deletes both legacy
implementations and temporary oracle paths after every compatibility gate
closes, and performs the final comparison against P0.

The public protocol, APIs, ABI, transports, security, lifecycle, and CLI remain
compatible while the permanent client does not depend on any server or
transport implementation, one typed authority drives projection and state,
transport composition uses reusable SNode.C layers, application entry points
have narrower responsibilities, and obsolete bridges and duplicate framers are
gone. Comparison reports show individual changes rather than hiding tradeoffs
behind a single score.

## Change discipline

Do not collapse P1–P3 into one cross-repository rewrite. P1 is the independent
SNode.C prerequisite, P2 builds and proves the complete greenfield frontend
beside the oracle, and P3 performs the controlled cutover and deletion. These
boundaries preserve the diagnostic value of the P0 evidence while avoiding
cleanup work on legacy code that will be deleted.

The immutable baseline and human report are in
[`p0-baseline.json`](p0-baseline.json) and
[`p0-baseline.md`](p0-baseline.md).
