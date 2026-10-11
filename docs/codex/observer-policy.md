<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p><picture><source media="(max-width: 640px)" srcset="../media/nav/banner-codex-compact.svg"><img src="../media/nav/banner-codex.svg" alt="AISuite — Codex" width="100%"></picture></p>
<p><a href="README.md#page-overview" title="Up to Codex"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/up-codex-dark.svg"><img src="../media/nav/up-codex-light.svg" alt="Up to Codex" width="160" height="28"></picture></a><a href="https://github.com/SNodeC" title="SNodeC organization"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/organization-dark.svg"><img align="right" src="../media/nav/organization-light.svg" alt="SNodeC organization" width="160" height="28"></picture></a><a href="../../README.md#project-overview" title="AISuite repository"><picture><source media="(prefers-color-scheme: dark)" srcset="../media/nav/repository-dark.svg"><img align="right" src="../media/nav/repository-light.svg" alt="AISuite repository" width="160" height="28"></picture></a><br clear="all"></p>
<!-- snodec:end page-header -->

# Codex bridge observer policy

For operator settings and trust boundaries, use [the runtime configuration reference](../architecture/codex-bridge.md#access-boundary). This page specifies exact request classification and records implementation evidence.

## Boundary and compatibility

The bridge owns the upstream initialization handshake and routes one initialized app-server connection to multiple frontends. The registered frontend role, not an envelope's claimed role, controls access. Controllers keep generic forwarding; observers may issue only exact-match approved requests when `observersMayRead` is enabled. Unknown methods, frontend notifications, and responses to another frontend's server request are not observer permissions.

This extension preserves all 34 established observer methods and adds 16 reads. No wire format, generated binding, request-ID translation, controller-transfer rule, notification fan-out, transport, or frontend API changes. Nothing requires an update to an existing CodexUI client. New bridge permissions do not add UI controls or override a frontend's own command policy.

The observer trust model remains the existing one: observers may see account, configuration, thread and workspace information, including the previously allowed filesystem reads. `observersMayRead=false` disables observer requests; it does not prevent observers from receiving broadcast notifications. It is not a data redaction or untrusted-guest mode.

## Newly allowed operations

Reviewed against upstream revision `6b9826e3aa83b1a5947db50f4332cb9c65f1b340` (`rust-v0.154.0`), matching the generated AISuite protocol bindings.

| Request | Reason for observer access |
| --- | --- |
| `account/bedrock/discover` | Lists AWS profile names/regions and credential availability, not credential values; does not perform Bedrock setup. |
| `collaborationMode/list` | Reads available collaboration-mode presets. |
| `environment/status` | Observes an existing environment's status without forcing a connection. |
| `externalAgentConfig/import/readHistories` | Reads stored import history and imported connector candidates. |
| `plugin/search` | Searches plugin catalogs without installing or activating results. |
| `remoteControl/client/list` | Lists remote clients without revocation or enrollment. |
| `remoteControl/status/read` | Snapshots current remote-control status without enabling it. |
| `server/diagnostics` | Reads process and diagnostic gauge snapshots. |
| `thread/backgroundTerminals/list` | Lists terminals of an already-loaded thread; does not start or terminate them. |
| `thread/queue/list` | Reads queued submissions without starting, reordering or deleting them. |
| `thread/realtime/listVoices` | Reads the built-in voice catalog without starting realtime work. |
| `thread/search` | Searches stored conversations. |
| `thread/searchOccurrences` | Searches visible message occurrences in stored history. |
| `thread/timeline/list` | Pages stored timeline entries. |
| `userVerification/status` | Status-only request; this upstream build returns unavailable. The bridge preserves that error, not a fabricated success. |
| `windowsSandbox/readiness` | Reads readiness without running sandbox setup. |

The upstream implementation evidence is in `codex-rs/app-server/src/request_processors/` (thread, queue, turn, catalog, environment, plugin, account/Bedrock, remote-control, diagnostics and Windows sandbox processors), `external_agent_migration/processor.rs`, and `message_processor.rs`. Downstream checks include `aws-auth/src/discovery.rs`, `exec-server/src/environment.rs`, and `app-server-transport/src/transport/remote_control/`.

## Deliberately not observer operations

An operation is not safe merely because its name sounds like a read:

| Operation family | Why it remains controller-only |
| --- | --- |
| `fuzzyFileSearch` | A supplied cancellation token can cancel another outstanding search on the shared upstream connection. |
| `fuzzyFileSearch/sessionStart`, `/sessionUpdate`, `/sessionStop` | Creates, changes or stops shared search sessions. |
| `externalAgentConfig/detect` | Records detected connector candidates; it is not just a query. |
| `environment/info` | Calls `force_info()`, which can initiate connection work. |
| `remoteControl/pairing/status` | Recovery can replace server enrollment and change persisted pairing credentials. |
| Filesystem watches and MCP event subscriptions | Create or remove shared upstream subscriptions. |
| Thread start/resume/fork, turn/steering, review and realtime controls | Create, subscribe to or control execution and conversation state. |
| Project/section/thread mutations, queue edits/start and goal changes | Change authoritative state, membership, order or lifecycle. |
| Authentication, verification actions, remote-control setup/revocation | Change identity, credentials or access. |
| Configuration, memory reset, provider setup, plugin/marketplace writes and migration/import | Change configuration, storage or installed functionality. |
| Shell, command, process and MCP tool execution; approvals and elicitation controls | Execute work or change its authorization/lifecycle. |
| Feedback upload and account credit/email actions | External side effects. |
| Mock experimental operation | No observer product contract. |

`initialize` and `initialized` are bridge-owned, not controller permissions. There is no prefix, suffix, `/read`, `/list`, or `/status` heuristic.

## Version and verification contract

The app-server remains responsible for operation availability, experimental capabilities and parameter validation. The bridge forwards unsupported-method and capability errors unchanged. It does not substitute a legacy operation or retry a mutation as a read.

`BridgeRoutingTest.cpp` independently specifies the 50 approved reads and checks all 159 generated client requests with observer reads both enabled and disabled. Its catalog-size assertion forces a permission review when the generated request count changes. It also checks every generated server notification and server request. Payloads are deliberately opaque routing fixtures, not end-to-end app-server schema validation.

Regression coverage includes exact matching, forged envelope roles, concurrent equal request IDs, numeric versus string IDs, response/error isolation, controller transfer, provider readiness and write rejection, disconnect/reconnect, abandoned requests, stale responses, and generic controller forwarding of future methods. The existing frontend SDK and transport suites remain separate compatibility gates. No app-server mutation or desktop restart is required for these checks.

## Implementation record

Historical verification and change accounting are kept in the [implementation record](../records/observer-verification.md#page-overview). Follow the current user guide above for behavior; historical test commands are not an onboarding requirement.
