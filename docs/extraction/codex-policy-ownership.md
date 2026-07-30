# AISuite Codex policy ownership

AISuite Codex policy ownership is complete and the reviewed SNode.C cutover is
performed. Normal AISuite compilation and linking use cleaned SNode.C commit
`77415c71a87fb7955e9a050bedaca02b65754324`, tree
`2d39c334f12c308828936656c820447bfcc38d47`.

This change transfers source-policy ownership only. It does not change Codex
protocol identities, production code, backend or frontend behavior, the
`codex-backend` applications, the Codex pin, or SOVERSION 1. No file below
`src/` is part of the transfer, and the pinned SNode.C source remains
unchanged.

## Source authority and extraction boundary

The AISuite base for this transfer is commit
`19de4f50be64e187761274f043091090609d27a3`, tree
`c71a91e545d70d649446ca9d698d729c307a480a`. The normal installed dependency is
`https://github.com/SNodeC/snode.c` at cleaned commit
`77415c71a87fb7955e9a050bedaca02b65754324`, tree
`2d39c334f12c308828936656c820447bfcc38d47`. The separate read-only extraction
and policy authority is the historical commit
`d18b231a1d2ec2235fd6f204786b0a761cc24ff5`, tree
`88a63edc985a851b2b76b0c56df19fae74ea8069`.

| Source-policy authority | Git blob | Transferred responsibility |
| --- | --- | --- |
| `tests/policy/codex/CodexA12PublicHeaderPolicyTest.cpp` | `73537e7d68b74afb335db8c4bd8b42d533c86814` | `codex-public-header-policy` |
| `tests/policy/log/LoggingApiSurfacePolicyTest.cpp` | `e9beada86e032261d5b128cf58d0efdf1f927234` | `codex-logging-api-surface-policy` |
| `tests/policy/log/ParameterlessSemanticLoggerPolicyTest.cpp` | `e4fddcaf69b23549eab318cb86afee6210b2aaad` | `codex-semantic-logger-policy` |

The small generic source-root helper was adapted, where needed, from
`tests/policy/SourcePolicyTestRoot.h`, blob
`47ec895a1bb6d6344d3c9d6bbdbc345c72a09380`. Adapted files retain their SPDX,
copyright, source path, and source blob in the machine-readable ownership
record.

The original filtered-history selection included
`tests/policy/security/CodexSyntheticSecretLeakGuardTest.py`, but it did not
include SNode.C's Codex public-header policy or the repository-wide logging
policy files. Consequently, the transferred owners and their focused support
are post-extraction `standalone_files`; they are not retroactively represented
as immutable filtered-history copies or selected-history adaptations.
`filter-map.json` is unchanged.

Exactly three Codex-specific responsibilities transfer:

1. installed public Codex header inventory, guard, and self-containment policy;
2. backend logging API surface policy;
3. parameterless Codex semantic-logger classification policy.

MQTT, HTTP, WebSocket, MariaDB, event-loop, SNode.C CI, non-Codex security, and
other repository-wide SNode.C policy remain out of scope.

## Functional policy hierarchy

The CMake-owned hierarchy under `tests/policy/` owns exactly five functional
Codex policy tests:

| Test | Kind | Labels | Timeout |
| --- | --- | --- | --- |
| `CodexPublicHeaderPolicyTest` | new transferred owner | `policy;ai;openai;codex;extraction;headers;public-api` | 30 seconds |
| `CodexPublicHeaderSelfContainmentTest` | new transferred owner | `policy;ai;openai;codex;extraction;headers;install;consumer` | 300 seconds |
| `CodexLoggingApiSurfacePolicyTest` | new transferred owner | `policy;ai;openai;codex;extraction;logging;api;architecture` | 30 seconds |
| `CodexSemanticLoggerPolicyTest` | new transferred owner | `policy;ai;openai;codex;extraction;logging;architecture` | 30 seconds |
| `CodexSyntheticSecretLeakGuardTest` | pre-existing AISuite security policy | `policy;security;codex;extraction;package` | 120 seconds |

The first four tests implement the three transferred responsibilities. The
fifth test is not a fourth transferred responsibility.

`tests/policy/security/CodexSyntheticSecretLeakGuardTest.py` remains at its
original path and remains byte-identical (Git blob
`5ae5cc4f272eef186422188e7b4ebce1507a8149`). Its extraction-manifest
`imported_files` provenance remains source blob
`b5f5f6d3ef24b9c4f91ccec226ed6a437de31b79`, with the reviewed
`standalone-adaptation` disposition. Only its CTest registration moves from
`tests/CMakeLists.txt` to `tests/policy/security/CMakeLists.txt`.

The configured CTest model proves that the security test is registered exactly
once and preserves:

- the Python interpreter, script, `--repo-root ${PROJECT_SOURCE_DIR}`, and
  `--build-root ${CMAKE_BINARY_DIR}` command;
- `PYTHONDONTWRITEBYTECODE=1`;
- dependencies, in order: `AISuiteInstalledConsumerTest`,
  `AISuiteSourcePackageTest`, `AISuiteBinaryPackageTest`,
  `CodexAppServerGeneratedArtifactsGuardTest`, and `CodexA14AuditToolTest`;
- `RUN_SERIAL TRUE`, its exact labels, and its 120-second timeout.

Its registration also explicitly sets
`WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"`, recorded in configured CTest
evidence as `${BUILD_DIR}/tests`. This preserves the implicit working directory
the test had when it was registered from `tests/CMakeLists.txt`; moving the
registration into `tests/policy/security/CMakeLists.txt` without the explicit
property would silently change the execution directory. The reviewed ownership
record carries both expressions and this exact rationale:

> Explicit property added to preserve the implicit cwd the test had when
> registered from tests/CMakeLists.txt; the directory move to
> tests/policy/security would otherwise silently change it.

The root test configuration retains exactly one
`add_subdirectory(component/codex)` and adds exactly one
`add_subdirectory(policy)` after the security guard's dependencies are
registered. The baseline and final configured models each contain 131 tests
owned by `tests/component/codex/` or a nested CMake file there. Every one
remains present exactly once, enabled, and unchanged in command, labels,
timeout, and dependencies. In particular,
`CodexAppServerGeneratedArtifactsGuardTest` remains registered and satisfies
the security guard's dependency. All 140 pre-existing configured tests remain
present; no other pre-existing test disappears.

Both GitHub Actions jobs, `gcc-debug` and `gcc-15-debug`, execute CTest with
`-L 'ai|openai|codex|extraction'`. The configured model proves that each of the
five functional tests has at least one matching label, is enabled, and has a
finite nonzero timeout.

## Bounded policy history

The A1.4 predecessor closure validates the policy-ownership PR as a bounded
reviewed range rather than treating every future descendant as part of this
change. It recognizes exactly three construction and promotion states:

1. the exact Commit-1 construction state at
   `ab0c734143eddcd1d5b20d29ac7f61baa25711bf`;
2. the unmerged two-commit policy branch, with Commit 2 derived structurally as
   the direct child of Commit 1; and
3. a unique normal GitHub merge with subject
   `Merge pull request #3 from SNodeC/extraction/complete-codex-policy-ownership`,
   first parent equal to the policy base, second parent equal to the validated
   Commit 2, and a tree byte-identical to Commit 2.

After the third state is validated, the guard accepts the merge itself or any
later descendant. Production and registry immutability are checked only across
the bounded policy range from
`19de4f50be64e187761274f043091090609d27a3` through the structurally validated
Commit 2. Later reviewed PRs may therefore change production authorities
without being retroactively classified as part of PR #3. Wrong or ambiguous
merge topology reports `UserIntegrationPromotionStageMismatch`; an actual
production change inside either policy commit reports
`UserIntegrationFalseComplete`. Neither the unknown future merge SHA nor the
amended Commit-2 SHA is embedded in generated evidence.

## Public headers

The public-header owner mechanically derives its inventory from
`AI_OPENAI_CODEX_PUBLIC_H`, `AI_OPENAI_CODEX_BACKEND_PUBLIC_H`, and
`AI_OPENAI_CODEX_FRONTEND_PUBLIC_H`, cross-checks every `install(FILES ...)`
authority, and compares the staged installation and binary-package inventory.
The current exact inventory is 28 main, 7 backend, and 7 frontend headers: 42
total.

Every installed public header must use one conventional, correctly ordered,
matching `#ifndef` / `#define` / `#endif` guard, without `#pragma once` or a
competing guard. The original A1.2 identities remain exact:

| Header | Required guard |
| --- | --- |
| `typed/Accounts.h` | `AI_OPENAI_CODEX_TYPED_ACCOUNTS_H` |
| `typed/Models.h` | `AI_OPENAI_CODEX_TYPED_MODELS_H` |
| `typed/Configuration.h` | `AI_OPENAI_CODEX_TYPED_CONFIGURATION_H` |

This deliberately strengthens the former three-header check to all 42
installed headers. The self-containment test installs AISuite into an isolated
prefix and compiles one translation unit per header against installed AISuite
and SNode.C packages, with CMake package registries disabled. Compile-command
evidence rejects source-tree, build-tree, private-header, and prior-include
order dependencies.

## Logging API surface

`CodexLoggingApiSurfacePolicyTest` tokenizes
`src/ai/openai/codex/backend/BackendEvent.h` and
`src/ai/openai/codex/backend/BackendState.h`. It forbids these exact
logging-lifecycle bookkeeping identifiers from canonical backend event and
state types:

```text
lifecycleStart
creationLogged
lifecycleStarted
lifecycleTerminalLogged
```

Comments, string literals, formatting changes, and partial identifiers cannot
satisfy or defeat the token-aware check.

## Semantic logger classifications

`CodexSemanticLoggerPolicyTest` recursively scans only the production C++ source
and header extensions below `src/ai/openai/codex/`. Its accepted inventory is
mechanically pinned to exactly four SNode.C entries. All four use
`lifecycleLog`, have classification `DOMAIN_OR_PROTOCOL_SCOPE`, and occur in
`src/ai/openai/codex/backend/Reducer.cpp`.

| Source path | Logger | Identifying expression | Classification | Rationale |
| --- | --- | --- | --- | --- |
| `src/ai/openai/codex/backend/Reducer.cpp` | `lifecycleLog` | `"turn {}: thread={} turn={}"` | `DOMAIN_OR_PROTOCOL_SCOPE` | Typed turn completion is owned by Codex thread and turn identifiers. |
| `src/ai/openai/codex/backend/Reducer.cpp` | `lifecycleLog` | `"turn failed: thread={} turn={}"` | `DOMAIN_OR_PROTOCOL_SCOPE` | Typed turn failure is owned by Codex thread and turn identifiers. |
| `src/ai/openai/codex/backend/Reducer.cpp` | `lifecycleLog` | `"thread created: thread={}"` | `DOMAIN_OR_PROTOCOL_SCOPE` | Typed thread creation is owned by the Codex thread identifier. |
| `src/ai/openai/codex/backend/Reducer.cpp` | `lifecycleLog` | `"turn started: thread={} turn={}"` | `DOMAIN_OR_PROTOCOL_SCOPE` | Typed turn start is owned by Codex thread and turn identifiers. |

The first expression is intentionally not paraphrased as “turn completed”.
Every discovered parameterless semantic logger call must be classified;
unclassified calls, stale classifications, paths outside the Codex production
tree, changed authority fields, and mismatched accepted-entry counts fail. The
classification boundary explicitly distinguishes domain/protocol scope from
transport-connection scope.

## Ownership, manifests, and packages

`codex-policy-ownership.json` is the canonical generated ownership record.
`verify_codex_policy_ownership.py` provides `generate`, `check`, and
`check-package` modes. It binds each pinned responsibility and blob to exactly
one AISuite owner, functional CTest, focused-CI inclusion, planted-failure
coverage, manifest bucket, source-package presence, and binary-package
exclusion. The canonical baseline and final CTest models prove component-suite
and registration preservation. Cutover readiness is true only when that full
bijection passes.

New CMake, policy, support, checker, mutation, ownership, and documentation
files are `source-manifest.json` `standalone_files`. In particular, the new
`tests/policy/security/CMakeLists.txt` registration owner is standalone, while
the existing synthetic-secret Python guard retains its `imported_files` record.
No new path is added to `filter-map.json`, and no new owner is misrepresented
as an immutable copy or selected-history adaptation.

Source packages contain all owners, support, CMake registration, canonical
CTest models, ownership evidence, documentation, and mutation harnesses. The
extracted archive runs `verify_codex_policy_ownership.py check-package` without
`.git`, network access, an external SNode.C checkout, package registries, or a
parent source tree. Package-safe mode validates the recorded authority rather
than pretending to refetch it.

Binary packages continue to contain only installed production Codex headers,
libraries, applications, and CMake metadata. They reject policy test
executables, `tests/policy/`, policy support and mutations, the ownership
checker, canonical test models, ownership JSON, and internal extraction
documentation.

## Planted-failure coverage

The isolated mutation harness asserts the intended diagnostic, not merely a
nonzero result. It covers:

- public-header inventory removal, `#pragma once`, and broken guard pairs;
- forbidden backend lifecycle members;
- unclassified semantic calls, missing classifications, wrong authority
  counts, and changed identifying expressions;
- missing or multiple owners and altered SNode.C source blobs;
- missing, disabled, relabeled, or duplicate functional CTest registrations;
- focused-filter drift in either CI job;
- missing, misplaced, duplicated, relabeled, or property-drifted security
  registration, including configured working-directory drift and misleading
  CMake property-value decoys;
- missing or changed reviewed CMake/normalized working-directory evidence and
  missing or changed preservation rationale;
- removal of the component hierarchy, a component test, generated-artifact
  guard, or another pre-existing test, plus component property drift;
- standalone/imported manifest-bucket inversions;
- missing source-package owners and leaked binary-package internals.

The exact diagnostics include
`CodexPolicyPublicHeaderInventoryMismatch`,
`CodexPolicyHeaderGuardMismatch`,
`CodexPolicyPublicHeaderSelfContainmentMismatch`,
`CodexPolicyLoggingApiSurfaceMismatch`,
`CodexPolicySemanticLoggerUnclassified`,
`CodexPolicySemanticLoggerClassificationMismatch`,
`CodexPolicySemanticLoggerAuthorityMismatch`,
`CodexPolicyOwnershipMappingMismatch`,
`CodexPolicyOwnershipMultiplicityMismatch`,
`CodexPolicyCutoverReadinessMismatch`,
`CodexPolicySourceAuthorityMismatch`,
`CodexPolicyTestNotRegistered`, `CodexPolicyTestDisabled`,
`CodexPolicyTestExcludedFromFocusedCI`, `CodexPolicyCIFilterMismatch`,
`CodexPolicyDuplicateTestRegistration`,
`CodexPolicyExistingSecurityGuardNotRegistered`,
`CodexPolicyHierarchyRegistrationMismatch`,
`CodexPolicyExistingSecurityGuardDrift`,
`CodexPolicyPreexistingComponentTestMissing`,
`CodexPolicyPreexistingComponentTestDrift`,
`CodexPolicyPreexistingCTestRemoval`,
`CodexPolicyManifestClassificationMismatch`,
`CodexPolicySourcePackageMismatch`, and `CodexPolicyBinaryPackageLeak`.

The ownership evidence and extraction closure are regenerated twice in the
required dependency order. Pass two must be byte-identical to pass one; any
drift reports `CodexPolicySecondPassNondeterminism`.

The protocol registry remains `313 / 4 / 22 / 48`, with Partial identities
`initialize`, `initialized`, `error`, and
`item/tool/requestUserInput`. PR B and PR C remain untouched. SNode.C Codex
removal was completed in SNode.C commit
`77415c71a87fb7955e9a050bedaca02b65754324`; AISuite now adopts that cleaned
dependency while preserving the historical source authority separately.
