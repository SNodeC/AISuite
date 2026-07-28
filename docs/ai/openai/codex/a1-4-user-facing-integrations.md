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

The four catalog operations `plugin/installed`, `plugin/list`, `plugin/read`,
and `plugin/share/list` remain deferred with the `git`, `local`, `npm`, and
`remote` `PluginSource` alternatives until the next production batch. This
checkpoint introduces no npm build or runtime dependency and does not execute
npm. Notification variants remain at their final 57 and 59 alternatives, and
Codex SOVERSION remains 1.
