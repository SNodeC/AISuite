/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_CLIENT_COMMANDDRAINCONTROLLER_H
#define APPS_CODEX_BACKEND_CLIENT_COMMANDDRAINCONTROLLER_H

#include "ai/openai/codex/frontend/client/Client.h"
#include "apps/codex-backend-client/CommandParser.h"
#include "apps/codex-backend-client/Configuration.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace apps::codex_backend_client {

    struct CommandDrainCallbacks {
        std::function<void()> requestExit;
        std::function<void(std::string)> reportFailure;
        // A non-empty result rejects this pre-acceptance reconnect request
        // without changing the physical connection or terminating the app.
        std::function<std::optional<std::string>()> requestReconnect;
    };

    class CommandDrainController {
    public:
        enum class SessionState { Disconnected, Connecting, Synchronizing, Ready, ShuttingDown, Closed };
        enum class InputState { Reading, DrainOnEof, ImmediateQuit };
        enum class Outcome { Running, Success, Failure };
        enum class NewStage { None, Queued, AwaitingThreadStartResponse, WaitingToSubmitTurn, AwaitingTurnStartResponse };

        CommandDrainController(ai::openai::codex::frontend::client::Client& sdk,
                               CommandDrainCallbacks callbacks = {},
                               CommandQueueLimits queueLimits = {});

        [[nodiscard]] bool enqueue(RemoteCommand command, std::size_t retainedInputBytes = 0);
        [[nodiscard]] bool enqueue(NewCommand command, std::size_t retainedInputBytes = 0);
        void localCommandFailed(std::string message);
        [[nodiscard]] bool reconnect();
        void connectionAttemptFailed(std::string message);
        void connectionStateChanged(ai::openai::codex::frontend::client::ConnectionState state);
        void connectionStateChanged(const ai::openai::codex::frontend::client::ConnectionStateChange& change);
        void localShutdownRequested() noexcept;
        void inputEof();
        void inputFailed(std::string message);
        void startupFailed(std::string message);
        void connectionFailed(std::string message);
        void disconnected();
        void quit();

        [[nodiscard]] SessionState sessionState() const noexcept;
        [[nodiscard]] InputState inputState() const noexcept;
        [[nodiscard]] Outcome outcome() const noexcept;
        [[nodiscard]] bool failed() const noexcept;
        [[nodiscard]] bool applicationShutdownActive() const noexcept;
        [[nodiscard]] const std::string& failureReason() const noexcept;
        [[nodiscard]] std::size_t queuedCount() const noexcept;
        [[nodiscard]] std::size_t queuedCommandBytes() const noexcept;
        [[nodiscard]] std::size_t pendingResponseCount() const noexcept;
        [[nodiscard]] std::size_t pendingSyncCount() const noexcept;
        [[nodiscard]] NewStage newStage() const noexcept;

    private:
        struct QueuedEntry {
            std::variant<RemoteCommand, NewCommand> command;
            std::size_t retainedInputBytes = 0;
        };

        struct ActiveNewWorkflow {
            std::uint64_t token = 0;
            NewStage stage = NewStage::None;
            std::string prompt;
            std::optional<ai::openai::codex::typed::ThreadId> createdThreadId;
        };

        enum class SubmissionDisposition { Accepted, Deferred, Rejected, ConnectionFailed, ApplicationFatal, IgnoredDuringShutdown };

        struct SubmissionAttempt {
            SubmissionDisposition disposition = SubmissionDisposition::Rejected;
            std::optional<ai::openai::codex::frontend::client::Error> error;
        };

        [[nodiscard]] SubmissionAttempt submit(const RemoteCommand& command);
        [[nodiscard]] SubmissionAttempt submitNew(const NewCommand& command);
        [[nodiscard]] SubmissionAttempt submitActiveNewTurn();
        [[nodiscard]] SubmissionAttempt
        classifySubmission(const ai::openai::codex::frontend::client::Submission& submission) const noexcept;
        [[nodiscard]] bool queue(QueuedEntry entry);
        void handleRejectedSubmission(const SubmissionAttempt& attempt);
        void restoreFront(QueuedEntry entry);
        void releaseQueuedAccounting(const QueuedEntry& entry);
        void clearQueued(std::string_view reason);
        void flushQueued();
        void operationCompleted(bool succeeded, const std::optional<ai::openai::codex::frontend::client::Error>& error);
        void synchronizationCompleted(
            const ai::openai::codex::frontend::client::OperationResult<ai::openai::codex::frontend::client::SynchronizationResult>& result);
        void threadStartCompleted(
            std::uint64_t token,
            const ai::openai::codex::frontend::client::OperationResult<ai::openai::codex::frontend::client::ThreadStartResult>& result);
        void turnStartCompleted(
            std::uint64_t token,
            const ai::openai::codex::frontend::client::OperationResult<ai::openai::codex::frontend::client::TurnStartResult>& result);
        void completeActiveNewFailure(std::uint64_t token);
        void recordCommandFailure() noexcept;
        void reportLocalCommandError(std::string message);
        void recordConnectionFailure(std::string message);
        void markDisconnected();
        void maybeCompleteDrain();
        void finish(Outcome outcome);
        void terminateApplicationFailure(std::string message);

        ai::openai::codex::frontend::client::Client& sdk;
        CommandDrainCallbacks callbacks;
        CommandQueueLimits queueLimits;
        SessionState currentSessionState = SessionState::Connecting;
        InputState currentInputState = InputState::Reading;
        Outcome currentOutcome = Outcome::Running;
        std::deque<QueuedEntry> queuedMessages;
        std::optional<ActiveNewWorkflow> activeNew;
        std::string currentFailureReason;
        std::string currentConnectionFailure;
        std::size_t queuedBytes = 0;
        std::uint64_t nextNewToken = 1;
        bool activeExplicitSynchronization = false;
        bool encounteredWorkFailure = false;
        bool intentionalLocalShutdown = false;
        bool disconnectHandled = false;
        bool flushingQueue = false;
    };

} // namespace apps::codex_backend_client

#endif // APPS_CODEX_BACKEND_CLIENT_COMMANDDRAINCONTROLLER_H
