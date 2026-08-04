/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_FRONTENDSTREAMSOCKETCONTEXTFACTORY_H
#define APPS_CODEX_BACKEND_FRONTENDSTREAMSOCKETCONTEXTFACTORY_H

#include "ai/openai/codex/frontend/FrontendService.h"
#include "apps/codex-backend/Configuration.h"
#include "core/socket/stream/SocketContextFactory.h"

#include <functional>

namespace core::socket::stream {
    class SocketConnection;
}

namespace apps::codex_backend {

    [[nodiscard]] constexpr bool isJsonLineStreamTransport(ai::openai::codex::frontend::FrontendTransportKind transport) noexcept {
        using ai::openai::codex::frontend::FrontendTransportKind;
        switch (transport) {
            case FrontendTransportKind::Unix:
            case FrontendTransportKind::Ipv4:
            case FrontendTransportKind::Ipv6:
            case FrontendTransportKind::TcpTls:
            case FrontendTransportKind::Rfcomm:
            case FrontendTransportKind::RfcommTls:
                return true;
            case FrontendTransportKind::WebSocket:
            case FrontendTransportKind::WebSocketTls:
            case FrontendTransportKind::InMemory:
                return false;
        }
        return false;
    }

    [[nodiscard]] constexpr bool isEncryptedTransport(ai::openai::codex::frontend::FrontendTransportKind transport) noexcept {
        using ai::openai::codex::frontend::FrontendTransportKind;
        return transport == FrontendTransportKind::TcpTls || transport == FrontendTransportKind::WebSocketTls ||
               transport == FrontendTransportKind::RfcommTls;
    }

    // Derives bounded numeric peer metadata directly from the accepted socket.
    // Loopback never implies local trust; localPeer remains false here.
    [[nodiscard]] ai::openai::codex::frontend::FrontendPeerContext
    streamPeerContextFromFileDescriptor(int descriptor, ai::openai::codex::frontend::FrontendTransportKind transport) noexcept;

    using FrontendStreamPeerResolver =
        std::function<ai::openai::codex::frontend::FrontendPeerContext(core::socket::stream::SocketConnection&)>;

    struct FrontendStreamSocketContextFactoryOptions {
        ai::openai::codex::frontend::FrontendTransportKind transport = ai::openai::codex::frontend::FrontendTransportKind::Ipv4;
        SocketFrontendOptions socket;
        FrontendStreamPeerResolver resolvePeer;
    };

    // One factory type composes with every SNode.C legacy/TLS stream server.
    // It borrows the application-owned FrontendService and never owns service,
    // backend, journal, controller, or authorization state.
    class FrontendStreamSocketContextFactory final : public core::socket::stream::SocketContextFactory {
    public:
        explicit FrontendStreamSocketContextFactory(ai::openai::codex::frontend::FrontendService& service,
                                                    FrontendStreamSocketContextFactoryOptions options = {});

        core::socket::stream::SocketContext* create(core::socket::stream::SocketConnection* socketConnection) override;

        [[nodiscard]] ai::openai::codex::frontend::FrontendService* serviceIdentity() const noexcept;
        [[nodiscard]] ai::openai::codex::frontend::FrontendTransportKind transport() const noexcept;

    private:
        ai::openai::codex::frontend::FrontendService& service;
        FrontendStreamSocketContextFactoryOptions options;
    };

} // namespace apps::codex_backend

#endif // APPS_CODEX_BACKEND_FRONTENDSTREAMSOCKETCONTEXTFACTORY_H
