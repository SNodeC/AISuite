/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_STREAMSOCKETCONTEXTFACTORY_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_STREAMSOCKETCONTEXTFACTORY_H

#include "core/socket/stream/SocketContextFactory.h"

#include <cstddef>

namespace ai::openai::codex::frontend::client {

    class ClientConnection;

    class StreamSocketContextFactory final : public core::socket::stream::SocketContextFactory {
    public:
        StreamSocketContextFactory(ClientConnection& connection, std::size_t maximumFrameBytes);

        core::socket::stream::SocketContext* create(core::socket::stream::SocketConnection* socketConnection) override;

    private:
        ClientConnection& connection_;
        std::size_t maximumFrameBytes_;
    };

} // namespace ai::openai::codex::frontend::client

#endif
