/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/CommandDrainController.h"

#include "ai/openai/codex/frontend/client/Controller.h"
#include "ai/openai/codex/frontend/client/Synchronization.h"
#include "ai/openai/codex/frontend/client/Threads.h"
#include "ai/openai/codex/frontend/client/Turns.h"

#include <cstddef>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace apps::codex_backend_client {
    namespace frontend = ai::openai::codex::frontend;
    namespace sdk_client = ai::openai::codex::frontend::client;
    namespace typed = ai::openai::codex::typed;

    namespace {

        std::string_view errorOriginName(sdk_client::ErrorOrigin origin) noexcept {
            switch (origin) {
                case sdk_client::ErrorOrigin::Client:
                    return "client";
                case sdk_client::ErrorOrigin::Transport:
                    return "transport";
                case sdk_client::ErrorOrigin::Protocol:
                    return "protocol";
                case sdk_client::ErrorOrigin::Command:
                    return "command";
            }
            return "unknown";
        }

        std::string sdkFailureMessage(const sdk_client::Error& error) {
            std::string message = "frontend SDK ";
            message += errorOriginName(error.origin);
            message += " failure: ";
            if (error.origin == sdk_client::ErrorOrigin::Protocol && !error.clientCode && error.protocolCode) {
                message += "server reported ";
                message += frontend::toString(*error.protocolCode);
            } else {
                message += error.message.empty() ? "unspecified frontend client error" : error.message;
            }
            return message;
        }

        std::string safeFailureMessage(std::string_view message) {
            constexpr std::size_t MaximumReasonBytes = 240;
            if (message.empty()) {
                return "unspecified frontend client failure";
            }
            std::string safe;
            safe.reserve(message.size() < MaximumReasonBytes ? message.size() : MaximumReasonBytes + 3U);
            for (const unsigned char character : message.substr(0, MaximumReasonBytes)) {
                safe.push_back(character >= 0x20U && character < 0x7fU ? static_cast<char>(character) : ' ');
            }
            if (message.size() > MaximumReasonBytes) {
                safe += "...";
            }
            return safe;
        }

        sdk_client::Error localError(sdk_client::ClientErrorCode code, std::string message) {
            return {sdk_client::ErrorOrigin::Client, code, std::nullopt, std::move(message), std::nullopt, std::nullopt, false};
        }

    } // namespace

    CommandDrainController::CommandDrainController(sdk_client::Client& sdk, CommandDrainCallbacks callbacks, CommandQueueLimits queueLimits)
        : sdk(sdk)
        , callbacks(std::move(callbacks))
        , queueLimits(queueLimits) {
    }

    bool CommandDrainController::enqueue(RemoteCommand command, std::size_t retainedInputBytes) {
        if (currentOutcome != Outcome::Running || currentInputState != InputState::Reading) {
            return false;
        }
        if (currentSessionState == SessionState::Disconnected) {
            reportLocalCommandError("frontend is disconnected; enter 'reconnect'");
            return false;
        }
        if (currentSessionState == SessionState::ShuttingDown || currentSessionState == SessionState::Closed) {
            return false;
        }
        if (currentSessionState != SessionState::Ready || !queuedMessages.empty() || activeNew || activeExplicitSynchronization) {
            return queue(QueuedEntry{std::move(command), retainedInputBytes});
        }

        const SubmissionAttempt attempt = submit(command);
        if (attempt.disposition == SubmissionDisposition::Accepted) {
            return true;
        }
        if (attempt.disposition == SubmissionDisposition::Deferred) {
            return queue(QueuedEntry{std::move(command), retainedInputBytes});
        }
        handleRejectedSubmission(attempt);
        return false;
    }

    bool CommandDrainController::enqueue(NewCommand command, std::size_t retainedInputBytes) {
        if (currentOutcome != Outcome::Running || currentInputState != InputState::Reading) {
            return false;
        }
        if (currentSessionState == SessionState::Disconnected) {
            reportLocalCommandError("frontend is disconnected; enter 'reconnect'");
            return false;
        }
        if (currentSessionState == SessionState::ShuttingDown || currentSessionState == SessionState::Closed) {
            return false;
        }
        if (currentSessionState != SessionState::Ready || !queuedMessages.empty() || activeNew || activeExplicitSynchronization) {
            return queue(QueuedEntry{std::move(command), retainedInputBytes});
        }

        const SubmissionAttempt attempt = submitNew(command);
        if (attempt.disposition == SubmissionDisposition::Accepted) {
            return true;
        }
        if (attempt.disposition == SubmissionDisposition::Deferred) {
            return queue(QueuedEntry{std::move(command), retainedInputBytes});
        }
        handleRejectedSubmission(attempt);
        return false;
    }

    void CommandDrainController::localCommandFailed(std::string message) {
        if (currentOutcome == Outcome::Running && currentInputState != InputState::ImmediateQuit) {
            reportLocalCommandError(std::move(message));
            maybeCompleteDrain();
        }
    }

    bool CommandDrainController::reconnect() {
        if (currentOutcome != Outcome::Running || currentInputState != InputState::Reading || intentionalLocalShutdown) {
            return false;
        }
        switch (currentSessionState) {
            case SessionState::Disconnected:
                break;
            case SessionState::Connecting:
                reportLocalCommandError("a frontend connection attempt is already active");
                return false;
            case SessionState::Synchronizing:
                reportLocalCommandError("the frontend connection is already synchronizing");
                return false;
            case SessionState::Ready:
                reportLocalCommandError("the frontend is already connected");
                return false;
            case SessionState::ShuttingDown:
            case SessionState::Closed:
                return false;
        }
        if (sdk.hasActiveConnection()) {
            reportLocalCommandError("the previous frontend connection is still closing");
            return false;
        }

        currentSessionState = SessionState::Connecting;
        currentConnectionFailure.clear();
        disconnectHandled = false;
        try {
            if (!callbacks.requestReconnect) {
                terminateApplicationFailure("frontend reconnect callback is unavailable");
                return false;
            }
            if (std::optional<std::string> rejection = callbacks.requestReconnect()) {
                // The physical adapter rejected this attempt before accepting
                // any transport work (for example, an older socket is still
                // detaching). Restore the prior application state without
                // synthesizing a remote-disconnect failure or clearing work a
                // second time.
                currentSessionState = SessionState::Disconnected;
                disconnectHandled = true;
                reportLocalCommandError(std::move(*rejection));
                return false;
            }
            return currentOutcome == Outcome::Running;
        } catch (...) {
            connectionAttemptFailed("frontend reconnect callback failed");
            return false;
        }
    }

    void CommandDrainController::connectionAttemptFailed(std::string message) {
        if (currentOutcome != Outcome::Running || intentionalLocalShutdown) {
            return;
        }
        recordConnectionFailure(std::move(message));
        markDisconnected();
    }

    void CommandDrainController::connectionStateChanged(sdk_client::ConnectionState state) {
        if (intentionalLocalShutdown) {
            if (state == sdk_client::ConnectionState::Closed || state == sdk_client::ConnectionState::Disconnected) {
                currentSessionState = SessionState::Closed;
            }
            return;
        }
        switch (state) {
            case sdk_client::ConnectionState::Connecting:
            case sdk_client::ConnectionState::Authenticating:
                currentSessionState = SessionState::Connecting;
                disconnectHandled = false;
                break;
            case sdk_client::ConnectionState::Synchronizing:
                currentSessionState = SessionState::Synchronizing;
                break;
            case sdk_client::ConnectionState::Ready:
                currentSessionState = SessionState::Ready;
                currentConnectionFailure.clear();
                disconnectHandled = false;
                flushQueued();
                break;
            case sdk_client::ConnectionState::Closing:
                // The SDK is closing one physical connection. Mark the
                // application disconnected before pending-operation callbacks
                // run so none of them can flush commands into that connection.
                markDisconnected();
                break;
            case sdk_client::ConnectionState::Disconnected:
                markDisconnected();
                break;
            case sdk_client::ConnectionState::Closed:
                currentSessionState = SessionState::Closed;
                terminateApplicationFailure("frontend SDK Client closed outside application shutdown");
                break;
        }
    }

    void CommandDrainController::connectionStateChanged(const sdk_client::ConnectionStateChange& change) {
        if (change.error && !intentionalLocalShutdown && currentOutcome == Outcome::Running) {
            recordConnectionFailure(sdkFailureMessage(*change.error));
        }
        connectionStateChanged(change.current);
    }

    void CommandDrainController::localShutdownRequested() noexcept {
        if (currentOutcome != Outcome::Running) {
            return;
        }
        intentionalLocalShutdown = true;
        currentInputState = InputState::ImmediateQuit;
        currentSessionState = SessionState::ShuttingDown;
        queuedMessages.clear();
        queuedBytes = 0;
        activeNew.reset();
        activeExplicitSynchronization = false;
        finish(Outcome::Success);
    }

    CommandDrainController::SubmissionAttempt CommandDrainController::submit(const RemoteCommand& command) {
        const auto operationHandler = [this](const auto& result) {
            operationCompleted(static_cast<bool>(result), result.error);
        };
        const auto synchronizationHandler = [this](const sdk_client::OperationResult<sdk_client::SynchronizationResult>& result) {
            synchronizationCompleted(result);
        };

        const sdk_client::Submission submission = std::visit(
            [this, &operationHandler, &synchronizationHandler]<typename Command>(const Command& typedCommand) -> sdk_client::Submission {
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
                    return sdk.threads().list(typedCommand.parameters, operationHandler);
                } else if constexpr (std::is_same_v<T, ThreadStartCommand>) {
                    return sdk.threads().start(typedCommand.parameters, operationHandler);
                } else if constexpr (std::is_same_v<T, ThreadResumeCommand>) {
                    return sdk.threads().resume(typedCommand.parameters, operationHandler);
                } else if constexpr (std::is_same_v<T, ThreadReadCommand>) {
                    return sdk.threads().read(typedCommand.parameters, operationHandler);
                } else if constexpr (std::is_same_v<T, TurnStartCommand>) {
                    return sdk.turns().start(typedCommand.parameters, operationHandler);
                } else if constexpr (std::is_same_v<T, TurnInterruptCommand>) {
                    return sdk.turns().interrupt(typedCommand.parameters, operationHandler);
                } else {
                    static_assert(std::is_same_v<T, RawCommand>);
                    return sdk.submit(typedCommand.parameters, operationHandler);
                }
            },
            command);
        SubmissionAttempt attempt = classifySubmission(submission);
        if (attempt.disposition == SubmissionDisposition::Accepted &&
            (std::holds_alternative<SnapshotCommand>(command) || std::holds_alternative<ReplayCommand>(command))) {
            activeExplicitSynchronization = true;
        }
        return attempt;
    }

    CommandDrainController::SubmissionAttempt CommandDrainController::submitNew(const NewCommand& command) {
        if (nextNewToken == std::numeric_limits<std::uint64_t>::max()) {
            return {SubmissionDisposition::Rejected,
                    localError(sdk_client::ClientErrorCode::RequestIdExhausted, "frontend compound-workflow identifiers exhausted")};
        }
        const std::uint64_t token = nextNewToken++;
        activeNew = ActiveNewWorkflow{token, NewStage::AwaitingThreadStartResponse, command.prompt, std::nullopt};
        const sdk_client::Submission submission =
            sdk.threads().start(command.options, [this, token](const sdk_client::OperationResult<sdk_client::ThreadStartResult>& result) {
                threadStartCompleted(token, result);
            });
        SubmissionAttempt attempt = classifySubmission(submission);
        if (attempt.disposition != SubmissionDisposition::Accepted) {
            activeNew.reset();
        }
        return attempt;
    }

    CommandDrainController::SubmissionAttempt CommandDrainController::submitActiveNewTurn() {
        if (!activeNew || activeNew->stage != NewStage::WaitingToSubmitTurn || !activeNew->createdThreadId) {
            return {SubmissionDisposition::Rejected,
                    localError(sdk_client::ClientErrorCode::InvalidConfiguration, "frontend new workflow has no created thread")};
        }

        typed::TurnStartParams turn;
        turn.threadId = *activeNew->createdThreadId;
        typed::TextInput input;
        input.text = activeNew->prompt;
        turn.input.emplace_back(std::move(input));
        const std::uint64_t token = activeNew->token;
        const sdk_client::Submission submission =
            sdk.turns().start(std::move(turn), [this, token](const sdk_client::OperationResult<sdk_client::TurnStartResult>& result) {
                turnStartCompleted(token, result);
            });
        SubmissionAttempt attempt = classifySubmission(submission);
        if (attempt.disposition == SubmissionDisposition::Accepted && activeNew && activeNew->token == token) {
            activeNew->stage = NewStage::AwaitingTurnStartResponse;
        }
        return attempt;
    }

    CommandDrainController::SubmissionAttempt
    CommandDrainController::classifySubmission(const sdk_client::Submission& submission) const noexcept {
        if (submission) {
            return {SubmissionDisposition::Accepted, std::nullopt};
        }
        if (intentionalLocalShutdown || currentOutcome != Outcome::Running) {
            return {SubmissionDisposition::IgnoredDuringShutdown, submission.error};
        }
        if (!submission.error || !submission.error->clientCode) {
            return {SubmissionDisposition::Rejected, submission.error};
        }
        switch (*submission.error->clientCode) {
            case sdk_client::ClientErrorCode::TooManyPendingOperations:
            case sdk_client::ClientErrorCode::SynchronizationAlreadyActive:
                return {SubmissionDisposition::Deferred, submission.error};
            case sdk_client::ClientErrorCode::NotReady:
            case sdk_client::ClientErrorCode::NotConnected:
                if (currentSessionState == SessionState::Connecting || currentSessionState == SessionState::Synchronizing) {
                    return {SubmissionDisposition::Deferred, submission.error};
                }
                return {SubmissionDisposition::Rejected, submission.error};
            case sdk_client::ClientErrorCode::SendRejected:
            case sdk_client::ClientErrorCode::TransportFailure:
                return {SubmissionDisposition::ConnectionFailed, submission.error};
            case sdk_client::ClientErrorCode::Closed:
                return {SubmissionDisposition::ApplicationFatal, submission.error};
            default:
                return {SubmissionDisposition::Rejected, submission.error};
        }
    }

    bool CommandDrainController::queue(QueuedEntry entry) {
        if (queueLimits.maximumCommands == 0 || queueLimits.maximumCommandBytes == 0 ||
            queuedMessages.size() >= queueLimits.maximumCommands) {
            reportLocalCommandError("frontend command queue capacity exhausted");
            return false;
        }
        if (entry.retainedInputBytes > queueLimits.maximumCommandBytes ||
            queuedBytes > queueLimits.maximumCommandBytes - entry.retainedInputBytes) {
            reportLocalCommandError("frontend command queue byte capacity exhausted");
            return false;
        }
        queuedMessages.push_back(std::move(entry));
        queuedBytes += queuedMessages.back().retainedInputBytes;
        return true;
    }

    void CommandDrainController::handleRejectedSubmission(const SubmissionAttempt& attempt) {
        const std::string message =
            attempt.error && !attempt.error->message.empty() ? attempt.error->message : "frontend SDK rejected the command";
        switch (attempt.disposition) {
            case SubmissionDisposition::Rejected:
                reportLocalCommandError(message);
                break;
            case SubmissionDisposition::ConnectionFailed:
                recordCommandFailure();
                recordConnectionFailure("frontend SDK transport failure: " + message);
                if (sdk.connectionState() == sdk_client::ConnectionState::Disconnected) {
                    markDisconnected();
                }
                break;
            case SubmissionDisposition::ApplicationFatal:
                terminateApplicationFailure("frontend SDK Client is unusable: " + message);
                break;
            case SubmissionDisposition::IgnoredDuringShutdown:
            case SubmissionDisposition::Accepted:
            case SubmissionDisposition::Deferred:
                break;
        }
    }

    void CommandDrainController::restoreFront(QueuedEntry entry) {
        // The entry was admitted previously and was removed immediately before
        // this restoration. Re-adding it cannot exceed either configured bound.
        queuedBytes += entry.retainedInputBytes;
        queuedMessages.push_front(std::move(entry));
    }

    void CommandDrainController::releaseQueuedAccounting(const QueuedEntry& entry) {
        if (entry.retainedInputBytes > queuedBytes) {
            queuedBytes = 0;
            terminateApplicationFailure("frontend command queue accounting underflow");
            return;
        }
        queuedBytes -= entry.retainedInputBytes;
    }

    void CommandDrainController::clearQueued(std::string_view reason) {
        const std::size_t discarded = queuedMessages.size();
        queuedMessages.clear();
        queuedBytes = 0;
        if (discarded == 0) {
            return;
        }
        recordCommandFailure();
        reportLocalCommandError("discarded " + std::to_string(discarded) + " queued command" + (discarded == 1 ? "" : "s") + " " +
                                std::string(reason));
    }

    void CommandDrainController::flushQueued() {
        if (flushingQueue || currentOutcome != Outcome::Running || currentSessionState != SessionState::Ready ||
            activeExplicitSynchronization) {
            maybeCompleteDrain();
            return;
        }
        flushingQueue = true;
        while (currentOutcome == Outcome::Running && currentSessionState == SessionState::Ready && !activeExplicitSynchronization) {
            if (activeNew) {
                if (activeNew->stage != NewStage::WaitingToSubmitTurn) {
                    break;
                }
                const std::uint64_t token = activeNew->token;
                const SubmissionAttempt attempt = submitActiveNewTurn();
                if (attempt.disposition == SubmissionDisposition::Accepted || attempt.disposition == SubmissionDisposition::Deferred) {
                    break;
                }
                if (activeNew && activeNew->token == token) {
                    activeNew.reset();
                    handleRejectedSubmission(attempt);
                }
                continue;
            }
            if (queuedMessages.empty()) {
                break;
            }

            QueuedEntry entry = std::move(queuedMessages.front());
            queuedMessages.pop_front();
            releaseQueuedAccounting(entry);
            if (currentOutcome != Outcome::Running) {
                break;
            }
            const SubmissionAttempt attempt = std::visit(
                [this](const auto& command) {
                    using T = std::remove_cvref_t<decltype(command)>;
                    if constexpr (std::is_same_v<T, RemoteCommand>) {
                        return submit(command);
                    } else {
                        return submitNew(command);
                    }
                },
                entry.command);
            if (attempt.disposition == SubmissionDisposition::Accepted) {
                continue;
            }
            if (attempt.disposition == SubmissionDisposition::Deferred) {
                restoreFront(std::move(entry));
                break;
            }
            handleRejectedSubmission(attempt);
        }
        flushingQueue = false;
        maybeCompleteDrain();
    }

    void CommandDrainController::operationCompleted(bool succeeded, const std::optional<sdk_client::Error>&) {
        if (currentOutcome != Outcome::Running || intentionalLocalShutdown) {
            return;
        }
        if (!succeeded) {
            // Accepted command failures are already observable as Response or
            // ProtocolError messages, or as the connection's first causal
            // failure. Do not print a second generic copy here.
            recordCommandFailure();
        }
        if (sdk.connectionState() == sdk_client::ConnectionState::Ready && currentSessionState == SessionState::Ready) {
            flushQueued();
        }
        maybeCompleteDrain();
    }

    void CommandDrainController::synchronizationCompleted(const sdk_client::OperationResult<sdk_client::SynchronizationResult>& result) {
        if (currentOutcome != Outcome::Running || intentionalLocalShutdown) {
            return;
        }
        activeExplicitSynchronization = false;
        if (!result) {
            recordCommandFailure();
        }
        if (sdk.connectionState() == sdk_client::ConnectionState::Ready && currentSessionState == SessionState::Ready) {
            flushQueued();
        }
        maybeCompleteDrain();
    }

    void CommandDrainController::threadStartCompleted(std::uint64_t token,
                                                      const sdk_client::OperationResult<sdk_client::ThreadStartResult>& result) {
        if (!activeNew || activeNew->token != token || currentOutcome != Outcome::Running || intentionalLocalShutdown) {
            // A connection-level failure clears the application workflow before
            // the SDK completes its accepted operation. The completion still
            // releases pending capacity and may finish an EOF drain.
            maybeCompleteDrain();
            return;
        }
        if (!result || !result.value) {
            completeActiveNewFailure(token);
            return;
        }
        activeNew->createdThreadId = result.value->threadId;
        activeNew->stage = NewStage::WaitingToSubmitTurn;
        const SubmissionAttempt attempt = submitActiveNewTurn();
        if (attempt.disposition == SubmissionDisposition::Accepted || attempt.disposition == SubmissionDisposition::Deferred) {
            return;
        }
        if (activeNew && activeNew->token == token) {
            activeNew.reset();
            handleRejectedSubmission(attempt);
            flushQueued();
            maybeCompleteDrain();
        }
    }

    void CommandDrainController::turnStartCompleted(std::uint64_t token,
                                                    const sdk_client::OperationResult<sdk_client::TurnStartResult>& result) {
        if (!activeNew || activeNew->token != token || currentOutcome != Outcome::Running || intentionalLocalShutdown) {
            maybeCompleteDrain();
            return;
        }
        if (!result) {
            completeActiveNewFailure(token);
            return;
        }
        activeNew.reset();
        flushQueued();
        maybeCompleteDrain();
    }

    void CommandDrainController::completeActiveNewFailure(std::uint64_t token) {
        if (!activeNew || activeNew->token != token) {
            return;
        }
        recordCommandFailure();
        activeNew.reset();
        if (sdk.connectionState() == sdk_client::ConnectionState::Ready && currentSessionState == SessionState::Ready) {
            flushQueued();
        }
        maybeCompleteDrain();
    }

    void CommandDrainController::recordCommandFailure() noexcept {
        encounteredWorkFailure = true;
    }

    void CommandDrainController::reportLocalCommandError(std::string message) {
        recordCommandFailure();
        const std::string safe = safeFailureMessage(message);
        if (currentFailureReason.empty()) {
            currentFailureReason = safe;
        }
        if (callbacks.reportFailure) {
            callbacks.reportFailure(safe);
        }
    }

    void CommandDrainController::recordConnectionFailure(std::string message) {
        if (currentOutcome != Outcome::Running || intentionalLocalShutdown || !currentConnectionFailure.empty()) {
            return;
        }
        encounteredWorkFailure = true;
        currentConnectionFailure = safeFailureMessage(message);
        if (currentFailureReason.empty()) {
            currentFailureReason = currentConnectionFailure;
        }
        if (callbacks.reportFailure) {
            callbacks.reportFailure(currentConnectionFailure);
        }
    }

    void CommandDrainController::markDisconnected() {
        if (intentionalLocalShutdown) {
            currentSessionState = SessionState::Closed;
            return;
        }
        if (disconnectHandled) {
            // A later SDK/physical disconnect notification can arrive after
            // pending callbacks released the final EOF-drain operation.
            maybeCompleteDrain();
            return;
        }
        disconnectHandled = true;
        currentSessionState = SessionState::Disconnected;
        activeExplicitSynchronization = false;
        if (activeNew) {
            recordCommandFailure();
            activeNew.reset();
        }
        if (currentConnectionFailure.empty()) {
            recordConnectionFailure("frontend connection closed unexpectedly");
        }
        clearQueued("after frontend connection loss");
        maybeCompleteDrain();
    }

    void CommandDrainController::inputEof() {
        if (currentOutcome == Outcome::Running && currentInputState == InputState::Reading) {
            currentInputState = InputState::DrainOnEof;
            maybeCompleteDrain();
        }
    }

    void CommandDrainController::inputFailed(std::string message) {
        terminateApplicationFailure(std::move(message));
    }

    void CommandDrainController::startupFailed(std::string message) {
        terminateApplicationFailure(std::move(message));
    }

    void CommandDrainController::connectionFailed(std::string message) {
        if (!intentionalLocalShutdown) {
            recordConnectionFailure(std::move(message));
        }
    }

    void CommandDrainController::disconnected() {
        markDisconnected();
    }

    void CommandDrainController::quit() {
        localShutdownRequested();
    }

    void CommandDrainController::maybeCompleteDrain() {
        if (currentOutcome != Outcome::Running || currentInputState != InputState::DrainOnEof || !queuedMessages.empty() || activeNew ||
            activeExplicitSynchronization || sdk.pendingOperationCount() != 0) {
            return;
        }
        finish(encounteredWorkFailure ? Outcome::Failure : Outcome::Success);
    }

    void CommandDrainController::finish(Outcome value) {
        if (currentOutcome != Outcome::Running) {
            return;
        }
        currentOutcome = value;
        intentionalLocalShutdown = true;
        currentSessionState = SessionState::ShuttingDown;
        if (callbacks.requestExit) {
            callbacks.requestExit();
        }
    }

    void CommandDrainController::terminateApplicationFailure(std::string message) {
        if (currentOutcome != Outcome::Running) {
            return;
        }
        const std::string safe = safeFailureMessage(message);
        if (currentFailureReason.empty()) {
            currentFailureReason = safe;
        }
        if (callbacks.reportFailure) {
            callbacks.reportFailure(safe);
        }
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

    bool CommandDrainController::applicationShutdownActive() const noexcept {
        return intentionalLocalShutdown || currentSessionState == SessionState::ShuttingDown || currentSessionState == SessionState::Closed;
    }

    const std::string& CommandDrainController::failureReason() const noexcept {
        return currentFailureReason;
    }

    std::size_t CommandDrainController::queuedCount() const noexcept {
        return queuedMessages.size();
    }

    std::size_t CommandDrainController::queuedCommandBytes() const noexcept {
        return queuedBytes;
    }

    std::size_t CommandDrainController::pendingResponseCount() const noexcept {
        return sdk.pendingOperationCount();
    }

    std::size_t CommandDrainController::pendingSyncCount() const noexcept {
        return activeExplicitSynchronization ? 1U : 0U;
    }

    CommandDrainController::NewStage CommandDrainController::newStage() const noexcept {
        if (activeNew) {
            return activeNew->stage;
        }
        for (const QueuedEntry& entry : queuedMessages) {
            if (std::holds_alternative<NewCommand>(entry.command)) {
                return NewStage::Queued;
            }
        }
        return NewStage::None;
    }

} // namespace apps::codex_backend_client
