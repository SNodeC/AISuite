# A1.7a Codex frontend contract

A1.7a completes and freezes the additive Frontend Protocol v1 and its owner-approved security contract. It does not activate the new surface at runtime; authentication, authorization enforcement, `FrontendService`, listeners, transports, and per-connection capability projection remain A1.7b work.

## Verified prerequisite

The work started from `origin/master` at `c2910324533daf7c4779d537952f27d7c2ae4490`. Before production changes, all targets built and the complete ordinary suite registered 147 tests: 146 passed, one credential-dependent integration test was skipped, and none failed. Both installed consumers and the source and binary package tests passed.

The inherited invariants were mechanically rechecked: typed Codex protocol 339/0/0/48, 86/86 BackendCore operations, 169/169 applicable canonical-state entries, 182/16/0 backend/state disposition, 65-alternative `ProviderOperationValue`, 68-alternative `CommandValue`, and 101-alternative `BackendCommand`.

## Fixed review denominator

The registry generator independently derives two disjoint sets:

- 148 previously unresolved decisions: 86 stable application operations, ten stable server requests, 35 experimental client requests, one experimental server request, and 16 stable `ResponseItem` alternatives;
- 86 compatibility contracts: 14 normalized notifications, 54 bounded redacted extension notifications, eight normalized `ThreadItem` contracts, and ten metadata-only `ThreadItem` contracts.

Their fixed union is 234 identities. All 234 now carry final exposure and security dispositions, and none retains an unresolved production decision. Exactly 36 experimental requests are denied frontend exposure. Exactly 16 `ResponseItem` alternatives remain genuinely `NotApplicable` because A1.6 established that they have no runtime backend-state path.

The generator rejects a missing reviewed identity, a changed bucket count, an extra `NotApplicable` identity, an unresolved final decision, or a denominator reduced below 148 + 86. The generated coverage report publishes the numerator and both fixed denominator components separately.

## Security boundary

Stable provider operations receive schema-defined frontend methods. Filesystem operations and arbitrary command execution are conditional and default-disabled for remote use. `account.read` explicitly distinguishes its observer read from `refreshToken=true`, which requires control, the account-management scope, and current controller ownership. Scope possession and controller ownership remain independent.

Definition in Frontend Protocol v1 does not mean a method is implemented by the current server, enabled by a deployment, or permitted for a principal. A1.7a defines the complete contract while the production server continues advertising and executing only its original 15 methods until A1.7b installs the security-aware service boundary.
