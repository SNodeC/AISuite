# Codex A1.4 integrations and long-tail current state

## Scope and authority

Native A1.4 is complete for the checked-in Codex App Server 0.144.6 stable
protocol. The vendored schemas define method direction, fields, requiredness,
nullability, numeric formats, unions, and object openness. The private
`ProtocolSurfaceRegistry` remains the sole production implementation-state
authority.

AISuite validates current source, compiled behavior, wire behavior, and
compatibility with the current installed SNode.C package.

## Exact native A1.4 surface

The 56 native A1.4 identities comprise:

- 29 client requests;
- 16 server notifications;
- four server requests; and
- seven tagged-union alternatives.

They are delivered in three cohesive functional groups:

| Group | Identities | Taxonomy | Stable schema closure |
| --- | ---: | --- | --- |
| User-facing integrations | 33 | 23 requests, six notifications, four `PluginSource` alternatives | 52 seeds / 118 v2 definitions / 411 paths |
| MCP and reverse requests | 13 | four requests, two notifications, four server requests, three elicitation alternatives | 18 seeds / 55 definitions / 204 paths |
| Runtime and platform | 10 | two requests, eight notifications | 11 seeds / 17 v2 definitions / 31 paths |

The first group owns apps, external agents, feedback, hooks, marketplace,
plugins, and skills. The second owns MCP operations, typed reverse requests,
and the `McpServerElicitationRequestParams::mode` union. The final group owns
Windows sandbox operations and runtime/platform observation.

## Runtime and platform completion

The final ten identities are:

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

The two requests are exposed through the cross-platform
`client.typed().windowsSandbox()` facade:

```cpp
client.typed().windowsSandbox().checkReadiness(readinessHandler);

ai::openai::codex::typed::WindowsSandboxSetupStartParams params;
params.mode =
    ai::openai::codex::typed::WindowsSandboxSetupMode::unelevated();
client.typed().windowsSandbox().startSetup(std::move(params), setupHandler);
```

`checkReadiness` constructs the wire-level `Unit` parameter internally.
`startSetup` accepts the complete `WindowsSandboxSetupStartParams`. Both return
`RawProtocol::Submission` immediately and complete asynchronously through
`OperationResult<T>` on the existing event-loop path.

AISuite does not implement a readiness probe, setup installer, elevation,
PowerShell command, filesystem/security mutation, or other local Windows
behavior. The protocol types are available on Linux, macOS, and Windows.

The eight notifications use the existing canonical decoder and `Events`
observer. `process/exited` and `process/outputDelta` do not add outgoing process
control. `remoteControl/status/changed` does not add outgoing remote control.
Warnings, deprecations, process data, remote status, and Windows diagnostics do
not add backend presentation policy or frontend UI state.

See the
[runtime and platform long-tail report](a1-4-runtime-and-platform-long-tail.md)
for complete field, lifecycle, variant, and security details.

## MCP and reverse requests

The `client.typed().mcp()` facade provides typed asynchronous OAuth login,
resource read, tool call, and server-status list operations. Four incoming
requests use the established occurrence lifecycle:

```text
attestation/generate
item/tool/call
item/tool/requestUserInput
mcpServer/elicitation/request
```

Tool user input has no `mode` discriminator. The `form`, `openai/form`, and
`url` alternatives belong only to `McpServerElicitationRequestParams::mode`,
followed by a raw-preserving future-unknown alternative.

The direct response path retains the original request ID, transport generation,
and occurrence token. Successful response enqueue retires an occurrence once;
disconnect and shutdown clean up remaining occurrences without adding a second
registry, timer, or response transport.

## `serverRequest/resolved`

The exact payload is:

```text
ServerRequestResolvedNotification {
    threadId: string
    requestId: string | int64
}
```

Both fields are required and non-nullable, and the object is open. There is no
method field. Correlation starts with the exact request ID in the transport
generation that delivered the notification. The existing occurrence supplies
its stored thread ID and registered request target. The occurrence retires
externally only when the thread matches and the stored target is one of:

```text
item/commandExecution/requestApproval
item/fileChange/requestApproval
item/permissions/requestApproval
item/tool/requestUserInput
mcpServer/elicitation/request
```

The negative target set is:

```text
applyPatchApproval
execCommandApproval
item/tool/call
attestation/generate
```

No method is inferred from notification data. Unknown, stale, duplicate,
already-retired, wrong-thread, reconnect-reused, and negative-target cases are
nonfatal occurrence-state no-ops, while the typed event and raw notification
are still delivered in that order.

External resolution is reconciled before the typed callback. A later local
response therefore fails locally without wire output. Conversely, a direct
response remains independently usable without ever receiving the notification;
a later resolution notification does not cause another transition or response.

## Public variants and ABI boundary

Every predecessor index is preserved. Runtime/platform notifications append at
canonical indices 59 through 66 and `Event` indices 61 through 68:

| Notification | Canonical | Event |
| --- | ---: | ---: |
| `deprecationNotice` | 59 | 61 |
| `process/exited` | 60 | 62 |
| `process/outputDelta` | 61 | 63 |
| `remoteControl/status/changed` | 62 | 64 |
| `serverRequest/resolved` | 63 | 65 |
| `warning` | 64 | 66 |
| `windows/worldWritableWarning` | 65 | 67 |
| `windowsSandbox/setupCompleted` | 66 | 68 |

`CanonicalServerNotification` now has 67 alternatives and `Event` has 69.
`TypedServerRequest` remains unchanged with 11 alternatives:
`UserInputRequest` remains index 2, `UnknownServerRequest` index 4,
`AttestationGenerateRequest` index 8, `DynamicToolCallRequest` index 9, and
`McpServerElicitationRequest` index 10. Canonical notification index 67 remains
unconsumed for the Common `error` completion.

The installed Codex header inventory is 29 main-library headers, seven backend
headers, and seven frontend headers, 43 total. `typed/WindowsSandbox.h` is the
one runtime/platform addition. `typed::Client` remains one-pointer PIMPL-backed,
and `AppServerClient` retains its existing PIMPL. Codex SOVERSION remains 1;
the final SOVERSION decision remains deferred to final-A1 completion.

## Current registry state

| Scope | Complete | Partial | Not implemented | Not applicable |
| --- | ---: | ---: | ---: | ---: |
| Global | 336 | 3 | 0 | 48 |
| Native A1.4 | 56 | 0 | 0 | — |

Native A1.4 is complete. The remaining Partial identities are `initialize`,
`initialized`, and `error`, owned by Common/A1.0 and deferred to final-A1
completion. The 36 experimental-only and 12 stable-but-unreachable
InventoryOnly identities remain NotApplicable.

## Architecture, diagnostics, and non-goals

All A1.4 client operations reuse one `RawProtocol`, JSONL/JSON-RPC engine,
request-ID allocator, pending-operation map, event-loop callback path, and
shutdown path. All reverse requests reuse one occurrence registry and
transport generation. All notifications reuse one dispatcher and typed/raw
observer path. Typed callbacks precede raw callbacks and may submit another
typed request reentrantly.

Malformed known data produces structural diagnostics and preserves raw data
without disconnecting solely for typed decode failure. Diagnostics may include
the method or type, safe structural path, expected type, and safe counts, but
never sensitive values. Process output and handles, IDs, warning and
deprecation text, paths, Windows diagnostics, remote identity/status, MCP data,
answers, attestation data, raw envelopes, and opaque extension values are
sensitive.

A1.4 adds no blocking call, polling loop, sleep, hidden worker thread,
future/coroutine lifecycle, second transport, second pending map, second
occurrence registry, process-control facade, remote-control facade, local
Windows setup, backend product state, frontend product protocol, experimental
InventoryOnly operation, protocol-version upgrade, or SOVERSION bump.
