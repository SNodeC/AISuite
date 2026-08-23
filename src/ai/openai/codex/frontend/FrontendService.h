/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_FRONTENDSERVICE_H
#define AI_OPENAI_CODEX_FRONTEND_FRONTENDSERVICE_H

#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/EventCoalescer.h"
#include "ai/openai/codex/frontend/EventJournal.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/Security.h"
#include "ai/openai/codex/frontend/UpdateBatch.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::openai::codex::frontend {

    struct OutboundMessage {
        ServerMessage message;
        std::string compactJson;
        std::size_t serializedBytes = 0;

        bool operator==(const OutboundMessage&) const = default;
    };

    struct FrontendConnectionCallbacks {
        // Legacy terminal delivery callback. Returning false means the
        // transport is closed; bounded adapters should use tryMessage so
        // temporary write-queue pressure remains lossless and retryable.
        using Send = std::function<bool(const OutboundMessage&)>;
        using TrySend = std::function<OutboundDeliveryStatus(const OutboundMessage&)>;
        using Closed = std::function<void(const std::string&)>;

        Send onMessage;
        Closed onClosed;
        TrySend tryMessage = {};
    };

    using FrontendTimerCancellation = std::function<void()>;
    using FrontendTimerScheduler = std::function<FrontendTimerCancellation(std::uint64_t delayMs, std::function<void()> callback)>;
    using FrontendAuthenticator = std::function<AuthenticationResult(const FrontendPeerContext&, const AuthenticationCredential&)>;
    using FrontendInvocationPolicy =
        std::function<bool(const FrontendPrincipal&, std::string_view method, const Json& validatedParameters)>;

    struct FrontendServiceOptions {
        EventJournalConfig journal;
        UpdateBatchConfig batches;
        EventCoalescerConfig coalescer;
        std::size_t maxOutboundMessagesPerConnection = DefaultFrontendServiceMaxOutboundMessages;
        std::size_t maxOutboundMessageBytes = DefaultFrontendMaximumServerMessageBytes;
        std::size_t maxOutboundBytesPerConnection = DefaultFrontendServiceMaxOutboundBytes;
        std::size_t maxMessagesPerDelivery = DefaultFrontendServiceMaxMessagesPerDelivery;
        std::size_t maxConnections = 128;
        std::size_t maxUnauthenticatedConnections = 16;
        std::uint64_t handshakeTimeoutMs = 10000;
        std::size_t maximumInboundMessageBytes = DefaultFrontendMaximumInboundMessageBytes;
        std::size_t maxInboundMessagesPerSecond = 50;
        std::size_t maxInboundBurst = 100;
        std::size_t maxOutstandingCommandsPerConnection = 256;
        std::size_t maximumFailedAuthenticationsPerPeer = 3;
        std::uint64_t failedAuthenticationWindowMs = 60000;
        bool allowVerifiedLocalTrust = true;
        bool allowInsecureLocalTrust = false;
        std::optional<std::uint64_t> trustedLocalUserId;
        bool enableFilesystemReadMethods = false;
        bool enableFilesystemWriteMethods = false;
        bool enableCommandExecutionMethods = false;
        FrontendInvocationPolicy filesystemReadPolicy;
        FrontendInvocationPolicy filesystemWritePolicy;
        FrontendInvocationPolicy commandExecutionPolicy;
        FrontendAuthenticator authenticator;
        std::function<void(std::function<void()>)> scheduler;
        FrontendTimerScheduler timerScheduler;
        std::function<std::uint64_t()> monotonicClockMs;
    };

    enum class ConnectionReceiveStatus { Accepted, Rejected, Closing, Closed };

    struct ConnectionReceiveResult {
        ConnectionReceiveStatus status = ConnectionReceiveStatus::Rejected;
        std::optional<CodecError> error;

        [[nodiscard]] bool accepted() const noexcept {
            return status == ConnectionReceiveStatus::Accepted;
        }
    };

    class FrontendService;

    class FrontendConnection {
    public:
        FrontendConnection() noexcept;
        FrontendConnection(const FrontendConnection&) = delete;
        FrontendConnection(FrontendConnection&& other) noexcept;

        FrontendConnection& operator=(const FrontendConnection&) = delete;
        FrontendConnection& operator=(FrontendConnection&& other) noexcept;

        ~FrontendConnection();

        [[nodiscard]] ConnectionReceiveResult receive(const ClientMessage& message) noexcept;
        [[nodiscard]] ConnectionReceiveResult receive(const Json& message) noexcept;
        [[nodiscard]] ConnectionReceiveResult receive(std::string_view compactJson) noexcept;
        [[nodiscard]] ConnectionReceiveResult receiveError(CodecError error) noexcept;

        // Transport adapters may learn additional verified peer metadata
        // (for example the HTTP Origin during a WebSocket upgrade) after
        // accepting the transport but before protocol authentication. Peer
        // facts are immutable once a Hello authentication attempt begins.
        [[nodiscard]] bool updatePeerContext(FrontendPeerContext peer) noexcept;

        void close(std::string reason = "frontend connection closed") noexcept;

        [[nodiscard]] bool isOpen() const noexcept;
        [[nodiscard]] bool helloComplete() const noexcept;
        [[nodiscard]] std::optional<std::string> sessionId() const;
        [[nodiscard]] std::optional<FrontendPrincipal> principal() const;
        [[nodiscard]] FrontendPeerContext peer() const;
        [[nodiscard]] std::size_t queuedMessages() const noexcept;
        [[nodiscard]] std::size_t queuedBytes() const noexcept;
        void recordTransportCloseReason(std::string reason) noexcept;

        // Retry the retained head message after a transport has made outbound
        // capacity available. Backpressured callbacks schedule this after they
        // return; they do not call it synchronously. Calling it on a closed
        // connection is harmless.
        void resumeDelivery() noexcept;

    private:
        friend class FrontendService;
        struct Control;

        explicit FrontendConnection(std::shared_ptr<Control> control) noexcept;

        std::shared_ptr<Control> control;
    };

    class FrontendService {
    public:
        template <typename ClientT>
        explicit FrontendService(backend::BackendCore<ClientT>& backend, FrontendServiceOptions options = {})
            : FrontendService(backend.implementation(), std::move(options)) {
        }

        FrontendService(const FrontendService&) = delete;
        FrontendService(FrontendService&&) = delete;

        FrontendService& operator=(const FrontendService&) = delete;
        FrontendService& operator=(FrontendService&&) = delete;

        ~FrontendService();

        [[nodiscard]] FrontendConnection openConnection(FrontendPeerContext peer, FrontendConnectionCallbacks callbacks);
        // Transport admission checks that necessarily precede protocol Hello
        // (for example Origin and transport-security policy) still consume
        // the service-owned per-peer authentication budget. No frontend or
        // BackendCore session is created by this operation.
        [[nodiscard]] AuthenticationFailureCode recordPreAuthenticationFailure(const FrontendPeerContext& peer,
                                                                               AuthenticationFailureCode failure) noexcept;
        void declareTransportFamily(FrontendTransportKind transport);
        void withdrawTransportFamily(FrontendTransportKind transport) noexcept;
        void flush();
        void close(std::string reason = "frontend service closed") noexcept;

        [[nodiscard]] bool isOpen() const noexcept;
        [[nodiscard]] bool flushScheduled() const noexcept;
        [[nodiscard]] SequenceNumber currentSequence() const noexcept;
        [[nodiscard]] std::size_t connectionCount() const noexcept;
        [[nodiscard]] std::size_t unauthenticatedConnectionCount() const noexcept;
        [[nodiscard]] std::size_t authenticatedConnectionCount() const noexcept;
        [[nodiscard]] std::optional<std::string> currentController() const;
        [[nodiscard]] std::vector<FrontendMethod> definedMethods() const;
        [[nodiscard]] std::vector<FrontendMethod> implementedMethods() const;
        [[nodiscard]] std::vector<FrontendMethod> availableMethods() const;
        [[nodiscard]] std::vector<FrontendMethod> permittedMethods(const FrontendPrincipal& principal) const;
        [[nodiscard]] std::vector<FrontendTransportKind> enabledTransportFamilies() const;
        [[nodiscard]] std::vector<FrontendCapability> implementedCapabilities() const;
        [[nodiscard]] EventJournalConfig journalConfig() const noexcept;
        [[nodiscard]] UpdateBatchConfig batchConfig() const noexcept;

    private:
        friend class FrontendConnection;

        explicit FrontendService(backend::detail::BackendCoreRuntime& backend, FrontendServiceOptions options);

        class Impl;
        std::shared_ptr<Impl> impl;
    };

} // namespace ai::openai::codex::frontend

#endif // AI_OPENAI_CODEX_FRONTEND_FRONTENDSERVICE_H
