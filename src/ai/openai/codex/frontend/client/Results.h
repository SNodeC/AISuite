#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_RESULTS_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_RESULTS_H

#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/frontend/client/StateTypes.h"
#include "ai/openai/codex/typed/Results.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ai::openai::codex::frontend::client {

    enum class ErrorOrigin { Client, Transport, Protocol, Command };
    enum class ClientErrorCode {
        InvalidConfiguration,
        AlreadyConnected,
        NotConnected,
        NotReady,
        Closed,
        MethodUnavailable,
        MethodNotPermitted,
        TooManyPendingOperations,
        SynchronizationAlreadyActive,
        SerializationFailed,
        SendRejected,
        TransportFailure,
        DecodeFailure,
        UnexpectedMessage,
        StateDivergence,
        StateCapacityExceeded,
        ResponseTypeMismatch,
        RequestIdExhausted,
        CallbackFailure,
    };

    struct Error {
        ErrorOrigin origin = ErrorOrigin::Client;
        std::optional<ClientErrorCode> clientCode;
        std::optional<frontend::ErrorCode> protocolCode;
        std::string message;
        std::optional<std::int64_t> remoteCode;
        std::optional<frontend::Json> details;
        bool retryable = false;
    };

    class RequestId {
    public:
        RequestId() = default;
        explicit RequestId(std::string value)
            : id(std::move(value)) {
        }
        [[nodiscard]] const std::string& value() const noexcept {
            return id;
        }
        auto operator<=>(const RequestId&) const = default;

    private:
        std::string id;
    };

    template <typename T>
    struct OperationResult {
        RequestId requestId;
        std::optional<T> value;
        std::optional<Error> error;
        [[nodiscard]] bool succeeded() const noexcept {
            return value.has_value() && !error.has_value();
        }
        explicit operator bool() const noexcept {
            return succeeded();
        }
    };

    template <typename T>
    using CompletionHandler = std::function<void(const OperationResult<T>&)>;
    using DoneHandler = CompletionHandler<typed::Unit>;

    // Frontend Protocol thread and turn command results carry complete nested
    // projections.  Keep those values together instead of reducing them to
    // the ordered identifier indexes used by the canonical client State.
    struct TurnResultState {
        TurnState state;
        std::vector<ItemState> items;

        [[nodiscard]] const ItemState* item(const typed::ItemId& id) const noexcept {
            for (const ItemState& candidate : items) {
                if (candidate.id == id) {
                    return &candidate;
                }
            }
            return nullptr;
        }

        bool operator==(const TurnResultState&) const = default;
    };

    struct ThreadResultState {
        ThreadState state;
        std::vector<TurnResultState> turns;

        [[nodiscard]] const TurnResultState* turn(const typed::TurnId& id) const noexcept {
            for (const TurnResultState& candidate : turns) {
                if (candidate.state.id == id) {
                    return &candidate;
                }
            }
            return nullptr;
        }

        bool operator==(const ThreadResultState&) const = default;
    };

    struct ProjectedThreadResult {
        typed::ThreadId threadId;
        std::optional<ThreadResultState> thread;
    };

    using ThreadStartResult = ProjectedThreadResult;
    using ThreadResumeResult = ProjectedThreadResult;
    using ThreadReadResult = ProjectedThreadResult;

    struct ThreadListResult {
        std::vector<ThreadResultState> threads;
        std::optional<std::string> nextCursor;
        std::optional<std::string> backwardsCursor;
    };

    struct TurnStartResult {
        typed::TurnId turnId;
        std::optional<TurnResultState> turn;
    };

    struct Submission {
        std::optional<RequestId> requestId;
        std::optional<Error> error;
        [[nodiscard]] bool accepted() const noexcept {
            return requestId.has_value() && !error.has_value();
        }
        explicit operator bool() const noexcept {
            return accepted();
        }
    };

    using GeneratedOperationResult = OperationResult<frontend::generated::CompleteCommandResult>;
    using GeneratedCompletionHandler = std::function<void(const GeneratedOperationResult&)>;

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_RESULTS_H
