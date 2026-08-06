/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_FRONTENDWEBSOCKETSUBPROTOCOL_H
#define APPS_CODEX_BACKEND_FRONTENDWEBSOCKETSUBPROTOCOL_H

#include "ai/openai/codex/frontend/FrontendService.h"
#include "apps/codex-backend/FrontendRuntimeBridge.h"
#include "web/websocket/server/SubProtocol.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace web::websocket {
    class SubProtocolContext;
}

namespace apps::codex_backend {

    class FrontendWebSocketSubProtocol final : public web::websocket::server::SubProtocol {
    public:
        FrontendWebSocketSubProtocol(web::websocket::SubProtocolContext* context, FrontendWebSocketRuntime runtime);
        ~FrontendWebSocketSubProtocol() override;

    private:
        struct Lifetime;

        void onConnected() override;
        void onMessageStart(int opCode) override;
        void onMessageData(const char* chunk, std::size_t chunkLength) override;
        void onMessageEnd() override;
        void onMessageError(std::uint16_t error) override;
        void onDisconnected() override;
        bool onSignal(int signal) override;

        bool sendOutbound(const ai::openai::codex::frontend::OutboundMessage& message) noexcept;
        void closeBounded(std::uint16_t status, const char* reason) noexcept;
        void detachFrontend(std::string reason) noexcept;

        ai::openai::codex::frontend::FrontendService& service;
        ai::openai::codex::frontend::FrontendPeerContext peer;
        ai::openai::codex::frontend::FrontendConnection frontendConnection;
        std::shared_ptr<Lifetime> lifetime;
        std::string inbound;
        int receivedOpCode = 0;
        bool inputBlocked = false;
        bool closeStarted = false;
    };

} // namespace apps::codex_backend

#endif // APPS_CODEX_BACKEND_FRONTENDWEBSOCKETSUBPROTOCOL_H
