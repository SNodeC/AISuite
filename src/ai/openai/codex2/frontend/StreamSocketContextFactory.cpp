/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex2/frontend/StreamSocketContextFactory.h"

#include "ai/openai/codex2/frontend/StreamSocketContext.h"

#include <stdexcept>

namespace ai::openai::codex2::frontend {

    StreamSocketContextFactory::StreamSocketContextFactory(bridge::CodexBridge& bridge, std::size_t maximumFrameBytes)
        : bridge_(bridge)
        , maximumFrameBytes_(maximumFrameBytes) {
        if (maximumFrameBytes_ == 0) {
            throw std::invalid_argument("frontend maximum frame size must be greater than zero");
        }
    }

    core::socket::stream::SocketContext*
    StreamSocketContextFactory::create(core::socket::stream::SocketConnection* socketConnection) {
        if (socketConnection == nullptr) {
            throw std::invalid_argument("frontend socket connection must not be null");
        }
        return new StreamSocketContext(socketConnection, bridge_, maximumFrameBytes_);
    }

} // namespace ai::openai::codex2::frontend
