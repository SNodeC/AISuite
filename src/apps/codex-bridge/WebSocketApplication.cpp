/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-bridge/WebSocketApplication.h"

#include "ai/openai/codex2/bridge/CodexBridge.h"
#include "ai/openai/codex2/frontend/WebSocketUpgrade.h"
#include "core/socket/stream/SocketConnection.h"
#include "express/Request.h"
#include "express/Response.h"
#include "express/Router.h"
#include "web/http/server/SocketContext.h"

#include <memory>
#include <stdexcept>
#include <utility>

namespace apps::codex_bridge {

    void configureWebSocketApplication(express::Router& router,
                                       ai::openai::codex2::bridge::CodexBridge& bridge,
                                       std::string endpoint,
                                       std::size_t maximumFrameBytes) {
        if (endpoint.empty() || endpoint.front() != '/' || endpoint.find_first_of("?#") != std::string::npos) {
            throw std::invalid_argument("WebSocket endpoint must be an absolute path without query or fragment");
        }
        ai::openai::codex2::frontend::linkWebSocketSubProtocol();
        router.get(
            endpoint,
            [&bridge, maximumFrameBytes](const std::shared_ptr<express::Request>& request,
                                         const std::shared_ptr<express::Response>& response) {
                if (!request || !response || response->getSocketContext() == nullptr ||
                    response->getSocketContext()->getSocketConnection() == nullptr) {
                    if (response) {
                        response->sendStatus(500);
                    }
                    return;
                }
                core::socket::stream::SocketConnection& connection =
                    *response->getSocketContext()->getSocketConnection();
                try {
                    ai::openai::codex2::frontend::ScopedWebSocketUpgrade upgrade(connection, bridge, maximumFrameBytes);
                    response->upgrade(request, [response](const std::string& selected) {
                        if (selected.empty()) {
                            response->sendStatus(400);
                        } else {
                            response->end();
                        }
                    });
                } catch (...) {
                    response->sendStatus(500);
                }
            });
    }

} // namespace apps::codex_bridge
