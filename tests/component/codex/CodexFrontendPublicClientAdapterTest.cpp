/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "ai/openai/codex/frontend/client/detail/ClientTestAccess.h"
#include "ai/openai/codex/frontend/client/detail/StateReducer.h"
#include "ai/openai/codex/frontend/internal/client/CanonicalStateBuilder.h"
#include "ai/openai/codex/frontend/internal/client/ClientCore.h"
#include "ai/openai/codex/frontend/internal/model/Model.h"
#include "ai/openai/codex/frontend/internal/model/Occurrence.h"
#include "oracle/LegacyFrontendClientCapture.h"
#include "support/TestResult.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace client = frontend::client;
    namespace core = frontend::internal::client;
    namespace model = frontend::internal::model;
    namespace generated = frontend::generated;

    std::string traceText(const std::vector<std::string>& trace) {
        std::string result;
        for (const std::string& entry : trace) {
            if (!result.empty()) {
                result += ',';
            }
            result += entry;
        }
        return result;
    }

    std::vector<frontend::FrontendMethod> allMethods() {
        std::vector<frontend::FrontendMethod> result;
        result.reserve(generated::AllMethods.size());
        for (const generated::MethodMetadata& method : generated::AllMethods) {
            result.emplace_back(method.method);
        }
        return result;
    }

    frontend::CapabilityAdvertisement expandedCapabilities() {
        std::vector<frontend::FrontendCapability> defined;
        for (const generated::CapabilityMetadata& capability : generated::AllCapabilities) {
            if (capability.defined) {
                defined.push_back(static_cast<frontend::FrontendCapability>(capability.id));
            }
        }
        const std::vector<frontend::FrontendCapability> selected{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
        };
        return {std::move(defined), selected, selected, frontend::Json::object()};
    }

    std::optional<client::State> buildCanonicalState(client::Client& adopter,
                                                     const core::PublishedState& publication,
                                                     std::size_t maximumBytes,
                                                     std::size_t maximumRetainedDiagnostics,
                                                     std::string& error,
                                                     client::detail::CanonicalStateBuildFailure* failure = nullptr) {
        auto storage = client::detail::CanonicalStateBuilder::build(publication, maximumBytes, maximumRetainedDiagnostics, error, failure);
        if (!storage.has_value()) {
            return std::nullopt;
        }
        return client::detail::ClientTestAccess::adoptStateStorage(adopter, std::move(*storage));
    }

    frontend::Welcome welcome(std::uint64_t sequence) {
        return {"7",
                frontend::SessionRole::Observer,
                frontend::SequenceNumber(sequence),
                frontend::SyncMode::Snapshot,
                frontend::Json{{"permittedScopes", frontend::Json::array({"observe", "control"})},
                               {"projection", frontend::Json{{"identity", "public-adapter"}}}},
                expandedCapabilities(),
                allMethods(),
                allMethods()};
    }

    model::CanonicalSnapshot canonicalSnapshot(std::uint64_t sequence, std::uint64_t generation, std::string title) {
        model::CanonicalSnapshot snapshot;
        snapshot.sequence = model::FrontendSequence(sequence);
        snapshot.provider.lifecycle = model::ProviderLifecycle::Ready;
        snapshot.provider.generation = generation;
        snapshot.provider.desiredRunning = true;
        snapshot.sessions.emplace_back(model::SessionIdentity{"7"});
        snapshot.threads.emplace_back(model::ThreadIdentity{"adapter-thread"});
        snapshot.threads.back().title = std::move(title);
        snapshot.threadList.hasLoadedPage = true;
        snapshot.threadList.complete = true;
        snapshot.threadList.pagesLoaded = 1;
        snapshot.backendCursor.currentSequence = snapshot.sequence;
        return snapshot;
    }

    frontend::Snapshot expandedSnapshot(std::uint64_t sequence, std::uint64_t generation, std::string title) {
        const auto expanded = model::encodeSnapshot(canonicalSnapshot(sequence, generation, std::move(title)));
        if (!expanded) {
            return {frontend::SequenceNumber(sequence), frontend::Json{{"invalid", true}, {"error", expanded.error().message}}};
        }
        const auto encoded = frontend::Codec::encodeExpandedSnapshot(expanded.value());
        return {frontend::SequenceNumber(sequence),
                encoded ? encoded.value().at("state") : frontend::Json{{"invalid", true}, {"error", encoded.error().message}}};
    }

    frontend::FrontendEvent providerEvent(std::uint64_t sequence, std::uint64_t generation) {
        model::ProviderState provider;
        provider.lifecycle = model::ProviderLifecycle::Ready;
        provider.generation = generation;
        provider.desiredRunning = true;
        model::OccurrenceIdentity identity{
            model::FrontendSequence(sequence), model::OccurrenceGroupIdentity{"adapter-live"}, 0, 1, model::SourceStamp{"adapter-source"}};
        auto occurrence = model::makeOccurrence(std::move(identity), model::ProviderUpdatedOccurrence{std::move(provider)});
        if (!occurrence) {
            return {frontend::SequenceNumber(sequence), "invalid", frontend::Json::object()};
        }
        auto expanded = model::encodeExpandedOccurrence(occurrence.value());
        if (!expanded || expanded.value().empty()) {
            return {frontend::SequenceNumber(sequence), "invalid", frontend::Json::object()};
        }
        const frontend::ExpandedFrontendEvent& value = expanded.value().front();
        return {value.sequence, std::string(frontend::toString(value.type)), value.data, value.extensions};
    }

    client::ClientOptions publicOptions() {
        client::ClientOptions result;
        result.credentialProvider = [] {
            return client::AuthenticationContext{frontend::NoCredential{}, std::string{"adapter-continuity"}};
        };
        return result;
    }

    tests::codex::oracle::LegacyFrontendClientCapture capturePublicFrontendClient(std::span<const frontend::ServerMessage> messages,
                                                                                  bool expanded,
                                                                                  bool disconnectAfterMessages,
                                                                                  bool closeAfterMessages = false) {
        tests::codex::oracle::LegacyFrontendClientCapture capture;
        client::ClientOptions options;
        options.credentialProvider = [] {
            return client::AuthenticationContext{frontend::NoCredential{}, std::string{"p3-public-differential"}};
        };
        if (!expanded) {
            options.requestedCapabilities.clear();
        }

        client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&capture](const client::ConnectionStateChange& change) {
            capture.callbacks.emplace_back("connection");
            if (change.error.has_value()) {
                capture.diagnostics.push_back(change.error->message);
            }
        };
        callbacks.onStateUpdated = [&capture](const client::StateUpdate&) {
            capture.callbacks.emplace_back("state");
        };
        callbacks.onSynchronized = [&capture](const client::SynchronizationInfo&) {
            capture.callbacks.emplace_back("synchronized");
        };
        callbacks.onCursorAdvanced = [&capture](frontend::SequenceNumber) {
            capture.callbacks.emplace_back("cursor");
        };
        callbacks.onProtocolMessage = [&capture](const frontend::ServerMessage&) {
            capture.callbacks.emplace_back("protocol");
        };
        callbacks.onDiagnostic = [&capture](const client::Diagnostic& diagnostic) {
            capture.callbacks.emplace_back("diagnostic");
            capture.diagnostics.push_back(diagnostic.message);
        };

        client::Client sdk(std::move(options), std::move(callbacks));
        client::Connection connection = sdk.openConnection({[&capture](client::OutboundMessage message) {
                                                                capture.outbound.push_back(std::move(message.compactJson));
                                                                return client::SendResult{client::SendStatus::Accepted, std::nullopt};
                                                            },
                                                            [&capture](std::string) {
                                                                ++capture.closes;
                                                            }});
        connection.transportConnected();
        capture.states.push_back(client::detail::StateReducer::serializeForTesting(sdk.state()));

        capture.accepted = true;
        for (const frontend::ServerMessage& message : messages) {
            capture.accepted = connection.receive(message).accepted && capture.accepted;
            capture.states.push_back(client::detail::StateReducer::serializeForTesting(sdk.state()));
        }
        capture.ready = sdk.isReady();
        if (closeAfterMessages) {
            connection.close("p3 public differential local close");
            capture.states.push_back(client::detail::StateReducer::serializeForTesting(sdk.state()));
            capture.ready = sdk.isReady();
        } else if (disconnectAfterMessages) {
            connection.transportDisconnected(client::TransportError{"p3 public differential disconnect", true});
            capture.states.push_back(client::detail::StateReducer::serializeForTesting(sdk.state()));
            capture.ready = sdk.isReady();
        }
        // Return a frozen copy so local teardown cannot mutate the result via
        // optional NRVO after the requested differential border.
        return tests::codex::oracle::LegacyFrontendClientCapture{capture};
    }

    frontend::Welcome legacyWelcome(std::uint64_t sequence) {
        frontend::CapabilityAdvertisement capabilities = expandedCapabilities();
        capabilities.implemented.clear();
        capabilities.permitted.clear();
        return {"7",
                frontend::SessionRole::Observer,
                frontend::SequenceNumber(sequence),
                frontend::SyncMode::Snapshot,
                frontend::Json{{"permittedScopes", frontend::Json::array({"observe", "control"})}},
                std::move(capabilities),
                allMethods(),
                allMethods()};
    }

    frontend::Json minimalLegacyState() {
        return {{"backendRevision", std::uint64_t{7}},
                {"lifecycle", "ready"},
                {"diagnostics", {{"received", std::uint64_t{0}}, {"recent", frontend::Json::array()}}},
                {"sessions", frontend::Json::array()},
                {"threadList", {{"hasLoadedPage", false}, {"complete", false}, {"pagesLoaded", std::uint64_t{0}}}},
                {"threads", frontend::Json::array()},
                {"pendingRequests", frontend::Json::array()},
                {"codexExtensions", frontend::Json::array()},
                {"omittedCodexExtensions", std::uint64_t{0}},
                {"journal", {{"oldestReplayableAfter", std::uint64_t{0}}, {"currentSequence", std::uint64_t{1}}}},
                {"sequenceExhausted", false}};
    }

    frontend::Json knownLegacyItem(std::string id) {
        return {{"id", std::move(id)},
                {"type", "command_execution"},
                {"status", "completed"},
                {"agentText", ""},
                {"reasoningText", ""},
                {"reasoningSummary", ""},
                {"commandOutput", "known output"},
                {"droppedContentBytes", std::uint64_t{0}},
                {"contentTruncated", false},
                {"data", {{"exitCode", 0}}},
                {"extensions", {{"knownItemExtension", true}}}};
    }

    frontend::Json futureLegacyItem(std::string id) {
        return {{"id", std::move(id)},
                {"type", "future_item"},
                {"status", "completed"},
                {"agentText", ""},
                {"reasoningText", ""},
                {"reasoningSummary", ""},
                {"commandOutput", ""},
                {"droppedContentBytes", std::uint64_t{0}},
                {"contentTruncated", false},
                {"data", frontend::Json::object()},
                {"truncated", false},
                {"omittedFields", frontend::Json::array()},
                {"extensions", {{"futureItemExtension", true}}}};
    }

    struct LegacyStateDifferential {
        tests::codex::oracle::LegacyFrontendClientCapture oracle;
        tests::codex::oracle::LegacyFrontendClientCapture publicClient;
    };

    LegacyStateDifferential captureLegacyStateDifferential(frontend::Json state) {
        const std::vector<frontend::ServerMessage> messages{
            legacyWelcome(1),
            frontend::Snapshot{frontend::SequenceNumber(1), std::move(state)},
            frontend::SyncComplete{frontend::SequenceNumber(1)},
        };
        return {tests::codex::oracle::captureLegacyFrontendClient(messages, false, false),
                capturePublicFrontendClient(messages, false, false)};
    }

    bool exactLegacyStateParity(const LegacyStateDifferential& capture) {
        return capture.oracle.accepted && capture.publicClient.accepted && capture.oracle.ready && capture.publicClient.ready &&
               capture.oracle.states == capture.publicClient.states && capture.oracle.callbacks == capture.publicClient.callbacks &&
               capture.oracle.outbound == capture.publicClient.outbound && capture.oracle.closes == capture.publicClient.closes;
    }

    std::string legacyStateDifference(const LegacyStateDifferential& capture) {
        if (capture.oracle.accepted != capture.publicClient.accepted || capture.oracle.ready != capture.publicClient.ready) {
            return "accepted=" + std::to_string(capture.oracle.accepted) + "/" + std::to_string(capture.publicClient.accepted) +
                   " ready=" + std::to_string(capture.oracle.ready) + "/" + std::to_string(capture.publicClient.ready) +
                   " diagnostics=" + traceText(capture.oracle.diagnostics) + "/" + traceText(capture.publicClient.diagnostics);
        }
        if (capture.oracle.callbacks != capture.publicClient.callbacks) {
            return "callbacks=" + traceText(capture.oracle.callbacks) + "/" + traceText(capture.publicClient.callbacks);
        }
        if (capture.oracle.outbound != capture.publicClient.outbound) {
            return "outbound differs";
        }
        if (capture.oracle.closes != capture.publicClient.closes) {
            return "closes=" + std::to_string(capture.oracle.closes) + "/" + std::to_string(capture.publicClient.closes);
        }
        if (capture.oracle.states.size() != capture.publicClient.states.size()) {
            return "state-count=" + std::to_string(capture.oracle.states.size()) + "/" + std::to_string(capture.publicClient.states.size());
        }
        for (std::size_t index = 0; index < capture.oracle.states.size(); ++index) {
            const frontend::Json& oracle = capture.oracle.states[index];
            const frontend::Json& publicState = capture.publicClient.states[index];
            if (oracle == publicState) {
                continue;
            }
            for (auto member = oracle.begin(); member != oracle.end(); ++member) {
                const auto found = publicState.find(member.key());
                if (found == publicState.end() || *found != member.value()) {
                    return "state[" + std::to_string(index) + "]." + member.key() + " oracle=" + member.value().dump() +
                           " public=" + (found == publicState.end() ? std::string{"<absent>"} : found->dump());
                }
            }
            for (auto member = publicState.begin(); member != publicState.end(); ++member) {
                if (!oracle.contains(member.key())) {
                    return "state[" + std::to_string(index) + "]." + member.key() + " oracle=<absent> public=" + member.value().dump();
                }
            }
            return "state[" + std::to_string(index) + "] differs";
        }
        return "no difference";
    }

    struct PublicHarness {
        std::vector<client::OutboundMessage> outbound;
        std::vector<std::string> callbackOrder;
        std::vector<std::uint64_t> updateRevisions;
        std::vector<std::uint64_t> synchronizedRevisions;
        std::vector<frontend::SequenceNumber> cursors;
        std::vector<std::string> diagnostics;
        std::size_t protocolMessages = 0;
        std::size_t closes = 0;
        bool recording = false;
        bool revisionMismatch = false;
        bool readySawCommittedState = false;
        client::Client* sdk = nullptr;

        client::TransportCallbacks transport() {
            return {[this](client::OutboundMessage message) {
                        outbound.push_back(std::move(message));
                        return client::SendResult{client::SendStatus::Accepted, std::nullopt};
                    },
                    [this](std::string) {
                        ++closes;
                    }};
        }

        client::ClientCallbacks callbacks() {
            client::ClientCallbacks result;
            result.onConnectionStateChanged = [this](const client::ConnectionStateChange& change) {
                if (change.error.has_value()) {
                    diagnostics.push_back(change.error->message);
                }
                if (recording && change.current == client::ConnectionState::Ready) {
                    callbackOrder.emplace_back("ready");
                    readySawCommittedState = sdk != nullptr && sdk->state().freshness() == client::StateFreshness::Current &&
                                             sdk->state().visibleSequence() == frontend::SequenceNumber(7);
                }
            };
            result.onStateUpdated = [this](const client::StateUpdate& update) {
                if (sdk != nullptr && sdk->state().revision() != update.state.revision()) {
                    revisionMismatch = true;
                }
                updateRevisions.push_back(update.state.revision());
                if (recording) {
                    callbackOrder.emplace_back("state");
                }
            };
            result.onSynchronized = [this](const client::SynchronizationInfo& info) {
                if (sdk != nullptr && sdk->state().revision() != info.state.revision()) {
                    revisionMismatch = true;
                }
                synchronizedRevisions.push_back(info.state.revision());
                if (recording) {
                    callbackOrder.emplace_back("synchronized");
                }
            };
            result.onCursorAdvanced = [this](frontend::SequenceNumber sequence) {
                cursors.push_back(sequence);
                if (recording) {
                    callbackOrder.emplace_back("cursor");
                }
            };
            result.onProtocolMessage = [this](const frontend::ServerMessage&) {
                ++protocolMessages;
                if (recording) {
                    callbackOrder.emplace_back("protocol");
                }
            };
            result.onDiagnostic = [this](const client::Diagnostic& diagnostic) {
                diagnostics.push_back(diagnostic.message);
            };
            return result;
        }
    };

    void testDirectCanonicalStateBuilder(tests::support::TestResult& result) {
        client::Client adopter(publicOptions());
        core::PublishedState publication;
        publication.revision = 41;
        publication.freshness = core::PublishedFreshness::Current;
        publication.representation = core::RepresentationMode::ExpandedV1;
        publication.visibleSequence = model::FrontendSequence{17};
        publication.synchronizedThrough = model::FrontendSequence{16};
        publication.projectionFingerprint = "direct-canonical-fingerprint";
        publication.session.emplace(model::SessionIdentity{"7"});
        publication.session->synchronizationMode = frontend::SyncMode::Snapshot;
        model::CanonicalSnapshot direct = canonicalSnapshot(17, 3, "first title");
        direct.reviews.state = model::DomainState::present();
        direct.integrations.state = model::DomainState::present();
        direct.plugins.state = model::DomainState::present();
        direct.platform.state = model::DomainState::present();
        direct.controller.safeDetails = *model::SafeDetail::fromJson(frontend::Json{{"present", true}});
        direct.truncation.extensions = *model::SafeDetail::fromJson(frontend::Json{{"vendorTruncation", "state-truncation-extension"}});
        direct.processesState.extensions = *model::SafeDetail::fromJson(frontend::Json{{"vendorCollection", "processes-extension"}});
        direct.processesState.truncation.extensions =
            *model::SafeDetail::fromJson(frontend::Json{{"vendorTruncation", "processes-extension-truncation"}});
        model::ProcessState process{model::ProcessHandle{"adapter-process"}};
        process.stamp.extensions = *model::SafeDetail::fromJson(frontend::Json{{"vendorStamp", "process-stamp-extension"}});
        direct.processes.push_back(std::move(process));
        model::PendingRequestData emptyQuestions{model::PendingRequestIdentity{"adapter-pending"}};
        emptyQuestions.questionsPresent = true;
        direct.pendingRequests.push_back(model::UserInputRequest{std::move(emptyQuestions)});
        publication.snapshot = std::make_shared<const model::CanonicalSnapshot>(std::move(direct));

        std::string error;
        const auto first = buildCanonicalState(adopter, publication, std::numeric_limits<std::size_t>::max(), 64, error);
        const client::ThreadState* firstThread = first ? first->thread("adapter-thread") : nullptr;
        result.expectTrue(
            first.has_value() && error.empty() && first->revision() == 41 && first->freshness() == client::StateFreshness::Current &&
                first->representationMode() == client::RepresentationMode::ExpandedV1 &&
                first->visibleSequence() == frontend::SequenceNumber(17) && first->synchronizedThrough() == frontend::SequenceNumber(16) &&
                first->provider().value.has_value() && first->provider().value->generation == 3 && first->controller().value.has_value() &&
                first->controller().value->present &&
                first->controller().value->extensions.find("present") == first->controller().value->extensions.end() &&
                firstThread != nullptr && firstThread->title == std::optional<std::string>{"first title"} &&
                !first->permissionProfiles().value.has_value() && !first->apps().value.has_value() &&
                !first->externalAgents().value.has_value() && !first->hooks().value.has_value() &&
                !first->marketplace().value.has_value() && !first->skills().value.has_value() &&
                !first->windowsSandbox().value.has_value() && !first->platform().value.has_value() && first->truncation().value &&
                first->truncation().value->extensions.value("vendorTruncation", "") == "state-truncation-extension" &&
                first->processes().value && first->processes().value->extensions.value("vendorCollection", "") == "processes-extension" &&
                first->processes().value->truncation.extensions.value("vendorTruncation", "") == "processes-extension-truncation" &&
                first->processes().value->entries.front().stamp.extensions.value("vendorStamp", "") == "process-stamp-extension" &&
                first->pendingRequests().size() == 1 && first->pendingRequests().front().questions.has_value() &&
                first->pendingRequests().front().questions->empty(),
            "CanonicalStateBuilder maps the canonical typed publication directly into every sampled public State border");

        client::State immutable = first.value_or(client::State{});
        publication.revision = 42;
        publication.snapshot = std::make_shared<const model::CanonicalSnapshot>(canonicalSnapshot(18, 4, "second title"));
        publication.visibleSequence = model::FrontendSequence{18};
        const auto second = buildCanonicalState(adopter, publication, std::numeric_limits<std::size_t>::max(), 64, error);
        const client::ThreadState* immutableThread = immutable.thread("adapter-thread");
        const client::ThreadState* secondThread = second ? second->thread("adapter-thread") : nullptr;
        result.expectTrue(second.has_value() && second->revision() == 42 && secondThread != nullptr &&
                              secondThread->title == std::optional<std::string>{"second title"} && immutable.revision() == 41 &&
                              immutableThread != nullptr && immutableThread->title == std::optional<std::string>{"first title"},
                          "a later direct canonical build leaves the prior public State immutable");

        client::detail::CanonicalStateBuildFailure capacityFailure = client::detail::CanonicalStateBuildFailure::StateDivergence;
        const auto rejected = buildCanonicalState(adopter, publication, 1, 64, error, &capacityFailure);
        result.expectTrue(!rejected.has_value() && !error.empty() &&
                              capacityFailure == client::detail::CanonicalStateBuildFailure::Capacity && immutable.revision() == 41 &&
                              immutableThread != nullptr && immutableThread->title == std::optional<std::string>{"first title"},
                          "public capacity preparation rejects before exposing a candidate and preserves the prior immutable State");
    }

    void testCanonicalLookupIdentityPreflight(tests::support::TestResult& result) {
        client::Client adopter(publicOptions());
        core::PublishedState publication;
        publication.revision = 1;
        publication.freshness = core::PublishedFreshness::Current;
        publication.representation = core::RepresentationMode::ExpandedV1;

        const auto rejects = [&adopter, &publication](model::CanonicalSnapshot snapshot, std::string_view expected) {
            publication.snapshot = std::make_shared<const model::CanonicalSnapshot>(std::move(snapshot));
            std::string error;
            client::detail::CanonicalStateBuildFailure failure = client::detail::CanonicalStateBuildFailure::Capacity;
            const auto built = buildCanonicalState(adopter, publication, std::numeric_limits<std::size_t>::max(), 64, error, &failure);
            return !built.has_value() && failure == client::detail::CanonicalStateBuildFailure::StateDivergence &&
                   error.find(expected) != std::string::npos;
        };

        bool allRejected = true;
        model::CanonicalSnapshot sessions = canonicalSnapshot(1, 1, "sessions");
        sessions.sessions.emplace_back(model::SessionIdentity{"7"});
        allRejected = rejects(std::move(sessions), "duplicate session identity") && allRejected;

        model::CanonicalSnapshot threads = canonicalSnapshot(1, 1, "threads");
        threads.threads.emplace_back(model::ThreadIdentity{"adapter-thread"});
        allRejected = rejects(std::move(threads), "duplicate thread identity") && allRejected;

        model::CanonicalSnapshot turns = canonicalSnapshot(1, 1, "turns");
        turns.turns.emplace_back(model::TurnIdentity{"duplicate-turn"}, model::ThreadIdentity{"adapter-thread"});
        turns.turns.emplace_back(model::TurnIdentity{"duplicate-turn"}, model::ThreadIdentity{"adapter-thread"});
        allRejected = rejects(std::move(turns), "duplicate turn identity") && allRejected;

        model::CanonicalSnapshot typedItems = canonicalSnapshot(1, 1, "typed-items");
        typedItems.items.push_back(model::AgentMessageItem{model::ItemData{model::ItemIdentity{"duplicate-item"}}});
        typedItems.items.push_back(model::UserMessageItem{model::ItemData{model::ItemIdentity{"duplicate-item"}}});
        allRejected = rejects(std::move(typedItems), "duplicate item identity") && allRejected;

        model::CanonicalSnapshot mixedItems = canonicalSnapshot(1, 1, "mixed-items");
        mixedItems.items.push_back(model::AgentMessageItem{model::ItemData{model::ItemIdentity{"mixed-item"}}});
        mixedItems.legacyItems.push_back({model::ItemData{model::ItemIdentity{"mixed-item"}}, "future_item", 1, "/items/1"});
        allRejected = rejects(std::move(mixedItems), "duplicate item identity") && allRejected;

        model::CanonicalSnapshot typedPending = canonicalSnapshot(1, 1, "typed-pending");
        typedPending.pendingRequests.push_back(model::UserInputRequest{model::PendingRequestData{model::PendingRequestIdentity{"72"}}});
        typedPending.pendingRequests.push_back(
            model::AuthenticationRequest{model::PendingRequestData{model::PendingRequestIdentity{"72"}}});
        allRejected = rejects(std::move(typedPending), "duplicate pending request identity") && allRejected;

        model::CanonicalSnapshot mixedPending = canonicalSnapshot(1, 1, "mixed-pending");
        mixedPending.pendingRequests.push_back(model::UserInputRequest{model::PendingRequestData{model::PendingRequestIdentity{"73"}}});
        mixedPending.legacyPendingRequests.push_back(
            {model::PendingRequestData{model::PendingRequestIdentity{"73"}}, 1, "/pendingRequests/1"});
        allRejected = rejects(std::move(mixedPending), "duplicate pending request identity") && allRejected;

        model::CanonicalSnapshot processes = canonicalSnapshot(1, 1, "processes");
        processes.processes.emplace_back(model::ProcessHandle{"duplicate-process"});
        processes.processes.emplace_back(model::ProcessHandle{"duplicate-process"});
        allRejected = rejects(std::move(processes), "duplicate process identity") && allRejected;

        model::CanonicalSnapshot watches = canonicalSnapshot(1, 1, "watches");
        model::FilesystemWatchRecord duplicateWatch;
        duplicateWatch.watchId = "duplicate-watch";
        watches.filesystemWatches.entries.push_back(duplicateWatch);
        watches.filesystemWatches.entries.push_back(std::move(duplicateWatch));
        allRejected = rejects(std::move(watches), "duplicate filesystem watch identity") && allRejected;

        model::CanonicalSnapshot searches = canonicalSnapshot(1, 1, "searches");
        model::FuzzySearchRecord duplicateSearch;
        duplicateSearch.sessionId = "duplicate-search";
        searches.fuzzySearches.entries.push_back(duplicateSearch);
        searches.fuzzySearches.entries.push_back(std::move(duplicateSearch));
        allRejected = rejects(std::move(searches), "duplicate fuzzy search identity") && allRejected;

        model::CanonicalSnapshot activities = canonicalSnapshot(1, 1, "activities");
        model::ActivityRecord duplicateActivity;
        duplicateActivity.key = "duplicate-activity";
        activities.activities.entries.push_back(duplicateActivity);
        activities.activities.entries.push_back(std::move(duplicateActivity));
        allRejected = rejects(std::move(activities), "duplicate activity identity") && allRejected;

        model::CanonicalSnapshot emptyActivity = canonicalSnapshot(1, 1, "empty-activity");
        emptyActivity.activities.entries.emplace_back();
        allRejected = rejects(std::move(emptyActivity), "empty activity identity") && allRejected;

        result.expectTrue(
            allRejected,
            "CanonicalStateBuilder rejects duplicate or empty public lookup identities, including typed/legacy overlap, before commit");
    }

    void testHybridExpandedPublicationRetainsLegacyItems(tests::support::TestResult& result) {
        client::Client adopter(publicOptions());
        core::PublishedState publication;
        publication.revision = 1;
        publication.freshness = core::PublishedFreshness::Current;
        publication.representation = core::RepresentationMode::ExpandedV1;

        model::CanonicalSnapshot snapshot = canonicalSnapshot(1, 1, "hybrid-items");
        snapshot.turns.emplace_back(model::TurnIdentity{"hybrid-turn"}, model::ThreadIdentity{"adapter-thread"});
        model::ItemData item{
            model::ItemIdentity{"hybrid-future-item"}, model::ThreadIdentity{"adapter-thread"}, model::TurnIdentity{"hybrid-turn"}};
        snapshot.legacyItems.push_back({std::move(item), "future_item", 0, "/items/0"});
        publication.snapshot = std::make_shared<const model::CanonicalSnapshot>(std::move(snapshot));

        std::string error;
        const auto built = buildCanonicalState(adopter, publication, std::numeric_limits<std::size_t>::max(), 64, error);
        const client::ItemState* publicItem = built ? built->item("hybrid-future-item") : nullptr;
        const client::TurnState* publicTurn = built ? built->turn("hybrid-turn") : nullptr;
        result.expectTrue(
            built.has_value() && error.empty() && publicItem != nullptr && publicItem->kind.identity == "future_item" &&
                !publicItem->kind.known.has_value() && publicTurn != nullptr && publicTurn->orderedItems.size() == 1 &&
                publicTurn->orderedItems.front().value == "hybrid-future-item",
            "an ExpandedV1 publication with legacy-compatible items preserves the independently negotiated item representation");
    }

    void testPublicClientCoreAdapter(tests::support::TestResult& result) {
        PublicHarness harness;
        client::Client sdk(publicOptions(), harness.callbacks());
        harness.sdk = &sdk;
        client::Connection connection = sdk.openConnection(harness.transport());
        const std::uint64_t firstGeneration = connection.generation();
        connection.transportConnected();
        connection.transportConnected();

        const auto hello = harness.outbound.empty() ? frontend::Codec::decodeClient(std::string_view{})
                                                    : frontend::Codec::decodeClient(std::string_view(harness.outbound.front().compactJson));
        result.expectTrue(firstGeneration == 1 && harness.outbound.size() == 1 && hello &&
                              std::holds_alternative<frontend::Hello>(hello.value()) &&
                              sdk.connectionState() == client::ConnectionState::Authenticating,
                          "public Connection delegates one physical generation and one transport-connected transition to ClientCore");

        const bool welcomeAccepted = connection.receive(frontend::ServerMessage{welcome(7)}).accepted;
        const frontend::Snapshot initialSnapshot = expandedSnapshot(7, 3, "runtime title");
        const auto fixtureDecoded = model::decodeProjectedSnapshot(initialSnapshot, publicOptions().requestedCapabilities);
        const std::string fixtureError = fixtureDecoded ? std::string{} : fixtureDecoded.error().message;
        const bool snapshotAccepted = connection.receive(frontend::ServerMessage{initialSnapshot}).accepted;
        harness.recording = true;
        const bool synchronized = connection.receive(frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(7)}}).accepted;
        harness.recording = false;
        const client::State ready = sdk.state();
        const std::vector<std::string> expectedSynchronizationOrder{"ready", "state", "cursor", "synchronized", "protocol"};
        result.expectTrue(
            welcomeAccepted && snapshotAccepted && synchronized && sdk.isReady() && !harness.revisionMismatch &&
                harness.readySawCommittedState && harness.callbackOrder == expectedSynchronizationOrder &&
                !harness.updateRevisions.empty() && !harness.synchronizedRevisions.empty() &&
                harness.updateRevisions.back() == ready.revision() && harness.synchronizedRevisions.back() == ready.revision() &&
                ready.visibleSequence() == frontend::SequenceNumber(7) && ready.thread("adapter-thread") != nullptr &&
                ready.thread("adapter-thread")->title == std::optional<std::string>{"runtime title"},
            "ClientCore commits the direct public State before state/cursor/synchronized/protocol callbacks in frozen order: " +
                traceText(harness.callbackOrder) + " accepted=" + std::to_string(welcomeAccepted) + "/" + std::to_string(snapshotAccepted) +
                "/" + std::to_string(synchronized) + " state=" + std::to_string(static_cast<int>(sdk.connectionState())) + " fixture=" +
                fixtureError + " fixture-state=" + initialSnapshot.state.dump() + " diagnostics=" + traceText(harness.diagnostics));

        harness.callbackOrder.clear();
        harness.recording = true;
        const frontend::FrontendEvent live = providerEvent(8, 9);
        const bool liveAccepted =
            connection
                .receive(frontend::ServerMessage{frontend::EventBatch{frontend::SequenceNumber(8), frontend::SequenceNumber(8), {live}}})
                .accepted;
        harness.recording = false;
        const client::State afterLive = sdk.state();
        const std::vector<std::string> expectedLiveOrder{"state", "cursor", "protocol"};
        result.expectTrue(
            liveAccepted && harness.callbackOrder == expectedLiveOrder && ready.revision() != std::numeric_limits<std::uint64_t>::max() &&
                afterLive.revision() == ready.revision() + 1 && afterLive.provider().value.has_value() &&
                afterLive.provider().value->generation == 9 && ready.provider().value.has_value() &&
                ready.provider().value->generation == 3 && ready.revision() != afterLive.revision() && !harness.revisionMismatch,
            "live ClientCore publication advances one public revision while the prior State remains immutable: " +
                traceText(harness.callbackOrder));

        const client::State beforeDisconnect = afterLive;
        connection.transportDisconnected(client::TransportError{"cycle one ended", true});
        const client::State retainedStale = sdk.state();
        result.expectTrue(
            beforeDisconnect.revision() != std::numeric_limits<std::uint64_t>::max() &&
                retainedStale.revision() == beforeDisconnect.revision() + 1 && retainedStale.freshness() == client::StateFreshness::Stale &&
                beforeDisconnect.freshness() == client::StateFreshness::Current && beforeDisconnect.provider().value.has_value() &&
                beforeDisconnect.provider().value->generation == 9 && !harness.revisionMismatch,
            "public Client exposes the ClientCore retained-stale N+1 revision without mutating its prior State");
        client::Connection replacement = sdk.openConnection(harness.transport());
        const std::size_t observedBeforeStale = harness.protocolMessages;
        const auto stale = connection.receive(frontend::ServerMessage{frontend::ProtocolErrorMessage{
            frontend::ErrorCode::RateLimited, "retired generation", {}, false, std::nullopt, std::nullopt, frontend::Json::object()}});
        result.expectTrue(!stale.accepted && replacement.generation() == firstGeneration + 1 &&
                              sdk.connectionState() == client::ConnectionState::Connecting &&
                              harness.protocolMessages == observedBeforeStale,
                          "a retired public Connection cannot continue into the next ClientCore physical generation");
    }

    void testTransactionalPreparationFailure(tests::support::TestResult& result) {
        core::ClientOptions options;
        options.credentialProvider = [] {
            return core::AuthenticationContext{frontend::NoCredential{}, std::string{"prepare-failure"}};
        };
        std::size_t prepared = 0;
        std::size_t committed = 0;
        std::size_t published = 0;
        std::size_t readyTransitions = 0;
        bool currentCommitted = false;
        std::vector<std::string> diagnostics;
        core::ClientCallbacks callbacks;
        callbacks.prepareStatePublication = [&prepared](const core::PublishedState& candidate) -> std::optional<core::ClientError> {
            ++prepared;
            if (candidate.freshness == core::PublishedFreshness::Current) {
                return core::ClientError{core::ErrorOrigin::Protocol,
                                         core::ClientErrorCode::StateCapacityExceeded,
                                         frontend::ErrorCode::CapacityExceeded,
                                         "injected public State capacity rejection",
                                         std::nullopt,
                                         false};
            }
            return std::nullopt;
        };
        callbacks.commitStatePublication = [&committed, &currentCommitted](const core::PublishedState& candidate) {
            ++committed;
            currentCommitted = currentCommitted || candidate.freshness == core::PublishedFreshness::Current;
        };
        callbacks.onStatePublished = [&published](const std::shared_ptr<const core::PublishedState>&) {
            ++published;
        };
        callbacks.onDiagnostic = [&diagnostics](const core::Diagnostic& diagnostic) {
            diagnostics.push_back(diagnostic.message);
        };
        callbacks.onConnectionStateChanged = [&diagnostics, &readyTransitions](const core::StateChange& change) {
            if (change.current == core::ConnectionState::Ready) {
                ++readyTransitions;
            }
            if (change.error.has_value()) {
                diagnostics.push_back(change.error->message);
            }
        };
        core::ClientCore sdk(std::move(options), std::move(callbacks));
        std::vector<core::OutboundMessage> outbound;
        const auto generation = sdk.attach({[&outbound](core::OutboundMessage message) {
                                                outbound.push_back(std::move(message));
                                                return core::SendResult{};
                                            },
                                            [](std::string_view) {
                                            }});
        sdk.transportConnected(*generation);
        const bool welcomeAccepted = sdk.receive(*generation, frontend::ServerMessage{welcome(7)});
        const bool snapshotAccepted = sdk.receive(*generation, frontend::ServerMessage{expandedSnapshot(7, 3, "rejected title")});
        const bool accepted = sdk.receive(*generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(7)}});
        result.expectTrue(!accepted && prepared == 3 && committed == 2 && published == 2 && readyTransitions == 0 && !currentCommitted &&
                              sdk.state()->revision == 2 && sdk.state()->freshness == core::PublishedFreshness::Stale &&
                              sdk.connectionState() != core::ConnectionState::Ready,
                          "final SyncComplete preparation rejects before any transient Ready while preserving the last committed revision; "
                          "prepared=" +
                              std::to_string(prepared) + " committed=" + std::to_string(committed) +
                              " published=" + std::to_string(published) + " revision=" + std::to_string(sdk.state()->revision) +
                              " ready=" + std::to_string(readyTransitions) + " accepted=" + std::to_string(welcomeAccepted) + "/" +
                              std::to_string(snapshotAccepted) + "/" + std::to_string(accepted) + " diagnostics=" + traceText(diagnostics));
    }

    void testTransactionalStaleFallback(tests::support::TestResult& result) {
        core::ClientOptions options;
        options.credentialProvider = [] {
            return core::AuthenticationContext{frontend::NoCredential{}, std::string{"stale-fallback"}};
        };
        std::size_t staleRejections = 0;
        std::size_t emptyStaleCommits = 0;
        core::ClientCallbacks callbacks;
        callbacks.prepareStatePublication = [&staleRejections](const core::PublishedState& candidate) -> std::optional<core::ClientError> {
            if (candidate.freshness == core::PublishedFreshness::Stale && candidate.snapshot) {
                ++staleRejections;
                return core::ClientError{core::ErrorOrigin::Protocol,
                                         core::ClientErrorCode::StateCapacityExceeded,
                                         frontend::ErrorCode::CapacityExceeded,
                                         "injected retained stale capacity rejection",
                                         std::nullopt,
                                         false};
            }
            return std::nullopt;
        };
        callbacks.commitStatePublication = [&emptyStaleCommits](const core::PublishedState& committed) {
            if (committed.freshness == core::PublishedFreshness::Stale && !committed.snapshot && !committed.session) {
                ++emptyStaleCommits;
            }
        };
        core::ClientCore sdk(std::move(options), std::move(callbacks));
        const auto generation = sdk.attach({[](core::OutboundMessage) {
                                                return core::SendResult{};
                                            },
                                            [](std::string_view) {
                                            }});
        sdk.transportConnected(*generation);
        const bool welcomeAccepted = sdk.receive(*generation, frontend::ServerMessage{welcome(7)});
        const bool snapshotAccepted = sdk.receive(*generation, frontend::ServerMessage{expandedSnapshot(7, 3, "stale title")});
        const bool synchronized = sdk.receive(*generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(7)}});
        const std::shared_ptr<const core::PublishedState> prior = sdk.state();
        const std::uint64_t priorRevision = prior->revision;
        const std::string priorTitle = prior->snapshot && !prior->snapshot->threads.empty()
                                           ? prior->snapshot->threads.front().title.value_or(std::string{})
                                           : std::string{};
        sdk.transportDisconnected(*generation, core::TransportError{"capacity boundary disconnect", true});
        const std::shared_ptr<const core::PublishedState> stale = sdk.state();
        result.expectTrue(welcomeAccepted && snapshotAccepted && synchronized && staleRejections == 1 && emptyStaleCommits == 1 &&
                              priorRevision != std::numeric_limits<std::uint64_t>::max() && stale && stale->revision == priorRevision + 1 &&
                              stale->freshness == core::PublishedFreshness::Stale && !stale->snapshot && !stale->session &&
                              sdk.connectionState() == core::ConnectionState::Disconnected,
                          "a retained stale preparation failure atomically publishes the bounded empty stale fallback at revision N+1: "
                          "prior=" +
                              std::to_string(priorRevision) +
                              " fallback=" + std::to_string(stale ? stale->revision : std::numeric_limits<std::uint64_t>::max()));
        result.expectTrue(stale != prior && prior->revision == priorRevision && prior->freshness == core::PublishedFreshness::Current &&
                              prior->snapshot && !prior->snapshot->threads.empty() &&
                              prior->snapshot->threads.front().title.value_or(std::string{}) == priorTitle,
                          "bounded-empty stale fallback leaves the prior published State immutable");
    }

    void testPublicClientLegacyOracleDifferential(tests::support::TestResult& result) {
        const frontend::FrontendEvent live = providerEvent(8, 9);
        const std::vector<frontend::ServerMessage> expandedMessages{
            welcome(7),
            expandedSnapshot(7, 3, "differential title"),
            frontend::SyncComplete{frontend::SequenceNumber(7)},
            frontend::EventBatch{frontend::SequenceNumber(8), frontend::SequenceNumber(8), {live}},
        };
        const auto expandedOracle = tests::codex::oracle::captureLegacyFrontendClient(expandedMessages, true, true);
        const auto expandedPublic = capturePublicFrontendClient(expandedMessages, true, true);
        result.expectTrue(expandedOracle.accepted && expandedPublic.accepted && !expandedOracle.ready && !expandedPublic.ready &&
                              expandedOracle.states == expandedPublic.states && expandedOracle.callbacks == expandedPublic.callbacks &&
                              expandedOracle.outbound == expandedPublic.outbound && expandedOracle.closes == expandedPublic.closes,
                          "the cut-over public Client matches the preserved old client exactly at expanded snapshot, live, callback, "
                          "wire, and stale-state borders: callbacks=" +
                              traceText(expandedOracle.callbacks) + "/" + traceText(expandedPublic.callbacks));

        const auto closedOracle = tests::codex::oracle::captureLegacyFrontendClient(expandedMessages, true, false, true);
        const auto closedPublic = capturePublicFrontendClient(expandedMessages, true, false, true);
        result.expectTrue(!closedOracle.ready && !closedPublic.ready && closedOracle.states == closedPublic.states &&
                              closedOracle.callbacks == closedPublic.callbacks && closedOracle.outbound == closedPublic.outbound &&
                              closedOracle.closes == 1 && closedPublic.closes == 1,
                          "a local Connection close preserves the old transport-close, stale-State, and Disconnected callback border "
                          "without a synthetic Closing transition: callbacks=" +
                              traceText(closedOracle.callbacks) + "/" + traceText(closedPublic.callbacks));

        frontend::Json legacyState{
            {"backendRevision", std::uint64_t{7}},
            {"lifecycle", "ready"},
            {"diagnostics", {{"received", std::uint64_t{0}}, {"recent", frontend::Json::array()}}},
            {"sessions", frontend::Json::array()},
            {"threadList", {{"hasLoadedPage", false}, {"complete", true}, {"pagesLoaded", std::uint64_t{0}}}},
            {"threads",
             frontend::Json::array(
                 {{{"id", "legacy-thread"},
                   {"name", "legacy name"},
                   {"fullyLoaded", true},
                   {"extensions", {{"threadNested", true}}},
                   {"turns",
                    frontend::Json::array({{{"id", "legacy-turn"},
                                            {"threadId", "legacy-thread"},
                                            {"status", "completed"},
                                            {"active", false},
                                            {"terminal", true},
                                            {"extensions", {{"turnNested", true}}},
                                            {"items",
                                             frontend::Json::array({{{"id", "legacy-future-item"},
                                                                     {"type", "future_item"},
                                                                     {"threadId", "legacy-thread"},
                                                                     {"turnId", "legacy-turn"},
                                                                     {"summary", "future summary"},
                                                                     {"location", {{"path", "/tmp/future"}}},
                                                                     {"status", "completed"},
                                                                     {"agentText", ""},
                                                                     {"reasoningText", ""},
                                                                     {"reasoningSummary", ""},
                                                                     {"commandOutput", ""},
                                                                     {"droppedContentBytes", std::uint64_t{0}},
                                                                     {"contentTruncated", false},
                                                                     {"data", frontend::Json::object()},
                                                                     {"truncated", true},
                                                                     {"omittedFields", frontend::Json::array({"/future"})},
                                                                     {"extensions", {{"nestedExtension", true}}},
                                                                     {"futureField", "retained"}}})}}})}}})},
            {"pendingRequests", frontend::Json::array()},
            {"codexExtensions", frontend::Json::array()},
            {"omittedCodexExtensions", std::uint64_t{0}},
            {"journal", {{"oldestReplayableAfter", std::uint64_t{0}}, {"currentSequence", std::uint64_t{1}}}},
            {"sequenceExhausted", false},
            {"knownFutureExtension", {{"safe", true}}},
            {"authorization", "removed"},
            {"nested", {{"accessToken", "removed"}, {"commandOutput", "token-shaped sentinel retained"}}}};
        const std::vector<frontend::ServerMessage> legacyMessages{
            legacyWelcome(1),
            frontend::Snapshot{frontend::SequenceNumber(1), std::move(legacyState)},
            frontend::SyncComplete{frontend::SequenceNumber(1)},
        };
        const auto legacyOracle = tests::codex::oracle::captureLegacyFrontendClient(legacyMessages, false, false);
        const auto legacyPublic = capturePublicFrontendClient(legacyMessages, false, false);
        const frontend::Json& publicReadyState = legacyPublic.states.back();
        const auto futureItem = std::ranges::find_if(publicReadyState.at("items"), [](const frontend::Json& item) {
            return item.value("id", std::string{}) == "legacy-future-item";
        });
        const bool futureItemExtensionsRetained = futureItem != publicReadyState.at("items").end() &&
                                                  futureItem->value("nestedExtension", false) &&
                                                  futureItem->value("futureField", std::string{}) == "retained";
        result.expectTrue(
            legacyOracle.accepted && legacyPublic.accepted && legacyOracle.ready && legacyPublic.ready &&
                legacyOracle.states == legacyPublic.states && legacyOracle.callbacks == legacyPublic.callbacks &&
                legacyOracle.outbound == legacyPublic.outbound &&
                publicReadyState.at("compatibilityExtensions").contains("knownFutureExtension") &&
                publicReadyState.at("compatibilityExtensions").at("nested").contains("commandOutput") &&
                !publicReadyState.at("compatibilityExtensions").contains("authorization") &&
                !publicReadyState.at("compatibilityExtensions").at("nested").contains("accessToken") &&
                publicReadyState.at("threads").front().at("title") == "legacy name" &&
                publicReadyState.at("turns").front().at("orderedItems").front() == "legacy-future-item" && futureItemExtensionsRetained,
            "the cut-over public Client matches the preserved old client at the sanitized legacy-extension border: accepted=" +
                std::to_string(legacyOracle.accepted) + "/" + std::to_string(legacyPublic.accepted) +
                " ready=" + std::to_string(legacyOracle.ready) + "/" + std::to_string(legacyPublic.ready) +
                " callbacks=" + traceText(legacyOracle.callbacks) + "/" + traceText(legacyPublic.callbacks) +
                " oracle=" + (legacyOracle.states.empty() ? std::string{"<none>"} : legacyOracle.states.back().dump()) +
                " public=" + (legacyPublic.states.empty() ? std::string{"<none>"} : legacyPublic.states.back().dump()) +
                " diagnostics=" + traceText(legacyOracle.diagnostics) + "/" + traceText(legacyPublic.diagnostics));
    }

    void testLegacyStateOptionalityAndExtensionBorders(tests::support::TestResult& result) {
        const LegacyStateDifferential absent = captureLegacyStateDifferential(minimalLegacyState());
        const frontend::Json& absentReady = absent.publicClient.states.back();
        result.expectTrue(
            exactLegacyStateParity(absent) && !absentReady.contains("capacity") && !absentReady.contains("accounts") &&
                !absentReady.contains("processes") && !absentReady.contains("filesystemWatches") &&
                !absentReady.contains("fuzzySearches") && absentReady.value("threadProjectionPresent", false) &&
                absentReady.value("turnProjectionPresent", false) && absentReady.value("itemProjectionPresent", false) &&
                absentReady.value("pendingRequestProjectionPresent", false) && absentReady.contains("truncation") &&
                !absentReady.at("truncation").at("value").contains("droppedBytes"),
            "the direct legacy conversion preserves missing root domains, absent capacity, and omitted droppedBytes exactly: " +
                legacyStateDifference(absent));

        for (std::string_view requiredRoot : std::array{"sessions", "threadList", "threads", "pendingRequests"}) {
            frontend::Json missingRequiredState = minimalLegacyState();
            missingRequiredState.erase(std::string(requiredRoot));
            const LegacyStateDifferential missingRequired = captureLegacyStateDifferential(std::move(missingRequiredState));
            result.expectTrue(!missingRequired.oracle.accepted && !missingRequired.publicClient.accepted &&
                                  missingRequired.oracle.ready == missingRequired.publicClient.ready &&
                                  missingRequired.oracle.states == missingRequired.publicClient.states &&
                                  missingRequired.oracle.callbacks == missingRequired.publicClient.callbacks &&
                                  missingRequired.oracle.outbound == missingRequired.publicClient.outbound &&
                                  missingRequired.oracle.closes == missingRequired.publicClient.closes,
                              "the typed legacy decoder retains the frozen rejection of a missing required " + std::string(requiredRoot) +
                                  " root: " + legacyStateDifference(missingRequired) +
                                  " diagnostics=" + traceText(missingRequired.oracle.diagnostics) + "/" +
                                  traceText(missingRequired.publicClient.diagnostics));
        }

        frontend::Json emptyCapacityState = minimalLegacyState();
        emptyCapacityState["capacity"] = frontend::Json::object();
        const LegacyStateDifferential emptyCapacity = captureLegacyStateDifferential(std::move(emptyCapacityState));
        const frontend::Json& emptyCapacityReady = emptyCapacity.publicClient.states.back();
        result.expectTrue(exactLegacyStateParity(emptyCapacity) && emptyCapacityReady.contains("capacity") &&
                              emptyCapacityReady.at("capacity").at("value").empty(),
                          "an explicitly empty legacy capacity object remains present and differs from an absent capacity root: " +
                              legacyStateDifference(emptyCapacity));

        frontend::Json explicitState = minimalLegacyState();
        explicitState["capacity"] = frontend::Json{{"vendorCapacity", {{"safe", true}}}};
        explicitState["processes"] = frontend::Json{
            {"entries", frontend::Json::array()},
            {"truncation", {{"truncated", false}, {"omittedFields", frontend::Json::array()}, {"droppedBytes", std::uint64_t{0}}}}};
        const LegacyStateDifferential explicitValues = captureLegacyStateDifferential(std::move(explicitState));
        const frontend::Json& explicitReady = explicitValues.publicClient.states.back();
        const frontend::Json& explicitCapacity = explicitReady.at("capacity").at("value");
        result.expectTrue(
            exactLegacyStateParity(explicitValues) && explicitReady.contains("capacity") && explicitCapacity.contains("vendorCapacity") &&
                explicitCapacity.at("vendorCapacity").value("safe", false) &&
                explicitReady.at("processes").at("value").at("truncation").contains("droppedBytes") &&
                explicitReady.at("processes").at("value").at("truncation").at("droppedBytes") == 0,
            "explicit empty-compatible capacity extensions and explicit droppedBytes zero remain distinguishable from omission: " +
                legacyStateDifference(explicitValues));

        frontend::Json collectionsState = minimalLegacyState();
        collectionsState["processes"] = frontend::Json{
            {"entries",
             frontend::Json::array({{{"processHandle", "process-1"},
                                     {"lifecycle", "exited"},
                                     {"stdout", "out"},
                                     {"stdoutTruncated", false},
                                     {"stderrTruncated", false},
                                     {"stamp", {{"generation", std::uint64_t{3}}, {"freshness", "stale"}, {"stampVendor", "process"}}},
                                     {"connectionInvalidated", true},
                                     {"stateUnavailable", true},
                                     {"processVendor", {{"safe", true}}}}})},
            {"truncation", {{"truncated", false}, {"omittedFields", frontend::Json::array()}}},
            {"processCollectionVendor", true}};
        collectionsState["filesystemWatches"] = frontend::Json{
            {"entries",
             frontend::Json::array({{{"watchId", "watch-1"},
                                     {"root", "/tmp/watch"},
                                     {"changedPathCount", std::uint64_t{2}},
                                     {"stamp", {{"generation", std::uint64_t{4}}, {"freshness", "current"}, {"stampVendor", "watch"}}},
                                     {"connectionInvalidated", false},
                                     {"stateUnavailable", true},
                                     {"watchVendor", {{"safe", true}}}}})},
            {"truncation", {{"truncated", false}, {"omittedFields", frontend::Json::array()}}},
            {"watchCollectionVendor", true}};
        collectionsState["fuzzySearchSessions"] = frontend::Json{
            {"entries",
             frontend::Json::array({{{"sessionId", "search-1"},
                                     {"resultCount", std::uint64_t{5}},
                                     {"complete", true},
                                     {"stamp", {{"generation", std::uint64_t{5}}, {"freshness", "current"}, {"stampVendor", "search"}}},
                                     {"connectionInvalidated", true},
                                     {"stateUnavailable", true},
                                     {"searchVendor", {{"safe", true}}}}})},
            {"truncation", {{"truncated", false}, {"omittedFields", frontend::Json::array()}}},
            {"searchCollectionVendor", true}};
        const LegacyStateDifferential collections = captureLegacyStateDifferential(std::move(collectionsState));
        const frontend::Json& collectionsReady = collections.publicClient.states.back();
        const frontend::Json& process = collectionsReady.at("processes").at("value").at("entries").front();
        const frontend::Json& watch = collectionsReady.at("filesystemWatches").at("value").at("entries").front();
        const frontend::Json& search = collectionsReady.at("fuzzySearches").at("value").at("entries").front();
        result.expectTrue(
            exactLegacyStateParity(collections) && process.value("stateUnavailable", false) &&
                process.at("processVendor").value("safe", false) && process.at("stamp").value("stampVendor", "") == "process" &&
                watch.value("stateUnavailable", false) && watch.at("watchVendor").value("safe", false) &&
                watch.at("stamp").value("stampVendor", "") == "watch" && search.value("stateUnavailable", false) &&
                search.at("searchVendor").value("safe", false) && search.at("stamp").value("stampVendor", "") == "search" &&
                collectionsReady.at("fuzzySearches").at("value").value("searchCollectionVendor", false),
            "legacy process/watch/search entry extensions, stateUnavailable, stamp extensions, and the fuzzySearchSessions alias "
            "survive the typed bridge exactly: " +
                legacyStateDifference(collections));
    }

    void testLegacyMixedItemOrdering(tests::support::TestResult& result) {
        frontend::Json state = minimalLegacyState();
        state["threads"] = frontend::Json::array(
            {{{"id", "thread-a"},
              {"fullyLoaded", true},
              {"turns",
               frontend::Json::array({{{"id", "turn-a"},
                                       {"threadId", "thread-a"},
                                       {"status", "completed"},
                                       {"active", false},
                                       {"terminal", true},
                                       {"items", frontend::Json::array({knownLegacyItem("known-a"), futureLegacyItem("future-a")})},
                                       {"extensions", frontend::Json::object()}}})},
              {"extensions", frontend::Json::object()}},
             {{"id", "thread-b"},
              {"fullyLoaded", true},
              {"turns",
               frontend::Json::array({{{"id", "turn-b"},
                                       {"threadId", "thread-b"},
                                       {"status", "completed"},
                                       {"active", false},
                                       {"terminal", true},
                                       {"items", frontend::Json::array({futureLegacyItem("future-b"), knownLegacyItem("known-b")})},
                                       {"extensions", frontend::Json::object()}}})},
              {"extensions", frontend::Json::object()}}});
        const LegacyStateDifferential capture = captureLegacyStateDifferential(std::move(state));
        const frontend::Json& ready = capture.publicClient.states.back();
        const std::vector<std::string> globalOrder = [&] {
            std::vector<std::string> ids;
            for (const frontend::Json& item : ready.at("items")) {
                ids.push_back(item.value("id", std::string{}));
            }
            return ids;
        }();
        const std::vector<std::string> firstTurnOrder = ready.at("turns").at(0).at("orderedItems").get<std::vector<std::string>>();
        const std::vector<std::string> secondTurnOrder = ready.at("turns").at(1).at("orderedItems").get<std::vector<std::string>>();
        result.expectTrue(
            exactLegacyStateParity(capture) && globalOrder == std::vector<std::string>{"known-a", "future-a", "future-b", "known-b"} &&
                firstTurnOrder == std::vector<std::string>{"known-a", "future-a"} &&
                secondTurnOrder == std::vector<std::string>{"future-b", "known-b"},
            "known and future legacy items retain global and per-turn order across multiple turns: " + legacyStateDifference(capture));
    }

    void testLegacySessionCompatibilityAndUnknownPendingRejection(tests::support::TestResult& result) {
        frontend::Json controllerState = minimalLegacyState();
        controllerState["controller"] = {{"present", true}};
        const LegacyStateDifferential controllerCapture = captureLegacyStateDifferential(std::move(controllerState));
        const frontend::Json& publicController = controllerCapture.publicClient.states.back().at("controller").at("value");
        result.expectTrue(exactLegacyStateParity(controllerCapture) && publicController.value("present", false) &&
                              !publicController.contains("controllerSessionId"),
                          "an explicit controller-presence fact without an owner survives the typed bridge exactly: " +
                              legacyStateDifference(controllerCapture));

        frontend::Json sessionState = minimalLegacyState();
        sessionState["sessions"] =
            frontend::Json::array({{{"sessionId", "8"}, {"role", "observer"}, {"principalId", "principal-8"}, {"freshness", "stale"}}});
        const LegacyStateDifferential sessionCapture = captureLegacyStateDifferential(std::move(sessionState));
        const frontend::Json& publicReady = sessionCapture.publicClient.states.back();
        const frontend::Json& publicSession = publicReady.at("sessions").at("value").front();
        result.expectTrue(exactLegacyStateParity(sessionCapture) && publicSession.value("principalId", "") == "principal-8" &&
                              publicSession.value("freshness", "") == "stale",
                          "the direct builder preserves additive legacy session principalId/freshness extensions exactly: " +
                              legacyStateDifference(sessionCapture));

        frontend::Json duplicateSessionState = minimalLegacyState();
        duplicateSessionState["sessions"] =
            frontend::Json::array({{{"sessionId", "8"}, {"role", "observer"}}, {{"sessionId", "8"}, {"role", "controller"}}});
        const LegacyStateDifferential duplicateSessions = captureLegacyStateDifferential(std::move(duplicateSessionState));
        result.expectTrue(!duplicateSessions.oracle.accepted && !duplicateSessions.publicClient.accepted &&
                              duplicateSessions.oracle.states == duplicateSessions.publicClient.states &&
                              duplicateSessions.oracle.closes == 1 && duplicateSessions.publicClient.closes == 1,
                          "duplicate lookup identities retain the old public-client transactional rejection border: " +
                              legacyStateDifference(duplicateSessions));

        frontend::Json unknownPendingState = minimalLegacyState();
        unknownPendingState["pendingRequests"] =
            frontend::Json::array({{{"id", "72"},
                                    {"type", "future_server_request"},
                                    {"details", {{"method", "future/serverRequest"}, {"sensitiveFieldsRedacted", true}}}}});
        const LegacyStateDifferential unknownPending = captureLegacyStateDifferential(std::move(unknownPendingState));
        result.expectTrue(
            !unknownPending.oracle.accepted && !unknownPending.publicClient.accepted && !unknownPending.oracle.ready &&
                !unknownPending.publicClient.ready && unknownPending.oracle.closes == 1 && unknownPending.publicClient.closes == 1 &&
                unknownPending.oracle.states == unknownPending.publicClient.states,
            "an unrepresentable future legacy pending-request kind rejects transactionally instead of being dropped or coerced: " +
                legacyStateDifference(unknownPending) + " diagnostics=" + traceText(unknownPending.oracle.diagnostics) + "/" +
                traceText(unknownPending.publicClient.diagnostics));
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testDirectCanonicalStateBuilder(result);
    testCanonicalLookupIdentityPreflight(result);
    testHybridExpandedPublicationRetainsLegacyItems(result);
    testPublicClientCoreAdapter(result);
    testTransactionalPreparationFailure(result);
    testTransactionalStaleFallback(result);
    testPublicClientLegacyOracleDifferential(result);
    testLegacyStateOptionalityAndExtensionBorders(result);
    testLegacyMixedItemOrdering(result);
    testLegacySessionCompatibilityAndUnknownPendingRejection(result);
    return result.processResult();
}
