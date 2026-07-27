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

The synthetic-secret guard is extracted with its history and recognizes the
AISuite source-package staging layout while preserving the exact fixture
allowlist. Package tests run serially, and the security scan depends on the
installed-consumer and package tests so generated package trees are included.

Credential-bearing live App Server tests remain opt-in and are not mandatory
for offline extraction correctness. No test is deleted, unconditionally
skipped, relabeled to evade CI, or weakened from exact comparisons to substring
checks.
