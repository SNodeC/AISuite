/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/ClientConnection.h"

#include "apps/codex-backend-client/CodexBackendClientSocketContext.h"

#include <utility>

namespace apps::codex_backend_client {
    namespace sdk = ai::openai::codex::frontend::client;

    ClientConnection::ClientConnection(sdk::Client& sdk, ClientConnectionCallbacks callbacks)
        : sdk(sdk)
        , callbacks(std::move(callbacks)) {
    }

    ClientConnection::~ClientConnection() {
        disconnect();
    }

    void ClientConnection::disconnect() noexcept {
        if (context != nullptr) {
            context->disconnect();
        }
    }

    bool ClientConnection::connected() const noexcept {
        return online && context != nullptr && protocolConnection.isTransportConnected();
    }

    void ClientConnection::attach(CodexBackendClientSocketContext& attached) noexcept {
        context = &attached;
        online = false;
    }

    void ClientConnection::didConnect(CodexBackendClientSocketContext& attached) noexcept {
        if (context != &attached) {
            return;
        }
        protocolConnection = sdk.openConnection({[this](sdk::OutboundMessage message) {
                                                     return send(std::move(message));
                                                 },
                                                 [this](std::string reason) {
                                                     closeTransport(std::move(reason));
                                                 }});
        if (!protocolConnection.isOpen()) {
            try {
                if (callbacks.onFailure) {
                    callbacks.onFailure("exactly one outgoing frontend transport may be active");
                }
            } catch (...) {
            }
            closeTransport("frontend SDK rejected the transport attachment");
            return;
        }
        try {
            if (callbacks.onBeforeTransportConnected) {
                callbacks.onBeforeTransportConnected(callbacks.verifiedLocalUnix);
            }
        } catch (...) {
            try {
                if (callbacks.onFailure) {
                    callbacks.onFailure("frontend transport authentication preparation failed");
                }
            } catch (...) {
            }
            closeTransport("frontend transport authentication preparation failed");
            return;
        }
        online = true;
        protocolConnection.transportConnected();
        if (!protocolConnection.isTransportConnected()) {
            return;
        }
        try {
            if (callbacks.onConnected) {
                callbacks.onConnected();
            }
        } catch (...) {
        }
    }

    void ClientConnection::didReceive(CodexBackendClientSocketContext& attached, std::string frame) noexcept {
        if (context != &attached || !online) {
            return;
        }
        const sdk::ReceiveResult result = protocolConnection.receive(std::string_view(frame));
        if (!result.accepted && result.error && callbacks.onFailure) {
            try {
                callbacks.onFailure(result.error->message);
            } catch (...) {
            }
        }
    }

    void ClientConnection::didFail(CodexBackendClientSocketContext& attached, std::string message) noexcept {
        if (context != &attached) {
            return;
        }
        try {
            if (callbacks.onFailure) {
                callbacks.onFailure(std::move(message));
            }
        } catch (...) {
        }
    }

    void ClientConnection::detach(CodexBackendClientSocketContext& attached) noexcept {
        if (context != &attached) {
            return;
        }
        const bool notify = online;
        online = false;
        context = nullptr;
        protocolConnection.transportDisconnected(sdk::TransportError{"physical frontend transport disconnected", true});
        if (notify && callbacks.onDisconnected) {
            try {
                callbacks.onDisconnected();
            } catch (...) {
            }
        }
    }

    sdk::SendResult ClientConnection::send(sdk::OutboundMessage message) noexcept {
        if (!online || context == nullptr) {
            return {sdk::SendStatus::Closed, sdk::TransportError{"frontend transport is closed", true}};
        }
        try {
            if (callbacks.onOutbound) {
                callbacks.onOutbound(message);
            }
        } catch (...) {
        }
        return context->send(std::move(message));
    }

    void ClientConnection::closeTransport([[maybe_unused]] std::string reason) noexcept {
        disconnect();
    }

} // namespace apps::codex_backend_client
