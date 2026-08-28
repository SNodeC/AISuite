/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-bridge/WebSocketApplication.h"

#include "ai/openai/codex/bridge/CodexBridge.h"
#include "ai/openai/codex/frontend/WebSocketUpgrade.h"
#include "core/socket/stream/SocketConnection.h"
#include "express/Request.h"
#include "express/Response.h"
#include "express/Router.h"
#include "web/http/server/SocketContext.h"
#include "web/http/http_utils.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace apps::codex_bridge {

    std::optional<std::string> resolveWebUiPath(const std::string& webRoot, const std::string& requestPath) {
        if (webRoot.empty() || requestPath.empty() || requestPath.front() != '/') {
            return std::nullopt;
        }
        try {
            const std::filesystem::path root = std::filesystem::weakly_canonical(webRoot);
            std::string decoded = httputils::url_decode(requestPath);
            if (decoded == "/") {
                decoded = "/index.html";
            }
            if (decoded.find('\0') != std::string::npos || decoded.find('\\') != std::string::npos) {
                return std::nullopt;
            }
            const std::filesystem::path candidate = std::filesystem::weakly_canonical(root / decoded.substr(1));
            auto rootPart = root.begin();
            auto candidatePart = candidate.begin();
            for (; rootPart != root.end() && candidatePart != candidate.end(); ++rootPart, ++candidatePart) {
                if (*rootPart != *candidatePart) {
                    return std::nullopt;
                }
            }
            if (rootPart != root.end() || candidate == root) {
                return std::nullopt;
            }
            return candidate.string();
        } catch (...) {
            return std::nullopt;
        }
    }

    void configureWebSocketApplication(express::Router& router,
                                       ai::openai::codex::bridge::CodexBridge& bridge,
                                       std::string endpoint,
                                       std::size_t maximumFrameBytes,
                                       std::string webRoot) {
        if (endpoint.empty() || endpoint.front() != '/' || endpoint.find_first_of("?#") != std::string::npos) {
            throw std::invalid_argument("WebSocket endpoint must be an absolute path without query or fragment");
        }
        ai::openai::codex::frontend::linkWebSocketSubProtocol();
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
                    ai::openai::codex::frontend::ScopedWebSocketUpgrade upgrade(connection, bridge, maximumFrameBytes);
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
        if (!webRoot.empty()) {
            router.get(
                "/*",
                [webRoot = std::move(webRoot)](const std::shared_ptr<express::Request>& request,
                                               const std::shared_ptr<express::Response>& response) {
                    if (!request || !response) {
                        return;
                    }
                    const std::optional<std::string> file = resolveWebUiPath(webRoot, request->path);
                    if (!file) {
                        response->sendStatus(404);
                        return;
                    }
                    response->set("Cache-Control", request->path == "/" || request->path == "/index.html"
                                                       ? "no-cache"
                                                       : "public, max-age=31536000, immutable");
                    response->sendFile(*file, [response](int error) {
                        if (error != 0) {
                            response->sendStatus(404);
                        }
                    });
                });
        }
    }

} // namespace apps::codex_bridge
