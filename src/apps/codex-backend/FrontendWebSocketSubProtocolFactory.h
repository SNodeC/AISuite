/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_FRONTENDWEBSOCKETSUBPROTOCOLFACTORY_H
#define APPS_CODEX_BACKEND_FRONTENDWEBSOCKETSUBPROTOCOLFACTORY_H

#include "web/websocket/SubProtocolFactory.h"
#include "web/websocket/server/SubProtocol.h"

namespace apps::codex_backend {

    class FrontendWebSocketSubProtocolFactory final : public web::websocket::SubProtocolFactory<web::websocket::server::SubProtocol> {
    public:
        using web::websocket::SubProtocolFactory<web::websocket::server::SubProtocol>::SubProtocolFactory;

    private:
        web::websocket::server::SubProtocol* create(web::websocket::SubProtocolContext* context) override;
    };

} // namespace apps::codex_backend

extern "C" web::websocket::SubProtocolFactory<web::websocket::server::SubProtocol>* codexServerSubProtocolFactory();

#endif // APPS_CODEX_BACKEND_FRONTENDWEBSOCKETSUBPROTOCOLFACTORY_H
