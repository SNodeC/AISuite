/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "ai/openai/codex/frontend/client/GeneratedBindings.h"
#include "ai/openai/codex/frontend/client/ProjectionFingerprint.h"
#include "ai/openai/codex/frontend/client/detail/ClientTestAccess.h"
#include "support/TestResult.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace client = ai::openai::codex::frontend::client;

    struct Harness {
        std::vector<client::OutboundMessage> messages;
        std::vector<client::ConnectionState> states;
        std::size_t closes = 0;
        std::size_t diagnostics = 0;
        bool rejectCommands = false;
        bool throwOnCommand = false;

        client::TransportCallbacks transport() {
            return {
                [this](client::OutboundMessage message) {
                    const bool command = message.kind == client::OutboundKind::Command;
                    messages.push_back(std::move(message));
                    if (command && throwOnCommand) {
                        throw std::runtime_error("transport send sentinel");
                    }
                    if (command && rejectCommands) {
                        return client::SendResult{client::SendStatus::Backpressure,
                                                  client::TransportError{"transport backpressure sentinel", true}};
                    }
                    return client::SendResult{client::SendStatus::Accepted, std::nullopt};
                },
                [this](std::string) {
                    ++closes;
                },
            };
        }

        client::ClientCallbacks callbacks() {
            return {
                [this](const client::ConnectionStateChange& change) {
                    states.push_back(change.current);
                },
                {},
                {},
                {},
                {},
                [this](const client::Diagnostic&) {
                    ++diagnostics;
                },
            };
        }
    };

    client::ClientOptions options() {
        client::ClientOptions value;
        value.credentialProvider = [] {
            return client::AuthenticationContext{frontend::BearerCredential{"BEARER_SENTINEL"}, "bearer-profile:test"};
        };
        return value;
    }

    std::vector<frontend::FrontendCapability> definedCapabilities() {
        std::vector<frontend::FrontendCapability> result;
        for (const frontend::generated::CapabilityMetadata& metadata : frontend::generated::AllCapabilities) {
            if (metadata.defined) {
                result.push_back(static_cast<frontend::FrontendCapability>(metadata.id));
            }
        }
        return result;
    }

    std::vector<frontend::FrontendCapability> implementedCapabilities() {
        return {
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
            frontend::FrontendCapability::AuthenticatedFrontend,
        };
    }

    frontend::CapabilityAdvertisement
    advertisedCapabilities(std::vector<frontend::FrontendCapability> implemented = implementedCapabilities(),
                           std::optional<std::vector<frontend::FrontendCapability>> permitted = std::nullopt) {
        return {definedCapabilities(), implemented, permitted.value_or(implemented), frontend::Json::object()};
    }

    frontend::Json expandedState() {
        return {{"provider",
                 {{"lifecycle", "ready"},
                  {"generation", std::uint64_t{1}},
                  {"desiredRunning", true},
                  {"recovery", {{"status", "idle"}, {"attempts", std::uint64_t{0}}}}}},
                {"controller", frontend::Json::object()},
                {"sessions", frontend::Json::array()},
                {"threads", frontend::Json::array()},
                {"turns", frontend::Json::array()},
                {"items", frontend::Json::array()},
                {"pendingRequests", frontend::Json::array()},
                {"capacity", frontend::Json::object()},
                {"truncation", {{"truncated", false}}}};
    }

    frontend::Json minimalLegacyState() {
        return {{"backendRevision", std::uint64_t{1}},
                {"lifecycle", "ready"},
                {"diagnostics", {{"received", std::uint64_t{0}}, {"recent", frontend::Json::array()}}},
                {"sessions", frontend::Json::array()},
                {"threadList", {{"hasLoadedPage", false}, {"complete", true}, {"pagesLoaded", std::uint64_t{0}}}},
                {"threads", frontend::Json::array()},
                {"pendingRequests", frontend::Json::array()},
                {"codexExtensions", frontend::Json::array()},
                {"omittedCodexExtensions", std::uint64_t{0}},
                {"journal", {{"oldestReplayableAfter", std::uint64_t{0}}, {"currentSequence", std::uint64_t{1}}}},
                {"sequenceExhausted", false}};
    }

    void makeReady(client::Connection& connection, frontend::SequenceNumber sequence = frontend::SequenceNumber(7)) {
        (void) connection.receive(frontend::ServerMessage{frontend::Welcome{"session-1",
                                                                            frontend::SessionRole::Observer,
                                                                            sequence,
                                                                            frontend::SyncMode::Snapshot,
                                                                            frontend::Json::object(),
                                                                            advertisedCapabilities()}});
        (void) connection.receive(frontend::ServerMessage{frontend::Snapshot{sequence, expandedState()}});
        (void) connection.receive(frontend::ServerMessage{frontend::SyncComplete{sequence}});
    }

    void testHandshakeAndLifecycle(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());

        result.expectTrue(connection.isOpen() && !connection.isTransportConnected() && harness.messages.empty() &&
                              sdk.connectionState() == client::ConnectionState::Connecting,
                          "opening a logical attachment emits no Hello before the physical transport reports connected");

        connection.transportConnected();
        connection.transportConnected();
        const auto decodedHello = frontend::Codec::decodeClient(std::string_view(harness.messages.front().compactJson));
        const auto* hello = decodedHello ? std::get_if<frontend::Hello>(&decodedHello.value()) : nullptr;
        const std::vector<frontend::FrontendCapability> exactCapabilities{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
        };
        result.expectTrue(harness.messages.size() == 1 && harness.messages.front().kind == client::OutboundKind::Hello &&
                              harness.messages.front().sensitive && hello != nullptr && hello->capabilities == exactCapabilities &&
                              !hello->resumeAfter && sdk.connectionState() == client::ConnectionState::Authenticating,
                          "transportConnected emits exactly one sensitive Hello with exactly five representation capabilities");

        makeReady(connection);
        const std::string expectedProjectionFingerprint =
            client::projectionFingerprint(client::ProjectionFingerprintInput{exactCapabilities,
                                                                             exactCapabilities,
                                                                             std::optional<std::string>{"bearer-profile:test"},
                                                                             std::nullopt,
                                                                             std::nullopt,
                                                                             std::nullopt,
                                                                             std::nullopt});
        result.expectTrue(sdk.isReady() && sdk.session() && sdk.session()->sessionId == "session-1" &&
                              sdk.synchronizedThrough() == frontend::SequenceNumber(7) && sdk.state().projectionFingerprintMetadata() &&
                              sdk.state().projectionFingerprintMetadata()->canonical == expectedProjectionFingerprint,
                          "Welcome, Snapshot, and SyncComplete reach Ready with the canonical projection fingerprint attached to State");

        client::Connection rejected = sdk.openConnection(harness.transport());
        result.expectTrue(!rejected.isOpen() && sdk.hasActiveConnection(), "one Client permits at most one active physical attachment");

        connection.transportDisconnected(client::TransportError{"peer closed", true});
        result.expectTrue(
            !connection.isOpen() && sdk.connectionState() == client::ConnectionState::Disconnected &&
                sdk.state().projectionFingerprintMetadata() &&
                sdk.state().projectionFingerprintMetadata()->canonical == expectedProjectionFingerprint,
            "an unexpected disconnect returns the reusable Client to Disconnected while retained State keeps its projection identity");

        client::Connection reconnect = sdk.openConnection(harness.transport());
        reconnect.transportConnected();
        const auto reconnectHelloMessage = frontend::Codec::decodeClient(std::string_view(harness.messages.back().compactJson));
        const auto* reconnectHello = reconnectHelloMessage ? std::get_if<frontend::Hello>(&reconnectHelloMessage.value()) : nullptr;
        result.expectTrue(reconnectHello != nullptr && reconnectHello->resumeAfter == frontend::SequenceNumber(7) &&
                              reconnect.generation() == 2,
                          "unchanged authenticated continuity resumes from synchronizedThrough on a new connection generation");

        sdk.close();
        sdk.close();
        result.expectTrue(sdk.connectionState() == client::ConnectionState::Closed && harness.closes == 1,
                          "Client close is terminal, idempotent, and asks the active transport to close at most once");
    }

    void testMalformedOptionalLegacySnapshotsAreContained(tests::support::TestResult& result) {
        struct MalformedCase {
            std::string name;
            std::function<void(frontend::Json&)> mutate;
        };
        const std::vector<MalformedCase> cases{
            {"controller",
             [](frontend::Json& state) {
                 state["controller"] = true;
             }},
            {"domains",
             [](frontend::Json& state) {
                 state["domains"] = "invalid";
             }},
            {"nested domain",
             [](frontend::Json& state) {
                 state["domains"] = {{"accounts", frontend::Json::array()}};
             }},
            {"top-level domain",
             [](frontend::Json& state) {
                 state["accounts"] = frontend::Json::array();
             }},
            {"processes",
             [](frontend::Json& state) {
                 state["processes"] = "invalid";
             }},
            {"filesystem watches",
             [](frontend::Json& state) {
                 state["filesystemWatches"] = "invalid";
             }},
            {"fuzzy searches",
             [](frontend::Json& state) {
                 state["fuzzySearches"] = "invalid";
             }},
            {"fuzzy-search alias",
             [](frontend::Json& state) {
                 state["fuzzySearchSessions"] = "invalid";
             }},
            {"notices",
             [](frontend::Json& state) {
                 state["notices"] = "invalid";
             }},
            {"activities",
             [](frontend::Json& state) {
                 state["activities"] = "invalid";
             }},
            {"collection entries",
             [](frontend::Json& state) {
                 state["processes"] = {{"entries", "invalid"}};
             }},
            {"collection truncation",
             [](frontend::Json& state) {
                 state["notices"] = {{"entries", frontend::Json::array()}, {"truncation", "invalid"}};
             }},
            {"capacity",
             [](frontend::Json& state) {
                 state["capacity"] = "invalid";
             }},
            {"capacity member",
             [](frontend::Json& state) {
                 state["capacity"] = {{"sessions", "invalid"}};
             }},
            {"complete items",
             [](frontend::Json& state) {
                 state["items"] = frontend::Json::object();
             }},
            {"diagnostics",
             [](frontend::Json& state) {
                 state["diagnostics"]["recent"] = "invalid";
             }},
            {"bounded root schema",
             [](frontend::Json& state) {
                 for (std::size_t index = 0; index < 4100; ++index)
                     state["aaa-extension-" + std::to_string(index)] = index;
             }},
            {"bounded diagnostics schema",
             [](frontend::Json& state) {
                 for (std::size_t index = 0; index < 4100; ++index)
                     state["diagnostics"]["aaa-extension-" + std::to_string(index)] = index;
             }},
        };

        bool allRejected = true;
        for (const MalformedCase& malformed : cases) {
            Harness harness;
            client::Client sdk(options(), harness.callbacks());
            client::Connection connection = sdk.openConnection(harness.transport());
            connection.transportConnected();
            const client::ReceiveResult welcomeResult = connection.receive(frontend::ServerMessage{
                frontend::Welcome{"legacy", frontend::SessionRole::Observer, frontend::SequenceNumber(1), frontend::SyncMode::Snapshot}});
            frontend::Json state = minimalLegacyState();
            malformed.mutate(state);
            const client::ReceiveResult snapshotResult =
                connection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(1), std::move(state)}});
            allRejected = allRejected && welcomeResult.accepted && !snapshotResult.accepted && !connection.isOpen() &&
                          sdk.connectionState() == client::ConnectionState::Disconnected && harness.closes == 1;
            if (!allRejected) {
                result.expectTrue(false, "malformed optional legacy snapshot was not contained: " + malformed.name);
                return;
            }
        }
        result.expectTrue(allRejected,
                          "every malformed known optional legacy snapshot field is rejected through the public noexcept receive path "
                          "without escaping or remaining connected");
    }

    void testHandshakeReentrancy(tests::support::TestResult& result) {
        Harness credentialHarness;
        client::Client* credentialClient = nullptr;
        client::ClientOptions closingOptions = options();
        closingOptions.credentialProvider = [&credentialClient] {
            credentialClient->close("credential provider closed its client");
            return client::AuthenticationContext{frontend::BearerCredential{"CLOSE_CREDENTIAL_SENTINEL"}, "verified-local:test"};
        };
        client::Client closedDuringCredential(std::move(closingOptions), credentialHarness.callbacks());
        credentialClient = &closedDuringCredential;
        client::Connection credentialConnection = closedDuringCredential.openConnection(credentialHarness.transport());
        credentialConnection.transportConnected();
        result.expectTrue(credentialHarness.messages.empty() && credentialHarness.closes == 1 && !credentialConnection.isOpen() &&
                              !closedDuringCredential.hasActiveConnection() &&
                              closedDuringCredential.connectionState() == client::ConnectionState::Closed &&
                              client::detail::ClientTestAccess::erasedTransientBytes(closedDuringCredential) >=
                                  std::string_view("CLOSE_CREDENTIAL_SENTINEL").size(),
                          "closing from CredentialProvider prevents Hello serialization, wipes the returned credential, and preserves "
                          "terminal Client state");

        Harness sendHarness;
        client::Connection sendConnection;
        client::Client closedDuringSend(options(), sendHarness.callbacks());
        client::TransportCallbacks transport{
            [&sendHarness, &sendConnection](client::OutboundMessage message) {
                sendHarness.messages.push_back(std::move(message));
                sendConnection.close("transport send callback closed its connection");
                return client::SendResult{client::SendStatus::Accepted, std::nullopt};
            },
            [&sendHarness](std::string) {
                ++sendHarness.closes;
            },
        };
        sendConnection = closedDuringSend.openConnection(std::move(transport));
        sendConnection.transportConnected();
        result.expectTrue(sendHarness.messages.size() == 1 && sendHarness.messages.front().kind == client::OutboundKind::Hello &&
                              sendHarness.closes == 1 && !sendConnection.isOpen() && !closedDuringSend.hasActiveConnection() &&
                              closedDuringSend.connectionState() == client::ConnectionState::Disconnected,
                          "closing from the Hello send callback cannot transition a detached Client back to Authenticating");
    }

    void testCapabilityFactsAndRequirements(tests::support::TestResult& result) {
        std::vector<frontend::FrontendCapability> implemented = implementedCapabilities();
        implemented.push_back(frontend::FrontendCapability::MultiTransport);
        implemented.push_back(frontend::FrontendCapability::CppClientSdk);
        std::vector<frontend::FrontendCapability> permitted = implemented;
        std::erase(permitted, frontend::FrontendCapability::MultiTransport);

        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        (void) connection.receive(frontend::ServerMessage{frontend::Welcome{"modern",
                                                                            frontend::SessionRole::Observer,
                                                                            frontend::SequenceNumber(1),
                                                                            frontend::SyncMode::Snapshot,
                                                                            frontend::Json::object(),
                                                                            advertisedCapabilities(implemented, permitted)}});

        const client::CapabilityStatus representation = sdk.capabilityStatus(frontend::FrontendCapability::CompleteBackendDomains);
        const client::CapabilityStatus topology = sdk.capabilityStatus(frontend::FrontendCapability::MultiTransport);
        const client::CapabilityStatus product = sdk.capabilityStatus(frontend::FrontendCapability::CppClientSdk);
        const client::CapabilityStatus futureProduct = sdk.capabilityStatus(frontend::FrontendCapability::QtUi);
        const std::optional<client::SessionInfo> session = sdk.session();
        result.expectTrue(
            representation.defined == client::Availability::Yes && representation.implemented == client::Availability::Yes &&
                representation.permitted == client::Availability::Yes && topology.defined == client::Availability::Yes &&
                topology.implemented == client::Availability::Yes && topology.permitted == client::Availability::No &&
                product.defined == client::Availability::Yes && product.implemented == client::Availability::Yes &&
                product.permitted == client::Availability::Yes && futureProduct.defined == client::Availability::Yes &&
                futureProduct.implemented == client::Availability::No && futureProduct.permitted == client::Availability::No,
            "modern capability status keeps protocol definition, runtime implementation, and permission as independent facts");
        result.expectTrue(
            session && session->selectedRepresentationCapabilities == options().requestedCapabilities &&
                session->observedMechanismCapabilities == implementedCapabilities() &&
                session->observedTopologyCapabilities == std::vector{frontend::FrontendCapability::MultiTransport} &&
                session->observedProductCapabilities == std::vector{frontend::FrontendCapability::CppClientSdk},
            "SessionInfo separates requested/selected representation, observed mechanism, topology, and product capabilities");

        Harness disabledHarness;
        client::Client disabledSdk(options(), disabledHarness.callbacks());
        client::Connection disabledConnection = disabledSdk.openConnection(disabledHarness.transport());
        disabledConnection.transportConnected();
        (void) disabledConnection.receive(frontend::ServerMessage{frontend::Welcome{"disabled",
                                                                                    frontend::SessionRole::Observer,
                                                                                    frontend::SequenceNumber(1),
                                                                                    frontend::SyncMode::Snapshot,
                                                                                    frontend::Json::object(),
                                                                                    advertisedCapabilities()}});
        const client::CapabilityStatus disabledProduct = disabledSdk.capabilityStatus(frontend::FrontendCapability::CppClientSdk);
        result.expectTrue(disabledProduct.defined == client::Availability::Yes && disabledProduct.implemented == client::Availability::No &&
                              disabledProduct.permitted == client::Availability::No,
                          "a generated product capability remains defined when the modern server reports it unimplemented");

        Harness legacyHarness;
        client::Client legacySdk(options(), legacyHarness.callbacks());
        client::Connection legacyConnection = legacySdk.openConnection(legacyHarness.transport());
        legacyConnection.transportConnected();
        (void) legacyConnection.receive(frontend::ServerMessage{
            frontend::Welcome{"legacy", frontend::SessionRole::Observer, frontend::SequenceNumber(1), frontend::SyncMode::Snapshot}});
        const client::CapabilityStatus legacyProduct = legacySdk.capabilityStatus(frontend::FrontendCapability::QtUi);
        result.expectTrue(legacyProduct.defined == client::Availability::Yes &&
                              legacyProduct.implemented == client::Availability::Unknown &&
                              legacyProduct.permitted == client::Availability::Unknown,
                          "legacy Welcome preserves generated definition truth while implementation and permission remain unknown");

        Harness requiredHarness;
        client::ClientOptions requiredOptions = options();
        requiredOptions.requiredCapabilities = {frontend::FrontendCapability::CppClientSdk};
        client::Client requiredSdk(std::move(requiredOptions), requiredHarness.callbacks());
        client::Connection requiredConnection = requiredSdk.openConnection(requiredHarness.transport());
        requiredConnection.transportConnected();
        const auto requiredHello = frontend::Codec::decodeClient(std::string_view(requiredHarness.messages.front().compactJson));
        const auto* hello = requiredHello ? std::get_if<frontend::Hello>(&requiredHello.value()) : nullptr;
        (void) requiredConnection.receive(frontend::ServerMessage{frontend::Welcome{"required",
                                                                                    frontend::SessionRole::Observer,
                                                                                    frontend::SequenceNumber(1),
                                                                                    frontend::SyncMode::Snapshot,
                                                                                    frontend::Json::object(),
                                                                                    advertisedCapabilities(implemented, implemented)}});
        result.expectTrue(hello && hello->capabilities == options().requestedCapabilities &&
                              std::find(hello->capabilities->begin(),
                                        hello->capabilities->end(),
                                        frontend::FrontendCapability::CppClientSdk) == hello->capabilities->end() &&
                              requiredConnection.isOpen(),
                          "a required observed product is checked after Welcome without being added to Hello representation requests");

        Harness missingHarness;
        client::ClientOptions missingOptions = options();
        missingOptions.requiredCapabilities = {frontend::FrontendCapability::CppClientSdk};
        client::Client missingSdk(std::move(missingOptions), missingHarness.callbacks());
        client::Connection missingConnection = missingSdk.openConnection(missingHarness.transport());
        missingConnection.transportConnected();
        (void) missingConnection.receive(frontend::ServerMessage{frontend::Welcome{"missing",
                                                                                   frontend::SessionRole::Observer,
                                                                                   frontend::SequenceNumber(1),
                                                                                   frontend::SyncMode::Snapshot,
                                                                                   frontend::Json::object(),
                                                                                   advertisedCapabilities()}});
        result.expectTrue(!missingConnection.isOpen() && missingSdk.connectionState() == client::ConnectionState::Disconnected,
                          "a missing required observed capability terminates initial synchronization");

        Harness duplicateAvailableHarness;
        client::Client duplicateAvailableSdk(options(), duplicateAvailableHarness.callbacks());
        client::Connection duplicateAvailableConnection = duplicateAvailableSdk.openConnection(duplicateAvailableHarness.transport());
        duplicateAvailableConnection.transportConnected();
        frontend::Welcome duplicateAvailable{"duplicateAvailable",
                                             frontend::SessionRole::Observer,
                                             frontend::SequenceNumber(1),
                                             frontend::SyncMode::Snapshot,
                                             frontend::Json::object(),
                                             advertisedCapabilities()};
        duplicateAvailable.availableMethods = std::vector<std::string>{"thread.list", "thread.list"};
        (void) duplicateAvailableConnection.receive(frontend::ServerMessage{std::move(duplicateAvailable)});
        result.expectTrue(!duplicateAvailableConnection.isOpen() && duplicateAvailableHarness.closes == 1,
                          "duplicate available-method identities are rejected rather than silently canonicalized");

        Harness unknownPermittedHarness;
        client::Client unknownPermittedSdk(options(), unknownPermittedHarness.callbacks());
        client::Connection unknownPermittedConnection = unknownPermittedSdk.openConnection(unknownPermittedHarness.transport());
        unknownPermittedConnection.transportConnected();
        frontend::Welcome unknownPermitted{"unknownPermitted",
                                           frontend::SessionRole::Observer,
                                           frontend::SequenceNumber(1),
                                           frontend::SyncMode::Snapshot,
                                           frontend::Json::object(),
                                           advertisedCapabilities()};
        unknownPermitted.permittedMethods = std::vector<std::string>{"future.unknown"};
        (void) unknownPermittedConnection.receive(frontend::ServerMessage{std::move(unknownPermitted)});
        result.expectTrue(!unknownPermittedConnection.isOpen() && unknownPermittedHarness.closes == 1,
                          "unknown permitted-method identities are rejected rather than dropped from the projection fingerprint");

        Harness duplicateCapabilityHarness;
        client::Client duplicateCapabilitySdk(options(), duplicateCapabilityHarness.callbacks());
        client::Connection duplicateCapabilityConnection = duplicateCapabilitySdk.openConnection(duplicateCapabilityHarness.transport());
        duplicateCapabilityConnection.transportConnected();
        frontend::CapabilityAdvertisement duplicateAdvertisement = advertisedCapabilities();
        duplicateAdvertisement.implemented.push_back(frontend::FrontendCapability::CompleteBackendDomains);
        frontend::Welcome duplicateCapability{"duplicateCapability",
                                              frontend::SessionRole::Observer,
                                              frontend::SequenceNumber(1),
                                              frontend::SyncMode::Snapshot,
                                              frontend::Json::object(),
                                              std::move(duplicateAdvertisement)};
        (void) duplicateCapabilityConnection.receive(frontend::ServerMessage{std::move(duplicateCapability)});
        result.expectTrue(!duplicateCapabilityConnection.isOpen() && duplicateCapabilityHarness.closes == 1,
                          "duplicate advertised capability facts are rejected as protocol inconsistency");
    }

    void testLegacyPolicy(tests::support::TestResult& result) {
        Harness absentHarness;
        client::ClientOptions absentOptions = options();
        absentOptions.allowLegacyV1 = false;
        client::Client absentSdk(std::move(absentOptions), absentHarness.callbacks());
        client::Connection absentConnection = absentSdk.openConnection(absentHarness.transport());
        absentConnection.transportConnected();
        (void) absentConnection.receive(frontend::ServerMessage{
            frontend::Welcome{"legacy", frontend::SessionRole::Observer, frontend::SequenceNumber(1), frontend::SyncMode::Snapshot}});
        result.expectTrue(!absentConnection.isOpen(), "allowLegacyV1 false rejects a legacy Welcome without expanded capability selection");

        Harness legacySnapshotHarness;
        client::ClientOptions legacySnapshotOptions = options();
        legacySnapshotOptions.allowLegacyV1 = false;
        client::Client legacySnapshotSdk(std::move(legacySnapshotOptions), legacySnapshotHarness.callbacks());
        client::Connection legacySnapshotConnection = legacySnapshotSdk.openConnection(legacySnapshotHarness.transport());
        legacySnapshotConnection.transportConnected();
        (void) legacySnapshotConnection.receive(frontend::ServerMessage{frontend::Welcome{"strict",
                                                                                          frontend::SessionRole::Observer,
                                                                                          frontend::SequenceNumber(2),
                                                                                          frontend::SyncMode::Snapshot,
                                                                                          frontend::Json::object(),
                                                                                          advertisedCapabilities()}});
        (void) legacySnapshotConnection.receive(
            frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(2), frontend::Json::object()}});
        result.expectTrue(!legacySnapshotConnection.isOpen(),
                          "allowLegacyV1 false rejects legacy snapshot normalization even after a modern Welcome");

        Harness expandedHarness;
        client::ClientOptions expandedOptions = options();
        expandedOptions.allowLegacyV1 = false;
        client::Client expandedSdk(std::move(expandedOptions), expandedHarness.callbacks());
        client::Connection expandedConnection = expandedSdk.openConnection(expandedHarness.transport());
        expandedConnection.transportConnected();
        (void) expandedConnection.receive(frontend::ServerMessage{frontend::Welcome{"expanded",
                                                                                    frontend::SessionRole::Observer,
                                                                                    frontend::SequenceNumber(3),
                                                                                    frontend::SyncMode::Snapshot,
                                                                                    frontend::Json::object(),
                                                                                    advertisedCapabilities()}});
        (void) expandedConnection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(3), expandedState()}});
        (void) expandedConnection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(3)}});
        result.expectTrue(expandedSdk.isReady() && expandedSdk.state().representationMode() == client::RepresentationMode::ExpandedV1,
                          "allowLegacyV1 false accepts a capability-selected expanded snapshot without unrelated discovery arrays");

        const frontend::FrontendEvent legacyEvent{
            frontend::SequenceNumber(4), "thread.updated", frontend::Json{{"thread", {{"id", "legacy-thread"}}}}};
        const frontend::EventBatch legacyBatch{frontend::SequenceNumber(4), frontend::SequenceNumber(4), std::vector{legacyEvent}};
        (void) expandedConnection.receive(frontend::ServerMessage{legacyBatch});
        result.expectTrue(!expandedConnection.isOpen(), "allowLegacyV1 false rejects a legacy live-event family");
    }

    void testCorrelationAndFailures(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        makeReady(connection);

        std::size_t observations = 0;
        client::ClientCallbacks callbacks = harness.callbacks();
        callbacks.onProtocolMessage = [&observations](const frontend::ServerMessage&) {
            ++observations;
        };
        sdk.setCallbacks(std::move(callbacks));

        std::size_t completions = 0;
        bool succeeded = false;
        client::Submission submission = sdk.submit(
            generated::CompleteCommandParameters{
                generated::MethodParameters<generated::MethodId::ControllerAcquire>{frontend::Json::object()}},
            [&](const client::GeneratedOperationResult& operation) {
                ++completions;
                succeeded = operation.succeeded() && generated::commandMethod(*operation.value) == generated::MethodId::ControllerAcquire;
            });
        const auto encodedCommand = frontend::Codec::decodeDefinedCommand(std::string_view(harness.messages.back().compactJson));
        result.expectTrue(submission && encodedCommand && encodedCommand.value().requestId == submission.requestId->value() &&
                              generated::commandMethod(encodedCommand.value().parameters) == generated::MethodId::ControllerAcquire &&
                              sdk.pendingOperationCount() == 1,
                          "the SDK generates and correlates a bounded request ID through the generated MethodId authority");

        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(submission.requestId->value(), frontend::Json{{"role", "controller"}})});
        result.expectTrue(completions == 1 && succeeded && sdk.pendingOperationCount() == 0 && observations == 1,
                          "a schema-valid result completes its operation and is observed exactly once after semantic acceptance");

        std::size_t commandFailures = 0;
        client::Submission failedSubmission =
            sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                           frontend::Json::object()}},
                       [&](const client::GeneratedOperationResult& operation) {
                           commandFailures += operation.error && operation.error->origin == client::ErrorOrigin::Command ? 1U : 0U;
                       });
        (void) connection.receive(frontend::ServerMessage{frontend::Response::failure(
            failedSubmission.requestId->value(),
            frontend::CommandError{frontend::ErrorCode::Conflict, "controller is already owned", std::nullopt, frontend::Json::object()})});
        result.expectTrue(commandFailures == 1 && connection.isOpen() && sdk.isReady() && observations == 2,
                          "a normal command error completes once, remains usable, and is observed after correlation");

        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(submission.requestId->value(), frontend::Json{{"role", "controller"}})});
        result.expectTrue(completions == 1 && !connection.isOpen() && sdk.connectionState() == client::ConnectionState::Disconnected &&
                              harness.closes == 1 && observations == 2,
                          "a duplicate or unsolicited response is contained as a connection-local protocol failure without observation");
    }

    void testPendingDisconnectAndCallbackContainment(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        makeReady(connection);

        std::size_t completions = 0;
        client::Submission pending =
            sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                           frontend::Json::object()}},
                       [&](const client::GeneratedOperationResult& operation) {
                           completions += operation.error && operation.error->origin == client::ErrorOrigin::Transport ? 1U : 0U;
                       });
        const std::size_t messagesBeforeDisconnect = harness.messages.size();
        connection.transportDisconnected(client::TransportError{"connection lost", true});
        connection.transportDisconnected(client::TransportError{"duplicate disconnect", true});
        result.expectTrue(pending && completions == 1 && sdk.pendingOperationCount() == 0 &&
                              sdk.connectionState() == client::ConnectionState::Disconnected,
                          "disconnect completes every pending operation exactly once with a transport error");

        client::Connection reconnect = sdk.openConnection(harness.transport());
        reconnect.transportConnected();
        result.expectTrue(harness.messages.size() == messagesBeforeDisconnect + 1 &&
                              harness.messages.back().kind == client::OutboundKind::Hello,
                          "reconnect emits only a new Hello and never retries an unacknowledged command");
        reconnect.transportDisconnected();

        Harness throwingHarness;
        client::ClientCallbacks throwingCallbacks = throwingHarness.callbacks();
        throwingCallbacks.onConnectionStateChanged = [](const client::ConnectionStateChange&) {
            throw std::runtime_error("callback sentinel");
        };
        client::Client callbackClient(options(), std::move(throwingCallbacks));
        client::Connection callbackConnection = callbackClient.openConnection(throwingHarness.transport());
        callbackConnection.transportConnected();
        makeReady(callbackConnection);
        result.expectTrue(callbackClient.isReady() && throwingHarness.diagnostics > 0,
                          "application callback exceptions are contained and reported without corrupting lifecycle state");
    }

    void testExceptionContainmentAndCapacity(tests::support::TestResult& result) {
        Harness harness;
        client::ClientOptions failing = options();
        failing.credentialProvider = []() -> client::AuthenticationContext {
            throw std::runtime_error("credential sentinel");
        };
        client::Client sdk(std::move(failing), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        result.expectTrue(!connection.isOpen() && sdk.connectionState() == client::ConnectionState::Disconnected && harness.closes == 1 &&
                              harness.messages.empty(),
                          "credential-provider exceptions are contained without exposing or sending a partial Hello");

        bool invalidRequiredRejected = false;
        try {
            client::ClientOptions invalid = options();
            invalid.requestedCapabilities.clear();
            invalid.requiredCapabilities = {frontend::FrontendCapability::CompleteBackendDomains};
            client::Client invalidClient(std::move(invalid));
        } catch (const std::invalid_argument&) {
            invalidRequiredRejected = true;
        }
        result.expectTrue(invalidRequiredRejected,
                          "a required representation capability is rejected unless it is also in the Hello representation request");

        const auto configurationRejected = [](client::ClientOptions invalid) {
            try {
                client::Client invalidClient(std::move(invalid));
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        client::ClientOptions duplicateRequested = options();
        duplicateRequested.requestedCapabilities.push_back(duplicateRequested.requestedCapabilities.front());
        client::ClientOptions productRequested = options();
        productRequested.requestedCapabilities = {frontend::FrontendCapability::CppClientSdk};
        client::ClientOptions duplicateRequired = options();
        duplicateRequired.requiredCapabilities = {
            frontend::FrontendCapability::CppClientSdk,
            frontend::FrontendCapability::CppClientSdk,
        };
        client::ClientOptions invalidCapability = options();
        using CapabilityUnderlying = std::underlying_type_t<frontend::FrontendCapability>;
        invalidCapability.requiredCapabilities = {
            static_cast<frontend::FrontendCapability>(std::numeric_limits<CapabilityUnderlying>::max()),
        };
        result.expectTrue(configurationRejected(std::move(duplicateRequested)) && configurationRejected(std::move(productRequested)) &&
                              configurationRejected(std::move(duplicateRequired)) && configurationRejected(std::move(invalidCapability)),
                          "ClientOptions rejects duplicate, invalid, and non-representation Hello capability configuration");

        Harness continuityHarness;
        client::ClientOptions overlongContinuity = options();
        overlongContinuity.credentialProvider = [] {
            return client::AuthenticationContext{frontend::BearerCredential{"OVERLONG_CONTINUITY_CREDENTIAL_SENTINEL"},
                                                 std::string(257, 'c')};
        };
        client::Client continuityClient(std::move(overlongContinuity), continuityHarness.callbacks());
        client::Connection continuityConnection = continuityClient.openConnection(continuityHarness.transport());
        continuityConnection.transportConnected();
        result.expectTrue(!continuityConnection.isOpen() && continuityHarness.messages.empty() && continuityHarness.closes == 1 &&
                              client::detail::ClientTestAccess::erasedTransientBytes(continuityClient) >=
                                  std::string_view("OVERLONG_CONTINUITY_CREDENTIAL_SENTINEL").size(),
                          "an over-bound continuity key fails before Hello and wipes its transient bearer credential");

        constexpr std::string_view ShortCredential = "SSO_SECRET_12";
        Harness helloFailureHarness;
        client::ClientOptions helloFailureOptions = options();
        helloFailureOptions.credentialProvider = [] {
            return client::AuthenticationContext{frontend::BearerCredential{"SSO_SECRET_12"}, "short-credential"};
        };
        client::Client helloFailureClient(std::move(helloFailureOptions), helloFailureHarness.callbacks());
        client::Connection helloFailureConnection = helloFailureClient.openConnection(helloFailureHarness.transport());
        client::detail::ClientTestAccess::failNextHelloConstruction(helloFailureClient);
        helloFailureConnection.transportConnected();
        result.expectTrue(
            !helloFailureConnection.isOpen() && helloFailureHarness.messages.empty() && helloFailureHarness.closes == 1 &&
                client::detail::ClientTestAccess::erasedTransientBytes(helloFailureClient) >= ShortCredential.size() * 2,
            "an exception after direct Hello credential placement wipes both short-string credential copies and closes locally");

        Harness shortCredentialHarness;
        client::ClientOptions shortCredentialOptions = options();
        shortCredentialOptions.credentialProvider = [] {
            return client::AuthenticationContext{frontend::BearerCredential{"SSO_SECRET_12"}, "short-credential"};
        };
        client::Client shortCredentialClient(std::move(shortCredentialOptions), shortCredentialHarness.callbacks());
        client::Connection shortCredentialConnection = shortCredentialClient.openConnection(shortCredentialHarness.transport());
        shortCredentialConnection.transportConnected();
        result.expectTrue(
            shortCredentialHarness.messages.size() == 1 && shortCredentialHarness.messages.front().sensitive &&
                client::detail::ClientTestAccess::verifiedMovedFromStringScrubs(shortCredentialClient) >= 3 &&
                client::detail::ClientTestAccess::shortStringStorageScrubbed(),
            "successful sensitive Hello handoff scrubs codec, outbound, and transport moved-from storage including cleared SSO capacity");

        client::Client counterClient(options());
        const std::size_t maximumCount = std::numeric_limits<std::size_t>::max();
        client::detail::ClientTestAccess::setSynchronizationCounts(counterClient, maximumCount - 1, maximumCount - 2, maximumCount - 3);
        const bool reachedMaximum = client::detail::ClientTestAccess::tryAccumulateSynchronizationCounts(counterClient, 1, 2, 3);
        const auto maximumCounts = client::detail::ClientTestAccess::synchronizationCounts(counterClient);
        const bool overflowRejected = !client::detail::ClientTestAccess::tryAccumulateSynchronizationCounts(counterClient, 1, 0, 0);
        result.expectTrue(reachedMaximum && overflowRejected &&
                              maximumCounts == client::detail::ClientTestAccess::synchronizationCounts(counterClient) &&
                              maximumCounts[0] == maximumCount && maximumCounts[1] == maximumCount && maximumCounts[2] == maximumCount,
                          "synchronization counters reach their exact maximum and reject overflow without partial mutation");

        Harness counterOverflowHarness;
        client::Client counterOverflowClient(options(), counterOverflowHarness.callbacks());
        client::Connection counterOverflowConnection = counterOverflowClient.openConnection(counterOverflowHarness.transport());
        counterOverflowConnection.transportConnected();
        (void) counterOverflowConnection.receive(frontend::ServerMessage{frontend::Welcome{"counter-overflow",
                                                                                           frontend::SessionRole::Observer,
                                                                                           frontend::SequenceNumber(1),
                                                                                           frontend::SyncMode::Replay,
                                                                                           frontend::Json::object(),
                                                                                           advertisedCapabilities()}});
        client::detail::ClientTestAccess::setSynchronizationCounts(counterOverflowClient, maximumCount, maximumCount, maximumCount);
        const frontend::FrontendEvent counterEvent{
            frontend::SequenceNumber(1), "thread.upserted", frontend::Json{{"thread", {{"id", "counter-thread"}}}}};
        (void) counterOverflowConnection.receive(frontend::ServerMessage{
            frontend::EventBatch{frontend::SequenceNumber(1), frontend::SequenceNumber(1), std::vector{counterEvent}}});
        result.expectTrue(!counterOverflowConnection.isOpen() && counterOverflowHarness.closes == 1 &&
                              counterOverflowClient.state().threads().empty(),
                          "a replay batch that would overflow synchronization counts closes before committing its candidate State");

        Harness boundedHarness;
        client::ClientOptions boundedOptions = options();
        boundedOptions.maximumPendingOperations = 0;
        client::Client bounded(std::move(boundedOptions), boundedHarness.callbacks());
        client::Connection boundedConnection = bounded.openConnection(boundedHarness.transport());
        boundedConnection.transportConnected();
        makeReady(boundedConnection);
        const client::Submission capacityRejected =
            bounded.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                               frontend::Json::object()}},
                           {});
        result.expectTrue(!capacityRejected && capacityRejected.error &&
                              capacityRejected.error->clientCode == client::ClientErrorCode::TooManyPendingOperations,
                          "zero pending-operation capacity is finite and rejects locally rather than meaning unlimited");
    }

    void testEquivalentInboundBounds(tests::support::TestResult& result) {
        const frontend::ServerMessage message{frontend::Welcome{"bounded",
                                                                frontend::SessionRole::Observer,
                                                                frontend::SequenceNumber(1),
                                                                frontend::SyncMode::Snapshot,
                                                                frontend::Json::object(),
                                                                advertisedCapabilities()}};
        const auto encoded = frontend::Codec::encodeServer(message);
        const auto serialized = frontend::Codec::serializeServer(message);
        result.expectTrue(encoded && serialized && !serialized.value().empty(),
                          "the inbound-bound fixture has one canonical encoded representation");
        if (!encoded || !serialized || serialized.value().empty()) {
            return;
        }

        struct Probe {
            bool accepted = false;
            bool open = false;
            std::size_t observations = 0;
            std::size_t closes = 0;
        };
        const auto run = [&](int overload, std::size_t maximumBytes) {
            Harness harness;
            std::size_t observations = 0;
            client::ClientOptions bounded = options();
            bounded.maximumInboundMessageBytes = maximumBytes;
            client::ClientCallbacks callbacks = harness.callbacks();
            callbacks.onProtocolMessage = [&observations](const frontend::ServerMessage&) {
                ++observations;
            };
            client::Client sdk(std::move(bounded), std::move(callbacks));
            client::Connection connection = sdk.openConnection(harness.transport());
            connection.transportConnected();
            client::ReceiveResult receive;
            if (overload == 0) {
                receive = connection.receive(std::string_view(serialized.value()));
            } else if (overload == 1) {
                receive = connection.receive(encoded.value());
            } else {
                receive = connection.receive(message);
            }
            return Probe{receive.accepted, connection.isOpen(), observations, harness.closes};
        };

        const std::size_t exactBytes = serialized.value().size();
        const Probe exactString = run(0, exactBytes);
        const Probe exactJson = run(1, exactBytes);
        const Probe exactTyped = run(2, exactBytes);
        const Probe rejectedString = run(0, exactBytes - 1);
        const Probe rejectedJson = run(1, exactBytes - 1);
        const Probe rejectedTyped = run(2, exactBytes - 1);
        result.expectTrue(exactString.accepted && exactString.open && exactString.observations == 1 && exactJson.accepted &&
                              exactJson.open && exactJson.observations == 1 && exactTyped.accepted && exactTyped.open &&
                              exactTyped.observations == 1,
                          "all receive overloads accept the same server message at its exact canonical byte bound");
        result.expectTrue(!rejectedString.accepted && !rejectedString.open && rejectedString.observations == 0 &&
                              rejectedString.closes == 1 && !rejectedJson.accepted && !rejectedJson.open &&
                              rejectedJson.observations == 0 && rejectedJson.closes == 1 && !rejectedTyped.accepted &&
                              !rejectedTyped.open && rejectedTyped.observations == 0 && rejectedTyped.closes == 1,
                          "string, Json, and typed receive overloads all reject over-bound input before protocol dispatch");
    }

    void testDeferredCallbackSubmissions(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        makeReady(connection);
        harness.messages.clear();

        std::vector<std::size_t> messageCountsInsideCallbacks;
        std::vector<client::Submission> deferredSubmissions;
        bool observationSubmitted = false;
        std::size_t deferredCompletions = 0;
        client::ClientCallbacks callbacks = harness.callbacks();
        callbacks.onProtocolMessage = [&](const frontend::ServerMessage& observed) {
            if (observationSubmitted || !std::holds_alternative<frontend::Response>(observed)) {
                return;
            }
            observationSubmitted = true;
            messageCountsInsideCallbacks.push_back(harness.messages.size());
            deferredSubmissions.push_back(
                sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ProviderStart>{
                               frontend::Json::object()}},
                           [&](const client::GeneratedOperationResult&) {
                               ++deferredCompletions;
                           }));
        };
        sdk.setCallbacks(std::move(callbacks));

        const client::Submission initial =
            sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                           frontend::Json::object()}},
                       [&](const client::GeneratedOperationResult&) {
                           messageCountsInsideCallbacks.push_back(harness.messages.size());
                           deferredSubmissions.push_back(sdk.submit(
                               generated::CompleteCommandParameters{
                                   generated::MethodParameters<generated::MethodId::ControllerRelease>{frontend::Json::object()}},
                               [&](const client::GeneratedOperationResult&) {
                                   ++deferredCompletions;
                               }));
                       });
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(initial.requestId->value(), frontend::Json{{"role", "controller"}})});

        std::vector<generated::MethodId> sentMethods;
        for (std::size_t index = 1; index < harness.messages.size(); ++index) {
            const auto command = frontend::Codec::decodeDefinedCommand(std::string_view(harness.messages[index].compactJson));
            if (command) {
                sentMethods.push_back(generated::commandMethod(command.value().parameters));
            }
        }
        result.expectTrue(
            messageCountsInsideCallbacks == std::vector<std::size_t>{1, 1} && deferredSubmissions.size() == 2 && deferredSubmissions[0] &&
                deferredSubmissions[1] &&
                sentMethods == std::vector{generated::MethodId::ControllerRelease, generated::MethodId::ProviderStart} &&
                sdk.pendingOperationCount() == 2,
            "operation and protocol callbacks queue submissions without transport send until outer dispatch, then flush FIFO");

        for (const client::Submission& submission : deferredSubmissions) {
            (void) connection.receive(
                frontend::ServerMessage{frontend::Response::success(submission.requestId->value(), frontend::Json::object())});
        }
        result.expectTrue(deferredCompletions == 2 && sdk.pendingOperationCount() == 0,
                          "deferred operations retain normal correlation and complete exactly once");

        Harness closingHarness;
        client::Client closingSdk(options(), closingHarness.callbacks());
        client::Connection closingConnection = closingSdk.openConnection(closingHarness.transport());
        closingConnection.transportConnected();
        makeReady(closingConnection);
        closingHarness.messages.clear();
        std::size_t closedDeferredCompletions = 0;
        std::optional<client::Error> closedDeferredError;
        const client::Submission closingInitial =
            closingSdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                                  frontend::Json::object()}},
                              [&](const client::GeneratedOperationResult&) {
                                  const client::Submission deferred = closingSdk.submit(
                                      generated::CompleteCommandParameters{
                                          generated::MethodParameters<generated::MethodId::ControllerRelease>{frontend::Json::object()}},
                                      [&](const client::GeneratedOperationResult& operation) {
                                          ++closedDeferredCompletions;
                                          closedDeferredError = operation.error;
                                      });
                                  (void) deferred;
                                  closingSdk.close("close from operation callback");
                              });
        (void) closingConnection.receive(frontend::ServerMessage{
            frontend::Response::success(closingInitial.requestId->value(), frontend::Json{{"role", "controller"}})});
        result.expectTrue(closingSdk.connectionState() == client::ConnectionState::Closed && closingHarness.messages.size() == 1 &&
                              closingHarness.closes == 1 && closedDeferredCompletions == 1 && closedDeferredError &&
                              closedDeferredError->clientCode == client::ClientErrorCode::Closed,
                          "close from a callback cancels an accepted deferred submission once without sending it or leaving Closed");

        Harness rejectedHarness;
        client::Client rejectedSdk(options(), rejectedHarness.callbacks());
        client::Connection rejectedConnection = rejectedSdk.openConnection(rejectedHarness.transport());
        rejectedConnection.transportConnected();
        makeReady(rejectedConnection);
        rejectedHarness.messages.clear();
        std::size_t rejectedCompletions = 0;
        std::optional<client::Error> rejectedError;
        const client::Submission trigger =
            rejectedSdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                                   frontend::Json::object()}},
                               [&](const client::GeneratedOperationResult&) {
                                   const client::Submission deferred = rejectedSdk.submit(
                                       generated::CompleteCommandParameters{
                                           generated::MethodParameters<generated::MethodId::ControllerRelease>{frontend::Json::object()}},
                                       [&](const client::GeneratedOperationResult& operation) {
                                           ++rejectedCompletions;
                                           rejectedError = operation.error;
                                       });
                                   (void) deferred;
                                   rejectedHarness.rejectCommands = true;
                               });
        (void) rejectedConnection.receive(
            frontend::ServerMessage{frontend::Response::success(trigger.requestId->value(), frontend::Json{{"role", "controller"}})});
        result.expectTrue(!rejectedConnection.isOpen() && rejectedHarness.closes == 1 && rejectedCompletions == 1 && rejectedError &&
                              rejectedError->clientCode == client::ClientErrorCode::SendRejected &&
                              rejectedSdk.pendingOperationCount() == 0,
                          "a deferred transport rejection fails that accepted operation once and remains connection-local");

        Harness dispatchFailureHarness;
        client::Client dispatchFailureSdk(options(), dispatchFailureHarness.callbacks());
        client::Connection dispatchFailureConnection = dispatchFailureSdk.openConnection(dispatchFailureHarness.transport());
        dispatchFailureConnection.transportConnected();
        makeReady(dispatchFailureConnection);
        dispatchFailureHarness.messages.clear();
        std::size_t completedInitial = 0;
        std::size_t completedDeferred = 0;
        std::optional<client::Error> deferredFailure;
        const client::Submission dispatchTrigger = dispatchFailureSdk.submit(
            generated::CompleteCommandParameters{
                generated::MethodParameters<generated::MethodId::ControllerAcquire>{frontend::Json::object()}},
            [&](const client::GeneratedOperationResult&) {
                ++completedInitial;
                const client::Submission deferred = dispatchFailureSdk.submit(
                    generated::CompleteCommandParameters{
                        generated::MethodParameters<generated::MethodId::ControllerRelease>{frontend::Json::object()}},
                    [&](const client::GeneratedOperationResult& operation) {
                        ++completedDeferred;
                        deferredFailure = operation.error;
                    });
                (void) deferred;
            });
        client::detail::ClientTestAccess::failAfterNextDispatch(dispatchFailureSdk);
        (void) dispatchFailureConnection.receive(frontend::ServerMessage{
            frontend::Response::success(dispatchTrigger.requestId->value(), frontend::Json{{"role", "controller"}})});
        dispatchFailureConnection.transportDisconnected(client::TransportError{"late disconnect", true});
        result.expectTrue(
            completedInitial == 1 && completedDeferred == 1 && deferredFailure &&
                deferredFailure->origin == client::ErrorOrigin::Protocol && dispatchFailureHarness.messages.size() == 1 &&
                dispatchFailureHarness.closes == 1 && dispatchFailureSdk.pendingOperationCount() == 0 &&
                dispatchFailureSdk.connectionState() == client::ConnectionState::Disconnected,
            "an internal dispatch exception restores dispatch depth, cancels deferred work once, and closes only its connection");
    }

    void testStateCallbackDeferralAndTerminalClose(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        makeReady(connection);
        harness.messages.clear();

        bool submittedFromState = false;
        std::size_t messagesInsideStateCallback = 0;
        std::size_t completions = 0;
        std::optional<client::Submission> deferred;
        client::ClientCallbacks callbacks = harness.callbacks();
        callbacks.onStateUpdated = [&](const client::StateUpdate& update) {
            if (update.cause != client::UpdateCause::Live || submittedFromState) {
                return;
            }
            submittedFromState = true;
            messagesInsideStateCallback = harness.messages.size();
            deferred = sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ProviderStart>{
                                      frontend::Json::object()}},
                                  [&](const client::GeneratedOperationResult&) {
                                      ++completions;
                                  });
        };
        sdk.setCallbacks(std::move(callbacks));
        const frontend::FrontendEvent stateCallbackEvent{
            frontend::SequenceNumber(8), "thread.upserted", frontend::Json{{"thread", {{"id", "thread-from-state-callback"}}}}};
        const frontend::EventBatch stateCallbackBatch{
            frontend::SequenceNumber(8), frontend::SequenceNumber(8), std::vector{stateCallbackEvent}};
        (void) connection.receive(frontend::ServerMessage{stateCallbackBatch});
        result.expectTrue(connection.isOpen() && deferred && *deferred && messagesInsideStateCallback == 0 && harness.messages.size() == 1,
                          "a state-update callback submission is accepted but its transport send waits for committed dispatch completion");
        if (deferred && *deferred) {
            (void) connection.receive(
                frontend::ServerMessage{frontend::Response::success(deferred->requestId->value(), frontend::Json::object())});
        }
        result.expectTrue(completions == 1, "the state-callback deferred operation completes through normal correlation");

        Harness closingHarness;
        client::Client* closingClient = nullptr;
        std::size_t cursorCallbacks = 0;
        std::size_t synchronizationCallbacks = 0;
        client::ClientCallbacks closingCallbacks = closingHarness.callbacks();
        closingCallbacks.onStateUpdated = [&](const client::StateUpdate& update) {
            if (update.cause == client::UpdateCause::SynchronizationCompleted) {
                closingClient->close("close from synchronization state callback");
            }
        };
        closingCallbacks.onCursorAdvanced = [&](frontend::SequenceNumber) {
            ++cursorCallbacks;
        };
        closingCallbacks.onSynchronized = [&](const client::SynchronizationInfo&) {
            ++synchronizationCallbacks;
        };
        client::Client terminalSdk(options(), std::move(closingCallbacks));
        closingClient = &terminalSdk;
        client::Connection terminalConnection = terminalSdk.openConnection(closingHarness.transport());
        terminalConnection.transportConnected();
        makeReady(terminalConnection);
        result.expectTrue(terminalSdk.connectionState() == client::ConnectionState::Closed && !terminalConnection.isOpen() &&
                              closingHarness.closes == 1 && cursorCallbacks == 0 && synchronizationCallbacks == 0,
                          "close from a committed state callback is terminal and suppresses later callbacks in that dispatch frame");
    }

    void testProjectionRefreshCapacity(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection first = sdk.openConnection(harness.transport());
        first.transportConnected();
        makeReady(first, frontend::SequenceNumber(7));
        first.transportDisconnected();

        client::Connection second = sdk.openConnection(harness.transport());
        second.transportConnected();
        (void) second.receive(
            frontend::ServerMessage{frontend::Welcome{"session-2",
                                                      frontend::SessionRole::Observer,
                                                      frontend::SequenceNumber(8),
                                                      frontend::SyncMode::Replay,
                                                      frontend::Json{{"permittedScopes", frontend::Json::array({"observe"})}},
                                                      advertisedCapabilities()}});
        const std::size_t beforeRefresh = harness.messages.size();
        (void) second.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(8)}});
        const auto refresh = frontend::Codec::decodeDefinedCommand(std::string_view(harness.messages.back().compactJson));
        result.expectTrue(harness.messages.size() == beforeRefresh + 1 && refresh &&
                              generated::commandMethod(refresh.value().parameters) == generated::MethodId::SnapshotGet &&
                              sdk.pendingOperationCount() == 1,
                          "the internal projection-refresh snapshot occupies one bounded pending-operation slot");

        std::vector<std::size_t> refreshCountsAtCompletion;
        std::optional<client::Submission> completionSubmission;
        client::ClientCallbacks refreshCallbacks = harness.callbacks();
        refreshCallbacks.onStateUpdated = [&](const client::StateUpdate& update) {
            if (update.cause == client::UpdateCause::SynchronizationCompleted) {
                refreshCountsAtCompletion.push_back(sdk.pendingOperationCount());
                completionSubmission =
                    sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ProviderStart>{
                                   frontend::Json::object()}},
                               {});
            }
        };
        refreshCallbacks.onSynchronized = [&](const client::SynchronizationInfo&) {
            refreshCountsAtCompletion.push_back(sdk.pendingOperationCount());
        };
        sdk.setCallbacks(std::move(refreshCallbacks));
        (void) second.receive(frontend::ServerMessage{
            frontend::Response::success(refresh.value().requestId, frontend::Json{{"sequence", std::uint64_t{8}}})});
        const std::size_t afterRefreshResponse = sdk.pendingOperationCount();
        (void) second.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(8), expandedState()}});
        const std::size_t afterRefreshSnapshot = sdk.pendingOperationCount();
        const std::size_t messagesBeforeCompletion = harness.messages.size();
        (void) second.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(8)}});
        const auto callbackCommand = frontend::Codec::decodeDefinedCommand(std::string_view(harness.messages.back().compactJson));
        result.expectTrue(
            afterRefreshResponse == 1 && afterRefreshSnapshot == 1 && refreshCountsAtCompletion == std::vector<std::size_t>{0, 1} &&
                completionSubmission && *completionSubmission && sdk.pendingOperationCount() == 1 && sdk.isReady() &&
                harness.messages.size() == messagesBeforeCompletion + 1 && callbackCommand &&
                generated::commandMethod(callbackCommand.value().parameters) == generated::MethodId::ProviderStart,
            "projection refresh retires at SyncComplete before callbacks, which may immediately use the released bounded slot");
        if (completionSubmission && *completionSubmission) {
            (void) second.receive(
                frontend::ServerMessage{frontend::Response::success(completionSubmission->requestId->value(), frontend::Json::object())});
        }
        result.expectTrue(sdk.pendingOperationCount() == 0,
                          "the callback submission made at projection-refresh completion flushes and completes exactly once");

        Harness zeroHarness;
        client::ClientOptions zeroOptions = options();
        zeroOptions.maximumPendingOperations = 0;
        client::Client zeroSdk(std::move(zeroOptions), zeroHarness.callbacks());
        client::Connection zeroFirst = zeroSdk.openConnection(zeroHarness.transport());
        zeroFirst.transportConnected();
        makeReady(zeroFirst, frontend::SequenceNumber(7));
        zeroFirst.transportDisconnected();
        client::Connection zeroSecond = zeroSdk.openConnection(zeroHarness.transport());
        zeroSecond.transportConnected();
        (void) zeroSecond.receive(
            frontend::ServerMessage{frontend::Welcome{"session-2",
                                                      frontend::SessionRole::Observer,
                                                      frontend::SequenceNumber(8),
                                                      frontend::SyncMode::Replay,
                                                      frontend::Json{{"permittedScopes", frontend::Json::array({"observe"})}},
                                                      advertisedCapabilities()}});
        const std::size_t zeroBeforeRefresh = zeroHarness.messages.size();
        (void) zeroSecond.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(8)}});
        result.expectTrue(!zeroSecond.isOpen() && zeroHarness.messages.size() == zeroBeforeRefresh && zeroSdk.pendingOperationCount() == 0,
                          "zero pending-operation capacity also bounds the hidden projection-refresh operation");
    }

    void testSensitiveBindingAuthority(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        makeReady(connection);
        harness.messages.clear();

        frontend::Json authentication = frontend::Json::object();
        authentication["pendingRequestId"] = "1";
        authentication["accessToken"] = "ACCESS_TOKEN_SENTINEL";
        authentication["chatgptAccountId"] = "account";
        (void) sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::AuthenticationRespond>{
                              std::move(authentication)}},
                          {});

        frontend::Json answer = frontend::Json::object();
        answer["questionId"] = "q";
        answer["answers"] = frontend::Json::array({"secret"});
        frontend::Json userInput = frontend::Json::object();
        userInput["pendingRequestId"] = "2";
        userInput["answers"] = frontend::Json::array({std::move(answer)});
        (void) sdk.submit(
            generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::UserInputRespond>{std::move(userInput)}},
            {});

        frontend::Json unknown = frontend::Json::object();
        unknown["pendingRequestId"] = "3";
        unknown["result"] = frontend::Json{{"opaque", "secret"}};
        (void) sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::UnknownRequestRespond>{
                              std::move(unknown)}},
                          {});

        frontend::Json login = frontend::Json::object();
        login["type"] = "apiKey";
        login["apiKey"] = "API_KEY_SENTINEL";
        (void) sdk.submit(
            generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::AccountLoginStart>{std::move(login)}},
            {});
        (void) sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                              frontend::Json::object()}},
                          {});

        result.expectTrue(
            harness.messages.size() == 5 && harness.messages[0].sensitive && harness.messages[1].sensitive &&
                harness.messages[2].sensitive && harness.messages[3].sensitive && !harness.messages[4].sensitive,
            "reviewed generated binding metadata marks reverse secrets and account login sensitive without ad-hoc dispatch tests");
    }

    void testSensitiveResidualErasure(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        const std::size_t afterHello = client::detail::ClientTestAccess::erasedTransientBytes(sdk);
        result.expectTrue(afterHello >= std::string_view("BEARER_SENTINEL").size() && harness.messages.size() == 1 &&
                              harness.messages.front().sensitive,
                          "the SDK overwrites its retained Hello credential object before the transport-owned sensitive payload continues");
        makeReady(connection);
        harness.messages.clear();

        std::size_t afterDeferredEncoding = 0;
        std::size_t deferredCompletions = 0;
        std::optional<client::Error> deferredError;
        const client::Submission trigger =
            sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                           frontend::Json::object()}},
                       [&](const client::GeneratedOperationResult&) {
                           frontend::Json authentication = frontend::Json::object();
                           authentication["pendingRequestId"] = "1";
                           authentication["accessToken"] = "DEFERRED_ACCESS_TOKEN_SENTINEL";
                           authentication["chatgptAccountId"] = "account";
                           const client::Submission deferred = sdk.submit(
                               generated::CompleteCommandParameters{
                                   generated::MethodParameters<generated::MethodId::AuthenticationRespond>{std::move(authentication)}},
                               [&](const client::GeneratedOperationResult& operation) {
                                   ++deferredCompletions;
                                   deferredError = operation.error;
                               });
                           (void) deferred;
                           afterDeferredEncoding = client::detail::ClientTestAccess::erasedTransientBytes(sdk);
                           sdk.close("cancel sensitive deferred command");
                       });
        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(trigger.requestId->value(), frontend::Json{{"role", "controller"}})});
        const std::size_t afterCancellation = client::detail::ClientTestAccess::erasedTransientBytes(sdk);
        result.expectTrue(
            trigger && harness.messages.size() == 1 && afterDeferredEncoding > afterHello && afterCancellation > afterDeferredEncoding &&
                deferredCompletions == 1 && deferredError && deferredError->clientCode == client::ClientErrorCode::Closed,
            "canceling a sensitive deferred submission overwrites both encoded parameter values and the SDK-owned queued payload");
    }

    void testConnectionHandleOwnership(tests::support::TestResult& result) {
        static_assert(std::is_nothrow_move_assignable_v<client::Connection>);

        Harness displacedHarness;
        Harness incomingHarness;
        client::Client displacedClient(options(), displacedHarness.callbacks());
        client::Client incomingClient(options(), incomingHarness.callbacks());
        client::Connection destination = displacedClient.openConnection(displacedHarness.transport());
        client::Connection incoming = incomingClient.openConnection(incomingHarness.transport());
        destination.transportConnected();
        incoming.transportConnected();
        makeReady(destination, frontend::SequenceNumber(5));
        makeReady(incoming, frontend::SequenceNumber(6));
        const std::uint64_t incomingGeneration = incoming.generation();

        destination = std::move(incoming);
        result.expectTrue(displacedHarness.closes == 1 && displacedClient.connectionState() == client::ConnectionState::Disconnected &&
                              !displacedClient.hasActiveConnection() &&
                              displacedClient.state().freshness() == client::StateFreshness::Stale && !incoming.isOpen() &&
                              destination.isOpen() && destination.generation() == incomingGeneration && incomingClient.isReady() &&
                              incomingClient.hasActiveConnection(),
                          "move-assigning into an active Connection closes and detaches the displaced attachment exactly once while "
                          "preserving the incoming attachment");

        client::Connection& sameConnection = destination;
        destination = std::move(sameConnection);
        result.expectTrue(destination.isOpen() && destination.generation() == incomingGeneration && incomingHarness.closes == 0 &&
                              incomingClient.isReady() && incomingClient.hasActiveConnection(),
                          "self move-assignment preserves an open Connection without closing or detaching its transport attachment");

        Harness throwingHarness;
        Harness replacementHarness;
        client::Client throwingClient(options(), throwingHarness.callbacks());
        client::Client replacementClient(options(), replacementHarness.callbacks());
        client::TransportCallbacks throwingTransport{
            [&throwingHarness](client::OutboundMessage message) {
                throwingHarness.messages.push_back(std::move(message));
                return client::SendResult{client::SendStatus::Accepted, std::nullopt};
            },
            [&throwingHarness](std::string) {
                ++throwingHarness.closes;
                throw std::runtime_error("move-assignment close callback sentinel");
            },
        };
        client::Connection throwingDestination = throwingClient.openConnection(std::move(throwingTransport));
        client::Connection replacement = replacementClient.openConnection(replacementHarness.transport());
        throwingDestination.transportConnected();
        replacement.transportConnected();
        makeReady(throwingDestination, frontend::SequenceNumber(7));
        makeReady(replacement, frontend::SequenceNumber(8));
        const std::uint64_t replacementGeneration = replacement.generation();

        throwingDestination = std::move(replacement);
        result.expectTrue(
            throwingHarness.closes == 1 && throwingHarness.diagnostics == 1 &&
                throwingClient.connectionState() == client::ConnectionState::Disconnected && !throwingClient.hasActiveConnection() &&
                !replacement.isOpen() && throwingDestination.isOpen() && throwingDestination.generation() == replacementGeneration &&
                replacementClient.isReady() && replacementClient.hasActiveConnection(),
            "move-assignment contains a displaced transport close exception, detaches it once, and still takes the incoming attachment");

        Harness destructorHarness;
        client::Client destructorClient(options(), destructorHarness.callbacks());
        {
            client::Connection scoped = destructorClient.openConnection(destructorHarness.transport());
            scoped.transportConnected();
            makeReady(scoped, frontend::SequenceNumber(9));
            result.expectTrue(destructorClient.isReady() && destructorClient.state().freshness() == client::StateFreshness::Current,
                              "the destructor fixture reaches a current synchronized state before releasing its only Connection handle");
        }
        result.expectTrue(destructorHarness.closes == 1 && !destructorClient.hasActiveConnection() &&
                              destructorClient.connectionState() == client::ConnectionState::Disconnected &&
                              destructorClient.state().freshness() == client::StateFreshness::Stale,
                          "destroying an open Connection requests one transport close, detaches it, and marks retained state stale");
    }

    void testRepeatedDisconnectedStateIsIdempotent(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        makeReady(connection, frontend::SequenceNumber(9));
        connection.transportDisconnected();
        const std::uint64_t staleRevision = sdk.state().revision();

        client::Connection failedReconnect = sdk.openConnection(harness.transport());
        failedReconnect.transportConnected();
        failedReconnect.transportDisconnected(client::TransportError{"reconnect failed before Welcome", true});
        result.expectTrue(staleRevision != 0 && sdk.state().revision() == staleRevision &&
                              sdk.state().freshness() == client::StateFreshness::Stale && !sdk.state().session() &&
                              sdk.connectionState() == client::ConnectionState::Disconnected && !sdk.hasActiveConnection(),
                          "a failed reconnect does not repeatedly copy or grow an already-stale retained State");
    }

    void testAtomicDisconnectCallbacks(tests::support::TestResult& result) {
        Harness harness;
        client::Client* sdkPointer = nullptr;
        bool callbacksSawCommittedDisconnect = true;
        std::size_t staleCallbacks = 0;
        std::size_t operationCompletions = 0;
        client::ClientCallbacks callbacks = harness.callbacks();
        callbacks.onStateUpdated = [&](const client::StateUpdate& update) {
            if (update.cause != client::UpdateCause::ConnectionBecameStale) {
                return;
            }
            ++staleCallbacks;
            callbacksSawCommittedDisconnect = callbacksSawCommittedDisconnect &&
                                              sdkPointer->connectionState() == client::ConnectionState::Disconnected &&
                                              !sdkPointer->hasActiveConnection() && sdkPointer->pendingOperationCount() == 0 &&
                                              update.state.freshness() == client::StateFreshness::Stale;
        };
        callbacks.onConnectionStateChanged = [&](const client::ConnectionStateChange& change) {
            harness.states.push_back(change.current);
            if (change.current == client::ConnectionState::Disconnected) {
                callbacksSawCommittedDisconnect = callbacksSawCommittedDisconnect && !sdkPointer->hasActiveConnection() &&
                                                  sdkPointer->pendingOperationCount() == 0 &&
                                                  sdkPointer->state().freshness() == client::StateFreshness::Stale;
            }
        };
        client::Client sdk(options(), std::move(callbacks));
        sdkPointer = &sdk;
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        makeReady(connection, frontend::SequenceNumber(12));
        const client::Submission pending = sdk.submit(
            generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ProviderStart>{frontend::Json::object()}},
            [&](const client::GeneratedOperationResult& operation) {
                ++operationCompletions;
                callbacksSawCommittedDisconnect = callbacksSawCommittedDisconnect && operation.error &&
                                                  operation.error->origin == client::ErrorOrigin::Transport &&
                                                  sdk.connectionState() == client::ConnectionState::Disconnected &&
                                                  !sdk.hasActiveConnection() && sdk.pendingOperationCount() == 0;
            });
        connection.transportDisconnected(client::TransportError{"atomic disconnect", true});
        result.expectTrue(
            pending && staleCallbacks == 1 && operationCompletions == 1 && callbacksSawCommittedDisconnect &&
                sdk.connectionState() == client::ConnectionState::Disconnected && !sdk.hasActiveConnection(),
            "disconnect commits stale State, clears active/pending ownership, and enters Disconnected before invoking any user callback");
    }

    void testGeneratedReverseRequestRejectsStaleSession(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();

        frontend::Json state = expandedState();
        state["pendingRequests"].push_back(frontend::Json{
            {"pendingRequestId", "1"}, {"kind", "command_execution_approval"}, {"summary", "stale reverse request"}, {"truncated", false}});
        frontend::Json projectionMetadata = frontend::Json::object();
        projectionMetadata["permittedScopes"] = frontend::Json::array();
        const std::vector<frontend::FrontendMethod> reverseMethods{
            std::string(generated::methodString(generated::MethodId::ApprovalRespond))};
        (void) connection.receive(frontend::ServerMessage{frontend::Welcome{"stale-session",
                                                                            frontend::SessionRole::Observer,
                                                                            frontend::SequenceNumber(7),
                                                                            frontend::SyncMode::Snapshot,
                                                                            projectionMetadata,
                                                                            advertisedCapabilities(),
                                                                            reverseMethods,
                                                                            reverseMethods}});
        (void) connection.receive(frontend::ServerMessage{frontend::Snapshot{frontend::SequenceNumber(7), std::move(state)}});
        (void) connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(7)}});
        connection.transportDisconnected();

        client::Connection reconnect = sdk.openConnection(harness.transport());
        reconnect.transportConnected();
        (void) reconnect.receive(frontend::ServerMessage{frontend::Welcome{"replacement-session",
                                                                           frontend::SessionRole::Observer,
                                                                           frontend::SequenceNumber(7),
                                                                           frontend::SyncMode::Replay,
                                                                           std::move(projectionMetadata),
                                                                           advertisedCapabilities(),
                                                                           reverseMethods,
                                                                           reverseMethods}});
        (void) reconnect.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(7)}});

        const client::PendingRequestState* stale = sdk.state().pendingRequest(client::PendingRequestId{"1"});
        const std::size_t messageCount = harness.messages.size();
        std::size_t completions = 0;
        const client::Submission submission =
            sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ApprovalRespond>{
                           frontend::Json{{"pendingRequestId", "1"}, {"decision", "accept"}}}},
                       [&completions](const client::GeneratedOperationResult&) {
                           ++completions;
                       });
        result.expectTrue(
            stale != nullptr && stale->connectionInvalidated && sdk.isReady() && !submission && submission.error &&
                submission.error->clientCode == client::ClientErrorCode::MethodNotPermitted && harness.messages.size() == messageCount &&
                completions == 0 && reconnect.isOpen(),
            "restricted generated reverse submissions share typed Requests session invalidation policy and never reach transport");
    }

    void testReentrantConnectionCloseDuringClientClose(tests::support::TestResult& result) {
        Harness harness;
        client::Connection connection;
        client::ClientCallbacks callbacks = harness.callbacks();
        callbacks.onConnectionStateChanged = [&](const client::ConnectionStateChange& change) {
            harness.states.push_back(change.current);
            if (change.current == client::ConnectionState::Closing) {
                connection.close("connection close from Client Closing callback");
            }
        };
        client::Client sdk(options(), std::move(callbacks));
        connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        makeReady(connection, frontend::SequenceNumber(13));
        std::size_t operationCompletions = 0;
        std::optional<client::Error> operationError;
        const client::Submission pending = sdk.submit(
            generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ProviderStart>{frontend::Json::object()}},
            [&](const client::GeneratedOperationResult& operation) {
                ++operationCompletions;
                operationError = operation.error;
            });
        const std::size_t statesBeforeClose = harness.states.size();
        sdk.close("reentrant connection close fixture");
        const std::vector<client::ConnectionState> terminalStates(harness.states.begin() + static_cast<std::ptrdiff_t>(statesBeforeClose),
                                                                  harness.states.end());
        result.expectTrue(terminalStates == std::vector{client::ConnectionState::Closing, client::ConnectionState::Closed} &&
                              sdk.connectionState() == client::ConnectionState::Closed && !connection.isOpen() &&
                              !sdk.hasActiveConnection() && harness.closes == 1 && pending && operationCompletions == 1 && operationError &&
                              operationError->origin == client::ErrorOrigin::Client &&
                              operationError->clientCode == client::ClientErrorCode::Closed,
                          "Connection::close reentered from Client Closing remains terminal, reports pending work as Client Closed, and "
                          "never emits Disconnected");
    }

    void testSynchronousCloseDisconnect(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection;
        client::TransportCallbacks transport{
            [&harness](client::OutboundMessage message) {
                harness.messages.push_back(std::move(message));
                return client::SendResult{client::SendStatus::Accepted, std::nullopt};
            },
            [&harness, &connection](std::string) {
                ++harness.closes;
                connection.transportDisconnected();
            },
        };
        connection = sdk.openConnection(std::move(transport));
        connection.transportConnected();
        makeReady(connection, frontend::SequenceNumber(11));
        const std::size_t statesBeforeClose = harness.states.size();

        sdk.close("synchronous close callback fixture");
        const std::vector<client::ConnectionState> terminalStates(harness.states.begin() + static_cast<std::ptrdiff_t>(statesBeforeClose),
                                                                  harness.states.end());
        result.expectTrue(terminalStates == std::vector{client::ConnectionState::Closing, client::ConnectionState::Closed} &&
                              harness.closes == 1 && !connection.isOpen() && !sdk.hasActiveConnection() &&
                              sdk.connectionState() == client::ConnectionState::Closed &&
                              sdk.state().freshness() == client::StateFreshness::Stale,
                          "a transport close callback that synchronously reports disconnection produces only Closing then Closed and "
                          "leaves retained state stale");
    }

    void testProtocolErrorCorrelation(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        makeReady(connection);

        std::size_t observations = 0;
        std::size_t fatalObservations = 0;
        std::size_t completions = 0;
        std::optional<client::Error> completionError;
        client::ClientCallbacks callbacks = harness.callbacks();
        callbacks.onProtocolMessage = [&](const frontend::ServerMessage& message) {
            if (const auto* protocolError = std::get_if<frontend::ProtocolErrorMessage>(&message)) {
                ++observations;
                fatalObservations += protocolError->closeConnection ? 1U : 0U;
            }
        };
        sdk.setCallbacks(std::move(callbacks));

        const client::Submission operation = sdk.submit(
            generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ProviderStart>{frontend::Json::object()}},
            [&](const client::GeneratedOperationResult& completed) {
                ++completions;
                completionError = completed.error;
            });
        frontend::ProtocolErrorMessage correlated;
        correlated.code = frontend::ErrorCode::InvalidCommand;
        correlated.message = "correlated non-closing error";
        correlated.requestId = operation.requestId->value();
        (void) connection.receive(frontend::ServerMessage{correlated});
        result.expectTrue(completions == 1 && completionError && completionError->origin == client::ErrorOrigin::Protocol &&
                              sdk.pendingOperationCount() == 0 && connection.isOpen() && sdk.isReady() && observations == 1 &&
                              fatalObservations == 0 && harness.closes == 0,
                          "a correlated non-closing protocol.error completes only its operation and leaves the Ready connection usable");

        frontend::ProtocolErrorMessage fatal;
        fatal.code = frontend::ErrorCode::InvalidCommand;
        fatal.message = "fatal protocol error";
        fatal.closeConnection = true;
        (void) connection.receive(frontend::ServerMessage{fatal});
        result.expectTrue(completions == 1 && observations == 2 && fatalObservations == 1 && harness.closes == 1 && !connection.isOpen() &&
                              sdk.connectionState() == client::ConnectionState::Disconnected,
                          "a fatal protocol.error is observed exactly once and then closes only the current connection");
    }

    void testIdentifierExhaustionAndInvalidMethod(tests::support::TestResult& result) {
        using MethodUnderlying = std::underlying_type_t<generated::MethodId>;
        const generated::MethodId invalidMethod = static_cast<generated::MethodId>(std::numeric_limits<MethodUnderlying>::max());

        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        const client::MethodStatus invalidStatus = sdk.methodStatus(invalidMethod);
        result.expectTrue(invalidStatus.method == invalidMethod && invalidStatus.available == client::Availability::Unknown &&
                              invalidStatus.permitted == client::Availability::Unknown &&
                              client::generated::bindingMetadata(invalidMethod) == nullptr &&
                              !client::generated::bindingIsSensitive(invalidMethod),
                          "invalid MethodId values are handled safely without indexing generated metadata out of bounds");

        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        makeReady(connection);
        harness.messages.clear();
        client::detail::ClientTestAccess::setNextRequest(sdk, std::numeric_limits<std::uint64_t>::max());
        const client::Submission exhausted =
            sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                           frontend::Json::object()}},
                       {});
        result.expectTrue(!exhausted && exhausted.error && exhausted.error->clientCode == client::ClientErrorCode::RequestIdExhausted &&
                              harness.messages.empty() && sdk.pendingOperationCount() == 0 && sdk.isReady(),
                          "request-ID exhaustion fails locally without wraparound, send, or connection loss");

        Harness generationHarness;
        client::Client generationClient(options(), generationHarness.callbacks());
        client::detail::ClientTestAccess::setNextConnectionGeneration(generationClient, std::numeric_limits<std::uint64_t>::max());
        client::Connection unavailable = generationClient.openConnection(generationHarness.transport());
        result.expectTrue(!unavailable.isOpen() && !generationClient.hasActiveConnection() &&
                              generationClient.connectionState() == client::ConnectionState::Disconnected &&
                              generationHarness.messages.empty() && generationHarness.closes == 0,
                          "connection-generation exhaustion fails closed before allocation and never wraps");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testHandshakeAndLifecycle(result);
    testHandshakeReentrancy(result);
    testCapabilityFactsAndRequirements(result);
    testLegacyPolicy(result);
    testCorrelationAndFailures(result);
    testPendingDisconnectAndCallbackContainment(result);
    testExceptionContainmentAndCapacity(result);
    testEquivalentInboundBounds(result);
    testDeferredCallbackSubmissions(result);
    testStateCallbackDeferralAndTerminalClose(result);
    testProjectionRefreshCapacity(result);
    testSensitiveBindingAuthority(result);
    testSensitiveResidualErasure(result);
    testConnectionHandleOwnership(result);
    testRepeatedDisconnectedStateIsIdempotent(result);
    testMalformedOptionalLegacySnapshotsAreContained(result);
    testAtomicDisconnectCallbacks(result);
    testGeneratedReverseRequestRejectsStaleSession(result);
    testReentrantConnectionCloseDuringClientClose(result);
    testSynchronousCloseDisconnect(result);
    testProtocolErrorCorrelation(result);
    testIdentifierExhaustionAndInvalidMethod(result);
    return result.processResult();
}
