/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/client/Accounts.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "ai/openai/codex/frontend/client/Connection.h"
#include "ai/openai/codex/frontend/client/Controller.h"
#include "ai/openai/codex/frontend/client/GeneratedBindings.h"
#include "ai/openai/codex/frontend/client/Requests.h"
#include "ai/openai/codex/frontend/client/State.h"
#include "ai/openai/codex/frontend/client/Threads.h"
#include "ai/openai/codex/frontend/client/Turns.h"
#include "ai/openai/codex/frontend/client/detail/OperationCodecs.h"
#include "ai/openai/codex/typed/Threads.h"
#include "support/TestResult.h"

#include <concepts>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace typed = ai::openai::codex::typed;
    namespace client = ai::openai::codex::frontend::client;

#define AISUITE_CODEX_CLIENT_FACADE_LIST(X)                                                                                                \
    X(Controller, controller)                                                                                                              \
    X(Provider, provider)                                                                                                                  \
    X(Synchronization, synchronization)                                                                                                    \
    X(Accounts, accounts)                                                                                                                  \
    X(Apps, apps)                                                                                                                          \
    X(Commands, commands)                                                                                                                  \
    X(Configuration, configuration)                                                                                                        \
    X(ExternalAgents, externalAgents)                                                                                                      \
    X(Feedback, feedback)                                                                                                                  \
    X(Filesystem, filesystem)                                                                                                              \
    X(Hooks, hooks)                                                                                                                        \
    X(Marketplace, marketplace)                                                                                                            \
    X(Mcp, mcp)                                                                                                                            \
    X(Models, models)                                                                                                                      \
    X(PermissionProfiles, permissionProfiles)                                                                                              \
    X(Plugins, plugins)                                                                                                                    \
    X(Requests, requests)                                                                                                                  \
    X(Reviews, reviews)                                                                                                                    \
    X(Skills, skills)                                                                                                                      \
    X(Threads, threads)                                                                                                                    \
    X(Turns, turns)                                                                                                                        \
    X(WindowsSandbox, windowsSandbox)

#define AISUITE_CODEX_ASSERT_FACADE_LAYOUT(Type, accessor)                                                                                 \
    static_assert(sizeof(client::Type) == sizeof(void*));                                                                                  \
    static_assert(!std::is_polymorphic_v<client::Type>);                                                                                   \
    static_assert(!std::is_copy_constructible_v<client::Type>);                                                                            \
    static_assert(!std::is_move_constructible_v<client::Type>);
    AISUITE_CODEX_CLIENT_FACADE_LIST(AISUITE_CODEX_ASSERT_FACADE_LAYOUT)
#undef AISUITE_CODEX_ASSERT_FACADE_LAYOUT

    struct Harness {
        std::vector<client::OutboundMessage> outbound;
        std::size_t closes = 0;

        client::TransportCallbacks transport() {
            return {[this](client::OutboundMessage message) {
                        outbound.push_back(std::move(message));
                        return client::SendResult{client::SendStatus::Accepted, std::nullopt};
                    },
                    [this](std::string) {
                        ++closes;
                    }};
        }
    };

    client::ClientOptions options() {
        client::ClientOptions result;
        result.credentialProvider = [] {
            return client::AuthenticationContext{frontend::NoCredential{}, "verified-local:1000"};
        };
        return result;
    }

    frontend::CapabilityAdvertisement capabilities() {
        const std::vector selected{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
        };
        return {selected, selected, selected, frontend::Json::object()};
    }

    std::vector<frontend::FrontendMethod> advertisedMethods() {
        std::vector<frontend::FrontendMethod> result;
        result.reserve(frontend::generated::AllMethods.size());
        for (const auto& method : frontend::generated::AllMethods)
            result.emplace_back(method.method);
        return result;
    }

    frontend::Welcome modernWelcome(std::string sessionId, frontend::SequenceNumber sequence, frontend::SyncMode mode) {
        const std::vector<frontend::FrontendMethod> methods = advertisedMethods();
        return frontend::Welcome{std::move(sessionId),
                                 frontend::SessionRole::Observer,
                                 sequence,
                                 mode,
                                 frontend::Json{{"permittedScopes", frontend::Json::array({"observe"})}},
                                 capabilities(),
                                 methods,
                                 methods};
    }

    frontend::Json expandedState(std::optional<frontend::Json> pendingRequests = std::nullopt) {
        frontend::Json state = frontend::Json::object();
        state["provider"] = frontend::Json{{"lifecycle", "ready"},
                                           {"generation", 1},
                                           {"desiredRunning", true},
                                           {"recovery", frontend::Json{{"status", "idle"}, {"attempts", 0}}}};
        state["controller"] = frontend::Json::object();
        state["sessions"] = frontend::Json::array();
        state["threadList"] = frontend::Json{{"hasLoadedPage", false}, {"complete", false}, {"pagesLoaded", std::uint64_t{0}}};
        state["capacity"] = frontend::Json::object();
        state["truncation"] = frontend::Json{{"truncated", false}};
        if (pendingRequests) {
            state["pendingRequests"] = std::move(*pendingRequests);
        }
        return state;
    }

    void makeReady(client::Connection& connection, frontend::Json state = expandedState()) {
        connection.transportConnected();
        (void) connection.receive(
            frontend::ServerMessage{modernWelcome("session-1", frontend::SequenceNumber(7), frontend::SyncMode::Snapshot)});
        (void) connection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(7), std::move(state)}});
        (void) connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(7)}});
    }

    std::optional<frontend::generated::DefinedCommand> lastCommand(const Harness& harness) {
        if (harness.outbound.empty() || harness.outbound.back().kind != client::OutboundKind::Command) {
            return std::nullopt;
        }
        auto decoded = frontend::Codec::decodeDefinedCommand(std::string_view(harness.outbound.back().compactJson));
        if (!decoded) {
            return std::nullopt;
        }
        return std::move(decoded).value();
    }

    frontend::Json commandParameters(const frontend::generated::DefinedCommand& command) {
        return std::visit(
            [](const auto& parameters) {
                return parameters.value;
            },
            command.parameters);
    }

    frontend::Json projectedItem(std::string id, std::string text) {
        frontend::Json item = frontend::Json::object();
        item["id"] = std::move(id);
        item["type"] = "agent_message";
        item["status"] = "completed";
        item["agentText"] = std::move(text);
        item["reasoningText"] = "";
        item["reasoningSummary"] = "";
        item["commandOutput"] = "";
        item["droppedContentBytes"] = 0;
        item["contentTruncated"] = false;
        item["data"] = frontend::Json::object();
        item["extensions"] = frontend::Json{{"itemFuture", "preserved"}};
        return item;
    }

    frontend::Json projectedTurn(std::string id, std::string threadId, frontend::Json items = frontend::Json::array()) {
        frontend::Json turn = frontend::Json::object();
        turn["id"] = std::move(id);
        turn["threadId"] = std::move(threadId);
        turn["status"] = "inProgress";
        turn["active"] = true;
        turn["terminal"] = false;
        turn["items"] = std::move(items);
        turn["extensions"] = frontend::Json{{"turnFuture", 7}};
        return turn;
    }

    frontend::Json projectedThread(std::string id, frontend::Json turns = frontend::Json::array()) {
        frontend::Json thread = frontend::Json::object();
        thread["id"] = std::move(id);
        thread["fullyLoaded"] = true;
        thread["turns"] = std::move(turns);
        thread["extensions"] = frontend::Json{{"threadFuture", true}};
        return thread;
    }

    void testBindingAuthority(tests::support::TestResult& result) {
        std::set<ai::openai::codex::frontend::generated::MethodId> methods;
        std::size_t native = 0;
        std::size_t provider = 0;
        std::size_t reverse = 0;
        std::size_t requests = 0;
        std::size_t sensitive = 0;
        for (const client::generated::BindingMetadata& binding : client::generated::AllBindings) {
            methods.insert(binding.method);
            native += binding.category == client::generated::BindingCategory::Native ? 1U : 0U;
            provider += binding.category == client::generated::BindingCategory::Provider ? 1U : 0U;
            reverse += binding.category == client::generated::BindingCategory::Reverse ? 1U : 0U;
            requests += binding.facade == "Requests" ? 1U : 0U;
            sensitive += binding.sensitive ? 1U : 0U;
        }
        result.expectTrue(
            methods.size() == 105 && native == 7 && provider == 86 && reverse == 12 && requests == reverse,
            "the reviewed C++ authority binds every MethodId exactly once as 7 native, 86 provider, and 12 Requests operations");
        result.expectTrue(sensitive == 13, "the reviewed authority marks all 12 reverse responses and account.login.start as sensitive");
        result.expectTrue(client::generated::bindingIsSensitive(ai::openai::codex::frontend::generated::MethodId::AuthenticationRespond) &&
                              client::generated::bindingIsSensitive(ai::openai::codex::frontend::generated::MethodId::AccountLoginStart) &&
                              !client::generated::bindingIsSensitive(ai::openai::codex::frontend::generated::MethodId::ThreadRead),
                          "sensitivity lookup is generated rather than duplicated in Client");
    }

    void testTypedBindingIdentity(tests::support::TestResult& result) {
        using MethodId = ai::openai::codex::frontend::generated::MethodId;
        using ThreadStart = client::generated::BindingTraits<MethodId::ThreadStart>;
        using ApprovalRespond = client::generated::BindingTraits<MethodId::ApprovalRespond>;
        using ControllerAcquire = client::generated::BindingTraits<MethodId::ControllerAcquire>;
        using SessionLookup = const client::SessionState* (client::State::*) (const client::FrontendSessionId&) const noexcept;
        using ProcessLookup = const client::ProcessState* (client::State::*) (const client::ProcessHandle&) const noexcept;

        constexpr bool typed = std::same_as<ThreadStart::Facade, client::Threads> &&
                               std::same_as<ThreadStart::Parameter, ai::openai::codex::typed::ThreadStartParams> &&
                               std::same_as<ThreadStart::Result, client::ThreadStartResult> &&
                               std::same_as<ApprovalRespond::Facade, client::Requests> &&
                               std::same_as<ApprovalRespond::Parameter, client::ApprovalRespondParams> &&
                               std::same_as<ApprovalRespond::Result, ai::openai::codex::typed::Unit> &&
                               std::same_as<ControllerAcquire::Facade, client::Controller> &&
                               std::same_as<ControllerAcquire::Parameter, ai::openai::codex::typed::Unit> &&
                               std::same_as<ControllerAcquire::Result, client::ControllerResult> &&
                               std::same_as<decltype(client::ThreadResultState::state), client::ThreadState> &&
                               std::same_as<decltype(client::ThreadResultState::turns), std::vector<client::TurnResultState>> &&
                               std::same_as<decltype(client::TurnResultState::state), client::TurnState> &&
                               std::same_as<decltype(client::TurnResultState::items), std::vector<client::ItemState>> &&
                               std::same_as<decltype(static_cast<SessionLookup>(&client::State::session)), SessionLookup> &&
                               std::same_as<decltype(static_cast<ProcessLookup>(&client::State::process)), ProcessLookup> &&
                               !client::generated::IsGeneratedJsonWrapper<ThreadStart::Parameter>::value &&
                               !client::generated::IsGeneratedJsonWrapper<ThreadStart::Result>::value;
        result.expectTrue(typed,
                          "normal facade bindings expose domain parameter/result types while generated JSON wrappers remain internal");

        using RestrictedSubmit = client::Submission (client::Client::*)(ai::openai::codex::frontend::generated::CompleteCommandParameters,
                                                                        client::GeneratedCompletionHandler);
        constexpr bool restrictedSubmitPreserved =
            std::same_as<decltype(static_cast<RestrictedSubmit>(&client::Client::submit)), RestrictedSubmit>;
        result.expectTrue(restrictedSubmitPreserved, "the restricted generated submit API remains available separately from typed facades");
    }

    void testNativeCodecAuthority(tests::support::TestResult& result) {
        std::string error;
        const std::optional<frontend::Json> replayParameters =
            client::detail::encodeEventsReplayParams(frontend::SequenceNumber{41}, error);
        result.expectTrue(replayParameters && replayParameters->value("after", 0U) == 41U && error.empty(),
                          "the reviewed native replay parameter encoder is a compiled, executable codec");

        const std::optional<client::ControllerResult> controller = client::detail::decodeControllerResult(
            frontend::Json{{"role", "controller"}, {"controllerSessionId", "17"}}, std::string_view{"17"}, error);
        result.expectTrue(controller && controller->role == frontend::SessionRole::Controller &&
                              controller->controllerSessionId == std::optional<std::string>{"17"} && controller->ownedByThisClient &&
                              error.empty(),
                          "the reviewed native controller result decoder derives ownership from the active session identity");

        const std::optional<client::SynchronizationResult> snapshot = client::detail::decodeSnapshotSynchronizationResult(
            client::detail::SynchronizationDecodeInput{
                frontend::SyncMode::Snapshot, frontend::SequenceNumber{43}, client::State{}, 0, 0, 0, false},
            error);
        const std::optional<client::SynchronizationResult> replayFallback = client::detail::decodeReplaySynchronizationResult(
            client::detail::SynchronizationDecodeInput{
                frontend::SyncMode::Snapshot, frontend::SequenceNumber{44}, client::State{}, 0, 0, 0, true},
            error);
        result.expectTrue(snapshot && snapshot->mode == frontend::SyncMode::Snapshot &&
                              snapshot->synchronizedThrough == frontend::SequenceNumber{43} && replayFallback &&
                              replayFallback->snapshotFallback && replayFallback->synchronizedThrough == frontend::SequenceNumber{44} &&
                              error.empty(),
                          "the reviewed native synchronization result decoders are compiled and preserve snapshot-fallback semantics");

        const std::optional<typed::Unit> additiveUnit = client::detail::decodeUnitResult(frontend::Json{{"future", 1}}, error);
        const std::optional<typed::Unit> invalidUnit = client::detail::decodeUnitResult(frontend::Json::array(), error);
        result.expectTrue(additiveUnit.has_value() && !invalidUnit && !error.empty(),
                          "the frontend Unit bridge tolerates schema-validated additive object fields and rejects non-objects");
    }

    void testFundamentalTypeRules(tests::support::TestResult& result) {
        constexpr bool rules = !std::copy_constructible<client::Client> && !std::move_constructible<client::Client> &&
                               !std::copy_constructible<client::Connection> && std::move_constructible<client::Connection> &&
                               std::copy_constructible<client::State> && std::move_constructible<client::State> &&
                               !std::copy_constructible<client::Controller> && !std::move_constructible<client::Controller> &&
                               !std::copy_constructible<client::Requests> && !std::move_constructible<client::Requests>;
        result.expectTrue(rules,
                          "Client is fixed-address, Connection is move-only, State is cheap-copy capable, and facades are non-copyable");

        client::Client sdk{options()};
        bool stableFacadeAddresses = true;
#define AISUITE_CODEX_CHECK_FACADE_ADDRESS(Type, accessor)                                                                                 \
    stableFacadeAddresses = stableFacadeAddresses && &sdk.accessor() == &sdk.accessor();
        AISUITE_CODEX_CLIENT_FACADE_LIST(AISUITE_CODEX_CHECK_FACADE_ADDRESS)
#undef AISUITE_CODEX_CHECK_FACADE_ADDRESS
        result.expectTrue(stableFacadeAddresses,
                          "all 22 native and domain facade accessors return stable references to pointer-sized non-polymorphic objects");
    }

    void testTypedProviderAndUnitBridge(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk{options()};
        client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        typed::TurnSteerParams steer;
        steer.threadId = typed::ThreadId{"thread-1"};
        steer.expectedTurnId = typed::TurnId{"turn-1"};
        typed::TextInput text;
        text.text = "continue";
        steer.input.emplace_back(std::move(text));
        std::optional<client::OperationResult<typed::TurnSteerResponse>> steerResult;
        const client::Submission steerSubmission =
            sdk.turns().steer(std::move(steer), [&steerResult](const client::OperationResult<typed::TurnSteerResponse>& value) {
                steerResult = value;
            });
        const std::optional<frontend::generated::DefinedCommand> steerCommand = lastCommand(harness);
        const frontend::Json steerParameters = steerCommand ? commandParameters(*steerCommand) : frontend::Json::object();
        result.expectTrue(steerSubmission && steerCommand &&
                              frontend::generated::commandMethod(steerCommand->parameters) == frontend::generated::MethodId::TurnSteer &&
                              steerParameters.value("threadId", std::string{}) == "thread-1" &&
                              steerParameters.value("expectedTurnId", std::string{}) == "turn-1",
                          "a standard provider façade uses its exact typed encoder and internal MethodId");
        if (steerCommand) {
            (void) connection.receive(
                frontend::ServerMessage{frontend::Response::success(steerCommand->requestId, frontend::Json{{"turnId", "turn-2"}})});
        }
        result.expectTrue(steerResult && *steerResult && steerResult->value && steerResult->value->turnId == typed::TurnId{"turn-2"},
                          "a standard provider result is decoded into its public typed response");

        std::size_t unitCallbacks = 0;
        bool unitSucceeded = false;
        const client::Submission unitSubmission =
            sdk.turns().interrupt(typed::TurnInterruptParams{typed::ThreadId{"thread-1"}, typed::TurnId{"turn-2"}},
                                  [&unitCallbacks, &unitSucceeded](const client::OperationResult<typed::Unit>& value) {
                                      ++unitCallbacks;
                                      unitSucceeded = static_cast<bool>(value);
                                  });
        const std::optional<frontend::generated::DefinedCommand> unitCommand = lastCommand(harness);
        result.expectTrue(unitSubmission && unitCommand &&
                              frontend::generated::commandMethod(unitCommand->parameters) == frontend::generated::MethodId::TurnInterrupt,
                          "a Unit-returning typed façade preserves its exact internal MethodId");
        if (unitCommand) {
            (void) connection.receive(
                frontend::ServerMessage{frontend::Response::success(unitCommand->requestId, frontend::Json::object())});
        }
        result.expectTrue(unitCallbacks == 1 && unitSucceeded, "an empty-object Unit result completes its typed operation exactly once");

        bool additiveUnitSucceeded = false;
        const client::Submission additiveUnitSubmission =
            sdk.accounts().logout([&additiveUnitSucceeded](const client::OperationResult<typed::Unit>& value) {
                additiveUnitSucceeded = static_cast<bool>(value);
            });
        const std::optional<frontend::generated::DefinedCommand> additiveUnitCommand = lastCommand(harness);
        if (additiveUnitCommand) {
            (void) connection.receive(
                frontend::ServerMessage{frontend::Response::success(additiveUnitCommand->requestId, frontend::Json{{"future", 1}})});
        }
        result.expectTrue(additiveUnitSubmission && additiveUnitCommand && additiveUnitSucceeded,
                          "a schema-valid additive provider Unit result completes through the stable typed Unit façade");
    }

    void testProjectedThreadAndTurnResults(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk{options()};
        client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        std::optional<client::OperationResult<client::ThreadStartResult>> startResult;
        const client::Submission startSubmission = sdk.threads().start(
            typed::ThreadStartParams{}, [&startResult](const client::OperationResult<client::ThreadStartResult>& value) {
                startResult = value;
            });
        const std::optional<frontend::generated::DefinedCommand> startCommand = lastCommand(harness);
        if (startCommand) {
            frontend::Json firstTurn =
                projectedTurn("turn-start-1",
                              "thread-start",
                              frontend::Json::array({projectedItem("item-start-1", "first"), projectedItem("item-start-2", "second")}));
            frontend::Json secondTurn =
                projectedTurn("turn-start-2", "thread-start", frontend::Json::array({projectedItem("item-start-3", "third")}));
            frontend::Json thread = projectedThread("thread-start", frontend::Json::array({std::move(firstTurn), std::move(secondTurn)}));
            (void) connection.receive(frontend::ServerMessage{
                frontend::Response::success(startCommand->requestId, frontend::Json{{"thread", std::move(thread)}})});
        }
        result.expectTrue(startSubmission && startCommand &&
                              frontend::generated::commandMethod(startCommand->parameters) == frontend::generated::MethodId::ThreadStart &&
                              startResult && *startResult && startResult->value && startResult->value->thread &&
                              startResult->value->threadId == typed::ThreadId{"thread-start"} &&
                              startResult->value->thread->state.orderedTurns ==
                                  std::vector{typed::TurnId{"turn-start-1"}, typed::TurnId{"turn-start-2"}} &&
                              startResult->value->thread->turns.size() == 2 &&
                              startResult->value->thread->turns[0].state.orderedItems ==
                                  std::vector{typed::ItemId{"item-start-1"}, typed::ItemId{"item-start-2"}} &&
                              startResult->value->thread->turns[0].items.size() == 2 &&
                              startResult->value->thread->turns[0].items[1].agentText == std::optional<std::string>{"second"} &&
                              startResult->value->thread->state.extensions.value("threadFuture", false) &&
                              startResult->value->thread->turns[0].state.extensions.value("turnFuture", 0) == 7 &&
                              startResult->value->thread->turns[0].items[0].extensions.value("itemFuture", "") == "preserved" &&
                              startResult->value->thread->turn(typed::TurnId{"turn-start-2"}) != nullptr,
                          "thread.start preserves the complete ordered thread, turn, and item projection");

        typed::ThreadResumeParams resume;
        resume.threadId = typed::ThreadId{"thread-start"};
        std::optional<client::OperationResult<client::ThreadResumeResult>> resumeResult;
        const client::Submission resumeSubmission =
            sdk.threads().resume(std::move(resume), [&resumeResult](const client::OperationResult<client::ThreadResumeResult>& value) {
                resumeResult = value;
            });
        const std::optional<frontend::generated::DefinedCommand> resumeCommand = lastCommand(harness);
        if (resumeCommand) {
            (void) connection.receive(frontend::ServerMessage{
                frontend::Response::success(resumeCommand->requestId, frontend::Json{{"threadId", "thread-resume"}})});
        }
        result.expectTrue(resumeSubmission && resumeCommand &&
                              frontend::generated::commandMethod(resumeCommand->parameters) ==
                                  frontend::generated::MethodId::ThreadResume &&
                              resumeResult && *resumeResult && resumeResult->value && !resumeResult->value->thread &&
                              resumeResult->value->threadId == typed::ThreadId{"thread-resume"},
                          "thread.resume preserves the projected threadId-only fallback shape");

        std::optional<client::OperationResult<client::ThreadListResult>> listResult;
        const client::Submission listSubmission =
            sdk.threads().list(typed::ThreadListParams{}, [&listResult](const client::OperationResult<client::ThreadListResult>& value) {
                listResult = value;
            });
        const std::optional<frontend::generated::DefinedCommand> listCommand = lastCommand(harness);
        if (listCommand) {
            frontend::Json first = projectedThread(
                "thread-list-1",
                frontend::Json::array(
                    {projectedTurn("turn-list-1", "thread-list-1", frontend::Json::array({projectedItem("item-list-1", "listed")}))}));
            frontend::Json second = projectedThread(
                "thread-list-2",
                frontend::Json::array({projectedTurn(
                    "turn-list-2", "thread-list-2", frontend::Json::array({projectedItem("item-list-2", "listed second")}))}));
            (void) connection.receive(frontend::ServerMessage{
                frontend::Response::success(listCommand->requestId,
                                            frontend::Json{{"threads", frontend::Json::array({std::move(first), std::move(second)})},
                                                           {"nextCursor", "next"},
                                                           {"backwardsCursor", "previous"}})});
        }
        result.expectTrue(listSubmission && listCommand &&
                              frontend::generated::commandMethod(listCommand->parameters) == frontend::generated::MethodId::ThreadList &&
                              listResult && *listResult && listResult->value && listResult->value->threads.size() == 2 &&
                              listResult->value->threads.front().state.id == typed::ThreadId{"thread-list-1"} &&
                              listResult->value->threads.front().turns.size() == 1 &&
                              listResult->value->threads.front().turns.front().items.size() == 1 &&
                              listResult->value->threads.front().turns.front().items.front().agentText ==
                                  std::optional<std::string>{"listed"} &&
                              listResult->value->threads.back().state.id == typed::ThreadId{"thread-list-2"} &&
                              listResult->value->nextCursor == std::optional<std::string>{"next"} &&
                              listResult->value->backwardsCursor == std::optional<std::string>{"previous"},
                          "thread.list preserves page, thread, turn, and item ordering with both projected cursors");

        std::optional<client::OperationResult<client::ThreadReadResult>> readResult;
        const client::Submission readSubmission =
            sdk.threads().read(typed::ThreadReadParams{typed::ThreadId{"thread-read"}, true},
                               [&readResult](const client::OperationResult<client::ThreadReadResult>& value) {
                                   readResult = value;
                               });
        const std::optional<frontend::generated::DefinedCommand> readCommand = lastCommand(harness);
        if (readCommand) {
            frontend::Json thread = projectedThread(
                "thread-read",
                frontend::Json::array(
                    {projectedTurn("turn-read", "thread-read", frontend::Json::array({projectedItem("item-read", "read text")}))}));
            thread["preview"] = "typed preview";
            (void) connection.receive(frontend::ServerMessage{
                frontend::Response::success(readCommand->requestId, frontend::Json{{"thread", std::move(thread)}})});
        }
        result.expectTrue(readSubmission && readCommand &&
                              frontend::generated::commandMethod(readCommand->parameters) == frontend::generated::MethodId::ThreadRead &&
                              readResult && *readResult && readResult->value && readResult->value->thread &&
                              readResult->value->thread->state.preview == std::optional<std::string>{"typed preview"} &&
                              readResult->value->thread->turns.size() == 1 &&
                              readResult->value->thread->turns.front().state.id == typed::TurnId{"turn-read"} &&
                              readResult->value->thread->turns.front().items.size() == 1 &&
                              readResult->value->thread->turns.front().items.front().id == typed::ItemId{"item-read"},
                          "thread.read preserves the complete ordered typed thread projection");

        typed::TurnStartParams startTurn;
        startTurn.threadId = typed::ThreadId{"thread-read"};
        typed::TextInput input;
        input.text = "begin";
        startTurn.input.emplace_back(std::move(input));
        std::optional<client::OperationResult<client::TurnStartResult>> turnResult;
        const client::Submission turnSubmission =
            sdk.turns().start(std::move(startTurn), [&turnResult](const client::OperationResult<client::TurnStartResult>& value) {
                turnResult = value;
            });
        const std::optional<frontend::generated::DefinedCommand> turnCommand = lastCommand(harness);
        if (turnCommand) {
            frontend::Json turn = projectedTurn(
                "turn-start",
                "thread-read",
                frontend::Json::array({projectedItem("item-turn-1", "turn first"), projectedItem("item-turn-2", "turn second")}));
            (void) connection.receive(
                frontend::ServerMessage{frontend::Response::success(turnCommand->requestId, frontend::Json{{"turn", std::move(turn)}})});
        }
        result.expectTrue(
            turnSubmission && turnCommand &&
                frontend::generated::commandMethod(turnCommand->parameters) == frontend::generated::MethodId::TurnStart && turnResult &&
                *turnResult && turnResult->value && turnResult->value->turn && turnResult->value->turnId == typed::TurnId{"turn-start"} &&
                turnResult->value->turn->state.threadId == typed::ThreadId{"thread-read"} &&
                turnResult->value->turn->state.orderedItems == std::vector{typed::ItemId{"item-turn-1"}, typed::ItemId{"item-turn-2"}} &&
                turnResult->value->turn->items.size() == 2 && turnResult->value->turn->item(typed::ItemId{"item-turn-2"}) != nullptr,
            "turn.start preserves the complete ordered typed turn and item projection");
    }

    void testMalformedTypedResultFailsOnce(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk{options()};
        client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        std::size_t callbacks = 0;
        std::optional<client::Error> callbackError;
        const client::Submission submission = sdk.threads().start(
            typed::ThreadStartParams{}, [&callbacks, &callbackError](const client::OperationResult<client::ThreadStartResult>& value) {
                ++callbacks;
                callbackError = value.error;
            });
        const std::optional<frontend::generated::DefinedCommand> command = lastCommand(harness);
        if (command) {
            (void) connection.receive(
                frontend::ServerMessage{frontend::Response::success(command->requestId, frontend::Json{{"threadId", std::uint64_t{7}}})});
        }
        connection.transportDisconnected();
        result.expectTrue(submission && command && callbacks == 1 && callbackError &&
                              callbackError->clientCode == client::ClientErrorCode::ResponseTypeMismatch && !connection.isOpen() &&
                              sdk.pendingOperationCount() == 0 && harness.closes == 1,
                          "a malformed typed result fails its callback once, closes only the connection, and cannot complete twice");
    }

    void testNestedResultRelationshipValidation(tests::support::TestResult& result) {
        std::string error;
        frontend::Json wrongParent = projectedThread(
            "thread-parent",
            frontend::Json::array(
                {projectedTurn("turn-child", "different-thread", frontend::Json::array({projectedItem("item-child", "text")}))}));
        result.expectTrue(!client::detail::decodeThreadReadResult(frontend::Json{{"thread", std::move(wrongParent)}}, error) &&
                              !error.empty(),
                          "a thread result cannot claim a nested turn belonging to another thread");

        frontend::Json duplicateItems =
            projectedTurn("turn-duplicate",
                          "thread-parent",
                          frontend::Json::array({projectedItem("item-duplicate", "first"), projectedItem("item-duplicate", "second")}));
        result.expectTrue(!client::detail::decodeTurnStartResult(frontend::Json{{"turn", std::move(duplicateItems)}}, error) &&
                              !error.empty(),
                          "a turn result rejects duplicate nested item identities instead of losing ordering information");
    }

    void testSensitiveReverseBridge(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk{options()};
        client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        std::size_t callbacks = 0;
        bool succeeded = false;
        const client::Submission submission =
            sdk.requests().respond(client::ApprovalRespondParams{client::PendingRequestId{"41"}, typed::ApprovalDecision::accept()},
                                   [&callbacks, &succeeded](const client::OperationResult<typed::Unit>& value) {
                                       ++callbacks;
                                       succeeded = static_cast<bool>(value);
                                   });
        const std::optional<frontend::generated::DefinedCommand> command = lastCommand(harness);
        const frontend::Json parameters = command ? commandParameters(*command) : frontend::Json::object();
        result.expectTrue(submission && command && harness.outbound.back().sensitive &&
                              frontend::generated::commandMethod(command->parameters) == frontend::generated::MethodId::ApprovalRespond &&
                              parameters.value("pendingRequestId", std::string{}) == "41" &&
                              parameters.value("decision", std::string{}) == "accept",
                          "the generated reverse encoder selects the exact MethodId and marks its outbound command sensitive");
        if (command) {
            (void) connection.receive(frontend::ServerMessage{frontend::Response::success(command->requestId, frontend::Json::object())});
        }
        result.expectTrue(callbacks == 1 && succeeded, "a representative typed reverse response completes through the Unit bridge");
    }

    void testInvalidatedReverseRequestGuard(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk{options()};
        client::Connection first = sdk.openConnection(harness.transport());
        frontend::Json pendingRequests = frontend::Json::array();
        pendingRequests.push_back(
            frontend::Json{{"pendingRequestId", "41"}, {"kind", "command_execution_approval"}, {"summary", "approve"}});
        makeReady(first, expandedState(std::optional<frontend::Json>{std::move(pendingRequests)}));
        const client::State currentState = sdk.state();
        const client::PendingRequestState* current = currentState.pendingRequest(client::PendingRequestId{"41"});
        first.transportDisconnected();
        const client::State staleState = sdk.state();
        const client::PendingRequestState* stale = staleState.pendingRequest(client::PendingRequestId{"41"});

        client::Connection second = sdk.openConnection(harness.transport());
        second.transportConnected();
        (void) second.receive(frontend::ServerMessage{modernWelcome("session-2", frontend::SequenceNumber(8), frontend::SyncMode::Replay)});
        (void) second.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(8)}});
        const client::PendingRequestState* retained = sdk.state().pendingRequest(client::PendingRequestId{"41"});
        const std::size_t beforeRejected = harness.outbound.size();
        const client::Submission rejected =
            sdk.requests().respond(client::ApprovalRespondParams{client::PendingRequestId{"41"}, typed::ApprovalDecision::accept()}, {});
        result.expectTrue(current != nullptr && !current->connectionInvalidated && stale != nullptr && stale->connectionInvalidated &&
                              sdk.isReady() && retained != nullptr && retained->connectionInvalidated && !rejected && rejected.error &&
                              rejected.error->clientCode == client::ClientErrorCode::MethodNotPermitted &&
                              harness.outbound.size() == beforeRejected,
                          "a known connection-invalidated pending request is rejected locally without an outbound command");

        const client::Submission deferredToServer =
            sdk.requests().respond(client::ApprovalRespondParams{client::PendingRequestId{"42"}, typed::ApprovalDecision::decline()}, {});
        const std::optional<frontend::generated::DefinedCommand> command = lastCommand(harness);
        result.expectTrue(deferredToServer && command &&
                              frontend::generated::commandMethod(command->parameters) == frontend::generated::MethodId::ApprovalRespond &&
                              commandParameters(*command).value("pendingRequestId", std::string{}) == "42",
                          "an absent or unprojected pending request remains server-authoritative and is submitted normally");
        if (command) {
            (void) second.receive(frontend::ServerMessage{frontend::Response::success(command->requestId, frontend::Json::object())});
        }

        Harness unprojectedHarness;
        client::Client unprojectedSdk{options()};
        client::Connection unprojectedConnection = unprojectedSdk.openConnection(unprojectedHarness.transport());
        makeReady(unprojectedConnection);
        const client::Submission unprojected = unprojectedSdk.requests().respond(
            client::ApprovalRespondParams{client::PendingRequestId{"43"}, typed::ApprovalDecision::decline()}, {});
        const std::optional<frontend::generated::DefinedCommand> unprojectedCommand = lastCommand(unprojectedHarness);
        result.expectTrue(!unprojectedSdk.state().hasPendingRequestProjection() && unprojected && unprojectedCommand &&
                              frontend::generated::commandMethod(unprojectedCommand->parameters) ==
                                  frontend::generated::MethodId::ApprovalRespond,
                          "an unprojected pending-request domain also defers request authority to the server");
        if (unprojectedCommand) {
            (void) unprojectedConnection.receive(
                frontend::ServerMessage{frontend::Response::success(unprojectedCommand->requestId, frontend::Json::object())});
        }
    }
} // namespace

#undef AISUITE_CODEX_CLIENT_FACADE_LIST

int main() {
    tests::support::TestResult result;
    static_assert(client::generated::NativeBindingCount == 7);
    static_assert(client::generated::ProviderBindingCount == 86);
    static_assert(client::generated::ReverseBindingCount == 12);
    static_assert(client::generated::RequestsBindingCount == 12);
    testBindingAuthority(result);
    testTypedBindingIdentity(result);
    testNativeCodecAuthority(result);
    testFundamentalTypeRules(result);
    testTypedProviderAndUnitBridge(result);
    testProjectedThreadAndTurnResults(result);
    testMalformedTypedResultFailsOnce(result);
    testNestedResultRelationshipValidation(result);
    testSensitiveReverseBridge(result);
    testInvalidatedReverseRequestGuard(result);
    return result.processResult();
}
