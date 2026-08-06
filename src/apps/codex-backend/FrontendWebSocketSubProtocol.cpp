/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/FrontendWebSocketSubProtocol.h"

#include "core/socket/stream/SocketConnection.h"
#include "web/websocket/SubProtocolContext.h"

#include <string_view>
#include <utility>

namespace apps::codex_backend {

    namespace {

        constexpr std::uint16_t CloseUnsupportedData = 1003;
        constexpr std::uint16_t ClosePolicyViolation = 1008;
        constexpr std::uint16_t CloseUnexpectedCondition = 1011;

    } // namespace

    struct FrontendWebSocketSubProtocol::Lifetime {
        FrontendWebSocketSubProtocol* subProtocol = nullptr;
    };

    FrontendWebSocketSubProtocol::FrontendWebSocketSubProtocol(web::websocket::SubProtocolContext* context,
                                                               FrontendWebSocketRuntime runtime)
        : web::websocket::server::SubProtocol(context, "codex")
        , service(*runtime.service)
        , peer(std::move(runtime.peer))
        , lifetime(std::make_shared<Lifetime>()) {
        lifetime->subProtocol = this;
    }

    FrontendWebSocketSubProtocol::~FrontendWebSocketSubProtocol() {
        detachFrontend("frontend WebSocket subprotocol destroyed");
    }

    void FrontendWebSocketSubProtocol::onConnected() {
        try {
            const std::weak_ptr<Lifetime> weakLifetime = lifetime;
            frontendConnection =
                service.openConnection(peer,
                                       ai::openai::codex::frontend::FrontendConnectionCallbacks{
                                           [weakLifetime](const ai::openai::codex::frontend::OutboundMessage& message) {
                                               const std::shared_ptr<Lifetime> locked = weakLifetime.lock();
                                               return locked && locked->subProtocol && locked->subProtocol->sendOutbound(message);
                                           },
                                           [weakLifetime]([[maybe_unused]] const std::string& reason) {
                                               const std::shared_ptr<Lifetime> locked = weakLifetime.lock();
                                               if (locked && locked->subProtocol) {
                                                   locked->subProtocol->closeBounded(ClosePolicyViolation, "frontend connection closed");
                                               }
                                           },
                                       });
            if (!frontendConnection.isOpen()) {
                closeBounded(ClosePolicyViolation, "frontend connection unavailable");
            }
        } catch (...) {
            closeBounded(CloseUnexpectedCondition, "frontend connection unavailable");
        }
    }

    void FrontendWebSocketSubProtocol::onMessageStart(int opCode) {
        inbound.clear();
        receivedOpCode = opCode;
        if (opCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
            closeBounded(CloseUnsupportedData, "binary frontend messages are not supported");
        }
    }

    void FrontendWebSocketSubProtocol::onMessageData(const char* chunk, std::size_t chunkLength) {
        if (inputBlocked || receivedOpCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
            return;
        }
        try {
            inbound.append(chunk, chunkLength);
        } catch (...) {
            closeBounded(CloseUnexpectedCondition, "frontend message buffering failed");
        }
    }

    void FrontendWebSocketSubProtocol::onMessageEnd() {
        if (inputBlocked || receivedOpCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
            inbound.clear();
            return;
        }
        try {
            const auto result = frontendConnection.receive(std::string_view(inbound));
            inbound.clear();
            if (result.status != ai::openai::codex::frontend::ConnectionReceiveStatus::Accepted) {
                inputBlocked = true;
            }
        } catch (...) {
            inbound.clear();
            closeBounded(CloseUnexpectedCondition, "frontend message handling failed");
        }
    }

    void FrontendWebSocketSubProtocol::onMessageError([[maybe_unused]] std::uint16_t error) {
        closeBounded(ClosePolicyViolation, "invalid frontend WebSocket message");
    }

    void FrontendWebSocketSubProtocol::onDisconnected() {
        detachFrontend("frontend WebSocket disconnected");
    }

    bool FrontendWebSocketSubProtocol::onSignal([[maybe_unused]] int signal) {
        return true;
    }

    bool FrontendWebSocketSubProtocol::sendOutbound(const ai::openai::codex::frontend::OutboundMessage& message) noexcept {
        if (closeStarted || !lifetime) {
            return false;
        }
        try {
            sendMessage(message.compactJson);
            return true;
        } catch (...) {
            return false;
        }
    }

    void FrontendWebSocketSubProtocol::closeBounded(std::uint16_t status, const char* reason) noexcept {
        if (closeStarted) {
            return;
        }
        inputBlocked = true;
        closeStarted = true;
        inbound.clear();
        try {
            sendClose(status, reason, std::char_traits<char>::length(reason));
        } catch (...) {
            try {
                if (getSocketConnection() != nullptr) {
                    getSocketConnection()->close();
                }
            } catch (...) {
            }
        }
    }

    void FrontendWebSocketSubProtocol::detachFrontend(std::string reason) noexcept {
        inputBlocked = true;
        if (lifetime) {
            lifetime->subProtocol = nullptr;
        }
        frontendConnection.close(std::move(reason));
        inbound.clear();
        lifetime.reset();
    }

} // namespace apps::codex_backend
