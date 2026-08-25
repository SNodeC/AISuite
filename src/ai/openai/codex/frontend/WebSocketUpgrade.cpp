/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/WebSocketUpgrade.h"

#include "ai/openai/codex/frontend/WebSocketSubProtocol.h"
#include "core/socket/stream/SocketConnection.h"
#include "web/websocket/SubProtocolContext.h"
#include "web/websocket/SubProtocolFactory.h"
#include "web/websocket/server/SocketContextUpgradeFactory.h"
#include "web/websocket/server/SubProtocolFactorySelector.h"

#include <stdexcept>
#include <string>

namespace ai::openai::codex::frontend {

    namespace {

        thread_local ScopedWebSocketUpgrade* activeUpgrade = nullptr;

        class StaticWebSocketSubProtocolFactory final
            : public web::websocket::SubProtocolFactory<web::websocket::server::SubProtocol> {
        public:
            StaticWebSocketSubProtocolFactory()
                : web::websocket::SubProtocolFactory<web::websocket::server::SubProtocol>(
                      std::string(WebSocketSubProtocolName)) {
            }

        private:
            web::websocket::server::SubProtocol* create(web::websocket::SubProtocolContext* context) override {
                return activeUpgrade == nullptr ? nullptr : activeUpgrade->consume(context);
            }
        };

        web::websocket::SubProtocolFactory<web::websocket::server::SubProtocol>* createStaticFactory() {
            static StaticWebSocketSubProtocolFactory factory;
            return &factory;
        }

    } // namespace

    ScopedWebSocketUpgrade::ScopedWebSocketUpgrade(core::socket::stream::SocketConnection& connection,
                                                   bridge::CodexBridge& bridge,
                                                   std::size_t maximumFrameBytes)
        : connection_(&connection)
        , bridge_(&bridge)
        , maximumFrameBytes_(maximumFrameBytes)
        , previous_(activeUpgrade) {
        if (maximumFrameBytes_ == 0) {
            throw std::invalid_argument("WebSocket maximum frame size must be greater than zero");
        }
        activeUpgrade = this;
    }

    ScopedWebSocketUpgrade::~ScopedWebSocketUpgrade() {
        if (activeUpgrade == this) {
            activeUpgrade = previous_;
            return;
        }
        // Recover safely if scopes are destroyed out of stack order: unlink
        // this node so a later destruction cannot restore a dangling pointer.
        ScopedWebSocketUpgrade* current = activeUpgrade;
        while (current != nullptr && current->previous_ != this) {
            current = current->previous_;
        }
        if (current != nullptr) {
            current->previous_ = previous_;
        }
    }

    web::websocket::server::SubProtocol* ScopedWebSocketUpgrade::consume(web::websocket::SubProtocolContext* context) {
        core::socket::stream::SocketConnection* const requested = context == nullptr ? nullptr : context->getSocketConnection();
        if (consumed_ || requested == nullptr || requested != connection_ || bridge_ == nullptr) {
            return nullptr;
        }
        consumed_ = true;
        return new WebSocketSubProtocol(context, *bridge_, maximumFrameBytes_);
    }

    void linkWebSocketSubProtocol() {
        static const bool linked = [] {
            web::websocket::server::SocketContextUpgradeFactory::link();
            web::websocket::server::SubProtocolFactorySelector::link(std::string(WebSocketSubProtocolName), createStaticFactory);
            return true;
        }();
        static_cast<void>(linked);
    }

} // namespace ai::openai::codex::frontend
