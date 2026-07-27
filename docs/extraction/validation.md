# Safe AISuite extraction validation

## Extraction boundary

AISuite was extracted additively from SNode.C commit
`d18b231a1d2ec2235fd6f204786b0a761cc24ff5`, tree
`88a63edc985a851b2b76b0c56df19fae74ea8069`. The source repository was not
modified and remains the working fallback. A later cutover requires a separate
SNode.C pull request.

The filtered history retains 67 relevant commits. The immutable filtered
baseline and the complete source-to-filtered commit map are recorded in
`filter-map.json`. `source-manifest.json` verifies every imported file and every
reviewed standalone adaptation.

## Preserved protocol state

The extraction does not implement A1.4. The registry remains:

```text
Complete:       280
Partial:          4
NotImplemented:  55
NotApplicable:   48
```

The Partial identities remain `initialize`, `initialized`, `error`, and
`item/tool/requestUserInput`. The Codex pin remains `codex-cli 0.144.6` /
`rust-v0.144.6`, and the extracted Codex libraries retain SOVERSION 1.

## Standalone build boundary

AISuite discovers SNode.C through its installed CMake package. It contains no
source-relative SNode.C include, sibling `add_subdirectory`, private-header
dependency, or fixed SNode.C build-tree path. The installed consumer resolves
both `snodec` and `AISuite` from staging prefixes.

The standalone distribution exports:

```text
AISuite::OpenAICodex
AISuite::OpenAICodexBackend
AISuite::OpenAICodexFrontend
```

The applications remain `codex-backend` and `codex-backend-client`. No separate
native `codex-ui` source existed in the extraction baseline, so none was
fabricated or renamed during extraction.

## Deterministic checks

The final tree is required to pass:

- extraction manifest generation and verification;
- A1.1, A1.2, and A1.3 audits and closure checks;
- the A1.4 partition and implementation-plan audit with all mutation guards;
- schema, generated-artifact, fixture, and operation-contract guards;
- exact frontend-protocol byte checks;
- synthetic-secret source and package scans;
- installed-consumer validation;
- source- and binary-package validation;
- the complete extracted Codex component test suite in GitHub Actions.

Local validation uses focused, deterministic checks. The normal exact-head
GitHub Actions run performs the complete build and registered test suite against
an independently built and installed copy of the pinned SNode.C source.
