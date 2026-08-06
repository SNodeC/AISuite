#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_TRANSPORT_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_TRANSPORT_H

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace ai::openai::codex::frontend::client {

    enum class OutboundKind { Hello, Command };
    enum class SendStatus { Accepted, Backpressure, Closed, Failed };

    struct OutboundMessage {
        OutboundKind kind = OutboundKind::Command;
        std::string compactJson;
        std::size_t serializedBytes = 0;
        bool sensitive = false;
    };

    struct TransportError {
        std::string message;
        bool retryable = false;
    };

    struct SendResult {
        SendStatus status = SendStatus::Failed;
        std::optional<TransportError> error;
    };

    struct TransportCallbacks {
        std::function<SendResult(OutboundMessage)> send;
        std::function<void(std::string)> close;
    };

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_TRANSPORT_H
