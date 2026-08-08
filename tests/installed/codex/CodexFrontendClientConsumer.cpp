/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include <ai/openai/codex/frontend/client/Client.h>
#include <ai/openai/codex/frontend/client/Connection.h>
#include <ai/openai/codex/frontend/client/Controller.h>
#include <ai/openai/codex/frontend/client/GeneratedBindings.h>
#include <ai/openai/codex/frontend/client/Provider.h>
#include <ai/openai/codex/frontend/client/Requests.h>
#include <ai/openai/codex/frontend/client/State.h>
#include <ai/openai/codex/frontend/client/Synchronization.h>
#include <ai/openai/codex/frontend/client/Threads.h>
#include <ai/openai/codex/frontend/client/Turns.h>
#include <ai/openai/codex/typed/Threads.h>
#include <ai/openai/codex/typed/Turns.h>
#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace {
    namespace client = ai::openai::codex::frontend::client;
    namespace frontend = ai::openai::codex::frontend;
    namespace typed = ai::openai::codex::typed;

    [[nodiscard]] bool isNotReady(const client::Submission& submission) {
        return !submission && submission.error && submission.error->clientCode == client::ClientErrorCode::NotReady;
    }

    [[nodiscard]] bool exerciseTypedState(const client::State& state) {
        std::size_t located = 0;
        std::size_t expected = 0;
        if (state.sessions().value) {
            expected += state.sessions().value->size();
            for (const client::SessionState& session : *state.sessions().value) {
                located += state.session(session.sessionId) != nullptr ? 1U : 0U;
            }
        }
        for (const client::ThreadState& thread : state.threads()) {
            located += state.thread(thread.id) != nullptr ? 1U : 0U;
        }
        expected += state.threads().size();
        for (const client::TurnState& turn : state.turns()) {
            located += state.turn(turn.id) != nullptr ? 1U : 0U;
        }
        expected += state.turns().size();
        for (const client::ItemState& item : state.items()) {
            located += state.item(item.id) != nullptr ? 1U : 0U;
        }
        expected += state.items().size();
        if (state.processes().value) {
            expected += state.processes().value->entries.size();
            for (const client::ProcessState& process : state.processes().value->entries) {
                located += state.process(process.processHandle) != nullptr ? 1U : 0U;
            }
        }
        return located == expected;
    }
} // namespace

int main() {
    static_assert(!std::is_copy_constructible_v<client::Client>);
    static_assert(!std::is_move_constructible_v<client::Client>);
    static_assert(std::is_move_constructible_v<client::Connection>);
    static_assert(std::is_copy_constructible_v<client::State>);
    static_assert(requires(const client::State& state, const client::FrontendSessionId& sessionId) {
        { state.session(sessionId) } -> std::same_as<const client::SessionState*>;
    });
    static_assert(requires(const client::State& state, const client::ProcessHandle& processHandle) {
        { state.process(processHandle) } -> std::same_as<const client::ProcessState*>;
    });
    static_assert(
        std::is_same_v<client::generated::BindingTraits<frontend::generated::MethodId::ThreadStart>::Parameter, typed::ThreadStartParams>);
    static_assert(
        std::is_same_v<client::generated::BindingTraits<frontend::generated::MethodId::TurnStart>::Parameter, typed::TurnStartParams>);
    static_assert(std::is_same_v<client::generated::BindingTraits<frontend::generated::MethodId::UserInputRespond>::Parameter,
                                 client::UserInputRespondParams>);

    client::ClientOptions options;
    options.credentialProvider = [] {
        return client::AuthenticationContext{
            .credential = ai::openai::codex::frontend::NoCredential{},
            .continuityKey = "installed-consumer",
        };
    };
    client::Client sdk(std::move(options));
    const client::State state = sdk.state();
    const client::BackendCursorState& backendCursor = state.backendCursor();
    const client::ProjectionMetadataState& projectionMetadata = state.projectionMetadata();
    const std::optional<client::ProjectionFingerprintMetadata>& projectionFingerprint = state.projectionFingerprintMetadata();

    const client::Submission threadStart =
        sdk.threads().start(typed::ThreadStartParams{}, [](const client::OperationResult<client::ThreadStartResult>&) {
        });

    typed::TurnStartParams turnParameters;
    turnParameters.threadId = typed::ThreadId{"installed-consumer-thread"};
    turnParameters.input.emplace_back(typed::TextInput{.text = "installed consumer turn"});
    const client::Submission turnStart =
        sdk.turns().start(std::move(turnParameters), [](const client::OperationResult<client::TurnStartResult>&) {
        });

    const client::Submission controllerAcquire = sdk.controller().acquire([](const client::OperationResult<client::ControllerResult>&) {
    });
    const client::Submission providerStart = sdk.provider().start([](const client::OperationResult<typed::Unit>&) {
    });

    client::UserInputRespondParams reverseParameters;
    reverseParameters.pendingRequestId = client::PendingRequestId{"1"};
    reverseParameters.answers.push_back(
        typed::UserInputAnswer{.questionId = "installed-consumer-question", .answers = {"installed-consumer-answer"}});
    const client::Submission reverseResponse =
        sdk.requests().respond(std::move(reverseParameters), [](const client::OperationResult<typed::Unit>&) {
        });

    const client::Submission replay =
        sdk.synchronization().replay(frontend::SequenceNumber{0}, [](const client::OperationResult<client::SynchronizationResult>&) {
        });
    const client::CapabilityStatus capability = sdk.capabilityStatus(frontend::FrontendCapability::CppClientSdk);

    const bool typedOperationsRejectBeforeReady = isNotReady(threadStart) && isNotReady(turnStart) && isNotReady(controllerAcquire) &&
                                                  isNotReady(providerStart) && isNotReady(reverseResponse) && isNotReady(replay);
    const bool initialStateIsTypedAndEmpty = state.revision() == 0 && exerciseTypedState(state) && !backendCursor.currentSequence &&
                                             projectionMetadata.omittedFields.empty() && projectionMetadata.redactedFields.empty() &&
                                             !projectionFingerprint;
    const bool capabilityMetadataIsUsable =
        capability.capability == frontend::FrontendCapability::CppClientSdk && capability.defined == client::Availability::Yes;

    return sdk.connectionState() == client::ConnectionState::Disconnected && typedOperationsRejectBeforeReady &&
                   initialStateIsTypedAndEmpty && capabilityMetadataIsUsable
               ? 0
               : 1;
}
