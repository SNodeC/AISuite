/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/provider/WebSocketAppServer.h"

#include "ai/openai/codex/bridge/CodexBridge.h"
#include "core/socket/stream/QueueResult.h"
#include "core/socket/stream/SocketConnection.h"
#include "net/config/ConfigConnection.h"
#include "web/http/client/Response.h"
#include "web/http/client/SocketContext.h"
#include "web/websocket/SocketContextUpgrade.h"
#include "web/websocket/SubProtocolContext.h"
#include "web/websocket/client/SocketContextUpgradeFactory.h"
#include "web/websocket/client/SubProtocol.h"
#include "utils/base64.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ai::openai::codex::provider {

    namespace {

        constexpr std::uint16_t CloseUnsupportedData = 1003;
        constexpr std::uint16_t ClosePolicyViolation = 1008;
        constexpr std::uint16_t CloseUnexpectedCondition = 1011;
        constexpr std::size_t WebSocketFramePayloadBytes = 16U * 1024U;
        constexpr std::size_t MaximumClientFrameHeaderBytes = 8;

        class AppServerSubProtocol;

    } // namespace

    class WebSocketAppServerState {
    public:
        WebSocketAppServerState(bridge::CodexBridge& bridge, std::size_t maximumFrameBytes)
            : bridge_(&bridge)
            , maximumFrameBytes_(maximumFrameBytes) {
            if (maximumFrameBytes_ == 0) {
                throw std::invalid_argument("app-server WebSocket maximum frame size must be greater than zero");
            }
        }

        bool attach(AppServerSubProtocol& protocol) noexcept;
        void detach(AppServerSubProtocol& protocol, std::string_view reason) noexcept;
        bool send(const nlohmann::json& message);
        bool connected() const noexcept;
        void stop() noexcept;
        void releaseBridge() noexcept;
        bridge::CodexBridge* bridge() const noexcept;
        std::size_t maximumFrameBytes() const noexcept;

        bool upgradeCommitted = false;

    private:
        bridge::CodexBridge* bridge_;
        AppServerSubProtocol* active_ = nullptr;
        std::size_t maximumFrameBytes_;
    };

    namespace {

        std::optional<std::size_t> framedMessageBytes(std::size_t payloadBytes) noexcept {
            const std::size_t frameCount = payloadBytes == 0 ? 1 : (payloadBytes - 1) / WebSocketFramePayloadBytes + 1;
            if (frameCount > (std::numeric_limits<std::size_t>::max() - payloadBytes) / MaximumClientFrameHeaderBytes) {
                return std::nullopt;
            }
            return payloadBytes + frameCount * MaximumClientFrameHeaderBytes;
        }

        void sendBoundedClose(web::websocket::client::SubProtocol& protocol,
                              std::uint16_t status,
                              std::string_view reason) {
            const std::string_view bounded = reason.substr(0, 123);
            protocol.sendClose(status, bounded.data(), bounded.size());
        }

        class AppServerSubProtocol final : public web::websocket::client::SubProtocol {
        public:
            AppServerSubProtocol(web::websocket::SubProtocolContext* context,
                                 std::shared_ptr<WebSocketAppServerState> state)
                : web::websocket::client::SubProtocol(context, "codex-app-server", 0, 3)
                , state_(std::move(state)) {
            }

            ~AppServerSubProtocol() override {
                detach("app-server WebSocket subprotocol destroyed");
            }

            bool sendJson(const nlohmann::json& message) {
                if (closing_) {
                    return false;
                }
                try {
                    const std::string serialized = message.dump();
                    if (serialized.size() > state_->maximumFrameBytes()) {
                        return false;
                    }
                    core::socket::stream::SocketConnection* const connection = getSocketConnection();
                    const std::optional<std::size_t> frameBytes = framedMessageBytes(serialized.size());
                    if (connection == nullptr || !frameBytes) {
                        return false;
                    }
                    const auto* config = dynamic_cast<const net::config::ConfigConnection*>(connection->getConfigInstance());
                    const std::size_t writerLimit = config == nullptr ? 0 : config->getMaximumWriteQueueBytes();
                    const std::size_t totalQueued = connection->getTotalQueued();
                    const std::size_t totalSent = connection->getTotalSent();
                    if (totalQueued < totalSent) {
                        return false;
                    }
                    const std::size_t outstanding = totalQueued - totalSent;
                    if (writerLimit != 0 && (*frameBytes > writerLimit || outstanding > writerLimit - *frameBytes)) {
                        return false;
                    }
                    if (connection->trySendToPeer("", 0) != core::socket::stream::QueueResult::Queued) {
                        return false;
                    }
                    sendMessage(serialized);
                    return true;
                } catch (...) {
                    return false;
                }
            }

            void close() noexcept {
                if (closing_) {
                    return;
                }
                closing_ = true;
                inbound_.clear();
                try {
                    sendBoundedClose(*this, ClosePolicyViolation, "app-server provider closing");
                } catch (...) {
                }
            }

        private:
            void onConnected() override {
                if (!state_->attach(*this)) {
                    close();
                }
            }

            void onMessageStart(int opCode) override {
                inbound_.clear();
                receivedOpCode_ = opCode;
                if (opCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
                    closing_ = true;
                    sendBoundedClose(*this, CloseUnsupportedData, "app-server messages must be text");
                }
            }

            void onMessageData(const char* chunk, std::size_t chunkLength) override {
                if (closing_ || receivedOpCode_ != web::websocket::SubProtocolContext::OpCode::TEXT) {
                    return;
                }
                if (chunkLength > state_->maximumFrameBytes() ||
                    inbound_.size() > state_->maximumFrameBytes() - chunkLength) {
                    closing_ = true;
                    sendBoundedClose(*this, ClosePolicyViolation, "app-server message exceeds limit");
                    return;
                }
                inbound_.append(chunk, chunkLength);
            }

            void onMessageEnd() override {
                if (closing_ || receivedOpCode_ != web::websocket::SubProtocolContext::OpCode::TEXT) {
                    inbound_.clear();
                    return;
                }
                nlohmann::json message;
                try {
                    message = nlohmann::json::parse(inbound_);
                } catch (const nlohmann::json::exception&) {
                    inbound_.clear();
                    closing_ = true;
                    sendBoundedClose(*this, ClosePolicyViolation, "invalid app-server JSON message");
                    return;
                }
                inbound_.clear();
                try {
                    if (bridge::CodexBridge* const bridge = state_->bridge()) {
                        bridge->receiveFromAppServer(message);
                    }
                } catch (...) {
                    closing_ = true;
                    sendBoundedClose(*this, CloseUnexpectedCondition, "app-server message handling failed");
                }
            }

            void onMessageError(std::uint16_t error) override {
                static_cast<void>(error);
                close();
            }

            void onDisconnected() override {
                detach("app-server WebSocket disconnected");
            }

            bool onSignal(int signal) override {
                static_cast<void>(signal);
                close();
                return false;
            }

            void detach(std::string_view reason) noexcept {
                if (detached_) {
                    return;
                }
                detached_ = true;
                inbound_.clear();
                state_->detach(*this, reason);
            }

            std::shared_ptr<WebSocketAppServerState> state_;
            std::string inbound_;
            int receivedOpCode_ = 0;
            bool closing_ = false;
            bool detached_ = false;
        };

        class AppServerHttpSocketContext final : public web::http::client::SocketContext {
        public:
            AppServerHttpSocketContext(
                core::socket::stream::SocketConnection* connection,
                const std::function<void(const std::shared_ptr<web::http::client::MasterRequest>&)>& onHttpConnected,
                const std::function<void(const std::shared_ptr<web::http::client::MasterRequest>&)>& onHttpDisconnected,
                const std::string& hostHeader,
                bool pipelinedRequests,
                const web::http::ParserLimits& parserLimits,
                std::shared_ptr<WebSocketAppServerState> state)
                : web::http::client::SocketContext(
                      connection, onHttpConnected, onHttpDisconnected, hostHeader, pipelinedRequests, parserLimits)
                , state_(std::move(state)) {
            }

            std::shared_ptr<WebSocketAppServerState> state() const noexcept {
                return state_;
            }

        private:
            std::shared_ptr<WebSocketAppServerState> state_;
        };

        class AppServerSocketContextUpgrade final
            : public web::websocket::SocketContextUpgrade<web::websocket::client::SubProtocol,
                                                          web::http::client::Request,
                                                          web::http::client::Response> {
        private:
            using Super = web::websocket::SocketContextUpgrade<web::websocket::client::SubProtocol,
                                                               web::http::client::Request,
                                                               web::http::client::Response>;

        public:
            AppServerSocketContextUpgrade(core::socket::stream::SocketConnection* connection,
                                          web::http::client::SocketContextUpgradeFactory* factory,
                                          std::shared_ptr<WebSocketAppServerState> state)
                : Super(connection, factory, Role::CLIENT) {
                subProtocol = new AppServerSubProtocol(this, std::move(state));
            }

            ~AppServerSocketContextUpgrade() override {
                delete subProtocol;
                subProtocol = nullptr;
            }
        };

        class AppServerSocketContextUpgradeFactory final : public web::websocket::client::SocketContextUpgradeFactory {
        private:
            web::http::SocketContextUpgrade<web::http::client::Request, web::http::client::Response>*
            create(core::socket::stream::SocketConnection* connection,
                   web::http::client::Request* request,
                   web::http::client::Response* response) override {
                if (connection == nullptr || request == nullptr || response == nullptr ||
                    response->get("sec-websocket-accept") != base64::serverWebSocketKey(request->header("Sec-WebSocket-Key"))) {
                    checkRefCount();
                    return nullptr;
                }
                auto* httpContext = dynamic_cast<AppServerHttpSocketContext*>(connection->getSocketContext());
                if (httpContext == nullptr) {
                    checkRefCount();
                    return nullptr;
                }
                return new AppServerSocketContextUpgrade(connection, this, httpContext->state());
            }
        };

        web::http::client::SocketContextUpgradeFactory* createAppServerUpgradeFactory() {
            return new AppServerSocketContextUpgradeFactory();
        }

    } // namespace

    bool WebSocketAppServerState::attach(AppServerSubProtocol& protocol) noexcept {
        if (active_ != nullptr || bridge_ == nullptr) {
            return false;
        }
        active_ = &protocol;
        bridge_->appServerConnected();
        return true;
    }

    void WebSocketAppServerState::detach(AppServerSubProtocol& protocol, std::string_view reason) noexcept {
        if (active_ != &protocol) {
            return;
        }
        active_ = nullptr;
        if (bridge_ != nullptr) {
            bridge_->appServerDisconnected(reason);
        }
    }

    bool WebSocketAppServerState::send(const nlohmann::json& message) {
        return active_ != nullptr && active_->sendJson(message);
    }

    bool WebSocketAppServerState::connected() const noexcept {
        return active_ != nullptr;
    }

    void WebSocketAppServerState::stop() noexcept {
        if (active_ != nullptr) {
            active_->close();
        }
    }

    void WebSocketAppServerState::releaseBridge() noexcept {
        bridge_ = nullptr;
    }

    bridge::CodexBridge* WebSocketAppServerState::bridge() const noexcept {
        return bridge_;
    }

    std::size_t WebSocketAppServerState::maximumFrameBytes() const noexcept {
        return maximumFrameBytes_;
    }

    WebSocketAppServer::WebSocketAppServer(bridge::CodexBridge& bridge, std::size_t maximumFrameBytes)
        : state_(std::make_shared<WebSocketAppServerState>(bridge, maximumFrameBytes)) {
        bridge.setAppServer(this);
    }

    WebSocketAppServer::~WebSocketAppServer() {
        stop();
        if (bridge::CodexBridge* const bridge = state_->bridge()) {
            bridge->setAppServer(nullptr);
        }
        state_->releaseBridge();
    }

    bool WebSocketAppServer::send(const nlohmann::json& message) {
        return state_->send(message);
    }

    bool WebSocketAppServer::isConnected() const noexcept {
        return state_->connected();
    }

    void WebSocketAppServer::stop() noexcept {
        state_->stop();
    }

    void WebSocketAppServer::beginUpgrade(const std::shared_ptr<web::http::client::MasterRequest>& request,
                                          std::string endpoint) {
        state_->upgradeCommitted = false;
        request->upgrade(
            endpoint,
            "websocket",
            [](bool initiated) {
                if (!initiated) {
                    std::clog << "codex-bridge: app-server WebSocket upgrade could not be initiated\n";
                }
            },
            [state = state_](const auto&, const auto& response, bool success) {
                state->upgradeCommitted = success && response->get("upgrade") == "websocket";
                if (!state->upgradeCommitted) {
                    std::clog << "codex-bridge: app-server WebSocket upgrade rejected\n";
                }
            },
            [](const auto&, const std::string& message) {
                std::clog << "codex-bridge: app-server WebSocket HTTP error=" << message << '\n';
            });
    }

    void WebSocketAppServer::httpDisconnected(const std::shared_ptr<web::http::client::MasterRequest>& request) noexcept {
        static_cast<void>(request);
        if (!std::exchange(state_->upgradeCommitted, false) && !state_->connected()) {
            std::clog << "codex-bridge: app-server HTTP transport disconnected before WebSocket activation\n";
        }
    }

    std::shared_ptr<WebSocketAppServerState> WebSocketAppServer::state() const noexcept {
        return state_;
    }

    WebSocketHttpSocketContextFactory::WebSocketHttpSocketContextFactory(
        const std::function<void(const std::shared_ptr<MasterRequest>&)>& onHttpConnected,
        const std::function<void(const std::shared_ptr<MasterRequest>&)>& onHttpDisconnected,
        const std::function<net::config::ConfigInstance&()>& getConfigInstance,
        std::shared_ptr<WebSocketAppServerState> state)
        : onHttpConnected_(onHttpConnected)
        , onHttpDisconnected_(onHttpDisconnected)
        , configInstance_(getConfigInstance())
        , state_(std::move(state)) {
    }

    core::socket::stream::SocketContext*
    WebSocketHttpSocketContextFactory::create(core::socket::stream::SocketConnection* connection) {
        const auto* config = configInstance_.getSubCommand<web::http::client::ConfigHTTP>();
        return new AppServerHttpSocketContext(connection,
                                              onHttpConnected_,
                                              onHttpDisconnected_,
                                              config->getHostHeader(),
                                              config->getPipelinedRequests(),
                                              config->getParserLimits(),
                                              state_);
    }

    void linkAppServerWebSocketClient() {
        static const bool linked = [] {
            web::http::client::SocketContextUpgradeFactory::link("websocket", createAppServerUpgradeFactory);
            return true;
        }();
        static_cast<void>(linked);
    }

} // namespace ai::openai::codex::provider
