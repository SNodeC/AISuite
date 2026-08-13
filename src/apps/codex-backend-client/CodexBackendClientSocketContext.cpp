/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/CodexBackendClientSocketContext.h"

#include "apps/codex-backend-client/ClientConnection.h"
#include "core/socket/stream/QueueResult.h"
#include "core/socket/stream/SocketConnection.h"

#include <array>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace apps::codex_backend_client {

    namespace sdk = ai::openai::codex::frontend::client;

    CodexBackendClientSocketContext::CodexBackendClientSocketContext(core::socket::stream::SocketConnection* socketConnection,
                                                                     ClientConnection& connection,
                                                                     std::size_t maximumFrameSize)
        : core::socket::stream::SocketContext(socketConnection)
        , connection(connection)
        , framer(maximumFrameSize) {
        if (maximumFrameSize == 0) {
            throw std::invalid_argument("the maximum JSONL frame size must be greater than zero");
        }
        connection.attach(*this);
    }

    void CodexBackendClientSocketContext::onConnected() {
        connection.didConnect(*this);
    }

    void CodexBackendClientSocketContext::onDisconnected() {
        disconnecting = true;
        framer.clear();
        connection.detach(*this);
    }

    std::size_t CodexBackendClientSocketContext::onReceivedFromPeer() {
        std::array<char, 16 * 1024> bytes{};
        const std::size_t size = readFromPeer(bytes.data(), bytes.size());
        if (size == 0 || disconnecting) {
            return size;
        }

        try {
            using JsonLineFramer = ai::openai::codex::frontend::internal::transport::JsonLineFramer;
            const JsonLineFramer::Result result = framer.push(std::string_view(bytes.data(), size), [this](std::string frame) {
                if (!disconnecting) {
                    handleFrame(std::move(frame));
                }
            });
            if (result == JsonLineFramer::Result::FrameTooLarge && !disconnecting) {
                fail("frontend server JSONL frame exceeds the configured maximum size");
            }
        } catch (const std::exception& exception) {
            fail(std::string("frontend frame handling failed: ") + exception.what());
        } catch (...) {
            fail("frontend frame handling failed");
        }
        return size;
    }

    bool CodexBackendClientSocketContext::onSignal([[maybe_unused]] int signum) {
        connection.shutdown();
        return true;
    }

    sdk::SendResult CodexBackendClientSocketContext::send(sdk::OutboundMessage message) noexcept {
        if (disconnecting) {
            return {sdk::SendStatus::Closed, sdk::TransportError{"frontend transport is closing", true}};
        }
        try {
            std::string frame = std::move(message.compactJson);
            frame.push_back('\n');
            core::socket::stream::SocketConnection* const socketConnection = getSocketConnection();
            if (socketConnection == nullptr) {
                return {sdk::SendStatus::Closed, sdk::TransportError{"frontend transport is closed", true}};
            }
            switch (socketConnection->trySendToPeer(frame)) {
                case core::socket::stream::QueueResult::Queued:
                    return {sdk::SendStatus::Accepted, std::nullopt};
                case core::socket::stream::QueueResult::WouldExceedLimit:
                    return {sdk::SendStatus::Backpressure, sdk::TransportError{"frontend transport writer queue is full", true}};
                case core::socket::stream::QueueResult::Closed:
                case core::socket::stream::QueueResult::ShutdownInProgress:
                    return {sdk::SendStatus::Closed, sdk::TransportError{"frontend transport is closed", true}};
            }
        } catch (...) {
            return {sdk::SendStatus::Failed, sdk::TransportError{"failed to queue frontend message", true}};
        }
        return {sdk::SendStatus::Failed, sdk::TransportError{"unknown frontend queue result", false}};
    }

    void CodexBackendClientSocketContext::disconnect() noexcept {
        if (disconnecting) {
            return;
        }
        disconnecting = true;
        try {
            close();
        } catch (...) {
        }
    }

    void CodexBackendClientSocketContext::handleFrame(std::string frame) noexcept {
        connection.didReceive(*this, std::move(frame));
    }

    void CodexBackendClientSocketContext::fail(std::string error) noexcept {
        if (disconnecting) {
            return;
        }
        connection.didFail(*this, std::move(error));
        disconnect();
    }

} // namespace apps::codex_backend_client
