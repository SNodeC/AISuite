/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/FrontendWebSocketClient.h"

#include "apps/codex-backend-client/Configuration.h"

#include "core/socket/stream/SocketConnection.h"
#include "web/http/client/SocketContext.h"
#include "web/websocket/SubProtocolContext.h"
#include "web/websocket/SubProtocolFactory.h"
#include "web/websocket/client/SocketContextUpgradeFactory.h"
#include "web/websocket/client/SubProtocol.h"
#include "web/websocket/client/SubProtocolFactorySelector.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace apps::codex_backend_client {
    namespace sdk = ai::openai::codex::frontend::client;

    namespace {

        constexpr std::uint16_t CloseUnsupportedData = 1003;
        constexpr std::uint16_t ClosePolicyViolation = 1008;
        constexpr std::uint16_t CloseUnexpectedCondition = 1011;

        class FrontendWebSocketHttpSocketContext final : public web::http::client::SocketContext {
        public:
            FrontendWebSocketHttpSocketContext(
                core::socket::stream::SocketConnection* connection,
                const std::function<void(const std::shared_ptr<web::http::client::MasterRequest>&)>& onHttpConnected,
                const std::function<void(const std::shared_ptr<web::http::client::MasterRequest>&)>& onHttpDisconnected,
                const std::string& hostHeader,
                bool pipelinedRequests,
                const web::http::ParserLimits& parserLimits,
                std::shared_ptr<FrontendWebSocketClientBinding> binding)
                : web::http::client::SocketContext(
                      connection, onHttpConnected, onHttpDisconnected, hostHeader, pipelinedRequests, parserLimits)
                , frontendBinding(std::move(binding)) {
            }

            [[nodiscard]] std::shared_ptr<FrontendWebSocketClientBinding> binding() const noexcept {
                return frontendBinding;
            }

        private:
            std::shared_ptr<FrontendWebSocketClientBinding> frontendBinding;
        };

    } // namespace

    class FrontendWebSocketClientSubProtocol final : public web::websocket::client::SubProtocol {
    public:
        FrontendWebSocketClientSubProtocol(web::websocket::SubProtocolContext* context,
                                           std::shared_ptr<FrontendWebSocketClientBinding> binding)
            : web::websocket::client::SubProtocol(context, "codex", 0, 3)
            , binding(std::move(binding)) {
        }

        ~FrontendWebSocketClientSubProtocol() override {
            detach("frontend WebSocket subprotocol destroyed");
        }

        void close(bool intentional = false) noexcept {
            if (intentional && !intentionalShutdown) {
                intentionalShutdown = true;
                try {
                    if (binding->callbacks.onLocalShutdown) {
                        binding->callbacks.onLocalShutdown();
                    }
                } catch (...) {
                }
            }
            closeBounded(ClosePolicyViolation, "frontend client connection closed");
        }

    private:
        void onConnected() override {
            if (binding->active != nullptr && binding->active != this) {
                reportFailureOnce("exactly one outgoing frontend transport may be active");
                closeBounded(ClosePolicyViolation, "frontend SDK already has an active transport");
                return;
            }
            protocolConnection = binding->client.openConnection({[this](sdk::OutboundMessage message) {
                                                                     return send(std::move(message));
                                                                 },
                                                                 [this](std::string reason) {
                                                                     reportFailureOnce(std::move(reason));
                                                                     close();
                                                                 }});
            if (!protocolConnection.isOpen()) {
                reportFailureOnce("frontend SDK rejected the WebSocket transport attachment");
                closeBounded(ClosePolicyViolation, "frontend SDK rejected transport");
                return;
            }
            binding->active = this;
            try {
                if (binding->callbacks.onBeforeTransportConnected) {
                    binding->callbacks.onBeforeTransportConnected(false);
                }
            } catch (...) {
                reportFailureOnce("frontend transport authentication preparation failed");
                closeBounded(ClosePolicyViolation, "frontend transport authentication preparation failed");
                return;
            }
            protocolConnection.transportConnected();
            if (!protocolConnection.isTransportConnected()) {
                return;
            }
            try {
                if (binding->callbacks.onConnected) {
                    binding->callbacks.onConnected();
                }
            } catch (...) {
            }
        }

        void onMessageStart(int opCode) override {
            inbound.clear();
            receivedOpCode = opCode;
            if (opCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
                failBounded(CloseUnsupportedData, "binary frontend messages are not supported");
            }
        }

        void onMessageData(const char* chunk, std::size_t chunkLength) override {
            if (closeStarted || receivedOpCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
                return;
            }
            if (chunkLength > DEFAULT_MAXIMUM_SERVER_MESSAGE_BYTES
                || inbound.size() > DEFAULT_MAXIMUM_SERVER_MESSAGE_BYTES - chunkLength) {
                failBounded(ClosePolicyViolation, "frontend message exceeds the configured limit");
                return;
            }
            try {
                inbound.append(chunk, chunkLength);
            } catch (...) {
                failBounded(CloseUnexpectedCondition, "frontend message buffering failed");
            }
        }

        void onMessageEnd() override {
            if (closeStarted || receivedOpCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
                inbound.clear();
                return;
            }
            const sdk::ReceiveResult result = protocolConnection.receive(std::string_view(inbound));
            inbound.clear();
            if (!result.accepted) {
                reportFailureOnce(result.error ? result.error->message : "frontend WebSocket message was rejected");
                closeBounded(ClosePolicyViolation, "invalid frontend protocol message");
            }
        }

        void onMessageError([[maybe_unused]] std::uint16_t error) override {
            failBounded(ClosePolicyViolation, "invalid frontend WebSocket message");
        }

        void onDisconnected() override {
            detach("frontend WebSocket transport disconnected");
        }

        bool onSignal([[maybe_unused]] int signal) override {
            close(true);
            return false;
        }

        sdk::SendResult send(sdk::OutboundMessage message) noexcept {
            if (closeStarted || binding->active != this) {
                return {sdk::SendStatus::Closed, sdk::TransportError{"frontend WebSocket transport is closed", true}};
            }
            try {
                sendMessage(std::move(message.compactJson));
                return {sdk::SendStatus::Accepted, std::nullopt};
            } catch (...) {
                return {sdk::SendStatus::Failed, sdk::TransportError{"frontend WebSocket send failed", true}};
            }
        }

        void closeBounded(std::uint16_t status, const char* reason) noexcept {
            if (closeStarted) {
                return;
            }
            closeStarted = true;
            inbound.clear();
            try {
                sendClose(status, reason, std::char_traits<char>::length(reason));
            } catch (...) {
            }
        }

        void failBounded(std::uint16_t status, const char* reason) noexcept {
            reportFailureOnce(reason);
            closeBounded(status, reason);
        }

        void reportFailureOnce(std::string message) noexcept {
            if (failureReported) {
                return;
            }
            failureReported = true;
            binding->reportFailure(std::move(message));
        }

        void detach(std::string reason) noexcept {
            if (detached) {
                return;
            }
            detached = true;
            inbound.clear();
            if (intentionalShutdown) {
                protocolConnection.transportDisconnected();
            } else {
                protocolConnection.transportDisconnected(sdk::TransportError{std::move(reason), true});
            }
            if (binding->active == this) {
                binding->active = nullptr;
            }
            if (!disconnectNotified && binding->callbacks.onDisconnected) {
                disconnectNotified = true;
                try {
                    binding->callbacks.onDisconnected();
                } catch (...) {
                }
            }
        }

        std::shared_ptr<FrontendWebSocketClientBinding> binding;
        sdk::Connection protocolConnection;
        std::string inbound;
        int receivedOpCode = 0;
        bool disconnectNotified = false;
        bool closeStarted = false;
        bool detached = false;
        bool intentionalShutdown = false;
        bool failureReported = false;
    };

    namespace {

        class FrontendWebSocketClientFactory final : public web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol> {
        public:
            FrontendWebSocketClientFactory()
                : web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>("codex") {
            }

        private:
            web::websocket::client::SubProtocol* create(web::websocket::SubProtocolContext* context) override {
                core::socket::stream::SocketConnection* const connection = context != nullptr ? context->getSocketConnection() : nullptr;
                auto* const httpContext =
                    connection != nullptr ? dynamic_cast<FrontendWebSocketHttpSocketContext*>(connection->getSocketContext()) : nullptr;
                if (httpContext == nullptr || httpContext->getSocketConnection() != connection) {
                    return nullptr;
                }
                std::shared_ptr<FrontendWebSocketClientBinding> binding = httpContext->binding();
                return binding ? new FrontendWebSocketClientSubProtocol(context, std::move(binding)) : nullptr;
            }
        };

        web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>* createFrontendWebSocketClientFactory() {
            static FrontendWebSocketClientFactory factory;
            return &factory;
        }

    } // namespace

    FrontendWebSocketClientBinding::FrontendWebSocketClientBinding(sdk::Client& client, FrontendWebSocketClientCallbacks callbacks)
        : client(client)
        , callbacks(std::move(callbacks)) {
    }

    void FrontendWebSocketClientBinding::shutdown() noexcept {
        if (active != nullptr) {
            active->close(true);
        }
    }

    bool FrontendWebSocketClientBinding::connected() const noexcept {
        return active != nullptr;
    }

    void FrontendWebSocketClientBinding::reportFailure(std::string message) noexcept {
        try {
            if (callbacks.onFailure) {
                callbacks.onFailure(std::move(message));
            }
        } catch (...) {
        }
    }

    void FrontendWebSocketClientBinding::beginUpgrade() noexcept {
        upgradeCommitted = false;
    }

    void FrontendWebSocketClientBinding::commitUpgrade() noexcept {
        upgradeCommitted = true;
    }

    bool FrontendWebSocketClientBinding::consumeCommittedUpgrade() noexcept {
        return std::exchange(upgradeCommitted, false);
    }

    FrontendWebSocketHttpSocketContextFactory::FrontendWebSocketHttpSocketContextFactory(
        const std::function<void(const std::shared_ptr<MasterRequest>&)>& onHttpConnected,
        const std::function<void(const std::shared_ptr<MasterRequest>&)>& onHttpDisconnected,
        const std::function<net::config::ConfigInstance&()>& getConfigInstance,
        std::shared_ptr<FrontendWebSocketClientBinding> binding)
        : onHttpConnected(onHttpConnected)
        , onHttpDisconnected(onHttpDisconnected)
        , configInstance(getConfigInstance())
        , binding(std::move(binding)) {
    }

    core::socket::stream::SocketContext*
    FrontendWebSocketHttpSocketContextFactory::create(core::socket::stream::SocketConnection* socketConnection) {
        const auto* const config = configInstance.getSubCommand<web::http::client::ConfigHTTP>();
        return new FrontendWebSocketHttpSocketContext(socketConnection,
                                                      onHttpConnected,
                                                      onHttpDisconnected,
                                                      config->getHostHeader(),
                                                      config->getPipelinedRequests(),
                                                      config->getParserLimits(),
                                                      binding);
    }

    void linkFrontendWebSocketClient() noexcept {
        web::websocket::client::SocketContextUpgradeFactory::link();
        web::websocket::client::SubProtocolFactorySelector::link("codex", createFrontendWebSocketClientFactory);
    }

} // namespace apps::codex_backend_client
