/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "apps/codex-backend-client/ClientConnection.h"
#include "apps/codex-backend-client/CommandDrainController.h"
#include "apps/codex-backend-client/CommandParser.h"
#include "apps/codex-backend-client/Presenter.h"
#include "apps/codex-backend/FrontendCloseReason.h"
#include "support/TestResult.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apps::codex_backend_client {

    struct ClientConnectionTestAccess {
        static void closeTransport(ClientConnection& connection, std::string reason) {
            connection.closeTransport(std::move(reason));
        }
    };

} // namespace apps::codex_backend_client

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace sdk_client = ai::openai::codex::frontend::client;
    namespace app = apps::codex_backend_client;

    struct Harness {
        std::vector<sdk_client::OutboundMessage> outbound;
        std::size_t exits = 0;
        std::size_t closes = 0;
        std::vector<std::string> failures;

        sdk_client::TransportCallbacks transport() {
            return {[this](sdk_client::OutboundMessage message) {
                        outbound.push_back(std::move(message));
                        return sdk_client::SendResult{sdk_client::SendStatus::Accepted, std::nullopt};
                    },
                    [this](std::string) {
                        ++closes;
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
        state["threadList"] = frontend::Json{{"hasLoadedPage", true}, {"complete", true}, {"pagesLoaded", std::uint64_t{1}}};
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

    void makeReadyWithMethods(sdk_client::Connection& connection,
                              std::vector<frontend::FrontendMethod> available,
                              std::vector<frontend::FrontendMethod> permitted) {
        connection.transportConnected();
        frontend::Welcome welcome{"session-1",
                                  frontend::SessionRole::Observer,
                                  frontend::SequenceNumber(7),
                                  frontend::SyncMode::Snapshot,
                                  frontend::Json::object(),
                                  capabilities()};
        welcome.availableMethods = std::move(available);
        welcome.permittedMethods = std::move(permitted);
        (void) connection.receive(frontend::ServerMessage{std::move(welcome)});
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

    const app::RemoteCommand* remoteCommand(app::CommandParser& parser, std::string_view line, app::ParsedCommand& storage) {
        storage = parser.parse(line);
        return std::get_if<app::RemoteCommand>(&storage);
    }

    frontend::Response commandFailure(const frontend::generated::DefinedCommand& command, frontend::ErrorCode code, std::string message) {
        return frontend::Response::failure(command.requestId,
                                           frontend::CommandError{code, std::move(message), std::nullopt, frontend::Json::object()});
    }

    void testQueueAndEofDrain(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr)
                controller->connectionStateChanged(change);
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
                                               },
                                           .requestReconnect = {}});
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

    void testControllerTransitionsOrderQueuedCommands(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
        };
        sdk_client::Client sdk(options(), std::move(callbacks));
        app::CommandDrainController drain(sdk);
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        app::ParsedCommand acquireParsed;
        app::ParsedCommand releaseParsed;
        app::ParsedCommand startParsed;
        const app::RemoteCommand* acquire = remoteCommand(parser, "acquire", acquireParsed);
        const app::RemoteCommand* release = remoteCommand(parser, "release", releaseParsed);
        const app::RemoteCommand* start = remoteCommand(parser, "start", startParsed);
        if (acquire == nullptr || release == nullptr || start == nullptr) {
            result.expectTrue(false, "controller-transition ordering commands parse");
            return;
        }

        result.expectTrue(drain.enqueue(*acquire), "controller.acquire is submitted");
        const std::optional<frontend::generated::DefinedCommand> acquireWire = lastCommand(harness);
        const std::size_t outboundAfterAcquire = harness.outbound.size();
        result.expectTrue(drain.enqueue(*start, 7) && acquireWire &&
                              frontend::generated::commandMethod(acquireWire->parameters) ==
                                  frontend::generated::MethodId::ControllerAcquire &&
                              harness.outbound.size() == outboundAfterAcquire && drain.queuedCount() == 1 &&
                              drain.queuedCommandBytes() == 7 && sdk.pendingOperationCount() == 1,
                          "controller.acquire holds the next controller-required command until its operation completes");
        if (!acquireWire) {
            return;
        }

        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(acquireWire->requestId, frontend::Json{{"role", "controller"}})});
        const std::optional<frontend::generated::DefinedCommand> firstStartWire = lastCommand(harness);
        result.expectTrue(firstStartWire && firstStartWire->requestId != acquireWire->requestId &&
                              frontend::generated::commandMethod(firstStartWire->parameters) ==
                                  frontend::generated::MethodId::ThreadStart &&
                              harness.outbound.size() == outboundAfterAcquire + 1 && drain.queuedCount() == 0 &&
                              drain.queuedCommandBytes() == 0 && sdk.pendingOperationCount() == 1,
                          "controller.acquire completion resumes the queued command exactly once");
        if (!firstStartWire) {
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(firstStartWire->requestId, frontend::Json{{"threadId", "after-acquire"}})});

        result.expectTrue(drain.enqueue(*release), "controller.release is submitted");
        const std::optional<frontend::generated::DefinedCommand> releaseWire = lastCommand(harness);
        const std::size_t outboundAfterRelease = harness.outbound.size();
        result.expectTrue(drain.enqueue(*start, 9) && releaseWire &&
                              frontend::generated::commandMethod(releaseWire->parameters) ==
                                  frontend::generated::MethodId::ControllerRelease &&
                              harness.outbound.size() == outboundAfterRelease && drain.queuedCount() == 1 &&
                              drain.queuedCommandBytes() == 9 && sdk.pendingOperationCount() == 1,
                          "controller.release holds the next queued command until its operation completes");
        if (!releaseWire) {
            return;
        }

        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(releaseWire->requestId, frontend::Json{{"role", "observer"}})});
        const std::optional<frontend::generated::DefinedCommand> secondStartWire = lastCommand(harness);
        result.expectTrue(secondStartWire && secondStartWire->requestId != releaseWire->requestId &&
                              frontend::generated::commandMethod(secondStartWire->parameters) ==
                                  frontend::generated::MethodId::ThreadStart &&
                              harness.outbound.size() == outboundAfterRelease + 1 && drain.queuedCount() == 0 &&
                              drain.queuedCommandBytes() == 0 && sdk.pendingOperationCount() == 1,
                          "controller.release completion resumes the queued command exactly once");
        if (secondStartWire) {
            (void) connection.receive(frontend::ServerMessage{
                frontend::Response::success(secondStartWire->requestId, frontend::Json{{"threadId", "after-release"}})});
        }

        app::ParsedCommand rawAcquireParsed;
        const app::RemoteCommand* rawAcquire =
            remoteCommand(parser, R"(raw {"method":"controller.acquire","params":{}})", rawAcquireParsed);
        if (rawAcquire == nullptr) {
            result.expectTrue(false, "raw controller.acquire parses for the ordering-border probe");
            return;
        }
        result.expectTrue(drain.enqueue(*rawAcquire), "raw controller.acquire is submitted");
        const std::optional<frontend::generated::DefinedCommand> rawAcquireWire = lastCommand(harness);
        const std::size_t outboundAfterRawAcquire = harness.outbound.size();
        result.expectTrue(drain.enqueue(*start) && rawAcquireWire && harness.outbound.size() == outboundAfterRawAcquire,
                          "the raw command border cannot bypass controller-transition ordering");
        if (rawAcquireWire) {
            (void) connection.receive(
                frontend::ServerMessage{frontend::Response::success(rawAcquireWire->requestId, frontend::Json{{"role", "controller"}})});
            result.expectTrue(harness.outbound.size() == outboundAfterRawAcquire + 1,
                              "raw controller transition completion resumes the queued command");
        }
    }

    void testNewRemainsTwoTypedOperations(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr)
                controller->connectionStateChanged(change);
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

    void testCommandFailureRemainsInteractive(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
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
                                               },
                                           .requestReconnect = {}});
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        app::ParsedCommand parsed;
        const app::RemoteCommand* acquire = remoteCommand(parser, "acquire", parsed);
        result.expectTrue(acquire != nullptr && drain.enqueue(*acquire), "the command under test is accepted");
        const std::optional<frontend::generated::DefinedCommand> failed = lastCommand(harness);
        if (!failed) {
            result.expectTrue(false, "the accepted command emits a request");
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{commandFailure(*failed, frontend::ErrorCode::PermissionDenied, "the current controller is required")});

        result.expectTrue(drain.outcome() == app::CommandDrainController::Outcome::Running &&
                              drain.inputState() == app::CommandDrainController::InputState::Reading &&
                              drain.sessionState() == app::CommandDrainController::SessionState::Ready && sdk.isReady() &&
                              connection.isOpen() && sdk.pendingOperationCount() == 0 && harness.exits == 0 && harness.failures.empty(),
                          "Response(ok=false) ends only its command and leaves the interactive connection Ready");

        app::ParsedCommand nextParsed;
        const app::RemoteCommand* next = remoteCommand(parser, "acquire", nextParsed);
        result.expectTrue(next != nullptr && drain.enqueue(*next), "a valid command can be submitted after permission_denied");
        const std::optional<frontend::generated::DefinedCommand> succeeded = lastCommand(harness);
        if (succeeded) {
            (void) connection.receive(
                frontend::ServerMessage{frontend::Response::success(succeeded->requestId, frontend::Json{{"role", "controller"}})});
        }
        result.expectTrue(succeeded && failed->requestId != succeeded->requestId && sdk.pendingOperationCount() == 0 && sdk.isReady() &&
                              drain.outcome() == app::CommandDrainController::Outcome::Running && harness.exits == 0,
                          "a later command receives a fresh SDK request ID and completes without retrying the failed command");
        drain.quit();
        result.expectTrue(drain.outcome() == app::CommandDrainController::Outcome::Success && harness.exits == 1,
                          "interactive quit remains successful even after an earlier interactive command failed");
    }

    void testCommandFailureDispositionMatrix(tests::support::TestResult& result) {
        constexpr std::array commandErrors{
            frontend::ErrorCode::PermissionDenied,
            frontend::ErrorCode::InvalidCommand,
            frontend::ErrorCode::NotFound,
            frontend::ErrorCode::Conflict,
            frontend::ErrorCode::LocalSubmissionFailure,
            frontend::ErrorCode::TypedDecodingFailure,
            frontend::ErrorCode::RemoteAppServerError,
            frontend::ErrorCode::Cancelled,
            frontend::ErrorCode::BackendUnavailable,
            frontend::ErrorCode::DuplicateRequestId,
            frontend::ErrorCode::CapacityExceeded,
            frontend::ErrorCode::RateLimited,
        };

        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
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
                                               },
                                           .requestReconnect = {}});
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        for (const frontend::ErrorCode error : commandErrors) {
            app::ParsedCommand failedParsed;
            const app::RemoteCommand* failedCommand = remoteCommand(parser, "acquire", failedParsed);
            if (failedCommand == nullptr || !drain.enqueue(*failedCommand)) {
                result.expectTrue(false, "table-driven command submission remains accepted");
                return;
            }
            const std::optional<frontend::generated::DefinedCommand> failedWire = lastCommand(harness);
            if (!failedWire) {
                result.expectTrue(false, "table-driven command emits a request");
                return;
            }
            (void) connection.receive(
                frontend::ServerMessage{commandFailure(*failedWire, error, "representative ordinary command failure")});
            const bool failureWasLocalToCommand = drain.outcome() == app::CommandDrainController::Outcome::Running &&
                                                  drain.inputState() == app::CommandDrainController::InputState::Reading &&
                                                  drain.sessionState() == app::CommandDrainController::SessionState::Ready &&
                                                  sdk.isReady() && connection.isOpen() && sdk.pendingOperationCount() == 0 &&
                                                  harness.exits == 0 && harness.closes == 0 && harness.failures.empty();

            app::ParsedCommand nextParsed;
            const app::RemoteCommand* nextCommand = remoteCommand(parser, "acquire", nextParsed);
            if (nextCommand == nullptr || !drain.enqueue(*nextCommand)) {
                result.expectTrue(false, "the next table-driven command remains usable");
                return;
            }
            const std::optional<frontend::generated::DefinedCommand> nextWire = lastCommand(harness);
            if (nextWire) {
                (void) connection.receive(
                    frontend::ServerMessage{frontend::Response::success(nextWire->requestId, frontend::Json{{"role", "controller"}})});
            }
            result.expectTrue(failureWasLocalToCommand && nextWire && nextWire->requestId != failedWire->requestId && sdk.isReady() &&
                                  sdk.pendingOperationCount() == 0 && harness.exits == 0 && harness.closes == 0,
                              "each normal Response(ok=false) category preserves process, input, transport, and next-command usability");
        }
    }

    void testLocalErrorsRemainInteractive(tests::support::TestResult& result) {
        const auto exerciseMethodStatus = [&result](bool permitted) {
            Harness harness;
            app::CommandDrainController* controller = nullptr;
            sdk_client::ClientCallbacks callbacks;
            callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
                if (controller != nullptr) {
                    controller->connectionStateChanged(change);
                }
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
                                                   },
                                               .requestReconnect = {}});
            controller = &drain;
            sdk_client::Connection connection = sdk.openConnection(harness.transport());
            makeReadyWithMethods(connection,
                                 permitted ? std::vector<frontend::FrontendMethod>{"controller.acquire", "controller.release"}
                                           : std::vector<frontend::FrontendMethod>{"controller.release"},
                                 std::vector<frontend::FrontendMethod>{"controller.release"});

            const std::size_t outboundBefore = harness.outbound.size();
            app::CommandParser parser;
            app::ParsedCommand acquireParsed;
            const app::RemoteCommand* acquire = remoteCommand(parser, "acquire", acquireParsed);
            const bool rejected = acquire != nullptr && !drain.enqueue(*acquire);
            const bool correctLocalCode =
                !harness.failures.empty() && harness.failures.back().find(permitted ? "not permitted" : "unavailable") != std::string::npos;

            app::ParsedCommand releaseParsed;
            const app::RemoteCommand* release = remoteCommand(parser, "release", releaseParsed);
            const bool nextAccepted = release != nullptr && drain.enqueue(*release);
            const std::optional<frontend::generated::DefinedCommand> nextWire = lastCommand(harness);
            if (nextWire) {
                (void) connection.receive(
                    frontend::ServerMessage{frontend::Response::success(nextWire->requestId, frontend::Json{{"role", "observer"}})});
            }
            result.expectTrue(
                rejected && correctLocalCode && nextAccepted && harness.outbound.size() == outboundBefore + 1 && sdk.isReady() &&
                    connection.isOpen() && drain.outcome() == app::CommandDrainController::Outcome::Running &&
                    drain.inputState() == app::CommandDrainController::InputState::Reading && harness.exits == 0 && harness.closes == 0,
                permitted ? "local MethodNotPermitted rejects only that input line and the next command remains usable"
                          : "local MethodUnavailable rejects only that input line and the next command remains usable");
        };

        exerciseMethodStatus(false);
        exerciseMethodStatus(true);

        Harness queuedHarness;
        app::CommandDrainController* queuedController = nullptr;
        sdk_client::ClientCallbacks queuedCallbacks;
        queuedCallbacks.onConnectionStateChanged = [&queuedController](const sdk_client::ConnectionStateChange& change) {
            if (queuedController != nullptr) {
                queuedController->connectionStateChanged(change);
            }
        };
        sdk_client::Client queuedSdk(options(), std::move(queuedCallbacks));
        app::CommandDrainController queuedDrain(queuedSdk,
                                                {.requestExit = {},
                                                 .reportFailure =
                                                     [&queuedHarness](std::string message) {
                                                         queuedHarness.failures.push_back(std::move(message));
                                                     },
                                                 .requestReconnect = {}});
        queuedController = &queuedDrain;
        sdk_client::Connection queuedConnection = queuedSdk.openConnection(queuedHarness.transport());
        app::CommandParser queuedParser;
        app::ParsedCommand unavailableParsed;
        app::ParsedCommand availableParsed;
        const app::RemoteCommand* unavailable = remoteCommand(queuedParser, "acquire", unavailableParsed);
        const app::RemoteCommand* available = remoteCommand(queuedParser, "release", availableParsed);
        result.expectTrue(unavailable != nullptr && available != nullptr && queuedDrain.enqueue(*unavailable, 7) &&
                              queuedDrain.enqueue(*available, 8) && queuedDrain.queuedCount() == 2 &&
                              queuedDrain.queuedCommandBytes() == 15,
                          "pre-Ready queue retains a command that will be locally rejected followed by valid work");
        makeReadyWithMethods(queuedConnection,
                             std::vector<frontend::FrontendMethod>{"controller.release"},
                             std::vector<frontend::FrontendMethod>{"controller.release"});
        const std::optional<frontend::generated::DefinedCommand> availableWire = lastCommand(queuedHarness);
        result.expectTrue(queuedHarness.failures.size() == 1 && queuedHarness.failures.front().find("unavailable") != std::string::npos &&
                              availableWire &&
                              frontend::generated::commandMethod(availableWire->parameters) ==
                                  frontend::generated::MethodId::ControllerRelease &&
                              queuedDrain.queuedCount() == 0 && queuedDrain.queuedCommandBytes() == 0 &&
                              queuedSdk.pendingOperationCount() == 1 && queuedSdk.isReady(),
                          "queued command-local rejection releases accounting and preserves FIFO progress to the next command");
        if (availableWire) {
            (void) queuedConnection.receive(
                frontend::ServerMessage{frontend::Response::success(availableWire->requestId, frontend::Json{{"role", "observer"}})});
        }

        Harness parseHarness;
        sdk_client::Client parseSdk(options());
        app::CommandDrainController parseDrain(parseSdk,
                                               {.requestExit =
                                                    [&parseHarness] {
                                                        ++parseHarness.exits;
                                                    },
                                                .reportFailure =
                                                    [&parseHarness](std::string message) {
                                                        parseHarness.failures.push_back(std::move(message));
                                                    },
                                                .requestReconnect = {}});
        parseDrain.localCommandFailed("unknown command: this-command-does-not-exist");
        parseDrain.localCommandFailed("raw requires one complete JSON object");
        result.expectTrue(parseDrain.outcome() == app::CommandDrainController::Outcome::Running &&
                              parseDrain.inputState() == app::CommandDrainController::InputState::Reading && parseHarness.exits == 0 &&
                              parseHarness.closes == 0 && parseHarness.failures.size() == 2,
                          "unknown and malformed local commands are reported independently without terminating the application");
    }

    void testTwoSuccessfulNewWorkflowsPreserveOrder(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
        };
        sdk_client::Client sdk(options(), std::move(callbacks));
        app::CommandDrainController drain(sdk);
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        const app::ParsedCommand firstParsed = parser.parse("new -- first prompt");
        const app::ParsedCommand secondParsed = parser.parse("new -- second prompt");
        const auto* first = std::get_if<app::NewCommand>(&firstParsed);
        const auto* second = std::get_if<app::NewCommand>(&secondParsed);
        result.expectTrue(first != nullptr && second != nullptr && drain.enqueue(*first, 19) && drain.enqueue(*second, 20) &&
                              drain.queuedCount() == 1,
                          "two new commands retain input order with only one active workflow");

        const std::optional<frontend::generated::DefinedCommand> firstThread = lastCommand(harness);
        if (!firstThread) {
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(firstThread->requestId, frontend::Json{{"threadId", "first-thread"}})});
        const std::optional<frontend::generated::DefinedCommand> firstTurn = lastCommand(harness);
        if (!firstTurn) {
            return;
        }
        const frontend::Json firstTurnParameters = std::visit(
            [](const auto& parameters) {
                return parameters.value;
            },
            firstTurn->parameters);
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(firstTurn->requestId, frontend::Json{{"turnId", "first-turn"}})});

        const std::optional<frontend::generated::DefinedCommand> secondThread = lastCommand(harness);
        if (!secondThread) {
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(secondThread->requestId, frontend::Json{{"threadId", "second-thread"}})});
        const std::optional<frontend::generated::DefinedCommand> secondTurn = lastCommand(harness);
        if (!secondTurn) {
            return;
        }
        const frontend::Json secondTurnParameters = std::visit(
            [](const auto& parameters) {
                return parameters.value;
            },
            secondTurn->parameters);
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(secondTurn->requestId, frontend::Json{{"turnId", "second-turn"}})});

        result.expectTrue(
            firstTurnParameters.at("threadId") == "first-thread" && firstTurnParameters.at("input").at(0).at("text") == "first prompt" &&
                secondTurnParameters.at("threadId") == "second-thread" &&
                secondTurnParameters.at("input").at(0).at("text") == "second prompt" && firstThread->requestId != firstTurn->requestId &&
                firstTurn->requestId != secondThread->requestId && secondThread->requestId != secondTurn->requestId &&
                drain.newStage() == app::CommandDrainController::NewStage::None && drain.queuedCount() == 0 &&
                drain.queuedCommandBytes() == 0 && sdk.pendingOperationCount() == 0 && sdk.isReady(),
            "two successful new workflows preserve independent prompts, thread IDs, request IDs, and FIFO completion");
    }

    void testQueuedNewWorkflowsRemainIndependent(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
        };
        sdk_client::Client sdk(options(), std::move(callbacks));
        app::CommandDrainController drain(sdk);
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        const app::ParsedCommand firstParsed = parser.parse("new -- first");
        const app::ParsedCommand secondParsed = parser.parse("new -- second");
        const auto* first = std::get_if<app::NewCommand>(&firstParsed);
        const auto* second = std::get_if<app::NewCommand>(&secondParsed);
        result.expectTrue(first != nullptr && second != nullptr && drain.enqueue(*first, 12) && drain.enqueue(*second, 13) &&
                              drain.newStage() == app::CommandDrainController::NewStage::AwaitingThreadStartResponse &&
                              drain.queuedCount() == 1 && drain.queuedCommandBytes() == 13,
                          "a queued second new does not overwrite the active compound workflow");
        const std::optional<frontend::generated::DefinedCommand> firstThread = lastCommand(harness);
        if (!firstThread) {
            result.expectTrue(false, "the first new emits thread.start");
            return;
        }
        (void) connection.receive(frontend::ServerMessage{
            commandFailure(*firstThread, frontend::ErrorCode::PermissionDenied, "the current controller is required")});
        const std::optional<frontend::generated::DefinedCommand> secondThread = lastCommand(harness);
        result.expectTrue(secondThread && secondThread->requestId != firstThread->requestId &&
                              frontend::generated::commandMethod(secondThread->parameters) == frontend::generated::MethodId::ThreadStart &&
                              drain.queuedCount() == 0 &&
                              drain.newStage() == app::CommandDrainController::NewStage::AwaitingThreadStartResponse && sdk.isReady(),
                          "failure resets only the first new and starts the next queued workflow in order");
        if (!secondThread) {
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(secondThread->requestId, frontend::Json{{"threadId", "second-thread"}})});
        const std::optional<frontend::generated::DefinedCommand> secondTurn = lastCommand(harness);
        if (!secondTurn) {
            result.expectTrue(false, "the successful second thread.start emits turn.start");
            return;
        }
        const frontend::Json secondTurnParameters = std::visit(
            [](const auto& parameters) {
                return parameters.value;
            },
            secondTurn->parameters);
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(secondTurn->requestId, frontend::Json{{"turnId", "second-turn"}})});
        result.expectTrue(secondTurnParameters.at("threadId") == "second-thread" &&
                              drain.newStage() == app::CommandDrainController::NewStage::None && sdk.pendingOperationCount() == 0 &&
                              drain.outcome() == app::CommandDrainController::Outcome::Running,
                          "the second new keeps its own prompt/thread state and completes independently");

        const app::ParsedCommand thirdParsed = parser.parse("new -- third");
        const auto* third = std::get_if<app::NewCommand>(&thirdParsed);
        app::ParsedCommand queuedParsed;
        const app::RemoteCommand* queuedAcquire = remoteCommand(parser, "acquire", queuedParsed);
        result.expectTrue(third != nullptr && queuedAcquire != nullptr && drain.enqueue(*third) && drain.enqueue(*queuedAcquire),
                          "an ordinary command may queue behind an active new workflow");
        const std::optional<frontend::generated::DefinedCommand> thirdThread = lastCommand(harness);
        if (!thirdThread) {
            return;
        }
        (void) connection.receive(frontend::ServerMessage{
            frontend::Response::success(thirdThread->requestId, frontend::Json{{"threadId", "created-before-turn-failure"}})});
        const std::optional<frontend::generated::DefinedCommand> thirdTurn = lastCommand(harness);
        if (!thirdTurn) {
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{commandFailure(*thirdTurn, frontend::ErrorCode::Conflict, "turn could not be started")});
        const std::optional<frontend::generated::DefinedCommand> acquireAfterFailure = lastCommand(harness);
        result.expectTrue(acquireAfterFailure && acquireAfterFailure->requestId != thirdTurn->requestId &&
                              frontend::generated::commandMethod(acquireAfterFailure->parameters) ==
                                  frontend::generated::MethodId::ControllerAcquire &&
                              drain.newStage() == app::CommandDrainController::NewStage::None && sdk.isReady(),
                          "turn.start failure preserves its created thread, resets the workflow, and flushes later commands");
    }

    void testNewStageTwoLocalRejection(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
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
                                               },
                                           .requestReconnect = {}});
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReadyWithMethods(connection,
                             std::vector<frontend::FrontendMethod>{"thread.start", "controller.acquire"},
                             std::vector<frontend::FrontendMethod>{"thread.start", "controller.acquire"});

        app::CommandParser parser;
        const app::ParsedCommand newParsed = parser.parse("new -- stage two must reject locally");
        const auto* compound = std::get_if<app::NewCommand>(&newParsed);
        app::ParsedCommand acquireParsed;
        const app::RemoteCommand* acquire = remoteCommand(parser, "acquire", acquireParsed);
        result.expectTrue(compound != nullptr && acquire != nullptr && drain.enqueue(*compound) && drain.enqueue(*acquire, 7) &&
                              drain.queuedCount() == 1,
                          "a command may wait behind the active new workflow before local stage-two rejection");
        const std::optional<frontend::generated::DefinedCommand> thread = lastCommand(harness);
        if (!thread) {
            result.expectTrue(false, "the local stage-two rejection fixture emits thread.start");
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(thread->requestId, frontend::Json{{"threadId", "created-thread"}})});

        const std::optional<frontend::generated::DefinedCommand> acquireAfterRejection = lastCommand(harness);
        result.expectTrue(harness.failures.size() == 1 && harness.failures.front().find("unavailable") != std::string::npos &&
                              acquireAfterRejection && acquireAfterRejection->requestId != thread->requestId &&
                              frontend::generated::commandMethod(acquireAfterRejection->parameters) ==
                                  frontend::generated::MethodId::ControllerAcquire &&
                              drain.newStage() == app::CommandDrainController::NewStage::None && drain.queuedCount() == 0 &&
                              drain.queuedCommandBytes() == 0 && sdk.pendingOperationCount() == 1 && sdk.isReady() && connection.isOpen() &&
                              drain.outcome() == app::CommandDrainController::Outcome::Running && harness.exits == 0 && harness.closes == 0,
                          "local turn.start rejection preserves the created thread, resets new, and flushes later work");
        if (acquireAfterRejection) {
            (void) connection.receive(frontend::ServerMessage{
                frontend::Response::success(acquireAfterRejection->requestId, frontend::Json{{"role", "controller"}})});
        }
    }

    void testTurnFailureThenNextNewSucceeds(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
        };
        sdk_client::Client sdk(options(), std::move(callbacks));
        app::CommandDrainController drain(sdk);
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        const app::ParsedCommand firstParsed = parser.parse("new -- first turn fails");
        const app::ParsedCommand secondParsed = parser.parse("new -- second turn succeeds");
        const auto* first = std::get_if<app::NewCommand>(&firstParsed);
        const auto* second = std::get_if<app::NewCommand>(&secondParsed);
        result.expectTrue(first != nullptr && second != nullptr && drain.enqueue(*first) && drain.enqueue(*second, 24),
                          "the next new remains queued while the first workflow advances to turn.start");

        const std::optional<frontend::generated::DefinedCommand> firstThread = lastCommand(harness);
        if (!firstThread) {
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(firstThread->requestId, frontend::Json{{"threadId", "first-thread"}})});
        const std::optional<frontend::generated::DefinedCommand> firstTurn = lastCommand(harness);
        if (!firstTurn) {
            return;
        }
        (void) connection.receive(frontend::ServerMessage{commandFailure(*firstTurn, frontend::ErrorCode::Conflict, "first turn failed")});

        const std::optional<frontend::generated::DefinedCommand> secondThread = lastCommand(harness);
        if (!secondThread) {
            result.expectTrue(false, "turn failure starts the next queued new workflow");
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(secondThread->requestId, frontend::Json{{"threadId", "second-thread"}})});
        const std::optional<frontend::generated::DefinedCommand> secondTurn = lastCommand(harness);
        if (!secondTurn) {
            return;
        }
        const frontend::Json secondTurnParameters = std::visit(
            [](const auto& parameters) {
                return parameters.value;
            },
            secondTurn->parameters);
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(secondTurn->requestId, frontend::Json{{"turnId", "second-turn"}})});

        result.expectTrue(secondThread->requestId != firstTurn->requestId && secondTurn->requestId != secondThread->requestId &&
                              secondTurnParameters.at("threadId") == "second-thread" &&
                              secondTurnParameters.at("input").at(0).at("text") == "second turn succeeds" &&
                              drain.newStage() == app::CommandDrainController::NewStage::None && drain.queuedCount() == 0 &&
                              sdk.pendingOperationCount() == 0 && sdk.isReady() && connection.isOpen() &&
                              drain.outcome() == app::CommandDrainController::Outcome::Running,
                          "turn failure ends only its workflow and the next queued new succeeds independently");
    }

    void testDisconnectDuringActiveNew(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
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
                                               },
                                           .requestReconnect = {}});
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        const app::ParsedCommand activeParsed = parser.parse("new -- active at disconnect");
        const app::ParsedCommand queuedParsed = parser.parse("new -- must not retry");
        const auto* active = std::get_if<app::NewCommand>(&activeParsed);
        const auto* queued = std::get_if<app::NewCommand>(&queuedParsed);
        result.expectTrue(active != nullptr && queued != nullptr && drain.enqueue(*active) && drain.enqueue(*queued, 20) &&
                              drain.newStage() == app::CommandDrainController::NewStage::AwaitingThreadStartResponse &&
                              drain.queuedCount() == 1,
                          "disconnect fixture has one accepted new and one unsubmitted queued new");
        const std::size_t outboundBeforeDisconnect = harness.outbound.size();
        connection.transportDisconnected(sdk_client::TransportError{"physical transport lost", true});
        drain.disconnected();

        result.expectTrue(drain.newStage() == app::CommandDrainController::NewStage::None && drain.queuedCount() == 0 &&
                              drain.queuedCommandBytes() == 0 && sdk.pendingOperationCount() == 0 &&
                              harness.outbound.size() == outboundBeforeDisconnect &&
                              drain.sessionState() == app::CommandDrainController::SessionState::Disconnected &&
                              drain.outcome() == app::CommandDrainController::Outcome::Running &&
                              drain.inputState() == app::CommandDrainController::InputState::Reading && harness.exits == 0,
                          "disconnect clears active and queued new state exactly once without retry or process termination");
    }

    void testCorrelatedNonClosingProtocolError(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
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
                                               },
                                           .requestReconnect = {}});
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        app::ParsedCommand acquireParsed;
        const app::RemoteCommand* acquire = remoteCommand(parser, "acquire", acquireParsed);
        result.expectTrue(acquire != nullptr && drain.enqueue(*acquire), "protocol-error fixture accepts its correlated command");
        const std::optional<frontend::generated::DefinedCommand> command = lastCommand(harness);
        if (!command) {
            return;
        }
        frontend::ProtocolErrorMessage protocolError;
        protocolError.code = frontend::ErrorCode::InvalidCommand;
        protocolError.message = "correlated non-closing command error";
        protocolError.requestId = command->requestId;
        (void) connection.receive(frontend::ServerMessage{std::move(protocolError)});

        app::ParsedCommand nextParsed;
        const app::RemoteCommand* next = remoteCommand(parser, "release", nextParsed);
        const bool nextAccepted = next != nullptr && drain.enqueue(*next);
        const std::optional<frontend::generated::DefinedCommand> nextCommand = lastCommand(harness);
        result.expectTrue(nextAccepted && nextCommand && nextCommand->requestId != command->requestId && sdk.pendingOperationCount() == 1 &&
                              sdk.isReady() && connection.isOpen() &&
                              drain.sessionState() == app::CommandDrainController::SessionState::Ready &&
                              drain.outcome() == app::CommandDrainController::Outcome::Running && harness.exits == 0 &&
                              harness.closes == 0 && harness.failures.empty(),
                          "correlated non-closing protocol.error fails one command and preserves the interactive Ready connection");
        if (nextCommand) {
            (void) connection.receive(
                frontend::ServerMessage{frontend::Response::success(nextCommand->requestId, frontend::Json{{"role", "observer"}})});
        }
    }

    void testExplicitSynchronizationDrain(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr)
                controller->connectionStateChanged(change);
        };
        sdk_client::Client sdk(options(), std::move(callbacks));
        app::CommandDrainController drain(sdk,
                                          {.requestExit =
                                               [&harness] {
                                                   ++harness.exits;
                                               },
                                           .reportFailure = {},
                                           .requestReconnect = {}});
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

    void testRejectedSynchronizationReturnsToReady(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
        };
        sdk_client::Client sdk(options(), std::move(callbacks));
        app::CommandDrainController drain(sdk);
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        app::ParsedCommand snapshotParsed;
        app::ParsedCommand acquireParsed;
        const app::RemoteCommand* snapshot = remoteCommand(parser, "snapshot", snapshotParsed);
        const app::RemoteCommand* acquire = remoteCommand(parser, "acquire", acquireParsed);
        result.expectTrue(snapshot != nullptr && acquire != nullptr && drain.enqueue(*snapshot) && drain.enqueue(*acquire) &&
                              drain.pendingSyncCount() == 1 && drain.queuedCount() == 1,
                          "ordinary commands remain queued behind an accepted explicit synchronization");
        const std::optional<frontend::generated::DefinedCommand> snapshotCommand = lastCommand(harness);
        if (!snapshotCommand) {
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{commandFailure(*snapshotCommand, frontend::ErrorCode::Conflict, "snapshot request rejected")});
        const std::optional<frontend::generated::DefinedCommand> acquireCommand = lastCommand(harness);
        result.expectTrue(acquireCommand && acquireCommand->requestId != snapshotCommand->requestId &&
                              frontend::generated::commandMethod(acquireCommand->parameters) ==
                                  frontend::generated::MethodId::ControllerAcquire &&
                              drain.pendingSyncCount() == 0 && drain.queuedCount() == 0 && sdk.isReady() && connection.isOpen() &&
                              drain.outcome() == app::CommandDrainController::Outcome::Running,
                          "a rejected explicit snapshot returns to Ready and flushes later commands without closing the application");
        if (!acquireCommand) {
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(acquireCommand->requestId, frontend::Json{{"role", "controller"}})});

        app::ParsedCommand replayParsed;
        app::ParsedCommand releaseParsed;
        const app::RemoteCommand* replay = remoteCommand(parser, "replay 7", replayParsed);
        const app::RemoteCommand* release = remoteCommand(parser, "release", releaseParsed);
        result.expectTrue(replay != nullptr && release != nullptr && drain.enqueue(*replay) && drain.enqueue(*release) &&
                              drain.pendingSyncCount() == 1 && drain.queuedCount() == 1,
                          "replay uses the same explicit-synchronization queue boundary");
        const std::optional<frontend::generated::DefinedCommand> replayCommand = lastCommand(harness);
        if (!replayCommand) {
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{commandFailure(*replayCommand, frontend::ErrorCode::BackendUnavailable, "replay request rejected")});
        const std::optional<frontend::generated::DefinedCommand> releaseCommand = lastCommand(harness);
        result.expectTrue(releaseCommand && releaseCommand->requestId != replayCommand->requestId &&
                              frontend::generated::commandMethod(releaseCommand->parameters) ==
                                  frontend::generated::MethodId::ControllerRelease &&
                              drain.pendingSyncCount() == 0 && drain.queuedCount() == 0 && sdk.isReady() && connection.isOpen() &&
                              drain.outcome() == app::CommandDrainController::Outcome::Running,
                          "a rejected explicit replay also returns to Ready and flushes later commands");
    }

    void testBoundedQueueAndTemporaryDeferral(tests::support::TestResult& result) {
        app::CommandParser parser;
        app::ParsedCommand acquireParsed;
        app::ParsedCommand releaseParsed;
        const app::RemoteCommand* acquire = remoteCommand(parser, "acquire", acquireParsed);
        const app::RemoteCommand* release = remoteCommand(parser, "release", releaseParsed);
        if (acquire == nullptr || release == nullptr) {
            result.expectTrue(false, "queue-bound commands parse");
            return;
        }

        Harness boundedHarness;
        sdk_client::Client boundedSdk(options());
        app::CommandDrainController bounded(boundedSdk,
                                            {.requestExit = {},
                                             .reportFailure =
                                                 [&boundedHarness](std::string message) {
                                                     boundedHarness.failures.push_back(std::move(message));
                                                 },
                                             .requestReconnect = {}},
                                            {.maximumCommands = 2, .maximumCommandBytes = 5});
        sdk_client::Connection boundedConnection = boundedSdk.openConnection(boundedHarness.transport());
        result.expectTrue(bounded.enqueue(*acquire, 2) && bounded.enqueue(*release, 3) && bounded.queuedCount() == 2 &&
                              bounded.queuedCommandBytes() == 5,
                          "the application queue accepts the exact command-count and retained-byte boundaries");
        result.expectTrue(!bounded.enqueue(*acquire, 0) && bounded.queuedCount() == 2 && bounded.queuedCommandBytes() == 5 &&
                              boundedHarness.failures.size() == 1,
                          "count boundary plus one rejects the newest command without evicting admitted entries");

        Harness byteHarness;
        sdk_client::Client byteSdk(options());
        app::CommandDrainController bytes(byteSdk,
                                          {.requestExit = {},
                                           .reportFailure =
                                               [&byteHarness](std::string message) {
                                                   byteHarness.failures.push_back(std::move(message));
                                               },
                                           .requestReconnect = {}},
                                          {.maximumCommands = 3, .maximumCommandBytes = 5});
        sdk_client::Connection byteConnection = byteSdk.openConnection(byteHarness.transport());
        result.expectTrue(bytes.enqueue(*acquire, 5) && !bytes.enqueue(*release, 1) && bytes.queuedCount() == 1 &&
                              bytes.queuedCommandBytes() == 5 && byteHarness.failures.size() == 1,
                          "byte boundary plus one is rejected with checked accounting and no oldest-entry eviction");

        sdk_client::Client zeroSdk(options());
        app::CommandDrainController zero(zeroSdk, {}, {.maximumCommands = 0, .maximumCommandBytes = 0});
        result.expectTrue(!zero.enqueue(*acquire, 0) && zero.queuedCount() == 0 && zero.queuedCommandBytes() == 0,
                          "zero queue limits mean zero retained capacity rather than unlimited capacity");

        sdk_client::Client zeroCountSdk(options());
        app::CommandDrainController zeroCount(zeroCountSdk, {}, {.maximumCommands = 0, .maximumCommandBytes = 5});
        sdk_client::Client zeroBytesSdk(options());
        app::CommandDrainController zeroBytes(zeroBytesSdk, {}, {.maximumCommands = 1, .maximumCommandBytes = 0});
        result.expectTrue(!zeroCount.enqueue(*acquire, 0) && !zeroBytes.enqueue(*acquire, 0) && zeroCount.queuedCount() == 0 &&
                              zeroBytes.queuedCount() == 0 && zeroCount.queuedCommandBytes() == 0 && zeroBytes.queuedCommandBytes() == 0,
                          "zero command-count and zero retained-byte limits independently mean zero queue capacity");

        sdk_client::Client overflowSdk(options());
        app::CommandDrainController overflow(
            overflowSdk, {}, {.maximumCommands = 2, .maximumCommandBytes = std::numeric_limits<std::size_t>::max()});
        result.expectTrue(overflow.enqueue(*acquire, std::numeric_limits<std::size_t>::max()) && !overflow.enqueue(*release, 1) &&
                              overflow.queuedCount() == 1 && overflow.queuedCommandBytes() == std::numeric_limits<std::size_t>::max(),
                          "retained-byte addition overflow fails closed without corrupting queue accounting");

        const app::ParsedCommand largeNewParsed = parser.parse("new -- a deliberately large retained compound workflow");
        const auto* largeNew = std::get_if<app::NewCommand>(&largeNewParsed);
        sdk_client::Client largeNewSdk(options());
        app::CommandDrainController largeNewQueue(largeNewSdk, {}, {.maximumCommands = 1, .maximumCommandBytes = 64U * 1024U});
        result.expectTrue(largeNew != nullptr && largeNewQueue.enqueue(*largeNew, 64U * 1024U) && largeNewQueue.queuedCount() == 1 &&
                              largeNewQueue.queuedCommandBytes() == 64U * 1024U && !largeNewQueue.enqueue(*largeNew, 1) &&
                              largeNewQueue.queuedCount() == 1 && largeNewQueue.queuedCommandBytes() == 64U * 1024U,
                          "one large queued new consumes its exact retained-input budget and rejects only the newest overflow entry");

        Harness deferredHarness;
        sdk_client::ClientOptions deferredOptions = options();
        deferredOptions.maximumPendingOperations = 1;
        app::CommandDrainController* deferredController = nullptr;
        sdk_client::ClientCallbacks deferredCallbacks;
        deferredCallbacks.onConnectionStateChanged = [&deferredController](const sdk_client::ConnectionStateChange& change) {
            if (deferredController != nullptr) {
                deferredController->connectionStateChanged(change);
            }
        };
        sdk_client::Client deferredSdk(std::move(deferredOptions), std::move(deferredCallbacks));
        app::CommandDrainController deferred(deferredSdk);
        deferredController = &deferred;
        sdk_client::Connection deferredConnection = deferredSdk.openConnection(deferredHarness.transport());
        makeReady(deferredConnection);
        result.expectTrue(deferred.enqueue(*acquire) && deferred.enqueue(*release, 7) && deferred.queuedCount() == 1 &&
                              deferredSdk.pendingOperationCount() == 1,
                          "TooManyPendingOperations defers only the not-yet-accepted command in the bounded local queue");
        const std::optional<frontend::generated::DefinedCommand> first = lastCommand(deferredHarness);
        if (first) {
            (void) deferredConnection.receive(
                frontend::ServerMessage{frontend::Response::success(first->requestId, frontend::Json{{"role", "controller"}})});
        }
        const std::optional<frontend::generated::DefinedCommand> second = lastCommand(deferredHarness);
        result.expectTrue(first && second && first->requestId != second->requestId && deferred.queuedCount() == 0 &&
                              deferred.queuedCommandBytes() == 0 && deferredSdk.pendingOperationCount() == 1,
                          "a terminal callback releases SDK capacity and flushes the deferred first submission exactly once");
    }

    void testApplicationShutdownClearsQueueAccounting(tests::support::TestResult& result) {
        Harness harness;
        sdk_client::Client sdk(options());
        app::CommandDrainController drain(sdk,
                                          {.requestExit =
                                               [&harness] {
                                                   ++harness.exits;
                                               },
                                           .reportFailure =
                                               [&harness](std::string message) {
                                                   harness.failures.push_back(std::move(message));
                                               },
                                           .requestReconnect = {}});
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        app::CommandParser parser;
        app::ParsedCommand acquireParsed;
        const app::RemoteCommand* acquire = remoteCommand(parser, "acquire", acquireParsed);
        const app::ParsedCommand compoundParsed = parser.parse("new -- queued shutdown prompt");
        const auto* compound = std::get_if<app::NewCommand>(&compoundParsed);
        result.expectTrue(acquire != nullptr && compound != nullptr && drain.enqueue(*acquire, 7) && drain.enqueue(*compound, 26) &&
                              drain.queuedCount() == 2 && drain.queuedCommandBytes() == 33,
                          "shutdown fixture retains bounded commands while the connection is not Ready");

        drain.localShutdownRequested();
        result.expectTrue(drain.queuedCount() == 0 && drain.queuedCommandBytes() == 0 &&
                              drain.newStage() == app::CommandDrainController::NewStage::None &&
                              drain.outcome() == app::CommandDrainController::Outcome::Success && drain.applicationShutdownActive() &&
                              harness.exits == 1 && harness.failures.empty(),
                          "intentional shutdown releases all queued command accounting without reporting queued work as failure");
    }

    void testBatchDrainAccumulatesFailures(tests::support::TestResult& result) {
        Harness harness;
        sdk_client::ClientOptions limitedOptions = options();
        limitedOptions.maximumPendingOperations = 1;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
        };
        sdk_client::Client sdk(std::move(limitedOptions), std::move(callbacks));
        app::CommandDrainController drain(sdk,
                                          {.requestExit =
                                               [&harness] {
                                                   ++harness.exits;
                                               },
                                           .reportFailure =
                                               [&harness](std::string message) {
                                                   harness.failures.push_back(std::move(message));
                                               },
                                           .requestReconnect = {}});
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        app::ParsedCommand firstParsed;
        app::ParsedCommand secondParsed;
        const app::RemoteCommand* first = remoteCommand(parser, "acquire", firstParsed);
        const app::RemoteCommand* second = remoteCommand(parser, "release", secondParsed);
        result.expectTrue(first != nullptr && second != nullptr && drain.enqueue(*first) && drain.enqueue(*second, 7),
                          "batch commands are accepted or retained in input order");
        drain.localCommandFailed("unknown command: broken-line");
        drain.inputEof();
        const std::optional<frontend::generated::DefinedCommand> firstWire = lastCommand(harness);
        if (!firstWire) {
            return;
        }
        (void) connection.receive(
            frontend::ServerMessage{commandFailure(*firstWire, frontend::ErrorCode::Conflict, "ordinary batch command failed")});
        const std::optional<frontend::generated::DefinedCommand> secondWire = lastCommand(harness);
        result.expectTrue(secondWire && secondWire->requestId != firstWire->requestId && harness.exits == 0 &&
                              drain.outcome() == app::CommandDrainController::Outcome::Running,
                          "EOF drain continues after both a parser error and Response(ok=false)");
        if (secondWire) {
            (void) connection.receive(
                frontend::ServerMessage{frontend::Response::success(secondWire->requestId, frontend::Json{{"role", "observer"}})});
        }
        result.expectTrue(harness.exits == 1 && drain.outcome() == app::CommandDrainController::Outcome::Failure &&
                              sdk.pendingOperationCount() == 0 && drain.queuedCount() == 0,
                          "batch EOF exits nonzero once only after all queued and accepted work reaches a terminal callback");
    }

    void testConnectionLossCompletesEofDrain(tests::support::TestResult& result) {
        Harness harness;
        sdk_client::ClientOptions limitedOptions = options();
        limitedOptions.maximumPendingOperations = 1;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
        };
        sdk_client::Client sdk(std::move(limitedOptions), std::move(callbacks));
        app::CommandDrainController drain(sdk,
                                          {.requestExit =
                                               [&harness] {
                                                   ++harness.exits;
                                               },
                                           .reportFailure =
                                               [&harness](std::string message) {
                                                   harness.failures.push_back(std::move(message));
                                               },
                                           .requestReconnect = {}});
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        app::ParsedCommand firstParsed;
        app::ParsedCommand queuedParsed;
        const app::RemoteCommand* first = remoteCommand(parser, "acquire", firstParsed);
        const app::RemoteCommand* queued = remoteCommand(parser, "release", queuedParsed);
        result.expectTrue(first != nullptr && queued != nullptr && drain.enqueue(*first) && drain.enqueue(*queued, 7) &&
                              sdk.pendingOperationCount() == 1 && drain.queuedCount() == 1,
                          "EOF connection-loss fixture has one SDK-accepted and one locally queued command");
        const std::size_t outboundBeforeLoss = harness.outbound.size();
        drain.inputEof();
        connection.transportDisconnected(sdk_client::TransportError{"transport lost during batch drain", true});
        drain.disconnected();

        result.expectTrue(
            drain.outcome() == app::CommandDrainController::Outcome::Failure && harness.exits == 1 && drain.queuedCount() == 0 &&
                drain.queuedCommandBytes() == 0 && sdk.pendingOperationCount() == 0 && harness.outbound.size() == outboundBeforeLoss &&
                drain.applicationShutdownActive(),
            "connection loss fails accepted work once, discards unsubmitted work, and finishes EOF drain nonzero without retry");
    }

    void testFatalProtocolFailureCompletesActiveNewEofDrain(tests::support::TestResult& result) {
        Harness harness;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
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
                                               },
                                           .requestReconnect = {}});
        controller = &drain;
        sdk_client::Connection connection = sdk.openConnection(harness.transport());
        makeReady(connection);

        app::CommandParser parser;
        const app::ParsedCommand parsed = parser.parse("new -- active during fatal protocol failure");
        const auto* compound = std::get_if<app::NewCommand>(&parsed);
        result.expectTrue(compound != nullptr && drain.enqueue(*compound) &&
                              drain.newStage() == app::CommandDrainController::NewStage::AwaitingThreadStartResponse &&
                              sdk.pendingOperationCount() == 1,
                          "fatal-protocol EOF fixture has one accepted active new workflow");

        drain.inputEof();
        const sdk_client::ReceiveResult received = connection.receive(frontend::ServerMessage{
            frontend::Response::success("unsolicited-request", frontend::Json{{"threadId", "must-not-correlate"}})});
        connection.transportDisconnected();
        drain.disconnected();

        result.expectTrue(!received.accepted && drain.outcome() == app::CommandDrainController::Outcome::Failure && harness.exits == 1 &&
                              sdk.pendingOperationCount() == 0 && drain.newStage() == app::CommandDrainController::NewStage::None &&
                              drain.queuedCount() == 0 && drain.applicationShutdownActive(),
                          "fatal protocol closure clears active new, releases its callback, and completes EOF drain once without hanging");
    }

    void testDisconnectedAndManualReconnect(tests::support::TestResult& result) {
        Harness harness;
        std::size_t reconnectRequests = 0;
        bool rejectNextReconnect = true;
        app::CommandDrainController* controller = nullptr;
        sdk_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
            if (controller != nullptr) {
                controller->connectionStateChanged(change);
            }
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
                                               },
                                           .requestReconnect = [&reconnectRequests, &rejectNextReconnect]() -> std::optional<std::string> {
                                               ++reconnectRequests;
                                               if (rejectNextReconnect) {
                                                   rejectNextReconnect = false;
                                                   return "the previous frontend transport is still detaching";
                                               }
                                               return std::nullopt;
                                           }});
        controller = &drain;
        sdk_client::Connection firstConnection = sdk.openConnection(harness.transport());
        makeReady(firstConnection);
        const std::uint64_t firstGeneration = firstConnection.generation();
        firstConnection.transportDisconnected(sdk_client::TransportError{"test transport lost", true});
        drain.disconnected();
        result.expectTrue(drain.sessionState() == app::CommandDrainController::SessionState::Disconnected &&
                              drain.outcome() == app::CommandDrainController::Outcome::Running &&
                              drain.inputState() == app::CommandDrainController::InputState::Reading && harness.exits == 0,
                          "physical transport loss ends only the connection and leaves the application interactive");

        app::CommandParser parser;
        app::ParsedCommand disconnectedParsed;
        const app::RemoteCommand* disconnectedCommand = remoteCommand(parser, "acquire", disconnectedParsed);
        result.expectTrue(disconnectedCommand != nullptr && !drain.enqueue(*disconnectedCommand, 9) && drain.queuedCount() == 0,
                          "Disconnected rejects remote commands with guidance instead of retaining surprise work");
        const std::size_t failuresBeforeGateRejection = harness.failures.size();
        result.expectTrue(!drain.reconnect() && reconnectRequests == 1 &&
                              drain.sessionState() == app::CommandDrainController::SessionState::Disconnected &&
                              harness.failures.size() == failuresBeforeGateRejection + 1 &&
                              harness.failures.back() == "the previous frontend transport is still detaching",
                          "a physical-attempt gate rejection restores Disconnected and reports its one exact safe reason");
        result.expectTrue(drain.reconnect() && reconnectRequests == 2 &&
                              drain.sessionState() == app::CommandDrainController::SessionState::Connecting,
                          "the local reconnect command starts exactly one application-owned physical attempt");
        result.expectTrue(!drain.reconnect() && reconnectRequests == 2,
                          "reconnect while Connecting reports the active attempt without creating overlap");

        result.expectTrue(drain.enqueue(*disconnectedCommand, 9) && drain.queuedCount() == 1,
                          "commands entered during a specific connection attempt use its bounded pre-Ready queue");
        drain.connectionAttemptFailed("first explicit reconnect attempt failed");
        result.expectTrue(drain.sessionState() == app::CommandDrainController::SessionState::Disconnected &&
                              drain.outcome() == app::CommandDrainController::Outcome::Running && drain.queuedCount() == 0 &&
                              drain.queuedCommandBytes() == 0 && drain.reconnect() && reconnectRequests == 3,
                          "a failed attempt discards its unsubmitted queue and allows another user-requested attempt");

        sdk_client::Connection secondConnection = sdk.openConnection(harness.transport());
        makeReady(secondConnection);
        result.expectTrue(secondConnection.generation() > firstGeneration && sdk.isReady() &&
                              drain.sessionState() == app::CommandDrainController::SessionState::Ready && !drain.reconnect() &&
                              reconnectRequests == 3 && harness.exits == 0,
                          "a new SDK Connection generation reaches Ready and Ready reconnect is a harmless local rejection");
        app::ParsedCommand finalParsed;
        const app::RemoteCommand* finalCommand = remoteCommand(parser, "acquire", finalParsed);
        result.expectTrue(finalCommand != nullptr && drain.enqueue(*finalCommand),
                          "new commands can be submitted after explicit reconnect without replaying old work");
    }

    void testConcreteFailureWins(tests::support::TestResult& result) {
        Harness harness;
        sdk_client::Client sdk(options());
        app::CommandDrainController drain(sdk,
                                          {.requestExit =
                                               [&harness] {
                                                   ++harness.exits;
                                               },
                                           .reportFailure =
                                               [&harness](std::string message) {
                                                   harness.failures.push_back(std::move(message));
                                               },
                                           .requestReconnect = {}});

        sdk_client::Error error;
        error.origin = sdk_client::ErrorOrigin::Protocol;
        error.clientCode = sdk_client::ClientErrorCode::UnexpectedMessage;
        error.message = "synchronization message outside synchronization";
        drain.connectionStateChanged(
            sdk_client::ConnectionStateChange{sdk_client::ConnectionState::Ready, sdk_client::ConnectionState::Closing, error});
        drain.connectionFailed("frontend synchronization protocol violation");
        drain.disconnected();

        result.expectTrue(
            !drain.failed() && drain.outcome() == app::CommandDrainController::Outcome::Running && harness.exits == 0 &&
                drain.sessionState() == app::CommandDrainController::SessionState::Disconnected && harness.failures.size() == 1 &&
                harness.failures.front() == "frontend SDK protocol failure: synchronization message outside synchronization" &&
                drain.failureReason() == harness.failures.front(),
            "the first concrete SDK error is preserved while only the failed physical connection becomes terminal");
    }

    void testIntentionalAndRemoteDisconnectClassification(tests::support::TestResult& result) {
        Harness connectingHarness;
        sdk_client::Client connectingSdk(options());
        app::CommandDrainController connecting(connectingSdk,
                                               {.requestExit =
                                                    [&connectingHarness] {
                                                        ++connectingHarness.exits;
                                                    },
                                                .reportFailure =
                                                    [&connectingHarness](std::string message) {
                                                        connectingHarness.failures.push_back(std::move(message));
                                                    },
                                                .requestReconnect = {}});
        connecting.localShutdownRequested();
        connecting.connectionFailed("frontend WebSocket upgrade was rejected");
        connecting.disconnected();
        connecting.quit();
        result.expectTrue(connecting.outcome() == app::CommandDrainController::Outcome::Success && connectingHarness.failures.empty() &&
                              connectingHarness.exits == 1,
                          "local shutdown before transport attachment suppresses a later connect or HTTP-upgrade failure");

        Harness intentionalHarness;
        sdk_client::Client intentionalSdk(options());
        app::CommandDrainController intentional(intentionalSdk,
                                                {.requestExit =
                                                     [&intentionalHarness] {
                                                         ++intentionalHarness.exits;
                                                     },
                                                 .reportFailure =
                                                     [&intentionalHarness](std::string message) {
                                                         intentionalHarness.failures.push_back(std::move(message));
                                                     },
                                                 .requestReconnect = {}});
        intentional.localShutdownRequested();
        sdk_client::Error transportError;
        transportError.origin = sdk_client::ErrorOrigin::Transport;
        transportError.clientCode = sdk_client::ClientErrorCode::TransportFailure;
        transportError.message = "physical frontend transport disconnected";
        intentional.connectionStateChanged(sdk_client::ConnectionStateChange{
            sdk_client::ConnectionState::Ready, sdk_client::ConnectionState::Disconnected, transportError});
        intentional.disconnected();
        intentional.quit();
        result.expectTrue(intentional.outcome() == app::CommandDrainController::Outcome::Success &&
                              intentional.sessionState() == app::CommandDrainController::SessionState::Closed &&
                              intentionalHarness.failures.empty() && intentionalHarness.exits == 1,
                          "local signal/shutdown classification suppresses the generic unexpected-disconnect failure");

        Harness quitHarness;
        sdk_client::Client quitSdk(options());
        app::CommandDrainController quit(quitSdk,
                                         {.requestExit =
                                              [&quitHarness] {
                                                  ++quitHarness.exits;
                                              },
                                          .reportFailure =
                                              [&quitHarness](std::string message) {
                                                  quitHarness.failures.push_back(std::move(message));
                                              },
                                          .requestReconnect = {}});
        quit.quit();
        quit.disconnected();
        result.expectTrue(quit.outcome() == app::CommandDrainController::Outcome::Success && quitHarness.failures.empty() &&
                              quitHarness.exits == 1,
                          "quit remains a clean intentional shutdown without a duplicate disconnect failure");

        Harness remoteHarness;
        sdk_client::Client remoteSdk(options());
        app::CommandDrainController remote(remoteSdk,
                                           {.requestExit =
                                                [&remoteHarness] {
                                                    ++remoteHarness.exits;
                                                },
                                            .reportFailure =
                                                [&remoteHarness](std::string message) {
                                                    remoteHarness.failures.push_back(std::move(message));
                                                },
                                            .requestReconnect = {}});
        remote.disconnected();
        remote.disconnected();
        result.expectTrue(!remote.failed() && remote.outcome() == app::CommandDrainController::Outcome::Running &&
                              remote.sessionState() == app::CommandDrainController::SessionState::Disconnected &&
                              remote.inputState() == app::CommandDrainController::InputState::Reading && remoteHarness.exits == 0 &&
                              remoteHarness.failures.size() == 1 &&
                              remoteHarness.failures.front() == "frontend connection closed unexpectedly",
                          "remote EOF reports one connection failure while the interactive application remains alive");
    }

    void testTransportCloseReasonPreserved(tests::support::TestResult& result) {
        sdk_client::Client sdk(options());
        std::vector<std::string> failures;
        app::ClientConnection connection(sdk,
                                         {.onConnected = {},
                                          .onDisconnected = {},
                                          .onFailure =
                                              [&failures](std::string message) {
                                                  failures.push_back(std::move(message));
                                              },
                                          .onOutbound = {},
                                          .verifiedLocalUnix = false,
                                          .onBeforeTransportConnected = {},
                                          .onLocalShutdown = {}});
        app::ClientConnectionTestAccess::closeTransport(connection, "frontend synchronization protocol violation");
        app::ClientConnectionTestAccess::closeTransport(connection, "later generic transport close");
        result.expectTrue(failures.size() == 1 && failures.front() == "frontend synchronization protocol violation",
                          "the native adapter preserves the first SDK transport-close reason and suppresses duplicates");
    }

    void testIntentionalShutdownCompletesPendingWorkSilently(tests::support::TestResult& result) {
        const auto exercise = [&result](std::string_view commandLine, bool advanceNewToTurn, std::string_view description) {
            Harness harness;
            app::CommandDrainController* controller = nullptr;
            sdk_client::ClientCallbacks callbacks;
            callbacks.onConnectionStateChanged = [&controller](const sdk_client::ConnectionStateChange& change) {
                if (controller != nullptr) {
                    controller->connectionStateChanged(change);
                }
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
                                                   },
                                               .requestReconnect = {}});
            controller = &drain;
            app::ClientConnection shutdownAdapter(sdk,
                                                  {.onConnected = {},
                                                   .onDisconnected = {},
                                                   .onFailure = {},
                                                   .onOutbound = {},
                                                   .verifiedLocalUnix = false,
                                                   .onBeforeTransportConnected = {},
                                                   .onLocalShutdown = [&drain] {
                                                       drain.localShutdownRequested();
                                                   }});
            sdk_client::Connection connection = sdk.openConnection(harness.transport());
            makeReady(connection);

            app::CommandParser parser;
            const app::ParsedCommand parsed = parser.parse(commandLine);
            bool accepted = false;
            if (const auto* remote = std::get_if<app::RemoteCommand>(&parsed)) {
                accepted = drain.enqueue(*remote);
                if (commandLine == "acquire") {
                    drain.inputEof();
                }
            } else if (const auto* compound = std::get_if<app::NewCommand>(&parsed)) {
                accepted = drain.enqueue(*compound);
            }
            if (advanceNewToTurn) {
                const std::optional<frontend::generated::DefinedCommand> threadStart = lastCommand(harness);
                if (threadStart) {
                    (void) connection.receive(frontend::ServerMessage{
                        frontend::Response::success(threadStart->requestId, frontend::Json{{"threadId", "signal-thread"}})});
                }
            }
            const bool pendingBeforeShutdown = sdk.pendingOperationCount() == 1;
            shutdownAdapter.shutdown();
            connection.transportDisconnected();
            drain.quit();

            result.expectTrue(accepted && pendingBeforeShutdown && sdk.pendingOperationCount() == 0 && harness.failures.empty() &&
                                  drain.outcome() == app::CommandDrainController::Outcome::Success && harness.exits == 1,
                              std::string(description));
        };

        exercise("acquire", false, "local shutdown silently completes an ordinary operation already draining after EOF");
        exercise("snapshot", false, "local shutdown silently completes a pending explicit synchronization operation");
        exercise("new --cwd /work -- signal", false, "local shutdown silently completes a pending new thread.start operation");
        exercise("new --cwd /work -- signal", true, "local shutdown silently completes a pending new turn.start operation");
    }

    void testSafeFailureDiagnostics(tests::support::TestResult& result) {
        Harness remoteHarness;
        sdk_client::Client remoteSdk(options());
        app::CommandDrainController remote(remoteSdk,
                                           {.requestExit = {},
                                            .reportFailure =
                                                [&remoteHarness](std::string message) {
                                                    remoteHarness.failures.push_back(std::move(message));
                                                },
                                            .requestReconnect = {}});
        sdk_client::Error remoteError;
        remoteError.origin = sdk_client::ErrorOrigin::Protocol;
        remoteError.protocolCode = frontend::ErrorCode::InvalidField;
        remoteError.message = "remote text must not be mirrored\nAuthorization: Bearer not-a-real-token";
        remote.connectionStateChanged(
            sdk_client::ConnectionStateChange{sdk_client::ConnectionState::Ready, sdk_client::ConnectionState::Closing, remoteError});
        result.expectTrue(remoteHarness.failures.size() == 1 &&
                              remoteHarness.failures.front() == "frontend SDK protocol failure: server reported invalid_field" &&
                              remoteHarness.failures.front().find("Bearer") == std::string::npos,
                          "remote protocol-error text is replaced by its stable code instead of reaching diagnostics");

        Harness boundedHarness;
        sdk_client::Client boundedSdk(options());
        app::CommandDrainController bounded(boundedSdk,
                                            {.requestExit = {},
                                             .reportFailure =
                                                 [&boundedHarness](std::string message) {
                                                     boundedHarness.failures.push_back(std::move(message));
                                                 },
                                             .requestReconnect = {}});
        std::string unsafe = "transport failure\n";
        unsafe.append(300, 'x');
        bounded.connectionFailed(std::move(unsafe));
        result.expectTrue(boundedHarness.failures.size() == 1 && boundedHarness.failures.front().size() == 243 &&
                              boundedHarness.failures.front().find('\n') == std::string::npos &&
                              boundedHarness.failures.front().ends_with("..."),
                          "application-local close reasons are flattened and bounded before human or JSON presentation");

        std::string serverReason = "service close\r\n";
        serverReason.append(300, 'y');
        const std::string safeServerReason = apps::codex_backend::safeFrontendCloseReason(serverReason);
        result.expectTrue(safeServerReason.size() == 243 && safeServerReason.find('\n') == std::string::npos &&
                              safeServerReason.find('\r') == std::string::npos && safeServerReason.ends_with("..."),
                          "the native server transport uses the same bounded single-record close-reason discipline");
    }

    void testJsonFailurePresentation(tests::support::TestResult& result) {
        std::ostringstream protocolOutput;
        std::ostringstream diagnostics;
        app::Presenter presenter(app::OutputMode::Json, protocolOutput, diagnostics);
        const frontend::ServerMessage observed{frontend::SyncComplete{frontend::SequenceNumber{11}}};
        presenter.present(observed);
        const std::string protocolBeforeFailure = protocolOutput.str();

        sdk_client::Client sdk(options());
        app::CommandDrainController drain(sdk,
                                          {.requestExit = {},
                                           .reportFailure =
                                               [&presenter](std::string message) {
                                                   presenter.error(message);
                                               },
                                           .requestReconnect = {}});
        sdk_client::Error error;
        error.origin = sdk_client::ErrorOrigin::Protocol;
        error.clientCode = sdk_client::ClientErrorCode::UnexpectedMessage;
        error.message = "synchronization message outside synchronization";
        drain.connectionStateChanged(
            sdk_client::ConnectionStateChange{sdk_client::ConnectionState::Ready, sdk_client::ConnectionState::Closing, error});
        presenter.disconnected();

        std::string encoded = protocolBeforeFailure;
        if (!encoded.empty() && encoded.back() == '\n') {
            encoded.pop_back();
        }
        const auto decoded = frontend::Codec::decodeServer(std::string_view(encoded));
        result.expectTrue(decoded && protocolOutput.str() == protocolBeforeFailure &&
                              std::holds_alternative<frontend::SyncComplete>(decoded.value()),
                          "JSON-mode failure presentation leaves stdout as the unchanged valid Frontend Protocol record stream");
        result.expectTrue(diagnostics.str() ==
                              "codex-backend-client: frontend SDK protocol failure: synchronization message outside synchronization\n"
                              "disconnected\n",
                          "the safe concrete SDK failure and lifecycle message use the JSON mode diagnostics stream");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testQueueAndEofDrain(result);
    testControllerTransitionsOrderQueuedCommands(result);
    testNewRemainsTwoTypedOperations(result);
    testCommandFailureRemainsInteractive(result);
    testCommandFailureDispositionMatrix(result);
    testLocalErrorsRemainInteractive(result);
    testTwoSuccessfulNewWorkflowsPreserveOrder(result);
    testQueuedNewWorkflowsRemainIndependent(result);
    testNewStageTwoLocalRejection(result);
    testTurnFailureThenNextNewSucceeds(result);
    testDisconnectDuringActiveNew(result);
    testCorrelatedNonClosingProtocolError(result);
    testExplicitSynchronizationDrain(result);
    testRejectedSynchronizationReturnsToReady(result);
    testBoundedQueueAndTemporaryDeferral(result);
    testApplicationShutdownClearsQueueAccounting(result);
    testBatchDrainAccumulatesFailures(result);
    testConnectionLossCompletesEofDrain(result);
    testFatalProtocolFailureCompletesActiveNewEofDrain(result);
    testDisconnectedAndManualReconnect(result);
    testConcreteFailureWins(result);
    testIntentionalAndRemoteDisconnectClassification(result);
    testTransportCloseReasonPreserved(result);
    testIntentionalShutdownCompletesPendingWorkSilently(result);
    testSafeFailureDiagnostics(result);
    testJsonFailurePresentation(result);
    return result.processResult();
}
