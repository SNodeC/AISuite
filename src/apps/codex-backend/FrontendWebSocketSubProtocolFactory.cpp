/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/FrontendWebSocketSubProtocolFactory.h"

#include "apps/codex-backend/FrontendRuntimeBridge.h"
#include "apps/codex-backend/FrontendWebSocketSubProtocol.h"
#include "core/socket/stream/SocketConnection.h"
#include "web/websocket/SubProtocolContext.h"

#include <utility>

namespace apps::codex_backend {

    web::websocket::server::SubProtocol* FrontendWebSocketSubProtocolFactory::create(web::websocket::SubProtocolContext* context) {
        if (context == nullptr || context->getSocketConnection() == nullptr) {
            return nullptr;
        }
        std::optional<FrontendWebSocketRuntime> runtime = takeFrontendWebSocketRuntime(*context->getSocketConnection());
        if (!runtime.has_value() || runtime->service == nullptr) {
            return nullptr;
        }
        return new FrontendWebSocketSubProtocol(context, std::move(*runtime));
    }

} // namespace apps::codex_backend

extern "C" web::websocket::SubProtocolFactory<web::websocket::server::SubProtocol>* codexServerSubProtocolFactory() {
    return new apps::codex_backend::FrontendWebSocketSubProtocolFactory("codex");
}
