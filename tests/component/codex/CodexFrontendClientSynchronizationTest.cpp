/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "ai/openai/codex/frontend/client/ProjectionFingerprint.h"
#include "ai/openai/codex/frontend/client/Synchronization.h"
#include "ai/openai/codex/frontend/client/detail/StateReducer.h"
#include "support/TestResult.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace client = ai::openai::codex::frontend::client;
    namespace typed = ai::openai::codex::typed;

    static_assert(std::is_same_v<decltype(client::AccountState{}.details), client::AccountDetailsState>);
    static_assert(std::is_same_v<decltype(client::ConfigurationState{}.details), client::ConfigurationDetailsState>);
    static_assert(std::is_same_v<decltype(client::AppsState{}.details), client::IntegrationDetailsState>);
    static_assert(std::is_same_v<decltype(client::PluginsState{}.details), client::PluginsAndSkillsDetailsState>);
    static_assert(std::is_same_v<decltype(client::McpState{}.details), client::McpDetailsState>);
    static_assert(std::is_same_v<decltype(client::PlatformState{}.details), client::PlatformDetailsState>);
    static_assert(std::is_same_v<decltype(client::ProviderState{}.lastError), std::optional<client::ProviderErrorState>>);
    static_assert(std::is_same_v<decltype(client::ProviderState{}.initialization), std::optional<client::ProviderInitializationState>>);
    static_assert(std::is_same_v<decltype(client::ThreadState{}.realtime), std::optional<client::ThreadRealtimeState>>);
    static_assert(std::is_same_v<decltype(client::ThreadRealtimeState{}.version), std::optional<typed::RealtimeConversationVersion>>);
    static_assert(std::is_same_v<decltype(client::TurnState{}.status), typed::TurnStatus>);
    static_assert(std::is_same_v<decltype(client::AccountDetailsState{}.authMode), std::optional<typed::AuthMode>>);
    static_assert(std::is_same_v<decltype(client::AccountDetailsState{}.planType), std::optional<typed::PlanType>>);
    static_assert(std::is_same_v<decltype(client::ConfigurationDetailsState{}.writeStatus), std::optional<typed::WriteStatus>>);
    static_assert(std::is_same_v<decltype(client::McpDetailsState{}.startupStatus), std::optional<typed::McpServerStartupState>>);
    static_assert(
        std::is_same_v<decltype(client::PlatformDetailsState{}.remoteControlStatus), std::optional<typed::RemoteControlConnectionStatus>>);
    static_assert(std::is_same_v<decltype(client::ItemContentReplacedChange{}.channel), client::ItemContentChannel>);
    static_assert(client::toString(client::ItemContentChannel::AgentText) == "agentText");
    static_assert(client::toString(client::ItemContentChannel::ReasoningText) == "reasoningText");
    static_assert(client::toString(client::ItemContentChannel::ReasoningSummary) == "reasoningSummary");
    static_assert(client::toString(client::ItemContentChannel::CommandOutput) == "commandOutput");

    struct Harness {
        std::vector<client::OutboundMessage> messages;
        std::vector<client::StateUpdate> updates;
        std::vector<frontend::SequenceNumber> cursors;
        std::size_t synchronized = 0;
        bool lastSnapshotFallback = false;
        std::size_t closes = 0;

        client::TransportCallbacks transport() {
            return {
                [this](client::OutboundMessage message) {
                    messages.push_back(std::move(message));
                    return client::SendResult{client::SendStatus::Accepted, std::nullopt};
                },
                [this](std::string) {
                    ++closes;
                },
            };
        }

        client::ClientCallbacks callbacks() {
            client::ClientCallbacks result;
            result.onStateUpdated = [this](const client::StateUpdate& update) {
                updates.push_back(update);
            };
            result.onCursorAdvanced = [this](frontend::SequenceNumber sequence) {
                cursors.push_back(sequence);
            };
            result.onSynchronized = [this](const client::SynchronizationInfo& info) {
                ++synchronized;
                lastSnapshotFallback = info.snapshotFallback;
            };
            return result;
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
        const std::vector<frontend::FrontendCapability> implemented{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
            frontend::FrontendCapability::ConditionalFilesystem,
            frontend::FrontendCapability::CppClientSdk,
        };
        return {implemented, implemented, implemented, frontend::Json::object()};
    }

    std::vector<frontend::FrontendMethod> discoveredMethods() {
        std::vector<frontend::FrontendMethod> methods;
        for (const generated::MethodMetadata& metadata : generated::AllMethods) {
            if (metadata.currentlyImplemented && metadata.defaultEnabled) {
                methods.emplace_back(metadata.method);
            }
        }
        return methods;
    }

    frontend::Welcome welcome(frontend::SequenceNumber sequence,
                              frontend::SyncMode mode,
                              frontend::Json extensions = frontend::Json::object(),
                              std::string session = "1",
                              bool includeContinuityMetadata = true) {
        std::optional<std::vector<frontend::FrontendMethod>> available;
        std::optional<std::vector<frontend::FrontendMethod>> permitted;
        if (includeContinuityMetadata) {
            if (!extensions.contains("permittedScopes")) {
                frontend::Json scopes = frontend::Json::array();
                scopes.push_back("observe");
                extensions["permittedScopes"] = std::move(scopes);
            }
            available = discoveredMethods();
            permitted = available;
        }
        return {std::move(session),
                frontend::SessionRole::Observer,
                sequence,
                mode,
                std::move(extensions),
                capabilities(),
                std::move(available),
                std::move(permitted)};
    }

    frontend::Json legacyState(std::string output = "initial") {
        frontend::Json item{{"id", "item-1"},
                            {"type", "command_execution"},
                            {"status", "completed"},
                            {"agentText", "agent"},
                            {"reasoningText", "reasoning"},
                            {"reasoningSummary", "summary"},
                            {"commandOutput", std::move(output)},
                            {"droppedContentBytes", std::uint64_t{3}},
                            {"contentTruncated", true},
                            {"startedAtMs", std::int64_t{10}},
                            {"completedAtMs", std::int64_t{20}},
                            {"data", frontend::Json{{"exitCode", 0}}},
                            {"extensions", frontend::Json{{"safeExtension", "retained"}}}};
        frontend::Json turn{{"id", "turn-1"},
                            {"threadId", "thread-1"},
                            {"status", "completed"},
                            {"active", false},
                            {"terminal", true},
                            {"items", frontend::Json::array({std::move(item)})},
                            {"extensions", frontend::Json::object()}};
        frontend::Json thread{{"id", "thread-1"},
                              {"fullyLoaded", true},
                              {"turns", frontend::Json::array({std::move(turn)})},
                              {"extensions", frontend::Json::object()}};
        return {{"backendRevision", std::uint64_t{7}},
                {"lifecycle", "ready"},
                {"diagnostics", {{"received", std::uint64_t{0}}, {"recent", frontend::Json::array()}}},
                {"controllerSessionId", "7"},
                {"sessions", frontend::Json::array()},
                {"threadList",
                 {{"hasLoadedPage", true}, {"complete", false}, {"pagesLoaded", std::uint64_t{2}}, {"nextCursor", "cursor-next"}}},
                {"threads", frontend::Json::array({std::move(thread)})},
                {"pendingRequests", frontend::Json::array()},
                {"codexExtensions", frontend::Json::array()},
                {"omittedCodexExtensions", std::uint64_t{0}},
                {"journal", {{"oldestReplayableAfter", std::uint64_t{0}}, {"currentSequence", std::uint64_t{7}}}},
                {"sequenceExhausted", false}};
    }

    frontend::Json expandedState(std::string output = "initial");

    void connectAndSnapshot(client::Client& sdk,
                            client::Connection& connection,
                            frontend::SequenceNumber sequence,
                            frontend::Json extensions = frontend::Json::object()) {
        connection.transportConnected();
        (void) connection.receive(frontend::ServerMessage{welcome(sequence, frontend::SyncMode::Snapshot, std::move(extensions))});
        (void) connection.receive(frontend::ServerMessage{frontend::Snapshot{sequence, expandedState()}});
        (void) connection.receive(frontend::ServerMessage{frontend::SyncComplete{sequence}});
        (void) sdk;
    }

    frontend::FrontendEvent content(frontend::SequenceNumber sequence, std::string value) {
        return {sequence,
                "item.content.updated",
                frontend::Json{{"threadId", "thread-1"},
                               {"turnId", "turn-1"},
                               {"itemId", "item-1"},
                               {"channel", "commandOutput"},
                               {"content", std::move(value)},
                               {"contentTruncated", false},
                               {"droppedContentBytes", std::uint64_t{0}}}};
    }

    frontend::Json expandedState(std::string output) {
        return {{"provider",
                 {{"lifecycle", "ready"},
                  {"generation", std::uint64_t{1}},
                  {"desiredRunning", true},
                  {"recovery", {{"status", "idle"}, {"attempts", std::uint64_t{0}}}}}},
                {"controller", frontend::Json::object()},
                {"sessions", frontend::Json::array()},
                {"threadList",
                 {{"hasLoadedPage", true},
                  {"complete", false},
                  {"pagesLoaded", std::uint64_t{1}},
                  {"nextCursor", "cursor-next"},
                  {"stamp", {{"generation", std::uint64_t{1}}, {"freshness", "current"}}}}},
                {"threads", frontend::Json::array({frontend::Json{{"id", "thread-1"}, {"fullyLoaded", true}}})},
                {"turns",
                 frontend::Json::array({frontend::Json{
                     {"id", "turn-1"}, {"threadId", "thread-1"}, {"status", "completed"}, {"active", false}, {"terminal", true}}})},
                {"items",
                 frontend::Json::array({frontend::Json{{"id", "item-1"},
                                                       {"threadId", "thread-1"},
                                                       {"turnId", "turn-1"},
                                                       {"type", "commandExecution"},
                                                       {"agentText", "initial-agent"},
                                                       {"commandOutput", std::move(output)}}})},
                {"pendingRequests", frontend::Json::array()},
                {"capacity", frontend::Json::object()},
                {"truncation", {{"truncated", false}}}};
    }

    void testSparseLiveAndTransactionalState(tests::support::TestResult& result) {
        Harness harness;
        std::size_t observations = 0;
        bool sparseObservedAfterCommit = false;
        client::Client sdk(options(), harness.callbacks());
        client::ClientCallbacks callbacks = harness.callbacks();
        callbacks.onProtocolMessage = [&](const frontend::ServerMessage& message) {
            ++observations;
            const auto* batch = std::get_if<frontend::EventBatch>(&message);
            sparseObservedAfterCommit =
                sparseObservedAfterCommit || (batch && batch->toSequence == frontend::SequenceNumber(43) && sdk.state().item("item-1") &&
                                              sdk.state().item("item-1")->commandOutput == "three");
        };
        sdk.setCallbacks(std::move(callbacks));
        client::Connection connection = sdk.openConnection(harness.transport());
        connectAndSnapshot(sdk, connection, frontend::SequenceNumber(40));
        const client::State retained = sdk.state();
        const std::size_t observationsAfterSnapshot = observations;

        const frontend::EventBatch sparse{frontend::SequenceNumber(41),
                                          frontend::SequenceNumber(43),
                                          {content(frontend::SequenceNumber(41), "one"), content(frontend::SequenceNumber(43), "three")}};
        (void) connection.receive(frontend::ServerMessage{sparse});
        result.expectTrue(connection.isOpen() && sdk.visibleSequence() == frontend::SequenceNumber(43) &&
                              sdk.synchronizedThrough() == frontend::SequenceNumber(43) && sdk.state().item("item-1") &&
                              sdk.state().item("item-1")->commandOutput == "three" && retained.item("item-1") &&
                              retained.item("item-1")->commandOutput == "initial" && observations == observationsAfterSnapshot + 1 &&
                              sparseObservedAfterCommit,
                          "sparse visible live sequences 41 and 43 are accepted transactionally, then observed, while an old immutable "
                          "State remains unchanged");

        const client::State beforeMalformed = sdk.state();
        const frontend::EventBatch malformed{
            frontend::SequenceNumber(44),
            frontend::SequenceNumber(45),
            {content(frontend::SequenceNumber(44), "must-not-commit"),
             frontend::FrontendEvent{frontend::SequenceNumber(45), "unknown.top.level", frontend::Json::object()}}};
        (void) connection.receive(frontend::ServerMessage{malformed});
        result.expectTrue(!connection.isOpen() && beforeMalformed.item("item-1") &&
                              beforeMalformed.item("item-1")->commandOutput == "three" && sdk.state().item("item-1") &&
                              sdk.state().item("item-1")->commandOutput == "three" && observations == observationsAfterSnapshot + 1,
                          "a malformed second event closes only the connection, leaves State unchanged, and is not observed");

        Harness regressionHarness;
        std::size_t regressionObservations = 0;
        client::ClientCallbacks regressionCallbacks = regressionHarness.callbacks();
        regressionCallbacks.onProtocolMessage = [&regressionObservations](const frontend::ServerMessage&) {
            ++regressionObservations;
        };
        client::Client regressionSdk(options(), std::move(regressionCallbacks));
        client::Connection regressionConnection = regressionSdk.openConnection(regressionHarness.transport());
        connectAndSnapshot(regressionSdk, regressionConnection, frontend::SequenceNumber(43));
        const std::size_t observationsBeforeRegression = regressionObservations;
        const frontend::EventBatch regression{
            frontend::SequenceNumber(42), frontend::SequenceNumber(42), {content(frontend::SequenceNumber(42), "must-not-be-observed")}};
        (void) regressionConnection.receive(frontend::ServerMessage{regression});
        result.expectTrue(!regressionConnection.isOpen() && regressionSdk.state().item("item-1") &&
                              regressionSdk.state().item("item-1")->commandOutput == "initial" &&
                              regressionObservations == observationsBeforeRegression,
                          "a regressed live occurrence closes without mutating State or reaching protocol observation");

        Harness closeHarness;
        client::Client* closeSdkPointer = nullptr;
        std::size_t eventObservationsAfterClose = 0;
        client::ClientCallbacks closeCallbacks = closeHarness.callbacks();
        closeCallbacks.onStateUpdated = [&](const client::StateUpdate& update) {
            if (update.cause == client::UpdateCause::Live) {
                closeSdkPointer->close("close from committed live-state callback");
            }
        };
        closeCallbacks.onProtocolMessage = [&](const frontend::ServerMessage& message) {
            eventObservationsAfterClose += std::holds_alternative<frontend::EventBatch>(message) ? 1U : 0U;
        };
        client::Client closeSdk(options(), std::move(closeCallbacks));
        closeSdkPointer = &closeSdk;
        client::Connection closeConnection = closeSdk.openConnection(closeHarness.transport());
        connectAndSnapshot(closeSdk, closeConnection, frontend::SequenceNumber(40));
        (void) closeConnection.receive(
            frontend::ServerMessage{frontend::EventBatch{frontend::SequenceNumber(41),
                                                         frontend::SequenceNumber(41),
                                                         {content(frontend::SequenceNumber(41), "committed-before-close")}}});
        result.expectTrue(
            !closeConnection.isOpen() && closeSdk.connectionState() == client::ConnectionState::Closed && closeHarness.closes == 1 &&
                closeSdk.state().item("item-1") && closeSdk.state().item("item-1")->commandOutput == "committed-before-close" &&
                eventObservationsAfterClose == 0,
            "closing from the committed live-state callback detaches before protocol observation, so the accepted batch is not observed");
    }

    void testReadyLiveSnapshotBarriers(tests::support::TestResult& result) {
        Harness harness;
        std::size_t observedSnapshots = 0;
        client::ClientCallbacks callbacks = harness.callbacks();
        callbacks.onProtocolMessage = [&](const frontend::ServerMessage& message) {
            observedSnapshots += std::holds_alternative<frontend::Snapshot>(message) ? 1U : 0U;
        };
        client::Client sdk(options(), std::move(callbacks));
        client::Connection connection = sdk.openConnection(harness.transport());
        connectAndSnapshot(sdk, connection, frontend::SequenceNumber(40));
        const client::State retained = sdk.state();
        const auto retainedFingerprint = retained.projectionFingerprintMetadata();
        const std::uint64_t initialRevision = retained.revision();
        const std::size_t initialSynchronizedCallbacks = harness.synchronized;
        const std::size_t initialObservedSnapshots = observedSnapshots;
        harness.updates.clear();
        harness.cursors.clear();

        std::size_t commandCompletions = 0;
        const client::Submission snapshotFirstCommand = sdk.submit(
            generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ProviderStart>{frontend::Json::object()}},
            [&](const client::GeneratedOperationResult& operation) {
                if (operation)
                    ++commandCompletions;
            });
        frontend::Json higherState = expandedState("live-higher");
        higherState["threadList"]["complete"] = true;
        higherState["threadList"]["pagesLoaded"] = std::uint64_t{2};
        higherState["threadList"]["backwardsCursor"] = "cursor-back";
        (void) connection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(41), std::move(higherState)}});

        const bool higherChanges = harness.updates.size() == 1 && harness.updates.back().changes.size() == 2 &&
                                   std::holds_alternative<client::StateReplacedChange>(harness.updates.back().changes[0]) &&
                                   std::holds_alternative<client::CursorAdvancedChange>(harness.updates.back().changes[1]);
        result.expectTrue(
            snapshotFirstCommand && commandCompletions == 0 && sdk.pendingOperationCount() == 1 && connection.isOpen() && sdk.isReady() &&
                sdk.state().freshness() == client::StateFreshness::Current && sdk.state().revision() == initialRevision + 1 &&
                sdk.visibleSequence() == frontend::SequenceNumber(41) && sdk.synchronizedThrough() == frontend::SequenceNumber(41) &&
                sdk.state().session() && sdk.state().session()->sessionId == "1" &&
                sdk.state().projectionFingerprintMetadata() == retainedFingerprint && sdk.state().threadList().value &&
                sdk.state().threadList().value->complete && sdk.state().threadList().value->pagesLoaded == 2 &&
                sdk.state().threadList().value->backwardsCursor == "cursor-back" && sdk.state().item("item-1") &&
                sdk.state().item("item-1")->commandOutput == "live-higher" && retained.item("item-1") &&
                retained.item("item-1")->commandOutput == "initial" && higherChanges &&
                harness.updates.back().cause == client::UpdateCause::SnapshotFallback &&
                harness.updates.back().fromSequence == frontend::SequenceNumber(41) &&
                harness.updates.back().toSequence == frontend::SequenceNumber(41) &&
                harness.cursors == std::vector{frontend::SequenceNumber(41)} && harness.synchronized == initialSynchronizedCallbacks &&
                observedSnapshots == initialObservedSnapshots + 1 &&
                client::detail::StateReducer::accountingIsConsistentForTesting(sdk.state()),
            "a higher-sequence Ready Snapshot is one atomic SnapshotFallback commit, advances both cursors once, preserves the active "
            "session/fingerprint and pending command, and leaves retained immutable State unchanged");

        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(snapshotFirstCommand.requestId->value(), frontend::Json::object())});
        result.expectTrue(commandCompletions == 1 && sdk.pendingOperationCount() == 0 && connection.isOpen() && sdk.isReady(),
                          "a response arriving after a live Snapshot remains correlated and completes exactly once");

        harness.updates.clear();
        harness.cursors.clear();
        const client::Submission responseFirstCommand = sdk.submit(
            generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ProviderStart>{frontend::Json::object()}},
            [&](const client::GeneratedOperationResult& operation) {
                if (operation)
                    ++commandCompletions;
            });
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(responseFirstCommand.requestId->value(), frontend::Json::object())});
        frontend::Json equalState = expandedState("live-equal");
        equalState["threadList"]["pagesLoaded"] = std::uint64_t{3};
        (void) connection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(41), std::move(equalState)}});
        result.expectTrue(
            responseFirstCommand && commandCompletions == 2 && sdk.pendingOperationCount() == 0 && connection.isOpen() && sdk.isReady() &&
                sdk.visibleSequence() == frontend::SequenceNumber(41) && sdk.synchronizedThrough() == frontend::SequenceNumber(41) &&
                sdk.state().item("item-1") && sdk.state().item("item-1")->commandOutput == "live-equal" && sdk.state().threadList().value &&
                sdk.state().threadList().value->pagesLoaded == 3 && harness.updates.size() == 1 &&
                harness.updates.back().changes.size() == 1 &&
                std::holds_alternative<client::StateReplacedChange>(harness.updates.back().changes.front()) && harness.cursors.empty() &&
                harness.synchronized == initialSynchronizedCallbacks && observedSnapshots == initialObservedSnapshots + 2 &&
                client::detail::StateReducer::accountingIsConsistentForTesting(sdk.state()),
            "an equal-sequence Ready Snapshot is an authoritative replacement without a duplicate cursor notification, and a response "
            "arriving first remains terminal exactly once");

        Harness regressionHarness;
        std::optional<client::Error> regressionError;
        std::size_t regressionObservations = 0;
        std::vector<std::string> regressionOrder;
        client::ClientCallbacks regressionCallbacks = regressionHarness.callbacks();
        regressionCallbacks.onConnectionStateChanged = [&](const client::ConnectionStateChange& change) {
            if (!regressionError && change.error) {
                regressionError = change.error;
                regressionOrder.emplace_back("typed-error");
            }
        };
        regressionCallbacks.onProtocolMessage = [&](const frontend::ServerMessage&) {
            ++regressionObservations;
        };
        client::Client regressionSdk(options(), std::move(regressionCallbacks));
        client::Connection regressionConnection =
            regressionSdk.openConnection({[&regressionHarness](client::OutboundMessage message) {
                                              regressionHarness.messages.push_back(std::move(message));
                                              return client::SendResult{client::SendStatus::Accepted, std::nullopt};
                                          },
                                          [&regressionHarness, &regressionOrder](std::string) {
                                              ++regressionHarness.closes;
                                              regressionOrder.emplace_back("transport-close");
                                          }});
        connectAndSnapshot(regressionSdk, regressionConnection, frontend::SequenceNumber(40));
        const client::State beforeRegression = regressionSdk.state();
        const std::size_t observationsBeforeRegression = regressionObservations;
        (void) regressionConnection.receive(
            frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(39), expandedState("regressed-live-snapshot")}});
        result.expectTrue(
            !regressionConnection.isOpen() && regressionHarness.closes == 1 && regressionError &&
                regressionError->clientCode == client::ClientErrorCode::StateDivergence &&
                regressionError->message.find("regressed") != std::string::npos && beforeRegression.item("item-1") &&
                beforeRegression.item("item-1")->commandOutput == "initial" && regressionSdk.state().item("item-1") &&
                regressionSdk.state().item("item-1")->commandOutput == "initial" &&
                regressionSdk.state().freshness() == client::StateFreshness::Stale &&
                client::detail::StateReducer::accountingIsConsistentForTesting(beforeRegression) &&
                client::detail::StateReducer::accountingIsConsistentForTesting(regressionSdk.state()) &&
                regressionObservations == observationsBeforeRegression &&
                regressionOrder == std::vector<std::string>{"typed-error", "transport-close"},
            "a lower-sequence Ready Snapshot reports StateDivergence, closes after publishing the typed error, and cannot mutate or be "
            "observed as an accepted message");

        Harness malformedHarness;
        std::optional<client::Error> malformedError;
        client::ClientCallbacks malformedCallbacks = malformedHarness.callbacks();
        malformedCallbacks.onConnectionStateChanged = [&](const client::ConnectionStateChange& change) {
            if (!malformedError && change.error) {
                malformedError = change.error;
            }
        };
        client::Client malformedSdk(options(), std::move(malformedCallbacks));
        client::Connection malformedConnection = malformedSdk.openConnection(malformedHarness.transport());
        connectAndSnapshot(malformedSdk, malformedConnection, frontend::SequenceNumber(40));
        const client::State beforeMalformed = malformedSdk.state();
        frontend::Json malformedState = expandedState("malformed-live-snapshot");
        malformedState["threadList"] = frontend::Json::array();
        (void) malformedConnection.receive(
            frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(41), std::move(malformedState)}});
        result.expectTrue(!malformedConnection.isOpen() && malformedHarness.closes == 1 && malformedError &&
                              malformedError->clientCode == client::ClientErrorCode::StateDivergence && beforeMalformed.item("item-1") &&
                              beforeMalformed.item("item-1")->commandOutput == "initial" && malformedSdk.state().item("item-1") &&
                              malformedSdk.state().item("item-1")->commandOutput == "initial" &&
                              client::detail::StateReducer::accountingIsConsistentForTesting(beforeMalformed) &&
                              client::detail::StateReducer::accountingIsConsistentForTesting(malformedSdk.state()),
                          "a malformed Ready-state Snapshot fails through public dispatch with a typed error and no partial State commit");

        Harness oversizedHarness;
        client::ClientOptions boundedOptions = options();
        boundedOptions.maximumDecodedStateBytes = 64U * 1024U;
        std::optional<client::Error> oversizedError;
        client::ClientCallbacks boundedCallbacks = oversizedHarness.callbacks();
        boundedCallbacks.onConnectionStateChanged = [&](const client::ConnectionStateChange& change) {
            if (!oversizedError && change.error)
                oversizedError = change.error;
        };
        client::Client bounded(std::move(boundedOptions), std::move(boundedCallbacks));
        client::Connection boundedConnection = bounded.openConnection(oversizedHarness.transport());
        connectAndSnapshot(bounded, boundedConnection, frontend::SequenceNumber(40));
        const client::State beforeOversized = bounded.state();
        (void) boundedConnection.receive(
            frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(41), expandedState(std::string(96U * 1024U, 'x'))}});
        result.expectTrue(!boundedConnection.isOpen() && oversizedError &&
                              oversizedError->clientCode == client::ClientErrorCode::StateDivergence && beforeOversized.item("item-1") &&
                              beforeOversized.item("item-1")->commandOutput == "initial" && bounded.state().item("item-1") &&
                              bounded.state().item("item-1")->commandOutput == "initial" &&
                              bounded.state().freshness() == client::StateFreshness::Stale &&
                              client::detail::StateReducer::accountingIsConsistentForTesting(beforeOversized) &&
                              client::detail::StateReducer::accountingIsConsistentForTesting(bounded.state()),
                          "an oversized decoded live Snapshot fails closed and leaves the previous immutable State and accounting valid");
    }

    void testLiveRegressionRelationshipsAndDuplicateIdentities(tests::support::TestResult& result) {
        constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();
        client::SessionInfo session;
        session.sessionId = "1";
        session.syncMode = frontend::SyncMode::Snapshot;
        session.serverCurrentSequence = frontend::SequenceNumber(43);
        session.selectedRepresentationCapabilities = options().requestedCapabilities;
        std::string error;
        auto snapshot = client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                               frontend::Snapshot{frontend::SequenceNumber(43), expandedState()},
                                                               session,
                                                               unlimited,
                                                               64,
                                                               true,
                                                               error);
        std::optional<client::detail::StateReduction> synchronized;
        if (snapshot)
            synchronized =
                client::detail::StateReducer::synchronized(snapshot->state, frontend::SequenceNumber(43), session, unlimited, error);
        const frontend::Json beforeRegression =
            synchronized ? client::detail::StateReducer::serializeForTesting(synchronized->state) : frontend::Json::object();
        std::optional<client::detail::StateReduction> lowerRegression;
        std::optional<client::detail::StateReduction> equalRegression;
        std::optional<client::detail::StateReduction> snapshotRegression;
        if (synchronized) {
            error.clear();
            lowerRegression = client::detail::StateReducer::events(
                synchronized->state,
                frontend::EventBatch{
                    frontend::SequenceNumber(42), frontend::SequenceNumber(42), {content(frontend::SequenceNumber(42), "regressed")}},
                false,
                unlimited,
                64,
                true,
                error);
            error.clear();
            equalRegression = client::detail::StateReducer::events(
                synchronized->state,
                frontend::EventBatch{
                    frontend::SequenceNumber(43), frontend::SequenceNumber(43), {content(frontend::SequenceNumber(43), "duplicate")}},
                false,
                unlimited,
                64,
                true,
                error);
            error.clear();
            snapshotRegression = client::detail::StateReducer::snapshot(
                synchronized->state,
                frontend::Snapshot{frontend::SequenceNumber(42), expandedState("regressed snapshot")},
                session,
                unlimited,
                64,
                true,
                error);
        }
        result.expectTrue(snapshot && synchronized && !lowerRegression && !equalRegression && !snapshotRegression &&
                              client::detail::StateReducer::serializeForTesting(synchronized->state) == beforeRegression,
                          "live occurrences below or equal to synchronizedThrough and replacement snapshots below the represented cursor "
                          "are rejected without mutating State, while replay-only overlap deduplication remains separate");

        frontend::Json stateAfterRemoval = expandedState();
        stateAfterRemoval["items"] = frontend::Json::array();
        error.clear();
        auto removedSnapshot =
            client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                   frontend::Snapshot{frontend::SequenceNumber(43), std::move(stateAfterRemoval)},
                                                   session,
                                                   unlimited,
                                                   64,
                                                   true,
                                                   error);
        std::optional<client::detail::StateReduction> removedSynchronized;
        std::optional<client::detail::StateReduction> oldReferentialOverlap;
        frontend::Json beforeOldOverlap;
        if (removedSnapshot) {
            removedSynchronized =
                client::detail::StateReducer::synchronized(removedSnapshot->state, frontend::SequenceNumber(43), session, unlimited, error);
        }
        if (removedSynchronized) {
            beforeOldOverlap = client::detail::StateReducer::serializeForTesting(removedSynchronized->state);
            error.clear();
            oldReferentialOverlap =
                client::detail::StateReducer::events(removedSynchronized->state,
                                                     frontend::EventBatch{frontend::SequenceNumber(42),
                                                                          frontend::SequenceNumber(42),
                                                                          {content(frontend::SequenceNumber(42), "historical-content")}},
                                                     true,
                                                     unlimited,
                                                     64,
                                                     true,
                                                     error);
        }
        result.expectTrue(removedSnapshot && removedSynchronized && oldReferentialOverlap && oldReferentialOverlap->appliedEvents == 0 &&
                              oldReferentialOverlap->ignoredAlreadyAppliedEvents == 1 && oldReferentialOverlap->changes.empty() &&
                              !oldReferentialOverlap->state.item("item-1") &&
                              oldReferentialOverlap->state.visibleSequence() == frontend::SequenceNumber(43) &&
                              oldReferentialOverlap->state.synchronizedThrough() == frontend::SequenceNumber(43) &&
                              beforeOldOverlap.value("items", frontend::Json::array()).empty(),
                          "overlapping replay validates an old content wrapper without reapplying it against later state where the "
                          "referenced item was removed");

        Harness moveHarness;
        client::Client moving(options(), moveHarness.callbacks());
        client::Connection moveConnection = moving.openConnection(moveHarness.transport());
        moveConnection.transportConnected();
        frontend::Json movingState = expandedState();
        movingState["threads"].push_back(frontend::Json{{"id", "thread-2"}, {"fullyLoaded", true}});
        movingState["turns"].push_back(
            frontend::Json{{"id", "turn-2"}, {"threadId", "thread-2"}, {"status", "running"}, {"active", true}, {"terminal", false}});
        (void) moveConnection.receive(frontend::ServerMessage{welcome(frontend::SequenceNumber(1), frontend::SyncMode::Snapshot)});
        (void) moveConnection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(1), std::move(movingState)}});
        (void) moveConnection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(1)}});
        const frontend::EventBatch moves{
            frontend::SequenceNumber(2),
            frontend::SequenceNumber(2),
            {{frontend::SequenceNumber(2),
              "turn.upserted",
              frontend::Json{
                  {"turn", {{"id", "turn-1"}, {"threadId", "thread-2"}, {"status", "running"}, {"active", true}, {"terminal", false}}}}},
             {frontend::SequenceNumber(2),
              "item.upserted",
              frontend::Json{{"item",
                              {{"id", "item-1"},
                               {"threadId", "thread-2"},
                               {"turnId", "turn-2"},
                               {"type", "commandExecution"},
                               {"commandOutput", "moved"}}}}}}};
        (void) moveConnection.receive(frontend::ServerMessage{moves});
        const client::ThreadState* oldThread = moving.state().thread("thread-1");
        const client::ThreadState* newThread = moving.state().thread("thread-2");
        const client::TurnState* oldTurn = moving.state().turn("turn-1");
        const client::TurnState* newTurn = moving.state().turn("turn-2");
        result.expectTrue(
            moveConnection.isOpen() && oldThread && oldThread->orderedTurns.empty() && newThread &&
                std::find(newThread->orderedTurns.begin(), newThread->orderedTurns.end(), ai::openai::codex::typed::TurnId{"turn-1"}) !=
                    newThread->orderedTurns.end() &&
                oldTurn && oldTurn->orderedItems.empty() && newTurn &&
                std::find(newTurn->orderedItems.begin(), newTurn->orderedItems.end(), ai::openai::codex::typed::ItemId{"item-1"}) !=
                    newTurn->orderedItems.end(),
            "turn and item upserts remove moved identities from their old parent ordering before adding the new parent relation");

        auto duplicateRejected = [&](frontend::Json duplicateState, std::string_view description) {
            error.clear();
            const auto reduction =
                client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                       frontend::Snapshot{frontend::SequenceNumber(1), std::move(duplicateState)},
                                                       session,
                                                       unlimited,
                                                       64,
                                                       true,
                                                       error);
            result.expectTrue(!reduction && error.find("duplicate stable identity") != std::string::npos, std::string(description));
        };
        frontend::Json duplicateThreads = expandedState();
        duplicateThreads["threads"].push_back(duplicateThreads["threads"].front());
        duplicateRejected(std::move(duplicateThreads), "expanded snapshots reject duplicate thread identities");
        frontend::Json duplicateItems = expandedState();
        duplicateItems["items"].push_back(duplicateItems["items"].front());
        duplicateRejected(std::move(duplicateItems), "expanded snapshots reject duplicate item identities");
        frontend::Json duplicateSessions = expandedState();
        duplicateSessions["sessions"] = frontend::Json::array(
            {frontend::Json{{"sessionId", "7"}, {"role", "observer"}}, frontend::Json{{"sessionId", "7"}, {"role", "controller"}}});
        duplicateRejected(std::move(duplicateSessions), "expanded snapshots reject duplicate session identities");
        frontend::Json duplicateProcesses = expandedState();
        const frontend::Json process{{"processHandle", "process-1"},
                                     {"lifecycle", "running"},
                                     {"stamp", {{"generation", std::uint64_t{1}}, {"freshness", "current"}}}};
        duplicateProcesses["processes"] =
            frontend::Json{{"entries", frontend::Json::array({process, process})}, {"truncation", {{"truncated", false}}}};
        duplicateRejected(std::move(duplicateProcesses), "expanded snapshots reject duplicate process identities");

        client::SessionInfo legacySession = session;
        legacySession.selectedRepresentationCapabilities.clear();
        frontend::Json duplicateLegacyItems = legacyState();
        auto& legacyItems = duplicateLegacyItems["threads"][0]["turns"][0]["items"];
        legacyItems.push_back(legacyItems.front());
        error.clear();
        const auto duplicateLegacy =
            client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                   frontend::Snapshot{frontend::SequenceNumber(1), std::move(duplicateLegacyItems)},
                                                   legacySession,
                                                   unlimited,
                                                   64,
                                                   true,
                                                   error);
        result.expectTrue(!duplicateLegacy && error.find("duplicate") != std::string::npos,
                          "legacy snapshots reject duplicate nested item identities");
    }

    void testRevisionExhaustion(tests::support::TestResult& result) {
        constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();
        constexpr std::uint64_t maximumRevision = std::numeric_limits<std::uint64_t>::max();
        client::SessionInfo session;
        session.sessionId = "revision-session";
        session.syncMode = frontend::SyncMode::Replay;
        session.serverCurrentSequence = frontend::SequenceNumber(44);
        session.selectedRepresentationCapabilities = options().requestedCapabilities;
        std::string error;
        auto snapshot = client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                               frontend::Snapshot{frontend::SequenceNumber(43), expandedState()},
                                                               session,
                                                               unlimited,
                                                               64,
                                                               true,
                                                               error);
        std::optional<client::detail::StateReduction> synchronized;
        if (snapshot) {
            synchronized =
                client::detail::StateReducer::synchronized(snapshot->state, frontend::SequenceNumber(43), session, unlimited, error);
        }
        if (!synchronized) {
            result.expectTrue(false, "revision-exhaustion fixture establishes synchronized typed State");
            return;
        }

        const client::State nearMaximum = client::detail::StateReducer::withRevisionForTesting(synchronized->state, maximumRevision - 1);
        error.clear();
        const auto exactMaximum = client::detail::StateReducer::beginSynchronization(nearMaximum, session, unlimited, error);
        const client::State maximum = client::detail::StateReducer::withRevisionForTesting(synchronized->state, maximumRevision);
        const frontend::Json maximumBefore = client::detail::StateReducer::serializeForTesting(maximum);

        error.clear();
        const auto beginOverflow = client::detail::StateReducer::beginSynchronization(maximum, session, unlimited, error);
        const bool beginReported = error.find("revision exhausted") != std::string::npos;
        error.clear();
        const auto snapshotOverflow = client::detail::StateReducer::snapshot(
            maximum, frontend::Snapshot{frontend::SequenceNumber(44), expandedState()}, session, unlimited, 64, true, error);
        const bool snapshotReported = error.find("revision exhausted") != std::string::npos;
        error.clear();
        const auto eventOverflow = client::detail::StateReducer::events(
            maximum,
            frontend::EventBatch{
                frontend::SequenceNumber(44), frontend::SequenceNumber(44), {content(frontend::SequenceNumber(44), "revision-overflow")}},
            false,
            unlimited,
            64,
            true,
            error);
        const bool eventReported = error.find("revision exhausted") != std::string::npos;
        error.clear();
        const auto synchronizationOverflow =
            client::detail::StateReducer::synchronized(maximum, frontend::SequenceNumber(44), session, unlimited, error);
        const bool synchronizationReported = error.find("revision exhausted") != std::string::npos;
        error.clear();
        const auto staleOverflow = client::detail::StateReducer::stale(maximum, unlimited, error);
        const bool staleReported = error.find("revision exhausted") != std::string::npos;

        error.clear();
        auto validationStaging =
            client::detail::StateReducer::synchronizationStaging(session, frontend::SequenceNumber(43), unlimited, true, error);
        std::optional<client::detail::StateReduction> validationOverflow;
        bool validationReported = false;
        if (validationStaging) {
            const client::State maximumStaging = client::detail::StateReducer::withRevisionForTesting(*validationStaging, maximumRevision);
            error.clear();
            validationOverflow = client::detail::StateReducer::validateSynchronizationEvents(
                maximumStaging,
                frontend::EventBatch{frontend::SequenceNumber(44),
                                     frontend::SequenceNumber(44),
                                     {content(frontend::SequenceNumber(44), "revision-validation-overflow")}},
                unlimited,
                true,
                error);
            validationReported = error.find("revision exhausted") != std::string::npos;
        }

        result.expectTrue(exactMaximum && exactMaximum->revision() == maximumRevision && !beginOverflow && beginReported &&
                              !snapshotOverflow && snapshotReported && !eventOverflow && eventReported && !synchronizationOverflow &&
                              synchronizationReported && !staleOverflow && staleReported && validationStaging && !validationOverflow &&
                              validationReported && client::detail::StateReducer::serializeForTesting(maximum) == maximumBefore,
                          "every revision-advancing reducer path reaches the exact maximum and rejects overflow transactionally");
    }

    void testTypedProviderRealtimeAndItemStamp(tests::support::TestResult& result) {
        constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();
        client::SessionInfo session;
        session.sessionId = "1";
        session.syncMode = frontend::SyncMode::Snapshot;
        session.serverCurrentSequence = frontend::SequenceNumber(7);
        session.selectedRepresentationCapabilities = options().requestedCapabilities;

        frontend::Json state = expandedState();
        state["provider"]["lastError"] = frontend::Json{
            {"category", "transport"}, {"code", std::int64_t{17}}, {"detailsOmitted", true}, {"futureErrorField", "retained"}};
        state["provider"]["initialization"] = frontend::Json{{"codexHome", "/tmp/codex-home"},
                                                             {"platformFamily", "unix"},
                                                             {"platformOs", "linux"},
                                                             {"userAgent", "codex/1"},
                                                             {"futureInitializationField", 3}};
        state["threads"][0]["realtime"] = frontend::Json{{"lifecycle", "active"},
                                                         {"transcript", "bounded transcript"},
                                                         {"itemCount", std::uint64_t{4}},
                                                         {"receivedAudioBytes", std::uint64_t{120}},
                                                         {"droppedAudioBytes", std::uint64_t{8}},
                                                         {"transcriptTruncated", true},
                                                         {"errorDetailsOmitted", true},
                                                         {"sessionId", "realtime-1"},
                                                         {"version", "v1"},
                                                         {"lastSdpBytes", std::uint64_t{42}},
                                                         {"futureRealtimeField", "retained"}};
        state["accounts"] = frontend::Json{{"details", {{"authMode", "chatgpt"}, {"planType", "plus"}}}};
        state["configuration"] = frontend::Json{{"details", {{"writeStatus", "written"}}}};
        state["mcp"] = frontend::Json{{"details", {{"startupStatus", "complete"}}}};
        state["remoteControl"] = frontend::Json{{"details", {{"remoteControlStatus", "connected"}}}};
        state["items"][0]["generation"] = std::uint64_t{9};
        state["items"][0]["freshness"] = "current";

        std::string error;
        const auto reduction = client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                                      frontend::Snapshot{frontend::SequenceNumber(7), std::move(state)},
                                                                      session,
                                                                      unlimited,
                                                                      64,
                                                                      true,
                                                                      error);
        const client::ProviderState* provider =
            reduction && reduction->state.provider().value ? &*reduction->state.provider().value : nullptr;
        const client::ThreadState* thread = reduction ? reduction->state.thread("thread-1") : nullptr;
        const client::TurnState* turn = reduction ? reduction->state.turn("turn-1") : nullptr;
        const client::ItemState* item = reduction ? reduction->state.item("item-1") : nullptr;
        const client::AccountState* account =
            reduction && reduction->state.accounts().value ? &*reduction->state.accounts().value : nullptr;
        const client::ConfigurationState* configuration =
            reduction && reduction->state.configuration().value ? &*reduction->state.configuration().value : nullptr;
        const client::McpState* mcp = reduction && reduction->state.mcp().value ? &*reduction->state.mcp().value : nullptr;
        const client::PlatformState* platform =
            reduction && reduction->state.platform().value ? &*reduction->state.platform().value : nullptr;
        result.expectTrue(
            provider && provider->lastError && provider->lastError->category == "transport" && provider->lastError->code == 17 &&
                provider->lastError->detailsOmitted == true &&
                provider->lastError->extensions.value("futureErrorField", "") == "retained" && provider->initialization &&
                provider->initialization->codexHome.value == "/tmp/codex-home" && provider->initialization->platformFamily == "unix" &&
                provider->initialization->platformOs == "linux" && provider->initialization->userAgent == "codex/1" &&
                provider->initialization->extensions.value("futureInitializationField", 0) == 3 && thread && thread->realtime &&
                thread->realtime->lifecycle == "active" && thread->realtime->transcript == "bounded transcript" &&
                thread->realtime->itemCount == 4 && thread->realtime->receivedAudioBytes == 120 &&
                thread->realtime->droppedAudioBytes == 8 && thread->realtime->transcriptTruncated &&
                thread->realtime->errorDetailsOmitted == true && thread->realtime->sessionId == "realtime-1" && thread->realtime->version &&
                thread->realtime->version->value == "v1" && thread->realtime->lastSdpBytes == 42 &&
                thread->realtime->extensions.value("futureRealtimeField", "") == "retained" && turn &&
                turn->status == typed::TurnStatus::completed() && account && account->details.authMode &&
                account->details.authMode->value == "chatgpt" && account->details.planType && account->details.planType->value == "plus" &&
                configuration && configuration->details.writeStatus && configuration->details.writeStatus->value == "written" && mcp &&
                mcp->details.startupStatus && mcp->details.startupStatus->value == "complete" && platform &&
                platform->details.remoteControlStatus && platform->details.remoteControlStatus->value == "connected" && item &&
                item->stamp && item->stamp->generation == 9 && item->stamp->freshness == frontend::StateFreshness::Current,
            "provider, turn, realtime, account, configuration, MCP, and platform fields reuse their existing open-valued typed wrappers "
            "while preserving wire identities and bounded extensions");
    }

    void testHiddenSuffixAndReconnect(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection first = sdk.openConnection(harness.transport());
        connectAndSnapshot(sdk, first, frontend::SequenceNumber(51));
        first.transportDisconnected();

        client::Connection second = sdk.openConnection(harness.transport());
        second.transportConnected();
        const auto helloMessage = frontend::Codec::decodeClient(std::string_view(harness.messages.back().compactJson));
        const auto* hello = helloMessage ? std::get_if<frontend::Hello>(&helloMessage.value()) : nullptr;
        (void) second.receive(
            frontend::ServerMessage{welcome(frontend::SequenceNumber(53), frontend::SyncMode::Replay, frontend::Json::object(), "2")});
        (void) second.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(53)}});
        result.expectTrue(
            hello && hello->resumeAfter == frontend::SequenceNumber(51) && sdk.isReady() &&
                sdk.visibleSequence() == frontend::SequenceNumber(51) && sdk.synchronizedThrough() == frontend::SequenceNumber(53),
            "a replay with a hidden-only 52/53 suffix emits no batch yet advances synchronizedThrough beyond visibleSequence");

        const frontend::EventBatch later{
            frontend::SequenceNumber(54), frontend::SequenceNumber(54), {content(frontend::SequenceNumber(54), "later-visible")}};
        (void) second.receive(frontend::ServerMessage{later});
        result.expectTrue(second.isOpen() && sdk.visibleSequence() == frontend::SequenceNumber(54) && sdk.state().item("item-1") &&
                              sdk.state().item("item-1")->commandOutput == "later-visible",
                          "the connection remains usable and accepts a later visible global sequence 54 normally");
    }

    void testReconnectSessionRebinding(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection first = sdk.openConnection(harness.transport());
        first.transportConnected();
        (void) first.receive(frontend::ServerMessage{welcome(frontend::SequenceNumber(51), frontend::SyncMode::Snapshot)});
        frontend::Json initial = expandedState();
        initial["controller"] = frontend::Json::object();
        initial["controller"]["controllerSessionId"] = "1";
        initial["controller"]["present"] = true;
        (void) first.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(51), std::move(initial)}});
        (void) first.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(51)}});
        result.expectTrue(sdk.state().session() && sdk.state().session()->sessionId == "1" && sdk.state().controller().value &&
                              sdk.state().controller().value->ownedByThisClient,
                          "the initial synchronized State associates controller ownership with its first physical session");
        first.transportDisconnected();

        bool replayUpdateUsedNewSession = false;
        client::ClientCallbacks replayCallbacks = harness.callbacks();
        replayCallbacks.onStateUpdated = [&](const client::StateUpdate& update) {
            harness.updates.push_back(update);
            replayUpdateUsedNewSession =
                replayUpdateUsedNewSession || (update.cause == client::UpdateCause::ReconnectReplay && update.state.session() &&
                                               update.state.session()->sessionId == "2" && update.state.controller().value &&
                                               !update.state.controller().value->ownedByThisClient);
        };
        sdk.setCallbacks(std::move(replayCallbacks));

        client::Connection second = sdk.openConnection(harness.transport());
        second.transportConnected();
        const std::size_t messagesBeforeWelcome = harness.messages.size();
        (void) second.receive(
            frontend::ServerMessage{welcome(frontend::SequenceNumber(53), frontend::SyncMode::Replay, frontend::Json::object(), "2")});
        result.expectTrue(second.isOpen() && sdk.connectionState() == client::ConnectionState::Synchronizing &&
                              harness.messages.size() == messagesBeforeWelcome && sdk.state().session() &&
                              sdk.state().session()->sessionId == "2" && sdk.state().controller().value &&
                              !sdk.state().controller().value->ownedByThisClient,
                          "a continuity-compatible same-scope replay immediately rebinds retained State to the new session and clears old "
                          "controller ownership");

        (void) second.receive(frontend::ServerMessage{frontend::EventBatch{
            frontend::SequenceNumber(52), frontend::SequenceNumber(52), {content(frontend::SequenceNumber(52), "rebound-session")}}});
        (void) second.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(53)}});
        result.expectTrue(
            replayUpdateUsedNewSession && second.isOpen() && sdk.isReady() && sdk.state().session() &&
                sdk.state().session()->sessionId == "2" && sdk.state().controller().value &&
                !sdk.state().controller().value->ownedByThisClient && sdk.state().item("item-1") &&
                sdk.state().item("item-1")->commandOutput == "rebound-session",
            "every reconnect replay StateUpdate carries the new SessionInfo and never restores prior-session controller ownership");
    }

    void testPartialReplayAppendDeduplication(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection initial = sdk.openConnection(harness.transport());
        connectAndSnapshot(sdk, initial, frontend::SequenceNumber(1));
        initial.transportDisconnected();

        frontend::Json stamp = frontend::Json::object();
        stamp["generation"] = std::uint64_t{2};
        stamp["freshness"] = "current";
        frontend::Json noticeValue = frontend::Json::object();
        noticeValue["occurrence"] = std::uint64_t{2};
        noticeValue["category"] = "information";
        noticeValue["summary"] = "apply once";
        noticeValue["stamp"] = std::move(stamp);
        frontend::Json noticeData = frontend::Json::object();
        noticeData["notice"] = std::move(noticeValue);
        const frontend::FrontendEvent notice{frontend::SequenceNumber(2), "notice.added", std::move(noticeData), frontend::Json::object()};
        client::Connection interrupted = sdk.openConnection(harness.transport());
        interrupted.transportConnected();
        (void) interrupted.receive(frontend::ServerMessage{welcome(frontend::SequenceNumber(3), frontend::SyncMode::Replay)});
        (void) interrupted.receive(
            frontend::ServerMessage{frontend::EventBatch{frontend::SequenceNumber(2), frontend::SequenceNumber(2), {notice}}});
        interrupted.transportDisconnected();

        client::Connection resumed = sdk.openConnection(harness.transport());
        resumed.transportConnected();
        const auto helloMessage = frontend::Codec::decodeClient(std::string_view(harness.messages.back().compactJson));
        const auto* hello = helloMessage ? std::get_if<frontend::Hello>(&helloMessage.value()) : nullptr;
        (void) resumed.receive(frontend::ServerMessage{welcome(frontend::SequenceNumber(3), frontend::SyncMode::Replay)});
        (void) resumed.receive(
            frontend::ServerMessage{frontend::EventBatch{frontend::SequenceNumber(2), frontend::SequenceNumber(2), {notice}}});
        (void) resumed.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(3)}});
        result.expectTrue(hello && hello->resumeAfter == frontend::SequenceNumber(1) && resumed.isOpen() && sdk.isReady() &&
                              sdk.state().notices().value && sdk.state().notices().value->entries.size() == 1 &&
                              sdk.visibleSequence() == frontend::SequenceNumber(2) &&
                              sdk.synchronizedThrough() == frontend::SequenceNumber(3),
                          "replay after disconnect-before-SyncComplete validates and ignores the already represented notice occurrence");
    }

    void testFreshReplayDisconnectClearsSession(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        (void) connection.receive(
            frontend::ServerMessage{welcome(frontend::SequenceNumber(3), frontend::SyncMode::Replay, frontend::Json::object(), "9")});
        const bool staged = connection.isOpen() && sdk.state().freshness() == client::StateFreshness::Synchronizing &&
                            sdk.state().session() && sdk.state().session()->sessionId == "9";
        connection.transportDisconnected();
        result.expectTrue(staged && sdk.connectionState() == client::ConnectionState::Disconnected &&
                              sdk.state().freshness() == client::StateFreshness::Stale && !sdk.state().session(),
                          "disconnect immediately after a fresh replay Welcome clears the dead SessionInfo and marks staging State stale");
    }

    void testExpandedOccurrenceGroupsAndFamilies(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        (void) connection.receive(frontend::ServerMessage{welcome(frontend::SequenceNumber(20), frontend::SyncMode::Snapshot)});
        (void) connection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(20), expandedState()}});
        (void) connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(20)}});
        result.expectTrue(sdk.state().hasThreadProjection() && sdk.state().hasTurnProjection() && sdk.state().hasItemProjection() &&
                              sdk.state().hasPendingRequestProjection() && sdk.state().threadList().value &&
                              sdk.state().threadList().value->hasLoadedPage && !sdk.state().threadList().value->complete &&
                              sdk.state().threadList().value->pagesLoaded == 1 &&
                              sdk.state().threadList().value->nextCursor == "cursor-next" && sdk.state().threadList().value->stamp &&
                              sdk.state().threadList().value->stamp->generation == 1,
                          "expanded snapshots expose typed thread-list completeness, cursor, and freshness while projected collections "
                          "distinguish present empty projections from omitted projections");

        const frontend::EventBatch sameOccurrence{frontend::SequenceNumber(21),
                                                  frontend::SequenceNumber(21),
                                                  {content(frontend::SequenceNumber(21), "same-occurrence-output"),
                                                   frontend::FrontendEvent{frontend::SequenceNumber(21),
                                                                           "item.content.updated",
                                                                           frontend::Json{{"itemId", "item-1"},
                                                                                          {"threadId", "thread-1"},
                                                                                          {"turnId", "turn-1"},
                                                                                          {"channel", "agentText"},
                                                                                          {"content", "same-occurrence-agent"},
                                                                                          {"contentTruncated", false},
                                                                                          {"droppedContentBytes", std::uint64_t{0}}}}}};
        (void) connection.receive(frontend::ServerMessage{sameOccurrence});
        result.expectTrue(connection.isOpen() && sdk.visibleSequence() == frontend::SequenceNumber(21) && sdk.state().item("item-1") &&
                              sdk.state().item("item-1")->commandOutput == "same-occurrence-output" &&
                              sdk.state().item("item-1")->agentText == "same-occurrence-agent",
                          "adjacent expanded events may share one global occurrence sequence and retain their original order");

        const std::vector<std::string> families{
            "provider.updated",    "controller.updated", "sessions.updated",      "threadList.updated",   "thread.upserted",
            "thread.removed",      "turn.upserted",      "item.upserted",         "item.content.updated", "pendingRequests.updated",
            "account.updated",     "models.updated",     "configuration.updated", "process.updated",      "filesystemWatch.updated",
            "fuzzySearch.updated", "reviews.updated",    "integrations.updated",  "plugins.updated",      "skills.updated",
            "mcp.updated",         "platform.updated",   "notice.added",          "activity.updated",     "capacity.updated",
            "diagnostics.updated"};
        std::vector<frontend::FrontendEvent> events;
        for (std::size_t index = 0; index < families.size(); ++index) {
            const frontend::Json stamp{{"generation", std::uint64_t{1}}, {"freshness", "current"}};
            const frontend::Json domain{
                {"stamp", stamp},
                {"status", "ready"},
                {"latestResults", frontend::Json::array()},
                {"details",
                 {{"notificationCount", std::uint64_t{1}}, {"latestNotificationMethods", frontend::Json::array({"domain/updated"})}}}};
            frontend::Json data = frontend::Json::object();
            const std::string& family = families[index];
            if (family == "provider.updated")
                data = {{"provider",
                         {{"lifecycle", "ready"},
                          {"generation", std::uint64_t{2}},
                          {"desiredRunning", true},
                          {"recovery", {{"status", "idle"}, {"attempts", std::uint64_t{0}}}}}}};
            else if (family == "controller.updated")
                data = {{"controller", {{"present", false}}}};
            else if (family == "sessions.updated")
                data = {{"sessions", frontend::Json::array({frontend::Json{{"sessionId", "1"}, {"role", "observer"}}})}};
            else if (family == "threadList.updated")
                data = {{"threadList",
                         {{"hasLoadedPage", true},
                          {"complete", true},
                          {"pagesLoaded", std::uint64_t{2}},
                          {"backwardsCursor", "cursor-back"},
                          {"stamp", stamp}}}};
            else if (family == "thread.upserted")
                data = {{"thread", {{"id", "thread-family"}}}};
            else if (family == "thread.removed")
                data = {{"threadId", "thread-family"}};
            else if (family == "turn.upserted")
                data = {{"turn",
                         {{"id", "turn-family"}, {"threadId", "thread-1"}, {"status", "running"}, {"active", true}, {"terminal", false}}}};
            else if (family == "item.upserted")
                data = {{"item", {{"id", "item-family"}, {"threadId", "thread-1"}, {"turnId", "turn-1"}, {"type", "agentMessage"}}}};
            else if (family == "item.content.updated") {
                data = {{"threadId", "thread-1"},
                        {"turnId", "turn-1"},
                        {"itemId", "item-1"},
                        {"channel", "agentText"},
                        {"content", "family-content"}};
            } else if (family == "pendingRequests.updated")
                data = {{"pendingRequests", frontend::Json::array()}};
            else if (family == "account.updated" || family == "models.updated" || family == "configuration.updated" ||
                     family == "reviews.updated" || family == "integrations.updated" || family == "plugins.updated" ||
                     family == "skills.updated" || family == "mcp.updated" || family == "platform.updated")
                data = {{"domain", domain}};
            else if (family == "process.updated")
                data = {{"process", {{"processHandle", "process-family"}, {"lifecycle", "running"}, {"stamp", stamp}}}};
            else if (family == "filesystemWatch.updated")
                data = {{"filesystemWatch", {{"watchId", "watch-family"}, {"root", "/tmp"}, {"stamp", stamp}}}};
            else if (family == "fuzzySearch.updated")
                data = {{"fuzzySearch", {{"sessionId", "search-family"}, {"complete", false}, {"stamp", stamp}}}};
            else if (family == "notice.added")
                data = {{"notice", {{"occurrence", std::uint64_t{1}}, {"category", "warning"}, {"summary", "notice"}, {"stamp", stamp}}}};
            else if (family == "activity.updated")
                data = {{"activity",
                         {{"key", "activity-family"}, {"kind", "tool"}, {"lifecycle", "running"}, {"active", true}, {"stamp", stamp}}}};
            else if (family == "capacity.updated")
                data = {{"capacity", {{"sessions", std::uint64_t{1}}, {"retainedItems", std::uint64_t{2}}}}};
            else if (family == "diagnostics.updated")
                data = {{"diagnostic", {{"received", std::uint64_t{1}}, {"detailsOmitted", true}}}};
            events.push_back({frontend::SequenceNumber(22 + index), family, std::move(data)});
        }
        (void) connection.receive(
            frontend::ServerMessage{frontend::EventBatch{events.front().sequence, events.back().sequence, std::move(events)}});
        const frontend::Json changes = harness.updates.empty()
                                           ? frontend::Json::array()
                                           : client::detail::StateReducer::serializeChangesForTesting(harness.updates.back().changes);
        result.expectTrue(
            connection.isOpen() && sdk.visibleSequence() == frontend::SequenceNumber(47) && changes.size() == 26 &&
                sdk.state().threadList().value && sdk.state().threadList().value->complete &&
                sdk.state().threadList().value->pagesLoaded == 2 && sdk.state().threadList().value->backwardsCursor == "cursor-back" &&
                sdk.state().sessions().value && sdk.state().session(client::FrontendSessionId{"1"}) && sdk.state().processes().value &&
                sdk.state().processes().value->entries.size() == 1 && sdk.state().filesystemWatches().value &&
                sdk.state().filesystemWatches().value->entries.size() == 1 && sdk.state().fuzzySearches().value &&
                sdk.state().fuzzySearches().value->entries.size() == 1 && sdk.state().notices().value &&
                sdk.state().notices().value->entries.size() == 1 && sdk.state().activities().value &&
                sdk.state().activities().value->entries.size() == 1 && sdk.state().diagnostics().value &&
                sdk.state().diagnostics().value->entries.size() == 1 && sdk.state().permissionProfiles().value &&
                sdk.state().externalAgents().value && sdk.state().skills().value && sdk.state().windowsSandbox().value &&
                sdk.state().accounts().value && sdk.state().accounts().value->projection.notificationCount == 1 &&
                sdk.state().accounts().value->projection.latestNotificationMethods == std::vector<std::string>{"domain/updated"},
            "all 26 expanded event families produce typed changes and update the exact typed domain without losing aliases");

        const frontend::Json stamp{{"generation", std::uint64_t{2}}, {"freshness", "current"}};
        const std::vector<frontend::FrontendEvent> preserving{
            {frontend::SequenceNumber(48),
             "process.updated",
             frontend::Json{{"process", {{"processHandle", "process-second"}, {"lifecycle", "running"}, {"stamp", stamp}}}}},
            {frontend::SequenceNumber(49),
             "filesystemWatch.updated",
             frontend::Json{{"filesystemWatch", {{"watchId", "watch-second"}, {"stamp", stamp}}}}},
            {frontend::SequenceNumber(50),
             "fuzzySearch.updated",
             frontend::Json{{"fuzzySearch", {{"sessionId", "search-second"}, {"complete", false}, {"stamp", stamp}}}}},
            {frontend::SequenceNumber(51),
             "notice.added",
             frontend::Json{
                 {"notice", {{"occurrence", std::uint64_t{2}}, {"category", "information"}, {"summary", "second"}, {"stamp", stamp}}}}},
            {frontend::SequenceNumber(52),
             "activity.updated",
             frontend::Json{
                 {"activity",
                  {{"key", "activity-second"}, {"kind", "tool"}, {"lifecycle", "running"}, {"active", true}, {"stamp", stamp}}}}},
            {frontend::SequenceNumber(53),
             "diagnostics.updated",
             frontend::Json{{"diagnostic", {{"received", std::uint64_t{2}}, {"detailsOmitted", true}}}}},
            {frontend::SequenceNumber(54),
             "process.updated",
             frontend::Json{{"process", {{"processHandle", "process-second"}, {"lifecycle", "completed"}, {"stamp", stamp}}}}},
        };
        (void) connection.receive(
            frontend::ServerMessage{frontend::EventBatch{preserving.front().sequence, preserving.back().sequence, preserving}});
        result.expectTrue(connection.isOpen() && sdk.state().processes().value && sdk.state().processes().value->entries.size() == 2 &&
                              sdk.state().process(client::ProcessHandle{"process-family"}) &&
                              sdk.state().process(client::ProcessHandle{"process-second"}) &&
                              sdk.state().process(client::ProcessHandle{"process-second"})->lifecycle == "completed" &&
                              sdk.state().filesystemWatches().value && sdk.state().filesystemWatches().value->entries.size() == 2 &&
                              sdk.state().fuzzySearches().value && sdk.state().fuzzySearches().value->entries.size() == 2 &&
                              sdk.state().notices().value && sdk.state().notices().value->entries.size() == 2 &&
                              sdk.state().activities().value && sdk.state().activities().value->entries.size() == 2 &&
                              sdk.state().diagnostics().value && sdk.state().diagnostics().value->entries.size() == 2,
                          "singular process/watch/search/activity updates upsert while notices and diagnostics append without replacing "
                          "unrelated records");
    }

    void testDiagnosticRetentionAndLegacyState(tests::support::TestResult& result) {
        Harness boundedHarness;
        client::ClientOptions boundedOptions = options();
        boundedOptions.maximumRetainedDiagnostics = 1;
        client::Client bounded(std::move(boundedOptions), boundedHarness.callbacks());
        client::Connection boundedConnection = bounded.openConnection(boundedHarness.transport());
        connectAndSnapshot(bounded, boundedConnection, frontend::SequenceNumber(1));
        const std::vector<frontend::FrontendEvent> diagnostics{
            {frontend::SequenceNumber(2),
             "diagnostics.updated",
             frontend::Json{{"diagnostic", {{"received", std::uint64_t{1}}, {"message", "first"}}}}},
            {frontend::SequenceNumber(3),
             "diagnostics.updated",
             frontend::Json{{"diagnostic", {{"received", std::uint64_t{2}}, {"message", "second"}}}}},
        };
        (void) boundedConnection.receive(
            frontend::ServerMessage{frontend::EventBatch{diagnostics.front().sequence, diagnostics.back().sequence, diagnostics}});
        result.expectTrue(
            boundedConnection.isOpen() && bounded.state().diagnostics().value && bounded.state().diagnostics().value->entries.size() == 1 &&
                bounded.state().diagnostics().value->entries.front().message == "second" && bounded.state().diagnostics().truncated,
            "maximumRetainedDiagnostics keeps the newest typed diagnostics and records eviction");

        Harness zeroHarness;
        client::ClientOptions zeroOptions = options();
        zeroOptions.maximumRetainedDiagnostics = 0;
        client::Client zero(std::move(zeroOptions), zeroHarness.callbacks());
        client::Connection zeroConnection = zero.openConnection(zeroHarness.transport());
        connectAndSnapshot(zero, zeroConnection, frontend::SequenceNumber(1));
        (void) zeroConnection.receive(
            frontend::ServerMessage{frontend::EventBatch{frontend::SequenceNumber(2), frontend::SequenceNumber(2), {diagnostics.front()}}});
        result.expectTrue(zeroConnection.isOpen() && zero.state().diagnostics().value &&
                              zero.state().diagnostics().value->entries.empty() && zero.state().diagnostics().truncated,
                          "zero maximumRetainedDiagnostics retains no diagnostics and never means unlimited");

        Harness legacyHarness;
        client::Client legacy(options(), legacyHarness.callbacks());
        client::Connection legacyConnection = legacy.openConnection(legacyHarness.transport());
        legacyConnection.transportConnected();
        frontend::Welcome legacyWelcome{"7", frontend::SessionRole::Observer, frontend::SequenceNumber(4), frontend::SyncMode::Snapshot};
        (void) legacyConnection.receive(frontend::ServerMessage{legacyWelcome});
        (void) legacyConnection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(4), legacyState()}});
        (void) legacyConnection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(4)}});
        const client::ItemState* initialLegacyItem = legacy.state().item("item-1");
        result.expectTrue(
            legacyConnection.isOpen() && legacy.isReady() && legacy.state().representationMode() == client::RepresentationMode::LegacyV1 &&
                legacy.state().provider().value && legacy.state().provider().value->ready && legacy.state().controller().value &&
                legacy.state().controller().value->ownedByThisClient && legacy.state().threadList().value &&
                legacy.state().threadList().value->hasLoadedPage && !legacy.state().threadList().value->complete &&
                legacy.state().threadList().value->pagesLoaded == 2 && legacy.state().threadList().value->nextCursor == "cursor-next" &&
                initialLegacyItem && initialLegacyItem->status == "completed" && initialLegacyItem->commandOutput == "initial" &&
                initialLegacyItem->droppedContentBytes == 3 && initialLegacyItem->contentTruncated &&
                initialLegacyItem->startedAtMs == 10 && initialLegacyItem->completedAtMs == 20 && initialLegacyItem->data &&
                initialLegacyItem->data->value("exitCode", -1) == 0 &&
                initialLegacyItem->extensions.value("safeExtension", "") == "retained",
            "allowLegacyV1 normalizes the canonical legacy snapshot, including thread-list metadata and complete typed item fields");

        frontend::Json updatedLegacyItem{{"id", "item-1"},
                                         {"type", "command_execution"},
                                         {"status", "failed"},
                                         {"agentText", "updated-agent"},
                                         {"reasoningText", "updated-reasoning"},
                                         {"reasoningSummary", "updated-summary"},
                                         {"commandOutput", "updated-output"},
                                         {"droppedContentBytes", std::uint64_t{9}},
                                         {"contentTruncated", true},
                                         {"startedAtMs", std::int64_t{30}},
                                         {"completedAtMs", std::int64_t{40}},
                                         {"data", frontend::Json{{"exitCode", 1}}},
                                         {"extensions", frontend::Json{{"safeExtension", "updated"}}}};
        const frontend::EventBatch legacyUpdates{
            frontend::SequenceNumber(5),
            frontend::SequenceNumber(6),
            {{frontend::SequenceNumber(5),
              "thread.list.updated",
              frontend::Json{
                  {"hasLoadedPage", true}, {"complete", true}, {"pagesLoaded", std::uint64_t{3}}, {"backwardsCursor", "cursor-back"}}},
             {frontend::SequenceNumber(6),
              "item.updated",
              frontend::Json{{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"item", std::move(updatedLegacyItem)}}}}};
        (void) legacyConnection.receive(frontend::ServerMessage{legacyUpdates});
        const client::ItemState* updatedItem = legacy.state().item("item-1");
        result.expectTrue(
            legacyConnection.isOpen() && legacy.state().threadList().value && legacy.state().threadList().value->complete &&
                legacy.state().threadList().value->pagesLoaded == 3 &&
                legacy.state().threadList().value->backwardsCursor == "cursor-back" && updatedItem && updatedItem->status == "failed" &&
                updatedItem->agentText == "updated-agent" && updatedItem->reasoningText == "updated-reasoning" &&
                updatedItem->reasoningSummary == "updated-summary" && updatedItem->commandOutput == "updated-output" &&
                updatedItem->droppedContentBytes == 9 && updatedItem->contentTruncated && updatedItem->startedAtMs == 30 &&
                updatedItem->completedAtMs == 40 && updatedItem->data && updatedItem->data->value("exitCode", -1) == 1 &&
                updatedItem->extensions.value("safeExtension", "") == "updated",
            "canonical legacy thread-list and item updates preserve every stable item field instead of replacing it with a partial record");
    }

    void testExplicitReplayAndSnapshotFallback(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connectAndSnapshot(sdk, connection, frontend::SequenceNumber(10));

        std::size_t completions = 0;
        std::optional<client::SynchronizationResult> replayResult;
        client::Submission replay = sdk.synchronization().replay(
            frontend::SequenceNumber(8), [&](const client::OperationResult<client::SynchronizationResult>& operation) {
                ++completions;
                if (operation.value)
                    replayResult = *operation.value;
            });
        (void) connection.receive(frontend::ServerMessage{frontend::Response::success(
            replay.requestId->value(), frontend::Json{{"syncMode", "replay"}, {"sequence", std::uint64_t{10}}})});
        const client::Submission duringSync =
            sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                           frontend::Json::object()}},
                       {});
        result.expectTrue(completions == 0 && sdk.connectionState() == client::ConnectionState::Synchronizing && !duringSync &&
                              duringSync.error && duringSync.error->clientCode == client::ClientErrorCode::NotReady,
                          "an explicit synchronization response starts the stream but does not complete the operation before SyncComplete");

        const frontend::EventBatch overlap{
            frontend::SequenceNumber(9),
            frontend::SequenceNumber(10),
            {content(frontend::SequenceNumber(9), "old-nine"), content(frontend::SequenceNumber(10), "old-ten")}};
        (void) connection.receive(frontend::ServerMessage{overlap});
        (void) connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(10)}});
        result.expectTrue(completions == 1 && replayResult && replayResult->receivedEvents == 2 && replayResult->appliedEvents == 0 &&
                              replayResult->ignoredAlreadyAppliedEvents == 2 && sdk.state().item("item-1") &&
                              sdk.state().item("item-1")->commandOutput == "initial",
                          "overlapping explicit replay is validated, deduplicated, and completes exactly once at SyncComplete");

        std::optional<client::SynchronizationResult> fallback;
        client::Submission replayGap = sdk.synchronization().replay(
            frontend::SequenceNumber(1), [&](const client::OperationResult<client::SynchronizationResult>& operation) {
                if (operation.value)
                    fallback = *operation.value;
            });
        (void) connection.receive(frontend::ServerMessage{frontend::Response::success(
            replayGap.requestId->value(), frontend::Json{{"syncMode", "snapshot"}, {"sequence", std::uint64_t{12}}})});
        (void) connection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(12), expandedState("fallback")}});
        (void) connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(12)}});
        result.expectTrue(fallback && fallback->snapshotFallback && fallback->mode == frontend::SyncMode::Snapshot && sdk.isReady() &&
                              sdk.state().item("item-1") && sdk.state().item("item-1")->commandOutput == "fallback",
                          "server-reported replay unavailability uses one snapshot stream and reports snapshotFallback after SyncComplete");
    }

    void testExplicitSynchronizationFailureAndOrdering(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connectAndSnapshot(sdk, connection, frontend::SequenceNumber(7));
        harness.messages.clear();

        std::size_t synchronizationCompletions = 0;
        std::optional<client::Error> synchronizationError;
        std::size_t messagesInsideCompletion = 0;
        std::optional<client::Submission> callbackSubmission;
        const client::Submission snapshot =
            sdk.synchronization().snapshot([&](const client::OperationResult<client::SynchronizationResult>& operation) {
                ++synchronizationCompletions;
                synchronizationError = operation.error;
                messagesInsideCompletion = harness.messages.size();
                callbackSubmission =
                    sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ProviderStart>{
                                   frontend::Json::object()}},
                               {});
            });
        result.expectTrue(snapshot && sdk.connectionState() == client::ConnectionState::Synchronizing && harness.messages.size() == 1,
                          "an explicit snapshot enters Synchronizing as soon as its command is accepted");

        (void) connection.receive(frontend::ServerMessage{frontend::Response::failure(
            snapshot.requestId->value(),
            frontend::CommandError{frontend::ErrorCode::Conflict, "snapshot command rejected", std::nullopt, frontend::Json::object()})});
        const auto callbackCommand = frontend::Codec::decodeDefinedCommand(std::string_view(harness.messages.back().compactJson));
        result.expectTrue(synchronizationCompletions == 1 && synchronizationError &&
                              synchronizationError->origin == client::ErrorOrigin::Command && messagesInsideCompletion == 1 &&
                              callbackSubmission && *callbackSubmission && harness.messages.size() == 2 && callbackCommand &&
                              generated::commandMethod(callbackCommand.value().parameters) == generated::MethodId::ProviderStart &&
                              connection.isOpen() && sdk.isReady(),
                          "a failed explicit synchronization command restores Ready before its callback, while callback submission remains "
                          "deferred until dispatch completes");

        Harness orderingHarness;
        client::Client orderingSdk(options(), orderingHarness.callbacks());
        client::Connection orderingConnection = orderingSdk.openConnection(orderingHarness.transport());
        connectAndSnapshot(orderingSdk, orderingConnection, frontend::SequenceNumber(7));
        std::size_t orderingCompletions = 0;
        const client::Submission awaitingResponse =
            orderingSdk.synchronization().snapshot([&](const client::OperationResult<client::SynchronizationResult>&) {
                ++orderingCompletions;
            });
        (void) orderingConnection.receive(
            frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(8), expandedState("too-early")}});
        result.expectTrue(
            awaitingResponse && orderingCompletions == 1 && !orderingConnection.isOpen() && orderingHarness.closes == 1 &&
                orderingSdk.connectionState() == client::ConnectionState::Disconnected,
            "snapshot synchronization data received before its successful command response is a connection-local protocol failure");

        Harness rejectionHarness;
        bool rejectCommands = false;
        client::TransportCallbacks rejectingTransport{
            [&](client::OutboundMessage message) {
                const bool command = message.kind == client::OutboundKind::Command;
                rejectionHarness.messages.push_back(std::move(message));
                return command && rejectCommands ? client::SendResult{client::SendStatus::Backpressure,
                                                                      client::TransportError{"explicit synchronization rejection", true}}
                                                 : client::SendResult{client::SendStatus::Accepted, std::nullopt};
            },
            [&](std::string) {
                ++rejectionHarness.closes;
            },
        };
        client::Client rejectionSdk(options(), rejectionHarness.callbacks());
        client::Connection rejectionConnection = rejectionSdk.openConnection(std::move(rejectingTransport));
        connectAndSnapshot(rejectionSdk, rejectionConnection, frontend::SequenceNumber(7));
        rejectCommands = true;
        std::size_t rejectedCallbacks = 0;
        const client::Submission rejected =
            rejectionSdk.synchronization().snapshot([&](const client::OperationResult<client::SynchronizationResult>&) {
                ++rejectedCallbacks;
            });
        result.expectTrue(!rejected && rejected.error && rejected.error->clientCode == client::ClientErrorCode::SendRejected &&
                              rejectedCallbacks == 0 && !rejectionConnection.isOpen() && rejectionHarness.closes == 1,
                          "an explicit synchronization rejected before transport ownership returns a failed Submission without an empty-ID "
                          "completion callback");

        Harness readyCloseHarness;
        client::Client* readyCloseClient = nullptr;
        bool closeOnReady = false;
        client::ClientCallbacks readyCloseCallbacks = readyCloseHarness.callbacks();
        readyCloseCallbacks.onConnectionStateChanged = [&](const client::ConnectionStateChange& change) {
            if (closeOnReady && change.current == client::ConnectionState::Ready) {
                readyCloseClient->close("close from explicit synchronization Ready callback");
            }
        };
        client::Client readyCloseSdk(options(), std::move(readyCloseCallbacks));
        readyCloseClient = &readyCloseSdk;
        client::Connection readyCloseConnection = readyCloseSdk.openConnection(readyCloseHarness.transport());
        connectAndSnapshot(readyCloseSdk, readyCloseConnection, frontend::SequenceNumber(7));
        std::size_t readyCloseCompletions = 0;
        std::optional<client::OperationResult<client::SynchronizationResult>> readyCloseResult;
        const client::Submission readyCloseSubmission =
            readyCloseSdk.synchronization().snapshot([&](const client::OperationResult<client::SynchronizationResult>& operation) {
                ++readyCloseCompletions;
                readyCloseResult = operation;
            });
        (void) readyCloseConnection.receive(frontend::ServerMessage{
            frontend::Response::success(readyCloseSubmission.requestId->value(), frontend::Json{{"sequence", std::uint64_t{8}}})});
        (void) readyCloseConnection.receive(
            frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(8), expandedState("ready-close")}});
        closeOnReady = true;
        (void) readyCloseConnection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(8)}});
        result.expectTrue(readyCloseCompletions == 1 && readyCloseResult &&
                              readyCloseResult->requestId.value() == readyCloseSubmission.requestId->value() && !readyCloseResult->value &&
                              readyCloseResult->error && readyCloseResult->error->clientCode == client::ClientErrorCode::Closed &&
                              readyCloseSdk.connectionState() == client::ConnectionState::Closed && readyCloseHarness.closes == 1,
                          "closing Client from the explicit synchronization Ready transition fails the still-unreported operation exactly "
                          "once instead of reporting success after Closed");
    }

    void testAdvancedGeneratedSynchronization(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connectAndSnapshot(sdk, connection, frontend::SequenceNumber(10));

        std::size_t snapshotCompletions = 0;
        std::optional<generated::MethodId> snapshotResultMethod;
        const client::Submission snapshot = sdk.submit(
            generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::SnapshotGet>{frontend::Json::object()}},
            [&](const client::GeneratedOperationResult& operation) {
                ++snapshotCompletions;
                if (operation.value) {
                    snapshotResultMethod = generated::commandMethod(*operation.value);
                }
            });
        (void) connection.receive(frontend::ServerMessage{
            frontend::Response::success(snapshot.requestId->value(), frontend::Json{{"sequence", std::uint64_t{11}}})});
        (void) connection.receive(
            frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(11), expandedState("advanced-snapshot")}});
        result.expectTrue(snapshot && snapshotCompletions == 0 && sdk.pendingOperationCount() == 1 &&
                              sdk.connectionState() == client::ConnectionState::Synchronizing,
                          "advanced generated snapshot.get remains correlated after its response and snapshot payload");
        (void) connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(11)}});
        result.expectTrue(snapshotCompletions == 1 && snapshotResultMethod == generated::MethodId::SnapshotGet && sdk.isReady() &&
                              sdk.pendingOperationCount() == 0,
                          "advanced generated snapshot.get completes only after SyncComplete");

        std::size_t replayCompletions = 0;
        std::optional<generated::MethodId> replayResultMethod;
        const client::Submission replay =
            sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::EventsReplay>{
                           frontend::Json{{"after", std::uint64_t{11}}}}},
                       [&](const client::GeneratedOperationResult& operation) {
                           ++replayCompletions;
                           if (operation.value) {
                               replayResultMethod = generated::commandMethod(*operation.value);
                           }
                       });
        (void) connection.receive(frontend::ServerMessage{frontend::Response::success(
            replay.requestId->value(), frontend::Json{{"syncMode", "replay"}, {"sequence", std::uint64_t{13}}})});
        result.expectTrue(replay && replayCompletions == 0 && sdk.pendingOperationCount() == 1 &&
                              sdk.connectionState() == client::ConnectionState::Synchronizing,
                          "advanced generated events.replay remains pending after its command response even with no visible event batch");
        (void) connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(13)}});
        result.expectTrue(replayCompletions == 1 && replayResultMethod == generated::MethodId::EventsReplay && sdk.isReady() &&
                              sdk.pendingOperationCount() == 0 && sdk.visibleSequence() == frontend::SequenceNumber(11) &&
                              sdk.synchronizedThrough() == frontend::SequenceNumber(13),
                          "advanced generated events.replay completes at SyncComplete and preserves the hidden-suffix cursor distinction");
    }

    void testDeferredSynchronizationFromNotifications(tests::support::TestResult& result) {
        enum class Source { LiveState, CompletedState, Ready, Cursor, Synchronized };
        const auto run = [&](Source source, std::string_view label) {
            Harness harness;
            client::Client* sdkPointer = nullptr;
            bool scheduled = false;
            std::size_t messagesInsideSchedule = 0;
            std::size_t liveStateNotifications = 0;
            std::size_t completedStateNotifications = 0;
            std::size_t readyNotifications = 0;
            std::size_t cursorNotifications = 0;
            std::size_t synchronizedNotifications = 0;
            std::optional<client::Submission> firstSubmission;
            std::optional<client::Submission> synchronizationSubmission;
            const auto schedule = [&] {
                if (scheduled) {
                    return;
                }
                scheduled = true;
                messagesInsideSchedule = harness.messages.size();
                firstSubmission =
                    sdkPointer->submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ProviderStart>{
                                           frontend::Json::object()}},
                                       {});
                synchronizationSubmission = sdkPointer->synchronization().snapshot({});
            };

            client::ClientCallbacks callbacks;
            callbacks.onConnectionStateChanged = [&](const client::ConnectionStateChange& change) {
                if (change.current == client::ConnectionState::Ready) {
                    ++readyNotifications;
                    if (source == Source::Ready) {
                        schedule();
                    }
                }
            };
            callbacks.onStateUpdated = [&](const client::StateUpdate& update) {
                if (update.cause == client::UpdateCause::Live) {
                    ++liveStateNotifications;
                    if (source == Source::LiveState) {
                        schedule();
                    }
                } else if (update.cause == client::UpdateCause::SynchronizationCompleted) {
                    ++completedStateNotifications;
                    if (source == Source::CompletedState) {
                        schedule();
                    }
                }
            };
            callbacks.onCursorAdvanced = [&](frontend::SequenceNumber) {
                ++cursorNotifications;
                if (source == Source::Cursor) {
                    schedule();
                }
            };
            callbacks.onSynchronized = [&](const client::SynchronizationInfo&) {
                ++synchronizedNotifications;
                if (source == Source::Synchronized) {
                    schedule();
                }
            };

            client::Client sdk(options(), std::move(callbacks));
            sdkPointer = &sdk;
            client::Connection connection = sdk.openConnection(harness.transport());
            connectAndSnapshot(sdk, connection, frontend::SequenceNumber(7));
            std::size_t expectedMessageBase = 1;
            if (source == Source::LiveState) {
                harness.messages.clear();
                expectedMessageBase = 0;
                liveStateNotifications = 0;
                completedStateNotifications = 0;
                readyNotifications = 0;
                cursorNotifications = 0;
                synchronizedNotifications = 0;
                (void) connection.receive(
                    frontend::ServerMessage{frontend::EventBatch{frontend::SequenceNumber(8),
                                                                 frontend::SequenceNumber(8),
                                                                 {content(frontend::SequenceNumber(8), "callback-synchronization")}}});
            }

            std::vector<generated::MethodId> sentMethods;
            for (std::size_t index = expectedMessageBase; index < harness.messages.size(); ++index) {
                const auto command = frontend::Codec::decodeDefinedCommand(std::string_view(harness.messages[index].compactJson));
                if (command) {
                    sentMethods.push_back(generated::commandMethod(command.value().parameters));
                }
            }
            const bool outerNotificationsPreserved =
                source == Source::LiveState ? liveStateNotifications == 1 && completedStateNotifications == 0 && readyNotifications == 0 &&
                                                  cursorNotifications == 1 && synchronizedNotifications == 0
                                            : liveStateNotifications == 0 && completedStateNotifications == 1 && readyNotifications == 1 &&
                                                  cursorNotifications == 1 && synchronizedNotifications == 1;
            result.expectTrue(scheduled && firstSubmission && *firstSubmission && synchronizationSubmission && *synchronizationSubmission &&
                                  messagesInsideSchedule == expectedMessageBase && harness.messages.size() == expectedMessageBase + 2 &&
                                  sentMethods == std::vector{generated::MethodId::ProviderStart, generated::MethodId::SnapshotGet} &&
                                  outerNotificationsPreserved && sdk.connectionState() == client::ConnectionState::Synchronizing &&
                                  sdk.pendingOperationCount() == 2,
                              "deferred synchronization from " + std::string(label) +
                                  " preserves outer notifications, then sends preceding work and snapshot.get in FIFO order");
        };

        run(Source::LiveState, "a live state callback");
        run(Source::CompletedState, "the synchronization-completed state callback");
        run(Source::Ready, "the Ready transition callback");
        run(Source::Cursor, "the cursor callback");
        run(Source::Synchronized, "the synchronized callback");
    }

    void testProjectionFingerprintAndRefresh(tests::support::TestResult& result) {
        client::ProjectionFingerprintInput base;
        base.requestedRepresentationCapabilities = {frontend::FrontendCapability::CompleteBackendDomains,
                                                    frontend::FrontendCapability::CompleteThreadItems};
        base.selectedRepresentationCapabilities = base.requestedRepresentationCapabilities;
        base.continuityKey = "verified-local:1000";
        base.permittedScopes = std::vector{frontend::FrontendScope::Observe};
        base.permittedMethods = std::vector{generated::MethodId::ThreadList};
        base.availableMethods = std::vector{generated::MethodId::ThreadList};
        const std::string stable = client::projectionFingerprint(base);

        client::ProjectionFingerprintInput reordered = base;
        std::reverse(reordered.requestedRepresentationCapabilities.begin(), reordered.requestedRepresentationCapabilities.end());
        client::ProjectionFingerprintInput changedScope = base;
        changedScope.permittedScopes->push_back(frontend::FrontendScope::CommandExecution);
        client::ProjectionFingerprintInput changedMethod = base;
        changedMethod.permittedMethods->push_back(generated::MethodId::CommandExec);
        result.expectTrue(
            client::projectionFingerprint(reordered) == stable && client::projectionFingerprint(changedScope) != stable &&
                client::projectionFingerprint(changedMethod) != stable,
            "projection fingerprints canonicalize set order and change deterministically for scope or exact method-set changes");

        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection first = sdk.openConnection(harness.transport());
        connectAndSnapshot(
            sdk, first, frontend::SequenceNumber(51), frontend::Json{{"permittedScopes", frontend::Json::array({"observe"})}});
        first.transportDisconnected();

        client::Connection second = sdk.openConnection(harness.transport());
        second.transportConnected();
        (void) second.receive(
            frontend::ServerMessage{welcome(frontend::SequenceNumber(53),
                                            frontend::SyncMode::Replay,
                                            frontend::Json{{"permittedScopes", frontend::Json::array({"observe", "command_execution"})}},
                                            "2")});
        const std::size_t beforeBoundary = harness.messages.size();
        (void) second.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(53)}});
        const auto snapshotCommand = frontend::Codec::decodeDefinedCommand(std::string_view(harness.messages.back().compactJson));
        result.expectTrue(
            !sdk.isReady() && harness.messages.size() == beforeBoundary + 1 && snapshotCommand &&
                generated::commandMethod(snapshotCommand.value().parameters) == generated::MethodId::SnapshotGet,
            "a changed projection fingerprint consumes replay but automatically requests a replacement snapshot before Ready");

        (void) second.receive(frontend::ServerMessage{
            frontend::Response::success(snapshotCommand.value().requestId, frontend::Json{{"sequence", std::uint64_t{53}}})});
        frontend::Json replacement = expandedState();
        replacement["threads"] = frontend::Json::array();
        replacement["turns"] = frontend::Json::array();
        replacement["items"] = frontend::Json::array();
        (void) second.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(53), std::move(replacement)}});
        (void) second.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(53)}});
        result.expectTrue(sdk.isReady() && sdk.state().items().empty() && harness.lastSnapshotFallback,
                          "richer retained state is replaced rather than mixed into a changed principal projection");

        const auto missingContinuityMetadataForcesSnapshot = [](int missingField) {
            Harness missingHarness;
            client::Client missingClient(options(), missingHarness.callbacks());
            client::Connection firstConnection = missingClient.openConnection(missingHarness.transport());
            connectAndSnapshot(missingClient, firstConnection, frontend::SequenceNumber(70));
            firstConnection.transportDisconnected();
            client::Connection secondConnection = missingClient.openConnection(missingHarness.transport());
            secondConnection.transportConnected();
            frontend::Welcome incomplete =
                welcome(frontend::SequenceNumber(71), frontend::SyncMode::Replay, frontend::Json::object(), "missing");
            if (missingField == 0) {
                incomplete.extensions.erase("permittedScopes");
            } else if (missingField == 1) {
                incomplete.permittedMethods.reset();
            } else {
                incomplete.availableMethods.reset();
            }
            (void) secondConnection.receive(frontend::ServerMessage{std::move(incomplete)});
            const std::size_t beforeBoundary = missingHarness.messages.size();
            (void) secondConnection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(71)}});
            const auto snapshot = frontend::Codec::decodeDefinedCommand(std::string_view(missingHarness.messages.back().compactJson));
            return secondConnection.isOpen() && !missingClient.isReady() && missingHarness.messages.size() == beforeBoundary + 1 &&
                   snapshot && generated::commandMethod(snapshot.value().parameters) == generated::MethodId::SnapshotGet;
        };
        result.expectTrue(missingContinuityMetadataForcesSnapshot(0) && missingContinuityMetadataForcesSnapshot(1) &&
                              missingContinuityMetadataForcesSnapshot(2),
                          "retained replay requires explicit permitted scopes plus permitted and available method sets; any absent set "
                          "forces snapshot refresh");

        Harness noContinuityHarness;
        client::ClientOptions noContinuityOptions = options();
        noContinuityOptions.credentialProvider = [] {
            return client::AuthenticationContext{frontend::NoCredential{}, std::nullopt};
        };
        client::Client noContinuityClient(std::move(noContinuityOptions), noContinuityHarness.callbacks());
        client::Connection noContinuityFirst = noContinuityClient.openConnection(noContinuityHarness.transport());
        connectAndSnapshot(noContinuityClient, noContinuityFirst, frontend::SequenceNumber(61));
        noContinuityFirst.transportDisconnected();
        client::Connection noContinuitySecond = noContinuityClient.openConnection(noContinuityHarness.transport());
        noContinuitySecond.transportConnected();
        const auto noContinuityHello = frontend::Codec::decodeClient(std::string_view(noContinuityHarness.messages.back().compactJson));
        const auto* hello = noContinuityHello ? std::get_if<frontend::Hello>(&noContinuityHello.value()) : nullptr;
        (void) noContinuitySecond.receive(
            frontend::ServerMessage{welcome(frontend::SequenceNumber(62), frontend::SyncMode::Replay, frontend::Json::object(), "62")});
        result.expectTrue(
            hello && !hello->resumeAfter && noContinuitySecond.isOpen() &&
                noContinuityClient.connectionState() == client::ConnectionState::Synchronizing,
            "absent authentication continuity omits resumeAfter while retaining a valid snapshot-refresh synchronization path");
        const std::size_t beforeNoContinuityBoundary = noContinuityHarness.messages.size();
        (void) noContinuitySecond.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(62)}});
        const auto noContinuitySnapshot =
            frontend::Codec::decodeDefinedCommand(std::string_view(noContinuityHarness.messages.back().compactJson));
        result.expectTrue(!noContinuityClient.isReady() && noContinuityHarness.messages.size() == beforeNoContinuityBoundary + 1 &&
                              noContinuitySnapshot &&
                              generated::commandMethod(noContinuitySnapshot.value().parameters) == generated::MethodId::SnapshotGet,
                          "a server-selected replay cannot mix retained state when absent authentication continuity omitted resumeAfter");
        const std::size_t beforeOutOfOrderProjectionData = noContinuityHarness.messages.size();
        (void) noContinuitySecond.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(62)}});
        result.expectTrue(!noContinuitySecond.isOpen() && noContinuityHarness.closes == 1 &&
                              noContinuityHarness.messages.size() == beforeOutOfOrderProjectionData &&
                              noContinuityClient.connectionState() == client::ConnectionState::Disconnected,
                          "projection-refresh synchronization data cannot precede the successful snapshot command response");
    }

    void testStateCapacity(tests::support::TestResult& result) {
        Harness harness;
        client::ClientOptions bounded = options();
        bounded.maximumDecodedStateBytes = 0;
        client::Client sdk(std::move(bounded), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        (void) connection.receive(frontend::ServerMessage{welcome(frontend::SequenceNumber(1), frontend::SyncMode::Snapshot)});
        (void) connection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(1), expandedState()}});
        result.expectTrue(!connection.isOpen() && sdk.connectionState() == client::ConnectionState::Disconnected,
                          "zero decoded-state capacity rejects transactionally and never means unlimited");
    }

    void testLegacyOptionalValidationNoticeBoundsAndStaleCapacity(tests::support::TestResult& result) {
        constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();
        client::SessionInfo legacySession;
        legacySession.sessionId = "legacy";
        legacySession.syncMode = frontend::SyncMode::Snapshot;
        legacySession.serverCurrentSequence = frontend::SequenceNumber(1);
        frontend::Json malformedLegacy = legacyState();
        malformedLegacy["processes"] = "invalid";
        std::string malformedError;
        const auto malformed =
            client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                   frontend::Snapshot{frontend::SequenceNumber(1), std::move(malformedLegacy)},
                                                   legacySession,
                                                   unlimited,
                                                   64,
                                                   true,
                                                   malformedError);
        result.expectTrue(!malformed && malformedError.find("processes") != std::string::npos,
                          "direct legacy reduction rejects a malformed optional collection with a typed error instead of throwing");

        client::SessionInfo expandedSession;
        expandedSession.sessionId = "expanded";
        expandedSession.syncMode = frontend::SyncMode::Snapshot;
        expandedSession.serverCurrentSequence = frontend::SequenceNumber(258);
        expandedSession.selectedRepresentationCapabilities = options().requestedCapabilities;
        std::string error;
        auto snapshot = client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                               frontend::Snapshot{frontend::SequenceNumber(1), expandedState()},
                                                               expandedSession,
                                                               unlimited,
                                                               64,
                                                               true,
                                                               error);
        std::vector<frontend::FrontendEvent> notices;
        notices.reserve(257);
        for (std::uint64_t sequence = 2; sequence <= 258; ++sequence) {
            notices.push_back(frontend::FrontendEvent{frontend::SequenceNumber(sequence),
                                                      "notice.added",
                                                      frontend::Json{{"notice",
                                                                      {{"occurrence", sequence},
                                                                       {"category", "information"},
                                                                       {"summary", "bounded notice"},
                                                                       {"stamp", {{"generation", sequence}, {"freshness", "current"}}}}}}});
        }
        auto boundedNotices =
            snapshot ? client::detail::StateReducer::events(
                           snapshot->state,
                           frontend::EventBatch{frontend::SequenceNumber(2), frontend::SequenceNumber(258), std::move(notices)},
                           false,
                           unlimited,
                           64,
                           true,
                           error)
                     : std::nullopt;
        result.expectTrue(boundedNotices && boundedNotices->state.notices().value &&
                              boundedNotices->state.notices().value->entries.size() == 256 &&
                              boundedNotices->state.notices().value->entries.front().occurrence == std::optional<std::uint64_t>{3} &&
                              boundedNotices->state.notices().value->entries.back().occurrence == std::optional<std::uint64_t>{258} &&
                              boundedNotices->state.notices().truncated && boundedNotices->state.notices().value->truncation.truncated &&
                              boundedNotices->state.notices().value->truncation.omittedEntries == std::optional<std::size_t>{1},
                          "more than 256 notice events preserve the newest ordered notices and account exactly for SDK eviction");

        frontend::Json saturatedState = expandedState();
        saturatedState["notices"] = frontend::Json::object();
        saturatedState["notices"]["entries"] = frontend::Json::array();
        for (std::uint64_t occurrence = 1; occurrence <= 256; ++occurrence) {
            saturatedState["notices"]["entries"].push_back(
                frontend::Json{{"occurrence", occurrence},
                               {"category", "information"},
                               {"summary", "retained"},
                               {"stamp", {{"generation", occurrence}, {"freshness", "current"}}}});
        }
        saturatedState["notices"]["truncation"] =
            frontend::Json{{"truncated", true}, {"omittedEntries", std::numeric_limits<std::uint64_t>::max()}};
        error.clear();
        auto saturatedSnapshot =
            client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                   frontend::Snapshot{frontend::SequenceNumber(1), std::move(saturatedState)},
                                                   expandedSession,
                                                   unlimited,
                                                   64,
                                                   true,
                                                   error);
        const frontend::FrontendEvent oneMoreNotice{
            frontend::SequenceNumber(2),
            "notice.added",
            frontend::Json{{"notice",
                            {{"occurrence", std::uint64_t{257}},
                             {"category", "information"},
                             {"summary", "saturating notice"},
                             {"stamp", {{"generation", std::uint64_t{257}}, {"freshness", "current"}}}}}}};
        auto saturated = saturatedSnapshot
                             ? client::detail::StateReducer::events(
                                   saturatedSnapshot->state,
                                   frontend::EventBatch{frontend::SequenceNumber(2), frontend::SequenceNumber(2), {oneMoreNotice}},
                                   false,
                                   unlimited,
                                   64,
                                   true,
                                   error)
                             : std::nullopt;
        result.expectTrue(saturated && saturated->state.notices().value &&
                              saturated->state.notices().value->truncation.omittedEntries ==
                                  std::optional<std::size_t>{std::numeric_limits<std::size_t>::max()},
                          "notice eviction accounting saturates without wrapping an existing omitted-entry counter");

        auto synchronized = snapshot ? client::detail::StateReducer::synchronized(
                                           snapshot->state, frontend::SequenceNumber(1), expandedSession, unlimited, error)
                                     : std::nullopt;
        auto stale = synchronized ? client::detail::StateReducer::stale(synchronized->state, unlimited, error) : std::nullopt;
        auto staleAgain = stale ? client::detail::StateReducer::stale(*stale, unlimited, error) : std::nullopt;
        const std::size_t initialFixtureBytes =
            client::detail::StateReducer::serializeForTesting(client::detail::StateReducer::initial()).dump().size();
        const std::size_t retainedFixtureBytes =
            synchronized ? client::detail::StateReducer::serializeForTesting(synchronized->state).dump().size() : 0;
        std::optional<client::State> boundedStale;
        if (synchronized) {
            std::size_t lower = 0;
            std::size_t upper = 1024 * 1024;
            while (lower < upper) {
                const std::size_t limit = lower + (upper - lower) / 2;
                error.clear();
                const auto candidate = client::detail::StateReducer::stale(synchronized->state, limit, error);
                if (candidate) {
                    upper = limit;
                } else {
                    lower = limit + 1;
                }
            }
            error.clear();
            boundedStale = client::detail::StateReducer::stale(synchronized->state, lower, error);
        }
        std::string impossibleError;
        auto impossibleStale = synchronized ? client::detail::StateReducer::stale(synchronized->state, 0, impossibleError) : std::nullopt;
        result.expectTrue(stale && staleAgain && stale->revision() == staleAgain->revision() &&
                              client::detail::StateReducer::serializeForTesting(*stale) ==
                                  client::detail::StateReducer::serializeForTesting(*staleAgain),
                          "reapplying stale reduction is idempotent and does not grow the retained State revision");
        result.expectTrue(retainedFixtureBytes > initialFixtureBytes && boundedStale && boundedStale->revision() == 0 &&
                              boundedStale->representationMode() == client::RepresentationMode::Unknown,
                          "a stale mutation that exceeds maximumDecodedStateBytes transactionally falls back to bounded empty State "
                          "(initial fixture bytes=" +
                              std::to_string(initialFixtureBytes) + ", retained fixture bytes=" + std::to_string(retainedFixtureBytes) +
                              ", bounded revision=" + std::to_string(boundedStale ? boundedStale->revision() : 9999) + ")");
        result.expectTrue(!impossibleStale && impossibleError.find("maximumDecodedStateBytes") != std::string::npos,
                          "a stale mutation reports a contained capacity error when even empty State cannot fit");
    }

    void testProjectionRefreshStaging(tests::support::TestResult& result) {
        constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();
        client::SessionInfo legacySession;
        legacySession.sessionId = "2";
        legacySession.syncMode = frontend::SyncMode::Replay;
        legacySession.serverCurrentSequence = frontend::SequenceNumber(4);
        std::string error;
        auto legacyStaging =
            client::detail::StateReducer::synchronizationStaging(legacySession, frontend::SequenceNumber(1), unlimited, true, error);
        const frontend::Json canonicalLegacy = legacyState();
        const frontend::FrontendEvent legacyEvent{frontend::SequenceNumber(2),
                                                  "thread.updated",
                                                  frontend::Json{{"thread", canonicalLegacy.at("threads").at(0)}},
                                                  frontend::Json::object()};
        const frontend::EventBatch legacyBatch{
            frontend::SequenceNumber(2), frontend::SequenceNumber(2), {legacyEvent}, frontend::Json::object()};
        std::optional<client::detail::StateReduction> validatedLegacy;
        if (legacyStaging)
            validatedLegacy =
                client::detail::StateReducer::validateSynchronizationEvents(*legacyStaging, legacyBatch, unlimited, true, error);
        frontend::FrontendEvent referentialLegacyEvent = content(frontend::SequenceNumber(3), "validated without retained state");
        referentialLegacyEvent.data["itemId"] = "item-only-in-discarded-projection";
        const frontend::EventBatch referentialLegacyBatch{
            frontend::SequenceNumber(3), frontend::SequenceNumber(3), {referentialLegacyEvent}, frontend::Json::object()};
        std::optional<client::detail::StateReduction> validatedReference;
        if (validatedLegacy)
            validatedReference = client::detail::StateReducer::validateSynchronizationEvents(
                validatedLegacy->state, referentialLegacyBatch, unlimited, true, error);
        result.expectTrue(legacyStaging && legacyStaging->representationMode() == client::RepresentationMode::LegacyV1 && validatedLegacy &&
                              validatedReference && validatedReference->state.visibleSequence() == frontend::SequenceNumber(3) &&
                              validatedReference->state.threads().empty() && validatedReference->changes.empty(),
                          "projection refresh validates legacy replay in a fresh representation cursor without mixing retained entities");

        client::SessionInfo expandedSession = legacySession;
        expandedSession.selectedRepresentationCapabilities = options().requestedCapabilities;
        error.clear();
        auto expandedStaging =
            client::detail::StateReducer::synchronizationStaging(expandedSession, frontend::SequenceNumber(1), unlimited, true, error);
        std::optional<client::detail::StateReduction> rejectedLegacy;
        if (expandedStaging)
            rejectedLegacy =
                client::detail::StateReducer::validateSynchronizationEvents(*expandedStaging, legacyBatch, unlimited, true, error);
        result.expectTrue(expandedStaging && expandedStaging->representationMode() == client::RepresentationMode::ExpandedV1 &&
                              !rejectedLegacy,
                          "all five selected representation capabilities require expanded replay during projection refresh");

        error.clear();
        const auto disabledLegacy =
            client::detail::StateReducer::synchronizationStaging(legacySession, frontend::SequenceNumber(1), unlimited, false, error);
        result.expectTrue(!disabledLegacy && error.find("legacy Frontend Protocol v1") != std::string::npos,
                          "legacy projection-refresh staging obeys allowLegacyV1");
    }

    void testHybridRepresentationAndAtomicBatchBoundaries(tests::support::TestResult& result) {
        constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();
        client::SessionInfo expandedSnapshotSession;
        expandedSnapshotSession.sessionId = "1";
        expandedSnapshotSession.syncMode = frontend::SyncMode::Snapshot;
        expandedSnapshotSession.serverCurrentSequence = frontend::SequenceNumber(7);
        expandedSnapshotSession.requestedRepresentationCapabilities = options().requestedCapabilities;
        expandedSnapshotSession.selectedRepresentationCapabilities = {
            frontend::FrontendCapability::CompleteBackendDomains,
        };
        std::string error;
        auto expandedSnapshot = client::detail::StateReducer::snapshot(
            client::detail::StateReducer::initial(),
            frontend::Snapshot{
                frontend::SequenceNumber(7),
                expandedState(),
                frontend::Json{{"safeSnapshotExtension", true},
                               {"scopeProjection",
                                frontend::Json{{"omittedFields",
                                                frontend::Json::array({"/provider/initialization/codexHome", "/items/0/commandOutput"})},
                                               {"redactedFields", frontend::Json::array({"/provider/lastError/message"})}}}}},
            expandedSnapshotSession,
            unlimited,
            64,
            true,
            error);
        const frontend::EventBatch legacyDiagnostics{
            frontend::SequenceNumber(8),
            frontend::SequenceNumber(9),
            {frontend::FrontendEvent{
                 frontend::SequenceNumber(8),
                 "diagnostics.updated",
                 frontend::Json{{"received", std::uint64_t{1}}, {"recent", frontend::Json::array({"legacy diagnostic"})}}},
             content(frontend::SequenceNumber(9), "legacy accumulated content")}};
        std::optional<client::detail::StateReduction> legacyEvents;
        if (expandedSnapshot) {
            legacyEvents =
                client::detail::StateReducer::events(expandedSnapshot->state, legacyDiagnostics, true, unlimited, 64, true, error);
        }
        result.expectTrue(
            expandedSnapshot && expandedSnapshot->state.representationMode() == client::RepresentationMode::ExpandedV1 &&
                expandedSnapshot->state.projectionMetadata().omittedFields ==
                    std::vector<std::string>{"/items/0/commandOutput", "/provider/initialization/codexHome"} &&
                expandedSnapshot->state.projectionMetadata().redactedFields == std::vector<std::string>{"/provider/lastError/message"} &&
                expandedSnapshot->state.provider().omittedFields == std::vector<std::string>{"/provider/initialization/codexHome"} &&
                !expandedSnapshot->state.compatibilityExtensions().contains("scopeProjection") &&
                expandedSnapshot->state.compatibilityExtensions().value("safeSnapshotExtension", false) && legacyEvents &&
                legacyEvents->state.diagnostics().value && legacyEvents->state.diagnostics().value->entries.size() == 1 &&
                legacyEvents->state.item("item-1") && legacyEvents->state.item("item-1")->commandOutput == "legacy accumulated content",
            "complete_backend_domains independently selects an expanded snapshot while unselected diagnostic and item families remain "
            "valid legacy events");

        client::SessionInfo legacySnapshotSession = expandedSnapshotSession;
        legacySnapshotSession.selectedRepresentationCapabilities = {
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
        };
        frontend::Json mixedLegacySnapshot = legacyState();
        mixedLegacySnapshot["items"] = frontend::Json::array({frontend::Json{{"id", "item-1"},
                                                                             {"threadId", "thread-1"},
                                                                             {"turnId", "turn-1"},
                                                                             {"type", "commandExecution"},
                                                                             {"commandOutput", "complete top-level item"}}});
        error.clear();
        auto legacySnapshot =
            client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                   frontend::Snapshot{frontend::SequenceNumber(7), std::move(mixedLegacySnapshot)},
                                                   legacySnapshotSession,
                                                   unlimited,
                                                   64,
                                                   true,
                                                   error);
        const frontend::EventBatch expandedProvider{
            frontend::SequenceNumber(8),
            frontend::SequenceNumber(8),
            {frontend::FrontendEvent{
                frontend::SequenceNumber(8),
                "provider.updated",
                frontend::Json{{"provider", expandedState().at("provider")}},
                frontend::Json{{"scopeProjection",
                                frontend::Json{{"omittedFields",
                                                frontend::Json::array({"/provider/initialization/codexHome", "/event/process.updated"})},
                                               {"redactedFields", frontend::Json::array({"/provider/lastError/message"})}}}}}}};
        std::optional<client::detail::StateReduction> expandedEvents;
        if (legacySnapshot)
            expandedEvents =
                client::detail::StateReducer::events(legacySnapshot->state, expandedProvider, true, unlimited, 64, true, error);
        result.expectTrue(
            legacySnapshot && legacySnapshot->state.representationMode() == client::RepresentationMode::LegacyV1 &&
                legacySnapshot->state.item("item-1") && legacySnapshot->state.item("item-1")->commandOutput == "complete top-level item" &&
                expandedEvents && expandedEvents->state.provider().value && expandedEvents->state.provider().value->ready &&
                expandedEvents->state.provider().omittedFields == std::vector<std::string>{"/provider/initialization/codexHome"} &&
                expandedEvents->state.projectionMetadata().omittedFields ==
                    std::vector<std::string>{"/event/process.updated", "/provider/initialization/codexHome"} &&
                expandedEvents->state.projectionMetadata().redactedFields == std::vector<std::string>{"/provider/lastError/message"} &&
                legacySnapshot->state.backendCursor().backendRevision == std::uint64_t{7} &&
                legacySnapshot->state.backendCursor().currentSequence == frontend::SequenceNumber(7) &&
                !legacySnapshot->state.compatibilityExtensions().contains("backendRevision") &&
                !legacySnapshot->state.compatibilityExtensions().contains("journal"),
            "legacy snapshots accept independently selected complete items and expanded notification events while retaining exact typed "
            "scope-projection metadata");

        client::SessionInfo completeSession = expandedSnapshotSession;
        completeSession.selectedRepresentationCapabilities = options().requestedCapabilities;
        frontend::Json completeState = expandedState();
        completeState["frontendSequenceExhausted"] = true;
        error.clear();
        auto completeSnapshot =
            client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                   frontend::Snapshot{frontend::SequenceNumber(20), std::move(completeState)},
                                                   completeSession,
                                                   unlimited,
                                                   64,
                                                   false,
                                                   error);
        const frontend::FrontendEvent extension{frontend::SequenceNumber(21),
                                                "codex.extension",
                                                frontend::Json{{"method", "vendor/future"}, {"params", frontend::Json{{"safe", true}}}}};
        std::optional<client::detail::StateReduction> extensionApplied;
        if (completeSnapshot) {
            extensionApplied =
                client::detail::StateReducer::events(completeSnapshot->state,
                                                     frontend::EventBatch{extension.sequence, extension.sequence, {extension}},
                                                     true,
                                                     unlimited,
                                                     64,
                                                     false,
                                                     error);
        }
        result.expectTrue(
            extensionApplied && extensionApplied->state.backendCursor().frontendSequenceExhausted == true &&
                !extensionApplied->state.compatibilityExtensions().contains("frontendSequenceExhausted") &&
                extensionApplied->state.compatibilityExtensions().contains("codexExtensions") &&
                extensionApplied->state.compatibilityExtensions().at("codexExtensions").size() == 1,
            "allowLegacyV1=false still accepts the bounded codex.extension compatibility mechanism under all five capabilities");

        const frontend::FrontendEvent first{
            frontend::SequenceNumber(22), "provider.updated", frontend::Json{{"provider", expandedState().at("provider")}}};
        const frontend::FrontendEvent split{
            frontend::SequenceNumber(22), "controller.updated", frontend::Json{{"controller", frontend::Json{{"present", false}}}}};
        error.clear();
        auto firstBatch = completeSnapshot
                              ? client::detail::StateReducer::events(completeSnapshot->state,
                                                                     frontend::EventBatch{first.sequence, first.sequence, {first}},
                                                                     true,
                                                                     unlimited,
                                                                     64,
                                                                     false,
                                                                     error)
                              : std::nullopt;
        auto beforeSplit = firstBatch ? firstBatch->state : client::State{};
        auto rejectedSplit =
            firstBatch
                ? client::detail::StateReducer::events(
                      firstBatch->state, frontend::EventBatch{split.sequence, split.sequence, {split}}, true, unlimited, 64, false, error)
                : std::nullopt;
        result.expectTrue(
            firstBatch && !rejectedSplit &&
                client::detail::StateReducer::serializeForTesting(firstBatch->state) ==
                    client::detail::StateReducer::serializeForTesting(beforeSplit) &&
                error.find("split") != std::string::npos,
            "expanded members sharing an occurrence sequence must remain in one EventBatch and a split second batch rolls back");

        error.clear();
        auto represented = completeSnapshot ? client::detail::StateReducer::synchronized(
                                                  completeSnapshot->state, frontend::SequenceNumber(23), completeSession, unlimited, error)
                                            : std::nullopt;
        client::SessionInfo reconnectSession = completeSession;
        reconnectSession.sessionId = "2";
        reconnectSession.syncMode = frontend::SyncMode::Replay;
        auto reconnect = represented
                             ? client::detail::StateReducer::beginSynchronization(represented->state, reconnectSession, unlimited, error)
                             : std::nullopt;
        auto ignoredFirst =
            reconnect ? client::detail::StateReducer::events(
                            *reconnect, frontend::EventBatch{first.sequence, first.sequence, {first}}, true, unlimited, 64, false, error)
                      : std::nullopt;
        const client::State ignoredState = ignoredFirst ? ignoredFirst->state : client::State{};
        auto rejectedIgnoredSplit =
            ignoredFirst
                ? client::detail::StateReducer::events(
                      ignoredFirst->state, frontend::EventBatch{split.sequence, split.sequence, {split}}, true, unlimited, 64, false, error)
                : std::nullopt;
        result.expectTrue(ignoredFirst && ignoredFirst->ignoredAlreadyAppliedEvents == 1 && !rejectedIgnoredSplit &&
                              client::detail::StateReducer::serializeForTesting(ignoredFirst->state) ==
                                  client::detail::StateReducer::serializeForTesting(ignoredState) &&
                              error.find("split") != std::string::npos,
                          "already represented replay members cannot split one expanded occurrence across EventBatch boundaries");
    }

    void testExpandedSnapshotAndEventConvergence(tests::support::TestResult& result) {
        constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();
        client::SessionInfo session;
        session.sessionId = "1";
        session.syncMode = frontend::SyncMode::Snapshot;
        session.serverCurrentSequence = frontend::SequenceNumber(2);
        session.requestedRepresentationCapabilities = options().requestedCapabilities;
        session.selectedRepresentationCapabilities = options().requestedCapabilities;

        const frontend::Json retainedProcess{{"processHandle", "process-retained"},
                                             {"lifecycle", "running"},
                                             {"stamp", {{"generation", std::uint64_t{1}}, {"freshness", "current"}}}};
        const frontend::Json addedProcess{{"processHandle", "process-added"},
                                          {"lifecycle", "running"},
                                          {"stamp", {{"generation", std::uint64_t{2}}, {"freshness", "current"}}}};

        frontend::Json baseline = expandedState();
        baseline["processes"] =
            frontend::Json{{"entries", frontend::Json::array({retainedProcess})}, {"truncation", {{"truncated", false}}}};
        frontend::Json replacement = baseline;
        replacement.at("processes").at("entries").push_back(addedProcess);

        std::string error;
        auto snapshot = client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                               frontend::Snapshot{frontend::SequenceNumber(2), replacement},
                                                               session,
                                                               unlimited,
                                                               64,
                                                               true,
                                                               error);
        auto snapshotComplete =
            snapshot ? client::detail::StateReducer::synchronized(snapshot->state, frontend::SequenceNumber(2), session, unlimited, error)
                     : std::nullopt;

        auto eventBaseline = client::detail::StateReducer::snapshot(client::detail::StateReducer::initial(),
                                                                    frontend::Snapshot{frontend::SequenceNumber(1), baseline},
                                                                    session,
                                                                    unlimited,
                                                                    64,
                                                                    true,
                                                                    error);
        auto eventBaselineComplete =
            eventBaseline
                ? client::detail::StateReducer::synchronized(eventBaseline->state, frontend::SequenceNumber(1), session, unlimited, error)
                : std::nullopt;
        const frontend::FrontendEvent processEvent{
            frontend::SequenceNumber(2), "process.updated", frontend::Json{{"process", addedProcess}}};
        const frontend::EventBatch processBatch{
            processEvent.sequence, processEvent.sequence, std::vector<frontend::FrontendEvent>{processEvent}};
        auto eventApplied =
            eventBaselineComplete
                ? client::detail::StateReducer::events(eventBaselineComplete->state, processBatch, false, unlimited, 64, true, error)
                : std::nullopt;

        frontend::Json snapshotState =
            snapshotComplete ? client::detail::StateReducer::serializeForTesting(snapshotComplete->state) : frontend::Json();
        frontend::Json eventState =
            eventApplied ? client::detail::StateReducer::serializeForTesting(eventApplied->state) : frontend::Json();
        // Local revision counts committed transactions, so the snapshot and
        // event paths intentionally differ only in that connection-local value.
        snapshotState.erase("revision");
        eventState.erase("revision");

        result.expectTrue(snapshotComplete && eventApplied && snapshotState == eventState &&
                              snapshotComplete->state.process("process-retained") && snapshotComplete->state.process("process-added") &&
                              eventApplied->state.process("process-retained") && eventApplied->state.process("process-added") &&
                              client::detail::StateReducer::serializeChangesForTesting(eventApplied->changes) ==
                                  frontend::Json::array({frontend::Json{{"type", "process.updated"}, {"processHandle", "process-added"}}}),
                          "an expanded snapshot and the equivalent singular expanded event converge on the same normalized State while "
                          "preserving unrelated collection entries");
    }

    std::optional<client::ProjectionFingerprintInput> decodeProjectionFingerprintFixtureInput(const frontend::Json& encoded) {
        if (!encoded.is_object())
            return std::nullopt;
        client::ProjectionFingerprintInput result;
        const auto decodeIdentity = [](std::string_view identity, auto decoder) {
            auto decoded = decoder(identity);
            if (decoded || !identity.starts_with("invalid:"))
                return decoded;
            using Optional = decltype(decoded);
            using Enum = typename Optional::value_type;
            int numeric = 0;
            const std::string_view encodedNumber = identity.substr(std::string_view("invalid:").size());
            const auto parsed = std::from_chars(encodedNumber.data(), encodedNumber.data() + encodedNumber.size(), numeric);
            if (parsed.ec != std::errc{} || parsed.ptr != encodedNumber.data() + encodedNumber.size())
                return Optional{};
            return Optional{static_cast<Enum>(numeric)};
        };
        const auto readCapabilities = [&](std::string_view name, std::vector<frontend::FrontendCapability>& destination) {
            const auto found = encoded.find(std::string(name));
            if (found == encoded.end() || !found->is_array())
                return false;
            for (const frontend::Json& value : *found) {
                if (!value.is_string())
                    return false;
                const auto capability = decodeIdentity(value.get<std::string>(), frontend::frontendCapabilityFromString);
                if (!capability)
                    return false;
                destination.push_back(*capability);
            }
            return true;
        };
        if (!readCapabilities("requestedRepresentationCapabilities", result.requestedRepresentationCapabilities) ||
            !readCapabilities("selectedRepresentationCapabilities", result.selectedRepresentationCapabilities))
            return std::nullopt;

        const auto readOptionalSet = [&](std::string_view name, auto decoder, auto& destination) {
            const auto found = encoded.find(std::string(name));
            if (found == encoded.end() || !found->is_object())
                return false;
            const auto present = found->find("present");
            if (present == found->end() || !present->is_boolean())
                return false;
            if (!present->get<bool>()) {
                destination.reset();
                return true;
            }
            const auto values = found->find("values");
            if (values == found->end() || !values->is_array())
                return false;
            destination.emplace();
            for (const frontend::Json& value : *values) {
                if (!value.is_string())
                    return false;
                const auto decoded = decodeIdentity(value.get<std::string>(), decoder);
                if (!decoded)
                    return false;
                destination->push_back(*decoded);
            }
            return true;
        };
        if (!readOptionalSet("permittedScopes", frontend::frontendScopeFromString, result.permittedScopes) ||
            !readOptionalSet("permittedMethods", generated::definedMethodFromString, result.permittedMethods) ||
            !readOptionalSet("availableMethods", generated::definedMethodFromString, result.availableMethods))
            return std::nullopt;

        const auto continuity = encoded.find("continuityKey");
        if (continuity == encoded.end() || !continuity->is_object() || !continuity->contains("present") ||
            !continuity->at("present").is_boolean())
            return std::nullopt;
        if (continuity->at("present").get<bool>()) {
            const auto value = continuity->find("value");
            if (value == continuity->end() || !value->is_string())
                return std::nullopt;
            result.continuityKey = value->get<std::string>();
        }

        const auto metadata = encoded.find("explicitProjectionMetadata");
        if (metadata == encoded.end() || !metadata->is_object() || !metadata->contains("present") || !metadata->at("present").is_boolean())
            return std::nullopt;
        if (metadata->at("present").get<bool>()) {
            const auto value = metadata->find("value");
            if (value == metadata->end())
                return std::nullopt;
            result.explicitProjectionMetadata = *value;
        }
        return result;
    }

    void testProjectionFingerprintFixtures(tests::support::TestResult& result) {
        std::ifstream input(AISUITE_CODEX_FRONTEND_CLIENT_FIXTURE_DIR "/conformance.json");
        frontend::Json fixture;
        input >> fixture;
        const auto cases = fixture.find("projectionFingerprintCases");
        bool allPassed = cases != fixture.end() && cases->is_array() && cases->size() >= 5;
        std::vector<std::pair<std::string, std::string>> actualByName;
        if (allPassed) {
            for (const frontend::Json& testCase : *cases) {
                const std::string name = testCase.value("name", "unnamed");
                const auto decoded =
                    testCase.contains("input") ? decodeProjectionFingerprintFixtureInput(testCase.at("input")) : std::nullopt;
                bool casePassed = decoded && testCase.contains("expectedCanonical") && testCase.at("expectedCanonical").is_string();
                std::string actual;
                if (decoded) {
                    try {
                        actual = client::projectionFingerprint(*decoded);
                    } catch (...) {
                        casePassed = false;
                    }
                }
                if (casePassed)
                    casePassed = actual == testCase.at("expectedCanonical").get<std::string>();
                if (casePassed && testCase.contains("equivalentInputs")) {
                    if (!testCase.at("equivalentInputs").is_array()) {
                        casePassed = false;
                    } else {
                        for (const frontend::Json& equivalent : testCase.at("equivalentInputs")) {
                            const auto decodedEquivalent = decodeProjectionFingerprintFixtureInput(equivalent);
                            if (!decodedEquivalent || client::projectionFingerprint(*decodedEquivalent) != actual) {
                                casePassed = false;
                                break;
                            }
                        }
                    }
                }
                actualByName.emplace_back(name, std::move(actual));
                allPassed = allPassed && casePassed;
                if (!casePassed)
                    result.expectTrue(false, "projection fingerprint fixture failed: " + name);
            }
            for (const frontend::Json& testCase : *cases) {
                const auto differentFrom = testCase.find("differentFrom");
                if (differentFrom == testCase.end())
                    continue;
                const auto current = std::find_if(actualByName.begin(), actualByName.end(), [&](const auto& value) {
                    return value.first == testCase.value("name", "");
                });
                const auto other = std::find_if(actualByName.begin(), actualByName.end(), [&](const auto& value) {
                    return differentFrom->is_string() && value.first == differentFrom->get<std::string>();
                });
                allPassed = allPassed && current != actualByName.end() && other != actualByName.end() && current->second != other->second;
            }
        }

        result.expectTrue(allPassed,
                          "language-independent projection fingerprint fixtures fix exact canonical serialization, set-order invariance, "
                          "absent-versus-present inputs, invalid-enum safety, and exclusion of observed non-projection facts");
    }

    void testReducerFixtures(tests::support::TestResult& result) {
        std::ifstream input(AISUITE_CODEX_FRONTEND_CLIENT_FIXTURE_DIR "/conformance.json");
        frontend::Json fixture;
        input >> fixture;
        bool allPassed = fixture.value("format", "") == "snodec.codex-frontend.client-reducer.v3" &&
                         fixture.value("fixtureVersion", 0) == 3 && fixture.contains("inputContract") &&
                         fixture.at("inputContract").is_object() && fixture.contains("cases") && fixture.at("cases").is_array() &&
                         fixture.at("cases").size() >= 53;
        for (const frontend::Json& testCase : fixture.at("cases")) {
            const std::string name = testCase.value("name", "unnamed");
            bool casePassed = testCase.value("fixtureVersion", 0) == 3 && testCase.contains("initialNormalizedState") &&
                              testCase.contains("orderedProtocolInputs") && testCase.at("orderedProtocolInputs").is_array() &&
                              testCase.contains("expectedState") && testCase.contains("expectedVisibleSequence") &&
                              testCase.contains("expectedSynchronizedThrough") && testCase.contains("expectedFreshness") &&
                              testCase.contains("expectedChanges") && testCase.contains("expectedCounts") &&
                              testCase.contains("expectedSynchronizationResult") && testCase.contains("connectionRemainsOpen") &&
                              testCase.contains("expectedError");
            if (casePassed) {
                casePassed = std::ranges::all_of(testCase.at("orderedProtocolInputs"), [](const frontend::Json& protocolInput) {
                    return protocolInput.value("kind", "") != "events" ||
                           (protocolInput.contains("synchronizing") && protocolInput.at("synchronizing").is_boolean());
                });
            }
            client::State state = client::detail::StateReducer::initial();
            casePassed = casePassed && client::detail::StateReducer::serializeForTesting(state) == testCase.at("initialNormalizedState") &&
                         client::detail::StateReducer::accountingIsConsistentForTesting(state);
            client::SessionInfo session;
            session.sessionId = "1";
            session.syncMode = frontend::SyncMode::Snapshot;
            session.serverCurrentSequence = frontend::SequenceNumber(0);
            session.requestedRepresentationCapabilities = options().requestedCapabilities;
            const bool expandedFixture = std::any_of(
                testCase.at("orderedProtocolInputs").begin(), testCase.at("orderedProtocolInputs").end(), [](const frontend::Json& input) {
                    const auto state = input.find("state");
                    return input.value("kind", "") == "snapshot" && state != input.end() && state->is_object() &&
                           state->contains("provider") && !state->contains("backendRevision");
                });
            if (expandedFixture) {
                session.selectedRepresentationCapabilities = options().requestedCapabilities;
            }
            const std::size_t maximumBytes = testCase.value("maximumBytes", std::numeric_limits<std::size_t>::max());
            const std::size_t eventMaximumBytes = testCase.value("eventMaximumBytes", maximumBytes);
            const std::size_t maximumDiagnostics = testCase.value("maximumRetainedDiagnostics", std::size_t{64});
            const bool allowLegacy = testCase.value("allowLegacyV1", true);
            std::vector<client::Change> changes;
            std::size_t receivedEvents = 0;
            std::size_t appliedEvents = 0;
            std::size_t ignoredEvents = 0;
            bool connectionOpen = true;
            std::optional<std::string> actualError;
            frontend::Json actualSynchronizationResult;
            std::optional<client::ProjectionFingerprintInput> candidateProjection;
            std::optional<bool> projectionContinuityCompatible;
            std::optional<client::State> projectionValidationState;
            bool projectionRefreshRequired = false;
            bool projectionSnapshotStreaming = false;
            bool projectionReplayValidated = false;
            bool serverSelectedSnapshotFallback = false;

            for (const frontend::Json& protocolInput : testCase.at("orderedProtocolInputs")) {
                if (!connectionOpen)
                    break;
                if (protocolInput.value("protocol", "") != frontend::ProtocolIdentity ||
                    protocolInput.value("version", 0) != frontend::ProtocolVersion) {
                    connectionOpen = false;
                    actualError = "fixture protocol envelope does not identify Frontend Protocol v1";
                    break;
                }
                const std::string kind = protocolInput.value("kind", "");
                std::string error;
                if (kind == "transport.disconnected") {
                    auto stale = client::detail::StateReducer::stale(state, maximumBytes, error);
                    if (!stale) {
                        connectionOpen = false;
                        actualError = error;
                        break;
                    }
                    state = std::move(*stale);
                } else if (kind == "projection.continuity") {
                    const auto retained = protocolInput.contains("retained")
                                              ? decodeProjectionFingerprintFixtureInput(protocolInput.at("retained"))
                                              : std::nullopt;
                    const auto candidate = protocolInput.contains("candidate")
                                               ? decodeProjectionFingerprintFixtureInput(protocolInput.at("candidate"))
                                               : std::nullopt;
                    const auto expected = protocolInput.find("expectedCompatible");
                    if (!retained || !candidate || expected == protocolInput.end() || !expected->is_boolean()) {
                        connectionOpen = false;
                        actualError = "fixture projection-continuity input is malformed";
                        break;
                    }
                    const bool compatible = client::projectionFingerprint(*retained) == client::projectionFingerprint(*candidate);
                    if (compatible != expected->get<bool>()) {
                        connectionOpen = false;
                        actualError = "fixture projection-continuity decision disagrees with the canonical fingerprint";
                        break;
                    }
                    projectionContinuityCompatible = compatible;
                    candidateProjection = *candidate;
                } else if (kind == "welcome") {
                    const auto mode = frontend::syncModeFromString(protocolInput.value("syncMode", "replay"));
                    if (!mode) {
                        connectionOpen = false;
                        actualError = "fixture Welcome contains an invalid synchronization mode";
                        break;
                    }
                    session.sessionId = protocolInput.value("sessionId", "2");
                    session.syncMode = *mode;
                    session.serverCurrentSequence =
                        frontend::SequenceNumber(protocolInput.at("serverCurrentSequence").get<std::uint64_t>());
                    if (candidateProjection) {
                        session.requestedRepresentationCapabilities = candidateProjection->requestedRepresentationCapabilities;
                        session.selectedRepresentationCapabilities = candidateProjection->selectedRepresentationCapabilities;
                        session.permittedScopes = candidateProjection->permittedScopes;
                        session.permittedMethods = candidateProjection->permittedMethods;
                        session.availableMethods = candidateProjection->availableMethods;
                    }
                    projectionRefreshRequired =
                        *mode == frontend::SyncMode::Replay && projectionContinuityCompatible == std::optional(false);
                    projectionSnapshotStreaming = false;
                    projectionReplayValidated = false;
                    serverSelectedSnapshotFallback = *mode == frontend::SyncMode::Snapshot && state.synchronizedThrough().has_value();
                    projectionValidationState.reset();
                    if (projectionRefreshRequired) {
                        projectionValidationState = client::detail::StateReducer::synchronizationStaging(
                            session, state.synchronizedThrough(), maximumBytes, allowLegacy, error);
                        if (!projectionValidationState) {
                            connectionOpen = false;
                            actualError = error;
                            break;
                        }
                    } else if (*mode == frontend::SyncMode::Replay) {
                        auto rebound = client::detail::StateReducer::beginSynchronization(state, session, maximumBytes, error);
                        if (!rebound) {
                            connectionOpen = false;
                            actualError = error;
                            break;
                        }
                        state = std::move(*rebound);
                    }
                } else if (kind == "projection.snapshot.response") {
                    if (!projectionRefreshRequired || !projectionValidationState || !projectionReplayValidated) {
                        connectionOpen = false;
                        actualError = "fixture projection snapshot response has no incompatible replay to replace";
                        break;
                    }
                    projectionRefreshRequired = false;
                    projectionSnapshotStreaming = true;
                    projectionValidationState.reset();
                    receivedEvents = 0;
                    appliedEvents = 0;
                    ignoredEvents = 0;
                } else if (kind == "snapshot") {
                    if (const auto projection = protocolInput.find("projection"); projection != protocolInput.end()) {
                        const auto decodedProjection = decodeProjectionFingerprintFixtureInput(*projection);
                        if (!decodedProjection) {
                            connectionOpen = false;
                            actualError = "fixture snapshot projection fingerprint input is malformed";
                            break;
                        }
                        session.requestedRepresentationCapabilities = decodedProjection->requestedRepresentationCapabilities;
                        session.selectedRepresentationCapabilities = decodedProjection->selectedRepresentationCapabilities;
                        session.permittedScopes = decodedProjection->permittedScopes;
                        session.permittedMethods = decodedProjection->permittedMethods;
                        session.availableMethods = decodedProjection->availableMethods;
                    }
                    const frontend::SequenceNumber sequence(protocolInput.at("sequence").get<std::uint64_t>());
                    if (protocolInput.value("initial", false))
                        session.serverCurrentSequence = sequence;
                    auto reduction = client::detail::StateReducer::snapshot(
                        state,
                        frontend::Snapshot{
                            sequence, protocolInput.at("state"), protocolInput.value("extensions", frontend::Json::object())},
                        session,
                        maximumBytes,
                        maximumDiagnostics,
                        allowLegacy,
                        error);
                    if (!reduction) {
                        connectionOpen = false;
                        actualError = error;
                        break;
                    }
                    state = reduction->state;
                    changes.insert(changes.end(), reduction->changes.begin(), reduction->changes.end());
                } else if (kind == "events") {
                    const auto synchronizing = protocolInput.find("synchronizing");
                    if (synchronizing == protocolInput.end() || !synchronizing->is_boolean()) {
                        connectionOpen = false;
                        actualError = "fixture event input must declare its synchronization phase";
                        break;
                    }
                    std::vector<frontend::FrontendEvent> events;
                    for (const frontend::Json& encoded : protocolInput.at("events")) {
                        events.push_back({frontend::SequenceNumber(encoded.at("sequence").get<std::uint64_t>()),
                                          encoded.at("type").get<std::string>(),
                                          encoded.at("data"),
                                          encoded.value("extensions", frontend::Json::object())});
                    }
                    frontend::EventBatch batch{frontend::SequenceNumber(protocolInput.at("fromSequence").get<std::uint64_t>()),
                                               frontend::SequenceNumber(protocolInput.at("toSequence").get<std::uint64_t>()),
                                               std::move(events)};
                    const std::size_t rebuildCountBefore = client::detail::StateReducer::debugAccountingRebuildCountForTesting();
                    std::optional<client::detail::StateReduction> reduction;
                    if (projectionRefreshRequired) {
                        if (!synchronizing->get<bool>() || !projectionValidationState) {
                            connectionOpen = false;
                            actualError = "fixture incompatible replay is not a synchronization stream";
                            break;
                        }
                        reduction = client::detail::StateReducer::validateSynchronizationEvents(
                            *projectionValidationState, batch, eventMaximumBytes, allowLegacy, error);
                    } else {
                        reduction = client::detail::StateReducer::events(
                            state, batch, synchronizing->get<bool>(), eventMaximumBytes, maximumDiagnostics, allowLegacy, error);
                    }
                    if (client::detail::StateReducer::debugAccountingRebuildCountForTesting() != rebuildCountBefore) {
                        connectionOpen = false;
                        actualError = "ordinary fixture event admission performed a complete State-ledger rebuild";
                        break;
                    }
                    if (!reduction) {
                        connectionOpen = false;
                        actualError = error;
                        break;
                    }
                    if (projectionRefreshRequired) {
                        projectionValidationState = reduction->state;
                    } else {
                        state = reduction->state;
                        changes.insert(changes.end(), reduction->changes.begin(), reduction->changes.end());
                    }
                    receivedEvents += reduction->receivedEvents;
                    appliedEvents += reduction->appliedEvents;
                    ignoredEvents += reduction->ignoredAlreadyAppliedEvents;
                } else if (kind == "sync.complete") {
                    const frontend::SequenceNumber sequence(protocolInput.at("sequence").get<std::uint64_t>());
                    if (projectionRefreshRequired && !projectionSnapshotStreaming) {
                        if (!projectionValidationState) {
                            connectionOpen = false;
                            actualError = "fixture projection validation state is absent at sync.complete";
                            break;
                        }
                        auto reduction =
                            client::detail::StateReducer::synchronized(*projectionValidationState, sequence, session, maximumBytes, error);
                        if (!reduction) {
                            connectionOpen = false;
                            actualError = error;
                            break;
                        }
                        projectionValidationState = reduction->state;
                        projectionReplayValidated = true;
                        if (!client::detail::StateReducer::accountingIsConsistentForTesting(*projectionValidationState)) {
                            connectionOpen = false;
                            actualError = "projection-validation State accounting disagrees at sync.complete";
                            break;
                        }
                        continue;
                    }
                    auto reduction = client::detail::StateReducer::synchronized(state, sequence, session, maximumBytes, error);
                    if (!reduction) {
                        connectionOpen = false;
                        actualError = error;
                        break;
                    }
                    state = reduction->state;
                    changes.insert(changes.end(), reduction->changes.begin(), reduction->changes.end());
                    if (!protocolInput.value("initial", false)) {
                        const bool expectedSnapshotFallback = projectionSnapshotStreaming || serverSelectedSnapshotFallback;
                        if (protocolInput.value("snapshotFallback", false) != expectedSnapshotFallback) {
                            connectionOpen = false;
                            actualError = "fixture snapshot-fallback result disagrees with the executed synchronization path";
                            break;
                        }
                        actualSynchronizationResult = frontend::Json::object();
                        actualSynchronizationResult["mode"] = protocolInput.value("mode", "replay");
                        actualSynchronizationResult["synchronizedThrough"] = sequence.value();
                        actualSynchronizationResult["state"] = client::detail::StateReducer::serializeForTesting(state);
                        actualSynchronizationResult["receivedEvents"] = receivedEvents;
                        actualSynchronizationResult["appliedEvents"] = appliedEvents;
                        actualSynchronizationResult["ignoredAlreadyAppliedEvents"] = ignoredEvents;
                        actualSynchronizationResult["snapshotFallback"] = protocolInput.value("snapshotFallback", false);
                    }
                    projectionSnapshotStreaming = false;
                } else {
                    connectionOpen = false;
                    actualError = "fixture contains an unknown protocol input kind";
                }
                if (connectionOpen && !client::detail::StateReducer::accountingIsConsistentForTesting(state)) {
                    connectionOpen = false;
                    actualError = "incremental State accounting disagrees with the canonical fixture metric";
                }
                if (connectionOpen && projectionValidationState &&
                    !client::detail::StateReducer::accountingIsConsistentForTesting(*projectionValidationState)) {
                    connectionOpen = false;
                    actualError = "projection-validation State accounting disagrees with the canonical fixture metric";
                }
            }

            const frontend::Json actualState = client::detail::StateReducer::serializeForTesting(state);
            const frontend::Json actualChanges = client::detail::StateReducer::serializeChangesForTesting(changes);
            const frontend::Json actualVisible =
                state.visibleSequence() ? frontend::Json(state.visibleSequence()->value()) : frontend::Json(nullptr);
            const frontend::Json actualSynchronized =
                state.synchronizedThrough() ? frontend::Json(state.synchronizedThrough()->value()) : frontend::Json(nullptr);
            frontend::Json actualCounts = frontend::Json::object();
            actualCounts["received"] = receivedEvents;
            actualCounts["applied"] = appliedEvents;
            actualCounts["ignored"] = ignoredEvents;
            const frontend::Json& expectedError = testCase.at("expectedError");
            const bool errorMatches =
                expectedError.is_null()
                    ? !actualError.has_value()
                    : actualError && expectedError.is_string() && actualError->find(expectedError.get<std::string>()) != std::string::npos;
            const bool synchronizationMatches = testCase.at("expectedSynchronizationResult").is_null()
                                                    ? actualSynchronizationResult.is_null()
                                                    : actualSynchronizationResult == testCase.at("expectedSynchronizationResult");
            casePassed = casePassed && client::detail::StateReducer::accountingIsConsistentForTesting(state) &&
                         actualState == testCase.at("expectedState") && actualVisible == testCase.at("expectedVisibleSequence") &&
                         actualSynchronized == testCase.at("expectedSynchronizedThrough") &&
                         actualState.value("freshness", "") == testCase.at("expectedFreshness").get<std::string>() &&
                         actualChanges == testCase.at("expectedChanges") && actualCounts == testCase.at("expectedCounts") &&
                         synchronizationMatches && connectionOpen == testCase.at("connectionRemainsOpen").get<bool>() && errorMatches;
            allPassed = allPassed && casePassed;
            if (!casePassed) {
                result.expectTrue(false, "reducer fixture failed: " + name);
            }
        }
        result.expectTrue(allPassed,
                          "the C++ reducer executes every literal v3 cross-language fixture and compares complete normalized State, typed "
                          "Changes, cursors, synchronization outcomes, connection disposition, and errors");
    }

    void testExactIncrementalAccounting(tests::support::TestResult& result) {
        client::SessionInfo session;
        session.sessionId = "accounting-session";
        session.syncMode = frontend::SyncMode::Snapshot;
        session.serverCurrentSequence = frontend::SequenceNumber(1);
        session.requestedRepresentationCapabilities = options().requestedCapabilities;
        session.selectedRepresentationCapabilities = options().requestedCapabilities;
        const frontend::Snapshot snapshot{frontend::SequenceNumber(1), expandedState()};
        const client::State initial = client::detail::StateReducer::initial();
        std::string error;
        auto admitted =
            client::detail::StateReducer::snapshot(initial, snapshot, session, std::numeric_limits<std::size_t>::max(), 64, true, error);
        const std::optional<std::size_t> actual =
            admitted ? client::detail::StateReducer::referenceBytesForTesting(admitted->state) : std::nullopt;
        const auto atLimit = [&](std::size_t maximum) {
            std::string localError;
            return client::detail::StateReducer::snapshot(initial, snapshot, session, maximum, 64, true, localError).has_value();
        };
        result.expectTrue(admitted && actual && *actual > 0 &&
                              client::detail::StateReducer::accountedBytesForTesting(admitted->state) == actual && !atLimit(*actual - 1) &&
                              atLimit(*actual) && atLimit(*actual + 1) && !atLimit(0),
                          "decoded-State admission preserves exact actual-1/actual/actual+1 and zero-capacity boundaries");

        client::detail::StateReducer::resetDebugAccountingVerificationCountForTesting();
        client::detail::StateReducer::resetDebugAccountingRebuildCountForTesting();
        const frontend::Json escaped = frontend::Json{{"itemId", "item-1"},
                                                      {"threadId", "thread-1"},
                                                      {"turnId", "turn-1"},
                                                      {"channel", "agentText"},
                                                      {"content", "quoted \\\"value\\\" \\ path — Καλημέρα 😀"},
                                                      {"contentTruncated", true},
                                                      {"droppedContentBytes", std::numeric_limits<std::uint64_t>::max()}};
        frontend::FrontendEvent escapedEvent{frontend::SequenceNumber(2), "item.content.updated", escaped};
        frontend::EventBatch escapedBatch{
            frontend::SequenceNumber(2), frontend::SequenceNumber(2), std::vector<frontend::FrontendEvent>{std::move(escapedEvent)}};
        std::string eventError;
        auto updated = admitted ? client::detail::StateReducer::events(
                                      admitted->state, escapedBatch, false, std::numeric_limits<std::size_t>::max(), 64, true, eventError)
                                : std::nullopt;
        const std::optional<std::size_t> updatedBytes =
            updated ? client::detail::StateReducer::referenceBytesForTesting(updated->state) : std::nullopt;
        const auto eventAtLimit = [&](std::size_t maximum) {
            std::string localError;
            return client::detail::StateReducer::events(admitted->state, escapedBatch, false, maximum, 64, true, localError);
        };
        const std::optional<std::size_t> admittedBytes =
            admitted ? client::detail::StateReducer::accountedBytesForTesting(admitted->state) : std::nullopt;
        std::string malformedError;
        const frontend::Json malformedData{{"itemId", "missing-item"}, {"channel", "agentText"}, {"content", "must roll back"}};
        frontend::FrontendEvent malformedEvent{frontend::SequenceNumber(2), "item.content.updated", malformedData};
        frontend::EventBatch malformedBatch{
            frontend::SequenceNumber(2), frontend::SequenceNumber(2), std::vector<frontend::FrontendEvent>{std::move(malformedEvent)}};
        const auto malformed =
            admitted ? client::detail::StateReducer::events(
                           admitted->state, malformedBatch, false, std::numeric_limits<std::size_t>::max(), 64, true, malformedError)
                     : std::nullopt;
        result.expectTrue(updatedBytes && *updatedBytes > 0 && !eventAtLimit(*updatedBytes - 1) && eventAtLimit(*updatedBytes) &&
                              eventAtLimit(*updatedBytes + 1) && !eventAtLimit(0) && !malformed &&
                              client::detail::StateReducer::accountedBytesForTesting(admitted->state) == admittedBytes,
                          "ordinary singular-event admission preserves exact byte boundaries and malformed/capacity rollback leaves the "
                          "committed ledger unchanged");
        const frontend::Json extensionData{
            {"method", "vendor/accounting"},
            {"params", {{"signed", std::numeric_limits<std::int64_t>::min()}, {"opaque", frontend::Json::array({"escaped\nvalue", "λ"})}}}};
        frontend::FrontendEvent extensionEvent{frontend::SequenceNumber(3), "codex.extension", extensionData};
        frontend::EventBatch extensionBatch{
            frontend::SequenceNumber(3), frontend::SequenceNumber(3), std::vector<frontend::FrontendEvent>{std::move(extensionEvent)}};
        std::string extensionError;
        auto extended = updated
                            ? client::detail::StateReducer::events(
                                  updated->state, extensionBatch, false, std::numeric_limits<std::size_t>::max(), 64, true, extensionError)
                            : std::nullopt;
        frontend::Json emptyProjectionExtensions{
            {"scopeProjection", {{"omittedFields", frontend::Json::array()}, {"redactedFields", frontend::Json::array()}}}};
        frontend::FrontendEvent emptyProjectionEvent{
            frontend::SequenceNumber(4), "item.content.updated", escaped, std::move(emptyProjectionExtensions)};
        frontend::EventBatch emptyProjectionBatch{frontend::SequenceNumber(4),
                                                  frontend::SequenceNumber(4),
                                                  std::vector<frontend::FrontendEvent>{std::move(emptyProjectionEvent)}};
        std::string emptyProjectionError;
        auto emptyProjection =
            extended
                ? client::detail::StateReducer::events(
                      extended->state, emptyProjectionBatch, false, std::numeric_limits<std::size_t>::max(), 64, true, emptyProjectionError)
                : std::nullopt;
        result.expectTrue(updated && extended && emptyProjection &&
                              client::detail::StateReducer::accountingIsConsistentForTesting(updated->state) &&
                              client::detail::StateReducer::accountingIsConsistentForTesting(extended->state) &&
                              client::detail::StateReducer::accountingIsConsistentForTesting(emptyProjection->state) &&
                              client::detail::StateReducer::accountingRejectsOverflowForTesting() &&
                              client::detail::StateReducer::accountingRejectsUnderflowForTesting(),
                          "incremental accounting is exact for escaped UTF-8, maximum unsigned data, and fails closed on arithmetic "
                          "overflow or underflow");
#ifndef NDEBUG
        result.expectTrue(client::detail::StateReducer::debugAccountingInvariantEnabledForTesting() &&
                              client::detail::StateReducer::debugAccountingVerificationCountForTesting() > 0 &&
                              client::detail::StateReducer::debugAccountingRebuildCountForTesting() == 0,
                          "Debug State commits execute the canonical serializer equivalence invariant while ordinary singular events "
                          "perform no complete ledger rebuild");
#else
        result.expectTrue(!client::detail::StateReducer::debugAccountingInvariantEnabledForTesting() &&
                              client::detail::StateReducer::debugAccountingVerificationCountForTesting() == 0 &&
                              client::detail::StateReducer::debugAccountingRebuildCountForTesting() == 0,
                          "optimized State commits compile out the canonical serializer equivalence invariant");
#endif
    }

    void testStateAdmissionHotPathGuard(tests::support::TestResult& result) {
        std::ifstream input(AISUITE_CODEX_FRONTEND_CLIENT_STATE_SOURCE);
        const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const std::size_t stateFitsBegin = source.find("bool stateFits(");
        const std::size_t stateFitsEnd = source.find("bool advanceRevision(", stateFitsBegin);
        const std::size_t refreshBegin = source.find("bool refreshCommitStateSections(");
        const std::size_t refreshEnd = source.find("bool accountingFailure(", refreshBegin);
        const std::size_t accountedBegin = source.find("std::optional<std::size_t> accountedStateBytes(");
        const std::size_t accountedEnd = source.find("thread_local std::size_t DebugAccountingVerificationCount", accountedBegin);
        const std::size_t applyExpandedBegin = source.find("bool applyExpanded(");
        const std::size_t ordinaryEventsEnd = source.find("frontend::Json serializeChanges(", applyExpandedBegin);
        const bool rangesPresent = input.is_open() && stateFitsBegin != std::string::npos && stateFitsEnd != std::string::npos &&
                                   refreshBegin != std::string::npos && refreshEnd != std::string::npos &&
                                   accountedBegin != std::string::npos && accountedEnd != std::string::npos &&
                                   applyExpandedBegin != std::string::npos && ordinaryEventsEnd != std::string::npos;
        const std::string stateFitsBody = rangesPresent ? source.substr(stateFitsBegin, stateFitsEnd - stateFitsBegin) : std::string{};
        const std::string refreshBody = rangesPresent ? source.substr(refreshBegin, refreshEnd - refreshBegin) : std::string{};
        const std::string accountedBody = rangesPresent ? source.substr(accountedBegin, accountedEnd - accountedBegin) : std::string{};
        const std::string ordinaryEventHelpers =
            rangesPresent ? source.substr(applyExpandedBegin, ordinaryEventsEnd - applyExpandedBegin) : std::string{};
        std::size_t rebuildMentions = 0;
        for (std::size_t position = source.find("rebuildStateSizeLedger"); position != std::string::npos;
             position = source.find("rebuildStateSizeLedger", position + 1))
            ++rebuildMentions;
        result.expectTrue(rangesPresent && stateFitsBody.find("accountedStateBytes(state)") != std::string::npos &&
                              stateFitsBody.find("encodeState(") == std::string::npos &&
                              stateFitsBody.find(".dump(") == std::string::npos &&
                              stateFitsBody.find("#ifndef NDEBUG") != std::string::npos &&
                              stateFitsBody.find("referenceStateBytes(state)") != std::string::npos &&
                              refreshBody.find("rebuildStateSizeLedger") == std::string::npos &&
                              accountedBody.find("rebuildStateSizeLedger") == std::string::npos &&
                              ordinaryEventHelpers.find("rebuildStateSizeLedger") == std::string::npos &&
                              ordinaryEventHelpers.find("encodeState(") == std::string::npos && rebuildMentions == 7,
                          "ordinary production State admission consumes the ledger and cannot silently restore whole-State encoding; "
                          "the reference serializer remains Debug-only, while complete initial/live snapshot replacement may rebuild");
    }

} // namespace

int main() {
    tests::support::TestResult result;
    testSparseLiveAndTransactionalState(result);
    testReadyLiveSnapshotBarriers(result);
    testLiveRegressionRelationshipsAndDuplicateIdentities(result);
    testRevisionExhaustion(result);
    testTypedProviderRealtimeAndItemStamp(result);
    testHiddenSuffixAndReconnect(result);
    testReconnectSessionRebinding(result);
    testPartialReplayAppendDeduplication(result);
    testFreshReplayDisconnectClearsSession(result);
    testExpandedOccurrenceGroupsAndFamilies(result);
    testDiagnosticRetentionAndLegacyState(result);
    testExplicitReplayAndSnapshotFallback(result);
    testExplicitSynchronizationFailureAndOrdering(result);
    testAdvancedGeneratedSynchronization(result);
    testDeferredSynchronizationFromNotifications(result);
    testProjectionFingerprintAndRefresh(result);
    testStateCapacity(result);
    testLegacyOptionalValidationNoticeBoundsAndStaleCapacity(result);
    testProjectionRefreshStaging(result);
    testHybridRepresentationAndAtomicBatchBoundaries(result);
    testExpandedSnapshotAndEventConvergence(result);
    testProjectionFingerprintFixtures(result);
    testReducerFixtures(result);
    testExactIncrementalAccounting(result);
    testStateAdmissionHotPathGuard(result);
    return result.processResult();
}
