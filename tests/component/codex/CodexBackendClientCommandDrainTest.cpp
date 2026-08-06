/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "apps/codex-backend-client/CommandDrainController.h"
#include "apps/codex-backend-client/CommandParser.h"
#include "support/TestResult.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace sdk_client = ai::openai::codex::frontend::client;
    namespace app = apps::codex_backend_client;

    struct Harness {
        std::vector<sdk_client::OutboundMessage> outbound;
        std::size_t exits = 0;
        std::vector<std::string> failures;

        sdk_client::TransportCallbacks transport() {
            return {[this](sdk_client::OutboundMessage message) {
                        outbound.push_back(std::move(message));
                        return sdk_client::SendResult{sdk_client::SendStatus::Accepted, std::nullopt};
                    },
                    [](std::string) {
                    }};
        }
    };

    sdk_client::ClientOptions options() {
        sdk_client::ClientOptions result;
        result.credentialProvider = [] {
            return sdk_client::AuthenticationContext{frontend::NoCredential{}, "verified-local:1000"};
        };
        return result;
    }

    frontend::CapabilityAdvertisement capabilities() {
        const std::vector implemented{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
        };
        return {implemented, implemented, implemented, frontend::Json::object()};
    }

    frontend::Json expandedState() {
        frontend::Json state = frontend::Json::object();
        state["provider"] = frontend::Json{{"lifecycle", "ready"},
                                           {"generation", std::uint64_t{1}},
                                           {"desiredRunning", true},
                                           {"recovery", frontend::Json{{"status", "idle"}, {"attempts", std::uint64_t{0}}}}};
        state["controller"] = frontend::Json::object();
        state["sessions"] = frontend::Json::array();
        state["threads"] = frontend::Json::array();
        state["turns"] = frontend::Json::array();
        state["items"] = frontend::Json::array();
        state["pendingRequests"] = frontend::Json::array();
        state["capacity"] = frontend::Json::object();
        state["truncation"] = frontend::Json{{"truncated", false}};
        return state;
    }

    void makeReady(sdk_client::Connection& connection) {
        connection.transportConnected();
        (void) connection.receive(frontend::ServerMessage{frontend::Welcome{"session-1",
                                                                            frontend::SessionRole::Observer,
                                                                            frontend::SequenceNumber(7),
                                                                            frontend::SyncMode::Snapshot,
                                                                            frontend::Json::object(),
                                                                            capabilities()}});
        (void) connection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(7), expandedState()}});
        (void) connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(7)}});
    }

    std::optional<frontend::generated::DefinedCommand> lastCommand(const Harness& harness) {
        if (harness.outbound.empty() || harness.outbound.back().kind != sdk_client::OutboundKind::Command) {
            return std::nullopt;
        }
        const auto decoded = frontend::Codec::decodeDefinedCommand(std::string_view(harness.outbound.back().compactJson));
        if (!decoded) {
            return std::nullopt;
        }
        return decoded.value();
    }

    void testQueueAndEofDrain(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr)
                controller->connectionStateChanged(change.current);
        };
        sdk_client::Client sdk(options(), std::move(callbacks));
        app::CommandDrainController drain(sdk,
                                          {.requestExit =
                                               [&harness] {
                                                   ++harness.exits;
                                               },
                                           .reportFailure =
                                               [&harness](std::string message) {
                                                   harness.failures.push_back(std::move(message));
                                               }});
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());

        app::CommandParser parser;
        const app::ParsedCommand parsedAcquire = parser.parse("acquire");
        const auto* acquire = std::get_if<app::RemoteCommand>(&parsedAcquire);
        result.expectTrue(acquire != nullptr && drain.enqueue(*acquire) && drain.queuedCount() == 1,
                          "commands entered before Ready remain in the CLI application queue");
        makeReady(connection);
        const std::optional<frontend::generated::DefinedCommand> command = lastCommand(harness);
        result.expectTrue(drain.sessionState() == app::CommandDrainController::SessionState::Ready && drain.queuedCount() == 0 && command &&
                              frontend::generated::commandMethod(command->parameters) == frontend::generated::MethodId::ControllerAcquire &&
                              !command->requestId.empty(),
                          "Ready flushes generated parameters and the SDK supplies the request ID");
        if (!command) {
            return;
        }

        drain.inputEof();
        result.expectTrue(harness.exits == 0 && sdk.pendingOperationCount() == 1, "EOF waits on the SDK pending-operation authority");
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(command->requestId, frontend::Json{{"role", "controller"}})});
        result.expectTrue(harness.exits == 1 && !drain.failed() && sdk.pendingOperationCount() == 0,
                          "the terminal SDK operation callback completes EOF drain exactly once");
    }

    void testNewRemainsTwoTypedOperations(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr)
                controller->connectionStateChanged(change.current);
        };
        sdk_client::Client sdk(options(), std::move(callbacks));
        app::CommandDrainController drain(sdk);
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        const app::ParsedCommand parsedNew = parser.parse("new --cwd /work -- hello");
        const auto* compound = std::get_if<app::NewCommand>(&parsedNew);
        result.expectTrue(compound != nullptr && drain.enqueue(*compound), "new is accepted as a CLI-owned typed compound workflow");
        const std::optional<frontend::generated::DefinedCommand> threadStart = lastCommand(harness);
        result.expectTrue(threadStart &&
                              frontend::generated::commandMethod(threadStart->parameters) == frontend::generated::MethodId::ThreadStart,
                          "new first submits typed Threads::start parameters through the SDK");
        if (!threadStart) {
            return;
        }

        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(threadStart->requestId, frontend::Json{{"threadId", "thread-created"}})});
        const std::optional<frontend::generated::DefinedCommand> turnStart = lastCommand(harness);
        result.expectTrue(turnStart.has_value(), "typed thread.start completion submits typed Turns::start");
        if (!turnStart) {
            return;
        }
        const frontend::Json turnParameters = std::visit(
            [](const auto& parameters) {
                return parameters.value;
            },
            turnStart->parameters);
        result.expectTrue(frontend::generated::commandMethod(turnStart->parameters) == frontend::generated::MethodId::TurnStart &&
                              threadStart->requestId != turnStart->requestId && turnParameters.at("threadId") == "thread-created",
                          "the typed ThreadStartResult feeds typed Turns::start with a second SDK-owned request ID");
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(turnStart->requestId, frontend::Json{{"turnId", "turn-created"}})});
        result.expectTrue(drain.newStage() == app::CommandDrainController::NewStage::None && sdk.pendingOperationCount() == 0,
                          "new finishes only after the second typed operation completes");
    }

    void testExplicitSynchronizationDrain(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr)
                controller->connectionStateChanged(change.current);
        };
        sdk_client::Client sdk(options(), std::move(callbacks));
        app::CommandDrainController drain(sdk,
                                          {.requestExit =
                                               [&harness] {
                                                   ++harness.exits;
                                               },
                                           .reportFailure = {}});
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        const app::ParsedCommand parsedSnapshot = parser.parse("snapshot");
        const auto* snapshot = std::get_if<app::RemoteCommand>(&parsedSnapshot);
        result.expectTrue(snapshot != nullptr && drain.enqueue(*snapshot), "snapshot routes through the SDK Synchronization façade");
        const std::optional<frontend::generated::DefinedCommand> command = lastCommand(harness);
        result.expectTrue(command.has_value(), "snapshot submission emits a generated frontend command");
        if (!command) {
            return;
        }
        drain.inputEof();
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(command->requestId, frontend::Json{{"sequence", 8}})});
        result.expectTrue(harness.exits == 0 && sdk.pendingOperationCount() == 1,
                          "a successful synchronization response alone does not complete EOF drain");
        (void) connection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(8), expandedState()}});
        (void) connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(8)}});
        result.expectTrue(harness.exits == 1 && sdk.pendingOperationCount() == 0 && !drain.failed(),
                          "SyncComplete is correlated by the SDK and releases the CLI drain boundary");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testQueueAndEofDrain(result);
    testNewRemainsTwoTypedOperations(result);
    testExplicitSynchronizationDrain(result);
    return result.processResult();
}
