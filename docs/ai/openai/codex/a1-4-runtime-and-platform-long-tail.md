# Codex A1.4 runtime and platform long tail

Native A1.4 is complete. This slice adds exactly two typed client requests and
eight typed server notifications from the checked-in Codex App Server 0.144.6
stable protocol:

```text
windowsSandbox/readiness
windowsSandbox/setupStart
deprecationNotice
process/exited
process/outputDelta
remoteControl/status/changed
serverRequest/resolved
warning
windows/worldWritableWarning
windowsSandbox/setupCompleted
```

The vendored schemas and protocol sources remain the protocol authority.
AISuite builds against the current installed SNode.C package and validates the
current software state. No SNode.C source checkout is required by AISuite
tests.

## Windows sandbox client API

Include `<ai/openai/codex/typed/WindowsSandbox.h>` and use the PIMPL-backed
facade returned by `client.typed().windowsSandbox()`:

```cpp
#include <ai/openai/codex/AppServerClient.h>
#include <ai/openai/codex/typed/Client.h>
#include <ai/openai/codex/typed/WindowsSandbox.h>

#include <utility>

void inspectAndConfigure(ai::openai::codex::AppServerClient& client) {
    namespace typed = ai::openai::codex::typed;

    client.typed().windowsSandbox().checkReadiness(
        [](const typed::WindowsSandbox::CheckReadinessResult& result) {
            // Handle the typed asynchronous readiness result.
            (void)result;
        });

    typed::WindowsSandboxSetupStartParams params;
    params.mode = typed::WindowsSandboxSetupMode::unelevated();
    client.typed().windowsSandbox().startSetup(
        std::move(params),
        [](const typed::WindowsSandbox::StartSetupResult& result) {
            // Handle the typed asynchronous setup-start result.
            (void)result;
        });
}
```

The methods and contracts are:

| Public method | Wire method | Parameters | Result |
| --- | --- | --- | --- |
| `checkReadiness(handler)` | `windowsSandbox/readiness` | `Unit` internally | `WindowsSandboxReadinessResponse` |
| `startSetup(params, handler)` | `windowsSandbox/setupStart` | `WindowsSandboxSetupStartParams` | `WindowsSandboxSetupStartResponse` |

Both methods return `RawProtocol::Submission` immediately and complete through
an asynchronous `OperationResult<T>` callback on the existing SNode.C event
loop. `checkReadiness` hides the protocol's `Unit` parameter from application
code. `startSetup` preserves the required `WindowsSandboxSetupMode` and the
omitted/null/value states of its optional `cwd`.

These are cross-platform protocol calls to Codex App Server. The public types
and methods are available on Linux, macOS, and Windows. AISuite does not run
PowerShell, elevate a process, inspect local readiness, change filesystem or
security settings, install a service, or implement Windows sandbox setup.

## Runtime and platform events

The eight new notifications use the existing canonical notification decoder,
`Event` variant, typed observer, and raw observer. Their public types are:

| Wire notification | Typed event value |
| --- | --- |
| `deprecationNotice` | `DeprecationNoticeNotification` |
| `process/exited` | `ProcessExitedNotification` |
| `process/outputDelta` | `ProcessOutputDeltaNotification` |
| `remoteControl/status/changed` | `RemoteControlStatusChangedNotification` |
| `serverRequest/resolved` | `ServerRequestResolvedNotification` |
| `warning` | `WarningNotification` |
| `windows/worldWritableWarning` | `WindowsWorldWritableWarningNotification` |
| `windowsSandbox/setupCompleted` | `WindowsSandboxSetupCompletedNotification` |

Applications observe them without inspecting raw JSON:

```cpp
#include <ai/openai/codex/AppServerClient.h>
#include <ai/openai/codex/typed/Client.h>
#include <ai/openai/codex/typed/Events.h>

#include <variant>

void observeProcessExit(ai::openai::codex::AppServerClient& client) {
    namespace typed = ai::openai::codex::typed;
    client.typed().events().setOnEvent(
        [](const typed::Event& event) {
            if (const auto* exited =
                    std::get_if<typed::ProcessExitedNotification>(&event)) {
                // Handle the typed process-exit event.
                (void)exited;
            }
        });
}
```

`process/exited` and `process/outputDelta` report App Server-owned processes;
they do not add a local process-control API. Likewise,
`remoteControl/status/changed` adds observation only, with no outgoing
remote-control facade. Warning, deprecation, remote-control, process, and
Windows payloads are protocol data rather than presentation or UI policy.

Known malformed payloads remain visible to the raw-notification observer with
safe structural diagnostics, and do not disconnect the transport merely
because typed decoding failed. Open objects retain future fields in `raw`, and
unknown open-enum values remain typed with forward-compatibility diagnostics.
Typed delivery still precedes raw delivery; callback exceptions are contained,
and callbacks may submit another typed request reentrantly.

## `serverRequest/resolved`

The exact public payload is:

```cpp
struct ServerRequestResolvedNotification {
    ServerRequestId requestId; // string or int64
    ThreadId threadId;
    Json raw;
    std::vector<DecodeDiagnostic> diagnostics;
};
```

`threadId` and `requestId` are required and non-nullable. The object is open to
future properties. There is deliberately no method, item ID, call ID, result,
outcome, error, occurrence token, or transport-generation field. Integer and
string JSON-RPC request IDs are preserved exactly.

The existing occurrence registry supplies the missing correlation metadata.
Before the typed callback is delivered, the raw protocol:

1. binds the notification to the transport generation that received it;
2. looks up the current-generation occurrence by the exact original request
   ID;
3. reads that occurrence's stored thread ID, registered request target,
   generation, and pending state;
4. requires the stored thread ID to equal the notification thread ID; and
5. retires the occurrence only when its stored request target is in the
   positive matrix below.

No method is inferred from notification data, request-ID shape, thread ID,
item ID, or call ID. No second registry or lifecycle is involved.

| Request target | May emit `serverRequest/resolved` |
| --- | :---: |
| `item/commandExecution/requestApproval` | yes |
| `item/fileChange/requestApproval` | yes |
| `item/permissions/requestApproval` | yes |
| `item/tool/requestUserInput` | yes |
| `mcpServer/elicitation/request` | yes |
| `applyPatchApproval` | no |
| `execCommandApproval` | no |
| `item/tool/call` | no |
| `attestation/generate` | no |

A matching positive occurrence is retired at most once as externally
resolved. An application callback therefore cannot send a stale response for
an occurrence that another subscriber already resolved. Unknown IDs,
wrong-thread IDs, stale generations, reconnect ID reuse, duplicate
notifications, already-retired occurrences, and negative targets are nonfatal
registry-state no-ops. The typed event and then the raw notification remain
observable in every structurally valid case.

The notification never sends or acknowledges a JSON-RPC response. A direct
response remains independently usable without receiving this notification.
If the application responds first, a later resolution event changes no state;
if external resolution arrives first, a later local `respond()` or `reject()`
fails locally and emits no wire response. Disconnect does not synthesize the
notification, and no timeout or `autoResolutionMs` behavior is added.

## Append-only variants

Every predecessor index is preserved. The new alternatives occupy:

| Notification | `CanonicalServerNotification` | `Event` |
| --- | ---: | ---: |
| `deprecationNotice` | 59 | 61 |
| `process/exited` | 60 | 62 |
| `process/outputDelta` | 61 | 63 |
| `remoteControl/status/changed` | 62 | 64 |
| `serverRequest/resolved` | 63 | 65 |
| `warning` | 64 | 66 |
| `windows/worldWritableWarning` | 65 | 67 |
| `windowsSandbox/setupCompleted` | 66 | 68 |

The final sizes are 67 canonical notifications and 69 typed events.
`TypedServerRequest` remains unchanged at 11 alternatives, including
`UserInputRequest` at index 2, `UnknownServerRequest` at 4,
`AttestationGenerateRequest` at 8, `DynamicToolCallRequest` at 9, and
`McpServerElicitationRequest` at 10. Canonical notification index 67 remains
available for the later Common `error` completion.

## Schema completeness and current state

The current-state schema check derives exactly 11 seed definitions, 17
reachable v2 definitions, and 31 schema paths. Its 30 properties contain 25
required and five optional paths, five nullable paths, no default-bearing
paths, one array path, no map paths, 11 open objects, no closed objects, and no
intentionally opaque paths. Numeric formats are one `int32` and one `uint`,
with one minimum and no maximum constraint.

The final production registry is:

| Scope | Complete | Partial | Not implemented | Not applicable |
| --- | ---: | ---: | ---: | ---: |
| Global | 336 | 3 | 0 | 48 |
| Native A1.4 | 56 | 0 | 0 | — |

Native A1.4 is complete. The remaining Partial identities are `initialize`,
`initialized`, and `error`, owned by Common/A1.0 and deferred to final-A1
completion. All 48 InventoryOnly identities remain NotApplicable. Final A1b
subsequently moves all three Codex libraries to SOVERSION 2.

The installed Codex public-header inventory is 29 main-library headers, seven
backend headers, and seven frontend headers: 43 total. The only added header in
this slice is `typed/WindowsSandbox.h`.

## Architecture and security boundary

The implementation retains one non-blocking SNode.C transport, JSONL/JSON-RPC
engine, client request-ID allocator, pending-operation map, server-request
occurrence registry, transport generation, notification dispatcher, observer
path, callback scheduler, and shutdown path. It adds no blocking wait, polling
loop, sleep, worker-thread default, future/coroutine lifecycle, process-control
facade, remote-control facade, local Windows behavior, backend state, or
frontend protocol feature.

Process output and handles, request and thread IDs, warning and deprecation
text, paths, Windows diagnostics, remote-control identity/status fields, raw
envelopes, and opaque future fields are sensitive. Diagnostics contain only a
protocol type or method, structural path, expected type, and safe counts; they
do not contain payload values.
