# Codex A1.4 MCP and reverse requests

**A14-McpReverse complete. Native A1.4 has ten runtime/platform identities
remaining.**

This milestone types the frozen MCP and reverse-request surface from Codex CLI
0.144.6 (`rust-v0.144.6`, source
`5d1fbf26c43abc65a203928b2e31561cb039e06d`). It completes exactly 13
registry identities: four client requests, two server notifications, four
server requests, and three tagged-union alternatives.

Normal AISuite compilation and linking use the cleaned SNode.C commit
`77415c71a87fb7955e9a050bedaca02b65754324`, tree
`2d39c334f12c308828936656c820447bfcc38d47`. The historical SNode.C commit
`d18b231a1d2ec2235fd6f204786b0a761cc24ff5`, tree
`88a63edc985a851b2b76b0c56df19fae74ea8069`, remains read-only extraction
provenance and is neither built nor linked.

## Exact scope

The four typed client requests are:

| Wire method | Public facade method | Result |
| --- | --- | --- |
| `mcpServer/oauth/login` | `mcp().startOauthLogin` | `McpServerOauthLoginResponse` |
| `mcpServer/resource/read` | `mcp().readResource` | `McpResourceReadResponse` |
| `mcpServer/tool/call` | `mcp().callTool` | `McpServerToolCallResponse` |
| `mcpServerStatus/list` | `mcp().listServers` | `ListMcpServerStatusResponse` |

The two typed notifications are
`mcpServer/oauthLogin/completed` and
`mcpServer/startupStatus/updated`.

The four typed reverse requests are:

| Wire method | Public request | Canonical response |
| --- | --- | --- |
| `attestation/generate` | `AttestationGenerateRequest` | `AttestationGenerateResponse` |
| `item/tool/call` | `DynamicToolCallRequest` | `DynamicToolCallResponse` |
| `item/tool/requestUserInput` | `UserInputRequest` | `ToolRequestUserInputResponse` |
| `mcpServer/elicitation/request` | `McpServerElicitationRequest` | `McpServerElicitationRequestResponse` |

The final three identities are the `form`, `openai/form`, and `url`
alternatives of `McpServerElicitationRequestParams::mode`. They do not belong
to tool user input.

## MCP facade

Include the installed `ai/openai/codex/typed/Mcp.h` header. Each method takes
the complete parameter object, submits through the existing `RawProtocol`,
returns a `RawProtocol::Submission` immediately, and later delivers an
`OperationResult<T>` on the SNode.C event loop:

```cpp
#include <ai/openai/codex/AppServerClient.h>
#include <ai/openai/codex/typed/Client.h>
#include <ai/openai/codex/typed/Mcp.h>

#include <utility>

void listMcpServers(ai::openai::codex::AppServerClient& client) {
    ai::openai::codex::typed::ListMcpServerStatusParams params;
    client.typed().mcp().listServers(
        std::move(params),
        [](const auto& result) {
            // Handle the typed asynchronous result.
            (void)result;
        });
}
```

Applications do not provide JSON-RPC method names or IDs, inspect pending
maps, decode JSON, or manage transport generations. The facade performs no
external MCP HTTP request, OAuth flow, MCP server execution, or attestation
policy locally; the Codex App Server owns those operations.

## Reverse requests and responses

Reverse requests use the established
`client.typed().requests().setOnRequest()` observer and the same occurrence
registry as the five approval requests. Each typed request contains its
original JSON-RPC ID, connection generation-bound occurrence token, canonical
parameters, raw forward-compatibility value, and safe structural diagnostics.

The canonical response overloads are:

```text
respond(AttestationGenerateRequest, AttestationGenerateResponse)
respond(DynamicToolCallRequest, DynamicToolCallResponse)
respond(UserInputRequest, ToolRequestUserInputResponse)
respond(McpServerElicitationRequest, McpServerElicitationRequestResponse)
```

Each request type also has a typed `reject(request, ProtocolError)` overload.
The existing
`respond(UserInputRequest, std::vector<UserInputAnswer>)` convenience overload
is retained and forwards through the canonical user-input response.

For example, an application can decline elicitation without constructing a
wire action string or fabricating content:

```cpp
#include <ai/openai/codex/AppServerClient.h>
#include <ai/openai/codex/typed/Client.h>
#include <ai/openai/codex/typed/ServerRequests.h>

#include <utility>
#include <variant>

void declineElicitation(ai::openai::codex::AppServerClient& client) {
    namespace typed = ai::openai::codex::typed;
    client.typed().requests().setOnRequest(
        [&client](const typed::TypedServerRequest& request) {
            const auto* elicitation =
                std::get_if<typed::McpServerElicitationRequest>(&request);
            if (elicitation == nullptr) {
                return;
            }
            typed::McpServerElicitationRequestResponse response;
            response.action = typed::McpServerElicitationAction::decline();
            client.typed().requests().respond(*elicitation, std::move(response));
        });
}
```

`accept()`, `decline()`, and `cancel()` are the known elicitation action
factories. `content` and `_meta` retain omitted, explicit-null, and exact JSON
value states. Decline and cancel do not synthesize content. Elicitation
messages and content are excluded from diagnostics.

The first successfully enqueued response retires an occurrence exactly once.
Duplicate, stale-token, stale-generation, and wrong-occurrence attempts are
rejected. A failed local validation or enqueue does not consume the request,
so the application can correct and retry it. Disconnect and explicit shutdown
retire outstanding occurrences; reconnect reuse of a JSON-RPC ID cannot target
an older occurrence.

## Tool user input is not MCP elicitation

`item/tool/requestUserInput` has no `mode` discriminator. Its canonical
`ToolRequestUserInputParams` preserves:

- required thread, turn, item, and ordered question data;
- default-bearing `isOther` and `isSecret`;
- omitted, explicit-null, and empty `options`;
- omitted, explicit-null, and numeric `autoResolutionMs`; and
- the response map from question IDs to ordered answer arrays.

The application-friendly `UserInputRequest`, `UserInputQuestion`,
`UserInputOption`, and `UserInputAnswer` projections remain available. Secret
answers, questions, and answer-map keys are not emitted in diagnostics.
`autoResolutionMs` is protocol data and does not create an AISuite timer.

MCP elicitation is a separate open tagged union owned only by
`McpServerElicitationRequestParams`:

| Index | Alternative | Mode |
| ---: | --- | --- |
| 0 | `McpElicitationForm` | `form` |
| 1 | `McpElicitationOpenAiForm` | `openai/form` |
| 2 | `McpElicitationUrl` | `url` |
| 3 | `UnknownMcpElicitation` | future string |

The form branch uses the complete stable requested-schema graph.
`openai/form` retains its opaque requested schema, including JSON null. The
URL branch retains its elicitation ID and URL. All three branches retain
optional/nullable opaque `_meta` and future properties.

An unknown future mode preserves its mode and raw JSON with a nonfatal
`ForwardCompatibility` diagnostic. A known mode with missing or wrong-typed
required fields is malformed-known: it remains answerable through the
raw-preserving request path, receives a safe structural diagnostic, is not
misclassified as a future mode, and does not disconnect the transport.

## Variants, events, and concurrency

All existing public alternative indices are preserved:

| Variant | Appended alternatives |
| --- | --- |
| `CanonicalServerNotification` | 57 OAuth completion, 58 startup-status update; final size 59 |
| `Event` | 59 OAuth completion, 60 startup-status update; final size 61 |
| `TypedServerRequest` | 8 attestation, 9 dynamic tool, 10 MCP elicitation; final size 11 |

`UserInputRequest` remains `TypedServerRequest` index 2 and
`UnknownServerRequest` remains index 4. Notifications use the existing
decoder and observer mechanism, with typed delivery before raw delivery.
Callback exceptions remain contained, and callbacks may submit another typed
request reentrantly.

The focused real-stdio lifecycle case holds all five approvals and all four
new reverse requests pending simultaneously. It covers nine distinct
occurrence tokens, integer and string IDs, out-of-order terminal responses,
wrong-occurrence isolation, duplicate and stale rejection, reconnect ID reuse,
callback exceptions, reentrant MCP submission, disconnect cleanup, shutdown
cleanup, and return to an empty occurrence registry. It uses SNode.C-managed
non-blocking descriptors and the EventLoop; it adds no polling loop or sleep.

## Closure and deferrals

The frozen schema closure is exactly 18 seed definitions, 55 reachable
definitions (34 legacy and 21 v2), and 204 schema paths. The final production
registry is:

| Scope | Complete | Partial | Not implemented | Not applicable |
| --- | ---: | ---: | ---: | ---: |
| Global | 326 | 3 | 10 | 48 |
| Native A1.4 | 46 | 0 | 10 | — |

The three global Partial identities remain exactly `initialize`,
`initialized`, and `error`; they stay Common/A1.0-owned until the distinct
final-A1 closure. All 48 InventoryOnly identities remain NotApplicable.

PR C remains untouched:
`windowsSandbox/readiness`, `windowsSandbox/setupStart`,
`deprecationNotice`, `process/exited`, `process/outputDelta`,
`remoteControl/status/changed`, `serverRequest/resolved`, `warning`,
`windows/worldWritableWarning`, and `windowsSandbox/setupCompleted`.
Codex SOVERSION remains 1 and its final decision remains deferred to the
separate final-A1 closure.

Three bounded exhaustive-visitor adaptations preserve backend compile
compatibility without adding product behavior: `BackendCore` leaves the three
new request wrappers with typed `Requests`, the reducer emits no backend event
for either MCP notification, and snapshots use the existing `unknown`
fallback. They add no backend state, command, event type, or frontend surface.

The implementation retains one transport, JSONL engine, JSON-RPC ID
allocator, client pending map, occurrence registry, transport generation,
cancellation path, notification dispatcher, observer mechanism, and callback
scheduler. No synchronous wait, polling loop, sleep, hidden worker-thread
default, future/coroutine lifecycle, backend feature, or frontend protocol
feature is introduced.
