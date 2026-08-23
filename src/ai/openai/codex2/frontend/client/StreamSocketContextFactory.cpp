/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex2/frontend/client/StreamSocketContextFactory.h"

#include "ai/openai/codex2/frontend/client/StreamSocketContext.h"

namespace ai::openai::codex2::frontend::client {

    StreamSocketContextFactory::StreamSocketContextFactory(ClientConnection& connection,
                                                           std::size_t maximumFrameBytes)
        : connection_(connection)
        , maximumFrameBytes_(maximumFrameBytes) {
    }

    core::socket::stream::SocketContext*
    StreamSocketContextFactory::create(core::socket::stream::SocketConnection* socketConnection) {
        return new StreamSocketContext(socketConnection, connection_, maximumFrameBytes_);
    }

} // namespace ai::openai::codex2::frontend::client
