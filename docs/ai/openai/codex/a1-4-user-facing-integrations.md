# Codex A1.4 user-facing integrations

## Status and authority

This milestone types the 33 stable user-facing integration identities assigned
to PR A at Codex App Server 0.144.6. The audited base is commit
`10d3829958a6a17e7437326b6c42c51f3a8de4ec`, tree
`7b5e6500780f1c633fe18af5fba6164bd222a3ba`. The protocol authority remains
upstream tag `rust-v0.144.6`, source commit
`5d1fbf26c43abc65a203928b2e31561cb039e06d`.

The pinned stable schemas, vendored Rust contracts, frozen A1.4 evidence, and
`ProtocolSurfaceRegistryData.inc` remain authoritative. Experimental schemas
are not implementation inputs.

The exact PR-A denominator is:

| Category | Count |
|---|---:|
| Client requests | 23 |
| Server notifications | 6 |
| Server requests | 0 |
| `PluginSource` alternatives | 4 |
| **Total** | **33** |

Twenty request operations have concrete results. The three Unit-result
operations are exactly `plugin/share/delete`, `plugin/uninstall`, and
`skills/extraRoots/set`. The transitive stable schema closure is fixed at 52
seed definitions, 118 reachable v2 definitions, and 411 schema paths.

## Staged implementation

The first production batch completes:

- `app/list`;
- `externalAgentConfig/detect`;
- `externalAgentConfig/import`;
- `externalAgentConfig/import/readHistories`;
- `feedback/upload`;
- `app/list/updated`;
- `externalAgentConfig/import/completed`; and
- `externalAgentConfig/import/progress`.

At this checkpoint native A1.4 is 8 Complete, 1 Partial, and 47
NotImplemented. The global registry is 288 Complete, 4 Partial, 47
NotImplemented, and 48 NotApplicable. The Partial identities remain
`initialize`, `initialized`, `error`, and `item/tool/requestUserInput`.

The public entry points added by this batch are
`client.typed().apps().list()`,
`client.typed().externalAgents().detect()`,
`client.typed().externalAgents().importConfiguration()`,
`client.typed().externalAgents().readImportHistories()`, and
`client.typed().feedback().upload()`. The three notifications use the existing
typed `Events` observer and continue to coexist with raw observation.

All operations reuse the single existing path:

```
AppServerClient -> RawProtocol -> typed::Client -> grouped facade or Events
```

AISuite does not list apps locally, import external-agent state, or upload
feedback itself. Those behaviors remain owned by the Codex App Server.
Sensitive app, import, and feedback values are excluded from diagnostics.
Notifications retained through the generic backend extension path are
method-specifically redacted before snapshot or frontend exposure.

The predecessor variants were exactly 51 alternatives for
`CanonicalServerNotification` and 53 alternatives for `Event`. This batch
appends `AppListUpdatedNotification`,
`ExternalAgentConfigImportCompletedNotification`, and
`ExternalAgentConfigImportProgressNotification` at canonical indices 51–53
and Event indices 53–55. No predecessor alternative moves.

Native A1.4 remains in progress after this checkpoint. Hooks, marketplace,
skills, and plugins are completed by the following production batches; PR B,
PR C, inherited A1.0 Partials, and InventoryOnly identities remain untouched.
Codex SOVERSION remains 1 under the frozen milestone policy.

The second production batch completes:

- `hooks/list`;
- `marketplace/add`;
- `marketplace/remove`;
- `marketplace/upgrade`;
- `skills/config/write`;
- `skills/extraRoots/set`;
- `skills/list`;
- `hook/completed`;
- `hook/started`; and
- `skills/changed`.

At this checkpoint native A1.4 is 18 Complete, 1 Partial, and 37
NotImplemented. The global registry is 298 Complete, 4 Partial, 37
NotImplemented, and 48 NotApplicable.

The batch adds the `Hooks`, `Marketplace`, and `Skills` facades without adding
local hook execution, marketplace management, or skill scanning. Their public
methods are `Hooks::list`, `Marketplace::add`, `Marketplace::remove`,
`Marketplace::upgrade`, `Skills::writeConfig`, `Skills::setExtraRoots`, and
`Skills::list`. `skills/extraRoots/set` is the first PR-A Unit-result
operation; the other six requests have concrete typed responses.

`HookCompletedNotification`, `HookStartedNotification`, and
`SkillsChangedNotification` append at canonical notification indices 54–56
and Event indices 56–58. The resulting variants have 57 and 59 alternatives,
respectively, while every predecessor index remains unchanged. Open stable
objects preserve future fields, nullable fields retain omission versus
explicit-null state, and default-bearing hook source data is not invented
during decoding. Hook and skill payloads continue through the existing
generic extension route with method-specific snapshot redaction and no new
frontend protocol or backend state.

The third production batch completes the seven plugin operations that do not
reach `PluginSource`:

- `plugin/install`;
- `plugin/share/checkout`;
- `plugin/share/delete`;
- `plugin/share/save`;
- `plugin/share/updateTargets`;
- `plugin/skill/read`; and
- `plugin/uninstall`.

At this checkpoint native A1.4 is 25 Complete, 1 Partial, and 30
NotImplemented. The global registry is 305 Complete, 4 Partial, 30
NotImplemented, and 48 NotApplicable. `plugin/share/delete` and
`plugin/uninstall` use the established exact Unit-result decoder; the other
five operations have concrete typed responses.

The `Plugins` facade exposes `install`, `shareCheckout`, `shareDelete`,
`shareSave`, `shareUpdateTargets`, `readSkill`, and `uninstall`. It submits
each operation through the existing `RawProtocol`; AISuite does not install,
remove, share, or execute plugins locally. Plugin names, paths, principals,
targets, skill contents, and opaque future fields are never included in
production diagnostics.

The final production batch completes the four catalog operations that reach
`PluginSource`:

- `plugin/installed`;
- `plugin/list`;
- `plugin/read`; and
- `plugin/share/list`.

It also completes the four known `PluginSource` identities in exact
production-registry order: `git`, `local`, `npm`, and `remote`. The public
variant appends `UnknownPluginSource` after those known alternatives. `git`
requires `url` and distinguishes omitted, null, and present `path`, `refName`,
and `sha`; `local` requires `path`; `npm` requires `package` and preserves the
three states of `registry` and `version`; `remote` has no field beyond its
required discriminator. Every alternative retains the original open-object
JSON alongside its typed fields.

A future discriminator decodes nonfatally into `UnknownPluginSource`, retains
the discriminator and complete raw object, and records a forward-compatibility
diagnostic. A known discriminator with a missing or wrong-typed required field
also retains the raw object but records a malformed-known diagnostic; it is
never relabeled as a future alternative. Diagnostics identify only structural
paths and expected types, never plugin values.

At this checkpoint A14-UserIntegrations is complete: native A1.4 is exactly 33
Complete, 1 Partial, and 22 NotImplemented. The global registry is exactly 313
Complete, 4 Partial, 22 NotImplemented, and 48 NotApplicable. Native A1.4
remains in progress: PR B and PR C stay unimplemented, and
`item/tool/requestUserInput` remains Partial alongside the inherited A1.0
Partials `initialize`, `initialized`, and `error`.

The `Plugins` facade now exposes all eleven PR-A methods: `install`,
`installed`, `list`, `read`, `shareCheckout`, `shareDelete`, `shareList`,
`shareSave`, `shareUpdateTargets`, `readSkill`, and `uninstall`. Each method
continues to submit through the one existing `RawProtocol`; AISuite performs no
catalog scan, installation, sharing, or source retrieval itself.

`npm` is only a Codex protocol discriminator and data structure. This
milestone adds no Node.js or npm build/runtime dependency and executes neither.
Notification variants remain at their final 57 and 59 alternatives. Codex
SOVERSION remains 1; the public aggregate and variant additions have real ABI
impact even though the frozen SONAME policy defers the scoped bump to final A1
closure.

## Closure, packaging, and integrity

The PR-specific audit freezes the exact `23 / 6 / 0 / 4` taxonomy, `20 / 3`
concrete/Unit result split, `52 / 118 / 411` stable schema closure, batch
ownership, public methods, descriptors, fixtures, and installed headers.
Planted-failure tests require stable diagnostic codes for scope leakage,
result-contract drift, schema-closure drift, false completion, package
boundary violations, predecessor or appended variant movement, SOVERSION
drift, predecessor-evidence drift, and second-pass nondeterminism.

The installed-consumer gate reuses the one installed cleaned SNode.C prefix
and the one configured AISuite build, installs AISuite into an isolated
prefix, and configures the external consumer with package registries disabled
and inherited compiler, linker, loader, include, CMake, and pkg-config search
variables removed. Cache, compile-command, verbose-link, `readelf`, and `ldd`
evidence must resolve AISuite and SNode.C headers and libraries only from the
two install prefixes while rejecting the AISuite checkout/build and the
historical SNode.C provenance worktree. A small installed public-header
consumer links `snodec::core` directly and must compile with the cleaned
`include/snode.c` root. The sanitized consumers exercise all seven new
facades, all six appended notification types, and the five-alternative
`PluginSource` variant through installed public headers.

Local validation supplies the detached historical SNode.C worktree only
through `AISUITE_TEST_SNODEC_SOURCE_REPOSITORY`; normal dependency discovery
uses the cleaned installed prefix through `CMAKE_PREFIX_PATH`. Source packages
contain no `.git`, cannot discover an enclosing checkout under their
`GIT_CEILING_DIRECTORIES`, retain the three full-corpus generation-proof JSON
files, and run package-safe extraction, API/ABI, and closure checks. Binary
packages require all seven facade headers and exclude private tools, tests,
evidence, and codec implementation files.

The complete inherited and PR-A generated corpus is regenerated with the
authoritative repository tools in dependency order. Predecessor deltas are
accepted only where hashes or derived evidence changed because of reviewed
production/test inputs; identity keys, schema paths, contracts, ownership,
predecessor variant order, protocol pins, and frozen InventoryOnly boundaries
must remain unchanged. A second complete generator pass must be byte-identical
to the first for every target-corpus path, byte count, and SHA-256. That target
includes the static closure report and the extraction manifest. Exactly three
proof metadata files sit outside the target they describe; extraction records
only their fixed paths, while the specialized closure guard canonically hashes
and validates them and requires the final live target to equal both passes.
The extraction manifest is the unique last generator in each pass and retains
the original SNode.C source commit, tree, filtered-history baseline, commit
map, and original source hashes.

`AppServerClient` and `typed::Client` retain their PIMPL ownership model and no
predecessor variant index moves. The appended public notification alternatives,
the new public aggregates, and `PluginSource` nevertheless change the public
C++ API/ABI. SOVERSION intentionally remains 1 under the frozen PR-A policy;
that unchanged SONAME is not a binary-compatibility claim, and consumers must
be rebuilt with the updated library.

The authoritative
`app_server_a1_4_user_integrations_abi.py` generator records 112 layout and
variant observations from
`CodexA14UserIntegrationsAbiLayoutProbe.cpp`, hashes every affected public
header, and captures a sorted manifest of 715 strong exported Codex symbols.
The generated API/ABI JSON and symbol manifest are checked against both the
current public headers and the strict GCC library; they are package and
full-corpus generation inputs rather than a compatibility claim.

Sensitive app, external-agent, feedback, hook, marketplace, plugin, skill,
thread, turn, opaque JSON, and raw-envelope values remain outside production
diagnostics and logs. The generic backend extension path remains bounded and
redacted, and neither backend state nor Frontend Protocol gains a
user-integration product surface.
