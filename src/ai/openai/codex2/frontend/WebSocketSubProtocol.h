/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX2_FRONTEND_WEBSOCKETSUBPROTOCOL_H
#define AI_OPENAI_CODEX2_FRONTEND_WEBSOCKETSUBPROTOCOL_H

#include "ai/openai/codex2/bridge/CodexBridge.h"
#include "web/websocket/server/SubProtocol.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace web::websocket {
    class SubProtocolContext;
}

namespace ai::openai::codex2::frontend {

    inline constexpr std::string_view WebSocketSubProtocolName = "codex";

    class WebSocketSubProtocol final
        : public web::websocket::server::SubProtocol
        , public bridge::FrontendEndpoint {
    public:
        WebSocketSubProtocol(web::websocket::SubProtocolContext* context,
                             bridge::CodexBridge& bridge,
                             std::size_t maximumFrameBytes);
        ~WebSocketSubProtocol() override;

        bool send(const nlohmann::json& message) override;
        void close(std::string_view reason) override;

    private:
        void onConnected() override;
        void onMessageStart(int opCode) override;
        void onMessageData(const char* chunk, std::size_t chunkLength) override;
        void onMessageEnd() override;
        void onMessageError(std::uint16_t error) override;
        void onDisconnected() override;
        bool onSignal(int signal) override;

        void detachFrontend() noexcept;
        void closeWebSocket(std::uint16_t status, std::string_view reason) noexcept;

        bridge::CodexBridge& bridge_;
        std::size_t maximumFrameBytes_;
        std::string connectionId_;
        std::string inbound_;
        int receivedOpCode_ = 0;
        bool registered_ = false;
        bool closing_ = false;
    };

} // namespace ai::openai::codex2::frontend

#endif
