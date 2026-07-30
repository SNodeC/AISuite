# AISuite extraction test-integrity report

The extraction retains the complete selected Codex test history. The
post-extraction repository adds the standalone project harness, package tests,
installed-consumer wrapper, extraction manifest guard, CI workflow, and the
focused Codex policy owners needed to close the later SNode.C cutover boundary.

No Codex production `.cpp` or public/private `.h` file is changed by the
repository move. Build files are adapted to the AISuite target and installation
names. Historical audit and closure tools retain their protocol, fixture,
registry, lifecycle, and security invariants; only repository-boundary
fingerprints, installed paths, package names, and extraction-specific evidence
hashes are updated.

The following predecessor checks remain registered:

- A1.1 audit, closure, production-coverage, and byte-identity guards;
- A1.2 audit, documentation, and closure mutation guards;
- A1.3 audit and closure mutation guards;
- A1.4 partition, residue, implementation-plan, and SOVERSION-plan guards;
- exact schema, operation-contract, fixture, frontend-protocol, and generated
  descriptor checks;
- all compiled wire, codec, lifecycle, backend, and frontend component tests.

The root retains exactly one `add_subdirectory(component/codex)` and adds
exactly one separate `add_subdirectory(policy)`. Canonical configured CTest
models establish a 131-test Codex component baseline and final inventory; every
component test remains enabled and preserves its command, labels, timeout, and
dependencies. `CodexAppServerGeneratedArtifactsGuardTest` remains registered
for the synthetic-secret dependency chain. Every one of the 140 pre-existing
CTest names remains present.

The policy hierarchy owns exactly five functional Codex tests: four new tests
for three transferred SNode.C responsibilities, plus the pre-existing
`CodexSyntheticSecretLeakGuardTest`. The Python security file is byte-identical
and remains in the extraction manifest's imported bucket. Its command,
arguments, labels, environment, ordered dependency list, serial property, and
timeout remain exact; only its registration moves from `tests/CMakeLists.txt`
to `tests/policy/security/CMakeLists.txt`. The new registration explicitly
retains `WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"`, normalized in the
configured models as `${BUILD_DIR}/tests`, so the hierarchy move preserves the
test's former implicit execution directory. The machine-readable ownership
authority records both expressions and the reviewed preservation rationale.

The four new tests enforce all 41 installed public headers (`27 / 7 / 7`),
standalone installed-prefix compilation, the four forbidden logging-lifecycle
identifiers, and exactly four reviewed Codex semantic-logger classifications.
All five functional policy tests match the focused CI expression in both
compiler jobs. Their exact ownership and source provenance are recorded in
[`codex-policy-ownership.md`](codex-policy-ownership.md).

The A14-UserIntegrations successor adds an audit and closure guard for exactly
33 identities: 23 requests, six notifications, and four `PluginSource`
alternatives. Its planted-failure harness checks exact diagnostic codes rather
than accepting an arbitrary nonzero exit. It covers identity leakage, wrong
result associations, schema closure, staged promotion arithmetic, descriptors,
fixtures, public/install ownership, package boundaries, predecessor and final
variant indices, SOVERSION, predecessor evidence, and second-pass
nondeterminism.

The A1.4 closure history check now bounds the policy proof at the structurally
validated Commit 2. Deterministic topology tests cover the exact Commit-1
construction state, the exact unmerged two-commit branch, a normal PR #3 merge,
and later descendants, including descendants that change unrelated production
files or the protocol registry. Those later changes are not attributed to PR
#3. Separate invalid-topology cases reject wrong subjects, parents, parent
order or count, merge trees, duplicate candidates, inserted policy commits,
and production or registry changes inside the bounded policy range with their
exact diagnostics.

Every inherited generator is rerun in dependency order after the production
and test changes. Changed predecessor evidence is classified as derived
hash/evidence ratcheting only; semantic identities, schema paths, contracts,
ownership, protocol pins, and predecessor variant order remain frozen. The
same complete generator sequence is then run a second time and every generated
path, byte count, and SHA-256 must match the first pass. The extraction
manifest is the unique last generator in each pass.

The proof is deliberately acyclic. Its target corpus includes the PR-A closure
report and extraction manifest. Exactly three metadata documents
(`generation-pre`, `generation-pass-1`, and `generation-pass-2`) sit outside
that target hash domain because they contain its full path/size/SHA-256
snapshot. The extraction manifest has one hard-coded, path-only exception for
those exact three regular files, hashes none of their bytes, and permits no
fourth exception. The specialized closure guard parses and canonically hashes
all three documents, verifies both recorded passes, and requires a fresh live
snapshot—including the closure report and extraction manifest—to equal each
pass byte-for-byte. Isolated mutations cover both live artifacts and missing,
extra, or wrong extraction exceptions with exact diagnostic codes.

The synthetic-secret guard is extracted with its history and recognizes the
AISuite source-package staging layout while preserving the exact fixture
allowlist. Package tests run serially, and the security scan depends on the
installed-consumer and package tests so generated package trees are included.
The installed consumer reuses the one configured cleaned SNode.C install and
the one AISuite build, then installs AISuite into a separate fresh prefix.
CMake package registries are disabled and inherited compiler, linker, loader,
header, CMake, and pkg-config search variables are removed. Compile, link,
ELF, and `ldd` evidence rejects the AISuite source/build trees and the
historical SNode.C provenance worktree. A dedicated installed public-header
consumer links `snodec::core`, which proves the cleaned
`include/snode.c` and SNode.C library origins independently of the AISuite
targets. The consumer also exercises all seven new facades and final public
variants. Local runs configure `CMAKE_PREFIX_PATH` with the cleaned installed
dependency and `AISUITE_TEST_SNODEC_SOURCE_REPOSITORY` with the detached
historical worktree at the immutable extraction commit.

The source package must contain no `.git` metadata and must fail
`git rev-parse`, with `GIT_CEILING_DIRECTORIES` preventing discovery of the
enclosing checkout. It retains all three full-corpus generation-proof JSON
files and runs package-safe extraction, API/ABI, PR-A closure, and Codex-policy
ownership checks. The policy check uses recorded pinned SNode.C authority and
requires no network, external checkout, package registry, build directory, or
parent source tree. The binary package must contain all seven facade headers
and no private evidence, implementation inputs, policy tests or support,
mutation fixtures, ownership checker, canonical test models, or ownership
JSON.

The policy mutation harness copies valid authorities into isolated temporary
trees/models, changes exactly one invariant, asserts the exact named diagnostic,
discards the mutation, and proves the unmodified authority still passes.
Coverage includes public-header guards and inventory, logging API tokens,
semantic classifications, test registration and labels, both CI filters,
security registration ownership and properties (including the configured
working directory and a misleading CMake property-value decoy), all three
reviewed working-directory evidence fields, component and global CTest
preservation, source blobs, manifest buckets, and source/binary package
boundaries.

Credential-bearing live App Server tests remain opt-in and are not mandatory
for offline extraction correctness. No test is deleted, unconditionally
skipped, relabeled to evade CI, or weakened from exact comparisons to substring
checks.

AISuite Codex policy ownership is complete. The reviewed SNode.C cutover is
performed: normal builds use cleaned commit
`77415c71a87fb7955e9a050bedaca02b65754324`, while historical commit
`d18b231a1d2ec2235fd6f204786b0a761cc24ff5` remains provenance-only. No Codex
production or protocol behavior changes in this dependency-adoption step.
