/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_INTERNAL_SERVER_SERVERCORE_H
#define AI_OPENAI_CODEX_FRONTEND_INTERNAL_SERVER_SERVERCORE_H

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/Security.h"
#include "ai/openai/codex/frontend/internal/model/Journal.h"
#include "ai/openai/codex/frontend/internal/model/Model.h"
#include "ai/openai/codex/frontend/internal/model/Occurrence.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ai::openai::codex::frontend::internal::server {

    class ServerCore;

    class ConnectionIdentity {
    public:
        constexpr ConnectionIdentity() noexcept = default;

        explicit constexpr ConnectionIdentity(std::uint64_t value) noexcept
            : identity(value) {
        }

        [[nodiscard]] constexpr std::uint64_t value() const noexcept {
            return identity;
        }

        auto operator<=>(const ConnectionIdentity&) const = default;

    private:
        std::uint64_t identity = 0;
    };

    enum class ProviderLifecycleAction { Start, Stop, Restart };

    enum class OccurrenceEntityKind {
        BackendLifecycle,
        Diagnostic,
        Thread,
        Turn,
        Item,
        ItemContent,
        PendingRequest,
        Controller,
        Session,
        CodexExtension
    };

    // A semantic dirty key is deliberately separate from an occurrence's
    // expanded event-family count. Replacing a pending value for the same key
    // does not consume another maxDirtyEntities slot.
    struct OccurrenceCoalescingKey {
        OccurrenceEntityKind kind = OccurrenceEntityKind::BackendLifecycle;
        std::optional<model::ThreadIdentity> threadId;
        std::optional<model::TurnIdentity> turnId;
        std::optional<model::ItemIdentity> itemId;
        std::optional<model::PendingRequestIdentity> pendingRequestId;
        std::string entityId;
        std::string channel;

        [[nodiscard]] bool valid() const noexcept;

        auto operator<=>(const OccurrenceCoalescingKey&) const = default;
    };

    enum class OccurrenceFlushUrgency { Deferred, Immediate };
    enum class OccurrenceStageStatus { Accepted, SnapshotRequired, InvalidOccurrence, AllocationFailure };

    struct OccurrenceStageResult {
        OccurrenceStageStatus status = OccurrenceStageStatus::InvalidOccurrence;
        bool scheduleRequired = false;
        bool immediateFlush = false;

        [[nodiscard]] bool accepted() const noexcept {
            return status == OccurrenceStageStatus::Accepted;
        }
    };

    // One BackendCore observer callback is one semantic staging transaction.
    // Keeping the whole callback under a single ServerCore dispatch scope
    // preserves coalescing even when an injected scheduler runs inline.
    struct OccurrenceStageRequest {
        OccurrenceCoalescingKey key;
        model::OccurrenceDraft occurrence;
        OccurrenceFlushUrgency urgency = OccurrenceFlushUrgency::Deferred;
    };

    struct CommandToken {
        ConnectionIdentity connection;
        std::uint64_t connectionGeneration = 0;
        std::string requestId;
        generated::MethodId method = generated::MethodId::ControllerAcquire;

        bool operator==(const CommandToken&) const = default;
    };

    struct BackendInvocation {
        CommandToken token;
        model::SessionIdentity session;
        FrontendPrincipal principal;
        generated::DefinedCommand command;
    };

    // Identifies the exact physical frontend generation for the lifetime of
    // its authenticated backend session.  The semantic session identity is
    // intentionally independent from BackendCore's private SessionId.
    struct FrontendSessionToken {
        ConnectionIdentity connection;
        std::uint64_t connectionGeneration = 0;
        model::SessionIdentity session;

        bool operator==(const FrontendSessionToken&) const = default;
    };

    enum class BackendSubmitStatus { Accepted, Unavailable, CapacityExceeded, Rejected };

    struct BackendCommandSuccess {
        generated::CompleteCommandResult result;
    };

    struct BackendCommandFailure {
        ErrorCode code = ErrorCode::RemoteAppServerError;
        std::string message;
        std::optional<model::SafeDetail> details;
    };

    using BackendCompletionValue = std::variant<BackendCommandSuccess, BackendCommandFailure>;

    struct BackendCompletion {
        CommandToken token;
        BackendCompletionValue value;
    };

    // BackendPort is the only server-core dependency on the application/backend
    // side. Implementations must not retain authentication credentials, and
    // completions from an older token generation are harmlessly ignored.
    class BackendPort {
    public:
        BackendPort() = default;
        BackendPort(const BackendPort&) = delete;
        BackendPort(BackendPort&&) = delete;
        BackendPort& operator=(const BackendPort&) = delete;
        BackendPort& operator=(BackendPort&&) = delete;
        virtual ~BackendPort() = default;

        [[nodiscard]] virtual bool providerReady() const noexcept = 0;
        [[nodiscard]] virtual model::CanonicalSnapshot snapshot() const = 0;
        [[nodiscard]] virtual BackendSubmitStatus submit(BackendInvocation invocation) = 0;
        [[nodiscard]] virtual bool performProviderLifecycleAction(ProviderLifecycleAction action) = 0;

        virtual void bind(ServerCore&) noexcept {
        }
        virtual void unbind(ServerCore&) noexcept {
        }

        [[nodiscard]] virtual bool sessionOpened(const FrontendSessionToken& token, const FrontendPrincipal& principal) {
            sessionOpened(token.session, principal);
            return true;
        }
        virtual void sessionClosed(const FrontendSessionToken& token) noexcept {
            sessionClosed(token.session);
        }

        // Compatibility hooks for the P2 test ports. Production adapters use
        // the generation-aware overloads above.
        virtual void sessionOpened(const model::SessionIdentity&, const FrontendPrincipal&) {
        }
        virtual void sessionClosed(const model::SessionIdentity&) noexcept {
        }
        // Retained only as an internal source-compatibility hook for P2 test
        // ports. ServerCore never uses it as a production controller contract.
        virtual void controllerChanged(const std::optional<model::SessionIdentity>&) noexcept {
        }
    };

    struct ConnectionClose {
        std::string reason;
        std::optional<ErrorCode> protocolCode;
        bool clean = false;

        bool operator==(const ConnectionClose&) const = default;
    };

    struct ConnectionCallbacks {
        // Returning false means the transport cannot accept another message
        // without violating its own queue bound. Only this connection closes.
        using Send = std::function<bool(const ServerMessage&)>;
        using Closed = std::function<void(const ConnectionClose&)>;

        Send onMessage;
        Closed onClosed;
    };

    using Scheduler = std::function<void(std::function<void()>)>;
    using TimerCancellation = std::function<void()>;
    using TimerScheduler = std::function<TimerCancellation(std::uint64_t, std::function<void()>)>;
    using Authenticator = std::function<AuthenticationResult(const FrontendPeerContext&, const AuthenticationCredential&)>;
    using InvocationPolicy = std::function<bool(const FrontendPrincipal&, std::string_view, const Json&)>;

    struct ServerCoreOptions {
        std::size_t journalMaximumEntries = DefaultJournalMaxEntries;
        std::size_t journalMaximumBytes = DefaultJournalMaxBytes;
        model::FrontendSequence journalInitialSequence;
        std::size_t maxDirtyEntities = DefaultMaxDirtyEntities;
        // This bounds already-sequenced groups awaiting a delivery turn. It is
        // not the dirty/coalescing-key bound above.
        std::size_t maxPendingDeliveryGroups = DefaultMaxDirtyEntities;
        std::size_t maxOutboundMessagesPerConnection = DefaultFrontendServiceMaxOutboundMessages;
        std::size_t maxOutboundBytesPerConnection = DefaultFrontendServiceMaxOutboundBytes;
        std::size_t maxMessagesPerDelivery = DefaultFrontendServiceMaxMessagesPerDelivery;
        std::size_t maxEventsPerBatch = DefaultBatchMaxEvents;
        std::size_t maxBatchBytes = DefaultBatchMaxBytes;
        std::size_t maxConnections = 128;
        std::size_t maxUnauthenticatedConnections = 16;
        std::size_t maximumInboundMessageBytes = 1024U * 1024U;
        std::size_t maxInboundMessagesPerSecond = 50;
        std::size_t maxInboundBurst = 100;
        std::size_t maxOutstandingCommandsPerConnection = 256;
        std::size_t maximumFailedAuthenticationsPerPeer = 3;
        std::uint64_t failedAuthenticationWindowMs = 60000;
        std::uint64_t handshakeTimeoutMs = 10000;
        bool allowVerifiedLocalTrust = true;
        bool allowInsecureLocalTrust = false;
        std::optional<std::uint64_t> trustedLocalUserId;
        bool enableFilesystemReadMethods = false;
        bool enableFilesystemWriteMethods = false;
        bool enableCommandExecutionMethods = false;
        std::vector<FrontendCapability> requiredClientCapabilities;
        // Static mechanisms implemented by the core are always advertised.
        // Adapters add product capabilities, notably CppClientSdk, here.
        std::vector<FrontendCapability> implementedCapabilities;
        std::string serverVersion;
        InvocationPolicy filesystemReadPolicy;
        InvocationPolicy filesystemWritePolicy;
        InvocationPolicy commandExecutionPolicy;
        Authenticator authenticator;
        Scheduler scheduler;
        TimerScheduler timerScheduler;
        std::function<std::uint64_t()> monotonicClockMs;
    };

    enum class ReceiveStatus { Accepted, Rejected, Closing, Closed, UnknownConnection };

    struct ReceiveResult {
        ReceiveStatus status = ReceiveStatus::Rejected;
        std::optional<CodecError> error;

        [[nodiscard]] bool accepted() const noexcept {
            return status == ReceiveStatus::Accepted;
        }
    };

    enum class PublishDeliveryMode { None, Occurrences, SnapshotFallback };

    // Returned states are closed: rejection has an error and None; a commit has
    // no error and names its global delivery plan. Per-recipient projection may
    // still replace Occurrences with a Snapshot without changing this result.
    struct PublishResult {
        bool accepted = false;
        std::optional<ErrorCode> error;
        model::FrontendSequence sequence;
        std::size_t occurrenceCount = 0;
        PublishDeliveryMode deliveryMode = PublishDeliveryMode::None;
    };

    struct SnapshotPublishResult {
        bool accepted = false;
        std::optional<ErrorCode> error;
        model::FrontendSequence sequence;
        std::size_t recipientCount = 0;
    };

    class ServerCore {
    public:
        // ServerCore is confined to one owner event loop and is not thread-safe.
        // Injected callbacks may synchronously re-enter it; the implementation
        // explicitly defers delivery work and revalidates connection generations
        // across every such boundary.
        explicit ServerCore(BackendPort& backend, ServerCoreOptions options = {});
        ServerCore(const ServerCore&) = delete;
        ServerCore(ServerCore&&) = delete;
        ServerCore& operator=(const ServerCore&) = delete;
        ServerCore& operator=(ServerCore&&) = delete;
        ~ServerCore();

        void start();
        void close(std::string reason = "frontend server core closed") noexcept;

        [[nodiscard]] std::optional<ConnectionIdentity> openConnection(FrontendPeerContext peer, ConnectionCallbacks callbacks);
        [[nodiscard]] bool updatePeerContext(ConnectionIdentity connection, FrontendPeerContext peer) noexcept;
        void closeConnection(ConnectionIdentity connection, std::string reason = "frontend connection closed") noexcept;

        [[nodiscard]] ReceiveResult receive(ConnectionIdentity connection, const ClientMessage& message) noexcept;
        [[nodiscard]] ReceiveResult receive(ConnectionIdentity connection, const Json& message) noexcept;
        [[nodiscard]] ReceiveResult receive(ConnectionIdentity connection, std::string_view compactJson) noexcept;
        [[nodiscard]] ReceiveResult receiveDefinedCommand(ConnectionIdentity connection, const generated::DefinedCommand& command) noexcept;
        [[nodiscard]] ReceiveResult receiveError(ConnectionIdentity connection, CodecError error) noexcept;

        [[nodiscard]] AuthenticationFailureCode recordPreAuthenticationFailure(const FrontendPeerContext& peer,
                                                                               AuthenticationFailureCode failure) noexcept;

        [[nodiscard]] bool complete(BackendCompletion completion) noexcept;
        [[nodiscard]] OccurrenceStageResult stageGroup(OccurrenceCoalescingKey key,
                                                       model::OccurrenceDraft occurrence,
                                                       OccurrenceFlushUrgency urgency = OccurrenceFlushUrgency::Deferred) noexcept;
        [[nodiscard]] OccurrenceStageResult stageGroups(std::vector<OccurrenceStageRequest> groups) noexcept;
        [[nodiscard]] OccurrenceStageResult requireSnapshot(OccurrenceFlushUrgency urgency = OccurrenceFlushUrgency::Immediate) noexcept;
        [[nodiscard]] PublishResult publishGroup(model::OccurrenceDraft occurrence) noexcept;
        [[nodiscard]] SnapshotPublishResult publishSnapshot(model::CanonicalSnapshot snapshot) noexcept;
        void invalidateReplay() noexcept;

        // BackendCoreBridge reserves frontend-visible identities for sessions
        // owned by other in-process BackendCore consumers from the opposite
        // end of the numeric identity space. These source-private seams keep
        // external topology in ServerCore's one canonical authority without
        // exposing BackendCore SessionIds.
        [[nodiscard]] std::optional<model::SessionIdentity> reserveExternalSessionIdentity() noexcept;
        [[nodiscard]] bool replaceExternalTopology(std::vector<model::SessionState> sessions,
                                                   std::optional<model::SessionIdentity> controller,
                                                   bool bridgeControllerPresent) noexcept;
        [[nodiscard]] bool externalSessionChanged(model::SessionState session, bool connected) noexcept;
        [[nodiscard]] bool externalControllerChanged(std::optional<model::SessionIdentity> controller) noexcept;

        void flush();
        void flushConnection(ConnectionIdentity connection);

        void declareTransportFamily(FrontendTransportKind transport);
        void withdrawTransportFamily(FrontendTransportKind transport) noexcept;

        [[nodiscard]] bool isOpen() const noexcept;
        [[nodiscard]] bool flushScheduled() const noexcept;
        [[nodiscard]] model::FrontendSequence currentSequence() const noexcept;
        [[nodiscard]] std::size_t connectionCount() const noexcept;
        [[nodiscard]] std::size_t unauthenticatedConnectionCount() const noexcept;
        [[nodiscard]] std::size_t authenticatedConnectionCount() const noexcept;
        [[nodiscard]] std::optional<model::SessionIdentity> currentController() const;
        [[nodiscard]] std::optional<model::SessionIdentity> session(ConnectionIdentity connection) const;
        [[nodiscard]] bool connectionOpen(ConnectionIdentity connection) const noexcept;
        [[nodiscard]] std::optional<FrontendPrincipal> principal(ConnectionIdentity connection) const;
        [[nodiscard]] std::optional<FrontendPeerContext> peer(ConnectionIdentity connection) const;
        [[nodiscard]] bool helloComplete(ConnectionIdentity connection) const noexcept;
        [[nodiscard]] std::size_t queuedMessages(ConnectionIdentity connection) const noexcept;
        [[nodiscard]] std::size_t queuedBytes(ConnectionIdentity connection) const noexcept;
        [[nodiscard]] std::size_t outstandingCommands(ConnectionIdentity connection) const noexcept;
        [[nodiscard]] std::vector<FrontendMethod> definedMethods() const;
        [[nodiscard]] std::vector<FrontendMethod> implementedMethods() const;
        [[nodiscard]] std::vector<FrontendMethod> availableMethods() const;
        [[nodiscard]] std::vector<FrontendMethod> permittedMethods(const FrontendPrincipal& principal) const;
        [[nodiscard]] std::vector<FrontendCapability> implementedCapabilities() const;
        [[nodiscard]] std::vector<FrontendTransportKind> enabledTransportFamilies() const;

    private:
        class Impl;
        std::shared_ptr<Impl> impl;
    };

} // namespace ai::openai::codex::frontend::internal::server

#endif // AI_OPENAI_CODEX_FRONTEND_INTERNAL_SERVER_SERVERCORE_H
