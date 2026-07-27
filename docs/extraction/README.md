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

Standalone project, package, CI, and extraction-validation files are introduced
as new AISuite commits after the preserved history.

## Temporary duplication

Until the later SNode.C cutover, both repositories contain a Codex
implementation. Install them into separate prefixes. AISuite libraries use
`aisuite-` output names and install headers below `include/aisuite`, preventing
file collisions with the unchanged SNode.C installation.


## Standalone dependency boundary

AISuite consumes SNode.C only through the installed `snodec` CMake package.
The extraction must not use a sibling checkout, private SNode.C headers, or
source-relative include paths. The installed-consumer test resolves both
packages from staging prefixes.

## Protocol state at extraction

The extraction intentionally preserves the post-PR-#223 state:

```text
Complete:       280
Partial:          4
NotImplemented:  55
NotApplicable:   48
```

No A1.4 implementation identity is promoted by the repository move. The
remaining implementation sequence is PR A, PR B, PR C, and final A1 closure
in AISuite.
