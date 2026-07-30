# Safe AISuite extraction validation

## Extraction boundary

AISuite was extracted additively from SNode.C commit
`d18b231a1d2ec2235fd6f204786b0a761cc24ff5`, tree
`88a63edc985a851b2b76b0c56df19fae74ea8069`. That historical tree remains
immutable extraction provenance and is never built by AISuite. Normal
compilation and linking use the cleaned SNode.C commit
`77415c71a87fb7955e9a050bedaca02b65754324`, tree
`2d39c334f12c308828936656c820447bfcc38d47`; the reviewed SNode.C cutover has
therefore been performed without changing the historical authority.

The filtered history retains 67 relevant commits. The immutable filtered
baseline and the complete source-to-filtered commit map are recorded in
`filter-map.json`. `source-manifest.json` verifies every imported file and every
reviewed standalone adaptation.

## Preserved extraction and current protocol states

The repository move itself preserved this registry baseline:

```text
Complete:       280
Partial:          4
NotImplemented:  55
NotApplicable:   48
```

The later A14-UserIntegrations milestone promotes exactly 33 identities, giving
the live registry `313 / 4 / 22 / 48` and native A1.4 `33 / 1 / 22`. The
Partial identities remain `initialize`, `initialized`, `error`, and
`item/tool/requestUserInput`; native A1.4 remains in progress. The Codex pin
remains `codex-cli 0.144.6` / `rust-v0.144.6`, and the extracted Codex
libraries retain SOVERSION 1.

## Standalone build boundary

AISuite discovers SNode.C through its installed CMake package. It contains no
source-relative SNode.C include, sibling `add_subdirectory`, private-header
dependency, or fixed SNode.C build-tree path. CI builds the cleaned checkout
once into one installed prefix and configures AISuite only against that
prefix. The installed-consumer gate reuses the same configured SNode.C package
and the one AISuite build, installs AISuite into an isolated prefix, disables
CMake package registries, scrubs inherited compiler/linker/loader/package
search variables, verifies `AISuite_DIR` and `snodec_DIR`, and rejects the
AISuite checkout/build plus the historical SNode.C worktree in compile, link,
ELF, and `ldd` evidence. A direct public-header consumer linked to
`snodec::core` requires the cleaned prefix's `include/snode.c` and SNode.C
library.

Local test-enabled configuration names only the detached historical worktree
used by extraction guards:

```sh
cmake -S . -B build \
  -DAISUITE_BUILD_TESTS=ON \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/cleaned/snode.c/install \
  -DAISUITE_TEST_SNODEC_SOURCE_REPOSITORY=/absolute/path/to/historical/worktree
```

`AISUITE_TEST_INSTALLED_CONSUMER_TEMP_ROOT` may point at a spacious temporary
filesystem for the disjoint source/build/install trees.

The standalone distribution exports:

```text
AISuite::OpenAICodex
AISuite::OpenAICodexBackend
AISuite::OpenAICodexFrontend
```

The applications remain `codex-backend` and `codex-backend-client`. No separate
native `codex-ui` source existed in the extraction baseline, so none was
fabricated or renamed during extraction.

## Codex policy closure

The CMake-owned `tests/policy/` hierarchy implements the three transferred
Codex source-policy responsibilities with four new functional tests:
`CodexPublicHeaderPolicyTest`,
`CodexPublicHeaderSelfContainmentTest`,
`CodexLoggingApiSurfacePolicyTest`, and
`CodexSemanticLoggerPolicyTest`. The unchanged, pre-existing
`CodexSyntheticSecretLeakGuardTest` is the fifth functional policy test; only
its CTest registration owner moves from the root test file to
`tests/policy/security/CMakeLists.txt`.

The configured CTest evidence, rather than CMake-source grep alone, proves that
all five tests are enabled, registered exactly once, have finite timeouts, and
match the `ai|openai|codex|extraction` focused filter used by both CI jobs. It
also proves that the existing `add_subdirectory(component/codex)` is preserved
alongside exactly one new `add_subdirectory(policy)`, all 131 pre-existing
Codex component tests remain unchanged, and all 140 pre-existing tests remain
present.

The public-header checks derive the exact `27 / 7 / 7` installed inventory
mechanically and compile all 41 headers independently from an installed prefix.
The logging API check tokenizes the two canonical backend state/event headers.
The semantic-logger check pins and classifies exactly four accepted
`lifecycleLog` calls in `Reducer.cpp`. The machine-readable ownership evidence
binds those checks to the pinned SNode.C source blobs, manifest classifications,
mutation diagnostics, focused CI, and both package boundaries. See
[`codex-policy-ownership.md`](codex-policy-ownership.md).

## Deterministic checks

The final tree is required to pass:

- extraction manifest generation and verification;
- Codex policy ownership generation and verification against canonical
  baseline and final CTest models;
- all four new functional Codex policy tests and the reorganized pre-existing
  security policy test;
- isolated planted failures for header, logging, semantic, registration,
  component-preservation, CI-filter, authority, manifest, and package
  invariants, each asserting its exact diagnostic code;
- A1.1, A1.2, and A1.3 audits and closure checks;
- the A1.4 partition and implementation-plan audit with all mutation guards;
- the A14-UserIntegrations audit and closure guards, including exact
  `23 / 6 / 0 / 4`, `20 / 3`, and `52 / 118 / 411` counts;
- bounded A14-UserIntegrations policy-history validation for the exact Commit-1
  construction state, unmerged two-commit branch, normal PR #3 merge, and
  later descendants, plus malformed-topology and in-range production-change
  mutations with exact diagnostics;
- exact predecessor `51 / 53` and final `57 / 59` notification variant
  sizes and index mappings;
- schema, generated-artifact, fixture, and operation-contract guards;
- dependency-ordered, two-pass byte identity across the complete inherited and
  PR-A target corpus, with the extraction manifest as the unique final
  generator in each pass;
- exact path-only extraction exceptions for the three proof metadata files,
  canonical proof-file validation, and live closure-report/extraction-manifest
  equality with both recorded passes;
- exact frontend-protocol byte checks;
- synthetic-secret source and package scans;
- genuine installed-consumer validation against the one cleaned SNode.C
  install and an isolated AISuite install;
- source-package package-safe extraction/ABI/closure and Codex-policy ownership
  checks, with no `.git` metadata, network, SNode.C checkout, or
  enclosing-checkout discovery, and binary-package installed-header plus
  policy-internal exclusion validation;
- exact preservation of the synthetic-secret guard's reviewed
  `WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"` expression, normalized
  `${BUILD_DIR}/tests` configured evidence, and ownership rationale in normal
  and package-safe checks;
- the complete extracted Codex component test suite in GitHub Actions.

Local validation uses focused, deterministic checks. The normal exact-head
GitHub Actions run performs the complete build and registered test suite
against the one cleaned SNode.C build/install; its historical worktree is
read-only provenance only.

The ownership record is generated before the extraction manifest, and the
complete applicable sequence is run twice. The second pass must be
byte-identical. Policy ownership completion changes no protocol authority,
production ABI, registry row, fixture, variant index, Codex pin, or SOVERSION.
