/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/FrontendRuntimeBridge.h"

#include "ai/openai/codex/frontend/FrontendService.h"
#include "core/socket/stream/SocketConnection.h"

#include <map>
#include <memory>
#include <utility>

namespace apps::codex_backend {

    namespace {

        ai::openai::codex::frontend::FrontendService* runtimeService = nullptr;
        std::map<core::socket::stream::SocketConnection*, ai::openai::codex::frontend::FrontendPeerContext> pendingPeers;

    } // namespace

    bool installFrontendRuntime(ai::openai::codex::frontend::FrontendService& service) noexcept {
        if (runtimeService != nullptr && runtimeService != std::addressof(service)) {
            return false;
        }
        runtimeService = std::addressof(service);
        return true;
    }

    void uninstallFrontendRuntime(ai::openai::codex::frontend::FrontendService& service) noexcept {
        if (runtimeService == std::addressof(service)) {
            pendingPeers.clear();
            runtimeService = nullptr;
        }
    }

    bool prepareFrontendWebSocket(core::socket::stream::SocketConnection& connection,
                                  ai::openai::codex::frontend::FrontendPeerContext peer) noexcept {
        if (runtimeService == nullptr) {
            return false;
        }
        try {
            pendingPeers.insert_or_assign(std::addressof(connection), std::move(peer));
            return true;
        } catch (...) {
            return false;
        }
    }

    void cancelFrontendWebSocket(core::socket::stream::SocketConnection& connection) noexcept {
        pendingPeers.erase(std::addressof(connection));
    }

    std::optional<FrontendWebSocketRuntime> takeFrontendWebSocketRuntime(core::socket::stream::SocketConnection& connection) noexcept {
        const auto found = pendingPeers.find(std::addressof(connection));
        if (runtimeService == nullptr || found == pendingPeers.end()) {
            return std::nullopt;
        }
        FrontendWebSocketRuntime runtime{runtimeService, std::move(found->second)};
        pendingPeers.erase(found);
        return runtime;
    }

} // namespace apps::codex_backend
