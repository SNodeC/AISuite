/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_STREAMSOCKETCONTEXT_H
#define AI_OPENAI_CODEX_FRONTEND_STREAMSOCKETCONTEXT_H

#include "ai/openai/codex/bridge/CodexBridge.h"
#include "ai/openai/codex/protocol/JsonLineFramer.h"
#include "core/socket/stream/SocketContext.h"

#include <cstddef>
#include <string>

namespace core::socket::stream {
    class SocketConnection;
}

namespace ai::openai::codex::frontend {

    class StreamSocketContext final
        : public core::socket::stream::SocketContext
        , public bridge::FrontendEndpoint {
    public:
        StreamSocketContext(core::socket::stream::SocketConnection* socketConnection,
                            bridge::CodexBridge& bridge,
                            std::size_t maximumFrameBytes);

        bool send(const nlohmann::json& message) override;
        void close(std::string_view reason) override;

    private:
        void onConnected() override;
        void onDisconnected() override;
        std::size_t onReceivedFromPeer() override;
        bool onSignal(int signum) override;

        bridge::CodexBridge& bridge_;
        protocol::JsonLineFramer framer_;
        std::string connectionId_;
        std::size_t maximumFrameBytes_;
        bool disconnecting_ = false;
    };

} // namespace ai::openai::codex::frontend

#endif
