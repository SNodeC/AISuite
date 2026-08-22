# P0 Codex architecture-reduction baseline

This report is the human-readable companion to the immutable
[`p0-baseline.json`](p0-baseline.json). The JSON artifact is authoritative for
machine comparison. This report explains what was observed, measured, judged,
and targeted without turning current internal complexity into a permanent
architecture contract.

P0 freezes the external behavior and compatibility borders. The current
frontend server and client implementations are the behavioral reference and a
temporary executable differential oracle for P2; they are not the
implementation strategy or architectural dependencies of the replacements.

Evidence labels used below:

- **Observed fact**: directly established by current source, generated
  authority, configuration, or behavior already covered by a named test.
- **Measured fact**: produced by the deterministic P0 capture from tracked
  files, the CMake File API, the temporary install, ELF metadata, or CTest.
- **Owner-reported live evidence**: reported from the owner's real interaction;
  Codex did not reproduce it.
- **Architectural judgment**: a qualitative interpretation, not a scientific
  score.
- **Future target**: an intended P1–P3 direction, never a P0 implementation.

The machine artifact also identifies how each blocking fact is supported:
`generated-authority-derived`, `build-or-install-derived`,
`executable-observation`, `named-test-evidence`,
`owner-approved-declarative-contract`, or
`inherited-dependency-evidence`. A declarative obligation is not described as
mechanically derived merely because the capture tool records it.

## 1. Baseline provenance

**Observed fact.** P0 branches from authoritative `origin/master` at
`4c0cfbf99667fef64c9fed010d84031248ceaba2`. That commit is the merged PR #14
commit and has the same tree as PR #14 source head
`d524a6788631680e9fd86bda94ef49337a370d4c`. Repository provenance is
`SNodeC/AISuite`, source branch `master`, project version `0.1.0`, and Codex
SOVERSION 2.

**Measured fact.** The exact dependency used by the configured P0 build is
repository `SNodeC/snode.c`, commit
`6ae8fafcd50052a9d86932b0be721ef39cce7a44`, version `2.0.0`. The capture derives
that provenance from the checked-out dependency source, its configured build,
and the installed package used by AISuite. The normalized machine artifact
does not retain the checkout path. A later SNode.C SHA or version is reported
as dependency-change evidence rather than blocked, because P1 intentionally
changes SNode.C.

**Measured fact.** The P0 capture used a feature-complete GNU 15.3.0 Debug
Ninja build with CMake 4.3.4. Applications, tests, the frontend client SDK,
TLS, WebSocket, and RFCOMM were enabled. Capture date was 2026-08-09. The host
resource observation immediately before the final build was 28 logical CPUs,
62 GiB memory total with 42 GiB available, and 59 GiB swap total with 31 GiB
free.

No absolute source/build/install paths, user names, hostnames, credentials,
credential-file paths, or capture timestamps occur in the machine artifact.

## 2. Current architecture diagram

**Observed fact.** The current legacy behavioral reference has one provider
boundary, one backend authority, one frontend service, application-owned
server/client transport composition, and a transport-neutral SDK:

```text
Codex App Server
       |
       | typed App Server protocol
       v
 AppServerClient
       |
       v
  BackendCore -------- typed state/events/results
       |
       v
 FrontendService ----- auth/scopes/controller/journal/snapshot/replay
       |
       | complete Frontend Protocol v1 JSON objects
       v
 server application adapters === 11 physical compositions === client adapters
                                                               |
                                                               v
                                                   frontend::client::Client
                                                               |
                                                               v
                                                      immutable SDK State
```

**Architectural judgment.** Domain state, protocol security, bounded replay,
and transport semantics are necessary complexity. Dual projections, duplicated
framers, application-owned matrices, runtime bridges, and broad application
lifecycle controllers are accidental complexity targeted later.

This entire current frontend path remains executable only as needed for
differential evidence and rollback during the replacement. It is an oracle,
not a design dependency of the greenfield server or client core.

## 3. Current library DAG

**Measured fact.** Configured installed-target property introspection records this
public link-interface direction; the CMake File API separately records resolved
in-project build dependencies:

```text
AISuite::OpenAICodexFrontendClient
    -> AISuite::OpenAICodexFrontend
        -> AISuite::OpenAICodexBackend
            -> AISuite::OpenAICodex
                -> nlohmann_json::nlohmann_json
```

The frontend client therefore currently depends on the frontend server target
and transitively on backend/provider-side implementation. This is the P0 fact,
not the desired direction. There is no transitional cleanup phase: P2 creates
the permanent client with the correct dependency direction from inception, and
P3 removes this obsolete graph when it deletes the old implementation.

Application-private composition currently includes
`codex-reference-authentication`, `codex-backend-stream-adapter`,
`codex-backend-runtime-bridge`, `codex-backend-web-adapter`,
`codex-backend-websocket-subprotocol`, and
`codex-backend-client-support`. The complete normalized target records and
edges are in `architectureMeasurements.cmake`.

## 4. Current server data path

**Observed fact.** Typed App Server input enters through `AppServerClient`, is
reduced into the authoritative `BackendCore` state/occurrence stream, and is
projected by `FrontendService`. The service authenticates before creating a
backend session, authorizes by scopes and controller facts, retains a bounded
canonical journal, and emits snapshot/live/replay data as complete JSON
objects. JSONL or WebSocket server adapters then own framing and physical I/O.

Raw App Server JSON is never a Frontend Protocol v1 product. All listeners
borrow the same service, state, controller, sequence, and journal.

## 5. Current client data path

**Observed fact.** Each application-owned SNode.C client adapter converts JSONL
records or WebSocket text messages into complete JSON objects. One SDK Client
owns Hello, credential placement, request IDs, response correlation,
synchronization, replay cursors, validation, reduction, and immutable State.
The same Client survives sequential physical attachments; at most one is
active. The reference application owns endpoint selection, explicit reconnect,
commands, compound workflows, stdin, presentation, and process shutdown.

## 6. Current transport composition

**Observed fact.** The server instantiates Unix, IPv4, IPv6, TLS IPv4/TLS IPv6,
RFCOMM/RFCOMM TLS, WebSocket IPv4/IPv6, and WSS IPv4/IPv6 separately. The client
does the same. Unix is enabled by default; every other named instance is
disabled by default. The installed executables were checked for the configured
named-instance strings, while feature availability came from the configured
CMake cache.

The transport evidence is deliberately mixed rather than reconstructed from a
single declarative table. Server/client instance names and enabled/disabled
state are executable observations from installed expanded help; feature gates
are build-derived from the configured CMake cache; carrier, family, framing,
TLS, subprotocol, default, and authentication obligations are backed by the
named native, CLI-compatibility, WebSocket/WSS, TLS, and authentication tests
and by the owner-approved transport contract. Each blocking JSON row names its
evidence classes and concrete sources. Hardware limitations remain explicit.

**Architectural judgment.** The transport set is necessary external behavior;
the number of application-owned factories, adapters, bridges, and lifecycle
stacks is a reduction dimension.

## 7. Transport matrix

**Observed fact** unless a test limitation is explicitly marked manual or
inherited. Every external row is classified with `build-or-install-derived`,
`executable-observation`, `named-test-evidence`, and
`owner-approved-declarative-contract` evidence;
the named tests in the table identify the executable coverage rather than
serving as invented physical-hardware evidence.

| Transport | Server / client named instance | Feature | Carrier; family; framing | Encryption / default | Authentication and peer facts | Automated P0 coverage | Limitations |
|---|---|---|---|---|---|---|---|
| Unix JSONL | `codex-backend` / `codex-backend-client-unix` | always | Unix stream; Unix; JSONL | none / enabled | owner-only pathname; peer credentials where supported; verified-local; bearer fallback where required | `CodexFrontendNativeTransportTest`, `CodexBackendUnixAcceptanceTest`, `CodexBackendClientUnixAcceptanceTest`, `CodexBackendClientRealBackendAcceptanceTest`, `CodexBackendClientThreadWorkflowAcceptanceTest` | automated coverage uses a fake provider; owner real-provider evidence is separate |
| IPv4 JSONL | `codex-backend-ipv4` / `codex-backend-client-ipv4` | always | TCP; IPv4; JSONL | none / disabled | loopback default; non-loopback needs insecure override; bearer; socket metadata | `CodexFrontendNativeTransportTest`, `CodexBackendClientIpv4AcceptanceTest` | none beyond local socket support |
| IPv6 JSONL | `codex-backend-ipv6` / `codex-backend-client-ipv6` | always | TCP; IPv6; JSONL | none / disabled | loopback default; non-loopback needs insecure override; bearer; socket metadata | `CodexFrontendNativeTransportTest`, `CodexBackendClientIpv6AcceptanceTest` | live loopback skips without usable host IPv6 |
| IPv4 TLS JSONL | `codex-backend-tls-ipv4` / `codex-backend-client-tls-ipv4` | `AISUITE_ENABLE_CODEX_FRONTEND_TLS` | TCP; IPv4; JSONL | TLS / disabled | verified TLS plus bearer; encrypted socket metadata | `CodexFrontendNativeTransportTest`, `CodexBackendClientTlsIpv4AcceptanceTest` | repository test TLS material |
| IPv6 TLS JSONL | `codex-backend-tls-ipv6` / `codex-backend-client-tls-ipv6` | `AISUITE_ENABLE_CODEX_FRONTEND_TLS` | TCP; IPv6; JSONL | TLS / disabled | verified TLS plus bearer; encrypted socket metadata | `CodexFrontendNativeTransportTest`, `CodexBackendClientTlsIpv6AcceptanceTest` | repository test TLS material and usable host IPv6 |
| RFCOMM JSONL | `codex-backend-rfcomm` / `codex-backend-client-rfcomm` | `AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM` | Bluetooth RFCOMM; RFCOMM; JSONL | none / disabled | bearer; address/channel metadata; pairing is not frontend authentication | `CodexFrontendNativeTransportTest`, `CodexBackendUnixCliCompatibilityTest`, `CodexBackendClientTransportCompositionTest`, `CodexBackendClientAuthenticationTest` | configuration/factory coverage only; physical exchange needs RFCOMM hardware |
| RFCOMM TLS JSONL | `codex-backend-rfcomm-tls` / `codex-backend-client-rfcomm-tls` | RFCOMM switch | Bluetooth RFCOMM; RFCOMM; JSONL | TLS / disabled | verified TLS plus bearer; encrypted address/channel facts | `CodexFrontendNativeTransportTest`, `CodexBackendUnixCliCompatibilityTest`, `CodexBackendClientTransportCompositionTest`, `CodexBackendClientAuthenticationTest` | configuration/factory coverage only; physical exchange needs hardware and TLS material |
| WebSocket IPv4 | `codex-backend-websocket-ipv4` / `codex-backend-client-websocket-ipv4` | `AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET` | HTTP/TCP upgrade; IPv4; WebSocket text | none / disabled | Origin/upgrade policy plus bearer; HTTP peer facts; subprotocol `codex` | `CodexFrontendWebHttpIntegrationTest`, `CodexFrontendWebSocketIntegrationTest`, `CodexBackendClientWebSocketIpv4AcceptanceTest` | none beyond local socket/plugin support |
| WebSocket IPv6 | `codex-backend-websocket-ipv6` / `codex-backend-client-websocket-ipv6` | `AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET` | HTTP/TCP upgrade; IPv6; WebSocket text | none / disabled | Origin/upgrade policy plus bearer; HTTP peer facts; subprotocol `codex` | `CodexBackendClientWebSocketIpv6AcceptanceTest` | live loopback skips without usable host IPv6 |
| WSS IPv4 | `codex-backend-wss-ipv4` / `codex-backend-client-wss-ipv4` | WebSocket + TLS switches | HTTPS/TCP upgrade; IPv4; WebSocket text | TLS / disabled | TLS, Origin/upgrade policy, bearer, encrypted HTTP facts; subprotocol `codex` | `CodexFrontendWebSocketTlsIntegrationTest`, `CodexBackendClientWssIpv4AcceptanceTest` | repository test TLS material |
| WSS IPv6 | `codex-backend-wss-ipv6` / `codex-backend-client-wss-ipv6` | WebSocket + TLS switches | HTTPS/TCP upgrade; IPv6; WebSocket text | TLS / disabled | TLS, Origin/upgrade policy, bearer, encrypted HTTP facts; subprotocol `codex` | `CodexBackendClientWssIpv6AcceptanceTest` | repository test TLS material and usable host IPv6 |

The in-memory transport is test-only, has no external listener, and is not a
twelfth application transport. Credential/quota-consuming real provider work
and physical RFCOMM exchange are not automated by P0.

## 8. Public targets, API, and ABI

**Build-or-install-derived fact.** The following four installed shared targets
are the required existing target subset:

| Imported target | Build target | Output | Type | VERSION / SOVERSION | Installed export |
|---|---|---|---|---|---|
| `AISuite::OpenAICodex` | `ai-openai-codex` | `libaisuite-openai-codex.so` | shared | 0.1.0 / 2 | `lib/cmake/AISuite/AISuiteTargets.cmake` |
| `AISuite::OpenAICodexBackend` | `ai-openai-codex-backend` | `libaisuite-openai-codex-backend.so` | shared | 0.1.0 / 2 | same |
| `AISuite::OpenAICodexFrontend` | `ai-openai-codex-frontend` | `libaisuite-openai-codex-frontend.so` | shared | 0.1.0 / 2 | same |
| `AISuite::OpenAICodexFrontendClient` | `ai-openai-codex-frontend-client` | `libaisuite-openai-codex-frontend-client.so` | shared | 0.1.0 / 2 | same |

Each required target must retain its imported name, output-library identity,
target type, compatible VERSION/SOVERSION, installed package/export
availability, and all P0 installed public-header paths. New public or private
targets, additive public headers, and additive compatible symbols are
permitted. The capture and comparison therefore use required-subset semantics,
not equality of the complete future target or header set.

Public API and ABI compatibility remain hard project requirements. P0
mechanically freezes target/package/header identity and maps the dedicated
installed-consumer, public-header policy, public-header self-containment,
symbol-visibility, dependency-policy, and existing source/ABI compatibility
evidence. It does not claim that the baseline tool alone proves complete C++
API or ABI compatibility. The current link DAG, NEEDED sets, binary sizes,
exported-symbol counts and fingerprints, and total header count are
non-blocking comparison measurements; additive ABI-compatible symbols are not
rejected through a hand-maintained symbol allow-list.

## 9. Protocol contract

**Generated-authority-derived fact.** The reviewed/generated authorities derive:

| Field | P0 value |
|---|---:|
| identity | `snodec.codex-frontend` |
| version | 1 |
| message kinds | 8 |
| methods | 105 |
| native / provider / reverse | 7 / 86 / 12 |
| expanded event families | 26 |
| ThreadItem discriminators | 18 |
| scopes | 12 |
| defined capabilities | 19 (14 static mechanisms, 1 conditional topology, 4 products) |
| implemented in the feature-complete P0 runtime | 16 (14 static + `multi_transport` + `cpp_client_sdk`) |

The exact method-ID/wire-name/ownership set, event-family set, discriminator
set, scope set, capability catalog/categories/runtime truth, and canonical
SHA-256 values for the schema, generated fixture, reducer fixture, reviewed
registry, manifest, and C++ binding authority are in
`externalContract.protocol`. Existing generator/currentness checks validate
the generated C++ authority instead of text-parsing it.

## 10. CLI and lifecycle contract

**Observed fact.** The parser authority freezes:

```text
help; quit; reconnect; snapshot; replay <sequence>; acquire; release; threads
start [--cwd <path>] [--model <model>] [--model-provider <provider>]
      [--approval-policy <policy>] [--sandbox-mode <mode>] [--ephemeral]
resume <thread-id> [the same options except --ephemeral]
new [thread-start-options] -- <prompt> | new <prompt>
read <thread-id>; turn <thread-id> <prompt>
interrupt <thread-id> <turn-id>; raw <json>; watch on; watch off
```

**Executable observation.** The installed `codex-backend-client` was started
without a remote attachment, sent the local `help` and `quit` commands, and its
normalized interactive help was compared byte-for-byte with
`tests/policy/codex/fixtures/codex-backend-client-help.txt`. The normalized
SHA-256 is
`780c918a578354fbfe8f45892dc534701e3bf9199a9e688c2a1177073f0f4dc3`.
The vocabulary and syntax above were parsed from that observation and checked
against the P0 contract. Semantics that help cannot express are supported by
`CodexBackendClientCommandTest`, `CodexBackendClientCommandDrainTest`,
`CodexBackendClientPresenterTest`, and
`CodexBackendClientStdinReaderTest`, together with the owner-approved lifecycle
contract.

With `new` options, `--` is required and everything after it is prompt text;
without options it may be omitted. `raw` accepts only one known generated
method with object params through normal SDK validation/correlation. It cannot
send Hello, unknown methods, caller IDs, or App Server JSON. `watch` changes
presentation only.

Malformed input, pre-acceptance rejection, and normal `ok=false` responses end
only that command. A command failure neither closes a valid connection nor
terminates the process. Transport or fatal protocol/state failure closes the
attachment and completes/fails accepted operations exactly once; retained
State becomes stale and the process remains Disconnected. Only quit,
Ctrl-C/framework shutdown, true EOF after drain, or unrecoverable application
infrastructure may terminate normally.

The queue defaults are 256 commands and 16 MiB retained input. Zero means zero;
the newest overflow is rejected without evicting older work. Disconnected does
not retain remote commands. EOF drains accepted/queued work in order, retains
ordinary failure as final nonzero status, continues later work, and never
retries. `EAGAIN`/`EWOULDBLOCK` is not EOF; only `read() == 0` is. Regular-file
stdin stays rejected and pipes stay supported. Asynchronous prompt redraw is
not frozen.

**Inherited-dependency evidence.** At SNode.C commit
`6ae8fafcd50052a9d86932b0be721ef39cce7a44`, version `2.0.0`, the configured
read- and write-inactivity defaults are both 60 seconds. These are inherited,
configurable SNode.C behavior, not hard-coded AISuite constants. The authorities
are `CMakeCache.txt:SNODEC_READ_TIMEOUT`,
`CMakeCache.txt:SNODEC_WRITE_TIMEOUT`, SNode.C `src/net/CMakeLists.txt`,
`src/net/config/ConfigConnection.cpp`, and
`src/core/socket/stream/SocketConnection.hpp`; installed connection help is an
independent executable observation of the same defaults. Owner-live
observations 13–18 record that inactivity closed the physical attachment while
the reference process stayed alive in Disconnected, retained local `help`, and
permitted explicit reconnect. No command was retried and controller ownership
was not restored. A different future SNode.C SHA is not silently treated as
proof of the same inherited defaults: capture/compare reports both the
dependency revision and any timeout-value change.

## 11. Synchronization, replay, and live Snapshot

**Observed fact.** Initial and explicit snapshot/replay remain supported. A
live Snapshot while Ready is a transactional authoritative replacement: it
does not fabricate another `sync.complete`, cancel pending correlation, or
leave Ready. Lower sequence is rejected; equal sequence is accepted
authoritatively; higher sequence advances the durable cursor.

Exact identities are retained without unrelated first/last substitution. One
thread-list page emits each identity once and one compact
`threadList.updated`. Snapshot/live/replay use the reviewed item projector;
backend spellings never become frontend discriminators. Accumulated content
remains accumulated-value semantics, and producer and consumer both validate
expanded events.

## 12. Security and authentication

**Observed fact.** Authentication precedes session creation. Verified local
Unix trust requires same-user peer evidence and the owner-only socket policy;
otherwise protected-file bearer authentication is used. Plaintext IP is
loopback by default, and non-loopback plaintext requires the explicit insecure
override. TLS/WSS verifies configured material. WebSocket also enforces the
endpoint, Origin, credential channel, upgrade, text-frame, and exact `codex`
subprotocol rules.

Authorization and scope projection apply to snapshot, live, and replay.
Controller serialization, outbound bounds, redaction/information ceilings,
and secret-safe diagnostics remain unchanged.

## 13. Owner live evidence

**Owner-reported live evidence.** On 2026-08-08 the owner manually exercised
Unix JSONL against the real Codex App Server. Codex did not reproduce this
credential/quota-consuming evidence. The owner observed, in order:

1. clean initial connection and synchronization;
2. distinct thread identities;
3. compact `threadList.updated`;
4. controller acquisition;
5. successful real `thread.start`;
6. successful real `turn.start`;
7. `userMessage`;
8. `agentMessage`;
9. agent content equal to the requested short answer;
10. completed agent item;
11. completed turn;
12. subsequent commands on the same connection;
13. the expected SNode.C inactivity timeout;
14. the client process remaining alive in Disconnected;
15. local `help` remaining available;
16. local rejection of remote commands while Disconnected;
17. successful explicit reconnect;
18. replay on reconnect when continuity was available;
19. controller ownership not being restored;
20. nonfatal `permission_denied` command error;
21. successful explicit reacquisition;
22. another real turn returning `Codex working`.

Accidental terminal input concatenation is not recorded as a software failure,
and terminal prompt redraw is outside P0.

## 14. Source, file, and tracked-physical-line measurements

**Measured fact.** Only `git ls-files` entries are counted. “Lines” below means
tracked physical lines, not logical LOC. Generated and hand-written totals are
separate in the JSON artifact. Source scope is divided into:

1. the production/reduction subject: `src/ai/openai/codex`,
   `src/apps/codex-backend`, and `src/apps/codex-backend-client`;
2. permanent compatibility/conformance support: protocol generators and
   fixtures, component tests, and non-P0 policy/tool/documentation files;
3. P0-only baseline infrastructure, reported separately and never treated as
   frontend architecture to reduce.

The final P3 comparison focuses primarily on the production subject and reports
permanent support and P0 infrastructure changes separately. The table below is
reconciled from the final machine capture.

| Root | Files | Tracked physical lines | Bytes | C/C++ sources | C/C++ headers | Python | CMake | JSON | Markdown |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `docs/ai/openai/codex` | 28 | 61,898 | 2,091,101 | 0 | 0 | 0 | 0 | 2 | 26 |
| `src/ai/openai/codex` | 202 | 85,229 | 5,347,661 | 60 | 138 | 0 | 4 | 0 | 0 |
| `src/apps/codex-backend` | 28 | 4,047 | 176,184 | 13 | 13 | 0 | 1 | 0 | 1 |
| `src/apps/codex-backend-client` | 25 | 4,745 | 203,064 | 12 | 11 | 0 | 1 | 0 | 1 |
| `tests/component/codex` | 151 | 112,546 | 5,592,184 | 122 | 4 | 11 | 2 | 4 | 0 |
| `tests/policy/codex` | 8 | 2,076 | 80,918 | 3 | 0 | 1 | 2 | 0 | 0 |
| `tools/codex` | 8,840 | 1,475,767 | 52,104,962 | 0 | 0 | 6 | 0 | 8,828 | 1 |
| `tools/frontend` | 5 | 57,716 | 1,944,703 | 0 | 0 | 2 | 0 | 3 | 0 |

The P0 infrastructure classification is exactly:

- `docs/ai/openai/codex/architecture-reduction/README.md`;
- `docs/ai/openai/codex/architecture-reduction/p0-baseline.json`;
- `docs/ai/openai/codex/architecture-reduction/p0-baseline.md`;
- `tests/policy/codex/CodexArchitectureReductionBaselineTest.py`;
- `tests/policy/codex/fixtures/codex-backend-client-help.txt`;
- `tools/codex/capture_architecture_baseline.py`.

The baseline JSON and this report are excluded from every encoded byte/line
metric, so `p0-baseline.json` never measures itself. The other four P0-only
paths have separate `baselineInfrastructure` metrics and are excluded from all
production and permanent-support totals. Those four measured paths contain 4
files, 5,230 tracked physical lines, and 240,465 bytes. The production subject
contains 255 files, 94,021 tracked physical lines, and 5,726,909 bytes;
permanent compatibility/conformance support contains 9,032 files, 1,710,003
tracked physical lines, and 61,813,868 bytes. Capture builds two complete
normalized documents from an unchanged tree/build/install/test result and
requires exact equality before writing output; volatile timestamp/resource
observations are not part of those deterministic machine sections.

The generated classification explicitly includes `GeneratedProtocol.h`,
`GeneratedProtocolSchema.inc`, `GeneratedBindings.h`, `GeneratedFacades.cpp`,
the generated protocol fixture, and reducer conformance fixture.

## 15. Target and dependency measurements

**Measured fact.** The File API captured 14 Codex target records: 13 production
or build-support targets and one test-support target. The normalized inventory
records target type, File API source-entry count, build outputs, resolved
in-project build dependencies, relevant compile definitions, and install
destinations. It contains 52 resolved in-project edges, or 48 when the
test-support target is excluded. No source-CMake regular expression was used
to infer relationships.

The classifications are four public libraries, two installed applications,
six application-private adapter/authentication/support targets, one production
implementation helper, and one test-support target. The six application-private
targets are the reference authenticator, stream adapter, runtime bridge, web
adapter, WebSocket subprotocol, and frontend-client application support.

These edges and counts are comparison measurements, not equality gates.

## 16. Installed headers and binary measurements

**Measured fact.** One temporary P0 prefix contained 102 normalized installed
files and 78 public Codex headers: 29 core, 7 backend, 9 frontend-service, and
33 frontend-client headers.

| Artifact | Public headers | Bytes | SONAME | Exported dynamic symbols | Sorted-name-set SHA-256 | NEEDED summary |
|---|---:|---:|---|---:|---|---|
| `libaisuite-codex-backend-runtime.so` | 0 | 11,944,192 | `libaisuite-codex-backend-runtime.so.2` | 637 | `63eb6cb00cf1f3abacbd20300c1f8898d0ced104431373f734ae91dba44e5fee` | 5; frontend, WebSocket server, and system runtimes |
| `libaisuite-openai-codex-backend.so` | 7 | 627,253,408 | `libaisuite-openai-codex-backend.so.2` | 129,667 | `7b2ef5bac0b7f2247d8dca8d3f799ea38ec1e0407659dda0b23d27dcd17343d3` | 8; core Codex, SNode.C core/logger/utils, and system runtimes |
| `libaisuite-openai-codex-frontend-client.so` | 33 | 466,150,768 | `libaisuite-openai-codex-frontend-client.so.2` | 441 | `96facec0964a8a02f2542f3852520e9d5d75f0ad84b9e280a1756c465e48b353` | 6; frontend, core Codex, and system runtimes |
| `libaisuite-openai-codex-frontend.so` | 9 | 699,502,840 | `libaisuite-openai-codex-frontend.so.2` | 112,924 | `6b5ea004007a6af9fdb1feef1a56d129a30217e3d50f35b71352c0ca317b9b8d` | 8; backend/core Codex, SNode.C core/utils, and system runtimes |
| `libaisuite-openai-codex.so` | 29 | 213,141,232 | `libaisuite-openai-codex.so.2` | 74,625 | `930ec02c3be547d6972c7f2d4a0fb5288e88488389c621f8984b8d50066cb2e0` | 6; SNode.C core/logger/utils and system runtimes |
| `libsnodec-websocket-codex-server.so` | 0 | 9,508,704 | `libsnodec-websocket-codex-server.so.2` | 137 | `7f5c043769001778cd58a1ffbfda1b1ede0ce139148e89f454527d86707dc110` | 4; backend runtime and system runtimes |
| `codex-backend` | n/a | 30,186,080 | n/a | n/a | n/a | 27; four AISuite Codex/runtime libraries plus SNode.C/system runtimes |
| `codex-backend-client` | n/a | 236,647,760 | n/a | n/a | n/a | 25; frontend client/service libraries plus SNode.C/system runtimes |

Installed header paths and package/export files are enumerated in the JSON.
Sizes, current NEEDED sets, symbols, and header counts report change during
P1–P3 but do not block intended reduction.

## 17. Test inventory and results

**Measured fact.** The feature-complete build registered 191 tests. The final
ordinary suite at parallelism 2 recorded 190 passed, 1 skipped, and 0 failed.
CTest's JUnit authority recorded 440 seconds (the console reported 440.05
seconds).

Exact skips:

- `CodexTypedAppServerIntegrationTest`: `SKIP: set
  SNODEC_RUN_CODEX_TYPED_INTEGRATION=1 to run the real typed Codex App Server
  integration; the test may use configured credentials and quota`.

The JSON contains every test name, labels, label counts, transport coverage,
and frozen-contract mapping. Required gates cover generated authorities,
bindings, codec/schema, projection, FrontendService, service-to-SDK,
lifecycle, synchronization/replay/live Snapshot, exact identity/item kinds,
command/connection/process lifetime, reconnect composition, Unix/IP/TLS/
WebSocket/WSS, authentication, installed consumer, headers, symbols,
dependencies, and binary/source packages. Physical RFCOMM, real provider
credentials/quota, and inherited SNode.C defaults are explicitly manual or
inherited rather than represented by fake tests.

## 18. Complexity hotspots

**Measured fact.** Hotspot size is tracked bytes and tracked physical lines:

| File | Bytes | Tracked physical lines |
|---|---:|---:|
| `src/ai/openai/codex/backend/BackendCore.cpp` | 117,312 | 2,365 |
| `src/ai/openai/codex/backend/Reducer.cpp` | 256,560 | 4,132 |
| `src/ai/openai/codex/frontend/FrontendService.cpp` | 147,265 | 2,858 |
| `src/ai/openai/codex/frontend/client/Client.cpp` | 133,809 | 2,469 |
| `src/ai/openai/codex/frontend/client/State.cpp` | 389,208 | 7,025 |
| `src/ai/openai/codex/frontend/detail/BackendProjectionBuilder.cpp` | 121,041 | 2,139 |
| `src/apps/codex-backend-client/CommandDrainController.cpp` | 32,363 | 751 |
| `src/apps/codex-backend-client/FrontendWebSocketClient.cpp` | 15,111 | 390 |
| `src/apps/codex-backend-client/main.cpp` | 54,193 | 988 |
| `src/apps/codex-backend/main.cpp` | 23,187 | 395 |

**Architectural judgment.** The major provider-to-SDK representation stages
are typed App Server values, typed BackendCore state/occurrence, legacy
canonical JSON occurrence, expanded wire JSON, and typed SDK State. This is a
five-stage explanatory model, not a complexity score. The two canonical
representations are legacy and expanded.

Other diagnosed classes are arbitrary JSON positions, event
coalescer/journal/batch machinery, server/client instantiation matrices, the
two application JSONL framers, separate native/WebSocket lifecycle stacks,
server/client runtime bridges, reconnect configuration copying, physical
attempt generations, and broad responsibilities in both `main.cpp` files and
`CommandDrainController`. The exact qualitative lists and evidence paths are
in `complexityInterpretation`.

## 19. Expected P1–P3 reduction dimensions

**Future target.** The final owner-approved dependency graph is:

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

P1 and P2 may proceed in parallel after P0 merges. P2 is transport-neutral and
does not require P1. P3 requires the merged results of both. Only P1 modifies
SNode.C; P0, P2, and P3 modify AISuite. None modifies `codex-ui`.

- P1 completes reusable SNode.C connect and direct per-upgrade/per-request
  WebSocket composition. Public `SocketClient::connect()` remains the explicit
  operation; no `connectOnce()` and no public `reconnect()` are planned. A
  later explicit `connect()` may start a new cycle after failure/disconnect,
  without losing configured retry/reconnect, active-flow, identity/statistic,
  shutdown, or stale-callback protections. A temporary `SocketClient` handle
  leaving scope must not cancel an already-started shared asynchronous flow.
- P2 builds the complete permanent frontend beside the old executable oracles:
  one shared protocol boundary, one transport-neutral server core, one
  transport-neutral client core, the typed frontend model/occurrence design,
  one snapshot/live/replay authority, and complete differential validation.
  Production applications remain on the old implementation throughout P2.
  The new client depends on neither server core, `BackendCore`, nor SNode.C
  transport modules.
- P3 switches the four required public targets, both applications, packages,
  and all eleven external transports to the replacements, using the merged P1
  primitives. In the same controlled PR it deletes both old implementations,
  temporary oracle wiring, duplicate framers/lifecycle stacks, runtime bridges,
  configuration copying, and obsolete adapters/tests. P3 performs the final P0
  comparison after differential, protocol, transport, package/header/ABI, and
  owner-live closure.

There is no preparatory AISuite dependency-DAG cleanup phase and no separate
protocol-extraction phase. The current client-to-server/backend dependency is
a measured P0 fact and diagnosed accidental complexity; it may remain unchanged
until P3 removes the old implementation. P2 creates the permanent protocol
boundary and correct client dependency direction from inception without making
the new design depend on old frontend classes.

The stable differential borders are:

```text
BackendCore typed inputs/state/occurrences
          |                    |
          v                    v
 old frontend server    new frontend server
          |                    |
          +---------+----------+
                    v
       canonical Frontend Protocol v1 output

canonical Frontend Protocol v1 input
          |                    |
          v                    v
 old frontend client    new frontend client
          |                    |
          +---------+----------+
                    v
 public results / errors / immutable State / callbacks
```

P2 differential coverage must include all 105 methods, all 26 expanded event
families, all 18 ThreadItem discriminators, all 12 scopes, capability
negotiation, Hello/Welcome, snapshots, initial synchronization, replay, live
Snapshot, equal-sequence groups, lower/higher/gapped sequence handling,
controller/session behavior, authentication and authorization failures, scope
projection, redaction/information ceilings, pending reverse requests, command
errors, malformed messages, item-content accumulation, truncation,
unknown/future-safe containment, and queue/backpressure terminal behavior.

The starting oracle corpus is the generated Frontend Protocol fixture, reducer
conformance fixture, projection and schema fixtures/tests,
`CodexFrontendServiceClientIntegrationTest`, and the separately classified
owner-live evidence. Their canonical hashes remain frozen. Future differential
normalization must be narrow, reviewed, and documented; broad JSON-ignore rules
are prohibited and differences in stable Frontend Protocol semantics fail.

The final P3 comparison must make visible protocol/API/ABI/transport/security/lifecycle
compatibility alongside target-DAG direction, missing or additive public
targets/headers, SNode.C revision changes, old-oracle removal, production and
adapter target changes, both `main.cpp` line changes, duplicate framer and
runtime bridge removal, configuration-copy and attempt-generation reduction,
arbitrary JSON reduction, binary size, NEEDED/symbol changes, and test
count/duration. No aggregate score is used. A correct shared protocol target
may temporarily increase target count, and not every numeric measurement must
decrease.

The final delivery order is PR #14/A1.7c-1, P0, parallel P1/P2, P3, then
A1.7c-2. A1.7c-2 starts only after P3, targets the reduced canonical frontend
architecture, and leaves `codex-ui` untouched throughout P0–P3. P0–P3 do not
implement the Qt UI. There is no P4, P5, P6, or P7.

## 20. Repeatable capture and comparison commands

**Observed fact.** The P0 feature-complete configuration requested the File API
before configure, used the CI feature switches, installed once into a temporary
prefix, retained ordinary CTest JUnit output, and supplied the exact SNode.C
source and matching configured build used by AISuite. In these commands, the
SNode.C checkout/build/install are one actual dependency instance and the two
install prefixes remain outside the AISuite source tree. Their paths are never
stored in normalized output:

```sh
work_root="path/to/a-work-area-outside-the-AISuite-source"
snodec_source_dir="$work_root/snode.c"
snodec_build_dir="$work_root/snodec-build"
snodec_install_dir="$work_root/snodec-stage"
aisuite_install_dir="$work_root/aisuite-p0-contract-install"
baseline_runtime_libraries="$snodec_install_dir/lib:$snodec_install_dir/lib/snode.c/web/http:$snodec_install_dir/lib/snode.c/web/http/upgrade"
baseline_linker_flags="-Wl,-rpath-link,$snodec_install_dir/lib -Wl,-rpath-link,$snodec_install_dir/lib/snode.c/web/http -Wl,-rpath-link,$snodec_install_dir/lib/snode.c/web/http/upgrade"

git -C "$snodec_source_dir" rev-parse HEAD
cmake -S "$snodec_source_dir" -B "$snodec_build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_INSTALL_PREFIX="$snodec_install_dir" \
  -DSNODEC_BUILD_TESTS=OFF \
  -DSNODEC_BUILD_APPS=OFF
cmake --build "$snodec_build_dir" --target all --parallel 26
cmake --install "$snodec_build_dir"

cmake -E make_directory build-p0/.cmake/api/v1/query
cmake -E touch \
  build-p0/.cmake/api/v1/query/codemodel-v2 \
  build-p0/.cmake/api/v1/query/cache-v2 \
  build-p0/.cmake/api/v1/query/toolchains-v1
cmake -E env "LDFLAGS=$baseline_linker_flags" \
  cmake -S . -B build-p0 -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_INSTALL_PREFIX="$aisuite_install_dir" \
  -DCMAKE_PREFIX_PATH="$snodec_install_dir" \
  -Dsnodec_DIR="$snodec_install_dir/lib/cmake/snodec" \
  -DCMAKE_JOB_POOLS='aisuite_compile=4;aisuite_link=2' \
  -DCMAKE_JOB_POOL_COMPILE=aisuite_compile \
  -DCMAKE_JOB_POOL_LINK=aisuite_link \
  -DAISUITE_BUILD_APPS=ON \
  -DAISUITE_BUILD_TESTS=ON \
  -DAISUITE_BUILD_CODEX_FRONTEND_CLIENT=ON \
  -DAISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET=ON \
  -DAISUITE_ENABLE_CODEX_FRONTEND_TLS=ON \
  -DAISUITE_ENABLE_CODEX_FRONTEND_RFCOMM=ON
cmake --build build-p0 --target all --parallel 26
cmake --install build-p0
cmake -E env "LD_LIBRARY_PATH=$baseline_runtime_libraries" \
  ctest --test-dir build-p0 --output-on-failure --parallel 2 \
  --output-junit "$PWD/build-p0/ctest-results.xml"
# Install-backed tests may replace CMake's install manifest; restore the P0
# staging prefix before the final capture.
cmake --install build-p0
cmake -E env "LD_LIBRARY_PATH=$baseline_runtime_libraries" \
  python3 -B tools/codex/capture_architecture_baseline.py capture \
  --source-dir . \
  --build-dir build-p0 \
  --install-dir "$aisuite_install_dir" \
  --snodec-source-dir "$snodec_source_dir" \
  --snodec-build-dir "$snodec_build_dir" \
  --ctest-results build-p0/ctest-results.xml \
  --ctest-parallelism 2 \
  --baseline-parent 4c0cfbf99667fef64c9fed010d84031248ceaba2 \
  --output build-p0/current-baseline.json
```

Source-only shape, policy, and generated-authority validation is explicitly
separate. It does not claim fresh build/install/executable verification:

```sh
python3 -B tools/codex/capture_architecture_baseline.py self-test
python3 -B tools/codex/capture_architecture_baseline.py validate-baseline \
  --source-dir . \
  --baseline docs/ai/openai/codex/architecture-reduction/p0-baseline.json
```

Full blocking-contract verification requires the configured build, temporary
install, installed executables, and exact SNode.C dependency evidence:

```sh
cmake -E env "LD_LIBRARY_PATH=$baseline_runtime_libraries" \
  python3 -B tools/codex/capture_architecture_baseline.py verify-contract \
  --source-dir . \
  --build-dir build-p0 \
  --install-dir "$aisuite_install_dir" \
  --snodec-source-dir "$snodec_source_dir" \
  --snodec-build-dir "$snodec_build_dir" \
  --baseline docs/ai/openai/codex/architecture-reduction/p0-baseline.json
```

`capture` independently constructs the complete normalized document twice and
fails unless both deterministic results are exactly equal before it writes the
requested output. Local validation and CI additionally run two complete
captures and require byte-for-byte equality. The source-oriented policy test
checks deterministic policy and mutation semantics without claiming to rerun
an installed capture; timestamp and host-resource observations are not
deterministic machine fields.

P3 compares its final capture with P0 as follows. Architecture changes are
reported; only external-contract drift fails:

```sh
python3 -B tools/codex/capture_architecture_baseline.py compare \
  --baseline docs/ai/openai/codex/architecture-reduction/p0-baseline.json \
  --current build-p3/current-baseline.json \
  --output build-p3/p0-to-final-comparison.json
```

Use `--diagnostic-only` only when intentionally inspecting external drift
without making the comparison command fail.
