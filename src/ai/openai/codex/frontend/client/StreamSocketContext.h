/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_STREAMSOCKETCONTEXT_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_STREAMSOCKETCONTEXT_H

#include "ai/openai/codex/frontend/client/ClientConnection.h"
#include "ai/openai/codex/protocol/JsonLineFramer.h"
#include "core/socket/stream/SocketContext.h"

#include <cstddef>
#include <string_view>

namespace ai::openai::codex::frontend::client {

    class StreamSocketContext final
        : public core::socket::stream::SocketContext
        , public TransportEndpoint {
    public:
        StreamSocketContext(core::socket::stream::SocketConnection* socketConnection,
                            ClientConnection& connection,
                            std::size_t maximumFrameBytes);
        ~StreamSocketContext() override;

        bool send(const nlohmann::json& message) override;
        void close(std::string_view reason) noexcept override;

    private:
        void onConnected() override;
        void onDisconnected() override;
        std::size_t onReceivedFromPeer() override;
        bool onSignal(int signal) override;
        void detach(std::string reason) noexcept;

        ClientConnection& connection_;
        protocol::JsonLineFramer framer_;
        std::size_t maximumFrameBytes_;
        bool attached_ = false;
        bool disconnecting_ = false;
    };

} // namespace ai::openai::codex::frontend::client

#endif
