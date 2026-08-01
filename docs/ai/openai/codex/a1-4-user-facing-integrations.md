# Codex A1.4 user-facing integrations

## Status and authority

This milestone types the 33 stable user-facing integration identities from the
checked-in Codex App Server 0.144.6 protocol. The vendored stable schemas and
Rust contracts remain the protocol authority.

The `ProtocolSurfaceRegistry` is the current implementation-state authority.
Generated descriptors, production codecs, fixtures, and current product tests
validate that state against the pinned schemas and Rust contracts. Experimental
schemas are not implementation inputs.

The exact user-integration denominator is:

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

## Functional composition

The first production batch completes:

- `app/list`;
- `externalAgentConfig/detect`;
- `externalAgentConfig/import`;
- `externalAgentConfig/import/readHistories`;
- `feedback/upload`;
- `app/list/updated`;
- `externalAgentConfig/import/completed`; and
- `externalAgentConfig/import/progress`.

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

Hooks, marketplace, skills, and plugins are completed by the following
functional groups. MCP/reverse and runtime/platform behavior is documented in
their respective current-state reports.
Codex SOVERSION remained 1 for this implementation slice; Final A1b
subsequently moves all three Codex libraries to SOVERSION 2.

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

The batch adds the `Hooks`, `Marketplace`, and `Skills` facades without adding
local hook execution, marketplace management, or skill scanning. Their public
methods are `Hooks::list`, `Marketplace::add`, `Marketplace::remove`,
`Marketplace::upgrade`, `Skills::writeConfig`, `Skills::setExtraRoots`, and
`Skills::list`. `skills/extraRoots/set` is the first user-integration Unit-result
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

`plugin/share/delete` and `plugin/uninstall` use the established exact
Unit-result decoder; the other five operations have concrete typed responses.

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

A14-UserIntegrations contributes exactly 33 Complete native A1.4 identities.
MCP/reverse requests and runtime/platform behavior complete the rest of the
native slice without changing this group's denominator.

The `Plugins` facade exposes all eleven user-integration methods: `install`,
`installed`, `list`, `read`, `shareCheckout`, `shareDelete`, `shareList`,
`shareSave`, `shareUpdateTargets`, `readSkill`, and `uninstall`. Each method
continues to submit through the one existing `RawProtocol`; AISuite performs no
catalog scan, installation, sharing, or source retrieval itself.

`npm` is only a Codex protocol discriminator and data structure. This
milestone adds no Node.js or npm build/runtime dependency and executes neither.
This group leaves the canonical and Event variants at 57 and 59 alternatives;
the later A1.4 groups append to the current sizes of 67 and 69 without moving
these predecessors. Codex SOVERSION remained 1 for this implementation slice;
Final A1b subsequently moves all three Codex libraries to SOVERSION 2 after the
public aggregate and variant additions established the rebuild boundary.

The current registry has since completed the MCP/reverse, runtime/platform,
and Common Final A1a groups: global status is 339 Complete / 0 Partial / 0
NotImplemented / 48 NotApplicable, and native A1.4 remains 56 Complete / 0
Partial / 0 NotImplemented.

## Packaging and integrity

Current-state tests cover the exact `23 / 6 / 0 / 4` taxonomy, `20 / 3`
concrete/Unit result split, `52 / 118 / 411` stable schema closure, public
methods, descriptors, fixtures, installed headers, and package boundaries.

The installed-consumer gate reuses the one installed current SNode.C prefix
and the one configured AISuite build, installs AISuite into an isolated
prefix, and configures the external consumer with package registries disabled
and inherited compiler, linker, loader, include, CMake, and pkg-config search
variables removed. Cache, compile-command, verbose-link, `readelf`, and `ldd`
evidence must resolve AISuite and SNode.C headers and libraries only from the
two install prefixes while rejecting the AISuite checkout/build. A small
installed public-header consumer links `snodec::core` directly and compiles
against the current installed
`include/snode.c` root. The sanitized consumers exercise all seven new
facades, all six appended notification types, and the five-alternative
`PluginSource` variant through installed public headers.

Dependency discovery uses only the current installed SNode.C prefix through
`CMAKE_PREFIX_PATH`; no SNode.C source checkout is required by AISuite tests.
Source packages contain no `.git`, and binary packages require all seven
facade headers while excluding private tools, tests, evidence, and codec
implementation files.

`AppServerClient` and `typed::Client` retain their PIMPL ownership model and no
predecessor variant index moves. The appended public notification alternatives,
the new public aggregates, and `PluginSource` nevertheless change the public
C++ API/ABI. SOVERSION intentionally remains 1 under the current A1 policy;
that unchanged SONAME is not a binary-compatibility claim, and consumers must
be rebuilt with the updated library.

The vendored Codex 0.144.6 schemas and protocol source define this protocol
surface, while the `ProtocolSurfaceRegistry`, generated descriptors, production
codecs, and current codec/wire tests verify its implementation. The public-header
policy, isolated self-containment test, installed consumer, and package tests
verify the current public API and consumption boundary without treating a
historical ABI report or symbol snapshot as an active authority.

Sensitive app, external-agent, feedback, hook, marketplace, plugin, skill,
thread, turn, opaque JSON, and raw-envelope values remain outside production
diagnostics and logs. The generic backend extension path remains bounded and
redacted, and neither backend state nor Frontend Protocol gains a
user-integration product surface.
