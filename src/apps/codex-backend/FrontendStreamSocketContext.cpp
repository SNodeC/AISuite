/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/FrontendStreamSocketContext.h"

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "apps/codex-backend/FrontendCloseReason.h"
#include "core/socket/stream/QueueResult.h"
#include "core/socket/stream/SocketConnection.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace apps::codex_backend {

    using ai::openai::codex::frontend::CodecError;
    using ai::openai::codex::frontend::ErrorCode;
    using ai::openai::codex::frontend::FrontendConnectionCallbacks;
    using ai::openai::codex::frontend::OutboundMessage;

    struct FrontendStreamSocketContext::Lifetime {
        FrontendStreamSocketContext* context = nullptr;
    };

    FrontendStreamSocketContext::FrontendStreamSocketContext(core::socket::stream::SocketConnection* socketConnection,
                                                             ai::openai::codex::frontend::FrontendService& service,
                                                             ai::openai::codex::frontend::FrontendPeerContext peer,
                                                             SocketFrontendOptions options)
        : core::socket::stream::SocketContext(socketConnection)
        , service(service)
        , peer(std::move(peer))
        , options(options)
        , framer(options.maximumFrameSize)
        , lifetime(std::make_shared<Lifetime>()) {
        lifetime->context = this;
    }

    void FrontendStreamSocketContext::onConnected() {
        try {
            const std::weak_ptr<Lifetime> weakLifetime = lifetime;
            frontendConnection =
                service.openConnection(peer,
                                       FrontendConnectionCallbacks{[weakLifetime](const OutboundMessage& message) {
                                                                       const std::shared_ptr<Lifetime> locked = weakLifetime.lock();
                                                                       return locked && locked->context && locked->context->send(message);
                                                                   },
                                                                   [weakLifetime](const std::string& reason) {
                                                                       const std::shared_ptr<Lifetime> locked = weakLifetime.lock();
                                                                       if (locked && locked->context) {
                                                                           locked->context->serviceClosed(reason);
                                                                       }
                                                                   }});
        } catch (...) {
            rejectFrame(ErrorCode::InternalError, "failed to open frontend connection");
        }
    }

    void FrontendStreamSocketContext::onDisconnected() {
        inputBlocked = true;
        disconnecting = true;
        if (lifetime) {
            lifetime->context = nullptr;
        }
        frontendConnection.close("frontend stream disconnected");
        framer.clear();
        lifetime.reset();
    }

    std::size_t FrontendStreamSocketContext::onReceivedFromPeer() {
        std::array<char, 16 * 1024> bytes{};
        const std::size_t size = readFromPeer(bytes.data(), bytes.size());
        if (size == 0 || disconnecting || inputBlocked) {
            return size;
        }

        try {
            const JsonLineFramer::Result frameResult = framer.push(std::string_view(bytes.data(), size), [this](std::string frame) {
                if (disconnecting || inputBlocked) {
                    return;
                }
                const auto receiveResult = frontendConnection.receive(std::string_view(frame));
                if (receiveResult.status == ai::openai::codex::frontend::ConnectionReceiveStatus::Closing) {
                    inputBlocked = true;
                } else if (receiveResult.status == ai::openai::codex::frontend::ConnectionReceiveStatus::Closed) {
                    inputBlocked = true;
                    serviceClosed(receiveResult.error ? receiveResult.error->message : "frontend service closed the stream");
                }
            });
            if (frameResult == JsonLineFramer::Result::FrameTooLarge) {
                rejectFrame(ErrorCode::FrameTooLarge, "frontend JSONL frame exceeds the configured maximum size");
            }
        } catch (...) {
            // A transport error must not reveal a credential or command value
            // captured by an exception from an application callback.
            rejectFrame(ErrorCode::InternalError, "frontend frame handler failed");
        }

        return size;
    }

    bool FrontendStreamSocketContext::onSignal([[maybe_unused]] int signum) {
        return true;
    }

    bool FrontendStreamSocketContext::send(const OutboundMessage& message) noexcept {
        if (disconnecting) {
            return false;
        }

        try {
            auto* socketConnection = getSocketConnection();
            if (socketConnection == nullptr) {
                return false;
            }
            std::string frame = message.compactJson;
            frame.push_back('\n');
            return socketConnection->trySendToPeer(frame) == core::socket::stream::QueueResult::Queued;
        } catch (...) {
            return false;
        }
    }

    void FrontendStreamSocketContext::serviceClosed(std::string reason) noexcept {
        if (disconnecting) {
            return;
        }
        logFrontendCloseReason("stream transport", reason);
        inputBlocked = true;
        disconnecting = true;
        try {
            shutdownRead();
            shutdownWrite();
        } catch (...) {
            try {
                close();
            } catch (...) {
            }
        }
    }

    void FrontendStreamSocketContext::rejectFrame(ErrorCode code, std::string message) noexcept {
        if (disconnecting) {
            return;
        }
        CodecError error;
        error.code = code;
        error.message = std::move(message);
        error.closeConnection = true;
        const auto result = frontendConnection.receiveError(std::move(error));
        if (result.status == ai::openai::codex::frontend::ConnectionReceiveStatus::Closing) {
            inputBlocked = true;
        } else if (result.status == ai::openai::codex::frontend::ConnectionReceiveStatus::Closed) {
            serviceClosed(result.error ? result.error->message : "frontend service rejected the stream frame");
        }
    }

} // namespace apps::codex_backend
