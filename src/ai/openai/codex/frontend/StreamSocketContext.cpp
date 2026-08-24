/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/StreamSocketContext.h"

#include "core/socket/stream/QueueResult.h"
#include "core/socket/stream/SocketConnection.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace ai::openai::codex::frontend {

    StreamSocketContext::StreamSocketContext(core::socket::stream::SocketConnection* socketConnection,
                                             bridge::CodexBridge& bridge,
                                             std::size_t maximumFrameBytes)
        : core::socket::stream::SocketContext(socketConnection)
        , bridge_(bridge)
        , framer_(maximumFrameBytes)
        , maximumFrameBytes_(maximumFrameBytes) {
    }

    bool StreamSocketContext::send(const nlohmann::json& message) {
        if (disconnecting_) {
            return false;
        }
        try {
            const std::string frame = protocol::JsonLineFramer::encode(message, maximumFrameBytes_);
            const core::socket::stream::QueueResult result = trySendToPeer(frame);
            if (result == core::socket::stream::QueueResult::Queued) {
                return true;
            }
            std::clog << "codex-bridge: frontend send rejected connection=" << connectionId_
                      << " frame-bytes=" << frame.size() << " queue-result=" << static_cast<int>(result)
                      << " total-queued=" << getTotalQueued() << " total-sent=" << getTotalSent() << '\n';
        } catch (const std::exception& exception) {
            std::clog << "codex-bridge: frontend serialization rejected connection=" << connectionId_
                      << " reason=" << exception.what() << '\n';
        }
        return false;
    }

    void StreamSocketContext::close(std::string_view reason) {
        if (disconnecting_) {
            return;
        }
        disconnecting_ = true;
        std::clog << "codex-bridge: closing frontend connection=" << connectionId_ << " reason=" << reason << '\n';
        core::socket::stream::SocketContext::close();
    }

    void StreamSocketContext::onConnected() {
        connectionId_ = bridge_.registerFrontend(*this);
        std::clog << "codex-bridge: frontend connected connection=" << connectionId_ << '\n';
    }

    void StreamSocketContext::onDisconnected() {
        disconnecting_ = true;
        bridge_.unregisterFrontend(connectionId_);
        std::clog << "codex-bridge: frontend disconnected connection=" << connectionId_ << '\n';
        connectionId_.clear();
        framer_.reset();
    }

    std::size_t StreamSocketContext::onReceivedFromPeer() {
        std::array<char, 16 * 1024> bytes{};
        const std::size_t size = readFromPeer(bytes.data(), bytes.size());
        if (size == 0 || disconnecting_) {
            return size;
        }

        const bool accepted = framer_.consume(
            std::string_view(bytes.data(), size),
            [this](nlohmann::json message) {
                if (!disconnecting_) {
                    bridge_.receiveFromFrontend(connectionId_, message);
                }
            },
            [this](std::string error) {
                std::clog << "codex-bridge: invalid frontend frame connection=" << connectionId_ << " reason=" << error << '\n';
            });
        if (!accepted) {
            close("invalid or oversized JSONL frame");
        }
        return size;
    }

    bool StreamSocketContext::onSignal(int signum) {
        static_cast<void>(signum);
        return true;
    }

} // namespace ai::openai::codex::frontend
