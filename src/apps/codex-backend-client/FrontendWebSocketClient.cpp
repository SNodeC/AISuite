/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/FrontendWebSocketClient.h"

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

        constexpr std::size_t MaximumInboundMessageBytes = 16U * 1024U * 1024U;
        constexpr std::uint16_t CloseUnsupportedData = 1003;
        constexpr std::uint16_t ClosePolicyViolation = 1008;
        constexpr std::uint16_t CloseUnexpectedCondition = 1011;
        FrontendWebSocketClientRuntime* installedRuntime = nullptr;

    } // namespace

    class FrontendWebSocketClientSubProtocol final : public web::websocket::client::SubProtocol {
    public:
        FrontendWebSocketClientSubProtocol(web::websocket::SubProtocolContext* context, FrontendWebSocketClientRuntime& runtime)
            : web::websocket::client::SubProtocol(context, "codex", 0, 3)
            , runtime(runtime) {
        }

        ~FrontendWebSocketClientSubProtocol() override {
            detach("frontend WebSocket subprotocol destroyed");
        }

        void close() noexcept {
            closeBounded(ClosePolicyViolation, "frontend client connection closed");
        }

    private:
        void onConnected() override {
            if (runtime.active != nullptr && runtime.active != this) {
                runtime.reportFailure("exactly one outgoing frontend transport may be active");
                closeBounded(ClosePolicyViolation, "frontend SDK already has an active transport");
                return;
            }
            protocolConnection = runtime.client.openConnection({[this](sdk::OutboundMessage message) {
                                                                    return send(std::move(message));
                                                                },
                                                                [this]([[maybe_unused]] std::string reason) {
                                                                    close();
                                                                }});
            if (!protocolConnection.isOpen()) {
                runtime.reportFailure("frontend SDK rejected the WebSocket transport attachment");
                closeBounded(ClosePolicyViolation, "frontend SDK rejected transport");
                return;
            }
            runtime.active = this;
            try {
                if (runtime.callbacks.onBeforeTransportConnected) {
                    runtime.callbacks.onBeforeTransportConnected(false);
                }
            } catch (...) {
                runtime.reportFailure("frontend transport authentication preparation failed");
                closeBounded(ClosePolicyViolation, "frontend transport authentication preparation failed");
                return;
            }
            protocolConnection.transportConnected();
            if (!protocolConnection.isTransportConnected()) {
                return;
            }
            notifyConnected = true;
            try {
                if (runtime.callbacks.onConnected) {
                    runtime.callbacks.onConnected();
                }
            } catch (...) {
            }
        }

        void onMessageStart(int opCode) override {
            inbound.clear();
            receivedOpCode = opCode;
            if (opCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
                closeBounded(CloseUnsupportedData, "binary frontend messages are not supported");
            }
        }

        void onMessageData(const char* chunk, std::size_t chunkLength) override {
            if (closeStarted || receivedOpCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
                return;
            }
            if (chunkLength > MaximumInboundMessageBytes || inbound.size() > MaximumInboundMessageBytes - chunkLength) {
                closeBounded(ClosePolicyViolation, "frontend message exceeds the configured limit");
                return;
            }
            try {
                inbound.append(chunk, chunkLength);
            } catch (...) {
                closeBounded(CloseUnexpectedCondition, "frontend message buffering failed");
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
                runtime.reportFailure(result.error ? result.error->message : "frontend WebSocket message was rejected");
                closeBounded(ClosePolicyViolation, "invalid frontend protocol message");
            }
        }

        void onMessageError([[maybe_unused]] std::uint16_t error) override {
            closeBounded(ClosePolicyViolation, "invalid frontend WebSocket message");
        }

        void onDisconnected() override {
            detach("frontend WebSocket transport disconnected");
        }

        bool onSignal([[maybe_unused]] int signal) override {
            close();
            return false;
        }

        sdk::SendResult send(sdk::OutboundMessage message) noexcept {
            if (closeStarted || runtime.active != this) {
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

        void detach(std::string reason) noexcept {
            if (detached) {
                return;
            }
            detached = true;
            inbound.clear();
            protocolConnection.transportDisconnected(sdk::TransportError{std::move(reason), true});
            if (runtime.active == this) {
                runtime.active = nullptr;
            }
            if (notifyConnected && runtime.callbacks.onDisconnected) {
                notifyConnected = false;
                try {
                    runtime.callbacks.onDisconnected();
                } catch (...) {
                }
            }
        }

        FrontendWebSocketClientRuntime& runtime;
        sdk::Connection protocolConnection;
        std::string inbound;
        int receivedOpCode = 0;
        bool notifyConnected = false;
        bool closeStarted = false;
        bool detached = false;
    };

    namespace {

        class FrontendWebSocketClientFactory final : public web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol> {
        public:
            FrontendWebSocketClientFactory()
                : web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>("codex") {
            }

        private:
            web::websocket::client::SubProtocol* create(web::websocket::SubProtocolContext* context) override {
                if (installedRuntime == nullptr) {
                    return nullptr;
                }
                return new FrontendWebSocketClientSubProtocol(context, *installedRuntime);
            }
        };

        web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>* createFrontendWebSocketClientFactory() {
            return new FrontendWebSocketClientFactory();
        }

    } // namespace

    FrontendWebSocketClientRuntime::FrontendWebSocketClientRuntime(sdk::Client& client, FrontendWebSocketClientCallbacks callbacks)
        : client(client)
        , callbacks(std::move(callbacks)) {
    }

    FrontendWebSocketClientRuntime::~FrontendWebSocketClientRuntime() {
        uninstall();
    }

    bool FrontendWebSocketClientRuntime::install() noexcept {
        if (installedRuntime != nullptr && installedRuntime != this) {
            return false;
        }
        installedRuntime = this;
        installed = true;
        return true;
    }

    void FrontendWebSocketClientRuntime::uninstall() noexcept {
        disconnect();
        if (installedRuntime == this) {
            installedRuntime = nullptr;
        }
        installed = false;
    }

    void FrontendWebSocketClientRuntime::disconnect() noexcept {
        if (active != nullptr) {
            active->close();
        }
    }

    bool FrontendWebSocketClientRuntime::connected() const noexcept {
        return active != nullptr;
    }

    void FrontendWebSocketClientRuntime::reportFailure(std::string message) noexcept {
        try {
            if (callbacks.onFailure) {
                callbacks.onFailure(std::move(message));
            }
        } catch (...) {
        }
    }

    void linkFrontendWebSocketClient() noexcept {
        web::websocket::client::SocketContextUpgradeFactory::link();
        web::websocket::client::SubProtocolFactorySelector::link("codex", createFrontendWebSocketClientFactory);
    }

} // namespace apps::codex_backend_client
