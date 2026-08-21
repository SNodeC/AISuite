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
#include "core/timer/Timer.h"
#include "utils/Timeval.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace apps::codex_backend {

    using ai::openai::codex::frontend::internal::transport::JsonLineFramer;
    using ai::openai::codex::frontend::CodecError;
    using ai::openai::codex::frontend::ErrorCode;
    using ai::openai::codex::frontend::FrontendConnectionCallbacks;
    using ai::openai::codex::frontend::OutboundMessage;
    using ai::openai::codex::frontend::OutboundDeliveryStatus;

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
        deliveryRetryBackoff.reset();
        try {
            const std::weak_ptr<Lifetime> weakLifetime = lifetime;
            frontendConnection =
                service.openConnection(peer,
                                       FrontendConnectionCallbacks{{},
                                                                   [weakLifetime](const std::string& reason) {
                                                                       const std::shared_ptr<Lifetime> locked = weakLifetime.lock();
                                                                       if (locked && locked->context) {
                                                                           locked->context->serviceClosed(reason);
                                                                       }
                                                                   },
                                                                   [weakLifetime](const OutboundMessage& message) {
                                                                       const std::shared_ptr<Lifetime> locked = weakLifetime.lock();
                                                                       return locked && locked->context
                                                                                  ? locked->context->send(message)
                                                                                  : OutboundDeliveryStatus::Closed;
                                                                   }});
        } catch (...) {
            rejectFrame(ErrorCode::InternalError, "failed to open frontend connection");
        }
    }

    void FrontendStreamSocketContext::onDisconnected() {
        inputBlocked = true;
        disconnecting = true;
        deliveryRetryScheduled = false;
        deliveryRetryBackoff.reset();
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
                const bool helloComplete = frontendConnection.helloComplete();
                const auto receiveResult = frontendConnection.receive(std::string_view(frame));
                if (!helloComplete && frontendConnection.helloComplete()) {
                    if (auto* socketConnection = getSocketConnection()) {
                        socketConnection->setReadTimeout(utils::Timeval({0, 0}));
                        socketConnection->setWriteTimeout(utils::Timeval({0, 0}));
                    }
                }
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

    OutboundDeliveryStatus FrontendStreamSocketContext::send(const OutboundMessage& message) noexcept {
        if (disconnecting) {
            return OutboundDeliveryStatus::Closed;
        }

        try {
            auto* socketConnection = getSocketConnection();
            if (socketConnection == nullptr) {
                return OutboundDeliveryStatus::Closed;
            }
            const std::size_t frameBytes = message.compactJson.size() + 1;
            if (frameBytes > DEFAULT_MAXIMUM_OUTBOUND_BYTES) {
                return OutboundDeliveryStatus::Closed;
            }

            // Avoid repeatedly copying a retained multi-MiB head while the
            // SNode.C writer is still too full to accept it. trySendToPeer
            // remains the authoritative final admission check.
            const std::size_t totalQueued = socketConnection->getTotalQueued();
            const std::size_t totalSent = socketConnection->getTotalSent();
            const std::size_t writerBytes = totalQueued >= totalSent ? totalQueued - totalSent : 0;
            if (totalQueued >= totalSent) {
                if (writerBytes > DEFAULT_MAXIMUM_OUTBOUND_BYTES - frameBytes) {
                    return scheduleDeliveryRetry(writerBytes) ? OutboundDeliveryStatus::Backpressured
                                                              : OutboundDeliveryStatus::Closed;
                }
            }

            std::string frame = message.compactJson;
            frame.push_back('\n');
            switch (socketConnection->trySendToPeer(frame)) {
                case core::socket::stream::QueueResult::Queued:
                    deliveryRetryBackoff.recordAccepted();
                    return OutboundDeliveryStatus::Accepted;
                case core::socket::stream::QueueResult::WouldExceedLimit:
                    // Retain the exact ServerCore head until the bounded
                    // writer has made room. If even an empty writer rejects
                    // it, an externally tightened transport bound makes the
                    // frame permanently undeliverable.
                    if (socketConnection->getTotalQueued() == socketConnection->getTotalSent()) {
                        return OutboundDeliveryStatus::Closed;
                    }
                    return scheduleDeliveryRetry(writerBytes) ? OutboundDeliveryStatus::Backpressured
                                                              : OutboundDeliveryStatus::Closed;
                case core::socket::stream::QueueResult::Closed:
                case core::socket::stream::QueueResult::ShutdownInProgress:
                    return OutboundDeliveryStatus::Closed;
            }
        } catch (...) {
            return OutboundDeliveryStatus::Closed;
        }
        return OutboundDeliveryStatus::Closed;
    }

    bool FrontendStreamSocketContext::scheduleDeliveryRetry(std::size_t outstandingWriterBytes) noexcept {
        if (deliveryRetryScheduled || disconnecting || !lifetime) {
            return deliveryRetryScheduled && !disconnecting;
        }
        deliveryRetryScheduled = true;
        const std::size_t delayMilliseconds = deliveryRetryBackoff.recordBackpressure(outstandingWriterBytes);
        const std::weak_ptr<Lifetime> weakLifetime = lifetime;
        try {
            static_cast<void>(core::timer::Timer::singleshotTimer(
                [weakLifetime] {
                    const std::shared_ptr<Lifetime> locked = weakLifetime.lock();
                    if (!locked || !locked->context) {
                        return;
                    }
                    FrontendStreamSocketContext& context = *locked->context;
                    context.deliveryRetryScheduled = false;
                    if (!context.disconnecting) {
                        context.frontendConnection.resumeDelivery();
                    }
                },
                utils::Timeval(static_cast<double>(delayMilliseconds) / 1000.0)));
            return true;
        } catch (...) {
            deliveryRetryScheduled = false;
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
        deliveryRetryScheduled = false;
        deliveryRetryBackoff.reset();
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
