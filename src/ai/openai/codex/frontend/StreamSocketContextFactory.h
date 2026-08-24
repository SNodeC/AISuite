/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_STREAMSOCKETCONTEXTFACTORY_H
#define AI_OPENAI_CODEX_FRONTEND_STREAMSOCKETCONTEXTFACTORY_H

#include "core/socket/stream/SocketContextFactory.h"

#include <cstddef>

namespace core::socket::stream {
    class SocketConnection;
}

namespace ai::openai::codex::bridge {
    class CodexBridge;
}

namespace ai::openai::codex::frontend {

    class StreamSocketContextFactory final : public core::socket::stream::SocketContextFactory {
    public:
        StreamSocketContextFactory(bridge::CodexBridge& bridge, std::size_t maximumFrameBytes);

        core::socket::stream::SocketContext* create(core::socket::stream::SocketConnection* socketConnection) override;

    private:
        bridge::CodexBridge& bridge_;
        std::size_t maximumFrameBytes_;
    };

} // namespace ai::openai::codex::frontend

#endif
