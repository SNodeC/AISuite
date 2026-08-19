# Codex application API

AISuite provides a typed, asynchronous C++ client for the Codex App Server.
Applications use direct domain accessors on `AppServerClient`; raw JSON-RPC is
an explicit escape hatch.

## Include and link target

The umbrella header exposes the complete ordinary application API:

```cpp
#include <ai/openai/codex/Api.h>
#include <core/SNodeC.h>
```

Link the provider library and the SNode.C core API used by the lifecycle
example through their installed CMake targets:

```cmake
find_package(AISuite CONFIG REQUIRED)

target_link_libraries(my_app PRIVATE
    AISuite::OpenAICodex
    snodec::core
)
```

Narrow consumers may continue to include individual public headers. The normal
application API is available through exactly 20 direct domains:

```cpp
client.accounts();
client.apps();
client.commands();
client.configuration();
client.events();
client.externalAgents();
client.feedback();
client.filesystem();
client.hooks();
client.marketplace();
client.mcp();
client.models();
client.permissionProfiles();
client.plugins();
client.requests();
client.reviews();
client.skills();
client.threads();
client.turns();
client.windowsSandbox();
```

These accessors return stable, client-owned façade objects. The façades cannot
be copied or moved. Each delegates to the client's one protocol engine and
shares its event-loop lifecycle.

## Construct the client

The common stdio client starts Codex App Server when `start()` is called:

```cpp
namespace codex = ai::openai::codex;

codex::stdio::Client client;
```

The default initialization identity is `aisuite`, title `AISuite`, and the
current AISuite project version. Applications may still supply `ClientInfo` or
the complete `typed::InitializeParams` when they need an explicit identity or
capabilities.

## Lifecycle

The client is non-blocking. `start()` schedules transport startup and the
automatic `initialize`/`initialized` handshake. Typed and raw application
operations must be submitted after the client reaches `Ready`; an earlier
submission fails immediately with a local `InvalidState` error. Structurally
valid incoming non-initialization messages received during initialization may
be retained by the bounded pre-ready input queue, but outgoing application
requests are not queued before `Ready`.

```cpp
client.setOnStateChanged([&](const codex::StateChange& change) {
    if (change.current == codex::State::Ready) {
        // Typed operations may now be submitted.
    } else if (change.current == codex::State::Stopped) {
        core::SNodeC::stop();
    }
});

client.setOnDiagnostic([](const codex::Diagnostic& diagnostic) {
    // Diagnostics are structural and exclude sensitive payload values.
});

client.start();
```

`getState()`, `isReady()`, and `getInitializeResponse()` provide current
lifecycle and handshake metadata. Call `stop()` for orderly asynchronous
shutdown. AISuite does not add blocking waits, futures, coroutines, polling, or
hidden worker threads.

## Events

Register one replaceable typed event handler:

```cpp
namespace typed = codex::typed;

client.events().setOnEvent([](const typed::Event& event) {
    std::visit([](const auto& value) {
        // Handle the typed application event.
    }, event);
});
```

`typed::Event` preserves the established application projections, including
`ThreadStarted`, `TurnCompleted`, `ItemCompleted`, delta events,
`ModelRerouted`, and `TurnErrorEvent`. The complete canonical notification is
available where a projection enriches or narrows the wire aggregate. Unknown
and malformed-known input remains observable through raw payloads and
structured diagnostics.

## Reverse requests and approvals

Reverse requests use the client's one occurrence registry. A response or
rejection is accepted locally at most once and returns the shared `SendResult`.

```cpp
client.requests().setOnRequest(
    [&](const typed::TypedServerRequest& request) {
        if (const auto* approval =
                std::get_if<typed::CommandApprovalRequest>(&request)) {
            const codex::SendResult sent = client.requests().respond(
                *approval,
                typed::ApprovalDecision::decline());
            if (!sent) {
                // Inspect sent.error.
            }
        }
    });
```

The application-friendly projections remain `CommandApprovalRequest`,
`FileChangeApprovalRequest`, `UserInputRequest`, `AuthenticationRequest`, and
their response helpers. Schema-complete token refresh responses use the
`respond(const AuthenticationRequest&,
ChatgptAuthTokensRefreshResponse)` overload. Raw response/rejection remains
available for genuinely unknown future requests.

## Threads

Canonical parameter objects preserve every stable schema field:

```cpp
typed::ThreadStartParams params;
params.cwd = std::string{"/synthetic/project"};

codex::Submission submission = client.threads().start(
    std::move(params),
    [](const typed::OperationResult<typed::ThreadStartResponse>& result) {
        if (result) {
            const typed::ThreadId& id = result->thread.id;
            (void) id;
        }
    });
```

For common cases, bounded conveniences delegate to those canonical params:

```cpp
const auto handler = [](const auto&) {};
const typed::ThreadId threadId{"thread-synthetic"};

client.threads().start(typed::AbsolutePath{"/synthetic/project"}, handler);
client.threads().start(handler); // all optional fields omitted
client.threads().resume(threadId, handler);
client.threads().list(handler);  // all optional fields omitted
client.threads().read(threadId, handler);
```

Advanced callers retain the complete `ThreadStartParams`,
`ThreadResumeParams`, `ThreadListParams`, and `ThreadReadParams` overloads.

## Turns

Turn input uses the concise canonical vocabulary `TextInput`, `ImageUrlInput`,
`LocalImageInput`, `SkillInput`, `MentionInput`, `UnknownTurnInput`, and
`TurnInput`.

```cpp
const typed::ThreadId threadId{"thread-synthetic"};
const typed::TurnId turnId{"turn-synthetic"};

client.turns().start(
    threadId,
    std::string{"Hello"},
    [](const typed::OperationResult<typed::TurnStartResponse>& result) {
        if (!result) {
            return;
        }
        const typed::TurnId& turnId = result->turn.id;
        (void) turnId;
    });

client.turns().interrupt(threadId, turnId, [](const auto&) {});
```

The vector convenience accepts `std::vector<typed::TurnInput>`. Canonical
`TurnStartParams`, `TurnInterruptParams`, and `TurnSteerParams` overloads remain
available without an incomplete options hierarchy.

## Result handling

All typed operations return `codex::Submission` immediately and complete with
`typed::OperationResult<T>` through `typed::CompletionHandler<T>`.
Parameterless Unit-result operations use `typed::DoneHandler`.

```cpp
client.models().list(
    [](const typed::OperationResult<typed::ModelListResponse>& result) {
        if (!result) {
            if (result.isRemoteError()) {
                // Inspect result.remoteError.
            } else if (result.isLocalError() || result.isCancelled()) {
                // Inspect result.localError.
            }
            return;
        }

        const auto& models = result->data;
        (void) models;
    });
```

`OperationResult<T>` distinguishes Success, RemoteError, Cancelled, and
LocalError. It retains the decoded value, request ID, raw result, protocol
error, local error, and structured Codex error information. `operator bool()`
and `isSuccess()` both require a successful kind and a decoded value.
`operator*` and `operator->` may be used only when that condition holds; they
are unchecked optional-like accessors and do not manufacture a missing value.

## Other domains

The same callback pattern applies throughout the API. Representative compact
calls are:

```cpp
const auto handler = [](const auto&) {};

client.accounts().read(handler);
client.accounts().logout(handler);
client.apps().list(handler);
client.configuration().read(handler);
client.externalAgents().detect(handler);
client.hooks().list(handler);
client.marketplace().upgrade(handler);
client.mcp().listServers(handler);
client.models().readProviderCapabilities(handler);
client.permissionProfiles().list(handler);
client.plugins().installed(handler);
client.plugins().list(handler);
client.plugins().shareList(handler);
client.skills().list(handler);
client.windowsSandbox().checkReadiness(handler);
```

`Accounts::startLogin` still requires `LoginAccountParams`, because that tagged
union selects the login method. `AttestationGenerateParams` remains an incoming
server-request model. The experimental `remoteControl/enable` and
`remoteControl/disable` identities remain InventoryOnly and have no façade
operations.

## Advanced raw protocol

Use `client.raw()` only when low-level JSON-RPC control or forward-compatible
observation is intentional:

```cpp
codex::Submission rawSubmission = client.raw().request(
    "future/method",
    codex::Json::object(),
    [](const codex::Response& response) {
        // Handle the raw correlated response.
    });
```

The raw path shares the same transport, request-ID allocator, pending map,
generation, callback scheduler, cancellation, and shutdown path as typed
domains. `setOnNotification`, `setOnServerRequest`, and `setOnUnknownMessage`
retain raw forward compatibility.

## Final A1 and roadmap boundary

The stable protocol registry remains 339 Complete / 0 Partial /
0 NotImplemented / 48 NotApplicable. Canonical notification, Event, and typed
server-request variants retain their established sizes and indices.

The four direct conversation accessors predate the grouped client and were
temporarily deprecated while the final façade shape was undecided. Final A1b
retained both paths. A1.5 resolves that deferred decision in favor of direct
domain access and removes the redundant public grouped client.

The Codex libraries use SOVERSION 3 after the additive public execution-
configuration State API changed the C++ ABI. A1.6a established
the backend foundation, and A1.6b completes the trusted BackendCore command
and canonical-state layer. Frontend Protocol redesign and provider-neutral
architecture remain separate A1.7 and A2 work.
