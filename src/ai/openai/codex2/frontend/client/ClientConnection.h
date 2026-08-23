/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX2_FRONTEND_CLIENT_CLIENTCONNECTION_H
#define AI_OPENAI_CODEX2_FRONTEND_CLIENT_CLIENTCONNECTION_H

#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace ai::openai::codex2::frontend {
    class CodexBridge;
}

namespace ai::openai::codex2::frontend::client {

    class TransportEndpoint {
    public:
        virtual ~TransportEndpoint() = default;
        virtual bool send(const nlohmann::json& message) = 0;
        virtual void close(std::string_view reason) noexcept = 0;
    };

    struct ClientConnectionCallbacks {
        std::function<void()> onConnected;
        std::function<void()> onDisconnected;
        std::function<void(std::string)> onFailure;
    };

    class ClientConnection {
    public:
        ClientConnection(frontend::CodexBridge& sdk, ClientConnectionCallbacks callbacks = {});
        ~ClientConnection();

        ClientConnection(const ClientConnection&) = delete;
        ClientConnection& operator=(const ClientConnection&) = delete;

        bool attach(TransportEndpoint& endpoint) noexcept;
        void connected(TransportEndpoint& endpoint) noexcept;
        void receive(TransportEndpoint& endpoint, nlohmann::json message) noexcept;
        void failed(TransportEndpoint& endpoint, std::string reason) noexcept;
        void detach(TransportEndpoint& endpoint, std::string reason) noexcept;
        void disconnect(std::string_view reason) noexcept;
        void shutdown() noexcept;

        bool attached() const noexcept;
        bool online() const noexcept;

    private:
        bool send(nlohmann::json message) noexcept;
        void reportFailure(std::string reason) noexcept;

        frontend::CodexBridge& sdk_;
        ClientConnectionCallbacks callbacks_;
        TransportEndpoint* endpoint_ = nullptr;
        bool online_ = false;
        bool shuttingDown_ = false;
        bool failureReported_ = false;
    };

} // namespace ai::openai::codex2::frontend::client

#endif
