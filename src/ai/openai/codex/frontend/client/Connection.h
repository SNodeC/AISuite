#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_CONNECTION_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_CONNECTION_H

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/client/Transport.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ai::openai::codex::frontend::client {

    class Client;

    struct ReceiveResult {
        bool accepted = false;
        std::optional<TransportError> error;
    };

    class Connection {
    public:
        Connection() noexcept;
        Connection(const Connection&) = delete;
        Connection(Connection&&) noexcept;
        Connection& operator=(const Connection&) = delete;
        Connection& operator=(Connection&&) noexcept;
        ~Connection();

        void transportConnected() noexcept;
        [[nodiscard]] ReceiveResult receive(std::string_view compactJson) noexcept;
        [[nodiscard]] ReceiveResult receive(const frontend::Json& message) noexcept;
        [[nodiscard]] ReceiveResult receive(const frontend::ServerMessage& message) noexcept;
        void transportDisconnected(std::optional<TransportError> error = std::nullopt) noexcept;
        void close(std::string reason = "frontend client connection closed") noexcept;
        [[nodiscard]] bool isOpen() const noexcept;
        [[nodiscard]] bool isTransportConnected() const noexcept;
        [[nodiscard]] std::uint64_t generation() const noexcept;

    private:
        friend class Client;
        struct Control;
        explicit Connection(std::shared_ptr<Control> control) noexcept;
        std::shared_ptr<Control> control;
    };

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_CONNECTION_H
