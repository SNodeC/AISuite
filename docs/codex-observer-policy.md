# Codex bridge observer policy

## Boundary and compatibility

The bridge owns the upstream initialization handshake and routes one initialized
app-server connection to multiple frontends. The registered frontend role, not
an envelope's claimed role, controls access. Controllers keep generic forwarding;
observers may issue only exact-match approved requests when `observersMayRead`
is enabled. Unknown methods, frontend notifications, and responses to another
frontend's server request are not observer permissions.

This extension preserves all 34 established observer methods and adds 16 reads.
No wire format, generated binding, request-ID translation, controller-transfer
rule, notification fan-out, transport, or frontend API changes. Nothing requires
an update to an existing CodexUI client. New bridge permissions do not add UI
controls or override a frontend's own command policy.

The observer trust model remains the existing one: observers may see account,
configuration, thread and workspace information, including the previously allowed
filesystem reads. `observersMayRead=false` disables observer requests; it does
not prevent observers from receiving broadcast notifications. It is not a data
redaction or untrusted-guest mode.

## Newly allowed operations

Reviewed against upstream revision `6b9826e3aa83b1a5947db50f4332cb9c65f1b340`
(`rust-v0.154.0`), matching the generated AISuite protocol bindings.

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

The upstream implementation evidence is in
`codex-rs/app-server/src/request_processors/` (thread, queue, turn, catalog,
environment, plugin, account/Bedrock, remote-control, diagnostics and Windows
sandbox processors), `external_agent_migration/processor.rs`, and
`message_processor.rs`. Downstream checks include `aws-auth/src/discovery.rs`,
`exec-server/src/environment.rs`, and
`app-server-transport/src/transport/remote_control/`.

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

`initialize` and `initialized` are bridge-owned, not controller permissions.
There is no prefix, suffix, `/read`, `/list`, or `/status` heuristic.

## Version and verification contract

The app-server remains responsible for operation availability, experimental
capabilities and parameter validation. The bridge forwards unsupported-method
and capability errors unchanged. It does not substitute a legacy operation or
retry a mutation as a read.

`BridgeRoutingTest.cpp` independently specifies the 50 approved reads and checks
all 159 generated client requests with observer reads both enabled and disabled.
Its catalog-size assertion forces a permission review when the generated request
count changes. It also checks every generated server notification and server
request. Payloads are deliberately opaque routing fixtures, not end-to-end
app-server schema validation.

Regression coverage includes exact matching, forged envelope roles, concurrent
equal request IDs, numeric versus string IDs, response/error isolation, controller
transfer, provider readiness and write rejection, disconnect/reconnect, abandoned
requests, stale responses, and generic controller forwarding of future methods.
The existing frontend SDK and transport suites remain separate compatibility
gates. No app-server mutation or desktop restart is required for these checks.

## Local verification of this extension

Before extending the production table, the expanded routing suite rejected all
16 new reads as expected. After the extension, all 159 request classifications,
81 notification methods and 11 server-request methods passed. The existing
34 observer permissions and the controller path are retained.

Commands (2026-09-26):

```sh
cmake --build /tmp/aisuite-project-routing.0Ed8Iw --parallel 14
ctest --test-dir /tmp/aisuite-project-routing.0Ed8Iw \
  -E '^CodexRealAppServer_' --output-on-failure --parallel 14
xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest \
  --test-dir /home/voc/projects/drafts/CodexUI/build/Desktop_GCC-Debug \
  -R '^(codexui-client-runtime-dispatch|codexui-shell-integration)$' \
  --output-on-failure --parallel 14
git diff --check
```

Results: full local AISuite build passed with warnings treated as errors;
24/24 selected AISuite checks passed; both existing local CodexUI checks passed.
The latter use their own fixtures/mock bridge, not the changed bridge connected
to the user's desktop session. Dedicated `CodexRealAppServer_*` transport cases
were excluded. Individual new operations were source-reviewed, not executed
against a live authenticated app-server. Server availability and capability
gates therefore remain separate from the verified routing policy.

The selected AISuite suite took 1.26 seconds before the change and 1.23 seconds
after it. This is a suite runtime observation, not a throughput benchmark or UI
smoothness proof. The observer lookup remains a bounded, allocation-free scan
of static entries (50 rather than 34); controllers and provider notifications
do not execute that lookup. No queues, timers, caches or callbacks were added.

Accounting: production +16/-0 lines; tests +243/-35 lines (net +208).
No CodexUI files, installed binaries or running user processes were changed.
