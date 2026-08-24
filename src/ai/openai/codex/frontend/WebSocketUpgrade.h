/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_WEBSOCKETUPGRADE_H
#define AI_OPENAI_CODEX_FRONTEND_WEBSOCKETUPGRADE_H

#include <cstddef>

namespace core::socket::stream {
    class SocketConnection;
}

namespace web::websocket {
    class SubProtocolContext;
    namespace server {
        class SubProtocol;
    }
}

namespace ai::openai::codex::bridge {
    class CodexBridge;
}

namespace ai::openai::codex::frontend {

    class ScopedWebSocketUpgrade {
    public:
        ScopedWebSocketUpgrade(core::socket::stream::SocketConnection& connection,
                               bridge::CodexBridge& bridge,
                               std::size_t maximumFrameBytes);
        ScopedWebSocketUpgrade(const ScopedWebSocketUpgrade&) = delete;
        ScopedWebSocketUpgrade& operator=(const ScopedWebSocketUpgrade&) = delete;
        ~ScopedWebSocketUpgrade();

        web::websocket::server::SubProtocol* consume(web::websocket::SubProtocolContext* context);

    private:
        core::socket::stream::SocketConnection* connection_;
        bridge::CodexBridge* bridge_;
        std::size_t maximumFrameBytes_;
        ScopedWebSocketUpgrade* previous_;
        bool consumed_ = false;
    };

    void linkWebSocketSubProtocol();

} // namespace ai::openai::codex::frontend

#endif
