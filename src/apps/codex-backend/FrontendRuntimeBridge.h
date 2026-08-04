/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_FRONTENDRUNTIMEBRIDGE_H
#define APPS_CODEX_BACKEND_FRONTENDRUNTIMEBRIDGE_H

#include "ai/openai/codex/frontend/Security.h"

#include <optional>

namespace ai::openai::codex::frontend {
    class FrontendService;
}

namespace core::socket::stream {
    class SocketConnection;
}

namespace apps::codex_backend {

    struct FrontendWebSocketRuntime {
        ai::openai::codex::frontend::FrontendService* service = nullptr;
        ai::openai::codex::frontend::FrontendPeerContext peer;
    };

    bool installFrontendRuntime(ai::openai::codex::frontend::FrontendService& service) noexcept;
    void uninstallFrontendRuntime(ai::openai::codex::frontend::FrontendService& service) noexcept;

    bool prepareFrontendWebSocket(core::socket::stream::SocketConnection& connection,
                                  ai::openai::codex::frontend::FrontendPeerContext peer) noexcept;
    void cancelFrontendWebSocket(core::socket::stream::SocketConnection& connection) noexcept;
    [[nodiscard]] std::optional<FrontendWebSocketRuntime>
    takeFrontendWebSocketRuntime(core::socket::stream::SocketConnection& connection) noexcept;

} // namespace apps::codex_backend

#endif // APPS_CODEX_BACKEND_FRONTENDRUNTIMEBRIDGE_H
