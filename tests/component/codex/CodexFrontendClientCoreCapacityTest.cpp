/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/client/ClientCore.h"
#include "support/TestResult.h"

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

    core::PhysicalGeneration ready(core::ClientCore& client, Harness& harness, model::CanonicalSnapshot state = {}) {
        const core::PhysicalGeneration generation = *client.attach(harness.transport());
        client.transportConnected(generation);
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
                                  methods(),
                                  methods()};
        (void) client.receive(generation, frontend::ServerMessage{welcome});
        (void) client.receive(generation, frontend::ServerMessage{snapshot(std::move(state))});
        (void) client.receive(generation, frontend::ServerMessage{frontend::SyncComplete{welcome.currentSequence}});
        return generation;
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
    testStateAndDiagnosticBounds(result);
    testTransportAndInboundBounds(result);
    testPublicOptionParity(result);
    return result.processResult();
}
