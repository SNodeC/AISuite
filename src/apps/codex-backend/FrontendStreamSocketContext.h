/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_FRONTENDSTREAMSOCKETCONTEXT_H
#define APPS_CODEX_BACKEND_FRONTENDSTREAMSOCKETCONTEXT_H

#include "ai/openai/codex/frontend/FrontendService.h"
#include "ai/openai/codex/frontend/internal/transport/JsonLineFramer.h"
#include "apps/codex-backend/Configuration.h"
#include "core/socket/stream/SocketContext.h"

#include <cstddef>
#include <memory>
#include <string>

namespace ai::openai::codex::frontend {
    enum class ErrorCode;
}

namespace core::socket::stream {
    class SocketConnection;
}

namespace apps::codex_backend {

    namespace detail {
        struct FrontendStreamSocketContextTestAccess;
    }

    // Transport-neutral JSONL bridge used by Unix, TCP, TLS, and RFCOMM
    // listeners. Authentication, authorization, projection, replay, and
    // command dispatch remain exclusively owned by FrontendService.
    class FrontendStreamSocketContext final : public core::socket::stream::SocketContext {
    public:
        FrontendStreamSocketContext(core::socket::stream::SocketConnection* socketConnection,
                                    ai::openai::codex::frontend::FrontendService& service,
                                    ai::openai::codex::frontend::FrontendPeerContext peer,
                                    SocketFrontendOptions options);

    private:
        struct Lifetime;

        void onConnected() override;
        void onDisconnected() override;
        std::size_t onReceivedFromPeer() override;
        bool onSignal(int signum) override;

        bool send(const ai::openai::codex::frontend::OutboundMessage& message) noexcept;
        void serviceClosed(std::string reason) noexcept;
        void rejectFrame(ai::openai::codex::frontend::ErrorCode code, std::string message) noexcept;

        ai::openai::codex::frontend::FrontendService& service;
        ai::openai::codex::frontend::FrontendPeerContext peer;
        SocketFrontendOptions options;
        ai::openai::codex::frontend::internal::transport::JsonLineFramer framer;
        ai::openai::codex::frontend::FrontendConnection frontendConnection;
        std::shared_ptr<Lifetime> lifetime;
        bool inputBlocked = false;
        bool disconnecting = false;

        friend struct detail::FrontendStreamSocketContextTestAccess;
    };

} // namespace apps::codex_backend

#endif // APPS_CODEX_BACKEND_FRONTENDSTREAMSOCKETCONTEXT_H
