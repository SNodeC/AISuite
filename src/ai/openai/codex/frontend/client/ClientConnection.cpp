/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/ClientConnection.h"

#include "ai/openai/codex/frontend/CodexBridge.h"

#include <utility>

namespace ai::openai::codex::frontend::client {

    ClientConnection::ClientConnection(frontend::CodexBridge& sdk, ClientConnectionCallbacks callbacks)
        : sdk_(sdk)
        , callbacks_(std::move(callbacks)) {
        sdk_.setSender([this](nlohmann::json message) { return send(std::move(message)); });
    }

    ClientConnection::~ClientConnection() {
        shutdown();
        sdk_.setSender({});
    }

    bool ClientConnection::attach(TransportEndpoint& endpoint) noexcept {
        if (shuttingDown_ || (endpoint_ != nullptr && endpoint_ != &endpoint)) {
            return false;
        }
        endpoint_ = &endpoint;
        failureReported_ = false;
        return true;
    }

    void ClientConnection::connected(TransportEndpoint& endpoint) noexcept {
        if (endpoint_ != &endpoint || shuttingDown_ || online_) {
            return;
        }
        online_ = true;
        try {
            if (callbacks_.onConnected) {
                callbacks_.onConnected();
            }
        } catch (...) {
        }
    }

    void ClientConnection::receive(TransportEndpoint& endpoint, nlohmann::json message) noexcept {
        if (endpoint_ != &endpoint || !online_ || shuttingDown_) {
            return;
        }
        try {
            if (!sdk_.receive(message)) {
                failed(endpoint, "bridge client received an invalid envelope");
            }
        } catch (...) {
            failed(endpoint, "bridge client failed while dispatching an envelope");
        }
    }

    void ClientConnection::failed(TransportEndpoint& endpoint, std::string reason) noexcept {
        if (endpoint_ != &endpoint) {
            return;
        }
        reportFailure(reason);
        endpoint.close(reason);
    }

    void ClientConnection::detach(TransportEndpoint& endpoint, std::string reason) noexcept {
        if (endpoint_ != &endpoint) {
            return;
        }
        endpoint_ = nullptr;
        const bool wasOnline = std::exchange(online_, false);
        failureReported_ = false;
        sdk_.transportDisconnected(shuttingDown_ ? "bridge client shutdown" : reason);
        if (wasOnline) {
            try {
                if (callbacks_.onDisconnected) {
                    callbacks_.onDisconnected();
                }
            } catch (...) {
            }
        }
    }

    void ClientConnection::disconnect(std::string_view reason) noexcept {
        if (!shuttingDown_ && endpoint_ != nullptr) {
            endpoint_->close(reason);
        }
    }

    void ClientConnection::shutdown() noexcept {
        if (shuttingDown_) {
            return;
        }
        shuttingDown_ = true;
        if (endpoint_ != nullptr) {
            endpoint_->close("bridge client shutdown");
        } else {
            sdk_.transportDisconnected("bridge client shutdown");
        }
    }

    bool ClientConnection::online() const noexcept {
        return online_;
    }

    bool ClientConnection::attached() const noexcept {
        return endpoint_ != nullptr;
    }

    bool ClientConnection::send(nlohmann::json message) noexcept {
        if (!online_ || shuttingDown_ || endpoint_ == nullptr) {
            return false;
        }
        try {
            return endpoint_->send(message);
        } catch (...) {
            return false;
        }
    }

    void ClientConnection::reportFailure(std::string reason) noexcept {
        if (failureReported_) {
            return;
        }
        failureReported_ = true;
        try {
            if (callbacks_.onFailure) {
                callbacks_.onFailure(std::move(reason));
            }
        } catch (...) {
        }
    }

} // namespace ai::openai::codex::frontend::client
