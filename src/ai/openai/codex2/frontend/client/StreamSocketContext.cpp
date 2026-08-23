/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex2/frontend/client/StreamSocketContext.h"

#include "core/socket/stream/QueueResult.h"
#include "core/socket/stream/SocketConnection.h"
#include "net/config/ConfigConnection.h"
#include "net/config/ConfigInstance.h"

#include <array>
#include <iostream>
#include <utility>

namespace ai::openai::codex2::frontend::client {

    StreamSocketContext::StreamSocketContext(core::socket::stream::SocketConnection* socketConnection,
                                             ClientConnection& connection,
                                             std::size_t maximumFrameBytes)
        : core::socket::stream::SocketContext(socketConnection)
        , connection_(connection)
        , framer_(maximumFrameBytes)
        , maximumFrameBytes_(maximumFrameBytes)
        , attached_(connection_.attach(*this)) {
        if (!attached_) {
            disconnecting_ = true;
        }
    }

    StreamSocketContext::~StreamSocketContext() {
        detach("bridge stream context destroyed");
    }

    bool StreamSocketContext::send(const nlohmann::json& message) {
        if (disconnecting_) {
            return false;
        }
        try {
            const std::string frame = protocol::JsonLineFramer::encode(message, maximumFrameBytes_);
            core::socket::stream::SocketConnection* const socket = getSocketConnection();
            if (socket == nullptr) {
                return false;
            }
            const core::socket::stream::QueueResult result = socket->trySendToPeer(frame);
            if (result == core::socket::stream::QueueResult::Queued) {
                return true;
            }
            const auto* config = dynamic_cast<const net::config::ConfigConnection*>(socket->getConfigInstance());
            const std::size_t queued = socket->getTotalQueued();
            const std::size_t sent = socket->getTotalSent();
            std::clog << "codex-bridge-client: stream send rejected queue-result=" << static_cast<int>(result)
                      << " frame-bytes=" << frame.size() << " writer-limit="
                      << (config == nullptr ? 0 : config->getMaximumWriteQueueBytes()) << " total-queued=" << queued
                      << " total-sent=" << sent << " outstanding=" << (queued >= sent ? queued - sent : 0) << '\n';
        } catch (const std::exception& exception) {
            std::clog << "codex-bridge-client: stream send failed reason=" << exception.what() << '\n';
        }
        return false;
    }

    void StreamSocketContext::close(std::string_view reason) noexcept {
        if (disconnecting_) {
            return;
        }
        disconnecting_ = true;
        std::clog << "codex-bridge-client: closing stream reason=" << reason << '\n';
        try {
            core::socket::stream::SocketContext::close();
        } catch (...) {
        }
    }

    void StreamSocketContext::onConnected() {
        if (!attached_) {
            close("another frontend transport is already active");
            return;
        }
        connection_.connected(*this);
    }

    void StreamSocketContext::onDisconnected() {
        disconnecting_ = true;
        detach("bridge stream disconnected");
    }

    std::size_t StreamSocketContext::onReceivedFromPeer() {
        std::array<char, 16U * 1024U> bytes{};
        const std::size_t size = readFromPeer(bytes.data(), bytes.size());
        if (size == 0 || disconnecting_) {
            return size;
        }
        const bool accepted = framer_.consume(
            std::string_view(bytes.data(), size),
            [this](nlohmann::json message) {
                if (!disconnecting_) {
                    connection_.receive(*this, std::move(message));
                }
            },
            [this](std::string reason) { connection_.failed(*this, std::move(reason)); });
        if (!accepted) {
            close("invalid or oversized bridge JSONL frame");
        }
        return size;
    }

    bool StreamSocketContext::onSignal(int signal) {
        static_cast<void>(signal);
        connection_.shutdown();
        return true;
    }

    void StreamSocketContext::detach(std::string reason) noexcept {
        if (!attached_) {
            return;
        }
        attached_ = false;
        framer_.reset();
        connection_.detach(*this, std::move(reason));
    }

} // namespace ai::openai::codex2::frontend::client
