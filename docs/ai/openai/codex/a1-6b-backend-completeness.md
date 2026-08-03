# A1.6b Codex backend completeness

A1.6b completes the trusted, transport-neutral C++ `BackendCore` above the
typed Codex App Server client. It does not widen the Codex protocol, and it
does not widen Frontend Protocol v1. The completed direction is:

```text
typed AppServerClient façades
            ↓
complete BackendCore command and policy layer
            ↓
canonical generation-aware Codex state
            ↓
bounded safe snapshots and backend events
            ↓
existing Frontend Protocol v1 adapter
```

A1.7 remains responsible for frontend protocol expansion, additional
transports, SDKs, and user interfaces. Provider-neutral architecture remains
A2.

## Mechanically frozen surface

The production registry and target variants independently establish the
A1.6b denominator:

| Surface | Stable entries |
| --- | ---: |
| Application client operations | 86 |
| Server notifications | 68 |
| `ThreadItem` alternatives | 18 |
| `ResponseItem` alternatives | 16 |
| Server requests | 10 |
| Total | 198 |

The client-request registry contains 87 stable requests; `initialize` is the
one internal lifecycle request and is excluded from the 86 application
operations. Tests derive these values from production identities rather than
from milestone text.

The relevant-layer closure is deliberately reported as separate values:

```text
Implemented:       182
NotApplicable:      16
NotImplemented:      0
Resolved:          198
Total:             198
```

The 16 NotApplicable entries are exactly the stable `ResponseItem`
alternatives. They have no runtime BackendCore state path and use
`NoRuntimeBackendStatePath`; no synthetic runtime path is created for them.

Canonical-state applicability is frozen independently. Exactly 13 application
operations are action/result-only, leaving:

```text
73 stateful application operations
68 server notifications
18 ThreadItem alternatives
10 server requests
--------------------------------
169 applicable canonical-state entries
```

The applicable denominator and implemented numerator are separately guarded
as 169 and 169. The authoritative action-only identity set contains exactly 13
entries and is shared mechanically with the C++ disposition validator. Guards
reject a missing member, an unapproved fourteenth member, or any attempt to
improve a percentage by shrinking the denominator.

## Complete provider command and result model

`BackendCommand` is a flat 101-alternative public variant:

```text
 3 control commands
86 stable provider-operation commands
12 provider-request response/rejection commands
-----------------------------------------------
101 alternatives
```

The 86 provider operations are grouped as follows:

| Domain | Count | Cumulative |
| --- | ---: | ---: |
| Existing thread/turn foundation | 6 | 6 |
| Accounts, models, and configuration | 18 | 24 |
| Remaining threads and turns | 17 | 41 |
| Commands, filesystem/search, permissions, and reviews | 16 | 57 |
| Apps, external agents, feedback, hooks, and marketplace | 9 | 66 |
| Plugins, skills, MCP, and Windows sandbox | 20 | 86 |

The exact MCP OAuth request method is `mcpServer/oauth/login`.
`mcpServer/oauthLogin/completed` remains the differently spelled notification;
the two strings are not interchangeable.

Provider successes preserve their exact typed response. The public
`ProviderOperationValue` variant has the same exact 65 alternatives, in the
same order, as the private typed client-operation decoder. `CommandValue` is a
flat 68-alternative variant: `std::monostate`, `Snapshot`,
`ControllerResult`, followed by the 65 provider results. It does not contain a
nested result variant or the former flattened `typed::Thread` and
`typed::Turn` alternatives.

In particular:

- `ThreadStart` returns `typed::ThreadStartResponse`;
- `ThreadResume` returns `typed::ThreadResumeResponse`;
- `ThreadRead` returns `typed::ThreadReadResponse`;
- `TurnStart` returns `typed::TurnStartResponse`;
- `ThreadList` returns `typed::ThreadListResponse`; and
- `TurnInterrupt` returns `typed::Unit`.

One internal typed provider-operation helper performs global capacity
admission, captures provider generation and callback epoch, submits through
the direct typed façade, maps immediate and asynchronous failures, publishes
state through `BackendEvent -> Reducer -> BackendState`, and only then queues
the exact command completion. Stale callbacks cannot mutate a newer provider
generation or release a newer operation's accounting.

## Action-only ledger

These are the exact 13 operations whose exact result is returned without
fabricating lasting shared state:

```text
account/sendAddCreditsNudgeEmail
thread/shellCommand
turn/interrupt
fs/copy
fs/createDirectory
fs/getMetadata
fs/readDirectory
fs/readFile
fs/remove
fs/writeFile
feedback/upload
mcpServer/resource/read
mcpServer/tool/call
```

Each uses `ActionOnlyNoPersistentState`. All other 73 stable application
operations have a reducer projection and are canonical-state Implemented.

## Exhaustive command policy

Every `BackendCommand` alternative has an explicit `CommandPolicy`; there is
no catch-all policy for a future alternative. All provider operations require
the provider to be Ready. Control commands do not.

The following read-only operations are observer-readable for trusted,
in-process BackendCore sessions:

```text
account/rateLimits/read
account/read
account/usage/read
account/workspaceMessages/read
config/read
configRequirements/read
experimentalFeature/list
model/list
modelProvider/capabilities/read
thread/list
thread/loaded/list
thread/read
thread/goal/get
fs/getMetadata
fs/readDirectory
fs/readFile
fuzzyFileSearch
permissionProfile/list
app/list
externalAgentConfig/detect
externalAgentConfig/import/readHistories
hooks/list
plugin/installed
plugin/list
plugin/read
plugin/share/list
plugin/skill/read
skills/list
mcpServer/resource/read
mcpServerStatus/list
windowsSandbox/readiness
```

`account/read` is parameter-sensitive: an absent or false `refreshToken` is
observer-readable, while `refreshToken == true` is controller-only because it
intentionally refreshes authentication material. Every other provider
operation and every provider-request response or rejection is controller-only.

The filesystem read classification is an authorization decision only for a
trusted, in-process BackendCore session. It does not expose filesystem reads
through Frontend Protocol v1. Any out-of-process exposure requires A1.7's
separate transport and frontend security review.

## Canonical domain state

Provider operation completions preserve exact typed values for the command
caller. The general `providerOperations` ledger and each domain's
`latestResults` collection are metadata-only records: they retain the method,
result alternative, bounded status/subject/page metadata, and source stamp,
not another copy of the complete result. The 42 named trusted replacement
caches retain bounded typed copies after discarding wire-only raw data and
diagnostics:

- account login cancellation/start, rate-limit read, account read, usage, and
  workspace messages;
- model list and provider capabilities;
- configuration read, requirements, experimental features, last writes, and
  feature enablement;
- thread goal get/clear/set, unsubscribe, loaded-thread list,
  permission-profile list, and latest review;
- app list, external-agent detection/import/history, hooks, and marketplace
  add/remove/upgrade results;
- plugin install/catalog/detail/share-list/share-checkout/share-save/
  share-update-targets/skill results plus skills list/configuration results;
- MCP OAuth start and server-status list; and
- Windows sandbox readiness.

Authoritative extra-root state and large process/search payloads instead move
into their dedicated bounded state.

After an exact typed notification has been reduced, the general notification
ledger retains only bounded metadata; authoritative domain fields and caches
carry their own source stamps, so a notification does not broadly promote
unconfirmed sibling state. Safe snapshots project bounded, renderable fields
and never copy occurrence tokens, access tokens, secret answers, arbitrary
unbounded raw JSON, or binary media.

The completed state covers:

- account/login, account, authentication mode, plan, rate-limit, usage,
  workspace-message, reset-credit, and logout state;
- replacement caches for model pages and capabilities;
- configuration, requirements, feature enablement, write metadata, and
  configuration warnings;
- complete thread, turn, item, goal, subscription, rollback, compaction, and
  bounded realtime conversation state;
- process lifecycle, handles, bounded stdout/stderr, exit state, and
  connection invalidation without retaining command input bytes;
- filesystem watches and changes plus bounded fuzzy-search sessions;
- permission-profile, review, guardian, import, hook, OAuth, and setup
  activity;
- apps, external agents, marketplace, plugins, skills, MCP server status, and
  Windows/remote-control status; and
- a bounded typed notice collection for warnings, deprecations,
  configuration/security notices, and Windows world-writable warnings.

Latest-page/read caches replace their previous value rather than merging
unbounded pagination. Cursors and completeness are retained, but BackendCore
does not automatically follow them. Activity, notices, process output,
watches, searches, and realtime text/audio accounting are explicitly bounded.
Provider invalidation marks durable caches Stale and invalidates transient
handles without fabricating successful or terminal provider state. A reconnect
marks only state confirmed by that generation Current. Child item changes do
not promote stale turn or thread metadata.

## Capacity extensions

A1.6a's session, observer, operation, pending-request, conversation-content,
and snapshot bounds remain in force. A1.6b adds these defaults:

```cpp
maxRetainedNotices = 256
maxRetainedProcesses = 256
maxProcessOutputBytesPerProcess = 4 MiB
maxAccumulatedProcessOutputBytes = 16 MiB
maxRetainedFilesystemWatches = 1024
maxRetainedFuzzySearchSessions = 256
maxRetainedActivityRecords = 512
```

Zero means zero capacity. Canonical incremental counters track retained
notices, processes, process-output bytes, filesystem watches, fuzzy searches,
and activity records alongside saturating rejection, eviction, and dropped
output counters. The ordinary under-limit reducer path compares counters in
O(1); it walks a deterministic order only when the corresponding limit is
exceeded.

Process execution, watch creation, and fuzzy search reserve capacity before
provider submission. A reservation is bound to its expected process, watch,
or search identifier. Only a matching early provider notification may promote
that reservation into the concrete resource without double counting; an
unrelated notification cannot steal a pending operation's reservation. Active
resources are protected from ordinary eviction.
Unsolicited provider resources that cannot be represented fail the provider
closed rather than creating an unmanaged live resource. The per-process limit
applies to stdout and stderr combined; per-process and global output retention
prefer the newest data. The existing final serialized
snapshot ceiling remains authoritative and omits optional old state before its
mandatory lifecycle, ownership, bounded pending-request summaries, capacity,
and truncation core.

## Reverse requests

All ten stable server-request alternatives are retained with exact typed
occurrence ownership and bounded safe summaries. The final 12
response/rejection commands are:

```text
ApprovalRespond
UserInputRespond
AuthenticationRespond
UnknownRequestRespondRaw
UnknownRequestReject
ApplyPatchApprovalRespond
ExecCommandApprovalRespond
PermissionsApprovalRespond
AttestationGenerateRespond
DynamicToolCallRespond
McpServerElicitationRespond
KnownRequestReject
```

The three application-friendly response commands also accept their complete
schema response alternatives. `KnownRequestReject` is restricted to the four
known request types whose typed façade supports rejection; unknown raw
response/rejection remains unknown-request-only. Before calling the typed
response façade, BackendCore preflights the reducer sequence needed to publish
the corresponding removal. Sequence exhaustion therefore returns
`backend_unavailable` without sending a response and retains the pending
occurrence. Validation or enqueue failure likewise retains it. A successful
enqueue immediately publishes `PendingRequestRemoved` and returns
`typed::Unit`; sensitive response data is never returned in `CommandValue` or
logged.

`serverRequest/resolved` retires the matching current-generation pending
request exactly once. Unknown, stale-generation, duplicate, and mismatched
notifications are harmless; a conflicting thread association retains the
request and records a bounded, redacted extension marker.

## Notifications and item variants

All 68 stable typed server notifications translate to at least one backend
event. The existing `error` notification remains on its single typed turn-error
path. The ten formerly empty translations are completed:

```text
mcpServer/oauthLogin/completed
mcpServer/startupStatus/updated
deprecationNotice
process/exited
process/outputDelta
remoteControl/status/changed
serverRequest/resolved
warning
windows/worldWritableWarning
windowsSandbox/setupCompleted
```

Durable notification families update their dedicated domain state. A generic
extension record remains only as a bounded, redacted occurrence projection;
no stable notification disappears, and one provider notification is not
delivered twice.

All 18 stable `ThreadItem` alternatives retain the exact typed value in
trusted state and have a dedicated bounded safe projection. This includes the
ten formerly generic-only alternatives: collab-agent tool call, context
compaction, entered/exited review mode, hook prompt, image generation, image
view, plan, sleep, and sub-agent activity. Binary data, unbounded prompts,
secrets, and complete raw provider JSON are excluded. Unknown future items
remain bounded and forward-compatible.

The 16 stable `ResponseItem` alternatives remain codec-complete protocol types
with no BackendCore runtime-state path. Their exact disposition remains
`NotApplicable / NoRuntimeBackendStatePath`.

## Frontend Protocol v1 boundary

A1.6b does not add frontend methods, fields, message kinds, error codes, or
snapshot sections. The protocol identity, version, replay journal, controller
behavior, batching, coalescing, and existing security dispositions are
unchanged. The adapter mechanically projects exact BackendCore wrappers back
to the existing v1 results:

```text
ThreadStartResponse  -> result.thread
ThreadResumeResponse -> result.thread
ThreadReadResponse   -> result.thread
TurnStartResponse    -> result.turn
ThreadListResponse   -> existing page result
Unit                 -> existing empty object
```

The other provider result alternatives are unreachable from the frozen v1
command set. The adapter treats an unexpected occurrence as an internal error
rather than inventing a v1 encoding or exposing raw typed state. Existing
generic pending-request and `codex.extension` contracts remain the only v1
projection for domains not yet given dedicated frontend methods.

## ABI and package boundary

A1.6b extends the still-unreleased SOVERSION-2 development boundary without
changing project version `0.1.0`. The installed public-header inventory remains
29 main, 7 backend, and 7 frontend headers. The canonical imported targets
remain `AISuite::OpenAICodex`, `AISuite::OpenAICodexBackend`, and
`AISuite::OpenAICodexFrontend`; AISuite continues to consume installed
`snodec::core`, with `snodec::net-un-stream-legacy` required only by the Unix
reference application and staged server consumer.

The typed protocol remains 339 Complete, 0 Partial, 0 NotImplemented, and 48
NotApplicable. Protocol methods, codecs, schema evidence, occurrence ownership,
and the stable protocol variant sizes and indices are unchanged.
