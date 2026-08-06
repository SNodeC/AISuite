/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_CLIENT_CLIENTCONNECTION_H
#define APPS_CODEX_BACKEND_CLIENT_CLIENTCONNECTION_H

#include "ai/openai/codex/frontend/client/Client.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace apps::codex_backend_client {

    class CodexBackendClientSocketContext;
    class CodexBackendClientSocketContextFactory;

    // Application-private authority for physical connection attempts. The SDK
    // Connection generation starts only after a transport attaches; this gate
    // also covers DNS/connect/HTTP-upgrade work before that attachment exists.
    class PhysicalConnectionAttemptGate {
    public:
        using Generation = std::uint64_t;

        [[nodiscard]] std::optional<Generation> begin() noexcept;
        [[nodiscard]] bool isCurrent(Generation generation) const noexcept;
        [[nodiscard]] bool complete(Generation generation) noexcept;
        [[nodiscard]] bool active() const noexcept;
        [[nodiscard]] std::optional<Generation> current() const noexcept;

    private:
        Generation nextGeneration = 1;
        std::optional<Generation> activeGeneration;
    };

    struct ClientConnectionCallbacks {
        std::function<void()> onConnected;
        std::function<void()> onDisconnected;
        std::function<void(std::string)> onFailure;
        std::function<void(PhysicalConnectionAttemptGate::Generation)> onAttemptConnected = {};
        std::function<void(PhysicalConnectionAttemptGate::Generation)> onAttemptDisconnected = {};
        std::function<void(PhysicalConnectionAttemptGate::Generation, std::string)> onAttemptFailure = {};
        std::function<void(const ai::openai::codex::frontend::client::OutboundMessage&)> onOutbound;
        bool verifiedLocalUnix = false;
        std::function<void(bool)> onBeforeTransportConnected;
        std::function<void()> onLocalShutdown = {};
    };

    class ClientConnection {
    public:
        ClientConnection(ai::openai::codex::frontend::client::Client& sdk, ClientConnectionCallbacks callbacks = {});
        ClientConnection(const ClientConnection&) = delete;
        ClientConnection(ClientConnection&&) = delete;
        ClientConnection& operator=(const ClientConnection&) = delete;
        ClientConnection& operator=(ClientConnection&&) = delete;
        ~ClientConnection();

        // Ends the physical attachment because the application itself is
        // terminating. Transport loss and SDK-requested connection closure
        // use the private detach/closeTransport paths and must not call this.
        void shutdown() noexcept;
        [[nodiscard]] bool connected() const noexcept;
        [[nodiscard]] bool prepareAttempt(PhysicalConnectionAttemptGate::Generation generation) noexcept;
        void cancelPreparedAttempt(PhysicalConnectionAttemptGate::Generation generation) noexcept;
        [[nodiscard]] bool hasAttachment(PhysicalConnectionAttemptGate::Generation generation) const noexcept;

    private:
        friend class CodexBackendClientSocketContext;
        friend class CodexBackendClientSocketContextFactory;
        friend struct ClientConnectionAttemptTestAccess;
        friend struct ClientConnectionTestAccess;

        [[nodiscard]] PhysicalConnectionAttemptGate::Generation preparedAttemptForFactory() const noexcept;
        [[nodiscard]] bool acceptsAttemptGeneration(PhysicalConnectionAttemptGate::Generation attemptGeneration) const noexcept;
        void attach(CodexBackendClientSocketContext& context, PhysicalConnectionAttemptGate::Generation attemptGeneration) noexcept;
        void didConnect(CodexBackendClientSocketContext& context) noexcept;
        void didReceive(CodexBackendClientSocketContext& context, std::string frame) noexcept;
        void didFail(CodexBackendClientSocketContext& context, std::string message) noexcept;
        void detach(CodexBackendClientSocketContext& context) noexcept;
        [[nodiscard]] ai::openai::codex::frontend::client::SendResult
        send(ai::openai::codex::frontend::client::OutboundMessage message) noexcept;
        void closeTransport(std::string reason) noexcept;
        void reportFailure(std::string message) noexcept;

        ai::openai::codex::frontend::client::Client& sdk;
        ClientConnectionCallbacks callbacks;
        ai::openai::codex::frontend::client::Connection protocolConnection;
        CodexBackendClientSocketContext* context = nullptr;
        PhysicalConnectionAttemptGate::Generation preparedGeneration = 0;
        PhysicalConnectionAttemptGate::Generation attachmentGeneration = 0;
        bool online = false;
        bool applicationShutdown = false;
        bool failureReported = false;
    };

} // namespace apps::codex_backend_client

#endif // APPS_CODEX_BACKEND_CLIENT_CLIENTCONNECTION_H
