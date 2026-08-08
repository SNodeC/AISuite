# Codex architecture-reduction roadmap

This document is the normative, fixed P0–P6 roadmap for reducing the internal
complexity of the AISuite Codex frontend and its reference applications. The
current implementation is the behavioral reference. It is not the desired
final internal architecture.

P0 freezes observable behavior and records present complexity. Later phases
must preserve the frozen external contract while changing the measured
architecture deliberately. Architecture measurements are comparison data,
not equality gates.

## Motivation

The completed Frontend Protocol v1 stack has broad behavior, transport, and
SDK coverage, but its implementation grew across successive delivery phases.
Protocol projection, server and client transport composition, and application
lifecycle supervision now cross boundaries that can be made smaller and more
direct without changing what callers or users observe.

Reduction must proceed from evidence. P0 therefore captures the protocol,
public C++ surface, CLI, transport matrix, security and lifecycle semantics,
installed artifacts, dependency graph, source measurements, and test coverage
before any production refactoring begins.

## Preserved boundaries

The following boundaries remain authoritative throughout P1–P6:

- `AppServerClient` is the typed provider boundary; frontend clients never
  connect directly to the Codex App Server or receive its raw JSON.
- `BackendCore` owns one authoritative typed backend state, provider lifecycle,
  frontend sessions, controller facts, bounded snapshots, and observations.
- `FrontendService` owns the transport-neutral complete-JSON-object protocol
  boundary, authentication, authorization, projection, journaling, replay,
  snapshots, live Snapshot barriers, bounded queues, and capabilities.
- `AISuite::OpenAICodexFrontendClient` owns Frontend Protocol semantics,
  correlation, synchronization, replay cursors, and immutable reduced State;
  applications own physical transports.
- Existing public targets, output library names, C++ APIs, ABI, SOVERSION,
  protocol identity/version, CLI behavior, security, and all eleven external
  transport compositions remain compatible.

## Diagnosed accidental complexity

The baseline distinguishes necessary domain and protocol complexity from
accidental implementation complexity. The latter currently includes the
frontend client's transitive dependency on the frontend server/backend,
multiple semantic JSON representations, legacy and expanded projections,
arbitrary JSON positions, journal/batch/coalescer machinery, duplicated JSONL
framing, separate native and WebSocket lifecycle stacks, application-owned
transport matrices, global WebSocket runtime bridges, reconnect configuration
copying, physical-attempt generation machinery, and broad responsibilities in
both application `main.cpp` files and `CommandDrainController`.

Those observations identify reduction opportunities. They do not authorize a
behavioral redesign or imply that every numeric measurement must decrease in
every phase.

## Fixed phases

### P0 — Freeze behavior and architecture baseline

Repository: AISuite

Scope:

- freeze external behavior;
- record public contracts;
- record the transport matrix;
- record current architecture, targets, dependencies, files, tracked physical
  lines, headers, symbols, binaries, and tests;
- add deterministic comparison tooling;
- perform no production refactoring.

### P1 — Correct the library dependency DAG

Repository: AISuite

Scope:

- introduce the shared Frontend Protocol target;
- make the C++ frontend client depend on the shared protocol rather than the
  server/backend implementation;
- preserve existing public targets, APIs, ABI, protocol, and behavior;
- hide server implementation mechanics from the public frontend boundary.

### P2 — Add reusable SNode.C connection/WebSocket composition primitives

Repository: SNode.C

Scope:

- add a reusable one-shot/restartable physical connection-attempt primitive;
- add a per-upgrade WebSocket server factory/context;
- add a per-request WebSocket client factory/context;
- preserve all existing SNode.C APIs and behavior;
- make no AISuite semantic change.

### P3 — Introduce the shared SNode.C frontend adapter layer

Repository: AISuite

Scope:

- add a shared JSONL channel;
- add a shared WebSocket text channel;
- add shared server/client protocol binders;
- add a reusable server host and client connector;
- remove application-owned transport-composition duplication;
- preserve Unix, IPv4, IPv6, TLS, RFCOMM, WebSocket, and WSS behavior.

### P4 — Replace dual JSON projection with one typed frontend model/occurrence

Repository: AISuite

Scope:

- introduce a typed frontend model;
- introduce a typed canonical occurrence journal;
- establish one projection authority for snapshot, live delivery, and replay;
- reduce arbitrary JSON transformations;
- preserve exact Frontend Protocol v1 wire behavior.

### P5 — Simplify `codex-backend-client`

Repository: AISuite

Scope:

- separate physical connection supervision, command execution, and process/UI
  lifecycle;
- make compound workflows self-contained;
- substantially reduce `main.cpp` and controller complexity;
- preserve all CLI behavior.

### P6 — Remove compatibility scaffolding and close the reduction

Repository: AISuite

Scope:

- delete obsolete bridges, adapters, helpers, and duplicate tests;
- simplify CMake;
- update documentation;
- compare final metrics against P0;
- preserve all frozen external contracts.

The phases may not be added, removed, merged, split, renamed, or reordered. If
implementation evidence places an internal change in another already-defined
phase, move it there. Do not invent a P7.

## Phase dependencies and repository ownership

P0 is the comparison base for every later phase. P1 establishes the correct
AISuite library direction before shared transport composition is adopted. P2
adds the reusable primitives in SNode.C that P3 consumes in AISuite. P4 then
reduces representation complexity on stable protocol and transport
boundaries. P5 simplifies the reference client on those shared layers. P6
removes obsolete scaffolding and performs the final comparison.

Only P2 changes SNode.C. P0, P1, P3, P4, P5, and P6 are AISuite work. This
roadmap does not authorize changes to `codex-ui`.

## P0 non-goals

P0 does not simplify, refactor, redesign, repair, or otherwise change
production code. In particular it does not change either protocol, any public
API or ABI, target dependencies, install/export rules, transport defaults or
timeouts, authentication or authorization, synchronization or replay,
controller ownership, reconnect semantics, command/process lifetimes, CLI or
output, SNode.C, or `codex-ui`. It does not start the shared protocol library,
typed occurrence model, shared adapters, A1.7c-2, browser/TypeScript work,
provider abstraction, a line editor, or asynchronous prompt redraw.

A defect discovered during P0 is recorded with a later phase or owner-decision
route; it is not fixed in P0.

## Hard external contracts

The blocking P0 external contract covers:

- Frontend Protocol identity/version, message kinds, exact method IDs and
  ownership, expanded event families, ThreadItem discriminators, scopes,
  capabilities, and canonical authority fingerprints;
- installed/imported C++ target and output names, source compatibility, ABI
  compatibility, and SOVERSION;
- the complete `codex-backend-client` command syntax and its command,
  connection, process, queue, EOF-drain, stdin, reconnect, and controller
  semantics;
- BackendCore, FrontendService, SDK, projection, synchronization, security,
  authentication, and authorization boundaries;
- eleven distinct external transports and the separate test-only in-memory
  transport;
- inherited configurable SNode.C read/write inactivity defaults and their
  disconnect behavior.

Changing a hard external contract requires explicit owner approval. A later
phase may replace internal implementations without changing this contract.

## Evidence and metric categories

The machine-readable baseline keeps three categories separate:

1. `externalContract` is blocking. P1–P6 preserve it unless the owner explicitly
   approves a contract change.
2. `architectureMeasurements` is non-blocking comparison evidence. Target
   edges, NEEDED sets, binary sizes, header/file counts, tracked physical
   lines, and test duration may change; desired reductions must never be
   prevented by equality gates.
3. `ownerLiveEvidence` records owner-reported real interactions. It is not
   represented as reproduced by automated tooling and is not rerun when it
   would require credentials, quota, an installed Codex executable, or
   hardware.

Facts in the human report are labeled as observed facts, measured facts,
owner-reported live evidence, architectural judgments, or future targets.
Estimates are not presented as measurements.

## Expected final direction

By P6 the public protocol, APIs, ABI, transports, security, and lifecycle
behavior remain stable while the frontend client no longer depends on server
implementation, transport composition uses shared primitives and adapters,
one typed occurrence/model drives snapshot/live/replay projection, application
entry points have narrower responsibilities, and obsolete bridges and
duplicate framers are gone. Comparison reports show individual changes rather
than hiding tradeoffs behind a single score.

## Change discipline

Do not implement this roadmap as a single pull-request rewrite. Each phase has
an explicit dependency, repository owner, and compatibility proof. Large
cross-phase rewrites make behavioral drift difficult to localize and discard
the value of the P0 evidence.

The immutable baseline and human report are in
[`p0-baseline.json`](p0-baseline.json) and
[`p0-baseline.md`](p0-baseline.md).
