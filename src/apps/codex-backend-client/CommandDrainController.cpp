/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/CommandDrainController.h"

#include "ai/openai/codex/frontend/client/Controller.h"
#include "ai/openai/codex/frontend/client/Synchronization.h"
#include "ai/openai/codex/frontend/client/Threads.h"
#include "ai/openai/codex/frontend/client/Turns.h"

#include <type_traits>
#include <utility>

namespace apps::codex_backend_client {
    namespace frontend = ai::openai::codex::frontend;
    namespace sdk_client = ai::openai::codex::frontend::client;
    namespace typed = ai::openai::codex::typed;

    CommandDrainController::CommandDrainController(sdk_client::Client& sdk, CommandDrainCallbacks callbacks)
        : sdk(sdk)
        , callbacks(std::move(callbacks)) {
    }

    bool CommandDrainController::enqueue(RemoteCommand command) {
        if (currentOutcome != Outcome::Running || currentInputState != InputState::Reading) {
            return false;
        }
        if (currentSessionState != SessionState::Ready || !queuedMessages.empty() || currentNewStage != NewStage::None) {
            queuedMessages.emplace_back(std::move(command));
            return true;
        }
        return submit(std::move(command));
    }

    bool CommandDrainController::enqueue(NewCommand command) {
        if (currentOutcome != Outcome::Running || currentInputState != InputState::Reading) {
            return false;
        }
        queuedMessages.emplace_back(QueuedNewCommand{std::move(command.options), std::move(command.prompt)});
        currentNewStage = NewStage::Queued;
        if (currentSessionState == SessionState::Ready) {
            flushQueued();
        }
        return currentOutcome == Outcome::Running;
    }

    void CommandDrainController::connectionStateChanged(sdk_client::ConnectionState state) {
        switch (state) {
            case sdk_client::ConnectionState::Connecting:
            case sdk_client::ConnectionState::Authenticating:
                currentSessionState = SessionState::Connecting;
                break;
            case sdk_client::ConnectionState::Synchronizing:
                currentSessionState = SessionState::Synchronizing;
                break;
            case sdk_client::ConnectionState::Ready:
                currentSessionState = SessionState::Ready;
                flushQueued();
                break;
            case sdk_client::ConnectionState::Closing:
                currentSessionState = SessionState::Closing;
                break;
            case sdk_client::ConnectionState::Disconnected:
            case sdk_client::ConnectionState::Closed:
                disconnected();
                break;
        }
    }

    bool CommandDrainController::submit(RemoteCommand command) {
        const auto operationHandler = [this](const auto& result) {
            operationCompleted(static_cast<bool>(result), result.error);
        };
        const auto synchronizationHandler = [this](const sdk_client::OperationResult<sdk_client::SynchronizationResult>& result) {
            synchronizationCompleted(result);
        };

        const sdk_client::Submission submission = std::visit(
            [this, &operationHandler, &synchronizationHandler]<typename Command>(Command&& typedCommand) -> sdk_client::Submission {
                using T = std::remove_cvref_t<Command>;
                if constexpr (std::is_same_v<T, SnapshotCommand>) {
                    return sdk.synchronization().snapshot(synchronizationHandler);
                } else if constexpr (std::is_same_v<T, ReplayCommand>) {
                    return sdk.synchronization().replay(typedCommand.after, synchronizationHandler);
                } else if constexpr (std::is_same_v<T, ControllerAcquireCommand>) {
                    return sdk.controller().acquire(operationHandler);
                } else if constexpr (std::is_same_v<T, ControllerReleaseCommand>) {
                    return sdk.controller().release(operationHandler);
                } else if constexpr (std::is_same_v<T, ThreadListCommand>) {
                    return sdk.threads().list(std::move(typedCommand.parameters), operationHandler);
                } else if constexpr (std::is_same_v<T, ThreadStartCommand>) {
                    return sdk.threads().start(std::move(typedCommand.parameters), operationHandler);
                } else if constexpr (std::is_same_v<T, ThreadResumeCommand>) {
                    return sdk.threads().resume(std::move(typedCommand.parameters), operationHandler);
                } else if constexpr (std::is_same_v<T, ThreadReadCommand>) {
                    return sdk.threads().read(std::move(typedCommand.parameters), operationHandler);
                } else if constexpr (std::is_same_v<T, TurnStartCommand>) {
                    return sdk.turns().start(std::move(typedCommand.parameters), operationHandler);
                } else if constexpr (std::is_same_v<T, TurnInterruptCommand>) {
                    return sdk.turns().interrupt(std::move(typedCommand.parameters), operationHandler);
                } else {
                    static_assert(std::is_same_v<T, RawCommand>);
                    return sdk.submit(std::move(typedCommand.parameters), operationHandler);
                }
            },
            std::move(command));
        if (!submission) {
            fail(submission.error ? submission.error->message : "frontend SDK rejected the command");
            return false;
        }
        return true;
    }

    bool CommandDrainController::submitNew(QueuedNewCommand command) {
        currentNewStage = NewStage::AwaitingThreadStartResponse;
        const sdk_client::Submission submission = sdk.threads().start(
            std::move(command.options),
            [this, prompt = std::move(command.prompt)](const sdk_client::OperationResult<sdk_client::ThreadStartResult>& result) mutable {
                if (!result || !result.value) {
                    fail(result.error ? result.error->message : "new thread.start failed");
                    return;
                }
                currentNewStage = NewStage::WaitingToSubmitTurn;

                typed::TurnStartParams turn;
                turn.threadId = result.value->threadId;
                typed::TextInput input;
                input.text = std::move(prompt);
                turn.input.emplace_back(std::move(input));
                currentNewStage = NewStage::AwaitingTurnStartResponse;
                const sdk_client::Submission turnSubmission =
                    sdk.turns().start(std::move(turn), [this](const sdk_client::OperationResult<sdk_client::TurnStartResult>& turnResult) {
                        if (!turnResult) {
                            fail(turnResult.error ? turnResult.error->message : "new turn.start failed");
                            return;
                        }
                        currentNewStage = NewStage::None;
                        flushQueued();
                        maybeCompleteDrain();
                    });
                if (!turnSubmission) {
                    fail(turnSubmission.error ? turnSubmission.error->message : "frontend SDK rejected new turn.start");
                    return;
                }
            });
        if (!submission) {
            fail(submission.error ? submission.error->message : "frontend SDK rejected new thread.start");
            return false;
        }
        return true;
    }

    void CommandDrainController::flushQueued() {
        while (currentOutcome == Outcome::Running && currentSessionState == SessionState::Ready &&
               (currentNewStage == NewStage::None || currentNewStage == NewStage::Queued) && !queuedMessages.empty()) {
            QueuedEntry next = std::move(queuedMessages.front());
            queuedMessages.pop_front();
            if (auto* command = std::get_if<RemoteCommand>(&next)) {
                if (!submit(std::move(*command)))
                    return;
            } else if (!submitNew(std::move(std::get<QueuedNewCommand>(next)))) {
                return;
            }
        }
        maybeCompleteDrain();
    }

    void CommandDrainController::operationCompleted(bool succeeded, const std::optional<sdk_client::Error>& error) {
        if (!succeeded && currentInputState == InputState::DrainOnEof) {
            fail(error ? error->message : "frontend operation failed");
            return;
        }
        maybeCompleteDrain();
    }

    void CommandDrainController::synchronizationCompleted(const sdk_client::OperationResult<sdk_client::SynchronizationResult>& result) {
        if (!result) {
            fail(result.error ? result.error->message : "frontend synchronization failed");
            return;
        }
        maybeCompleteDrain();
    }

    void CommandDrainController::inputEof() {
        if (currentOutcome == Outcome::Running && currentInputState == InputState::Reading) {
            currentInputState = InputState::DrainOnEof;
            maybeCompleteDrain();
        }
    }
    void CommandDrainController::inputFailed(std::string message) {
        fail(std::move(message));
    }
    void CommandDrainController::connectionFailed(std::string message) {
        fail(std::move(message));
    }
    void CommandDrainController::disconnected() {
        if (currentSessionState == SessionState::Closed)
            return;
        currentSessionState = SessionState::Closed;
        if (currentOutcome == Outcome::Running)
            fail("frontend connection closed unexpectedly");
    }
    void CommandDrainController::quit() {
        if (currentOutcome == Outcome::Running) {
            currentInputState = InputState::ImmediateQuit;
            finish(Outcome::Success);
        }
    }

    void CommandDrainController::maybeCompleteDrain() {
        if (currentOutcome == Outcome::Running && currentInputState == InputState::DrainOnEof && queuedMessages.empty() &&
            sdk.pendingOperationCount() == 0 && currentNewStage == NewStage::None) {
            finish(Outcome::Success);
        }
    }
    void CommandDrainController::finish(Outcome value) {
        currentOutcome = value;
        currentSessionState = SessionState::Closing;
        if (callbacks.requestExit)
            callbacks.requestExit();
    }
    void CommandDrainController::fail(std::string message) {
        if (currentOutcome != Outcome::Running)
            return;
        currentFailureReason = std::move(message);
        if (callbacks.reportFailure)
            callbacks.reportFailure(currentFailureReason);
        finish(Outcome::Failure);
    }

    CommandDrainController::SessionState CommandDrainController::sessionState() const noexcept {
        return currentSessionState;
    }
    CommandDrainController::InputState CommandDrainController::inputState() const noexcept {
        return currentInputState;
    }
    CommandDrainController::Outcome CommandDrainController::outcome() const noexcept {
        return currentOutcome;
    }
    bool CommandDrainController::failed() const noexcept {
        return currentOutcome == Outcome::Failure;
    }
    const std::string& CommandDrainController::failureReason() const noexcept {
        return currentFailureReason;
    }
    std::size_t CommandDrainController::queuedCount() const noexcept {
        return queuedMessages.size();
    }
    std::size_t CommandDrainController::pendingResponseCount() const noexcept {
        return sdk.pendingOperationCount();
    }
    std::size_t CommandDrainController::pendingSyncCount() const noexcept {
        return currentSessionState == SessionState::Synchronizing ? 1U : 0U;
    }
    CommandDrainController::NewStage CommandDrainController::newStage() const noexcept {
        return currentNewStage;
    }

} // namespace apps::codex_backend_client
