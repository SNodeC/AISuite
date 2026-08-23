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
#include <iostream>
#include <memory>
#include <optional>
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

    namespace {

        std::string_view queueResultName(core::socket::stream::QueueResult result) noexcept {
            switch (result) {
                case core::socket::stream::QueueResult::Queued:
                    return "queued";
                case core::socket::stream::QueueResult::WouldExceedLimit:
                    return "would-exceed-limit";
                case core::socket::stream::QueueResult::Closed:
                    return "closed";
                case core::socket::stream::QueueResult::ShutdownInProgress:
                    return "shutdown-in-progress";
            }
            return "unknown";
        }

        std::string boundedTransportCloseReason(std::string_view reason) {
            constexpr std::size_t MaximumReasonBytes = 160;
            std::string bounded(reason.substr(0, MaximumReasonBytes));
            for (char& character : bounded) {
                const unsigned char value = static_cast<unsigned char>(character);
                if (value < 0x20U || value >= 0x7fU) {
                    character = '?';
                }
            }
            return bounded.empty() ? "frontend transport rejected outbound data" : bounded;
        }

        void recordTransportClose(ai::openai::codex::frontend::FrontendConnection& connection,
                                  std::string_view reason) noexcept {
            try {
                connection.recordTransportCloseReason(boundedTransportCloseReason(reason));
            } catch (...) {
            }
        }

        void logTerminalSendClose(const ai::openai::codex::frontend::FrontendPeerContext& peer,
                                  ai::openai::codex::frontend::FrontendConnection& connection,
                                  const core::socket::stream::SocketConnection* socketConnection,
                                  std::string_view reason,
                                  std::size_t frameBytes,
                                  std::size_t maximumFrameBytes,
                                  std::optional<core::socket::stream::QueueResult> queueResult,
                                  std::size_t totalQueued,
                                  std::size_t totalSent,
                                  std::string_view detail = {}) noexcept {
            recordTransportClose(connection, reason);
            try {
                const std::size_t outstandingWriterBytes = totalQueued >= totalSent ? totalQueued - totalSent : 0;
                std::clog << "codex-backend: frontend stream send closed: reason=" << boundedTransportCloseReason(reason)
                          << " transport=" << ai::openai::codex::frontend::toString(peer.transport)
                          << " fd=" << (socketConnection ? socketConnection->getFd() : -1)
                          << " frame-bytes=" << frameBytes
                          << " adapter-max-bytes=" << maximumFrameBytes
                          << " total-queued=" << totalQueued
                          << " total-sent=" << totalSent
                          << " outstanding-writer-bytes=" << outstandingWriterBytes;
                if (queueResult) {
                    std::clog << " queue-result=" << queueResultName(*queueResult);
                }
                if (!detail.empty()) {
                    std::clog << " detail=" << boundedTransportCloseReason(detail);
                }
                if (const std::optional<std::string> session = connection.sessionId(); session) {
                    std::clog << " session=" << boundedTransportCloseReason(*session);
                }
                std::clog << '\n';
            } catch (...) {
            }
        }

        void logRetryTransition(const ai::openai::codex::frontend::FrontendPeerContext& peer,
                                const core::socket::stream::SocketConnection* socketConnection,
                                std::string_view reason,
                                std::size_t outstandingWriterBytes,
                                std::optional<std::size_t> delayMilliseconds = std::nullopt) noexcept {
            try {
                std::clog << "codex-backend: frontend stream retry state: reason=" << boundedTransportCloseReason(reason)
                          << " transport=" << ai::openai::codex::frontend::toString(peer.transport)
                          << " fd=" << (socketConnection ? socketConnection->getFd() : -1)
                          << " outstanding-writer-bytes=" << outstandingWriterBytes;
                if (delayMilliseconds) {
                    std::clog << " retry-delay-ms=" << *delayMilliseconds;
                }
                std::clog << '\n';
            } catch (...) {
            }
        }

    } // namespace

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
        deliveryRetryAlreadyScheduledLogged = false;
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
            logTerminalSendClose(peer, frontendConnection, getSocketConnection(), "already-disconnecting", message.serializedBytes + 1,
                                 DEFAULT_MAXIMUM_OUTBOUND_BYTES, std::nullopt, 0, 0);
            return OutboundDeliveryStatus::Closed;
        }

        try {
            auto* socketConnection = getSocketConnection();
            if (socketConnection == nullptr) {
                logTerminalSendClose(peer, frontendConnection, nullptr, "socket-missing", message.serializedBytes + 1,
                                     DEFAULT_MAXIMUM_OUTBOUND_BYTES, std::nullopt, 0, 0);
                return OutboundDeliveryStatus::Closed;
            }
            const std::size_t frameBytes = message.compactJson.size() + 1;
            if (frameBytes > DEFAULT_MAXIMUM_OUTBOUND_BYTES) {
                logTerminalSendClose(peer,
                                     frontendConnection,
                                     socketConnection,
                                     "frame-over-adapter-limit",
                                     frameBytes,
                                     DEFAULT_MAXIMUM_OUTBOUND_BYTES,
                                     std::nullopt,
                                     socketConnection->getTotalQueued(),
                                     socketConnection->getTotalSent());
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
                    if (scheduleDeliveryRetry(writerBytes)) {
                        return OutboundDeliveryStatus::Backpressured;
                    }
                    logTerminalSendClose(peer,
                                         frontendConnection,
                                         socketConnection,
                                         "delivery-retry-scheduling-failed",
                                         frameBytes,
                                         DEFAULT_MAXIMUM_OUTBOUND_BYTES,
                                         std::nullopt,
                                         totalQueued,
                                         totalSent);
                    return OutboundDeliveryStatus::Closed;
                }
            }

            std::string frame = message.compactJson;
            frame.push_back('\n');
            switch (socketConnection->trySendToPeer(frame)) {
                case core::socket::stream::QueueResult::Queued:
                    deliveryRetryBackoff.recordAccepted();
                    deliveryRetryAlreadyScheduledLogged = false;
                    return OutboundDeliveryStatus::Accepted;
                case core::socket::stream::QueueResult::WouldExceedLimit:
                    // Retain the exact ServerCore head until the bounded
                    // writer has made room. If even an empty writer rejects
                    // it, an externally tightened transport bound makes the
                    // frame permanently undeliverable.
                    if (socketConnection->getTotalQueued() == socketConnection->getTotalSent()) {
                        logTerminalSendClose(peer,
                                             frontendConnection,
                                             socketConnection,
                                             "empty-writer-rejected",
                                             frameBytes,
                                             DEFAULT_MAXIMUM_OUTBOUND_BYTES,
                                             core::socket::stream::QueueResult::WouldExceedLimit,
                                             socketConnection->getTotalQueued(),
                                             socketConnection->getTotalSent());
                        return OutboundDeliveryStatus::Closed;
                    }
                    if (scheduleDeliveryRetry(writerBytes)) {
                        return OutboundDeliveryStatus::Backpressured;
                    }
                    logTerminalSendClose(peer,
                                         frontendConnection,
                                         socketConnection,
                                         "delivery-retry-scheduling-failed",
                                         frameBytes,
                                         DEFAULT_MAXIMUM_OUTBOUND_BYTES,
                                         core::socket::stream::QueueResult::WouldExceedLimit,
                                         totalQueued,
                                         totalSent);
                    return OutboundDeliveryStatus::Closed;
                case core::socket::stream::QueueResult::Closed:
                    logTerminalSendClose(peer,
                                         frontendConnection,
                                         socketConnection,
                                         "writer-closed",
                                         frameBytes,
                                         DEFAULT_MAXIMUM_OUTBOUND_BYTES,
                                         core::socket::stream::QueueResult::Closed,
                                         socketConnection->getTotalQueued(),
                                         socketConnection->getTotalSent());
                    return OutboundDeliveryStatus::Closed;
                case core::socket::stream::QueueResult::ShutdownInProgress:
                    logTerminalSendClose(peer,
                                         frontendConnection,
                                         socketConnection,
                                         "writer-shutdown",
                                         frameBytes,
                                         DEFAULT_MAXIMUM_OUTBOUND_BYTES,
                                         core::socket::stream::QueueResult::ShutdownInProgress,
                                         socketConnection->getTotalQueued(),
                                         socketConnection->getTotalSent());
                    return OutboundDeliveryStatus::Closed;
            }
        } catch (...) {
            logTerminalSendClose(peer,
                                 frontendConnection,
                                 getSocketConnection(),
                                 "exception",
                                 message.serializedBytes + 1,
                                 DEFAULT_MAXIMUM_OUTBOUND_BYTES,
                                 std::nullopt,
                                 getSocketConnection() ? getSocketConnection()->getTotalQueued() : 0,
                                 getSocketConnection() ? getSocketConnection()->getTotalSent() : 0);
            return OutboundDeliveryStatus::Closed;
        }
        logTerminalSendClose(peer, frontendConnection, getSocketConnection(), "exception", message.serializedBytes + 1,
                             DEFAULT_MAXIMUM_OUTBOUND_BYTES, std::nullopt, 0, 0, "fell-through-send-switch");
        return OutboundDeliveryStatus::Closed;
    }

    bool FrontendStreamSocketContext::scheduleDeliveryRetry(std::size_t outstandingWriterBytes) noexcept {
        if (deliveryRetryScheduled) {
            if (!deliveryRetryAlreadyScheduledLogged) {
                logRetryTransition(peer, getSocketConnection(), "retry-already-scheduled", outstandingWriterBytes);
                deliveryRetryAlreadyScheduledLogged = true;
            }
            return true;
        }
        if (disconnecting || !lifetime) {
            logRetryTransition(peer, getSocketConnection(), disconnecting ? "already-disconnecting" : "socket-missing",
                               outstandingWriterBytes);
            return false;
        }
        deliveryRetryScheduled = true;
        deliveryRetryAlreadyScheduledLogged = false;
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
                    context.deliveryRetryAlreadyScheduledLogged = false;
                    if (!context.disconnecting) {
                        context.frontendConnection.resumeDelivery();
                    }
                },
                utils::Timeval(static_cast<double>(delayMilliseconds) / 1000.0)));
            return true;
        } catch (...) {
            deliveryRetryScheduled = false;
            deliveryRetryAlreadyScheduledLogged = false;
            logRetryTransition(peer, getSocketConnection(), "retry-scheduling-threw", outstandingWriterBytes, delayMilliseconds);
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
        deliveryRetryAlreadyScheduledLogged = false;
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
