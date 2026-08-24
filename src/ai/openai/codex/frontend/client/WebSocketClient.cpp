/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/WebSocketClient.h"

#include "ai/openai/codex/frontend/client/ClientConnection.h"
#include "core/socket/stream/QueueResult.h"
#include "core/socket/stream/SocketConnection.h"
#include "net/config/ConfigConnection.h"
#include "web/http/client/SocketContext.h"
#include "web/websocket/SubProtocolContext.h"
#include "web/websocket/SubProtocolFactory.h"
#include "web/websocket/client/SocketContextUpgradeFactory.h"
#include "web/websocket/client/SubProtocol.h"
#include "web/websocket/client/SubProtocolFactorySelector.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace ai::openai::codex::frontend::client {

    namespace {

        constexpr std::uint16_t CloseUnsupportedData = 1003;
        constexpr std::uint16_t ClosePolicyViolation = 1008;
        constexpr std::uint16_t CloseUnexpectedCondition = 1011;
        constexpr std::size_t WebSocketFramePayloadBytes = 16U * 1024U;
        constexpr std::size_t MaximumClientFrameHeaderBytes = 8;

        std::optional<std::size_t> framedMessageBytes(std::size_t payloadBytes) noexcept {
            const std::size_t frameCount = payloadBytes == 0 ? 1 : (payloadBytes - 1) / WebSocketFramePayloadBytes + 1;
            if (frameCount > (std::numeric_limits<std::size_t>::max() - payloadBytes) / MaximumClientFrameHeaderBytes) {
                return std::nullopt;
            }
            return payloadBytes + frameCount * MaximumClientFrameHeaderBytes;
        }

        class WebSocketHttpSocketContext final : public web::http::client::SocketContext {
        public:
            WebSocketHttpSocketContext(
                core::socket::stream::SocketConnection* connection,
                const std::function<void(const std::shared_ptr<web::http::client::MasterRequest>&)>& onHttpConnected,
                const std::function<void(const std::shared_ptr<web::http::client::MasterRequest>&)>& onHttpDisconnected,
                const std::string& hostHeader,
                bool pipelinedRequests,
                const web::http::ParserLimits& parserLimits,
                std::shared_ptr<WebSocketBinding> binding)
                : web::http::client::SocketContext(
                      connection, onHttpConnected, onHttpDisconnected, hostHeader, pipelinedRequests, parserLimits)
                , binding_(std::move(binding)) {
            }

            std::shared_ptr<WebSocketBinding> binding() const noexcept {
                return binding_;
            }

        private:
            std::shared_ptr<WebSocketBinding> binding_;
        };

    } // namespace

    class WebSocketSubProtocol final
        : public web::websocket::client::SubProtocol
        , public TransportEndpoint {
    public:
        WebSocketSubProtocol(web::websocket::SubProtocolContext* context,
                             std::shared_ptr<WebSocketBinding> binding)
            : web::websocket::client::SubProtocol(context, "codex", 0, 3)
            , binding_(std::move(binding)) {
        }

        ~WebSocketSubProtocol() override {
            detach("bridge WebSocket subprotocol destroyed");
        }

        bool send(const nlohmann::json& message) override {
            if (closing_ || binding_->active_ != this) {
                return false;
            }
            try {
                const std::string serialized = message.dump();
                if (serialized.size() > binding_->maximumFrameBytes_) {
                    return false;
                }
                core::socket::stream::SocketConnection* const socket = getSocketConnection();
                const std::optional<std::size_t> frameBytes = framedMessageBytes(serialized.size());
                if (socket == nullptr || !frameBytes) {
                    return false;
                }
                const auto* config = dynamic_cast<const net::config::ConfigConnection*>(socket->getConfigInstance());
                const std::size_t writerLimit = config == nullptr ? 0 : config->getMaximumWriteQueueBytes();
                const std::size_t queued = socket->getTotalQueued();
                const std::size_t sent = socket->getTotalSent();
                if (queued < sent) {
                    return false;
                }
                const std::size_t outstanding = queued - sent;
                if (writerLimit != 0 && (*frameBytes > writerLimit || outstanding > writerLimit - *frameBytes)) {
                    std::clog << "codex-bridge-client: WebSocket writer limit rejected frame-bytes=" << *frameBytes
                              << " writer-limit=" << writerLimit << " outstanding=" << outstanding << '\n';
                    return false;
                }
                const core::socket::stream::QueueResult probe = socket->trySendToPeer("", 0);
                if (probe != core::socket::stream::QueueResult::Queued) {
                    std::clog << "codex-bridge-client: WebSocket writer rejected queue-result="
                              << static_cast<int>(probe) << " outstanding=" << outstanding << '\n';
                    return false;
                }
                sendMessage(serialized);
                return true;
            } catch (...) {
                return false;
            }
        }

        void close(std::string_view reason) noexcept override {
            if (closing_) {
                return;
            }
            closing_ = true;
            inbound_.clear();
            const std::string bounded(reason.substr(0, 123));
            try {
                sendClose(ClosePolicyViolation, bounded.data(), bounded.size());
            } catch (...) {
                try {
                    if (getSocketConnection() != nullptr) {
                        getSocketConnection()->close();
                    }
                } catch (...) {
                }
            }
        }

    private:
        void onConnected() override {
            if (binding_->active_ != nullptr || !binding_->connection_.attach(*this)) {
                close("another frontend transport is already active");
                return;
            }
            binding_->active_ = this;
            attached_ = true;
            binding_->connection_.connected(*this);
        }

        void onMessageStart(int opCode) override {
            inbound_.clear();
            receivedOpCode_ = opCode;
            if (opCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
                closeWithStatus(CloseUnsupportedData, "bridge messages must be WebSocket text messages");
            }
        }

        void onMessageData(const char* chunk, std::size_t length) override {
            if (closing_ || receivedOpCode_ != web::websocket::SubProtocolContext::OpCode::TEXT) {
                return;
            }
            if (length > binding_->maximumFrameBytes_ || inbound_.size() > binding_->maximumFrameBytes_ - length) {
                closeWithStatus(ClosePolicyViolation, "bridge WebSocket message exceeds configured maximum");
                return;
            }
            try {
                inbound_.append(chunk, length);
            } catch (...) {
                closeWithStatus(CloseUnexpectedCondition, "bridge WebSocket buffering failed");
            }
        }

        void onMessageEnd() override {
            if (closing_ || receivedOpCode_ != web::websocket::SubProtocolContext::OpCode::TEXT) {
                inbound_.clear();
                return;
            }
            try {
                nlohmann::json message = nlohmann::json::parse(inbound_);
                inbound_.clear();
                binding_->connection_.receive(*this, std::move(message));
            } catch (...) {
                inbound_.clear();
                closeWithStatus(ClosePolicyViolation, "invalid bridge WebSocket JSON message");
            }
        }

        void onMessageError(std::uint16_t error) override {
            static_cast<void>(error);
            closeWithStatus(ClosePolicyViolation, "invalid bridge WebSocket message");
        }

        void onDisconnected() override {
            detach("bridge WebSocket disconnected");
        }

        bool onSignal(int signal) override {
            static_cast<void>(signal);
            binding_->connection_.shutdown();
            return true;
        }

        void closeWithStatus(std::uint16_t status, std::string_view reason) noexcept {
            if (closing_) {
                return;
            }
            closing_ = true;
            inbound_.clear();
            const std::string bounded(reason.substr(0, 123));
            try {
                sendClose(status, bounded.data(), bounded.size());
            } catch (...) {
            }
        }

        void detach(std::string reason) noexcept {
            if (!attached_) {
                return;
            }
            attached_ = false;
            inbound_.clear();
            if (binding_->active_ == this) {
                binding_->active_ = nullptr;
            }
            binding_->connection_.detach(*this, std::move(reason));
        }

        std::shared_ptr<WebSocketBinding> binding_;
        std::string inbound_;
        int receivedOpCode_ = 0;
        bool attached_ = false;
        bool closing_ = false;
    };

    namespace {

        class WebSocketClientSubProtocolFactory final
            : public web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol> {
        public:
            WebSocketClientSubProtocolFactory()
                : web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>("codex") {
            }

        private:
            web::websocket::client::SubProtocol* create(web::websocket::SubProtocolContext* context) override {
                core::socket::stream::SocketConnection* const connection =
                    context == nullptr ? nullptr : context->getSocketConnection();
                auto* httpContext = connection == nullptr
                    ? nullptr
                    : dynamic_cast<WebSocketHttpSocketContext*>(connection->getSocketContext());
                if (httpContext == nullptr || httpContext->getSocketConnection() != connection) {
                    return nullptr;
                }
                std::shared_ptr<WebSocketBinding> binding = httpContext->binding();
                return binding ? new WebSocketSubProtocol(context, std::move(binding)) : nullptr;
            }
        };

        web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>* createWebSocketFactory() {
            static WebSocketClientSubProtocolFactory factory;
            return &factory;
        }

    } // namespace

    WebSocketBinding::WebSocketBinding(ClientConnection& connection, std::size_t maximumFrameBytes)
        : connection_(connection)
        , maximumFrameBytes_(maximumFrameBytes) {
    }

    WebSocketBinding::~WebSocketBinding() {
        shutdown();
    }

    void WebSocketBinding::beginUpgrade(const std::shared_ptr<web::http::client::MasterRequest>& request,
                                        std::string endpoint) {
        upgradeCommitted_ = false;
        request->set("Sec-WebSocket-Protocol", "codex");
        request->upgrade(
            endpoint,
            "websocket",
            [](bool initiated) {
                if (!initiated) {
                    std::clog << "codex-bridge-client: WebSocket upgrade could not be initiated\n";
                }
            },
            [this](const auto&, const auto& response, bool success) {
                upgradeCommitted_ = success && response->get("upgrade") == "websocket" &&
                    response->get("sec-websocket-protocol") == "codex";
                if (!upgradeCommitted_) {
                    std::clog << "codex-bridge-client: WebSocket upgrade rejected\n";
                }
            },
            [](const auto&, const std::string& reason) {
                std::clog << "codex-bridge-client: WebSocket HTTP error=" << reason << '\n';
            });
    }

    void WebSocketBinding::httpDisconnected(
        const std::shared_ptr<web::http::client::MasterRequest>& request) noexcept {
        static_cast<void>(request);
        if (!std::exchange(upgradeCommitted_, false) && active_ == nullptr) {
            std::clog << "codex-bridge-client: HTTP transport disconnected before WebSocket activation\n";
        }
    }

    void WebSocketBinding::shutdown() noexcept {
        if (active_ != nullptr) {
            active_->close("bridge client shutdown");
        }
    }

    bool WebSocketBinding::connected() const noexcept {
        return active_ != nullptr;
    }

    WebSocketHttpSocketContextFactory::WebSocketHttpSocketContextFactory(
        const std::function<void(const std::shared_ptr<MasterRequest>&)>& onHttpConnected,
        const std::function<void(const std::shared_ptr<MasterRequest>&)>& onHttpDisconnected,
        const std::function<net::config::ConfigInstance&()>& getConfigInstance,
        std::shared_ptr<WebSocketBinding> binding)
        : onHttpConnected_(onHttpConnected)
        , onHttpDisconnected_(onHttpDisconnected)
        , configInstance_(getConfigInstance())
        , binding_(std::move(binding)) {
    }

    core::socket::stream::SocketContext*
    WebSocketHttpSocketContextFactory::create(core::socket::stream::SocketConnection* connection) {
        const auto* config = configInstance_.getSubCommand<web::http::client::ConfigHTTP>();
        return new WebSocketHttpSocketContext(connection,
                                              onHttpConnected_,
                                              onHttpDisconnected_,
                                              config->getHostHeader(),
                                              config->getPipelinedRequests(),
                                              config->getParserLimits(),
                                              binding_);
    }

    void linkWebSocketClient() {
        static const bool linked = [] {
            web::websocket::client::SocketContextUpgradeFactory::link();
            web::websocket::client::SubProtocolFactorySelector::link("codex", createWebSocketFactory);
            return true;
        }();
        static_cast<void>(linked);
    }

} // namespace ai::openai::codex::frontend::client
