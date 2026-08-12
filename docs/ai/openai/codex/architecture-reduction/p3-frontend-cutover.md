# P3 Codex frontend production cutover closure

Status: P3 implementation is complete. This closure-only Commit-7 record closes
the local work and identifies the exact-head hosted gates that remain pending
after publication.

## Provenance and compatibility boundary

| Item | Final P3 value |
| --- | --- |
| Repository | `SNodeC/AISuite` |
| Branch | `codex/p3-frontend-cutover` |
| Dynamic P3 base | `7e68847e14753553402e1d468c3af15a148eea80` |
| Merged P2 PR | `#16`, base `master`, state `MERGED` |
| Project version | `0.1.0` |
| Codex SOVERSION | `2` |
| Frontend Protocol | `snodec.codex-frontend`, version `1` |
| SNode.C package observed in the reused build | `2.0.0` |

The P3 base and SNode.C dependency provenance were derived from the fetched
repository, GitHub, merged CI configuration, and active build; P3 adds no
hard-coded SNode.C source pin and does not modify SNode.C.

P3 preserves the five installed public targets:

- `AISuite::OpenAICodex`
- `AISuite::OpenAICodexBackend`
- `AISuite::OpenAICodexFrontend`
- `AISuite::OpenAICodexFrontendClient`
- `AISuite::OpenAICodexFrontendProtocol`

It also preserves output-library names, shared-library identities, installed
header paths, public Pimpl layouts, constructors, methods, callbacks, aliases,
generated facades, enums and value semantics, protocol identity/version, CLI
syntax, transport instance names, configuration hierarchy, defaults, and
security policy. No public semantic transport type, virtual interface, second
protocol target, or installed internal model/core/bridge header was added.
`codex-ui`, Frontend Protocol schemas, VERSION, and SOVERSION are unchanged.

`EventCoalescer`, `EventJournal`, and `UpdateBatchBuilder` remain compatibility
utilities owned solely by `AISuite::OpenAICodexFrontend`. They do not enter the
frontend-client header, source, link-interface, or transitive dependency graph.

## Starting architecture

At the P3 base, P2's permanent model and cores existed beside the production
legacy Pimpls:

```text
AISuite::OpenAICodexFrontend
  -> legacy FrontendService semantic engine
  -> legacy projection/journal/replay/session/controller authority

AISuite::OpenAICodexFrontendClient
  -> AISuite::OpenAICodexFrontend
  -> legacy client lifecycle/synchronization/correlation engine
  -> JSON StateReducer

codex-backend / codex-backend-client
  -> duplicated app-local JSONL framers
  -> per-attempt client reconstruction
  -> global/dynamic WebSocket runtime composition
```

The starting frontend-client closure therefore reached the frontend server,
backend, OpenAICodex, and SNode.C. The temporary legacy oracle was created in
Commit 1 so Commits 2–5 could compare the new production path at stable
borders without keeping the old implementation as a production dependency.

## Final server architecture

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
      +---- bounded JSONL native adapters
      +---- WebSocket/WSS text adapters
```

`BackendCoreBridge` borrows `BackendCoreRuntime`, owns exactly one shared
`BackendObserverSubscription`, and opens one backend command session only after
each ServerCore frontend session authenticates. Per-session backend event and
snapshot callbacks remain empty, preventing one backend event from being
duplicated per frontend connection.

The shared observer suppresses active and retired bridge-owned topology echoes.
Bridge-owned private BackendCore session IDs never appear on the wire.
Independent BackendCore sessions receive stable, non-reused frontend-visible
identities from a disjoint private range. Their open, close, and controller
facts participate coherently in live delivery, replay, snapshots, and
`FrontendService::currentController()`.

Controller acquire/release remains an asynchronous transaction. ServerCore
validates and reserves the correlation, the bridge submits through the mapped
backend session, and canonical ownership changes only after successful backend
completion. Conflicts/failures leave the prior fact unchanged; stale or late
completions are ignored. Controller and session occurrences are emitted once.

Provider and reverse commands retain generated method identity and use the
single permanent `BackendCommandMapper` and `ProviderResultProjection`
authorities. Arbitrary backend JSON is not a protocol authority. The public
service/connection Pimpls delegate receive, peer-context, close, and flush
operations to ServerCore; they contain no second journal, replay engine,
snapshot authority, controller/session authority, authorization table,
projection system, or semantic queue.

## Final client architecture and dependency graph

```text
persistent configured SNode.C connector (application layer)
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
private StateStorage -> immutable public State
```

Every generated facade converges on the existing central submission border and
one `ClientCore`. A physical `Connection` carries one ClientCore generation.
The builder constructs the public state directly from the canonical typed
candidate; production no longer serializes that candidate to protocol JSON,
parses it again, or calls the deleted `StateReducer`.

Publication uses one prepare/commit seam. Public capacity/accounting is checked
before ClientCore commits; success installs the prepared immutable state and
emits callbacks in the preserved order. Failure does not expose a candidate or
advance either public or canonical revision/cursor. Publication revision 0 is
the initial baseline, every successful later publication is exact-next, and
`UINT64_MAX` cannot wrap.

Final production target direction is:

```text
AISuite::OpenAICodexFrontendClient
  PUBLIC  -> AISuite::OpenAICodexFrontendProtocol
  PRIVATE -> ai-openai-codex-frontend-client-core
          +> ai-openai-codex-frontend-model
  SOURCES -> CanonicalStateBuilder/client implementation objects
```

It has no direct or transitive dependency on
`AISuite::OpenAICodexFrontend`, `AISuite::OpenAICodexBackend`, ServerCore,
BackendCore, SNode.C, compatibility utilities, or concrete transports. Four
transport-free typed helper source authorities are compiled into the client DSO
to preserve existing installed typed-header linkage without restoring the
OpenAICodex/SNode.C edge or creating a new installed target. Non-ELF export and
cross-DSO duplicate-symbol behavior remain final hosted-platform obligations.

## Final transport composition

One private, non-installed `internal/transport/JsonLineFramer` owns byte
accumulation, newline extraction, frame-size enforcement, malformed/oversized
reporting, and bounded reset for both native server and client adapters. It has
no protocol semantics.

All configured native and WebSocket clients retain one persistent SNode.C
client object. Initial connection and explicit user `reconnect` both use
`connect()` on that same configured object after the preceding cycle closes.
Application-owned attempt generations, per-attempt object construction,
effective-configuration copying, and duplicate reconnect scheduling are gone;
ClientCore still owns frontend physical generations and protocol continuity.

The server WebSocket upgrade uses a small file-private RAII scope around the
synchronous HTTP-to-WebSocket upgrade. It validates exact connection identity,
supports nested strict-LIFO restoration, and clears on every exit path. This is
owner-event-loop synchronous composition, not threading infrastructure: no
`thread_local`, mutex, persistent registry, global connection map, service
singleton, or installed composition API exists.

The client WebSocket factory is stateless. It obtains the actual connection's
pre-upgrade AISuite HTTP socket context and copies the connection-owned frontend
binding into the statically constructed Codex subprotocol. Global runtime
installation and per-attempt request ownership are gone.

The eleven preserved compositions are:

1. Unix JSONL (`codex-backend` / `codex-backend-client-unix`), enabled by default.
2. IPv4 JSONL (`codex-backend-ipv4` / `codex-backend-client-ipv4`).
3. IPv6 JSONL (`codex-backend-ipv6` / `codex-backend-client-ipv6`).
4. IPv4 TLS JSONL (`codex-backend-tls-ipv4` / `codex-backend-client-tls-ipv4`).
5. IPv6 TLS JSONL (`codex-backend-tls-ipv6` / `codex-backend-client-tls-ipv6`).
6. RFCOMM JSONL (`codex-backend-rfcomm` / `codex-backend-client-rfcomm`).
7. RFCOMM TLS JSONL (`codex-backend-rfcomm-tls` / `codex-backend-client-rfcomm-tls`).
8. WebSocket IPv4 (`codex-backend-websocket-ipv4` / `codex-backend-client-websocket-ipv4`).
9. WebSocket IPv6 (`codex-backend-websocket-ipv6` / `codex-backend-client-websocket-ipv6`).
10. WSS IPv4 (`codex-backend-wss-ipv4` / `codex-backend-client-wss-ipv4`).
11. WSS IPv6 (`codex-backend-wss-ipv6` / `codex-backend-client-wss-ipv6`).

Compositions 2–11 remain disabled by default. Their configuration, TLS/RFCOMM,
endpoint, origin, parser/frame bounds, authentication, and static-file security
semantics are preserved.

## Mandatory Commit-5 checkpoint

Deletion did not proceed from local evidence alone. The exact Commit-5 head,
with the complete legacy implementation and temporary oracle still present,
was pushed and independently verified:

- SHA: `a535654be86410cd751f05972fe13ec5515e6b08`
- GitHub Actions run: `31621787681`
- job: `94198152958`
- conclusion: `success`

This was deliberately bounded hosted closure, not complete ordinary CTest:
221 tests were registered, 215 were executed, 209 passed, six were runtime
skipped, none failed, and CTest took 282.72 seconds. Installed-consumer
(130.31 seconds), source-package (6.29 seconds), and binary-package (58.44
seconds) gates passed, as did P0 build/install/executable verification and its
deterministic comparison.

The runtime skips were `CodexBackendClientIpv6AcceptanceTest`,
`CodexBackendClientTlsIpv6AcceptanceTest`,
`CodexBackendClientWebSocketIpv6AcceptanceTest`, and
`CodexBackendClientWssIpv6AcceptanceTest`, where the runner's
`AI_ADDRCONFIG`/`::1` environment was unavailable;
`CodexAppServerIntegrationTest`, because the `codex` executable was absent;
and `CodexTypedAppServerIntegrationTest`, because its opt-in provider,
credential, and quota requirements were absent. The explicitly deferred hosted
tests were `CodexAppServerFixtureInfrastructureTest`,
`CodexFrontendProtocolMinimalConfigurationTest`,
`CodexFrontendProtocolLegacyBinaryCompatibilityTest`,
`CodexPublicHeaderSelfContainmentTest`,
`CodexSyntheticSecretLeakGuardTest`, and `CodexPolicyMutationTest`.

That SHA is the rollback/reference point for the pre-deletion architecture.
Commit 6 began only after explicit approval of that exact hosted-green result.

## Legacy deletion and retained authorities

Commits 4–5 removed per-attempt reconstruction and effective-configuration
copying from active composition. Commit 6 then removed the old
projection/reducer authority, obsolete service test access, runtime bridge and
dynamic WebSocket plugin targets, duplicate app-local framers, residual dead
attempt-gate/callback machinery, the entire temporary oracle, and every
oracle-only target, capture wrapper, digest/source-closure file, link proof,
policy test, and audit document. No new semantic behavior first appeared in
Commit 6.

Permanent retained authorities include the protocol DSO, typed model,
occurrence authority, typed journal, `BackendProjection`, `ServerCore`,
`ClientCore`, `BackendCoreBridge`, `CanonicalStateBuilder`, public
`FrontendService`/`FrontendConnection`, public `Client`/`Connection`/`State`,
generated facades, server-only compatibility utilities, shared private JSONL
framer, final native/static-WebSocket adapters, generated protocol fixtures,
canonical expected outputs, mutation/coverage guards, dependency policy, and
permanent conformance tests.

## Exact seven-commit ledger

1. `5c336611` — `Freeze the P3 cutover contract and preserve the legacy oracle`
2. `37b10597` — `Cut the public Codex frontend service over to ServerCore`
3. `aecf72c2` — `Cut the public Codex frontend client over to ClientCore`
4. `0ebf395d` — `Consolidate native Codex frontend transport composition`
5. `a535654b` — `Cut WebSocket transports over to static composition`
6. `341ae5f` — `Remove the legacy Codex frontend implementation`
7. this commit — `Close and verify the Codex P3 cutover`

Commit 7 contains documentation/evidence only and no path below `src/`. Its
full SHA is recorded in the draft PR body after the commit exists; embedding it
inside itself would be self-referential.

## Local validation and deferred merge gates

The affected-target build and final incremental all-target build passed using:

```text
cmake --build build-p2-complete --parallel 24 --target <affected targets>
cmake --build build-p2-complete --parallel 24
```

The explicit focused local gate passed 62 of 62 tests, with zero failures and
zero skips, in 6.03 seconds. A directly affected follow-up selection passed 2
of 2 tests in 0.08 seconds. Every CTest invocation used:

```text
ctest --test-dir build-p2-complete --output-on-failure --parallel 24 --timeout 60 <explicit focused selection>
```

No prohibited long-running gate was run locally. Specifically, complete
ordinary CTest, installed consumers, source/binary package closure, full
public-header installed rebuild, ABI cold/relinked probes, complete P0
build/install-backed verification, deterministic full P0 captures/comparison,
the all-eleven live transport suite, real provider integration, and
credential/quota/account integration were not run locally.

The final exact Commit-7 head must still pass hosted closure and independent
review. Its workflow ID/status is pending publication. P3 is not declared
merge-ready from the focused local gate alone.
