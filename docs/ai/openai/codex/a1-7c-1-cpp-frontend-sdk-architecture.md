# A1.7c-1 — Canonical C++ Frontend SDK Architecture

Repository:
    SNodeC/AISuite

Canonical deliverable:
    AISuite::OpenAICodexFrontendClient

Canonical C++ namespace:
    ai::openai::codex::frontend::client

Canonical main class:
    ai::openai::codex::frontend::client::Client

Baseline:
    merged A1.7b
    complete FrontendService
    Frontend Protocol v1
    installed SNode.C 2.0 or newer

This document freezes the A1.7c-1 architecture.

It is not an invitation to redesign the SDK during implementation.

Codex must implement this architecture rather than inventing another one.

======================================================================
1. Purpose
======================================================================

A1.7c-1 introduces the canonical reusable C++ client implementation for:

    snodec.codex-frontend
    protocol version 1

The SDK is used by:

- codex-backend-client;
- codex-ui in A1.7c-2;
- the future Qt Creator plugin;
- future native C++ applications.

The SDK owns:

- Frontend Protocol encoding and decoding;
- Hello construction;
- authentication credential placement;
- capability negotiation;
- session lifecycle;
- command request-ID generation;
- command correlation;
- typed result decoding;
- initial synchronization;
- explicit synchronization;
- snapshot replacement;
- replay application;
- reconnect cursor handling;
- sparse visible-sequence handling;
- client-side state reduction;
- typed state snapshots;
- typed state-change notifications;
- method discovery;
- local static method-availability checks;
- bounded client-side resource policy;
- protocol-error containment.

Applications own:

- physical transport creation;
- Unix/TCP/TLS/RFCOMM/WebSocket endpoint selection;
- transport configuration;
- physical reconnect timing;
- UI;
- CLI;
- presentation;
- workflow composition;
- persistent application preferences;
- persistent credential storage;
- optional persistence of application data.

The transport sends and receives complete Frontend Protocol JSON objects.
It does not implement Frontend Protocol semantics.

======================================================================
2. Strict non-goals
======================================================================

A1.7c-1 does not:

- modify Frontend Protocol identity;
- modify Frontend Protocol version;
- add a new message kind;
- expose raw Codex App Server JSON;
- connect directly to the Codex App Server;
- redesign BackendCore;
- redesign FrontendService;
- add another backend runtime;
- add another journal;
- add a second state authority;
- implement codex-ui migration;
- implement a Qt Creator plugin;
- implement TypeScript;
- implement a browser frontend;
- implement provider abstraction;
- add ChatGPT, Claude, Gemini, or another provider;
- introduce a provider-neutral `IAIClient`;
- introduce threads, futures, promises, or coroutines;
- add automatic controller acquisition;
- add automatic command retry;
- add durable frontend sessions;
- restore command correlation after reconnect;
- persist authentication credentials;
- introduce a generic string-and-JSON command escape hatch.

A1.7c-2 remains the next PR after A1.7c-1.

A1.7d remains the TypeScript SDK and browser PR.

Provider abstraction remains after the complete A1.7 series.

======================================================================
3. Architectural layering
======================================================================

The final C++ architecture is:

    codex-backend
            │
            │ Frontend Protocol v1
            │
    application-owned physical transport
            │
            │ complete JSON objects
            │
    client::Connection
            │
    client::Client
            ├── handshake and authentication
            ├── command correlation
            ├── synchronization
            ├── replay cursor
            ├── state reducer
            ├── typed façades
            └── callbacks
                    │
            client::State
                    │
        ┌───────────┼───────────┐
        │           │           │
       CLI       codex-ui    Qt Creator

There is exactly one `client::Client` per logical backend connection context.

One Client may survive several sequential physical connections.

One Client may have at most one active physical connection at a time.

A physical transport never owns the Client.

The Client never owns a concrete physical transport.

======================================================================
4. Library, namespace, and installation
======================================================================

Add a new shared library:

    target:
        ai-openai-codex-frontend-client

    imported target:
        AISuite::OpenAICodexFrontendClient

    output:
        libaisuite-openai-codex-frontend-client.so

Its public namespace is:

    ai::openai::codex::frontend::client

Do not place the client implementation directly into:

    ai::openai::codex::frontend

because that namespace already contains the shared protocol and server-side
FrontendService boundary.

The new library publicly depends on:

    AISuite::OpenAICodexFrontend

and on the existing Codex typed value types needed by its public operation
signatures.

The SDK library must not publicly depend on:

- Unix sockets;
- IPv4;
- IPv6;
- TLS;
- RFCOMM;
- HTTP;
- WebSocket;
- Qt;
- ncurses.

Those remain transport/application dependencies.

The core SDK should not require SNode.C networking components. It may use
ordinary C++20 and the existing AISuite protocol libraries.

======================================================================
5. Public header organization
======================================================================

Use a dedicated installed directory:

    ai/openai/codex/frontend/client/

Required public headers:

    Client.h
    Connection.h
    Transport.h
    Results.h
    State.h
    StateTypes.h
    Changes.h
    Types.h

Native façade headers:

    Controller.h
    Provider.h
    Synchronization.h

Domain façade headers:

    Accounts.h
    Apps.h
    Commands.h
    Configuration.h
    ExternalAgents.h
    Feedback.h
    Filesystem.h
    Hooks.h
    Marketplace.h
    Mcp.h
    Models.h
    PermissionProfiles.h
    Plugins.h
    Requests.h
    Reviews.h
    Skills.h
    Threads.h
    Turns.h
    WindowsSandbox.h

Generated public metadata may use:

    GeneratedBindings.h

`Client.h` must forward-declare domain façades rather than including every
domain header.

Do not add an umbrella header that includes every Codex type unless an
installed-consumer use case proves it necessary.

Every installed header must compile independently.

======================================================================
6. Fundamental public object model
======================================================================

The fundamental public classes are:

    Client
    Connection
    State

The ownership and copy rules are:

    Client:
        application-owned;
        non-copyable;
        non-movable;
        PIMPL-backed;
        stable address for all façade references.

    Connection:
        move-only;
        default-constructible as closed;
        PIMPL/control-block backed;
        represents one physical transport attachment;
        closes/detaches on destruction.

    State:
        immutable;
        cheap-copy;
        copyable and movable;
        shared immutable implementation;
        remains valid after later client updates;
        remains valid after Client destruction.

Every façade:

- is non-copyable;
- is non-movable;
- is owned by Client;
- is returned by reference;
- contains only a stable pointer-sized link to Client;
- has no virtual public interface.

Do not expose:

- `shared_ptr<void>`;
- raw implementation pointers;
- mutable state references;
- transport socket pointers;
- SNode.C SocketContext objects;
- public inheritance as an extension mechanism.

======================================================================
7. Transport-neutral connection contract
======================================================================

The SDK transport boundary works with complete compact JSON objects.

It does not know whether an object came from:

- one JSONL record;
- one WebSocket text message;
- an in-memory test transport;
- a future transport.

The public transport types are conceptually:

    enum class OutboundKind {
        Hello,
        Command
    };

    struct OutboundMessage {
        OutboundKind kind;
        std::string compactJson;
        std::size_t serializedBytes;
        bool sensitive;
    };

    enum class SendStatus {
        Accepted,
        Backpressure,
        Closed,
        Failed
    };

    struct TransportError {
        std::string message;
        bool retryable;
    };

    struct SendResult {
        SendStatus status;
        std::optional<TransportError> error;
    };

    struct TransportCallbacks {
        std::function<SendResult(OutboundMessage)> send;
        std::function<void(std::string)> close;
    };

`send` takes ownership of the outbound JSON value.

`Accepted` means the physical transport has either sent the message or accepted
ownership into its own bounded queue.

Every other result means the transport did not accept ownership.

A rejected send closes only this Client connection.

The SDK never inspects a transport writer queue.

The SDK never polls transport queue size.

The SDK never calculates `totalQueued - totalSent`.

The transport's close callback must be invoked at most once by the SDK for one
connection failure.

Transport adapters must not log complete Frontend Protocol payloads.

`OutboundMessage.sensitive` is true for messages containing known
authentication or secret response material. Transport implementations must
treat all messages as potentially sensitive even when this flag is false.

JSONL transport adapters:

- append exactly one newline on send;
- remove framing before calling `Connection::receive`;
- tolerate fragmented reads;
- tolerate several frames in one read;
- enforce a finite frame bound.

WebSocket transport adapters:

- send one complete JSON object as one text message;
- request the `codex` WebSocket subprotocol;
- reject binary messages;
- do not append a newline.

======================================================================
8. Connection API
======================================================================

The conceptual public API is:

    class Connection {
    public:
        Connection() noexcept;

        Connection(const Connection&) = delete;
        Connection(Connection&&) noexcept;

        Connection& operator=(const Connection&) = delete;
        Connection& operator=(Connection&&) noexcept;

        ~Connection();

        void transportConnected() noexcept;

        ReceiveResult receive(std::string_view compactJson) noexcept;
        ReceiveResult receive(const frontend::Json& message) noexcept;
        ReceiveResult receive(const frontend::ServerMessage& message) noexcept;

        void transportDisconnected(
            std::optional<TransportError> error = std::nullopt) noexcept;

        void close(
            std::string reason = "frontend client connection closed") noexcept;

        bool isOpen() const noexcept;
        bool isTransportConnected() const noexcept;
        std::uint64_t generation() const noexcept;
    };

`receive(ServerMessage)` exists for in-memory integration and tests. It must
still execute the same ordering, correlation, state validation, and lifecycle
rules as the JSON path.

`transportConnected()` may be called exactly once for a Connection.

It causes the SDK to:

1. obtain the authentication context;
2. decide whether replay continuity is valid;
3. build Hello;
4. request the supported capabilities;
5. serialize Hello;
6. send Hello through the transport callback;
7. enter authentication state.

A transport must not construct or send Hello itself.

`transportDisconnected()` is the only normal physical-disconnect notification.

The SDK must tolerate a transport close callback followed later by
`transportDisconnected()` without double-closing or double-calling operation
callbacks.

======================================================================
9. Client API
======================================================================

The conceptual main API is:

    class Client {
    public:
        explicit Client(
            ClientOptions options = {},
            ClientCallbacks callbacks = {});

        Client(const Client&) = delete;
        Client(Client&&) = delete;

        Client& operator=(const Client&) = delete;
        Client& operator=(Client&&) = delete;

        ~Client();

        Connection openConnection(TransportCallbacks callbacks);

        void setCallbacks(ClientCallbacks callbacks);

        void close(
            std::string reason = "frontend client closed") noexcept;

        bool isOpen() const noexcept;
        bool hasActiveConnection() const noexcept;
        bool isReady() const noexcept;

        ConnectionState connectionState() const noexcept;
        State state() const noexcept;

        std::optional<SessionInfo> session() const;
        std::optional<frontend::SequenceNumber> visibleSequence() const;
        std::optional<frontend::SequenceNumber> synchronizedThrough() const;

        std::size_t pendingOperationCount() const noexcept;

        MethodStatus methodStatus(
            frontend::generated::MethodId method) const;

        CapabilityStatus capabilityStatus(
            frontend::FrontendCapability capability) const;

        Controller& controller() noexcept;
        Provider& provider() noexcept;
        Synchronization& synchronization() noexcept;

        Accounts& accounts() noexcept;
        Apps& apps() noexcept;
        Commands& commands() noexcept;
        Configuration& configuration() noexcept;
        ExternalAgents& externalAgents() noexcept;
        Feedback& feedback() noexcept;
        Filesystem& filesystem() noexcept;
        Hooks& hooks() noexcept;
        Marketplace& marketplace() noexcept;
        Mcp& mcp() noexcept;
        Models& models() noexcept;
        PermissionProfiles& permissionProfiles() noexcept;
        Plugins& plugins() noexcept;
        Requests& requests() noexcept;
        Reviews& reviews() noexcept;
        Skills& skills() noexcept;
        Threads& threads() noexcept;
        Turns& turns() noexcept;
        WindowsSandbox& windowsSandbox() noexcept;
    };

One Client represents one logical backend deployment for its entire lifetime.

To connect to an unrelated backend deployment, construct another Client.

The Client may retain state and replay position between sequential physical
connections to the same backend.

`openConnection()` fails closed when:

- Client is closed;
- another Connection is active;
- callbacks are incomplete;
- options are invalid.

Do not throw for ordinary connection lifecycle failures.

Configuration errors detected by the Client constructor may throw
`std::invalid_argument`.

Runtime protocol and transport failures must be delivered as typed errors and
must not escape through the event loop.

======================================================================
10. Client lifecycle state machine
======================================================================

The exact public states are:

    Disconnected
    Connecting
    Authenticating
    Synchronizing
    Ready
    Closing
    Closed

Transitions:

    construct Client
        -> Disconnected

    openConnection()
        -> Connecting

    Connection::transportConnected()
        -> Authenticating

    valid Welcome
        -> Synchronizing

    valid initial sync.complete
        -> Ready

    unexpected physical disconnect
        -> Disconnected

    Client::close()
        -> Closing
        -> Closed

    fatal protocol failure
        -> Closing
        -> Disconnected
        or Closed when the Client itself is closing

A failed physical connection does not permanently poison Client.

The application may create a new Connection after Disconnected.

Closed is terminal.

The Client retains the last state after an unexpected disconnect but marks it
stale.

The Client never treats a stale state as synchronized.

======================================================================
11. Authentication model
======================================================================

Authentication is supplied through:

    struct AuthenticationContext {
        frontend::AuthenticationCredential credential;
        std::optional<std::string> continuityKey;
    };

    using CredentialProvider =
        std::function<AuthenticationContext()>;

`ClientOptions` contains one CredentialProvider.

It is called after the physical transport reports connected and before Hello is
serialized.

The SDK does not persist the credential.

The SDK must release and erase its transient serialized credential material as
soon as the transport takes ownership of Hello.

The application may retain its own credential source; that is outside the SDK.

`continuityKey`:

- is never sent to the backend;
- must not contain the credential;
- is bounded to a small finite length;
- identifies the application’s belief that the authenticated information
  ceiling is the same as on the previous connection;
- enables automatic replay only when unchanged.

Examples of application-provided continuity keys:

    verified-local:<uid>
    bearer-profile:<local-account-id>
    credential-generation:<number>

Do not hash or log bearer credentials to create a continuity key.

If continuityKey is absent or changes, the SDK omits `resumeAfter` and requests
a fresh snapshot.

Authentication failure closes only the current physical connection.

No BackendCore session exists on the server before successful authentication;
the client must not report a SessionInfo before Welcome.

======================================================================
12. ClientOptions and resource bounds
======================================================================

The conceptual options are:

    struct ClientOptions {
        std::vector<frontend::FrontendCapability> requestedCapabilities;
        std::vector<frontend::FrontendCapability> requiredCapabilities;

        CredentialProvider credentialProvider;

        std::size_t maximumInboundMessageBytes;
        std::size_t maximumDecodedStateBytes;
        std::size_t maximumPendingOperations;
        std::size_t maximumRetainedDiagnostics;

        bool allowLegacyV1;
    };

Default requested capabilities are exactly the 13 A1.7b mechanism
capabilities:

    method_discovery
    security_scopes
    complete_provider_operations
    complete_reverse_requests
    complete_backend_domains
    conditional_filesystem
    conditional_command_execution
    dedicated_pending_requests
    dedicated_notification_events
    complete_thread_items
    authenticated_frontend
    scope_projected_state
    provider_lifecycle

Do not request:

    multi_transport
    cpp_client_sdk
    typescript_client_sdk
    browser_ui
    qt_ui

Those product/topology capabilities do not select a state representation.

Recommended defaults:

    maximumInboundMessageBytes:
        16 MiB

    maximumDecodedStateBytes:
        64 MiB

    maximumPendingOperations:
        256

    maximumRetainedDiagnostics:
        64

These defaults remain configurable.

Zero means zero capacity, not unlimited.

The Client validates requested and required capability arrays for:

- duplicates;
- invalid enum values;
- product capability misuse;
- impossible required/requested combinations.

If a required capability is not permitted by Welcome, initial synchronization
fails and the connection closes.

======================================================================
13. Welcome and session metadata
======================================================================

Welcome produces:

    struct SessionInfo {
        std::string sessionId;
        frontend::SessionRole role;
        frontend::SyncMode syncMode;
        frontend::SequenceNumber serverCurrentSequence;
        std::optional<std::string> serverVersion;
        CapabilityAdvertisement capabilities;
        MethodCatalog methods;
    };

SessionInfo exists only for the current physical connection.

Reconnect always creates a new session ID.

Reconnect always begins as observer.

Reconnect does not restore:

- controller ownership;
- the old session ID;
- pending operation correlation;
- command responses;
- prior role;
- prior scopes as an identity assertion.

The Client records:

- defined capabilities;
- implemented capabilities;
- permitted capabilities;
- available methods;
- permitted methods.

Optional Welcome discovery fields must be represented as unknown rather than
invented.

======================================================================
14. Method and capability status
======================================================================

Use a tri-state knowledge model:

    enum class Availability {
        Unknown,
        No,
        Yes
    };

    struct MethodStatus {
        frontend::generated::MethodId method;
        Availability available;
        Availability permitted;

        bool controllerRequired;
        bool providerReadyRequired;
        bool defaultEnabled;
        std::span<const frontend::FrontendScope> requiredScopes;
    };

    struct CapabilityStatus {
        frontend::FrontendCapability capability;
        Availability defined;
        Availability implemented;
        Availability permitted;
    };

Static method metadata comes from GeneratedProtocol.

Dynamic availability and permission come from Welcome.

Do not create another handwritten security table.

Submission behavior:

- explicit `available == No`:
    reject locally;

- explicit `permitted == No`:
    reject locally;

- `Unknown`:
    permit submission and let the server remain authoritative;

- controller ownership:
    advisory locally, authoritative on the server;

- provider readiness:
    advisory locally, authoritative on the server;

- parameter-sensitive policy:
    authoritative on the server.

Do not locally reject a command merely because a cached controller or provider
state may be stale.

======================================================================
15. Synchronization and replay cursor model
======================================================================

The Client tracks two distinct sequence concepts:

    visibleSequence:
        last fully applied visible snapshot/event occurrence;

    synchronizedThrough:
        authoritative global journal cursor.

They are not interchangeable.

Visible sequence numbers:

- are global journal occurrence identifiers;
- are ordered;
- may be sparse;
- may repeat inside one expanded atomic occurrence group;
- do not have to equal previous + 1.

A sequence jump is legal.

A sequence regression is not legal.

`sync.complete.sequence` becomes `synchronizedThrough` only after every
preceding synchronization message has been successfully applied.

`synchronizedThrough` may be greater than visibleSequence when a replay suffix
contains only hidden occurrences.

During normal live delivery, synchronizedThrough may advance to the last fully
applied visible occurrence.

On reconnect, Hello includes:

    resumeAfter = synchronizedThrough

only when:

- Client has retained synchronized state;
- authentication continuityKey is present;
- continuityKey is unchanged;
- requested representation capabilities are unchanged.

Otherwise Hello omits resumeAfter and requests a snapshot.

The SDK does not infer a replay gap from sequence arithmetic.

Snapshot fallback is selected only by the server’s synchronization mode or an
explicit server-reported replay condition.

======================================================================
16. Projection-continuity safety
======================================================================

Retained state may have been projected under a richer principal or capability
set than a later connection.

The SDK must not blindly replay onto an incompatible retained state.

After each Welcome, compute a non-secret projection fingerprint from:

- requested representation capabilities;
- permitted capability set;
- available method set when supplied;
- permitted method set when supplied;
- credential continuityKey presence/value.

If a reconnect Welcome fingerprint differs from the fingerprint associated
with retained state:

1. mark the retained state stale and unusable as current;
2. do not mix the new replay projection into the old state;
3. if the server selected snapshot, replace normally;
4. if the server selected replay, consume the initial replay protocol
   correctly, then automatically request `snapshot.get`;
5. do not enter Ready until that fresh snapshot and its sync.complete have been
   applied.

If discovery fields needed to establish continuity are omitted, prefer a fresh
snapshot over mixing projections.

This behavior requires no protocol change.

======================================================================
17. Initial synchronization
======================================================================

Initial synchronization accepts exactly:

Snapshot mode:

    Welcome
    Snapshot
    SyncComplete

Replay mode:

    Welcome
    zero or more EventBatch messages
    SyncComplete

Do not mix snapshot and replay inside one synchronization.

A replay synchronization with no visible EventBatch is valid.

A Snapshot is fully decoded and validated before replacing state.

An EventBatch is fully decoded and applied to a candidate state before the
current state is committed.

A failed event in a batch must not leave a partially mutated state.

The Client becomes Ready only after a valid initial SyncComplete.

Commands submitted before Ready are rejected with a typed `NotReady` client
error.

The SDK does not queue application operations before Ready.

Applications such as codex-backend-client may maintain their own input queue.

======================================================================
18. Explicit synchronization
======================================================================

Provide:

    class Synchronization {
    public:
        Submission snapshot(
            CompletionHandler<SynchronizationResult> handler);

        Submission replay(
            frontend::SequenceNumber after,
            CompletionHandler<SynchronizationResult> handler);
    };

    struct SynchronizationResult {
        frontend::SyncMode mode;
        frontend::SequenceNumber synchronizedThrough;
        State state;
        std::size_t receivedEvents;
        std::size_t appliedEvents;
        std::size_t ignoredAlreadyAppliedEvents;
        bool snapshotFallback;
    };

The command response arrives before synchronization data.

The synchronization callback is not called on successful response alone.

It is called only after:

- the response succeeded;
- all following snapshot/replay messages were applied;
- SyncComplete was applied.

If the command response fails, call the callback immediately with failure and
expect no synchronization stream.

If the connection closes after the successful response but before
SyncComplete, complete the operation with a local connection failure.

Allow only one explicit synchronization operation at a time.

While an explicit synchronization is active, transition to Synchronizing and
reject new application operations as NotReady.

Replay events whose occurrence is already fully represented by the current
synchronized state are validated but not applied again.

Do not duplicate notices, activities, or other append-oriented records during
an overlapping explicit replay.

======================================================================
19. Typed client state
======================================================================

Do not expose the raw Snapshot JSON object as the canonical state API.

Add:

    class State

with immutable, cheap-copy semantics.

State contains:

Connection-local metadata:

- local state revision;
- current/stale/synchronizing freshness;
- representation mode;
- visibleSequence;
- synchronizedThrough;
- current SessionInfo;
- projection fingerprint metadata.

Mandatory backend domains:

- provider;
- controller;
- sessions;
- capacity;
- truncation;
- diagnostics.

Conversation domains:

- thread list state;
- ordered threads;
- ordered turns;
- ordered items;
- accumulated visible content;
- pending requests.

Additional projected domains:

- accounts;
- models;
- configuration;
- processes;
- filesystem watches;
- fuzzy searches;
- permission profiles;
- reviews;
- apps;
- external agents;
- hooks;
- marketplace;
- plugins;
- skills;
- MCP;
- Windows sandbox;
- remote control/platform;
- notices;
- activities;
- bounded compatibility extensions.

For stable protocol fields, expose typed C++ members.

JSON remains permitted only where Frontend Protocol deliberately defines:

- arbitrary JSON;
- forward-compatible extensions;
- bounded details;
- unknown request results;
- injected Responses API items;
- explicitly opaque configuration values.

Applications must not need to traverse JSON for normal stable workflows.

======================================================================
20. Projected domain representation
======================================================================

A domain may be omitted because of:

- capability selection;
- deployment policy;
- scope filtering;
- representation choice;
- snapshot bounds.

The protocol does not always identify which reason caused an omission.

Do not invent an omission reason.

Use a representation conceptually equivalent to:

    template <typename T>
    struct Projected {
        std::optional<T> value;
        bool truncated;
        std::vector<std::string> omittedFields;
    };

An absent value means absent from the current projection.

It does not automatically mean:

- unsupported;
- unauthorized;
- empty.

Expose explicit truncation and omitted-field metadata only when the protocol
provides it.

======================================================================
21. Conversation-state records
======================================================================

Provide client-specific projection records such as:

    ThreadState
    TurnState
    ItemState
    PendingRequestState

Use existing typed IDs where available:

    typed::ThreadId
    typed::TurnId
    typed::ItemId

ThreadState contains at minimum:

- ID;
- title/name;
- preview;
- cwd;
- model;
- model provider;
- status;
- creation/update timestamps;
- fully-loaded state;
- source freshness;
- ordered turns;
- extensions explicitly permitted by the protocol.

TurnState contains at minimum:

- ID;
- thread ID;
- status;
- active;
- terminal;
- failure;
- token usage;
- ordered items;
- source freshness;
- connection-invalidated state.

ItemState contains at minimum:

- ID;
- item kind;
- lifecycle/status;
- agent text;
- reasoning text;
- reasoning summary;
- command output;
- truncation counters;
- start/completion timestamps;
- typed or explicitly opaque data;
- source freshness;
- connection-invalidated state.

Content events replace the accumulated channel value.

They are not appended as an assumed raw delta.

PendingRequestState:

- supports all ten stable request kinds;
- contains only projected presentation data;
- never retains secret answers;
- never retains provider request tokens;
- never retains JSON-RPC request IDs.

State provides ordered iteration and ID lookup without requiring a UI to
construct its own indexes.

The implementation may store maps and order vectors internally.

======================================================================
22. State callbacks and change model
======================================================================

Provide:

    enum class UpdateCause {
        InitialSnapshot,
        InitialReplay,
        ReconnectReplay,
        ProjectionRefresh,
        SnapshotFallback,
        ExplicitSnapshot,
        ExplicitReplay,
        Live,
        ConnectionBecameStale,
        SynchronizationCompleted
    };

    struct StateUpdate {
        State state;
        UpdateCause cause;

        std::optional<frontend::SequenceNumber> fromSequence;
        std::optional<frontend::SequenceNumber> toSequence;

        std::vector<Change> changes;
    };

`Change` is a typed variant covering:

- state replacement;
- cursor advancement;
- provider update;
- controller update;
- session update;
- thread-list update;
- thread upsert;
- thread removal;
- turn upsert;
- item upsert;
- item content replacement;
- pending-request update;
- account update;
- model update;
- configuration update;
- process update;
- filesystem-watch update;
- fuzzy-search update;
- review update;
- integration update;
- plugin update;
- skill update;
- MCP update;
- platform update;
- notice addition;
- activity update;
- capacity update;
- diagnostics update;
- bounded compatibility extension.

Each change carries stable identifiers rather than pointers into mutable state.

The State inside StateUpdate is the complete immutable state after applying the
whole update.

Snapshot replacement emits one state-replaced change rather than thousands of
synthetic per-object changes.

One EventBatch is applied transactionally and emits one StateUpdate.

Expanded events sharing one occurrence sequence are exposed in their original
order within the same StateUpdate.

A hidden-only suffix may emit no domain changes but must still produce a
synchronization/cursor notification at SyncComplete.

======================================================================
23. Client callbacks
======================================================================

Provide:

    struct ConnectionStateChange {
        ConnectionState previous;
        ConnectionState current;
        std::optional<Error> error;
    };

    struct SynchronizationInfo {
        frontend::SyncMode mode;
        frontend::SequenceNumber synchronizedThrough;
        State state;
        bool reconnect;
        bool snapshotFallback;
    };

    struct Diagnostic {
        enum class Severity {
            Debug,
            Information,
            Warning,
            Error
        };

        Severity severity;
        std::string message;
        std::optional<Error> error;
    };

    struct ClientCallbacks {
        std::function<void(const ConnectionStateChange&)>
            onConnectionStateChanged;

        std::function<void(const StateUpdate&)>
            onStateUpdated;

        std::function<void(const SynchronizationInfo&)>
            onSynchronized;

        std::function<void(frontend::SequenceNumber)>
            onCursorAdvanced;

        std::function<void(const frontend::ServerMessage&)>
            onProtocolMessage;

        std::function<void(const Diagnostic&)>
            onDiagnostic;
    };

`onProtocolMessage` is read-only observability.

It does not let an application bypass:

- state reduction;
- command correlation;
- synchronization;
- schema validation.

It is provided primarily for:

- codex-backend-client `--json`;
- diagnostics;
- protocol inspection;
- tests.

Invoke callbacks only after the relevant internal state is valid and committed.

Contain every callback exception.

A throwing callback must not escape into SNode.C or corrupt the Client.

Report callback exceptions through onDiagnostic where possible.

======================================================================
24. Error and operation result model
======================================================================

Do not reuse App Server `typed::OperationResult<T>` as the client result
container because Frontend Protocol has its own stable error taxonomy and
transport lifecycle.

Reuse its domain value types where appropriate, but define a frontend-client
result model.

Required concepts:

    enum class ErrorOrigin {
        Client,
        Transport,
        Protocol,
        Command
    };

    enum class ClientErrorCode {
        InvalidConfiguration,
        AlreadyConnected,
        NotConnected,
        NotReady,
        Closed,
        MethodUnavailable,
        MethodNotPermitted,
        TooManyPendingOperations,
        SynchronizationAlreadyActive,
        SerializationFailed,
        SendRejected,
        TransportFailure,
        DecodeFailure,
        UnexpectedMessage,
        StateDivergence,
        StateCapacityExceeded,
        ResponseTypeMismatch,
        RequestIdExhausted,
        CallbackFailure
    };

    struct Error {
        ErrorOrigin origin;

        std::optional<ClientErrorCode> clientCode;
        std::optional<frontend::ErrorCode> protocolCode;

        std::string message;
        std::optional<std::int64_t> remoteCode;
        std::optional<frontend::Json> details;

        bool retryable;
    };

    class RequestId {
    public:
        const std::string& value() const noexcept;
        auto operator<=>(const RequestId&) const = default;
    };

    template <typename T>
    struct OperationResult {
        RequestId requestId;
        std::optional<T> value;
        std::optional<Error> error;

        bool succeeded() const noexcept;
        explicit operator bool() const noexcept;
    };

    template <typename T>
    using CompletionHandler =
        std::function<void(const OperationResult<T>&)>;

    using DoneHandler =
        CompletionHandler<typed::Unit>;

    struct Submission {
        std::optional<RequestId> requestId;
        std::optional<Error> error;

        bool accepted() const noexcept;
        explicit operator bool() const noexcept;
    };

Applications branch on:

- `ErrorOrigin`;
- `ClientErrorCode` for local failures;
- `frontend::ErrorCode` for stable protocol/command failures.

Do not translate every local failure into an unrelated server error code.

Command failure does not close the connection unless the server separately says
the connection must close.

Protocol corruption and state divergence close the connection.

======================================================================
25. Request-ID generation and command correlation
======================================================================

Applications do not supply request IDs through domain façades.

The SDK generates them.

Requirements:

- non-empty;
- bounded;
- unique among pending operations;
- never reused while pending;
- deterministic monotonic generation per Client;
- includes sufficient connection/client generation context to avoid accidental
  reuse after reconnect;
- exhaustion fails locally without wraparound.

Every accepted operation has exactly one terminal callback:

- successful response;
- command failure;
- protocol failure;
- transport disconnect;
- Client close;
- response decoding failure.

Closing or disconnecting completes all pending operations exactly once.

A reconnect does not restore pending operations.

The SDK never automatically resubmits a command after reconnect.

An operation may have been accepted by the backend even when its response was
lost. Applications decide whether a later retry is safe.

An unsolicited response is a protocol violation.

A duplicate response is a protocol violation.

A response whose result does not match the submitted MethodId is a protocol
violation.

======================================================================
26. Typed operation façades
======================================================================

Use callback-last asynchronous APIs.

The domain façade organization is:

    Controller
    Provider
    Synchronization

    Accounts
    Apps
    Commands
    Configuration
    ExternalAgents
    Feedback
    Filesystem
    Hooks
    Marketplace
    Mcp
    Models
    PermissionProfiles
    Plugins
    Requests
    Reviews
    Skills
    Threads
    Turns
    WindowsSandbox

For the 86 provider operations:

- mirror the established typed AppServerClient façade naming;
- reuse existing `typed::*Params`;
- reuse existing `typed::*Response` values only when the Frontend Protocol
  result is semantically identical;
- define a client-specific projected result type when redaction, omission, or
  frontend-native synthesis changes the contract;
- use `client::Submission`;
- use `client::CompletionHandler<T>`.

Example:

    client.threads().start(
        typed::ThreadStartParams{...},
        [](const client::OperationResult<
               typed::ThreadStartResponse>& result) {
            ...
        });

The façade API must cover all 86 provider operations, not only the original
legacy 15.

Do not expose provider raw protocol methods.

======================================================================
27. Native façades
======================================================================

Controller:

    struct ControllerResult {
        std::optional<std::string> controllerSessionId;
        frontend::SessionRole role;
        bool ownedByThisClient;
    };

    class Controller {
    public:
        Submission acquire(
            CompletionHandler<ControllerResult> handler);

        Submission release(
            CompletionHandler<ControllerResult> handler);

        bool ownedByThisClient() const noexcept;
    };

Controller acquisition remains explicit.

Do not:

- acquire automatically;
- retry acquisition;
- force takeover;
- release another session;
- reacquire automatically after reconnect.

Provider:

    class Provider {
    public:
        Submission start(DoneHandler handler);
        Submission stop(DoneHandler handler);
        Submission restart(DoneHandler handler);
    };

Synchronization is defined in the earlier synchronization section.

======================================================================
28. Reverse-request façade
======================================================================

`Requests` covers all 12 approved reverse response/rejection methods.

It provides typed methods for:

- command-execution approval;
- file-change approval;
- user input;
- authentication response;
- apply-patch approval;
- exec-command approval;
- permissions approval;
- attestation;
- dynamic-tool response;
- MCP elicitation;
- known-request rejection;
- unknown-request response/rejection.

Use existing typed response value types where they match.

Unknown request result remains `frontend::Json` because the protocol explicitly
defines it as arbitrary JSON.

Do not add a generic response method that can target a known request kind with
an arbitrary unvalidated payload.

Do not retain submitted secret answers or access tokens in:

- Client state;
- diagnostics;
- operation history;
- protocol observation callbacks beyond the actual outbound object lifecycle.

======================================================================
29. Restricted generated operation API
======================================================================

The SDK may expose one advanced schema-validated API for tooling:

    Submission submit(
        frontend::generated::CompleteCommandParameters parameters,
        GeneratedCompletionHandler handler);

It must:

- accept only one of the 105 generated MethodId alternatives;
- generate the request ID internally;
- validate the parameters against the generated schema;
- correlate through the normal pending-operation map;
- validate the result through `decodeDefinedResult`;
- return a tagged `CompleteCommandResult`;
- obey the same lifecycle, availability, permission, and resource rules.

It must not accept:

    std::string method
    +
    arbitrary JSON parameters

It must not send:

- Hello;
- unknown methods;
- raw App Server messages;
- caller-supplied request IDs.

Domain façades remain the preferred public API.

This restricted API may support the CLI's diagnostic `raw` command without
letting the CLI reimplement correlation.

======================================================================
30. Generated C++ binding authority
======================================================================

Do not create a second handwritten runtime list of 105 methods.

The existing generated Frontend Protocol metadata remains authoritative for:

- MethodId;
- method spelling;
- parameter schema;
- result schema;
- category;
- scopes;
- controller requirement;
- provider readiness;
- default enablement;
- implementation status.

Add a C++ client binding authority containing only language-specific facts:

- façade;
- public C++ method name;
- public parameter type;
- public result type;
- parameter encoder;
- result decoder.

This may be a committed reviewed source such as:

    tools/frontend/cpp-client-bindings.json

It is not allowed to redefine:

- protocol method spelling;
- security policy;
- availability;
- controller policy;
- provider readiness;
- capability policy.

Generation must fail when:

- one MethodId has no client binding;
- one MethodId has more than one binding;
- a binding references a nonexistent type;
- parameter/result schema metadata disagrees;
- façade counts drift;
- an encoder or decoder is missing;
- an operation lacks a public completion type.

Generate:

    client/GeneratedBindings.h

and private dispatch tables as needed.

Hard compile-time and test invariants:

    native methods              7
    provider operations        86
    reverse methods            12
                               ---
    total                     105

======================================================================
31. Parameter and result conversion
======================================================================

Every domain façade performs:

    typed params
        ↓
    exact Frontend Protocol parameter object
        ↓
    generated schema validation
        ↓
    generated MethodId submission

Do not assume the App Server parameter encoding and Frontend Protocol parameter
encoding are always byte-identical.

Reuse an existing encoder only after verifying that it matches the Frontend
Protocol schema.

Every response performs:

    Response.result JSON
        ↓
    generated MethodId result-schema validation
        ↓
    exact client result decoder
        ↓
    typed OperationResult<T>

A decoding failure is a protocol failure, not a successful result with empty
fields.

Result decoding must not rely on an unvalidated type-name string.

The MethodId associated with the pending operation is the decoding authority.

======================================================================
32. account.read parameter-sensitive cleanup
======================================================================

A1.7c-1 also removes the documented cross-translation-unit coupling in the
server implementation of `account.read`.

FrontendService must derive `refreshToken` policy from the normalized,
schema-validated parameter object used for dispatch.

It must not reread an original pre-normalization tagged JSON object and depend
on another translation unit having rejected extension-field collisions.

This correction:

- changes no wire contract;
- changes no scopes;
- changes no method count;
- changes no result;
- remains inside A1.7c-1 because A1.7b explicitly deferred it here.

======================================================================
33. Legacy-v1 compatibility
======================================================================

`ClientOptions::allowLegacyV1` defaults to true.

When optional discovery and expanded capabilities are absent:

- accept the original Frontend Protocol v1 handshake;
- synchronize from legacy Snapshot/EventBatch forms;
- normalize legacy state into the same client::State model;
- mark unavailable expanded domains as absent;
- preserve the original 15-method compatibility path;
- do not fabricate expanded data.

When the server explicitly reports an operation unavailable, reject locally.

When availability remains unknown, the server remains authoritative.

A caller may require specific capabilities through `requiredCapabilities`.

codex-ui in A1.7c-2 may use stronger capability requirements than the reference
CLI.

======================================================================
34. State-reducer rules
======================================================================

The reducer must support:

- legacy snapshots;
- expanded snapshots;
- all legacy normalized event families;
- all 26 expanded event families;
- sparse visible sequences;
- repeated sequence values for one expanded occurrence;
- snapshot fallback;
- replay with zero visible events;
- overlapping explicit replay;
- additive unknown extension fields.

Apply a complete EventBatch transactionally.

Validate:

- non-empty events array;
- outer fromSequence equals first inner sequence;
- outer toSequence equals last inner sequence;
- sequences are nondecreasing in expanded representation;
- legacy occurrence sequences are strictly increasing;
- repeated sequences remain one adjacent occurrence group;
- no sequence regresses behind the applicable cursor;
- stable event data conforms to its schema.

Unknown safe `codex.extension` information remains bounded and observable.

An unknown top-level event type outside the allowed compatibility mechanism is
a protocol/state failure rather than a silently invented state transition.

Do not synthesize state changes from successful ordinary command results.

State authority comes from snapshot and event synchronization.

======================================================================
35. State memory and immutable snapshots
======================================================================

State update algorithm:

1. copy/share current immutable implementation;
2. create a private mutable candidate;
3. apply and validate the complete message/batch;
4. calculate bounded state size;
5. reject if the configured state limit is exceeded;
6. commit one new immutable State;
7. invoke callbacks.

A retained old State must not observe later mutations.

Use copy-on-write or equivalent structural sharing.

Do not require a deep copy of the complete model merely to call
`Client::state()`.

Applications may intentionally retain several State copies; that application
memory is outside the SDK's single-current-state bound.

======================================================================
36. Threading and execution model
======================================================================

The SDK is single-threaded and event-loop friendly.

It creates:

- no thread;
- no worker;
- no mutex-based callback executor;
- no polling loop;
- no sleep.

All public lifecycle, receive, submission, and close calls for one Client must
occur on the same serialized execution context.

Document this as an execution-affinity contract.

Callbacks execute on that same context.

Callbacks may:

- inspect State;
- submit another operation;
- request close.

Reentrant submissions are deferred until the current message dispatch and
callback frame complete.

No callback is invoked while the internal state is half-mutated.

A transport `send` callback must not synchronously call `Connection::receive`
for the same Client before returning.

Contain callback and transport exceptions.

======================================================================
37. Disconnect and reconnect semantics
======================================================================

On unexpected disconnect:

- mark State stale;
- clear SessionInfo;
- clear current controller ownership for this session;
- complete every pending operation exactly once with transport/client failure;
- retain state and synchronizedThrough for possible replay;
- invoke connection-state callbacks;
- permit a new Connection.

Do not:

- stop codex-backend;
- stop the provider;
- clear state immediately unless projection continuity changed;
- retry pending commands;
- assume the backend did not execute a command;
- restore the old session ID;
- restore controller ownership.

On reconnect:

- authenticate again;
- create a new server session;
- resume only when continuity rules allow;
- accept replay or snapshot fallback;
- become Ready only after SyncComplete.

======================================================================
38. Client close and destruction
======================================================================

`Client::close()` is idempotent.

It:

1. enters Closing;
2. rejects new operations;
3. completes pending operations exactly once with Client Closed;
4. requests physical transport close;
5. detaches the active Connection;
6. enters Closed.

Client destruction performs the same bounded teardown if close was not called.

Contain exceptions from user callbacks during destruction.

State copies retained by applications remain valid.

======================================================================
39. Security requirements
======================================================================

The SDK must never deliberately log:

- bearer credentials;
- access tokens;
- secret answers;
- complete Hello;
- complete command payloads;
- command output;
- reasoning text;
- arbitrary model text.

Authorized State may contain potentially sensitive projected text.

The SDK must not call that data secret-free.

It is the application’s responsibility to protect persisted State and UI logs.

Authentication material appears only in Hello or an explicitly authorized
reverse authentication response.

No credential enters:

- replay cursor metadata;
- SessionInfo;
- State;
- diagnostics;
- request-ID generation;
- capability negotiation;
- transport endpoint names.

Errors and diagnostics must remain bounded.

Unknown error details must be treated as potentially sensitive.

======================================================================
40. `cpp_client_sdk` capability
======================================================================

A1.7c-1 changes the product capability:

    cpp_client_sdk

from:

    defined = true
    implemented = false

to:

    defined = true
    implemented = true

Do not change:

    multi_transport = false
    typescript_client_sdk = false
    browser_ui = false
    qt_ui = false

The 13 A1.7b mechanism capabilities remain unchanged.

After A1.7c-1:

    implemented mechanism capabilities:
        13

    implemented product capabilities:
        1

    total advertised implemented capabilities:
        14

Update generated counts so mechanism and product capability counts remain
separate and accurately named.

Do not rename the capability.

Do not change protocol version.

The C++ SDK does not request `cpp_client_sdk`; it is product metadata, not a
representation selector.

======================================================================
41. codex-backend-client migration
======================================================================

Migrate `codex-backend-client` to:

    AISuite::OpenAICodexFrontendClient

Remove application-owned protocol responsibilities:

- automatic Hello construction;
- client-side Frontend Protocol codec ownership;
- response correlation;
- initial synchronization state machine;
- explicit synchronization correlation;
- replay cursor tracking;
- sparse-sequence interpretation;
- ServerMessage dispatch as application logic;
- direct request-ID ownership.

The CLI retains application concerns:

- command parsing;
- terminal input;
- human presentation;
- JSON presentation;
- input queuing before Client becomes Ready;
- EOF drain;
- `new` compound workflow;
- transport selection;
- exit status.

`new` remains:

    thread.start
        ↓
    extract typed thread ID
        ↓
    turn.start

It does not become:

- a server method;
- a Frontend Protocol method;
- an SDK primitive;
- hidden current-thread state.

The CLI's EOF drain uses SDK pending-operation counts and operation callbacks.

It no longer parses raw responses to count pending commands.

`--json` uses the read-only `onProtocolMessage` callback.

If the diagnostic `raw` command is retained, it must use the restricted
generated operation API. It must not bypass SDK correlation or send an
arbitrary unknown method.

======================================================================
42. C++ CLI transport coverage
======================================================================

The SDK itself remains transport-neutral.

The migrated reference CLI demonstrates application-owned SNode.C adapters for
all compiled backend transports:

- Unix JSONL;
- IPv4 JSONL;
- IPv6 JSONL;
- IPv4 TLS JSONL;
- IPv6 TLS JSONL;
- RFCOMM JSONL;
- RFCOMM TLS JSONL;
- WebSocket;
- WSS.

Use native SNode.C configuration for:

- disabled/enabled state;
- remote host;
- port;
- Unix path;
- RFCOMM address/channel;
- TLS certificates and verification;
- writer limits;
- WebSocket endpoint.

Do not add a transport registry to the SDK.

The CLI may construct named transport instances with:

- Unix enabled by default;
- other transports disabled by default;
- exactly one active outgoing transport required.

All physical transports attach to the same Client API.

WebSocket/WSS use the standard SNode.C client WebSocket subprotocol mechanism
and request:

    codex

Do not implement WebSocket framing inside the SDK.

Do not require physical RFCOMM hardware in CI.

======================================================================
43. Public ABI strategy
======================================================================

Use PIMPL for:

- Client;
- Connection;
- State where appropriate.

Use stable pointer-sized façade layouts.

Do not add public virtual interfaces.

Do not expose implementation templates except:

- value-result templates;
- generated compile-time method traits;
- ordinary read-only projected-value templates.

Public domain data records may use normal C++ value semantics.

AISuite is still version 0.1.0, but the new API should not create avoidable ABI
fragility.

Keep the current AISuite SOVERSION unless an independent ABI policy decision
changes it.

The addition of the new library does not require changing Frontend Protocol or
the existing FrontendService target.

======================================================================
44. Public API usability example
======================================================================

A normal application should look conceptually like:

    namespace client =
        ai::openai::codex::frontend::client;

    client::Client sdk(
        client::ClientOptions{
            .credentialProvider = [] {
                return client::AuthenticationContext{
                    .credential =
                        ai::openai::codex::frontend::NoCredential{},
                    .continuityKey =
                        "verified-local:1000"
                };
            }
        },
        client::ClientCallbacks{
            .onStateUpdated =
                [](const client::StateUpdate& update) {
                    for (const auto& thread :
                         update.state.threads()) {
                        // typed state
                    }
                },
            .onSynchronized =
                [](const client::SynchronizationInfo& sync) {
                    // ready state and durable global cursor
                }
        });

    client::Connection protocolConnection =
        sdk.openConnection(
            client::TransportCallbacks{
                .send =
                    [&transport](client::OutboundMessage message) {
                        return transport.send(std::move(message));
                    },
                .close =
                    [&transport](std::string reason) {
                        transport.close(std::move(reason));
                    }
            });

    transport.attach(protocolConnection);

    sdk.threads().start(
        typed::ThreadStartParams{},
        [](const client::OperationResult<
               typed::ThreadStartResponse>& result) {
            if (!result) {
                // typed error
                return;
            }

            // typed result
        });

The application must not:

- create Hello;
- choose request IDs;
- decode Response JSON;
- infer sequence gaps;
- correlate SyncComplete;
- mutate the State model.

======================================================================
45. Reducer conformance fixtures
======================================================================

Create committed language-independent reducer fixtures for:

- initial legacy snapshot;
- initial expanded snapshot;
- live legacy events;
- all 26 expanded event families;
- sparse visible sequences;
- repeated same-occurrence sequence groups;
- hidden replay suffix;
- snapshot fallback;
- overlapping explicit replay;
- truncation and omission metadata;
- unknown safe extension fields.

Each fixture contains:

- initial client state;
- ordered server messages;
- expected final normalized state;
- expected visibleSequence;
- expected synchronizedThrough;
- expected typed change list;
- expected synchronization outcome.

The C++ SDK consumes these fixtures in A1.7c-1.

The TypeScript SDK reuses them in A1.7d.

Do not add TypeScript implementation now.

======================================================================
46. Required tests
======================================================================

Public target and API:

- installed target exists;
- installed headers are self-contained;
- no SNode.C network dependency leaks through the public target;
- Client is non-copyable/non-movable;
- Connection is move-only;
- State is immutable cheap-copy;
- façades have stable addresses.

Handshake:

- no Hello before transportConnected;
- Hello sent exactly once;
- bearer appears only in Hello;
- requested capabilities exact;
- credential-provider failure contained;
- unsupported protocol handled;
- missing required capability handled.

Lifecycle:

- exact state transitions;
- one active Connection;
- disconnect permits reconnect;
- close is terminal;
- callback exceptions contained.

Synchronization:

- initial snapshot;
- initial replay;
- replay with no visible events;
- hidden suffix advances synchronizedThrough;
- sparse visible sequence accepted;
- sequence regression rejected;
- snapshot fallback;
- no replay/snapshot mixing;
- explicit response-before-sync ordering;
- explicit callback only after SyncComplete;
- disconnect between response and SyncComplete fails operation.

Projection continuity:

- unchanged continuity allows replay;
- absent continuity requests snapshot;
- changed continuity requests snapshot;
- changed Welcome projection fingerprint forces snapshot refresh;
- richer retained state is not mixed into reduced projection.

State reducer:

- legacy snapshot;
- expanded snapshot;
- all legacy normalized events;
- all 26 expanded events;
- same-sequence atomic group;
- transaction rollback on malformed event;
- content replacement rather than append;
- overlapping replay deduplication;
- immutable retained State;
- state-size limit.

Commands:

- all 105 MethodIds have exactly one binding;
- all 86 provider operations have typed façade coverage;
- all 12 reverse methods have typed coverage;
- all seven native methods have coverage;
- parameter schema validation;
- result schema validation;
- exactly one completion;
- unsolicited response rejected;
- duplicate response rejected;
- wrong result MethodId rejected;
- pending operations fail exactly once on disconnect;
- no command retry after reconnect;
- request-ID exhaustion does not wrap.

Method discovery:

- available No rejects locally;
- permitted No rejects locally;
- Unknown defers to server;
- controller/readiness remain advisory;
- legacy 15-method fallback;
- 53/90 and 90/90 server inventories unchanged.

Security:

- no credential in State;
- no credential in diagnostics;
- outbound sensitive flag;
- secret reverse responses not retained;
- callback failures do not dump payloads.

CLI migration:

- command queue waits for SDK Ready;
- EOF drains SDK operations;
- `new` remains two typed operations;
- `--json` receives decoded messages in order;
- Unix acceptance;
- hardware-independent TCP/TLS/WebSocket/WSS acceptance;
- RFCOMM configuration/build coverage without physical hardware.

Capabilities:

- 13 mechanism capabilities unchanged;
- cpp_client_sdk true;
- multi_transport false;
- qt_ui false;
- typescript_client_sdk false;
- browser_ui false;
- total implemented advertised capabilities 14.

======================================================================
47. Documentation
======================================================================

Add:

    docs/ai/openai/codex/a1-7c-1-cpp-frontend-sdk.md

Document:

- target and namespace;
- ownership;
- transport contract;
- lifecycle;
- authentication;
- command result model;
- typed façades;
- state model;
- callbacks;
- sparse sequence semantics;
- synchronizedThrough;
- reconnect;
- projection continuity;
- error model;
- resource bounds;
- CLI migration;
- supported transports;
- ABI model;
- capability change.

Update:

- README;
- frontend-protocol-v1.md;
- frontend manifest;
- generated protocol documentation;
- codex-backend-client README;
- installed target inventory;
- roadmap status.

Do not claim:

- durable session restoration;
- automatic command retry;
- automatic controller reacquisition;
- provider neutrality;
- browser support;
- Qt UI completion;
- secret-free arbitrary text.

======================================================================
48. One-PR boundary
======================================================================

A1.7c-1 is one pull request.

Do not add an intermediate SDK-foundation PR.

The PR contains:

- complete C++ SDK;
- complete 105-method typed client surface;
- complete client state model;
- replay/reconnect;
- codex-backend-client migration;
- required tests;
- documentation;
- packaging.

It does not contain:

- codex-ui migration;
- Qt UI implementation;
- TypeScript;
- browser frontend;
- provider abstraction.

A1.7c-2 follows immediately.

======================================================================
49. Hard architectural completion gates
======================================================================

A1.7c-1 is not complete unless:

- `AISuite::OpenAICodexFrontendClient` exists;
- the SDK is transport-neutral;
- applications never construct Hello;
- applications never generate request IDs;
- applications never correlate responses;
- applications never interpret sequence gaps;
- applications never correlate SyncComplete;
- visibleSequence and synchronizedThrough are distinct;
- sparse visible sequences are accepted;
- hidden suffixes advance the cursor;
- reconnect authenticates again;
- reconnect creates a new session;
- pending commands are not restored or retried;
- controller ownership is not restored;
- state replacement and event application are transactional;
- State is immutable and cheap-copy;
- all stable state fields are typed;
- JSON remains only in explicitly opaque protocol positions;
- all 105 methods have one client binding;
- all 86 provider methods have typed façades;
- all 12 reverse methods have typed façades;
- all seven native methods have typed façades;
- no unrestricted string-and-JSON call exists;
- method/security policy is not duplicated;
- account.read uses normalized validated params;
- codex-backend-client uses the SDK;
- codex-backend-client no longer implements protocol correlation;
- the SDK creates no thread;
- no SDK transport registry exists;
- cpp_client_sdk is advertised;
- qt_ui remains false;
- TypeScript/browser capabilities remain false;
- multi_transport remains false;
- Frontend Protocol identity/version/message kinds remain unchanged;
- A1.7c-2, A1.7d, and provider abstraction have not started.

---

# Steering Addendum — Final A1.7c-1 Capability and Façade Corrections

The previously supplied A1.7c-1 architecture specification and implementation prompt remain normative except where this addendum explicitly overrides them.

This addendum replaces the earlier capability/façade steering addendum in full.

Do not change unrelated architecture, public API, ownership, lifecycle, state, replay, transport, packaging, commit, or validation requirements.

======================================================================

1. Preserve the three capability categories
   ======================================================================

Merged A1.7b distinguishes three capability categories:

1. static service mechanism capabilities;
2. the conditional topology capability `multi_transport`;
3. product capabilities.

Preserve that taxonomy.

Do not redefine `multi_transport` as one of the static mechanism capabilities.

The fixed A1.7b static mechanism inventory remains:

```
13 static service mechanism capabilities
```

`multi_transport` remains:

```
one conditional runtime-topology capability
```

Product capabilities remain a separate category:

```
cpp_client_sdk
typescript_client_sdk
browser_ui
qt_ui
```

A1.7c-1 changes only the build-derived truth of:

```
cpp_client_sdk
```

Do not use the phrase:

```
14 mechanism capabilities including multi_transport
```

Instead report:

```
static mechanisms:
    13

conditional topology:
    multi_transport = true or false

implemented products:
    cpp_client_sdk = true or false

total implemented advertised capabilities:
    sum of those categories
```

======================================================================
2. Preserve topology-derived `multi_transport`
==============================================

`multi_transport` remains runtime-topology derived exactly as in merged A1.7b.

Required behavior:

```
one declared/bound transport family:
    multi_transport = false

more than one declared/bound transport family:
    multi_transport = true
```

A1.7c-1 must not:

* remove the transport-family topology mechanism;
* replace topology-derived behavior with a static value;
* disable dynamic `multi_transport` advertisement;
* weaken or delete the existing A1.7b topology tests;
* reinterpret `multi_transport` as an SDK feature;
* include `multi_transport` in requested representation capabilities.

`multi_transport` remains independent of:

* connected-client count;
* C++ SDK construction;
* client-requested capabilities;
* UI availability.

======================================================================
3. Define the subject of `cpp_client_sdk`
=========================================

`cpp_client_sdk` describes the AISuite product build that produced the running
server.

It does not describe:

* whether the running `codex-backend` executable directly links the SDK;
* whether one particular process has loaded the SDK library;
* whether a connected client was itself implemented with the SDK;
* whether the source repository merely contains SDK source files.

Required behavior:

```
AISuite configured with the C++ Frontend SDK product enabled and built:
    cpp_client_sdk = true

AISuite configured with the C++ Frontend SDK product disabled or excluded:
    cpp_client_sdk = false
```

The capability is compiled into the Frontend Protocol/FrontendService side of
the AISuite product.

Because the dependency direction is:

```
frontend-client
    depends on
frontend protocol/service library
```

the frontend library cannot infer SDK presence through link closure.

The top-level AISuite CMake configuration must therefore provide the
build-derived truth explicitly, using the existing generated capability
authority and a compile definition/configured generated value.

Do not add:

* a runtime registry;
* dynamic library probing;
* filesystem probing;
* process-link-map inspection;
* an application-owned capability override.

The C++ SDK itself must not request `cpp_client_sdk`; it is product metadata,
not a representation selector.

======================================================================
4. Correct capability accounting
================================

Use these three named categories:

```
static mechanisms:
    13

conditional topology:
    multi_transport = 0 or 1

implemented products:
    cpp_client_sdk = 0 or 1
```

The correct totals are:

```
SDK built, one transport family:

    static mechanisms:
        13

    conditional topology:
        0

    implemented products:
        1

    total implemented advertised:
        14

SDK built, multiple transport families:

    static mechanisms:
        13

    conditional topology:
        1

    implemented products:
        1

    total implemented advertised:
        15

SDK disabled, one transport family:

    static mechanisms:
        13

    conditional topology:
        0

    implemented products:
        0

    total implemented advertised:
        13

SDK disabled, multiple transport families:

    static mechanisms:
        13

    conditional topology:
        1

    implemented products:
        0

    total implemented advertised:
        14
```

Preserve:

```
typescript_client_sdk = false
browser_ui = false
qt_ui = false
```

Do not pin one unconditional total across all build and runtime configurations.

Do not change the existing definition of the 13 static mechanism capabilities.

======================================================================
5. Update the merged capability-taxonomy test explicitly
========================================================

Update the merged test that currently partitions generated capabilities using
logic equivalent to:

```
if implementedByCurrentRuntime:
    mechanisms
else if key == multi_transport:
    conditional topology
else:
    future products
```

That classification becomes invalid once `cpp_client_sdk` can be build-derived
true, because a true product capability would otherwise be incorrectly counted
as a mechanism.

Update the test and any shared helper so capability classification uses
explicit category metadata or another deterministic authoritative category,
not current implementation truth.

The final taxonomy must classify:

```
the 13 static mechanisms:
    as static mechanisms

multi_transport:
    as conditional topology

cpp_client_sdk:
    as product

typescript_client_sdk:
    as product

browser_ui:
    as product

qt_ui:
    as product
```

This applies whether each capability is currently true or false.

Do not infer category from:

```
implementedByCurrentRuntime
```

A capability's category and its current implementation truth are independent
facts.

Explicitly update:

```
CodexFrontendAuthorizationMetadataTest
```

or the actual current merged test carrying the equivalent assertion.

Preserve its substantive A1.7b guarantees:

* exactly 13 static service mechanisms;
* `multi_transport` remains topology-derived;
* product capabilities remain separately classified;
* security inventory remains unchanged.

Do not weaken the existing native transport/topology tests.

======================================================================
6. Request exactly five representation capabilities
===================================================

The default `ClientOptions::requestedCapabilities` must contain exactly:

```
complete_backend_domains
dedicated_pending_requests
dedicated_notification_events
complete_thread_items
scope_projected_state
```

These are the complete Frontend Protocol v1 representation-selection
capabilities.

Do not include as representation requests:

```
method_discovery
security_scopes
complete_provider_operations
complete_reverse_requests
conditional_filesystem
conditional_command_execution
authenticated_frontend
provider_lifecycle
multi_transport
cpp_client_sdk
typescript_client_sdk
browser_ui
qt_ui
```

The server may advertise those capabilities.

The SDK records them as observed mechanism, topology, or product facts.

They do not select the client-visible state representation.

======================================================================
7. Required capabilities
========================

`ClientOptions::requiredCapabilities` may contain any capability an application
requires for correct operation.

Distinguish:

```
required representation capabilities
```

from:

```
required observed mechanism/topology/product capabilities
```

Required behavior:

1. Requested representation capabilities are sent in Hello.
2. A required representation capability must also be present in the requested
   representation set.
3. Required non-representation capabilities are not added to the Hello
   representation request merely because they are required.
4. After Welcome, validate every required capability against the server's
   advertised/permitted truth.
5. A missing required capability prevents Ready and terminates synchronization
   with a typed error.

For Frontend Protocol v1, the five representation capabilities listed above
are the complete representation set. Therefore, the subset rule is currently
automatically satisfied by the default set.

Keep the rule because a future additive representation capability must not
become required without also being requested.

Do not let a product, topology, or invocation-policy capability accidentally
alter the selected state representation.

======================================================================
8. Deterministic projection-continuity fingerprint
==================================================

The projection-continuity fingerprint must be deterministic and identical
across the future C++ and TypeScript SDK implementations.

Do not use judgment-based conditions such as:

```
include available methods only where they materially constrain state
```

Include the following inputs unconditionally when supplied:

1. requested representation capabilities;
2. selected/permitted representation capabilities;
3. credential continuityKey;
4. permitted scope set;
5. permitted method set;
6. available method set;
7. other explicit Frontend Protocol v1 projection/representation metadata.

Canonicalize all sets before hashing/comparison:

* sort by stable protocol identity;
* remove duplicates through validation;
* preserve explicit absent-versus-present distinction;
* use one documented language-independent serialization form.

Do not include merely because they changed:

* conditional_filesystem capability truth;
* conditional_command_execution capability truth;
* provider_lifecycle capability truth;
* authenticated_frontend capability truth;
* complete_provider_operations capability truth;
* complete_reverse_requests capability truth;
* method_discovery capability truth;
* security_scopes capability truth;
* multi_transport;
* cpp_client_sdk;
* typescript_client_sdk;
* browser_ui;
* qt_ui.

The available and permitted method sets are included independently of the
capability flag that announced method discovery.

Reason:

```
exact discovered method sets may change the effective application-visible
operation/request surface and must produce deterministic reconnect
behavior across SDK languages.
```

A changed deployment gate capability alone must not force a snapshot when the
actual available/permitted method and scope sets, representation, and
information ceiling remain unchanged.

A changed permitted scope or method set must invalidate projection continuity.

Preserve:

```
when continuity cannot be proven,
request a fresh snapshot rather than mixing retained projections.
```

======================================================================
9. SessionInfo capability fields and fingerprint inputs
=======================================================

`SessionInfo` must distinguish:

```
requestedRepresentationCapabilities

selectedRepresentationCapabilities

observedMechanismCapabilities

observedTopologyCapabilities

observedProductCapabilities

availableMethods

permittedMethods

permittedScopes
```

The public API may additionally provide a unified read-only capability query.

Do not flatten all capability categories into one field with ambiguous
semantics.

The projection-continuity fingerprint consumes exactly:

```
requestedRepresentationCapabilities
selectedRepresentationCapabilities
availableMethods
permittedMethods
permittedScopes
credential continuityKey
explicit projection metadata
```

It does not consume:

```
observedMechanismCapabilities
observedTopologyCapabilities
observedProductCapabilities
```

except where an item is independently represented in one of the exact input
sets above.

Document this mapping next to both:

* SessionInfo;
* the projection fingerprint implementation.

Add one shared helper/fixture authority so the C++ implementation and later
TypeScript implementation cannot drift.

======================================================================
10. Clarify façade and method ownership
=======================================

The public façade inventory is:

Native façades:

```
Controller
Provider
Synchronization
```

Domain façades:

```
Accounts
Apps
Commands
Configuration
ExternalAgents
Feedback
Filesystem
Hooks
Marketplace
Mcp
Models
PermissionProfiles
Plugins
Requests
Reviews
Skills
Threads
Turns
WindowsSandbox
```

`client::Requests` is the one domain façade owning all 12 reverse
response/rejection methods.

The exact method-accounting relation is:

```
Controller:
    2 native methods

Provider:
    3 native methods

Synchronization:
    2 native methods

Native total:
    7

Requests:
    12 reverse methods

Remaining 18 domain façades:
    86 provider-backed methods

Overall total:
    105
```

Do not introduce:

* a separate `ReverseRequests` façade;
* 12 reverse façade classes;
* reverse operations on `Client` directly;
* reverse operations spread arbitrarily across provider domain façades.

Use the current generated MethodId registry as authority for the exact 12
reverse method names and identities.

The C++ binding generator must assert:

```
Controller + Provider + Synchronization:
    7

Requests:
    12

remaining domain façades:
    86

total:
    105
```

======================================================================
11. Preserve item-content replacement
=====================================

Keep the existing state-reducer rule:

```
ItemContentUpdated replaces the accumulated bounded content channel value.
```

It is not a delta append.

The server already emits the accumulated bounded content.

Appending would duplicate or corrupt state, especially when the server has
truncated the accumulated value.

Preserve:

* replacement semantics;
* truncation counters;
* transactional application;
* immutable prior State values;
* reducer conformance fixtures for repeated content updates.

======================================================================
12. Test all four capability combinations without a second build tree
=====================================================================

Do not create a second full AISuite build merely to test SDK-disabled product
truth.

Extract or use a deterministic capability-computation function with explicit
inputs conceptually equivalent to:

```
CapabilityTruth computeCapabilities(
    bool cppClientSdkBuilt,
    std::size_t declaredTransportFamilies);
```

or an equivalent strongly typed design.

The real build supplies:

```
cppClientSdkBuilt
```

from the top-level AISuite build configuration.

The real runtime supplies:

```
declaredTransportFamilies
```

from the existing A1.7b topology mechanism.

Unit-test all four combinations against the computation:

1. SDK built, one family:
   static mechanisms 13
   topology 0
   products 1
   total 14

2. SDK built, multiple families:
   static mechanisms 13
   topology 1
   products 1
   total 15

3. SDK disabled, one family:
   static mechanisms 13
   topology 0
   products 0
   total 13

4. SDK disabled, multiple families:
   static mechanisms 13
   topology 1
   products 0
   total 14

End-to-end coverage:

* cases 1 and 2 must run through the ordinary primary SDK-enabled build;
* existing A1.7b topology tests continue to prove the single-family and
  multi-family runtime behavior;
* cases 3 and 4 are covered through injected unit-level build truth;
* do not configure or compile a second full SDK-disabled build solely for this
  matrix.

A normal existing feature-off/package configuration test may still validate an
SDK-disabled build if it already belongs to the standard CI matrix, but this
addendum does not require another build tree.

======================================================================
13. Required capability and continuity tests
============================================

Add tests proving:

* Hello requests exactly the five representation capabilities;
* no mechanism, topology, or product capability is sent as a representation
  request;
* required representation capability must be requested;
* required observed capability is checked after Welcome but not added to the
  representation request;
* changed representation capability invalidates replay continuity;
* changed permitted scope set invalidates continuity;
* changed permitted method set invalidates continuity;
* changed available method set invalidates continuity;
* changed continuityKey invalidates continuity;
* changed `conditional_filesystem` capability truth alone does not invalidate
  continuity when exact method/scope/representation sets remain unchanged;
* changed `conditional_command_execution` capability truth alone follows the
  same rule;
* changed `multi_transport` does not invalidate projection continuity;
* changed `cpp_client_sdk` does not invalidate projection continuity;
* SessionInfo category fields are populated correctly;
* `Requests` owns exactly all 12 reverse bindings;
* the 7/12/86/105 façade accounting is exact.

Use shared language-independent fingerprint fixtures where practical so A1.7d
can reuse them.

======================================================================
14. Documentation and completion gates
======================================

Update:

* normative architecture document;
* implementation report;
* generated capability documentation;
* capability metadata tests;
* PR body;
* final response;
* completion gates.

State exactly:

```
static service mechanisms:
    13

conditional topology capability:
    multi_transport

conditional product capability introduced by A1.7c-1:
    cpp_client_sdk
```

Do not state:

```
mechanism capabilities = 14
```

Do not state unconditionally:

```
total implemented capabilities = 14
```

Report the actual total according to:

```
build-derived cpp_client_sdk truth
+
topology-derived multi_transport truth
+
13 static mechanisms
```

Explicitly report the primary CI runtime topology and its expected total.

Update the merged metadata-taxonomy test rather than weakening it.

======================================================================
15. Precedence
==============

This addendum overrides conflicting statements in:

* the A1.7c-1 architecture specification;
* the A1.7c-1 execution prompt;
* the previous steering addendum.

All non-conflicting requirements remain unchanged.

Do not change anything else merely because this addendum was supplied.

======================================================================
Live-integration correction — live Snapshot barriers and thread-list state
======================================================================

This correction is normative for the unreleased A1.7c-1 Frontend Protocol v1
and C++ SDK surface. It changes no protocol identity, version, message kind, or
method identity.

A bare `Snapshot` received after a connection has reached `Ready` is a live
snapshot barrier. It is a bounded authoritative state replacement selected by
FrontendService when one atomic visible occurrence cannot fit an event batch.
It has no preceding `Welcome` and no following `SyncComplete`.

The Client must decode and validate a live snapshot transactionally, preserve
the active physical `SessionInfo` and projection fingerprint, remain `Ready`,
and preserve pending command correlation. The snapshot sequence becomes both
`visibleSequence` and `synchronizedThrough`. A lower sequence is state
divergence, an equal sequence is an idempotent authoritative replacement, and
a higher sequence advances the reconnect cursor. The update uses
`UpdateCause::SnapshotFallback`, emits one `StateReplacedChange` and, only when
the cursor advances, one `CursorAdvancedChange`. It invokes `onStateUpdated`,
then `onCursorAdvanced` when applicable, and exposes the accepted wire message
through `onProtocolMessage`; it does not invoke `onSynchronized`.

Expanded snapshots contain an authoritative `threadList` projection with
`hasLoadedPage`, `complete`, `pagesLoaded`, optional forward/backward cursors,
and source stamp metadata when present. Snapshot and event encoding share one
projection implementation.

Frontend Protocol v1 has 26 capability-gated expanded event families. The
additive twenty-sixth family is:

```
threadList.updated
```

Its stable data object contains exactly one required `threadList` wrapper.
Legacy `thread.list.updated` continues unchanged for legacy connections. An
expanded connection receives one compact `threadList.updated` occurrence;
the metadata occurrence must not expand into every retained thread and must
not fabricate a thread for an empty page. Threads returned by the actual page
remain independent ordinary `thread.upserted` canonical occurrences.

The reference application preserves the first concrete SDK/transport failure
through lifecycle presentation. A later physical disconnect must not replace
it with a generic close message. Intentional `quit` and framework signal
shutdown are classified before transport detachment; remote EOF while the
application is otherwise running remains an error. No complete protocol
payload, authentication credential, or command parameter is logged.

======================================================================
Holistic correction — command, connection, and process lifetimes
======================================================================

The reference client has three independent lifetimes:

1. a command lifetime, ending in exactly one operation completion;
2. a physical-connection lifetime, ending when one transport attachment is
   closed or lost;
3. an application/process lifetime, ending only through an explicit
   application-termination condition.

The reference application's states are `Disconnected`, `Connecting`,
`Synchronizing`, `Ready`, `ShuttingDown`, and `Closed`. They are application
states, distinct from the SDK connection state machine. In particular,
`Disconnected` is running and nonterminal.

A normal command failure ends only that command. This includes a local parse
error, a pre-acceptance command-local submission rejection, `Response(ok=false)`,
and a rejected explicit snapshot or replay command. Transport/send rejection
remains a physical-connection failure, while temporary pending-capacity and
active-synchronization conditions remain bounded pre-acceptance deferrals. A
terminal command result removes accepted correlation exactly once, records
batch failure when stdin is being drained, and leaves an otherwise valid
connection `Ready`. It must not disable stdin, close the transport, or stop the
event loop. A non-closing protocol diagnostic follows the same disposition.

A malformed message, state divergence, invalid synchronization stream,
authentication failure, SDK-requested close, or physical transport loss may
and often must end the physical connection. Accepted pending operations fail
exactly once, retained State becomes stale, and unsubmitted application queue
entries for that attempt are discarded rather than retried. The application
enters `Disconnected` and remains alive with stdin enabled. `help`, `watch`,
`reconnect`, and `quit` remain local and usable; a remote command entered while
Disconnected is rejected with reconnect guidance and is not retained for
surprise execution later.

The application terminates only for `quit`, an application-termination signal,
true stdin EOF after controlled drain, unrecoverable startup/input/event-loop
infrastructure failure, or an internal invariant failure that makes the
application object unusable. Unintentional transport disconnect must not call
the process-stop operation. Intentional shutdown is marked before transport
teardown and remains idempotent while connecting, upgrading WebSocket, Ready,
or already Disconnected.

`reconnect` is explicitly application-owned. From Disconnected it creates one
new physical transport attempt from the already selected configuration and
attaches a new `Connection` generation to the same SDK `Client`. The SDK alone
chooses replay or snapshot from retained continuity. No automatic reconnect or
backoff exists; no prior command is resubmitted; controller ownership is not
reacquired. Connecting, Authenticating, and Synchronizing reject a parallel
attempt; Ready reports that it is already connected; shutdown rejects it.

Every application-owned physical attempt has one immutable generation. Native
socket factories, HTTP upgrade callbacks, WebSocket subprotocol creation, and
detach callbacks must consume the generation of their originating attempt,
never a later mutable "current generation". A WebSocket generation is also
bound to the exact originating socket connection before upgrade; a late
factory from a retired transport cannot claim a newer prepared generation.
Stale callbacks are ignored or close only their stale transport and cannot
attach a second SDK `Connection`, retire a newer attempt, or affect application
lifecycle state.

The application-owned queue is finite and input-ordered. Its configurable
defaults are:

```
maximumQueuedCommands:     256
maximumQueuedCommandBytes: 16 MiB
```

They are exposed as `--maximum-queued-commands` and
`--maximum-queued-command-bytes`; zero means zero queue capacity. Checked
arithmetic accounts retained command-owned UTF-8 input, and overflow rejects
the newest entry without evicting an older one. Connecting and Synchronizing
may queue remote commands. Ready submits immediately, except that a command
not yet accepted may remain queued for temporary pending-capacity or
active-synchronization conditions. Disconnected never queues remote commands.
Queue accounting is restored transactionally when a first submission is
deferred and released on dequeue or discard.

The compound `new` workflow has one explicit active record, separate from later
queued `new` commands. A thread-start failure ends only that workflow. A
turn-start failure preserves the successfully created thread, performs no
rollback or retry, clears the active workflow, and permits the next queued
command to proceed. A connection failure clears the active workflow without
resubmission. Every terminal operation callback, successful or failed, makes
released pending capacity available to the ordered queue.

True stdin EOF stops new input but does not restore fail-fast behavior. The
application drains all queued entries and accepted operations, continues after
local and remote command failures, and exits once the queue, SDK pending set,
active `new`, and explicit synchronization are all empty. The final status is
nonzero if any command failed. Connection loss during drain explicitly fails
unsubmitted entries and completes nonzero without waiting for manual
reconnect. `EAGAIN` and `EWOULDBLOCK` remain temporary lack of input, never
synthetic EOF.

======================================================================
Holistic correction — exact expanded-event projection identity
======================================================================

Every identity-bearing expanded event obtains its stable identity from the
exact canonical occurrence, and any snapshot-resolved entity must carry that
same identity. Projection must never choose the first, last, newest, or merely
nonempty retained entity as a substitute. It must not recursively search
arbitrary descendants for a convenient field named `id`.

The reviewed normalized identity paths are:

```
thread.upserted          data.thread.id
thread.removed           data.threadId
turn.upserted            data.turn.id + data.turn.threadId
item.upserted            data.item.id + data.threadId + data.turnId
item.content.updated     data.itemId + data.threadId + data.turnId
process.updated          data.process.processHandle
filesystemWatch.updated  data.filesystemWatch.watchId
fuzzySearch.updated      data.fuzzySearch.sessionId
activity.updated         data.activity.key
notice.added             exact data.notice occurrence
```

Reviewed `codex.extension` mappings use their corresponding exact `params`
paths, such as `params.thread.id`, `params.threadId`, `params.turn.id`,
`params.item.id`, `params.processHandle`/`params.processId`, `params.watchId`,
and `params.sessionId`. They do not use a generic recursive identity helper.

Notification mapping is semantic rather than prefix-only. `thread/deleted`
maps to `thread.removed`. Only the five accumulated-content notifications
`item/agentMessage/delta`, `item/commandExecution/outputDelta`,
`item/fileChange/outputDelta`, `item/reasoning/summaryTextDelta`, and
`item/reasoning/textDelta` map to `item.content.updated`. Item lifecycle,
terminal-interaction, patch, progress, plan, and summary-part notifications map
to `item.upserted`; they must not fabricate a content-channel replacement.

The stable `commandOutput` member has a discriminator-specific information
ceiling: a `commandExecution` item requires `command_execution`, while a
`fileChange` item requires `filesystem_write`. Snapshot and event projection
apply this rule by walking the exact typed items, independently of the bounded
generic projection-rule count. A missing, unknown, malformed, or conflicting
type is visible only when both scopes are present; otherwise `commandOutput`
is omitted and reported. Tail items must therefore remain protected even when
a state contains more items than the generic rule budget.

The 26 expanded families remain classified as follows:

- aggregate/singleton replacement: provider, controller, sessions,
  thread-list, pending requests, account, models, configuration, reviews,
  integrations, plugins, skills, MCP, platform, capacity, and diagnostics;
- exact entity: thread upsert/removal, turn upsert, item upsert/content,
  process, filesystem watch, fuzzy search, and activity;
- exact occurrence: notice addition.

If an exact target cannot be found, projection may use only a
contract-approved minimum that retains the proven same identity; otherwise the
canonical occurrence requires bounded Snapshot fallback. Missing identity may
never fabricate `"unavailable"` as an upsert ID or select an unrelated
`.back()` element. Live and replay projection use the same canonical record and
therefore produce the same identity and data under the same scope.

The real defect that froze this rule returned 25 distinct page-thread IDs, but
all 25 `thread.upserted` events projected one retained tail thread. The
canonical occurrence contained `data.thread.id`; generic extraction ignored
that exact path and the projection substituted `snapshot.threads.back()`.
The corrected path emits the exact 25 unique page identities and matching
per-ID content, followed by one compact `threadList.updated`; unrelated
retained threads remain unchanged. This changes neither protocol identity nor
version, the eight message kinds, 105 methods, or 26 expanded families.
Reconnect and queue policy remain reference-application concerns, so this
correction changes no public SDK API or ABI.

======================================================================
Frozen server-projection representation boundary
======================================================================

Canonical occurrences are authoritative for occurrence identity only. A
backend or legacy occurrence object is not an `ExpandedThreadItem` and is
never authoritative for Frontend Protocol representation. In particular, an
`item.updated` occurrence contributes exactly `threadId`, `turnId`, and
`item.id`; the captured backend Snapshot must resolve that complete parent
triple, and the resolved item must pass through the one shared expanded-item
projector used by snapshots, live events, and journal replay. Item IDs are not
assumed globally unique. A missing parent or item, conflicting identity, or
unsafe projection requires bounded Snapshot fallback rather than an unrelated
lookup or a fabricated item.

The projector owns stable type conversion, bounded fields and detail,
timestamps, truncation and invalidation metadata, generation/freshness, and
scope compatibility. The reviewed backend spellings `agent_message`,
`user_message`, `command_execution`, `file_change`, `web_search`, and
`tool_call` therefore cannot cross the expanded wire boundary as item
discriminators. They map to the existing 18-value Frontend Protocol
vocabulary; `tool_call` maps to `mcpToolCall` only with the reviewed MCP server
marker and otherwise maps to `dynamicToolCall`. No nineteenth `unknown`
discriminator is introduced. A genuinely unsupported backend item produces no
expanded item event and is omitted from an expanded Snapshot only through its
bounded truncation accounting.

The producer and consumer both enforce the generated authority. Before the
generic server codec serializes any event whose type is a recognized expanded
family, it validates the complete event against
`#/$defs/ExpandedFrontendEvent`. A local validation failure queues no bytes and
closes the affected physical connection through the bounded internal-failure
path; the SDK retains its independent strict validation. Diagnostics may name
the exact failing discriminator path and a safely escaped, UTF-8-prefix-bounded
string value, but never the surrounding item or event payload.
