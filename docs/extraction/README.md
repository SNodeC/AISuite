# SNode.C to AISuite extraction provenance

## Safety invariant

The extraction is additive. No source, build rule, test, package declaration,
or application is removed from SNode.C during this stage. SNode.C remains the
working fallback until a separate cutover is reviewed and approved.

## Source

- repository: `https://github.com/SNodeC/snode.c`
- commit: `d18b231a1d2ec2235fd6f204786b0a761cc24ff5`
- tree: `88a63edc985a851b2b76b0c56df19fae74ea8069`
- Codex pin: `codex-cli 0.144.6` / `rust-v0.144.6`

`filter-map.json` records every retained source commit, rewritten commit, tree,
parent mapping, and selected path.

## Selected history

The filtered history contains only:

- `src/ai/openai/codex/`
- `src/apps/codex-backend/`
- `src/apps/codex-backend-client/`
- `docs/ai/openai/codex/`
- `tools/codex/`
- `tests/component/codex/`
- `tests/installed/codex/`
- the two Codex package tests
- `tests/policy/security/CodexSyntheticSecretLeakGuardTest.py`

Standalone project, package, CI, extraction-validation, and transferred Codex
policy-owner files are introduced as new AISuite commits after the preserved
history. The policy owners are not added to the immutable selected paths.

## Codex policy ownership

Three Codex-specific responsibilities that lived outside the original selected
history are now owned and enforced by AISuite:

- all installed public Codex header inventory, guard, and self-containment
  policy;
- backend logging API surface policy;
- parameterless Codex semantic-logger classification policy.

The new CMake-owned `tests/policy/` hierarchy contains four new functional
tests for those three responsibilities. It also owns the unchanged registration
of the pre-existing
`tests/policy/security/CodexSyntheticSecretLeakGuardTest.py`, which is not a
fourth transferred responsibility. The security test remains an imported file;
the new policy owners, registration files, ownership verifier, mutations, and
documentation are standalone files.

The existing `tests/component/codex/` hierarchy remains registered exactly
once. Its 131-test baseline is preserved at 131 enabled tests with no command,
label, timeout, or dependency drift, and no other pre-existing CTest is
removed. See the
[Codex policy ownership report](codex-policy-ownership.md) for exact source
blobs, owners, functional tests, labels, accepted logger entries, package
boundaries, and cutover-readiness evidence.

## Temporary duplication

Until the later SNode.C cutover, both repositories contain a Codex
implementation. Install them into separate prefixes. AISuite libraries use
`aisuite-` output names and install headers below `include/aisuite`, preventing
file collisions with the unchanged SNode.C installation.

## Standalone dependency boundary

AISuite consumes SNode.C only through the installed `snodec` CMake package.
The extraction must not use a sibling checkout, private SNode.C headers, or
source-relative include paths. The installed-consumer test independently
builds and installs the pinned SNode.C tree and AISuite into disjoint prefixes,
disables CMake package registries, scrubs inherited build and package-search
environment variables, and rejects source/build/outer-stage header, library,
cache, RPATH, RUNPATH, and `ldd` resolution. Its direct `snodec::core`
public-header probe proves that the fresh SNode.C include and library prefixes
are used. Configure local test builds with
`AISUITE_TEST_SNODEC_SOURCE_REPOSITORY=/absolute/path/to/snode.c`, pointing at
a clean clone containing the pinned extraction commit.

## Protocol state at extraction

The extraction intentionally preserves the post-PR-#223 state:

```text
Complete:       280
Partial:          4
NotImplemented:  55
NotApplicable:   48
```

No A1.4 implementation identity is promoted by the repository move. The
successor A14-UserIntegrations milestone in AISuite later promotes exactly 33
A1.4 identities. That successor work does not alter this extraction baseline,
the SNode.C source commit/tree, the filtered-history map, the Codex pin, or any
original source hash.

## Current successor status

After A14-UserIntegrations, the live registry is:

```text
Complete:       313
Partial:          4
NotImplemented:  22
NotApplicable:   48
```

Native A1.4 is `33 / 1 / 22`; it remains in progress. PR B, PR C, inherited
A1.0 Partials, and InventoryOnly identities remain unchanged, and Codex
SOVERSION remains 1. AISuite Codex policy ownership is complete. SNode.C Codex
removal remains a separate reviewed cutover.
