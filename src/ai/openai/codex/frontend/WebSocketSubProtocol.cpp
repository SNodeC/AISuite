/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/WebSocketSubProtocol.h"

#include "core/socket/stream/QueueResult.h"
#include "core/socket/stream/SocketConnection.h"
#include "net/config/ConfigConnection.h"
#include "net/config/ConfigInstance.h"
#include "web/websocket/SubProtocolContext.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>

namespace ai::openai::codex::frontend {

    namespace {

        constexpr std::uint16_t CloseUnsupportedData = 1003;
        constexpr std::uint16_t ClosePolicyViolation = 1008;
        constexpr std::uint16_t CloseUnexpectedCondition = 1011;
        constexpr std::size_t WebSocketFramePayloadBytes = 16U * 1024U;
        constexpr std::size_t MaximumServerFrameHeaderBytes = 4;

        std::optional<std::size_t> framedMessageBytes(std::size_t payloadBytes) noexcept {
            const std::size_t frameCount = payloadBytes == 0 ? 1 : (payloadBytes - 1) / WebSocketFramePayloadBytes + 1;
            if (frameCount > (std::numeric_limits<std::size_t>::max() - payloadBytes) / MaximumServerFrameHeaderBytes) {
                return std::nullopt;
            }
            return payloadBytes + frameCount * MaximumServerFrameHeaderBytes;
        }

        std::size_t configuredWriterBytes(const core::socket::stream::SocketConnection& connection) noexcept {
            const auto* config = dynamic_cast<const net::config::ConfigConnection*>(connection.getConfigInstance());
            return config == nullptr ? 0 : config->getMaximumWriteQueueBytes();
        }

    } // namespace

    WebSocketSubProtocol::WebSocketSubProtocol(web::websocket::SubProtocolContext* context,
                                               bridge::CodexBridge& bridge,
                                               std::size_t maximumFrameBytes)
        : web::websocket::server::SubProtocol(context, std::string(WebSocketSubProtocolName))
        , bridge_(bridge)
        , maximumFrameBytes_(maximumFrameBytes) {
    }

    WebSocketSubProtocol::~WebSocketSubProtocol() {
        detachFrontend();
    }

    bool WebSocketSubProtocol::send(const nlohmann::json& message) {
        if (closing_) {
            return false;
        }
        try {
            const std::string serialized = message.dump();
            if (serialized.size() > maximumFrameBytes_) {
                std::clog << "codex-bridge: WebSocket frame rejected connection=" << connectionId_
                          << " reason=frame-over-adapter-limit frame-bytes=" << serialized.size()
                          << " adapter-limit=" << maximumFrameBytes_ << '\n';
                return false;
            }

            core::socket::stream::SocketConnection* const connection = getSocketConnection();
            const std::optional<std::size_t> frameBytes = framedMessageBytes(serialized.size());
            if (connection == nullptr || !frameBytes) {
                return false;
            }
            const std::size_t writerLimit = configuredWriterBytes(*connection);
            const std::size_t totalQueued = connection->getTotalQueued();
            const std::size_t totalSent = connection->getTotalSent();
            if (totalQueued < totalSent) {
                return false;
            }
            const std::size_t outstanding = totalQueued - totalSent;
            if (writerLimit != 0 && (*frameBytes > writerLimit || outstanding > writerLimit - *frameBytes)) {
                std::clog << "codex-bridge: WebSocket writer rejected connection=" << connectionId_
                          << " reason=frame-over-writer-limit frame-bytes=" << *frameBytes
                          << " writer-limit=" << writerLimit << " outstanding=" << outstanding << '\n';
                return false;
            }
            const core::socket::stream::QueueResult probe = connection->trySendToPeer("", 0);
            if (probe != core::socket::stream::QueueResult::Queued) {
                std::clog << "codex-bridge: WebSocket writer rejected connection=" << connectionId_
                          << " reason=writer-not-queueable queue-result=" << static_cast<int>(probe)
                          << " outstanding=" << outstanding << '\n';
                return false;
            }
            sendMessage(serialized);
            return true;
        } catch (const std::exception& exception) {
            std::clog << "codex-bridge: WebSocket serialization rejected connection=" << connectionId_
                      << " reason=" << exception.what() << '\n';
            return false;
        }
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
