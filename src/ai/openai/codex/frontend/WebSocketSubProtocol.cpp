/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/WebSocketSubProtocol.h"

#include "core/socket/stream/SocketConnection.h"
#include "web/websocket/SubProtocolContext.h"

#include <iostream>
#include <utility>

namespace ai::openai::codex::frontend {

    namespace {

        constexpr std::uint16_t CloseUnsupportedData = 1003;
        constexpr std::uint16_t ClosePolicyViolation = 1008;
        constexpr std::uint16_t CloseUnexpectedCondition = 1011;

    } // namespace

    WebSocketSubProtocol::WebSocketSubProtocol(web::websocket::SubProtocolContext* context,
                                               bridge::CodexBridge& bridge,
                                               std::size_t maximumFrameBytes)
        : web::websocket::server::SubProtocol(context, std::string(WebSocketSubProtocolName), 15, 2)
        , bridge_(bridge)
        , maximumFrameBytes_(maximumFrameBytes) {
    }

    WebSocketSubProtocol::~WebSocketSubProtocol() {
        detachFrontend();
    }

    bool WebSocketSubProtocol::send(const nlohmann::json& message) {
        bool accepted = false;
        if (!closing_) {
            try {
                const std::string serialized = message.dump();
                core::socket::stream::SocketConnection* const connection = getSocketConnection();
                if (connection != nullptr && serialized.size() <= maximumFrameBytes_) {
                    const std::size_t queued = connection->getTotalQueued();
                    sendMessage(serialized);
                    accepted = connection->getTotalQueued() != queued;
                }
            } catch (const std::exception& exception) {
                std::clog << "codex-bridge: WebSocket serialization rejected connection=" << connectionId_ << " reason=" << exception.what()
                          << '\n';
            }
        }
        return accepted;
    }

    void WebSocketSubProtocol::close(std::string_view reason) {
        closeWebSocket(ClosePolicyViolation, reason);
    }

    void WebSocketSubProtocol::onConnected() {
        connectionId_ = bridge_.registerFrontend(*this);
        if (connectionId_.empty()) {
            closeWebSocket(ClosePolicyViolation, "frontend connection limit reached");
            return;
        }
        registered_ = true;
        std::clog << "codex-bridge: WebSocket frontend connected connection=" << connectionId_ << '\n';
    }

    void WebSocketSubProtocol::onMessageStart(int opCode) {
        inbound_.clear();
        receivedOpCode_ = opCode;
        if (opCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
            closeWebSocket(CloseUnsupportedData, "bridge messages must be WebSocket text messages");
        }
    }

    void WebSocketSubProtocol::onMessageData(const char* chunk, std::size_t chunkLength) {
        if (closing_ || receivedOpCode_ != web::websocket::SubProtocolContext::OpCode::TEXT) {
            return;
        }
        if (chunkLength > maximumFrameBytes_ || inbound_.size() > maximumFrameBytes_ - chunkLength) {
            closeWebSocket(ClosePolicyViolation, "bridge message exceeds configured maximum");
            return;
        }
        try {
            inbound_.append(chunk, chunkLength);
        } catch (...) {
            closeWebSocket(CloseUnexpectedCondition, "bridge message buffering failed");
        }
    }

    void WebSocketSubProtocol::onMessageEnd() {
        if (closing_ || receivedOpCode_ != web::websocket::SubProtocolContext::OpCode::TEXT) {
            inbound_.clear();
            return;
        }
        nlohmann::json message;
        try {
            message = nlohmann::json::parse(inbound_);
        } catch (const nlohmann::json::exception&) {
            inbound_.clear();
            closeWebSocket(ClosePolicyViolation, "invalid bridge JSON message");
            return;
        }
        inbound_.clear();
        try {
            bridge_.receiveFromFrontend(connectionId_, message);
        } catch (...) {
            closeWebSocket(CloseUnexpectedCondition, "bridge message handling failed");
        }
    }

    void WebSocketSubProtocol::onMessageError(std::uint16_t error) {
        static_cast<void>(error);
        closeWebSocket(ClosePolicyViolation, "invalid WebSocket message");
    }

    void WebSocketSubProtocol::onDisconnected() {
        detachFrontend();
    }

    bool WebSocketSubProtocol::onSignal(int signal) {
        static_cast<void>(signal);
        return true;
    }

    void WebSocketSubProtocol::detachFrontend() noexcept {
        if (!registered_) {
            return;
        }
        registered_ = false;
        bridge_.unregisterFrontend(connectionId_);
        std::clog << "codex-bridge: WebSocket frontend disconnected connection=" << connectionId_ << '\n';
        connectionId_.clear();
        inbound_.clear();
    }

    void WebSocketSubProtocol::closeWebSocket(std::uint16_t status, std::string_view reason) noexcept {
        if (closing_) {
            return;
        }
        closing_ = true;
        inbound_.clear();
        const std::string boundedReason(reason.substr(0, 123));
        try {
            sendClose(status, boundedReason.data(), boundedReason.size());
        } catch (...) {
            try {
                if (getSocketConnection() != nullptr) {
                    getSocketConnection()->close();
                }
            } catch (...) {
            }
        }
    }

} // namespace ai::openai::codex::frontend
