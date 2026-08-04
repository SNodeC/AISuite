/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_FRONTENDWEBAPPLICATION_H
#define APPS_CODEX_BACKEND_FRONTENDWEBAPPLICATION_H

#include "ai/openai/codex/frontend/FrontendService.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace express {
    class Router;
}

namespace apps::codex_backend {

    inline constexpr std::string_view FrontendWebSocketSubProtocol = "codex";

    struct FrontendWebApplicationOptions {
        std::string endpoint = "/frontend";
        std::optional<std::filesystem::path> staticRoot;
        std::vector<std::string> allowedOrigins;
        ai::openai::codex::frontend::FrontendTransportKind transport = ai::openai::codex::frontend::FrontendTransportKind::WebSocket;
        bool encrypted = false;
    };

    // Derives only transport facts available from the accepted socket. Web
    // frontends are never classified as local-trusted: localPeer remains
    // false even for a loopback address.
    [[nodiscard]] ai::openai::codex::frontend::FrontendPeerContext frontendWebPeerContextFromFileDescriptor(
        int descriptor, ai::openai::codex::frontend::FrontendTransportKind transport, bool encrypted) noexcept;

    // Owns the app-private HTTP/WebSocket policy captured by the Express
    // routes. The Router and every accepted context borrow the application-
    // owned FrontendService; neither creates service, journal, controller, or
    // method-dispatch state.
    class FrontendWebApplication {
    public:
        FrontendWebApplication(ai::openai::codex::frontend::FrontendService& service, FrontendWebApplicationOptions options);

        void configure(express::Router& router) const;

        [[nodiscard]] ai::openai::codex::frontend::FrontendService* serviceIdentity() const noexcept;

    private:
        class State;
        std::shared_ptr<State> state;
    };

} // namespace apps::codex_backend

#endif // APPS_CODEX_BACKEND_FRONTENDWEBAPPLICATION_H
