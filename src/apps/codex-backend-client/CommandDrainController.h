/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_CLIENT_COMMANDDRAINCONTROLLER_H
#define APPS_CODEX_BACKEND_CLIENT_COMMANDDRAINCONTROLLER_H

#include "ai/openai/codex/frontend/client/Client.h"
#include "apps/codex-backend-client/CommandParser.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace apps::codex_backend_client {

    struct CommandDrainCallbacks {
        std::function<void()> requestExit;
        std::function<void(std::string)> reportFailure;
    };

    class CommandDrainController {
    public:
        enum class SessionState { Connecting, Synchronizing, Ready, Closing, Closed };
        enum class InputState { Reading, DrainOnEof, ImmediateQuit };
        enum class Outcome { Running, Success, Failure };
        enum class NewStage { None, Queued, AwaitingThreadStartResponse, WaitingToSubmitTurn, AwaitingTurnStartResponse };

        CommandDrainController(ai::openai::codex::frontend::client::Client& sdk, CommandDrainCallbacks callbacks = {});

        [[nodiscard]] bool enqueue(RemoteCommand command);
        [[nodiscard]] bool enqueue(NewCommand command);
        void connectionStateChanged(ai::openai::codex::frontend::client::ConnectionState state);
        void inputEof();
        void inputFailed(std::string message);
        void connectionFailed(std::string message);
        void disconnected();
        void quit();

        [[nodiscard]] SessionState sessionState() const noexcept;
        [[nodiscard]] InputState inputState() const noexcept;
        [[nodiscard]] Outcome outcome() const noexcept;
        [[nodiscard]] bool failed() const noexcept;
        [[nodiscard]] const std::string& failureReason() const noexcept;
        [[nodiscard]] std::size_t queuedCount() const noexcept;
        [[nodiscard]] std::size_t pendingResponseCount() const noexcept;
        [[nodiscard]] std::size_t pendingSyncCount() const noexcept;
        [[nodiscard]] NewStage newStage() const noexcept;

    private:
        struct QueuedNewCommand {
            ai::openai::codex::typed::ThreadStartParams options;
            std::string prompt;
        };
        using QueuedEntry = std::variant<RemoteCommand, QueuedNewCommand>;

        [[nodiscard]] bool submit(RemoteCommand command);
        [[nodiscard]] bool submitNew(QueuedNewCommand command);
        void flushQueued();
        void operationCompleted(bool succeeded, const std::optional<ai::openai::codex::frontend::client::Error>& error);
        void synchronizationCompleted(
            const ai::openai::codex::frontend::client::OperationResult<ai::openai::codex::frontend::client::SynchronizationResult>& result);
        void maybeCompleteDrain();
        void finish(Outcome outcome);
        void fail(std::string message);

        ai::openai::codex::frontend::client::Client& sdk;
        CommandDrainCallbacks callbacks;
        SessionState currentSessionState = SessionState::Connecting;
        InputState currentInputState = InputState::Reading;
        Outcome currentOutcome = Outcome::Running;
        std::deque<QueuedEntry> queuedMessages;
        NewStage currentNewStage = NewStage::None;
        std::string currentFailureReason;
    };

} // namespace apps::codex_backend_client

#endif // APPS_CODEX_BACKEND_CLIENT_COMMANDDRAINCONTROLLER_H
