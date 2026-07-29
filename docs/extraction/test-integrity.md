# AISuite extraction test-integrity report

The extraction retains the complete selected Codex test history and adds only
the standalone project harness, package tests, installed-consumer wrapper,
extraction manifest guard, and CI workflow.

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

The A14-UserIntegrations successor adds an audit and closure guard for exactly
33 identities: 23 requests, six notifications, and four `PluginSource`
alternatives. Its planted-failure harness checks exact diagnostic codes rather
than accepting an arbitrary nonzero exit. It covers identity leakage, wrong
result associations, schema closure, staged promotion arithmetic, descriptors,
fixtures, public/install ownership, package boundaries, predecessor and final
variant indices, SOVERSION, predecessor evidence, and second-pass
nondeterminism.

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
The installed consumer builds pinned SNode.C and AISuite into separate fresh
prefixes with CMake package registries disabled and inherited compiler,
linker, loader, header, CMake, and pkg-config search variables removed. It
rejects the original checkouts plus outer and fresh source/build trees in
compile, link, ELF, and `ldd` evidence. A dedicated installed public-header
consumer links `snodec::core`, which proves the fresh
`include/snode.c` and SNode.C library origins independently of the AISuite
targets. The consumer also exercises all seven new facades and final public
variants. Local runs must configure
`AISUITE_TEST_SNODEC_SOURCE_REPOSITORY` with an absolute path to a clean clone
containing the pinned SNode.C commit.

The source package must contain no `.git` metadata and must fail
`git rev-parse`, with `GIT_CEILING_DIRECTORIES` preventing discovery of the
enclosing checkout. It retains all three full-corpus generation-proof JSON
files and runs only package-safe extraction, API/ABI, and PR-A closure checks.
The binary package must contain all seven facade headers and no private
evidence or implementation inputs.

Credential-bearing live App Server tests remain opt-in and are not mandatory
for offline extraction correctness. No test is deleted, unconditionally
skipped, relabeled to evade CI, or weakened from exact comparisons to substring
checks.
