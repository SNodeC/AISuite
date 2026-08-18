/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/client/ClientCore.h"
#include "support/TestResult.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace core = ai::openai::codex::frontend::internal::client;
    namespace model = ai::openai::codex::frontend::internal::model;
    namespace generated = ai::openai::codex::frontend::generated;

    core::ClientOptions clientOptions() {
        core::ClientOptions options;
        options.credentialProvider = [] {
            return core::AuthenticationContext{frontend::NoCredential{}, std::nullopt};
        };
        return options;
    }

    struct Harness {
        std::vector<core::OutboundMessage> outbound;
        core::SendStatus status = core::SendStatus::Accepted;
        std::size_t closes = 0;

        core::TransportCallbacks transport() {
            return {[this](core::OutboundMessage message) {
                        outbound.push_back(std::move(message));
                        return core::SendResult{status,
                                                status == core::SendStatus::Accepted
                                                    ? std::nullopt
                                                    : std::optional<core::TransportError>{{"send rejected", true}}};
                    },
                    [this](std::string_view) {
                        ++closes;
                    }};
        }
    };

    std::vector<frontend::FrontendMethod> methods() {
        std::vector<frontend::FrontendMethod> result;
        for (const auto& metadata : generated::AllMethods) {
            result.emplace_back(metadata.method);
        }
        return result;
    }

    frontend::CapabilityAdvertisement capabilities() {
        std::vector<frontend::FrontendCapability> defined;
        for (const auto& metadata : generated::AllCapabilities) {
            if (metadata.defined) {
                defined.push_back(static_cast<frontend::FrontendCapability>(metadata.id));
            }
        }
        const std::vector<frontend::FrontendCapability> selected{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
        };
        return {defined, selected, selected, frontend::Json::object()};
    }

    frontend::Snapshot snapshot(model::CanonicalSnapshot state) {
        const auto expanded = model::encodeSnapshot(state);
        if (!expanded) {
            throw std::runtime_error(expanded.error().path + ": " + expanded.error().message);
        }
        const auto encoded = frontend::Codec::encodeExpandedSnapshot(expanded.value());
        if (!encoded) {
            throw std::runtime_error(encoded.error().message);
        }
        return {state.sequence.protocolValue(), encoded.value().at("state")};
    }

    core::PhysicalGeneration ready(core::ClientCore& client,
                                   Harness& harness,
                                   model::CanonicalSnapshot state = {},
                                   std::optional<std::vector<frontend::FrontendMethod>> permittedMethods = std::nullopt,
                                   std::optional<std::uint64_t> maximumInboundMessageBytes = std::nullopt) {
        const core::PhysicalGeneration generation = *client.attach(harness.transport());
        client.transportConnected(generation);
        std::vector<frontend::FrontendMethod> availableMethods = methods();
        std::vector<frontend::FrontendMethod> effectivePermittedMethods =
            permittedMethods.has_value() ? std::move(*permittedMethods) : availableMethods;
        frontend::Welcome welcome{"capacity-session",
                                  frontend::SessionRole::Controller,
                                  state.sequence.protocolValue(),
                                  frontend::SyncMode::Snapshot,
                                  {{"permittedScopes",
                                    frontend::Json::array({"observe",
                                                           "control",
                                                           "provider_lifecycle",
                                                           "account_management",
                                                           "configuration_write",
                                                           "command_execution",
                                                           "filesystem_read",
                                                           "filesystem_write",
                                                           "extension_management",
                                                           "mcp_invoke",
                                                           "sensitive_response",
                                                           "unknown_request_response"})}},
                                  capabilities(),
                                  std::move(availableMethods),
                                  std::move(effectivePermittedMethods)};
        welcome.maximumInboundMessageBytes = maximumInboundMessageBytes;
        (void) client.receive(generation, frontend::ServerMessage{welcome});
        (void) client.receive(generation, frontend::ServerMessage{snapshot(std::move(state))});
        (void) client.receive(generation, frontend::ServerMessage{frontend::SyncComplete{welcome.currentSequence}});
        return generation;
    }

    generated::CompleteCommandParameters sensitiveParameters(std::string secret) {
        frontend::Json parameters = frontend::Json::object();
        parameters["pendingRequestId"] = "1";
        parameters["accessToken"] = std::move(secret);
        parameters["chatgptAccountId"] = "a";
        return generated::makeParameters(generated::MethodId::AuthenticationRespond, std::move(parameters));
    }

    generated::CompleteCommandParameters userInputParameters(std::string answer) {
        frontend::Json response = {
            {"pendingRequestId", "1"},
            {"answers",
             frontend::Json::array(
                 {frontend::Json{{"questionId", "question"}, {"answers", frontend::Json::array({std::move(answer)})}}})},
        };
        return generated::makeParameters(generated::MethodId::UserInputRespond, std::move(response));
    }

    frontend::FrontendEvent diagnosticEvent(std::uint64_t sequence) {
        model::DiagnosticRecord diagnostic;
        diagnostic.received = sequence;
        diagnostic.detailsOmitted = true;
        diagnostic.message = "bounded diagnostic " + std::to_string(sequence);
        model::OccurrenceIdentity identity{model::FrontendSequence(sequence),
                                           model::OccurrenceGroupIdentity{"capacity-diagnostic-" + std::to_string(sequence)},
                                           0,
                                           1,
                                           model::SourceStamp{"capacity-source-" + std::to_string(sequence)}};
        const auto occurrence = model::makeOccurrence(std::move(identity), model::DiagnosticsUpdatedOccurrence{std::move(diagnostic)});
        if (!occurrence) {
            throw std::runtime_error(occurrence.error().path + ": " + occurrence.error().message);
        }
        const auto expanded = model::encodeExpandedOccurrence(occurrence.value());
        if (!expanded) {
            throw std::runtime_error(expanded.error().path + ": " + expanded.error().message);
        }
        const frontend::ExpandedFrontendEvent& event = expanded.value().front();
        return {event.sequence, std::string(frontend::toString(event.type)), event.data, event.extensions};
    }

    void testOperationAndRequestIdBounds(tests::support::TestResult& result) {
        core::ClientOptions bounded = clientOptions();
        bounded.limits.maximumPendingOperations = 1;
        Harness boundedHarness;
        core::ClientCore boundedClient(std::move(bounded));
        (void) ready(boundedClient, boundedHarness);
        const auto first = boundedClient.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()));
        const auto second = boundedClient.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()));
        result.expectTrue(first && !second && second.error->clientCode == core::ClientErrorCode::TooManyPendingOperations &&
                              boundedClient.pendingOperationCount() == 1,
                          "pending operations are bounded before another command enters the transport");

        core::ClientOptions exhausting = clientOptions();
        exhausting.initialRequestId = std::numeric_limits<std::uint64_t>::max() - 1;
        exhausting.limits.maximumPendingOperations = 2;
        Harness exhaustionHarness;
        core::ClientCore exhaustionClient(std::move(exhausting));
        (void) ready(exhaustionClient, exhaustionHarness);
        const auto finalId = exhaustionClient.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()));
        const auto exhausted = exhaustionClient.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()));
        result.expectTrue(finalId && !exhausted && exhausted.error->clientCode == core::ClientErrorCode::RequestIdExhausted,
                          "request-ID exhaustion is terminal for allocation and never wraps into a duplicate identity");
    }

    void testSensitiveEarlyRejectionErasure(tests::support::TestResult& result) {
        constexpr std::string_view secret =
            "CLIENT_CORE_EARLY_REJECTION_SECRET_SENTINEL_0123456789_ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz_"
            "0123456789_ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
        const auto erasedBy = [](core::ClientCore& client, const auto& submit) {
            const std::size_t before = core::ClientCoreTestAccess::erasedTransientBytes(client);
            const core::Submission submission = submit();
            const std::size_t after = core::ClientCoreTestAccess::erasedTransientBytes(client);
            return std::pair{submission, after >= before ? after - before : 0};
        };

        core::ClientCore notReady(clientOptions());
        const auto [notReadySubmission, notReadyErased] = erasedBy(notReady, [&] {
            return notReady.submit(sensitiveParameters(std::string(secret)));
        });

        core::ClientOptions capacityOptions = clientOptions();
        capacityOptions.limits.maximumPendingOperations = 0;
        Harness capacityHarness;
        core::ClientCore capacity(std::move(capacityOptions));
        (void) ready(capacity, capacityHarness);
        const auto [capacitySubmission, capacityErased] = erasedBy(capacity, [&] {
            return capacity.submit(sensitiveParameters(std::string(secret)));
        });

        std::vector<frontend::FrontendMethod> deniedMethods = methods();
        deniedMethods.erase(std::remove(deniedMethods.begin(), deniedMethods.end(), "request.authentication.respond"), deniedMethods.end());
        Harness deniedHarness;
        core::ClientCore denied(clientOptions());
        (void) ready(denied, deniedHarness, {}, std::move(deniedMethods));
        const auto [deniedSubmission, deniedErased] = erasedBy(denied, [&] {
            return denied.submit(sensitiveParameters(std::string(secret)));
        });

        result.expectTrue(!notReadySubmission && notReadySubmission.error.has_value() &&
                              notReadySubmission.error->clientCode == core::ClientErrorCode::NotReady && notReadyErased >= secret.size(),
                          "a sensitive NotReady submission overwrites its Core-owned parameter storage before rejection");
        result.expectTrue(!capacitySubmission && capacitySubmission.error.has_value() &&
                              capacitySubmission.error->clientCode == core::ClientErrorCode::TooManyPendingOperations &&
                              capacityErased >= secret.size(),
                          "a sensitive capacity rejection overwrites its Core-owned parameter storage before returning");
        result.expectTrue(!deniedSubmission && deniedSubmission.error.has_value() &&
                              deniedSubmission.error->clientCode == core::ClientErrorCode::MethodNotPermitted &&
                              deniedErased >= secret.size(),
                          "a sensitive policy denial overwrites its Core-owned parameter storage before returning");
    }

    void testSensitiveSerializedSizeProbe(tests::support::TestResult& result) {
        const std::string secret =
            "CLIENT_CORE_SERIALIZED_SIZE_SECRET_SENTINEL_" + std::string(512, 's') + "_END_SENTINEL";

        Harness measuredHarness;
        core::ClientCore measured(clientOptions());
        (void) ready(measured, measuredHarness);
        const std::size_t erasedBefore = core::ClientCoreTestAccess::erasedTransientBytes(measured);
        const core::Submission measuredSubmission = measured.submit(userInputParameters(secret));
        const std::size_t erasedAfter = core::ClientCoreTestAccess::erasedTransientBytes(measured);
        const auto* measuredCommand = measuredHarness.outbound.empty()
                                          ? nullptr
                                          : std::get_if<generated::DefinedCommand>(&measuredHarness.outbound.back().value);
        const std::optional<frontend::CodecResult<frontend::Json>> measuredWire =
            measuredCommand ? std::optional{frontend::Codec::encodeDefinedCommand(*measuredCommand)} : std::nullopt;
        const std::size_t exactBytes = measuredWire && *measuredWire ? measuredWire->value().dump().size() : 0;
        const std::size_t erasedDelta = erasedAfter >= erasedBefore ? erasedAfter - erasedBefore : 0;

        core::ClientOptions belowOptions = clientOptions();
        belowOptions.limits.maximumOutboundMessageBytes = exactBytes > 0 ? exactBytes - 1 : 0;
        Harness belowHarness;
        core::ClientCore below(std::move(belowOptions));
        (void) ready(below, belowHarness);
        const std::size_t belowMessagesBefore = belowHarness.outbound.size();
        const core::Submission belowSubmission = below.submit(userInputParameters(secret));

        core::ClientOptions exactOptions = clientOptions();
        exactOptions.limits.maximumOutboundMessageBytes = exactBytes;
        Harness exactHarness;
        core::ClientCore exact(std::move(exactOptions));
        (void) ready(exact, exactHarness);
        const std::size_t exactMessagesBefore = exactHarness.outbound.size();
        const core::Submission exactSubmission = exact.submit(userInputParameters(secret));

        result.expectTrue(measuredSubmission && measuredWire && *measuredWire && exactBytes > secret.size() &&
                              erasedDelta >= exactBytes + secret.size(),
                          "sensitive command sizing overwrites both the exact compact size probe and validated secret storage");
        result.expectTrue(!belowSubmission && belowSubmission.error && below.ready() &&
                              belowHarness.outbound.size() == belowMessagesBefore && exactSubmission && exact.ready() &&
                              exactHarness.outbound.size() == exactMessagesBefore + 1,
                          "sensitive command preflight rejects exact compact size minus one and accepts the exact byte boundary");
    }

    void testPublishedRevisionExhaustion(tests::support::TestResult& result) {
        std::size_t publications = 0;
        std::size_t updates = 0;
        std::vector<core::StateChange> stateChanges;
        core::ClientCallbacks callbacks;
        callbacks.onStatePublished = [&publications](const auto&) {
            ++publications;
        };
        callbacks.onStateUpdated = [&updates](const auto&) {
            ++updates;
        };
        callbacks.onConnectionStateChanged = [&stateChanges](const core::StateChange& change) {
            stateChanges.push_back(change);
        };

        Harness harness;
        core::ClientCore client(clientOptions(), std::move(callbacks));
        const core::PhysicalGeneration generation = ready(client, harness);
        publications = 0;
        updates = 0;
        stateChanges.clear();
        core::ClientCoreTestAccess::setPublishedRevision(client, std::numeric_limits<std::uint64_t>::max());
        const std::shared_ptr<const core::PublishedState> prior = client.state();

        frontend::FrontendEvent event = diagnosticEvent(1);
        const bool accepted =
            client.receive(generation, frontend::ServerMessage{frontend::EventBatch{event.sequence, event.sequence, {std::move(event)}}});
        const auto terminal = std::find_if(stateChanges.rbegin(), stateChanges.rend(), [](const core::StateChange& change) {
            return change.current == core::ConnectionState::Disconnected;
        });

        result.expectTrue(!accepted && client.connectionState() == core::ConnectionState::Disconnected && harness.closes == 1,
                          "a changed canonical publication cannot reuse UINT64_MAX and deterministically closes its generation");
        result.expectTrue(client.state() == prior && client.state()->revision == std::numeric_limits<std::uint64_t>::max() &&
                              publications == 0 && updates == 0,
                          "revision exhaustion leaves the prior immutable publication and callback-visible revision unchanged");
        result.expectTrue(terminal != stateChanges.rend() && terminal->error.has_value() &&
                              terminal->error->clientCode == core::ClientErrorCode::StateCapacityExceeded,
                          "revision exhaustion reports the existing bounded StateCapacityExceeded terminal error");

        std::size_t disconnectPublications = 0;
        std::size_t disconnectUpdates = 0;
        core::ClientCallbacks disconnectCallbacks;
        disconnectCallbacks.onStatePublished = [&disconnectPublications](const auto&) {
            ++disconnectPublications;
        };
        disconnectCallbacks.onStateUpdated = [&disconnectUpdates](const auto&) {
            ++disconnectUpdates;
        };
        Harness disconnectHarness;
        core::ClientCore disconnectClient(clientOptions(), std::move(disconnectCallbacks));
        const core::PhysicalGeneration disconnectGeneration = ready(disconnectClient, disconnectHarness);
        disconnectPublications = 0;
        disconnectUpdates = 0;
        core::ClientCoreTestAccess::setPublishedRevision(disconnectClient, std::numeric_limits<std::uint64_t>::max());
        const std::shared_ptr<const core::PublishedState> disconnectPrior = disconnectClient.state();
        disconnectClient.transportDisconnected(disconnectGeneration, core::TransportError{"revision boundary disconnect", true});

        result.expectTrue(disconnectClient.connectionState() == core::ConnectionState::Disconnected &&
                              disconnectClient.state() == disconnectPrior &&
                              disconnectClient.state()->revision == std::numeric_limits<std::uint64_t>::max() &&
                              disconnectPublications == 0 && disconnectUpdates == 0,
                          "revision exhaustion prevents retained and bounded-empty stale fallbacks from wrapping or publishing");
    }

    void testPublishedRevisionAuthority(tests::support::TestResult& result) {
        std::size_t prepared = 0;
        std::size_t committed = 0;
        std::size_t published = 0;
        std::size_t cursors = 0;
        std::size_t updated = 0;
        core::ClientCallbacks callbacks;
        callbacks.prepareStatePublication = [&prepared](const core::PublishedState&) -> std::optional<core::ClientError> {
            ++prepared;
            return std::nullopt;
        };
        callbacks.commitStatePublication = [&committed](const core::PublishedState&) {
            ++committed;
        };
        callbacks.onStatePublished = [&published](const auto&) {
            ++published;
        };
        callbacks.onCursorAdvanced = [&cursors](model::FrontendSequence) {
            ++cursors;
        };
        callbacks.onStateUpdated = [&updated](const auto&) {
            ++updated;
        };

        Harness harness;
        core::ClientCore client(clientOptions(), std::move(callbacks));
        (void) ready(client, harness);
        prepared = 0;
        committed = 0;
        published = 0;
        cursors = 0;
        updated = 0;
        const std::shared_ptr<const core::PublishedState> prior = client.state();
        const bool reused = core::ClientCoreTestAccess::tryCommitPublishedRevision(client, prior->revision);
        const bool regressed =
            prior->revision == 0 ? false : core::ClientCoreTestAccess::tryCommitPublishedRevision(client, prior->revision - 1);

        result.expectTrue(!reused && !regressed && client.state() == prior && client.ready(),
                          "the central publication authority rejects reused and regressed candidate revisions transactionally");
        result.expectTrue(prepared == 0 && committed == 0 && published == 0 && cursors == 0 && updated == 0,
                          "rejected candidate revisions never reach preparation, commit, publication, cursor, or update callbacks");

        std::size_t reentrantPreparations = 0;
        std::size_t reentrantCommits = 0;
        bool nestedCommitted = false;
        bool nesting = false;
        core::ClientCore* reentrantClient = nullptr;
        core::ClientCallbacks reentrantCallbacks;
        reentrantCallbacks.prepareStatePublication = [&](const core::PublishedState& candidate) -> std::optional<core::ClientError> {
            ++reentrantPreparations;
            if (!nesting) {
                nesting = true;
                nestedCommitted = core::ClientCoreTestAccess::tryCommitPublishedRevision(*reentrantClient, candidate.revision);
                nesting = false;
            }
            return std::nullopt;
        };
        reentrantCallbacks.commitStatePublication = [&reentrantCommits](const core::PublishedState&) {
            ++reentrantCommits;
        };
        core::ClientCore reentrant(clientOptions(), std::move(reentrantCallbacks));
        reentrantClient = &reentrant;
        const std::shared_ptr<const core::PublishedState> reentrantPrior = reentrant.state();
        const bool outerCommitted = core::ClientCoreTestAccess::tryCommitPublishedRevision(reentrant, reentrantPrior->revision + 1);

        result.expectTrue(nestedCommitted && !outerCommitted && reentrant.state() != reentrantPrior &&
                              reentrant.state()->revision == reentrantPrior->revision + 1 && reentrantPreparations == 2 &&
                              reentrantCommits == 1,
                          "a reentrant preparation may commit one exact-next revision but the stale outer candidate cannot reuse it");

        std::size_t receivePublications = 0;
        std::size_t receiveUpdates = 0;
        std::size_t receiveCursors = 0;
        std::size_t receiveCloses = 0;
        std::optional<core::UpdateCause> receiveUpdateCause;
        bool receiveNestedCommitted = false;
        bool receiveNesting = false;
        core::ClientCore* receiveClient = nullptr;
        core::ClientCallbacks receiveCallbacks;
        receiveCallbacks.prepareStatePublication = [&](const core::PublishedState& candidate) -> std::optional<core::ClientError> {
            if (!receiveNesting && receiveClient && receiveClient->ready()) {
                receiveNesting = true;
                receiveNestedCommitted = core::ClientCoreTestAccess::tryCommitPublishedRevision(*receiveClient, candidate.revision);
                receiveNesting = false;
            }
            return std::nullopt;
        };
        receiveCallbacks.onStatePublished = [&receivePublications](const auto&) {
            ++receivePublications;
        };
        receiveCallbacks.onStateUpdated = [&receiveUpdates, &receiveUpdateCause](const core::StateUpdate& update) {
            ++receiveUpdates;
            receiveUpdateCause = update.cause;
        };
        receiveCallbacks.onCursorAdvanced = [&receiveCursors](model::FrontendSequence) {
            ++receiveCursors;
        };
        receiveCallbacks.onConnectionStateChanged = [&receiveCloses](const core::StateChange& change) {
            if (change.current == core::ConnectionState::Disconnected) {
                ++receiveCloses;
            }
        };
        Harness receiveHarness;
        core::ClientCore receiveReentrant(clientOptions(), std::move(receiveCallbacks));
        receiveClient = &receiveReentrant;
        const core::PhysicalGeneration receiveGeneration = ready(receiveReentrant, receiveHarness);
        receiveNestedCommitted = false;
        receivePublications = 0;
        receiveUpdates = 0;
        receiveCursors = 0;
        const std::shared_ptr<const core::PublishedState> receivePrior = receiveReentrant.state();
        frontend::FrontendEvent receiveEvent = diagnosticEvent(1);
        const bool receiveAccepted = receiveReentrant.receive(
            receiveGeneration,
            frontend::ServerMessage{frontend::EventBatch{receiveEvent.sequence, receiveEvent.sequence, {std::move(receiveEvent)}}});
        result.expectTrue(
            !receiveAccepted && receiveNestedCommitted && receiveReentrant.state()->revision == receivePrior->revision + 2 &&
                receiveReentrant.state()->freshness == core::PublishedFreshness::Stale && receivePublications == 1 && receiveUpdates == 1 &&
                receiveUpdateCause == core::UpdateCause::ConnectionBecameStale && receiveCursors == 0 && receiveCloses == 1 &&
                receiveHarness.closes == 1 && receiveReentrant.connectionState() == core::ConnectionState::Disconnected,
            "a stale receive-path candidate emits only the deterministic later stale publication, never its live update or cursor");

        bool maximumNestedCommitted = false;
        bool maximumNesting = false;
        core::ClientCore* maximumClient = nullptr;
        std::vector<core::StateChange> maximumChanges;
        core::ClientCallbacks maximumCallbacks;
        maximumCallbacks.prepareStatePublication = [&](const core::PublishedState& candidate) -> std::optional<core::ClientError> {
            if (!maximumNesting && maximumClient && maximumClient->ready()) {
                maximumNesting = true;
                maximumNestedCommitted = core::ClientCoreTestAccess::tryCommitPublishedRevision(*maximumClient, candidate.revision);
                maximumNesting = false;
            }
            return std::nullopt;
        };
        maximumCallbacks.onConnectionStateChanged = [&maximumChanges](const core::StateChange& change) {
            maximumChanges.push_back(change);
        };
        Harness maximumHarness;
        core::ClientCore maximumReentrant(clientOptions(), std::move(maximumCallbacks));
        maximumClient = &maximumReentrant;
        const core::PhysicalGeneration maximumGeneration = ready(maximumReentrant, maximumHarness);
        core::ClientCoreTestAccess::setPublishedRevision(maximumReentrant, std::numeric_limits<std::uint64_t>::max() - 1);
        maximumChanges.clear();
        frontend::FrontendEvent maximumEvent = diagnosticEvent(1);
        const bool maximumAccepted = maximumReentrant.receive(
            maximumGeneration,
            frontend::ServerMessage{frontend::EventBatch{maximumEvent.sequence, maximumEvent.sequence, {std::move(maximumEvent)}}});
        const auto maximumTerminal = std::find_if(maximumChanges.rbegin(), maximumChanges.rend(), [](const core::StateChange& change) {
            return change.current == core::ConnectionState::Disconnected;
        });
        result.expectTrue(!maximumAccepted && maximumNestedCommitted &&
                              maximumReentrant.state()->revision == std::numeric_limits<std::uint64_t>::max() &&
                              maximumReentrant.connectionState() == core::ConnectionState::Disconnected && maximumHarness.closes == 1 &&
                              maximumTerminal != maximumChanges.rend() && maximumTerminal->error &&
                              maximumTerminal->error->clientCode == core::ClientErrorCode::StateCapacityExceeded,
                          "reentrant publication reaching UINT64_MAX applies the deterministic exhaustion policy without wrapping");
    }

    void testStateAndDiagnosticBounds(tests::support::TestResult& result) {
        core::ClientOptions stateOptions = clientOptions();
        stateOptions.limits.maximumRetainedEntities = 1;
        Harness stateHarness;
        core::ClientCore stateClient(std::move(stateOptions));
        model::CanonicalSnapshot oversized;
        oversized.threads.emplace_back(model::ThreadIdentity{"thread-1"});
        oversized.threads.emplace_back(model::ThreadIdentity{"thread-2"});
        (void) ready(stateClient, stateHarness, std::move(oversized));
        result.expectTrue(stateClient.connectionState() == core::ConnectionState::Disconnected && stateHarness.closes == 1,
                          "decoded typed State exceeding its retained-entity ceiling closes the physical generation");

        model::CanonicalSnapshot boundaryState;
        boundaryState.threads.emplace_back(model::ThreadIdentity{"boundary-thread"});
        Harness measurementHarness;
        core::ClientCore measurementClient(clientOptions());
        (void) ready(measurementClient, measurementHarness, boundaryState);
        const std::size_t boundaryBytes = *measurementClient.state()->measuredBytes();
        core::ClientOptions admittedOptions = clientOptions();
        admittedOptions.limits.maximumDecodedStateBytes = boundaryBytes;
        Harness admittedHarness;
        core::ClientCore admitted(std::move(admittedOptions));
        (void) ready(admitted, admittedHarness, boundaryState);
        core::ClientOptions rejectedOptions = clientOptions();
        rejectedOptions.limits.maximumDecodedStateBytes = boundaryBytes - 1;
        Harness rejectedHarness;
        core::ClientCore rejected(std::move(rejectedOptions));
        (void) ready(rejected, rejectedHarness, boundaryState);
        result.expectTrue(admitted.ready() && rejected.connectionState() == core::ConnectionState::Disconnected,
                          "maximumDecodedStateBytes admits the exact normalized public-State boundary and rejects one byte below it");

        core::ClientOptions diagnosticOptions = clientOptions();
        diagnosticOptions.limits.maximumRetainedDiagnostics = 1;
        diagnosticOptions.limits.maximumLocalDiagnostics = 2;
        core::ClientCallbacks callbacks;
        callbacks.onDiagnostic = [](const core::Diagnostic&) {
            throw std::runtime_error("contained");
        };
        Harness diagnosticHarness;
        core::ClientCore diagnosticClient(std::move(diagnosticOptions), std::move(callbacks));
        const core::PhysicalGeneration diagnosticGeneration = ready(diagnosticClient, diagnosticHarness);
        for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
            frontend::FrontendEvent event = diagnosticEvent(sequence);
            (void) diagnosticClient.receive(
                diagnosticGeneration, frontend::ServerMessage{frontend::EventBatch{event.sequence, event.sequence, {std::move(event)}}});
        }
        const auto state = diagnosticClient.state();
        (void) diagnosticClient.attach({});
        (void) diagnosticClient.attach({});
        result.expectTrue(state->snapshot->diagnostics.entries.size() == 1,
                          "protocol diagnostic entries are trimmed to the configured retained ceiling");
        result.expectTrue(state->snapshot->diagnostics.state.truncation.truncated &&
                              state->snapshot->diagnostics.state.truncation.omittedEntries == 2,
                          "protocol diagnostic trimming records exact omission metadata");
        result.expectTrue(diagnosticClient.diagnostics().size() <= 2,
                          "local diagnostic retention is independently bounded and diagnostic callback exceptions are contained");
    }

    void testTransportAndInboundBounds(tests::support::TestResult& result) {
        Harness rejectedHarness;
        rejectedHarness.status = core::SendStatus::Backpressure;
        core::ClientCore rejectedClient(clientOptions());
        const core::PhysicalGeneration rejectedGeneration = *rejectedClient.attach(rejectedHarness.transport());
        rejectedClient.transportConnected(rejectedGeneration);
        result.expectTrue(rejectedClient.connectionState() == core::ConnectionState::Disconnected && rejectedHarness.closes == 1,
                          "transport backpressure while sending Hello invalidates only the active physical generation");

        core::ClientOptions inboundOptions = clientOptions();
        inboundOptions.limits.maximumInboundMessageBytes = 8;
        Harness inboundHarness;
        core::ClientCore inboundClient(std::move(inboundOptions));
        const core::PhysicalGeneration inboundGeneration = *inboundClient.attach(inboundHarness.transport());
        inboundClient.transportConnected(inboundGeneration);
        const bool accepted = inboundClient.receiveEncoded(inboundGeneration, std::string(9, 'x'));
        result.expectTrue(!accepted && inboundClient.connectionState() == core::ConnectionState::Disconnected && inboundHarness.closes == 1,
                          "the inbound frame ceiling is enforced before protocol decoding or state allocation");

        core::ClientOptions outboundOptions = clientOptions();
        outboundOptions.limits.maximumOutboundMessageBytes = 512;
        Harness outboundHarness;
        core::ClientCore outboundClient(std::move(outboundOptions));
        (void) ready(outboundClient, outboundHarness);
        const std::size_t messagesBeforeSubmission = outboundHarness.outbound.size();
        frontend::Json answer = {{"questionId", "question"}, {"answers", frontend::Json::array({std::string(512, 'x')})}};
        frontend::Json response = {{"pendingRequestId", "1"}, {"answers", frontend::Json::array({std::move(answer)})}};
        const core::Submission oversized = outboundClient.submit(
            generated::makeParameters(generated::MethodId::UserInputRespond, std::move(response)));
        result.expectTrue(!oversized && oversized.error &&
                              oversized.error->clientCode == core::ClientErrorCode::SerializationFailed && outboundClient.ready() &&
                              outboundClient.pendingOperationCount() == 0 && outboundHarness.outbound.size() == messagesBeforeSubmission &&
                              outboundHarness.closes == 0,
                          "an encoded command exceeding the peer ingress budget is rejected locally without disconnecting the client");

        core::ClientOptions expandedOptions = clientOptions();
        expandedOptions.limits.maximumOutboundMessageBytes = 512;
        Harness expandedHarness;
        core::ClientCore expandedClient(std::move(expandedOptions));
        (void) ready(expandedClient, expandedHarness, {}, std::nullopt, 2048);
        frontend::Json expandedAnswer = {
            {"questionId", "question"}, {"answers", frontend::Json::array({std::string(700, 'x')})}};
        frontend::Json expandedResponse = {
            {"pendingRequestId", "1"}, {"answers", frontend::Json::array({std::move(expandedAnswer)})}};
        const core::Submission expanded = expandedClient.submit(
            generated::makeParameters(generated::MethodId::UserInputRespond, std::move(expandedResponse)));
        result.expectTrue(expanded && !expandedHarness.outbound.empty() && expandedHarness.outbound.back().maximumBytes == 2048,
                          "a Welcome ingress advertisement replaces the conservative configured fallback after authentication");

        core::ClientOptions reducedOptions = clientOptions();
        reducedOptions.limits.maximumOutboundMessageBytes = 2048;
        Harness reducedHarness;
        core::ClientCore reducedClient(std::move(reducedOptions));
        (void) ready(reducedClient, reducedHarness, {}, std::nullopt, 512);
        const std::size_t reducedMessagesBefore = reducedHarness.outbound.size();
        frontend::Json reducedAnswer = {
            {"questionId", "question"}, {"answers", frontend::Json::array({std::string(700, 'x')})}};
        frontend::Json reducedResponse = {
            {"pendingRequestId", "1"}, {"answers", frontend::Json::array({std::move(reducedAnswer)})}};
        const core::Submission reduced = reducedClient.submit(
            generated::makeParameters(generated::MethodId::UserInputRespond, std::move(reducedResponse)));
        result.expectTrue(!reduced && reduced.error && reducedClient.ready() &&
                              reducedHarness.outbound.size() == reducedMessagesBefore && reducedHarness.closes == 0,
                          "a smaller advertised ingress limit narrows command preflight without disconnecting the client");
    }

    void testPublicOptionParity(tests::support::TestResult& result) {
        const auto rejected = [](core::ClientOptions options) {
            try {
                core::ClientCore client(std::move(options));
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };

        core::ClientOptions missingCredential;
        core::ClientOptions productRequested = clientOptions();
        productRequested.requestedCapabilities = {frontend::FrontendCapability::CppClientSdk};
        core::ClientOptions missingRepresentation = clientOptions();
        missingRepresentation.requestedCapabilities.clear();
        missingRepresentation.requiredCapabilities = {frontend::FrontendCapability::CompleteBackendDomains};
        core::ClientOptions requiredProduct = clientOptions();
        requiredProduct.requiredCapabilities = {frontend::FrontendCapability::CppClientSdk};
        bool requiredProductAccepted = true;
        try {
            core::ClientCore accepted(std::move(requiredProduct));
        } catch (...) {
            requiredProductAccepted = false;
        }
        result.expectTrue(rejected(std::move(missingCredential)) && rejected(std::move(productRequested)) &&
                              rejected(std::move(missingRepresentation)) && requiredProductAccepted,
                          "CoreOptions preserves credential and representation-capability validation from public ClientOptions");

        core::ClientOptions zeroInboundOptions = clientOptions();
        zeroInboundOptions.limits.maximumInboundMessageBytes = 0;
        Harness zeroInboundHarness;
        core::ClientCore zeroInbound(std::move(zeroInboundOptions));
        (void) ready(zeroInbound, zeroInboundHarness);

        core::ClientOptions zeroStateOptions = clientOptions();
        zeroStateOptions.limits.maximumDecodedStateBytes = 0;
        Harness zeroStateHarness;
        core::ClientCore zeroState(std::move(zeroStateOptions));
        (void) ready(zeroState, zeroStateHarness);

        core::ClientOptions zeroPendingOptions = clientOptions();
        zeroPendingOptions.limits.maximumPendingOperations = 0;
        Harness zeroPendingHarness;
        core::ClientCore zeroPending(std::move(zeroPendingOptions));
        (void) ready(zeroPending, zeroPendingHarness);
        const std::size_t beforeSubmission = zeroPendingHarness.outbound.size();
        const core::Submission submission =
            zeroPending.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()));

        result.expectTrue(zeroInbound.connectionState() == core::ConnectionState::Disconnected && zeroInboundHarness.closes == 1 &&
                              zeroState.connectionState() == core::ConnectionState::Disconnected && zeroStateHarness.closes == 1 &&
                              zeroPending.ready() && !submission && submission.error.has_value() &&
                              submission.error->clientCode == core::ClientErrorCode::TooManyPendingOperations &&
                              zeroPendingHarness.outbound.size() == beforeSubmission,
                          "zero inbound, decoded-State, and pending-operation capacities construct and enforce zero at runtime");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testOperationAndRequestIdBounds(result);
    testSensitiveEarlyRejectionErasure(result);
    testSensitiveSerializedSizeProbe(result);
    testPublishedRevisionExhaustion(result);
    testPublishedRevisionAuthority(result);
    testStateAndDiagnosticBounds(result);
    testTransportAndInboundBounds(result);
    testPublicOptionParity(result);
    return result.processResult();
}
