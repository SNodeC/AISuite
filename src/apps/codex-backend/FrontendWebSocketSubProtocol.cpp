/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/FrontendWebSocketSubProtocol.h"

#include "apps/codex-backend/Configuration.h"
#include "apps/codex-backend/FrontendCloseReason.h"
#include "core/socket/stream/QueueResult.h"
#include "core/socket/stream/SocketConnection.h"
#include "core/timer/Timer.h"
#include "net/config/ConfigConnection.h"
#include "net/config/ConfigInstance.h"
#include "utils/Timeval.h"
#include "web/websocket/SubProtocolContext.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace apps::codex_backend {

    namespace {

        constexpr std::uint16_t CloseUnsupportedData = 1003;
        constexpr std::uint16_t ClosePolicyViolation = 1008;
        constexpr std::uint16_t CloseUnexpectedCondition = 1011;
        constexpr std::size_t WebSocketFramePayloadBytes = 16U * 1024U;
        constexpr std::size_t MaximumServerFrameHeaderBytes = 4;

        std::optional<std::size_t> boundedWebSocketMessageBytes(std::size_t payloadBytes) noexcept {
            const std::size_t frameCount = payloadBytes == 0 ? 1 : (payloadBytes - 1) / WebSocketFramePayloadBytes + 1;
            if (frameCount > (std::numeric_limits<std::size_t>::max() - payloadBytes) / MaximumServerFrameHeaderBytes) {
                return std::nullopt;
            }
            // SNode.C fragments complete messages into 16-KiB frames. Server
            // frames are unmasked and need at most four header bytes each.
            return payloadBytes + frameCount * MaximumServerFrameHeaderBytes;
        }

        std::size_t configuredWriterBytes(const core::socket::stream::SocketConnection& connection) noexcept {
            const auto* config = dynamic_cast<const net::config::ConfigConnection*>(connection.getConfigInstance());
            const std::size_t writerBytes = config == nullptr ? 0 : config->getMaximumWriteQueueBytes();
            return writerBytes == 0 ? DEFAULT_MAXIMUM_OUTBOUND_BYTES : writerBytes;
        }

    } // namespace

    using ai::openai::codex::frontend::OutboundDeliveryStatus;

    struct FrontendWebSocketSubProtocol::Lifetime {
        FrontendWebSocketSubProtocol* subProtocol = nullptr;
    };

    FrontendWebSocketSubProtocol::FrontendWebSocketSubProtocol(web::websocket::SubProtocolContext* context,
                                                               ai::openai::codex::frontend::FrontendService& service,
                                                               ai::openai::codex::frontend::FrontendPeerContext peer)
        : web::websocket::server::SubProtocol(context, "codex")
        , service(service)
        , peer(std::move(peer))
        , lifetime(std::make_shared<Lifetime>()) {
        lifetime->subProtocol = this;
    }

    FrontendWebSocketSubProtocol::~FrontendWebSocketSubProtocol() {
        detachFrontend("frontend WebSocket subprotocol destroyed");
    }

    void FrontendWebSocketSubProtocol::onConnected() {
        deliveryRetryBackoff.reset();
        try {
            const std::weak_ptr<Lifetime> weakLifetime = lifetime;
            frontendConnection = service.openConnection(peer,
                                                        ai::openai::codex::frontend::FrontendConnectionCallbacks{
                                                            {},
                                                            [weakLifetime](const std::string& reason) {
                                                                const std::shared_ptr<Lifetime> locked = weakLifetime.lock();
                                                                if (locked && locked->subProtocol) {
                                                                    locked->subProtocol->serviceClosed(reason);
                                                                }
                                                            },
                                                            [weakLifetime](const ai::openai::codex::frontend::OutboundMessage& message) {
                                                                const std::shared_ptr<Lifetime> locked = weakLifetime.lock();
                                                                return locked && locked->subProtocol
                                                                           ? locked->subProtocol->sendOutbound(message)
                                                                           : OutboundDeliveryStatus::Closed;
                                                            },
                                                        });
            if (!frontendConnection.isOpen()) {
                closeBounded(ClosePolicyViolation, "frontend connection unavailable");
            }
        } catch (...) {
            closeBounded(CloseUnexpectedCondition, "frontend connection unavailable");
        }
    }

    void FrontendWebSocketSubProtocol::onMessageStart(int opCode) {
        inbound.clear();
        receivedOpCode = opCode;
        if (opCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
            closeBounded(CloseUnsupportedData, "binary frontend messages are not supported");
        }
    }

    void FrontendWebSocketSubProtocol::onMessageData(const char* chunk, std::size_t chunkLength) {
        if (inputBlocked || receivedOpCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
            return;
        }
        try {
            inbound.append(chunk, chunkLength);
        } catch (...) {
            closeBounded(CloseUnexpectedCondition, "frontend message buffering failed");
        }
    }

    void FrontendWebSocketSubProtocol::onMessageEnd() {
        if (inputBlocked || receivedOpCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
            inbound.clear();
            return;
        }
        try {
            const auto result = frontendConnection.receive(std::string_view(inbound));
            inbound.clear();
            if (result.status != ai::openai::codex::frontend::ConnectionReceiveStatus::Accepted) {
                inputBlocked = true;
            }
        } catch (...) {
            inbound.clear();
            closeBounded(CloseUnexpectedCondition, "frontend message handling failed");
        }
    }

    void FrontendWebSocketSubProtocol::onMessageError([[maybe_unused]] std::uint16_t error) {
        closeBounded(ClosePolicyViolation, "invalid frontend WebSocket message");
    }

    void FrontendWebSocketSubProtocol::onDisconnected() {
        deliveryRetryScheduled = false;
        deliveryRetryBackoff.reset();
        detachFrontend("frontend WebSocket disconnected");
    }

    bool FrontendWebSocketSubProtocol::onSignal([[maybe_unused]] int signal) {
        return true;
    }

    OutboundDeliveryStatus
    FrontendWebSocketSubProtocol::sendOutbound(const ai::openai::codex::frontend::OutboundMessage& message) noexcept {
        if (closeStarted || !lifetime) {
            return OutboundDeliveryStatus::Closed;
        }
        try {
            core::socket::stream::SocketConnection* socketConnection = getSocketConnection();
            if (socketConnection == nullptr) {
                return OutboundDeliveryStatus::Closed;
            }

            const std::optional<std::size_t> frameBytes = boundedWebSocketMessageBytes(message.compactJson.size());
            if (!frameBytes) {
                return OutboundDeliveryStatus::Closed;
            }
            const std::size_t writerLimit = configuredWriterBytes(*socketConnection);
            const std::size_t applicationLimit = writerLimit > DEFAULT_TRANSPORT_FRAMING_HEADROOM_BYTES
                                                     ? std::min(writerLimit - DEFAULT_TRANSPORT_FRAMING_HEADROOM_BYTES,
                                                                ai::openai::codex::frontend::DefaultFrontendServiceMaxOutboundBytes)
                                                     : 0;
            const std::size_t totalQueued = socketConnection->getTotalQueued();
            const std::size_t totalSent = socketConnection->getTotalSent();
            if (totalQueued < totalSent) {
                return OutboundDeliveryStatus::Closed;
            }
            const std::size_t writerBytes = totalQueued - totalSent;
            // Application JSON must remain inside the service-derived budget;
            // the writer reserve is exclusively for WebSocket/control/TLS
            // framing. Also verify the resulting unmasked WS bytes against
            // the physical writer before calling SNode.C's void transmitter.
            if (applicationLimit == 0 || message.compactJson.size() > applicationLimit || *frameBytes > writerLimit) {
                return OutboundDeliveryStatus::Closed;
            }
            if (writerBytes > applicationLimit - message.compactJson.size() || writerBytes > writerLimit - *frameBytes) {
                return scheduleDeliveryRetry(writerBytes) ? OutboundDeliveryStatus::Backpressured
                                                          : OutboundDeliveryStatus::Closed;
            }

            // The exact aggregate byte admission was decided above. This
            // zero-byte probe only observes terminal writer state; it does
            // not reserve capacity or stand in for the framed send.
            switch (socketConnection->trySendToPeer("", 0)) {
                case core::socket::stream::QueueResult::Queued:
                    break;
                case core::socket::stream::QueueResult::WouldExceedLimit:
                    return scheduleDeliveryRetry(writerBytes) ? OutboundDeliveryStatus::Backpressured
                                                              : OutboundDeliveryStatus::Closed;
                case core::socket::stream::QueueResult::Closed:
                case core::socket::stream::QueueResult::ShutdownInProgress:
                    return OutboundDeliveryStatus::Closed;
            }

            sendMessage(message.compactJson);
            deliveryRetryBackoff.recordAccepted();
            return OutboundDeliveryStatus::Accepted;
        } catch (...) {
            return OutboundDeliveryStatus::Closed;
        }
    }

    bool FrontendWebSocketSubProtocol::scheduleDeliveryRetry(std::size_t outstandingWriterBytes) noexcept {
        if (deliveryRetryScheduled || closeStarted || !lifetime) {
            return deliveryRetryScheduled && !closeStarted;
        }
        deliveryRetryScheduled = true;
        const std::size_t delayMilliseconds = deliveryRetryBackoff.recordBackpressure(outstandingWriterBytes);
        const std::weak_ptr<Lifetime> weakLifetime = lifetime;
        try {
            static_cast<void>(core::timer::Timer::singleshotTimer(
                [weakLifetime] {
                    const std::shared_ptr<Lifetime> locked = weakLifetime.lock();
                    if (!locked || !locked->subProtocol) {
                        return;
                    }
                    FrontendWebSocketSubProtocol& subProtocol = *locked->subProtocol;
                    subProtocol.deliveryRetryScheduled = false;
                    if (!subProtocol.closeStarted) {
                        subProtocol.frontendConnection.resumeDelivery();
                    }
                },
                utils::Timeval(static_cast<double>(delayMilliseconds) / 1000.0)));
            return true;
        } catch (...) {
            deliveryRetryScheduled = false;
            return false;
        }
    }

    void FrontendWebSocketSubProtocol::serviceClosed(std::string reason) noexcept {
        if (closeStarted) {
            return;
        }
        logFrontendCloseReason("WebSocket transport", reason);
        closeBounded(ClosePolicyViolation, "frontend connection closed");
    }

    void FrontendWebSocketSubProtocol::closeBounded(std::uint16_t status, const char* reason) noexcept {
        if (closeStarted) {
            return;
        }
        inputBlocked = true;
        closeStarted = true;
        deliveryRetryScheduled = false;
        deliveryRetryBackoff.reset();
        inbound.clear();
        try {
            sendClose(status, reason, std::char_traits<char>::length(reason));
        } catch (...) {
            try {
                if (getSocketConnection() != nullptr) {
                    getSocketConnection()->close();
                }
            } catch (...) {
            }
        }
    }

    void FrontendWebSocketSubProtocol::detachFrontend(std::string reason) noexcept {
        inputBlocked = true;
        deliveryRetryScheduled = false;
        deliveryRetryBackoff.reset();
        if (lifetime) {
            lifetime->subProtocol = nullptr;
        }
        frontendConnection.close(std::move(reason));
        inbound.clear();
        lifetime.reset();
    }

} // namespace apps::codex_backend
