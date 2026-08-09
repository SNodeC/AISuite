# A1.7a Codex frontend contract

A1.7a completes and freezes the owner-approved additive Frontend Protocol v1
contract. It preserves protocol identity `snodec.codex-frontend`, version `1`,
the eight message kinds, and every existing field and method meaning. At its
landing it did not activate the additive command/state/event surface at
runtime.

Authentication, authorization enforcement, connection-specific scope
projection, `FrontendService`, additional listeners/transports, and provider
lifecycle exposure were intentionally deferred to A1.7b. They are documented
in the [A1.7b FrontendService report](a1-7b-frontend-service.md). This A1.7a
report remains the authority for the frozen contract and must not be read as a
claim that those runtime features belonged to A1.7a or that a UI exists.

## Verified prerequisite

The work started from `origin/master` at
`c2910324533daf7c4779d537952f27d7c2ae4490`. Before production changes, all
targets built and the complete ordinary suite registered 147 tests: 146
passed, one credential-dependent integration test was skipped, and none
failed. Both installed consumers and the source and binary package tests
passed.

The inherited invariants were mechanically rechecked: typed Codex protocol
339/0/0/48, 86/86 BackendCore operations, 169/169 applicable canonical-state
entries, 182/16/0 backend/state disposition, 65-alternative
`ProviderOperationValue`, 68-alternative `CommandValue`, and 101-alternative
`BackendCommand`.

## Additive method catalog

The generated Frontend Protocol catalog contains exactly:

```text
original runtime methods       15
additive method definitions    90
                              ---
defined v1 methods            105
```

The same 105 methods decompose by implementation target as:

```text
frontend-native methods         7
non-native BackendCore methods 98
                              ---
defined v1 methods            105
```

The seven native methods are `controller.acquire`, `controller.release`,
`snapshot.get`, `events.replay`, `provider.start`, `provider.stop`, and
`provider.restart`. The 98 non-native methods are all 86 stable provider
operations and all 12 reverse response/rejection commands. The arithmetic is
independently guarded: the original 15 consist of four native, six provider,
and five reverse methods; the additive 90 consist of three native, 80 provider,
and seven reverse methods.

The complete reverse set is `request.approval.respond`,
`request.userInput.respond`, `request.authentication.respond`,
`request.unknown.respond`, `request.unknown.reject`,
`request.applyPatchApproval.respond`, `request.execCommandApproval.respond`,
`request.permissionsApproval.respond`, `request.attestation.respond`,
`request.dynamicTool.respond`, `request.mcpElicitation.respond`, and
`request.known.reject`. A compiled target-mapping guard constructs the exact
`BackendCommand` alternative for every one of the 86 provider and 12 reverse
methods without placing that converter on the production dispatch path.

The runtime that landed with A1.7a accepted exactly the original 15 methods;
all 90 additive methods were contract definitions whose metadata said
`currentlyImplemented: false`. A1.7b subsequently supplies all 105 handlers
while keeping 15 conditional methods deployment-disabled by default. Exact-name
lookup rejects prefixes and suffixes, so related names such as `command.exec`
and `command.exec.resize` remain distinct. Definition still does not imply
deployment enablement, permission, or invocation readiness.

## Fixed review denominator

The complete owner-review denominator is fixed as two disjoint sets:

- 148 previously unresolved decisions: 86 stable application operations, ten
  stable server requests, 35 experimental client requests, one experimental
  server request, and 16 stable `ResponseItem` alternatives;
- 86 existing compatibility contracts: all 68 stable server notifications and
  all 18 stable `ThreadItem` alternatives.

Their union is exactly 234 identities. Every identity has a final exposure and
security disposition, leaving zero unresolved decisions. Exactly 36
experimental requests are denied frontend exposure. Exactly 16 `ResponseItem`
alternatives remain genuinely `NotApplicable` because A1.6 established that
they have no runtime backend-state path.

The generator rejects a missing or duplicate reviewed identity, changed bucket
count, extra `NotApplicable` identity, unresolved final decision, or denominator
reduced below 148 + 86. Reports retain the two denominator components and final
zero separately; a percentage cannot become complete by silently shrinking the
review population.

Final exposure counts are 71 dedicated methods, 15 conditional methods, ten
dedicated pending-request contracts, 22 approved existing event/item
contracts, 54 dedicated events with legacy extension compatibility, ten
dedicated items with legacy metadata compatibility, 36 deliberately
non-exposed experimental requests, and 16 genuine `NotApplicable` rows. Final
security counts are 26 observer reads, 22 controller-required methods, 22
privileged scoped methods, 15 conditionally enabled methods, one
parameter-sensitive method, 96 scope-projected state/event contracts, 36
approved non-exposures, and 16 genuine `NotApplicable` rows.

## Authority and generation direction

Owner-approved frontend exposure and security policy lives upstream with the
App Server surface authority in `tools/codex/app_server_surface.py` and the
production protocol registry. Generation proceeds only downstream:

```text
app_server_surface.py frontend policy
        + production registry and pinned schema evidence
        -> tools/frontend/frontend-registry-source.json
        + tools/frontend/frontend-protocol-v1.schema.template.json
        -> tools/frontend/generate_frontend_protocol.py
             |-> docs/.../frontend-protocol-v1.manifest.json
             |-> docs/.../frontend-protocol-v1.schema.json
             `-> frontend/GeneratedProtocol.h
```

The frontend-registry export is the downstream generator's sole provider
inventory input. The frontend generator does not reparse vendored Rust,
TypeScript, schemas, or a local Codex executable and does not invent owner
policy. The schema template is the legacy-v1 compatibility base. The manifest,
complete JSON Schema, and generated C++ header are outputs, not authority fed
back into the source. Currentness and bijection tests reject manual drift.

## Capability discovery

Hello and welcome gain only optional additive discovery fields. A client may
request capabilities in hello. Welcome may report a capability advertisement,
available methods, permitted methods, and server version. Their absence retains
legacy v1 bytes and semantics.

A capability advertisement separates `defined`, `implemented`, and `permitted`.
The exact 18 capability names are:

```text
method_discovery                 security_scopes
complete_provider_operations     complete_reverse_requests
complete_backend_domains         conditional_filesystem
conditional_command_execution    dedicated_pending_requests
dedicated_notification_events    complete_thread_items
authenticated_frontend           scope_projected_state
provider_lifecycle               multi_transport
cpp_client_sdk                   typescript_client_sdk
browser_ui                       qt_ui
```

A1.7a generated metadata marked only `method_discovery` and `security_scopes`
as implemented by its landing runtime. A1.7b implements 13 service mechanisms.
The `multi_transport` identity remains defined for v1 compatibility but is not
implemented or advertised because SNode.C owns listener lifecycle and AISuite
keeps no duplicate transport registry. Future product capabilities remain
false. A defined capability or method is never treated as automatically
permitted.

## Security profiles

The exact default remote profile is:

```text
observe
control
```

The local trusted profile has all 12 scopes:

```text
observe
control
provider_lifecycle
account_management
configuration_write
command_execution
filesystem_read
filesystem_write
extension_management
mcp_invoke
sensitive_response
unknown_request_response
```

Scope possession and controller ownership remain independent. Holding
`control` does not acquire the controller role, and controller ownership does
not grant a scope. Dispatch must satisfy every method scope, the controller
requirement, deployment enablement, authentication state, and provider state.

All filesystem operations and arbitrary command execution are conditional and
default-disabled. The conditional set includes filesystem reads, fuzzy search,
watches, filesystem mutations, `command.exec` and its process-control family,
and `thread.shellCommand`. A trusted in-process BackendCore read policy is not
remote frontend authorization.

`account.read` has a frozen parameter-sensitive rule:

- absent or false `refreshToken`: observer read requiring `observe`;
- true `refreshToken`: requires `control`, `account_management`, and current
  controller ownership.

The generator rejects enabling a conditional filesystem or command method by
default and rejects loss of the parameter-sensitive account rule.

The exact 15 default-disabled methods are the ten filesystem/search/watch
methods (`fs.copy`, `fs.createDirectory`, `fs.getMetadata`, `fs.readDirectory`,
`fs.readFile`, `fs.remove`, `fs.unwatch`, `fs.watch`, `fs.writeFile`, and
`fuzzyFileSearch`) plus `command.exec`, `command.exec.resize`,
`command.exec.terminate`, `command.exec.write`, and `thread.shellCommand`.

## Notification and item compatibility

All 68 stable server notifications retain one existing v1 compatibility path:
14 use normalized state/events and 54 use bounded, recursively redacted
`codex.extension`. All 18 stable `ThreadItem` alternatives also retain one
path: eight use normalized item contracts and ten retain bounded metadata-only
compatibility.

The additive schema defines all 25 expanded event families, all 18 safe item
kinds, all ten safe pending-request kinds, and scope-projectable backend
snapshot domains. A connection receiving expanded projections must not also
receive the legacy projection for the same provider occurrence. This
duplicate-suppression rule preserves existing bytes for legacy connections
while allowing later capability-gated expansion.

A1.7a defines and mechanically checks these mappings. A1.7b activates them
through one mandatory per-principal projection path while retaining the
legacy paths without duplication. No raw provider JSON, occurrence token,
authentication token, secret answer, unbounded content, or binary payload
becomes a safe frontend projection.

## Schema, C++ values, and compatibility guards

The committed Draft 2020-12 schema has exact parameter and result references
for all 105 methods, all 25 expanded event families, the complete safe expanded
snapshot, ten pending-request kinds, and all 18 `ThreadItem` kinds. Generated
minimal, complete, nullable, and malformed fixtures drive the C++ codec. Safe
unknown fields are bounded and standard schema `propertyNames` rules reject
credential-shaped names; the C++ validator applies the same rule without
serializing secrets.

## C++ tagged-JSON model

A1.7a's complete C++ protocol model is a method-tagged, schema-validated JSON
contract layer. `generated::MethodParameters<Id>` and
`generated::MethodResult<Id>` distinguish all 105 frontend methods at compile
time, while each payload remains a `nlohmann::json` value. These types provide
exact method correlation, generated metadata, schema validation, and wire
conformance; they are not yet the ergonomic domain-typed C++ application API.

A1.7c-1 owns `AISuite::OpenAICodexFrontendClient`: domain-oriented façades,
callback-last asynchronous operations, typed client-side state,
replay/reconnection, and stable application workflows that do not require raw
JSON. The A1.7a method tags and 105-alternative parameter/result variants remain
useful protocol types and are not a substitute for that SDK.

## Runtime schema validation profile

The published protocol artifact is JSON Schema Draft 2020-12. The production
C++ validator is deliberately not advertised as a general-purpose Draft
2020-12 implementation. It implements the exact assertion and numeric-format
subset that is mechanically audited as reachable from the generated AISuite
runtime schemas. Generator currentness fails if an unsupported assertion,
format, malformed or non-local RFC 6901 reference, or unreviewed
`x-aisuite-*` assertion enters that runtime subset.

The closed runtime assertion vocabulary is:

```text
$ref  allOf  anyOf  oneOf  not  if  then  else
type  const  enum
properties  propertyNames  additionalProperties  required
minProperties  maxProperties
items  minItems  maxItems  uniqueItems
minLength  maxLength  pattern
minimum  maximum  format
x-aisuite-sensitiveFieldNamesForbidden
x-aisuite-forbiddenNormalizedPropertyNames
```

`$defs` is the structural keyword. The exact annotation-only vocabulary
currently present is `$id`, `$schema`, `default`, `description`, `title`,
`x-aisuite-frontend-contract`, and `x-aisuite-redactionClass`. The two
reviewed custom assertions are distinct from the last two annotation-only
keywords. Supported numeric formats are exactly `int32`, `int64`, `uint`,
`uint16`, `uint32`, and `uint64`.

The current generated graph uses 27 of the 29 supported assertions (`else` and
`minProperties` are presently unused), two distinct patterns, and nine bounded
`uniqueItems` sites. Its largest unique catalog has cardinality 105 and at most
5,460 pair comparisons. Those values are mechanically recomputed and guarded.
Generated patterns pass a generation-time syntax check and both exact current
patterns are compiled and exercised under the C++ runtime; this is not a claim
of general ECMA-262 or Draft-2020-12 regular-expression conformance.

The generated schema is parsed once. Production validation uses deterministic
limits of 128 schema-recursion levels and 4,000,000 node visits; validator
exceptions are contained at the `Codec` boundary. Every generated
`uniqueItems: true` array must have a statically provable maximum cardinality,
so its worst-case pair-comparison count is known during generation rather than
discovered from untrusted input.

The reviewed valid corpus comprises 558 minimal, complete, nullable, snapshot,
and event validations. Its maxima are 3,323 visits, depth 16, 1,806 resolved
references, 38 evaluated alternatives, 28 discriminator fast paths, zero
unique-item comparisons, and 11 regular-expression evaluations. The valid
2,000-item expanded snapshot uses 225,307 visits at depth 12. The current
generated `$defs` reference graph is acyclic, so exact production-limit tests
use the private synthetic-schema seam; real generated snapshots are separately
tested at their measured exact depth and through the public codec. A deeply
nested additive result also reaches the generated sensitive-field guard, is
rejected at the public codec's production depth bound, and leaves that codec
immediately reusable.

Unknown non-conflicting fields are an intentional additive-v1 compatibility
rule. Known fields are still validated, and unknown values still pass the
applicable safe-name, sensitive-field, nesting, size, and nested-value checks.
In particular, generated `additionalProperties: false` closes the published
schema for general validators but is deliberately not used by the AISuite C++
runtime to reject safe additive v1 fields. A1.7b owns network admission,
the frontend frame bound, rate limiting, authentication, and connection-scope
enforcement; those runtime features are implemented and documented by the
subsequent A1.7b milestone, not by this A1.7a contract milestone.

The five additive error-code strings are `authentication_required`,
`authentication_failed`, `origin_rejected`, `transport_security_required`, and
`rate_limited`. They are contract and codec definitions only; A1.7a does not
create the corresponding authentication, Origin, TLS, or rate-limit runtime
conditions.

`Protocol.h` continues exposing all 15 original `inline constexpr` method
names as aliases of generated constants. Legacy command branches, message
fixtures, replay, synchronization, controller behavior, and response shapes
remain byte-compatible. Exact lookup covers every discovered prefix pair and
never uses prefix routing. The capability projection function selects exactly
one legacy or expanded representation for every one of the 68 notification and
18 item mappings.

## Public headers and versioning

A1.7a adds exactly two installed public frontend headers:

- `GeneratedProtocol.h`, containing generated method IDs, exact strings,
  metadata, and complete method-tagged JSON command/result definitions;
- `Security.h`, containing the stable scope enum and the default-remote and
  local-trusted profiles.

Installed public-header inventory is:

```text
main       29
backend     7
frontend    9
           --
total      45
```

A1.7b subsequently replaces `BackendAdapter.h` with `FrontendService.h`
without an alias, so the 9/45 counts remain unchanged.

Project version remains `0.1.0`. All three Codex libraries remain on the
intentionally unreleased SOVERSION 2 boundary. No compatibility library or ABI
version 3 is introduced.

## Subsequent A1.7 ownership

- A1.7b: authenticated and scope-enforcing `FrontendService`, per-connection
  capability/state projection, all approved handlers and mappings, provider
  lifecycle exposure, and multi-transport composition;
- A1.7c-1: C++ Frontend SDK and `codex-backend-client` migration;
- P0–P3: owner-approved architecture reduction after A1.7c-1, with generic
  SNode.C prerequisites and the complete greenfield frontend built in parallel
  before final cutover and legacy deletion;
- A1.7c-2: migration of the existing `codex-ui` into the reduced canonical
  standalone AI IDE after P3;
- A1.7d: TypeScript Frontend SDK and browser frontend.

A1.7a itself started none of those runtime or product deliverables. Persistence,
multiple controllers, forced takeover, and provider-neutral architecture are
also outside this contract milestone; provider-neutral work remains A2.
