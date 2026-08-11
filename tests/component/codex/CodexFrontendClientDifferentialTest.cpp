/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexFrontendDifferentialAuthority.h"
#include "CodexFrontendDifferentialComparison.h"
#include "CodexFrontendDifferentialExecutionLedger.h"
#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Security.h"
#include "ai/openai/codex/frontend/internal/client/ClientCore.h"
#include "ai/openai/codex/frontend/internal/server/BackendProjection.h"
#include "oracle/LegacyFrontendClient.h"
#include "support/TestResult.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef AISUITE_CODEX_FRONTEND_DIFFERENTIAL_FIXTURE
#error "AISUITE_CODEX_FRONTEND_DIFFERENTIAL_FIXTURE is required"
#endif
#ifndef AISUITE_CODEX_FRONTEND_DIFFERENTIAL_COVERAGE_FIXTURE
#error "AISUITE_CODEX_FRONTEND_DIFFERENTIAL_COVERAGE_FIXTURE is required"
#endif
#ifndef AISUITE_CODEX_FRONTEND_CLIENT_DIFFERENTIAL_LEDGER
#error "AISUITE_CODEX_FRONTEND_CLIENT_DIFFERENTIAL_LEDGER is required"
#endif

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace oracle = ai::openai::codex::frontend::client;
    namespace permanent = ai::openai::codex::frontend::internal::client;
    namespace backend = ai::openai::codex::backend;
    namespace server = ai::openai::codex::frontend::internal::server;

    struct CorpusMethod {
        const generated::MethodMetadata* metadata = nullptr;
        std::string coverageId;
        frontend::Json parameters = frontend::Json::object();
        frontend::Json result = frontend::Json::object();
    };

    frontend::Json loadFixture() {
        std::ifstream input(AISUITE_CODEX_FRONTEND_DIFFERENTIAL_FIXTURE);
        if (!input) {
            throw std::runtime_error("cannot open generated frontend differential fixture");
        }
        return frontend::Json::parse(input);
    }

    frontend::Json loadCoverageFixture() {
        std::ifstream input(AISUITE_CODEX_FRONTEND_DIFFERENTIAL_COVERAGE_FIXTURE);
        if (!input) {
            throw std::runtime_error("cannot open authority-derived frontend coverage fixture");
        }
        return frontend::Json::parse(input);
    }

    std::vector<CorpusMethod> methodCorpus(const frontend::Json& fixture, const frontend::Json& coverage) {
        std::vector<CorpusMethod> result;
        result.reserve(generated::AllMethods.size());
        for (const generated::MethodMetadata& metadata : generated::AllMethods) {
            const auto row = std::find_if(fixture.at("methods").begin(), fixture.at("methods").end(), [&](const frontend::Json& value) {
                return value.at("method").get_ref<const std::string&>() == metadata.method;
            });
            if (row == fixture.at("methods").end()) {
                throw std::runtime_error("generated differential fixture omits method " + std::string(metadata.method));
            }
            const auto coverageRow =
                std::find_if(coverage.at("methods").begin(), coverage.at("methods").end(), [&](const frontend::Json& value) {
                    return value.at("method").get_ref<const std::string&>() == metadata.method;
                });
            if (coverageRow == coverage.at("methods").end()) {
                throw std::runtime_error("authority coverage fixture omits method " + std::string(metadata.method));
            }
            result.push_back({&metadata, coverageRow->at("id").get<std::string>(), row->at("minimalParams"), row->at("minimalResult")});
        }
        return result;
    }

    std::string coverageCase(std::string_view family, std::string_view identity, std::string_view dimension) {
        return std::string(family) + ":" + std::string(identity) + ":" + std::string(dimension);
    }

    std::string traceText(const std::vector<std::string>& trace) {
        std::string joined;
        for (const std::string& entry : trace) {
            if (!joined.empty()) {
                joined += ',';
            }
            joined += entry;
        }
        return joined;
    }

    bool normalizedStateParity(const oracle::State& oldState,
                               const std::shared_ptr<const permanent::PublishedState>& newState,
                               std::string& mismatch) {
        if (!newState) {
            mismatch = "/ old=state new=<missing>";
            return false;
        }
        const frontend::Json oldProjection = oracle::detail::StateReducer::serializeForTesting(oldState);
        const frontend::Json newProjection = newState->serializeForTesting();
        const auto difference = tests::codex::differential::firstMismatch(oldProjection, newProjection, "");
        if (!difference) {
            return true;
        }
        mismatch = difference->path + " old=" + difference->oldValue + " new=" + difference->newValue;
        return false;
    }

    std::vector<frontend::FrontendCapability> definedCapabilities() {
        std::vector<frontend::FrontendCapability> result;
        for (const generated::CapabilityMetadata& metadata : generated::AllCapabilities) {
            if (metadata.defined) {
                result.push_back(static_cast<frontend::FrontendCapability>(metadata.id));
            }
        }
        return result;
    }

    std::vector<frontend::FrontendMethod> allMethods() {
        std::vector<frontend::FrontendMethod> result;
        result.reserve(generated::AllMethods.size());
        for (const generated::MethodMetadata& metadata : generated::AllMethods) {
            result.emplace_back(metadata.method);
        }
        return result;
    }

    frontend::Json continuityMetadata() {
        frontend::Json scopes = frontend::Json::array();
        for (frontend::FrontendScope scope : frontend::LocalTrustedScopes) {
            scopes.push_back(frontend::toString(scope));
        }
        return {{"permittedScopes", std::move(scopes)}};
    }

    frontend::Json emptyExpandedState() {
        return {{"provider",
                 {{"lifecycle", "ready"},
                  {"generation", std::uint64_t{1}},
                  {"desiredRunning", true},
                  {"recovery", {{"status", "idle"}, {"attempts", std::uint64_t{0}}}}}},
                {"controller", frontend::Json::object()},
                {"sessions", frontend::Json::array()},
                {"threadList", {{"hasLoadedPage", false}, {"complete", true}, {"pagesLoaded", std::uint64_t{0}}}},
                {"threads", frontend::Json::array()},
                {"turns", frontend::Json::array()},
                {"items", frontend::Json::array()},
                {"pendingRequests", frontend::Json::array()},
                {"capacity", frontend::Json::object()},
                {"truncation", {{"truncated", false}}}};
    }

    frontend::Json emptyLegacyState() {
        return {{"backendRevision", std::uint64_t{1}},
                {"lifecycle", "ready"},
                {"diagnostics", {{"received", std::uint64_t{0}}, {"recent", frontend::Json::array()}}},
                {"sessions", frontend::Json::array()},
                {"threadList", {{"hasLoadedPage", false}, {"complete", true}, {"pagesLoaded", std::uint64_t{0}}}},
                {"threads", frontend::Json::array()},
                {"pendingRequests", frontend::Json::array()},
                {"codexExtensions", frontend::Json::array()},
                {"omittedCodexExtensions", std::uint64_t{0}},
                {"journal", {{"oldestReplayableAfter", std::uint64_t{0}}, {"currentSequence", std::uint64_t{7}}}},
                {"sequenceExhausted", false}};
    }

    frontend::CapabilityAdvertisement advertisement(bool) {
        const std::vector<frontend::FrontendCapability> capabilities = definedCapabilities();
        return {capabilities, capabilities, capabilities, frontend::Json::object()};
    }

    struct OracleHarness {
        std::vector<oracle::OutboundMessage> outbound;
        std::vector<oracle::ConnectionState> states;
        std::vector<oracle::ConnectionStateChange> stateChanges;
        std::vector<frontend::ServerMessage> observed;
        std::vector<oracle::GeneratedOperationResult> completions;
        std::vector<std::string> trace;
        std::function<void()> stateUpdateHook;
        std::size_t closes = 0;

        oracle::TransportCallbacks transport() {
            return {[this](oracle::OutboundMessage message) {
                        outbound.push_back(std::move(message));
                        return oracle::SendResult{oracle::SendStatus::Accepted, std::nullopt};
                    },
                    [this](std::string) {
                        ++closes;
                    }};
        }

        oracle::ClientCallbacks callbacks() {
            oracle::ClientCallbacks value;
            value.onConnectionStateChanged = [this](const oracle::ConnectionStateChange& change) {
                states.push_back(change.current);
                stateChanges.push_back(change);
                trace.emplace_back("connection");
            };
            value.onStateUpdated = [this](const oracle::StateUpdate&) {
                trace.emplace_back("state");
                if (stateUpdateHook) {
                    stateUpdateHook();
                }
            };
            value.onSynchronized = [this](const oracle::SynchronizationInfo&) {
                trace.emplace_back("synchronized");
            };
            value.onCursorAdvanced = [this](frontend::SequenceNumber) {
                trace.emplace_back("cursor");
            };
            value.onProtocolMessage = [this](const frontend::ServerMessage& message) {
                observed.push_back(message);
                trace.emplace_back("protocol");
            };
            return value;
        }
    };

    struct PermanentHarness {
        std::vector<permanent::OutboundMessage> outbound;
        std::vector<permanent::ConnectionState> states;
        std::vector<permanent::StateChange> stateChanges;
        std::vector<frontend::ServerMessage> observed;
        std::vector<permanent::OperationResult> completions;
        std::vector<std::string> trace;
        std::function<void()> stateUpdateHook;
        std::size_t closes = 0;

        permanent::TransportCallbacks transport() {
            return {[this](permanent::OutboundMessage message) {
                        outbound.push_back(std::move(message));
                        return permanent::SendResult{permanent::SendStatus::Accepted, std::nullopt};
                    },
                    [this](std::string_view) {
                        ++closes;
                    }};
        }

        permanent::ClientCallbacks callbacks() {
            permanent::ClientCallbacks value;
            value.onConnectionStateChanged = [this](const permanent::StateChange& change) {
                states.push_back(change.current);
                stateChanges.push_back(change);
                trace.emplace_back("connection");
            };
            value.onStateUpdated = [this](const permanent::StateUpdate&) {
                trace.emplace_back("state");
                if (stateUpdateHook) {
                    stateUpdateHook();
                }
            };
            value.onCursorAdvanced = [this](permanent::model::FrontendSequence) {
                trace.emplace_back("cursor");
            };
            value.onSynchronized = [this](const permanent::SynchronizationInfo&) {
                trace.emplace_back("synchronized");
            };
            value.onProtocolMessage = [this](const frontend::ServerMessage& message) {
                observed.push_back(message);
                trace.emplace_back("protocol");
            };
            return value;
        }
    };

    struct Pair {
        OracleHarness oracleHarness;
        PermanentHarness permanentHarness;
        oracle::Client oracleClient;
        permanent::ClientCore permanentClient;
        oracle::Connection oracleConnection;
        permanent::PhysicalGeneration generation = 0;
        bool expanded = true;

        explicit Pair(bool useExpanded)
            : oracleClient(oracleOptions(useExpanded), oracleHarness.callbacks())
            , permanentClient(permanentOptions(useExpanded), permanentHarness.callbacks())
            , oracleConnection(oracleClient.openConnection(oracleHarness.transport()))
            , expanded(useExpanded) {
            const std::optional<permanent::PhysicalGeneration> attached = permanentClient.attach(permanentHarness.transport());
            if (!attached.has_value()) {
                throw std::runtime_error("permanent differential client refused its first attachment");
            }
            generation = *attached;
            oracleConnection.transportConnected();
            permanentClient.transportConnected(generation);
        }

        static oracle::ClientOptions oracleOptions(bool expanded) {
            oracle::ClientOptions value;
            value.maximumPendingOperations = generated::AllMethods.size() + 16;
            value.credentialProvider = [] {
                return oracle::AuthenticationContext{frontend::NoCredential{}, "p2-differential-continuity"};
            };
            if (!expanded) {
                value.requestedCapabilities.clear();
            }
            return value;
        }

        static permanent::ClientOptions permanentOptions(bool expanded) {
            permanent::ClientOptions value;
            value.limits.maximumPendingOperations = generated::AllMethods.size() + 16;
            value.credentialProvider = [] {
                return permanent::AuthenticationContext{frontend::NoCredential{}, "p2-differential-continuity"};
            };
            if (!expanded) {
                value.requestedCapabilities.clear();
            }
            return value;
        }

        void makeReadyWithSnapshot(frontend::Snapshot snapshot,
                                   frontend::SessionRole role = frontend::SessionRole::Observer,
                                   std::string sessionId = "1") {
            const std::vector<frontend::FrontendMethod> methods = allMethods();
            const frontend::SequenceNumber current = snapshot.sequence;
            const frontend::Welcome welcome{std::move(sessionId),
                                            role,
                                            current,
                                            frontend::SyncMode::Snapshot,
                                            continuityMetadata(),
                                            advertisement(expanded),
                                            methods,
                                            methods,
                                            "p2-differential"};
            const frontend::SyncComplete complete{current};
            const bool oldWelcome = oracleConnection.receive(frontend::ServerMessage{welcome}).accepted;
            const bool newWelcome = permanentClient.receive(generation, frontend::ServerMessage{welcome});
            const bool oldSnapshot = oldWelcome && oracleConnection.receive(frontend::ServerMessage{snapshot}).accepted;
            const bool newSnapshot = newWelcome && permanentClient.receive(generation, frontend::ServerMessage{snapshot});
            const bool oldComplete = oldSnapshot && oracleConnection.receive(frontend::ServerMessage{complete}).accepted;
            const bool newComplete = newSnapshot && permanentClient.receive(generation, frontend::ServerMessage{complete});
            if (!oldWelcome || !newWelcome || !oldSnapshot || !newSnapshot || !oldComplete || !newComplete) {
                throw std::runtime_error("client differential handshake did not reach Ready: welcome=" + std::to_string(oldWelcome) + "/" +
                                         std::to_string(newWelcome) + " snapshot=" + std::to_string(oldSnapshot) + "/" +
                                         std::to_string(newSnapshot) + " complete=" + std::to_string(oldComplete) + "/" +
                                         std::to_string(newComplete));
            }
        }

        void makeReady(bool expanded) {
            makeReadyWithSnapshot(frontend::Snapshot{frontend::SequenceNumber{7}, expanded ? emptyExpandedState() : emptyLegacyState()});
        }

        void attachNext() {
            oracleConnection = oracleClient.openConnection(oracleHarness.transport());
            const std::optional<permanent::PhysicalGeneration> attached = permanentClient.attach(permanentHarness.transport());
            if (!attached.has_value()) {
                throw std::runtime_error("permanent differential client refused a later attachment");
            }
            generation = *attached;
            oracleConnection.transportConnected();
            permanentClient.transportConnected(generation);
        }

        void makeReadyWithReplay(frontend::SequenceNumber current = frontend::SequenceNumber{7},
                                 frontend::SessionRole role = frontend::SessionRole::Observer,
                                 std::string sessionId = "2") {
            const std::vector<frontend::FrontendMethod> methods = allMethods();
            const frontend::Welcome welcome{std::move(sessionId),
                                            role,
                                            current,
                                            frontend::SyncMode::Replay,
                                            continuityMetadata(),
                                            advertisement(expanded),
                                            methods,
                                            methods,
                                            "p2-differential"};
            const bool oldWelcome = oracleConnection.receive(frontend::ServerMessage{welcome}).accepted;
            const bool newWelcome = permanentClient.receive(generation, frontend::ServerMessage{welcome});
            const frontend::SyncComplete complete{current};
            const bool oldComplete = oldWelcome && oracleConnection.receive(frontend::ServerMessage{complete}).accepted;
            const bool newComplete = newWelcome && permanentClient.receive(generation, frontend::ServerMessage{complete});
            if (!oldWelcome || !newWelcome || !oldComplete || !newComplete) {
                throw std::runtime_error("client differential reconnect replay did not reach Ready");
            }
        }
    };

    const frontend::Json& fixtureForMethod(const frontend::Json& fixture, generated::MethodId method) {
        const std::string_view identity = generated::methodString(method);
        const auto found = std::find_if(fixture.at("methods").begin(), fixture.at("methods").end(), [identity](const frontend::Json& row) {
            return row.at("method").get_ref<const std::string&>() == identity;
        });
        if (found == fixture.at("methods").end()) {
            throw std::runtime_error("generated client differential fixture omits method " + std::string(identity));
        }
        return *found;
    }

    bool sameClientError(const oracle::Error& oldError, const permanent::ClientError& newError) {
        const bool clientCode =
            oldError.clientCode.has_value() == newError.clientCode.has_value() &&
            (!oldError.clientCode || static_cast<unsigned>(*oldError.clientCode) == static_cast<unsigned>(*newError.clientCode));
        return static_cast<unsigned>(oldError.origin) == static_cast<unsigned>(newError.origin) && clientCode &&
               oldError.protocolCode == newError.protocolCode && oldError.message == newError.message && !oldError.remoteCode.has_value() &&
               oldError.details == newError.remoteDetails && oldError.retryable == newError.retryable;
    }

    const oracle::Error* latestConnectionError(const OracleHarness& harness) {
        for (auto change = harness.stateChanges.rbegin(); change != harness.stateChanges.rend(); ++change) {
            if (change->error.has_value()) {
                return &*change->error;
            }
        }
        return nullptr;
    }

    const permanent::ClientError* latestConnectionError(const PermanentHarness& harness) {
        for (auto change = harness.stateChanges.rbegin(); change != harness.stateChanges.rend(); ++change) {
            if (change->error.has_value()) {
                return &*change->error;
            }
        }
        return nullptr;
    }

    bool singleHelloParity(const Pair& pair) {
        if (pair.oracleHarness.outbound.size() != 1 || pair.permanentHarness.outbound.size() != 1) {
            return false;
        }
        const auto oldDecoded = frontend::Codec::decodeClient(std::string_view(pair.oracleHarness.outbound.front().compactJson));
        const auto* oldHello = oldDecoded ? std::get_if<frontend::Hello>(&oldDecoded.value()) : nullptr;
        const auto* newHello = std::get_if<frontend::Hello>(&pair.permanentHarness.outbound.front().value);
        return oldHello != nullptr && newHello != nullptr && *oldHello == *newHello &&
               pair.oracleHarness.outbound.front().sensitive == pair.permanentHarness.outbound.front().sensitive;
    }

    bool completeStateParity(const Pair& pair, std::string& mismatch) {
        return normalizedStateParity(pair.oracleClient.state(), pair.permanentClient.state(), mismatch);
    }

    const generated::DefinedCommand& permanentCommand(const permanent::OutboundMessage& message) {
        const auto* command = std::get_if<generated::DefinedCommand>(&message.value);
        if (command == nullptr) {
            throw std::runtime_error("permanent client emitted Hello where command was expected");
        }
        return *command;
    }

    void appendCommandAttempts(const OracleHarness& harness, std::uint64_t generation, frontend::Json& attempts) {
        for (const oracle::OutboundMessage& outbound : harness.outbound) {
            const auto decoded = frontend::Codec::decodeDefinedCommand(std::string_view(outbound.compactJson));
            if (decoded) {
                const generated::MethodId method = generated::commandMethod(decoded.value().parameters);
                const bool reverseResponse =
                    generated::AllMethods[static_cast<std::size_t>(method)].category == generated::MethodCategory::ReverseResponse;
                attempts.push_back({{"requestId", decoded.value().requestId},
                                    {"physicalGeneration", generation},
                                    {"method", std::string(generated::methodString(method))},
                                    {"kind", reverseResponse ? "reverse-response" : "command"}});
            }
        }
    }

    void appendCommandAttempts(const PermanentHarness& harness, permanent::PhysicalGeneration generation, frontend::Json& attempts) {
        for (const permanent::OutboundMessage& outbound : harness.outbound) {
            if (const auto* command = std::get_if<generated::DefinedCommand>(&outbound.value)) {
                const generated::MethodId method = generated::commandMethod(command->parameters);
                const bool reverseResponse =
                    generated::AllMethods[static_cast<std::size_t>(method)].category == generated::MethodCategory::ReverseResponse;
                attempts.push_back({{"requestId", command->requestId},
                                    {"physicalGeneration", generation},
                                    {"method", std::string(generated::methodString(method))},
                                    {"kind", reverseResponse ? "reverse-response" : "command"}});
            }
        }
    }

    frontend::Json lifecycleBorderObservation(std::uint64_t generation,
                                              frontend::Json commandAttempts,
                                              frontend::Json controllerOwnership,
                                              frontend::Json deliveredCallbacks) {
        return {{"lifecycle",
                 {{"physicalGeneration", generation},
                  {"commandAttempts", std::move(commandAttempts)},
                  {"controllerOwnedByGeneration", std::move(controllerOwnership)}}},
                {"queue", {{"maximumMessages", std::uint64_t{0}}, {"retainedMessages", std::uint64_t{0}}}},
                {"callbacks", {{"physicalGeneration", generation}, {"delivered", std::move(deliveredCallbacks)}}}};
    }

    void compareHello(tests::support::TestResult& result,
                      tests::codex::FrontendDifferentialExecutionLedger& ledger,
                      const Pair& pair,
                      std::string_view representation) {
        if (pair.oracleHarness.outbound.empty() || pair.permanentHarness.outbound.empty()) {
            result.expectTrue(false, std::string(representation) + " client omitted its Hello");
            return;
        }
        const auto decoded = frontend::Codec::decodeClient(std::string_view(pair.oracleHarness.outbound.front().compactJson));
        const auto* oracleHello = decoded ? std::get_if<frontend::Hello>(&decoded.value()) : nullptr;
        const auto* outboundHello = std::get_if<frontend::Hello>(&pair.permanentHarness.outbound.front().value);
        const auto encodedPermanent = outboundHello ? frontend::Codec::encodeClient(frontend::ClientMessage{*outboundHello})
                                                    : frontend::CodecResult<frontend::Json>{frontend::CodecError{}};
        const auto decodedPermanent = encodedPermanent ? frontend::Codec::decodeClient(encodedPermanent.value())
                                                       : frontend::CodecResult<frontend::ClientMessage>{frontend::CodecError{}};
        const auto* permanentHello = decodedPermanent ? std::get_if<frontend::Hello>(&decodedPermanent.value()) : nullptr;
        const bool parity = oracleHello != nullptr && permanentHello != nullptr && *oracleHello == *permanentHello &&
                            pair.oracleHarness.outbound.front().sensitive == pair.permanentHarness.outbound.front().sensitive;
        result.expectTrue(parity, std::string(representation) + " clients emit the exact same sensitive Hello");
        if (parity) {
            ledger.cover("message:hello:client");
            ledger.cover("message:hello:decode");
        }
    }

    std::set<generated::MethodId> testAllGeneratedOperations(tests::support::TestResult& result,
                                                             tests::codex::FrontendDifferentialExecutionLedger& ledger,
                                                             const frontend::Json& fixture,
                                                             const frontend::Json& coverage) {
        const std::vector<CorpusMethod> corpus = methodCorpus(fixture, coverage);
        std::set<generated::MethodId> completedMethods;
        std::size_t covered = 0;
        for (const CorpusMethod& row : corpus) {
            Pair pair(true);
            if (covered == 0) {
                compareHello(result, ledger, pair, "expanded-v1");
            }
            pair.makeReady(true);
            const bool readyParity = pair.oracleClient.isReady() && pair.permanentClient.ready() &&
                                     pair.oracleClient.visibleSequence() == frontend::SequenceNumber{7} && pair.permanentClient.state() &&
                                     pair.permanentClient.state()->visibleSequence == permanent::model::FrontendSequence{7};
            result.expectTrue(readyParity, "both clients are ready before " + std::string(row.metadata->method));
            if (readyParity && covered == 0) {
                ledger.cover("representation:expanded-v1:client");
                ledger.cover("message:welcome:client");
                ledger.cover("message:welcome:decode");
                ledger.cover("message:snapshot:client");
                ledger.cover("message:snapshot:decode");
                ledger.cover("message:sync.complete:client");
                ledger.cover("message:sync.complete:decode");
            }
            pair.oracleHarness.outbound.clear();
            pair.permanentHarness.outbound.clear();
            pair.oracleHarness.completions.clear();
            pair.permanentHarness.completions.clear();

            const generated::CompleteCommandParameters parameters = generated::makeParameters(row.metadata->id, row.parameters);
            const oracle::Submission oldSubmission =
                pair.oracleClient.submit(parameters, [&pair](const oracle::GeneratedOperationResult& completion) {
                    pair.oracleHarness.completions.push_back(completion);
                });
            const permanent::Submission newSubmission =
                pair.permanentClient.submit(parameters, [&pair](const permanent::OperationResult& completion) {
                    pair.permanentHarness.completions.push_back(completion);
                });
            const bool bothAccepted = oldSubmission.accepted() && newSubmission.accepted() && oldSubmission.requestId.has_value() &&
                                      newSubmission.requestId.has_value();
            const std::string oldId = bothAccepted ? oldSubmission.requestId->value() : std::string{};
            const std::string newId = bothAccepted ? *newSubmission.requestId : std::string{};
            result.expectTrue(bothAccepted && oldId == newId,
                              "exact submission/request-ID parity for " + std::string(row.metadata->method));

            const bool oneCommandEach = pair.oracleHarness.outbound.size() == 1 && pair.permanentHarness.outbound.size() == 1;
            bool commandParity = false;
            if (oneCommandEach) {
                const auto decoded =
                    frontend::Codec::decodeDefinedCommand(std::string_view(pair.oracleHarness.outbound.front().compactJson));
                const generated::DefinedCommand& candidate = permanentCommand(pair.permanentHarness.outbound.front());
                commandParity = decoded && decoded.value() == candidate &&
                                generated::commandMethod(candidate.parameters) == row.metadata->id &&
                                pair.oracleHarness.outbound.front().sensitive == pair.permanentHarness.outbound.front().sensitive &&
                                pair.oracleHarness.outbound.front().sensitive == oracle::generated::bindingIsSensitive(row.metadata->id);
            } else {
                commandParity = false;
            }
            result.expectTrue(ledger.matched(coverageCase("method", row.coverageId, "client-dispatch"), commandParity),
                              "exact command wire/sensitivity parity for " + std::string(row.metadata->method));
            if (commandParity && covered == 0) {
                ledger.cover("message:command:client");
                ledger.cover("message:command:decode");
            }

            if (!bothAccepted) {
                continue;
            }
            const frontend::ServerMessage response = frontend::Response::success(oldId, row.result);
            const bool oldAccepted = pair.oracleConnection.receive(response).accepted;
            const bool newAccepted = pair.permanentClient.receive(pair.generation, response);
            result.expectTrue(oldAccepted == newAccepted && oldAccepted,
                              "exact response acceptance parity for " + std::string(row.metadata->method));
            if (row.metadata->id == generated::MethodId::SnapshotGet) {
                const frontend::Snapshot snapshot{frontend::SequenceNumber{7}, emptyExpandedState()};
                const frontend::SyncComplete complete{frontend::SequenceNumber{7}};
                result.expectTrue(pair.oracleConnection.receive(frontend::ServerMessage{snapshot}).accepted &&
                                      pair.permanentClient.receive(pair.generation, frontend::ServerMessage{snapshot}) &&
                                      pair.oracleConnection.receive(frontend::ServerMessage{complete}).accepted &&
                                      pair.permanentClient.receive(pair.generation, frontend::ServerMessage{complete}),
                                  "snapshot.get completes the identical snapshot synchronization stream");
            } else if (row.metadata->id == generated::MethodId::EventsReplay) {
                const frontend::SyncComplete complete{frontend::SequenceNumber{7}};
                result.expectTrue(pair.oracleConnection.receive(frontend::ServerMessage{complete}).accepted &&
                                      pair.permanentClient.receive(pair.generation, frontend::ServerMessage{complete}),
                                  "events.replay completes the identical replay synchronization stream");
            }

            const bool oneCompletionEach = pair.oracleHarness.completions.size() == 1 && pair.permanentHarness.completions.size() == 1;
            bool resultParity = false;
            if (oneCompletionEach) {
                const oracle::GeneratedOperationResult& oldCompletion = pair.oracleHarness.completions.front();
                const permanent::OperationResult& newCompletion = pair.permanentHarness.completions.front();
                resultParity = oldCompletion.requestId.value() == newCompletion.requestId && oldCompletion.succeeded() &&
                               newCompletion.succeeded() && oldCompletion.value == newCompletion.value &&
                               newCompletion.method == row.metadata->id && pair.oracleClient.pendingOperationCount() == 0 &&
                               pair.permanentClient.pendingOperationCount() == 0;
            }
            result.expectTrue(ledger.matched(coverageCase("method", row.coverageId, "result-schema"), resultParity),
                              "typed result/correlation parity for " + std::string(row.metadata->method));
            if (resultParity) {
                completedMethods.insert(row.metadata->id);
                ++covered;
                if (covered == 1) {
                    ledger.cover("message:response:client");
                    ledger.cover("message:response:decode");
                }
                if (row.metadata->id == generated::MethodId::SnapshotGet) {
                    ledger.cover("synchronization:snapshot:client");
                } else if (row.metadata->id == generated::MethodId::EventsReplay) {
                    ledger.cover("synchronization:replay:client");
                }
            }
        }
        result.expectTrue(covered == generated::AllMethods.size() && covered == 105,
                          "all 105 generated operation contracts dispatch and complete through both clients");
        return completedMethods;
    }

    void testLegacyRepresentationAndStaleTransition(tests::support::TestResult& result,
                                                    tests::codex::FrontendDifferentialExecutionLedger& ledger) {
        Pair pair(false);
        compareHello(result, ledger, pair, "legacy-v1");
        pair.makeReady(false);
        const oracle::State oldState = pair.oracleClient.state();
        const std::shared_ptr<const permanent::PublishedState> newState = pair.permanentClient.state();
        const bool representationParity = pair.oracleClient.isReady() && pair.permanentClient.ready() &&
                                          oldState.representationMode() == oracle::RepresentationMode::LegacyV1 && newState &&
                                          newState->representation == permanent::RepresentationMode::LegacyV1 &&
                                          oldState.visibleSequence() == frontend::SequenceNumber{7} &&
                                          newState->visibleSequence == permanent::model::FrontendSequence{7};
        result.expectTrue(ledger.matched("representation:legacy-v1:client", representationParity),
                          "legacy-v1 snapshot reduction has exact readiness, representation, and sequence parity");

        const std::vector<frontend::FrontendEvent> legacyEvents{
            {frontend::SequenceNumber{8}, "backend.lifecycle.changed", frontend::Json{{"lifecycle", "ready"}}, frontend::Json::object()},
            {frontend::SequenceNumber{9},
             "codex.extension",
             frontend::Json{{"method", "vendor/future"}, {"params", {{"vendor", "future"}, {"bounded", true}}}},
             frontend::Json::object()},
        };
        for (const frontend::FrontendEvent& event : legacyEvents) {
            pair.oracleHarness.trace.clear();
            pair.permanentHarness.trace.clear();
            const frontend::EventBatch batch{event.sequence, event.sequence, {event}, frontend::Json::object()};
            const bool oldAccepted = pair.oracleConnection.receive(frontend::ServerMessage{batch}).accepted;
            const bool newAccepted = pair.permanentClient.receive(pair.generation, frontend::ServerMessage{batch});
            const oracle::State currentOld = pair.oracleClient.state();
            const std::shared_ptr<const permanent::PublishedState> currentNew = pair.permanentClient.state();
            result.expectTrue(oldAccepted && newAccepted && currentOld.visibleSequence() == event.sequence && currentNew &&
                                  currentNew->visibleSequence == permanent::model::FrontendSequence{event.sequence} &&
                                  currentOld.revision() == currentNew->revision && pair.oracleHarness.trace == pair.permanentHarness.trace,
                              "legacy-v1 live reducer/callback parity for " + event.type);
        }

        pair.oracleConnection.transportDisconnected(oracle::TransportError{"differential disconnect", true});
        pair.permanentClient.transportDisconnected(pair.generation, permanent::TransportError{"differential disconnect", true});
        const oracle::State staleOld = pair.oracleClient.state();
        const std::shared_ptr<const permanent::PublishedState> staleNew = pair.permanentClient.state();
        result.expectTrue(pair.oracleClient.connectionState() == oracle::ConnectionState::Disconnected &&
                              pair.permanentClient.connectionState() == permanent::ConnectionState::Disconnected &&
                              staleOld.freshness() == oracle::StateFreshness::Stale && staleNew &&
                              staleNew->freshness == permanent::PublishedFreshness::Stale &&
                              staleOld.visibleSequence() == frontend::SequenceNumber{9} &&
                              staleNew->visibleSequence == permanent::model::FrontendSequence{9},
                          "physical disconnect retains the same state/cursor and marks both clients stale");
    }

    void testLiveStateCallbackInvalidation(tests::support::TestResult& result, const frontend::Json& fixture) {
        const auto fixtureEvent =
            std::find_if(fixture.at("expandedEvents").begin(), fixture.at("expandedEvents").end(), [](const frontend::Json& value) {
                return value.at("type").get_ref<const std::string&>() == "provider.updated";
            });
        if (fixtureEvent == fixture.at("expandedEvents").end()) {
            throw std::runtime_error("generated differential fixture omits provider.updated");
        }

        Pair pair(true);
        pair.makeReady(true);
        pair.oracleHarness.trace.clear();
        pair.permanentHarness.trace.clear();
        pair.oracleHarness.observed.clear();
        pair.permanentHarness.observed.clear();
        pair.oracleHarness.closes = 0;
        pair.permanentHarness.closes = 0;

        bool oldCallbackArmed = true;
        bool newCallbackArmed = true;
        pair.oracleHarness.stateUpdateHook = [&] {
            if (oldCallbackArmed) {
                oldCallbackArmed = false;
                pair.oracleClient.close("differential callback close");
            }
        };
        pair.permanentHarness.stateUpdateHook = [&] {
            if (newCallbackArmed) {
                newCallbackArmed = false;
                pair.permanentClient.close("differential callback close");
            }
        };

        const frontend::FrontendEvent event{frontend::SequenceNumber{8},
                                            fixtureEvent->at("type").get<std::string>(),
                                            fixtureEvent->at("data"),
                                            frontend::Json::object()};
        const frontend::EventBatch batch{event.sequence, event.sequence, {event}, frontend::Json::object()};
        const frontend::ServerMessage message{batch};
        const bool oldAccepted = pair.oracleConnection.receive(message).accepted;
        const bool newAccepted = pair.permanentClient.receive(pair.generation, message);

        const std::vector<std::string> expectedTrace{"state", "connection", "state", "connection"};
        const std::vector<std::string> oldTrace = pair.oracleHarness.trace;
        const std::vector<std::string> newTrace = pair.permanentHarness.trace;
        const bool retiredOldAccepted = pair.oracleConnection.receive(message).accepted;
        const bool retiredNewAccepted = pair.permanentClient.receive(pair.generation, message);
        const bool parity = !oldAccepted && !newAccepted && !retiredOldAccepted && !retiredNewAccepted && !oldCallbackArmed &&
                            !newCallbackArmed && pair.oracleClient.connectionState() == oracle::ConnectionState::Closed &&
                            pair.permanentClient.connectionState() == permanent::ConnectionState::Closed &&
                            pair.oracleHarness.closes == 1 && pair.permanentHarness.closes == 1 &&
                            pair.oracleHarness.observed.empty() && pair.permanentHarness.observed.empty() && oldTrace == expectedTrace &&
                            newTrace == expectedTrace && pair.oracleHarness.trace == oldTrace && pair.permanentHarness.trace == newTrace;
        result.expectTrue(parity,
                          "onStateUpdated close stops cursor/protocol continuation for the retired generation with old/new callback-order "
                          "parity: accepted=" +
                              std::to_string(oldAccepted) + "/" + std::to_string(newAccepted) + " retired=" +
                              std::to_string(retiredOldAccepted) + "/" + std::to_string(retiredNewAccepted) + " trace=" +
                              traceText(pair.oracleHarness.trace) + "/" + traceText(pair.permanentHarness.trace));
    }

    template <typename Value>
    std::size_t projectedEntryCount(const oracle::Projected<Value>& projected) {
        return projected.value ? projected.value->entries.size() : 0;
    }

    void testOptionalMetadataPresenceParity(tests::support::TestResult& result) {
        for (const bool explicitDefaults : {false, true}) {
            frontend::Json state = emptyExpandedState();
            state["accounts"] = frontend::Json::object();
            if (explicitDefaults) {
                state["threadList"]["stamp"] = frontend::Json{{"generation", std::uint64_t{0}}, {"freshness", "unknown"}};
                state["accounts"]["stamp"] = frontend::Json{{"generation", std::uint64_t{0}}, {"freshness", "unknown"}};
                state["accounts"]["truncation"] = frontend::Json{{"truncated", false}};
            }

            Pair pair(true);
            pair.makeReadyWithSnapshot(frontend::Snapshot{frontend::SequenceNumber{7}, std::move(state)});
            const oracle::State oldState = pair.oracleClient.state();
            const std::shared_ptr<const permanent::PublishedState> newState = pair.permanentClient.state();
            std::string mismatch;
            const bool parity = normalizedStateParity(oldState, newState, mismatch);
            const bool oldPresence = oldState.threadList().value && oldState.accounts().value &&
                                     oldState.threadList().value->stamp.has_value() == explicitDefaults &&
                                     oldState.accounts().value->projection.stamp.has_value() == explicitDefaults &&
                                     oldState.accounts().value->projection.truncation.has_value() == explicitDefaults;
            const bool newPresence = newState && newState->snapshot && newState->snapshot->threadList.stampKnown == explicitDefaults &&
                                     newState->snapshot->accounts.state.stampKnown == explicitDefaults &&
                                     newState->snapshot->accounts.state.truncationKnown == explicitDefaults;
            result.expectTrue(parity && oldPresence && newPresence,
                              std::string("old/new State parity preserves ") + (explicitDefaults ? "explicit default" : "absent") +
                                  " stamp/truncation presence" + (mismatch.empty() ? std::string{} : ": " + mismatch));
        }
    }

    void testAllExpandedEventFamilies(tests::support::TestResult& result,
                                      tests::codex::FrontendDifferentialExecutionLedger& ledger,
                                      const frontend::Json& fixture) {
        std::size_t covered = 0;
        for (const frontend::Json& fixtureEvent : fixture.at("expandedEvents")) {
            Pair pair(true);
            frontend::SequenceNumber::Value sequence = 8;
            frontend::FrontendEvent event;
            event.type = fixtureEvent.at("type").get<std::string>();
            event.data = fixtureEvent.at("data");
            if (event.type == "item.content.updated") {
                frontend::Json state = emptyExpandedState();
                state["threads"].push_back(frontend::Json{{"id", "x"}});
                state["turns"].push_back(
                    frontend::Json{{"id", "x"}, {"threadId", "x"}, {"status", "x"}, {"active", false}, {"terminal", false}});
                state["items"].push_back(frontend::Json{{"id", "x"}, {"type", "agentMessage"}, {"threadId", "x"}, {"turnId", "x"}});
                pair.makeReadyWithSnapshot(frontend::Snapshot{frontend::SequenceNumber{7}, std::move(state)});
                event.data["channel"] = "agentText";
            } else if (event.type == "activity.updated") {
                pair.makeReady(true);
                event.data = {{"activity",
                               {{"key", "activity-differential"},
                                {"kind", "tool"},
                                {"lifecycle", "running"},
                                {"active", true},
                                {"stamp", {{"generation", std::uint64_t{1}}, {"freshness", "current"}}}}}};
            } else {
                pair.makeReady(true);
            }
            event.sequence = frontend::SequenceNumber{sequence};
            const frontend::EventBatch batch{event.sequence, event.sequence, {event}, frontend::Json::object()};

            pair.oracleHarness.trace.clear();
            pair.permanentHarness.trace.clear();
            const bool oldAccepted = pair.oracleConnection.receive(frontend::ServerMessage{batch}).accepted;
            const bool newAccepted = pair.permanentClient.receive(pair.generation, frontend::ServerMessage{batch});
            const oracle::State oldState = pair.oracleClient.state();
            const std::shared_ptr<const permanent::PublishedState> newState = pair.permanentClient.state();
            const permanent::model::CanonicalSnapshot* candidate = newState && newState->snapshot ? newState->snapshot.get() : nullptr;

            std::string stateMismatch;
            const bool completeStateParity = normalizedStateParity(oldState, newState, stateMismatch);

            bool indexedParity = candidate != nullptr && oldState.threads().size() == candidate->threads.size() &&
                                 oldState.turns().size() == candidate->turns.size() && oldState.items().size() == candidate->items.size() &&
                                 oldState.pendingRequests().size() == candidate->pendingRequests.size() &&
                                 projectedEntryCount(oldState.processes()) == candidate->processes.size() &&
                                 projectedEntryCount(oldState.filesystemWatches()) == candidate->filesystemWatches.entries.size() &&
                                 projectedEntryCount(oldState.fuzzySearches()) == candidate->fuzzySearches.entries.size() &&
                                 projectedEntryCount(oldState.notices()) == candidate->notices.entries.size() &&
                                 projectedEntryCount(oldState.activities()) == candidate->activities.entries.size();
            const bool parity = oldAccepted == newAccepted && oldAccepted && oldState.visibleSequence() == event.sequence &&
                                candidate != nullptr &&
                                newState->visibleSequence == permanent::model::FrontendSequence{event.sequence.value()} &&
                                oldState.revision() == newState->revision && indexedParity && completeStateParity &&
                                pair.oracleHarness.trace == pair.permanentHarness.trace;
            result.expectTrue(ledger.matched(coverageCase("event", event.type, "client-expanded"), parity),
                              "typed reducer/index/callback parity for event family " + event.type +
                                  " accepted=" + std::to_string(oldAccepted) + "/" + std::to_string(newAccepted) +
                                  " revision=" + std::to_string(oldState.revision()) + "/" +
                                  std::to_string(newState ? newState->revision : 0) + " indexed=" + std::to_string(indexedParity) +
                                  " trace=" + traceText(pair.oracleHarness.trace) + "/" + traceText(pair.permanentHarness.trace) +
                                  (stateMismatch.empty() ? std::string{} : " stateMismatch=" + stateMismatch));
            result.expectTrue(ledger.matched(coverageCase("event", event.type, "reducer"), parity),
                              "canonical typed reducer parity for event family " + event.type);
            if (parity && covered == 0) {
                ledger.cover("message:events:client");
                ledger.cover("message:events:decode");
            }
            ++covered;
        }
        result.expectTrue(covered == static_cast<std::size_t>(frontend::ExpandedEventType::DiagnosticsUpdated) + 1U && covered == 26,
                          "authority-derived client differential reduces all 26 expanded event families");
    }

    void testAllLegacyEventFamilies(tests::support::TestResult& result,
                                    tests::codex::FrontendDifferentialExecutionLedger& ledger,
                                    const frontend::Json& fixture) {
        std::size_t covered = 0;
        for (const frontend::Json& fixtureEvent : fixture.at("expandedEvents")) {
            const std::string family = fixtureEvent.at("type").get<std::string>();
            const auto type = frontend::expandedEventTypeFromString(family);
            if (!type) {
                throw std::runtime_error("generated fixture contains an unknown expanded event family");
            }
            frontend::Json representativeData = fixtureEvent.at("data");
            if (family == "sessions.updated") {
                representativeData["sessions"] = frontend::Json::array({frontend::Json{{"sessionId", "1"}, {"role", "observer"}}});
            } else if (family == "thread.upserted") {
                representativeData["thread"]["fullyLoaded"] = false;
                representativeData["thread"]["turns"] = frontend::Json::array();
                representativeData["thread"]["extensions"] = frontend::Json::object();
            } else if (family == "turn.upserted") {
                representativeData["turn"]["items"] = frontend::Json::array();
                representativeData["turn"]["extensions"] = frontend::Json::object();
            } else if (family == "item.upserted") {
                representativeData["item"]["threadId"] = "x";
                representativeData["item"]["turnId"] = "x";
            } else if (family == "pendingRequests.updated") {
                representativeData["pendingRequests"] =
                    frontend::Json::array({fixture.at("expandedSnapshot").at("state").at("pendingRequests").at(0)});
            } else if (family == "item.content.updated") {
                representativeData["channel"] = "agentText";
            } else if (family == "activity.updated") {
                representativeData = {{"activity",
                                       {{"key", "legacy-activity-differential"},
                                        {"kind", "tool"},
                                        {"lifecycle", "running"},
                                        {"active", true},
                                        {"stamp", {{"generation", std::uint64_t{1}}, {"freshness", "current"}}}}}};
            }
            const frontend::ExpandedFrontendEvent expanded{
                frontend::SequenceNumber{8}, *type, std::move(representativeData), frontend::Json::object()};
            permanent::model::OccurrenceDecodeContext context{
                permanent::model::OccurrenceGroupIdentity{"client-legacy-" + std::to_string(covered)},
                0,
                1,
                permanent::model::SourceStamp{"p2-client-differential"}};
            const auto canonical = permanent::model::decodeExpandedOccurrence(expanded, context);
            const auto compatible = canonical
                                        ? tests::codex::withAuthorityLegacyCompatibility(canonical.value())
                                        : permanent::model::OccurrenceResult<permanent::model::CanonicalOccurrence>{canonical.error()};
            const auto event = compatible ? permanent::model::encodeLegacyOccurrence(compatible.value())
                                          : permanent::model::OccurrenceResult<frontend::FrontendEvent>{compatible.error()};
            if (!event) {
                throw std::runtime_error("canonical legacy encoding failed for " + family + ": " + event.error().message);
            }
            Pair pair(false);
            if (family == "item.content.updated") {
                permanent::model::CanonicalSnapshot snapshot;
                snapshot.sequence = permanent::model::FrontendSequence{7};
                snapshot.provider.lifecycle = permanent::model::ProviderLifecycle::Ready;
                snapshot.provider.desiredRunning = true;
                snapshot.threads.emplace_back(permanent::model::ThreadIdentity{"x"});
                snapshot.turns.emplace_back(permanent::model::TurnIdentity{"x"}, permanent::model::ThreadIdentity{"x"});
                permanent::model::ItemData item(
                    permanent::model::ItemIdentity{"x"}, permanent::model::ThreadIdentity{"x"}, permanent::model::TurnIdentity{"x"});
                snapshot.items.emplace_back(permanent::model::AgentMessageItem{std::move(item)});
                const auto legacySnapshot = permanent::model::encodeLegacySnapshot(snapshot);
                if (!legacySnapshot) {
                    throw std::runtime_error("canonical legacy item-content prerequisite snapshot failed");
                }
                pair.makeReadyWithSnapshot(legacySnapshot.value());
            } else {
                pair.makeReady(false);
            }
            const frontend::EventBatch batch{event.value().sequence, event.value().sequence, {event.value()}, frontend::Json::object()};
            pair.oracleHarness.trace.clear();
            pair.permanentHarness.trace.clear();
            const bool oldAccepted = pair.oracleConnection.receive(frontend::ServerMessage{batch}).accepted;
            const bool newAccepted = pair.permanentClient.receive(pair.generation, frontend::ServerMessage{batch});
            const oracle::State oldState = pair.oracleClient.state();
            const std::shared_ptr<const permanent::PublishedState> newState = pair.permanentClient.state();
            const permanent::model::CanonicalSnapshot* candidate = newState && newState->snapshot ? newState->snapshot.get() : nullptr;
            std::string stateMismatch;
            const bool completeStateParity = normalizedStateParity(oldState, newState, stateMismatch);
            const bool indexedParity = candidate != nullptr && oldState.threads().size() == candidate->threads.size() &&
                                       oldState.turns().size() == candidate->turns.size() &&
                                       oldState.items().size() == candidate->items.size() &&
                                       oldState.pendingRequests().size() == candidate->pendingRequests.size();
            const bool parity = oldAccepted && newAccepted && oldState.visibleSequence() == event.value().sequence && newState &&
                                newState->visibleSequence == permanent::model::FrontendSequence{event.value().sequence} &&
                                oldState.revision() == newState->revision && indexedParity && completeStateParity &&
                                pair.oracleHarness.trace == pair.permanentHarness.trace;
            result.expectTrue(ledger.matched(coverageCase("event", family, "client-legacy"), parity),
                              "legacy-v1 reducer/index/callback parity for event family " + family +
                                  " accepted=" + std::to_string(oldAccepted) + "/" + std::to_string(newAccepted) +
                                  " sequence=" + std::to_string(oldState.visibleSequence() ? oldState.visibleSequence()->value() : 0) +
                                  "/" + std::to_string(newState && newState->visibleSequence ? newState->visibleSequence->value() : 0) +
                                  " revision=" + std::to_string(oldState.revision()) + "/" +
                                  std::to_string(newState ? newState->revision : 0) +
                                  " counts=" + std::to_string(oldState.threads().size()) + "," + std::to_string(oldState.turns().size()) +
                                  "," + std::to_string(oldState.items().size()) + "," + std::to_string(oldState.pendingRequests().size()) +
                                  "/" + std::to_string(candidate ? candidate->threads.size() : 0) + "," +
                                  std::to_string(candidate ? candidate->turns.size() : 0) + "," +
                                  std::to_string(candidate ? candidate->items.size() : 0) + "," +
                                  std::to_string(candidate ? candidate->pendingRequests.size() : 0) +
                                  " trace=" + traceText(pair.oracleHarness.trace) + "/" + traceText(pair.permanentHarness.trace) +
                                  (stateMismatch.empty() ? std::string{} : " stateMismatch=" + stateMismatch));
            ++covered;
        }
        result.expectTrue(covered == 26, "legacy-v1 client differential visits all 26 expanded authority families");
    }

    void testAllThreadItemDiscriminators(tests::support::TestResult& result,
                                         tests::codex::FrontendDifferentialExecutionLedger& ledger,
                                         const frontend::Json& fixture) {
        Pair pair(true);
        pair.makeReady(true);
        frontend::SequenceNumber::Value sequence = 8;
        std::size_t covered = 0;
        for (frontend::Json item : fixture.at("expandedSnapshot").at("state").at("items")) {
            const std::string identity = "differential-item-" + std::to_string(covered);
            item["id"] = identity;
            const std::string discriminator = item.at("type").get<std::string>();
            const frontend::FrontendEvent event{
                frontend::SequenceNumber{sequence}, "item.upserted", frontend::Json{{"item", std::move(item)}}, frontend::Json::object()};
            const frontend::EventBatch batch{event.sequence, event.sequence, {event}, frontend::Json::object()};
            pair.oracleHarness.trace.clear();
            pair.permanentHarness.trace.clear();
            const bool oldAccepted = pair.oracleConnection.receive(frontend::ServerMessage{batch}).accepted;
            const bool newAccepted = pair.permanentClient.receive(pair.generation, frontend::ServerMessage{batch});
            const oracle::State oldState = pair.oracleClient.state();
            const std::shared_ptr<const permanent::PublishedState> newState = pair.permanentClient.state();
            const oracle::ItemState* oldItem = oldState.item(identity);
            const permanent::model::ThreadItem* newItem = newState ? newState->item(identity) : nullptr;
            std::string stateMismatch;
            const bool parity =
                oldAccepted && newAccepted && oldItem != nullptr && newItem != nullptr && oldItem->kind.identity == discriminator &&
                frontend::toString(permanent::model::threadItemKind(*newItem)) == discriminator &&
                normalizedStateParity(oldState, newState, stateMismatch) && pair.oracleHarness.trace == pair.permanentHarness.trace;
            result.expectTrue(ledger.matched(coverageCase("item", discriminator, "reduction-expanded"), parity),
                              "typed ThreadItem projection/reduction parity for " + discriminator +
                                  (stateMismatch.empty() ? std::string{} : " stateMismatch=" + stateMismatch));
            ++sequence;
            ++covered;
        }
        result.expectTrue(covered == generated::AllThreadItemProjections.size() && covered == 18,
                          "authority-derived client differential reduces all 18 ThreadItem discriminators");
    }

    void testAllPendingRequestKinds(tests::support::TestResult& result,
                                    tests::codex::FrontendDifferentialExecutionLedger& ledger,
                                    const frontend::Json& fixture,
                                    const std::set<generated::MethodId>& completedMethods) {
        Pair pair(true);
        pair.makeReady(true);
        frontend::SequenceNumber::Value sequence = 8;
        std::size_t covered = 0;
        for (frontend::Json pending : fixture.at("expandedSnapshot").at("state").at("pendingRequests")) {
            const std::string identity = std::to_string(covered + 100U);
            pending["pendingRequestId"] = identity;
            const std::string kind = pending.at("kind").get<std::string>();
            const frontend::FrontendEvent event{frontend::SequenceNumber{sequence},
                                                "pendingRequests.updated",
                                                frontend::Json{{"pendingRequests", frontend::Json::array({std::move(pending)})}},
                                                frontend::Json::object()};
            const frontend::EventBatch batch{event.sequence, event.sequence, {event}, frontend::Json::object()};
            pair.oracleHarness.trace.clear();
            pair.permanentHarness.trace.clear();
            const bool oldAccepted = pair.oracleConnection.receive(frontend::ServerMessage{batch}).accepted;
            const bool newAccepted = pair.permanentClient.receive(pair.generation, frontend::ServerMessage{batch});
            const oracle::State oldState = pair.oracleClient.state();
            const std::shared_ptr<const permanent::PublishedState> newState = pair.permanentClient.state();
            const oracle::PendingRequestState* oldPending = oldState.pendingRequest(oracle::PendingRequestId{identity});
            const permanent::model::PendingRequest* newPending = newState ? newState->pendingRequest(identity) : nullptr;
            std::string stateMismatch;
            const bool parity = oldAccepted && newAccepted && oldPending != nullptr && newPending != nullptr &&
                                frontend::toString(oldPending->kind) == kind &&
                                frontend::toString(permanent::model::pendingRequestKind(*newPending)) == kind &&
                                normalizedStateParity(oldState, newState, stateMismatch) &&
                                pair.oracleHarness.trace == pair.permanentHarness.trace;
            result.expectTrue(ledger.matched(coverageCase("pending", kind, "reduction"), parity),
                              "typed pending-request projection/reduction parity for " + kind +
                                  (stateMismatch.empty() ? std::string{} : " stateMismatch=" + stateMismatch));
            const generated::PendingRequestProjectionMetadata* metadata = generated::pendingRequestProjectionFromKind(kind);
            bool responseParity = metadata != nullptr && !metadata->responseMethods.empty();
            if (metadata != nullptr) {
                for (std::string_view responseMethod : metadata->responseMethods) {
                    const auto method = generated::definedMethodFromString(responseMethod);
                    responseParity = responseParity && method.has_value() && completedMethods.contains(*method);
                }
            }
            result.expectTrue(ledger.matched(coverageCase("pending", kind, "response"), responseParity),
                              "pending-request response facades completed for " + kind);
            ++sequence;
            ++covered;
        }
        result.expectTrue(covered == generated::AllPendingRequestProjections.size() && covered == 10,
                          "authority-derived client differential reduces all pending-request kinds");
    }

    void testLegacyItemAndPendingReduction(tests::support::TestResult& result,
                                           tests::codex::FrontendDifferentialExecutionLedger& ledger,
                                           const frontend::Json& fixture) {
        const auto expanded = frontend::Codec::decodeExpandedSnapshot(fixture.at("expandedSnapshot"));
        if (!expanded) {
            throw std::runtime_error("generated expanded snapshot fixture failed typed decoding");
        }
        auto canonical = permanent::model::decodeSnapshot(expanded.value());
        if (!canonical) {
            throw std::runtime_error("generated expanded snapshot fixture failed canonical decoding");
        }
        const permanent::model::CanonicalSnapshot& decoded = canonical.value();
        if (decoded.items.size() != generated::AllThreadItemProjections.size() ||
            decoded.pendingRequests.size() != generated::AllPendingRequestProjections.size()) {
            throw std::runtime_error("canonical fixture does not contain every item and pending-request authority member");
        }
        for (std::size_t index = 0; index < decoded.items.size(); ++index) {
            permanent::model::CanonicalSnapshot snapshot;
            snapshot.sequence = permanent::model::FrontendSequence{7};
            snapshot.provider.lifecycle = permanent::model::ProviderLifecycle::Ready;
            snapshot.provider.desiredRunning = true;
            snapshot.threads.emplace_back(permanent::model::ThreadIdentity{"thread"});
            snapshot.turns.emplace_back(permanent::model::TurnIdentity{"turn"}, permanent::model::ThreadIdentity{"thread"});
            permanent::model::ThreadItem item = decoded.items[index];
            const std::string identity = "item-" + std::to_string(index + 1U);
            std::visit(
                [&identity](auto& value) {
                    value.value.id = permanent::model::ItemIdentity{identity};
                    value.value.threadId = permanent::model::ThreadIdentity{"thread"};
                    value.value.turnId = permanent::model::TurnIdentity{"turn"};
                },
                item);
            const std::string discriminator(frontend::toString(permanent::model::threadItemKind(item)));
            snapshot.items.push_back(std::move(item));
            const auto legacy = permanent::model::encodeLegacySnapshot(snapshot);
            if (!legacy) {
                throw std::runtime_error("canonical item failed legacy projection for " + discriminator);
            }
            Pair pair(false);
            try {
                pair.makeReadyWithSnapshot(legacy.value());
            } catch (const std::exception& error) {
                const oracle::Error* oldError = latestConnectionError(pair.oracleHarness);
                const permanent::ClientError* newError = latestConnectionError(pair.permanentHarness);
                throw std::runtime_error("legacy item snapshot handshake failed for " + discriminator + ": " + error.what() +
                                         " old=" + (oldError != nullptr ? oldError->message : std::string{"<none>"}) +
                                         " new=" + (newError != nullptr ? newError->message : std::string{"<none>"}));
            }
            const oracle::State oldState = pair.oracleClient.state();
            const std::shared_ptr<const permanent::PublishedState> newState = pair.permanentClient.state();
            const oracle::ItemState* oldItem = oldState.item(identity);
            const permanent::model::ThreadItem* newItem = newState ? newState->item(identity) : nullptr;
            std::string stateMismatch;
            const bool parity = oldItem != nullptr && newItem != nullptr && oldItem->kind.identity == discriminator &&
                                frontend::toString(permanent::model::threadItemKind(*newItem)) == discriminator &&
                                normalizedStateParity(oldState, newState, stateMismatch);
            result.expectTrue(ledger.matched(coverageCase("item", discriminator, "reduction-legacy"), parity),
                              "legacy-v1 ThreadItem reduction parity for " + discriminator +
                                  (stateMismatch.empty() ? std::string{} : " stateMismatch=" + stateMismatch));
        }
        {
            constexpr std::string_view OpaqueKey = "futureProviderState";
            constexpr std::string_view OpaqueValue = "safe-looking-but-opaque-sentinel";
            constexpr std::string_view OpaqueExtensionKey = "futureProviderExtension";
            constexpr std::string_view OpaqueExtensionValue = "safe-looking-extension-sentinel";
            backend::Snapshot source;
            source.sequence = backend::SequenceNumber{7};
            source.provider.lifecycle = backend::ProviderLifecycle::Ready;
            backend::ThreadSnapshot thread;
            thread.id = "future-thread";
            thread.fullyLoaded = true;
            backend::TurnSnapshot turn;
            turn.id = "future-turn";
            turn.threadId = thread.id;
            turn.status = "completed";
            backend::ItemSnapshot before;
            before.id = "known-before";
            before.type = "plan";
            before.status = "completed";
            turn.items.push_back(std::move(before));
            backend::ItemSnapshot future;
            future.id = "future-item";
            future.type = "future_codex_item_kind";
            future.status = "completed";
            future.agentText = "visible future agent text";
            future.contentTruncated = false;
            future.data = frontend::Json{{OpaqueKey, OpaqueValue}};
            turn.items.push_back(std::move(future));
            backend::ItemSnapshot after;
            after.id = "known-after";
            after.type = "plan";
            after.status = "completed";
            turn.items.push_back(std::move(after));
            thread.turns.push_back(std::move(turn));
            source.threads.push_back(std::move(thread));

            server::BackendProjection backendProjection;
            const auto projected = backendProjection.projectSnapshot(source);
            if (!projected) {
                throw std::runtime_error("backend-derived future-item Snapshot projection failed");
            }
            const auto legacy = permanent::model::encodeLegacySnapshot(projected.value());
            Pair pair(false);
            if (legacy) {
                pair.makeReadyWithSnapshot(legacy.value());
            }
            const oracle::State oldState = pair.oracleClient.state();
            const auto newState = pair.permanentClient.state();
            const oracle::ItemState* oldFuture = oldState.item("future-item");
            const permanent::model::LegacyItemCompatibility* newFuture =
                newState && newState->snapshot && newState->snapshot->legacyItems.size() == 1 ? &newState->snapshot->legacyItems.front()
                                                                                              : nullptr;
            const std::string snapshotState = newState ? newState->serializeForTesting().dump() : std::string{};
            const bool snapshotContained =
                legacy && oldFuture && oldFuture->kind.identity == "future_codex_item_kind" && !oldFuture->kind.known.has_value() &&
                newFuture && newFuture->discriminator == "future_codex_item_kind" &&
                newFuture->value.threadId == permanent::model::ThreadIdentity{"future-thread"} &&
                newFuture->value.turnId == permanent::model::TurnIdentity{"future-turn"} &&
                newFuture->value.agentText == "visible future agent text" &&
                (!newFuture->value.safeDetails.has_value() || newFuture->value.safeDetails->empty()) &&
                newFuture->value.legacyExtensions.empty() && (newFuture->value.contentTruncated || newFuture->value.truncation.truncated) &&
                snapshotState.find(OpaqueKey) == std::string::npos && snapshotState.find(OpaqueValue) == std::string::npos;

            backend::CodexExtensionReceived extension;
            extension.method = "item/fileChange/patchUpdated";
            extension.payload = frontend::Json{{"threadId", "future-thread"}, {"turnId", "future-turn"}, {"itemId", "future-item"}};
            extension.safeProjection = true;
            const std::vector<backend::SequencedBackendEvent> events{{backend::SequenceNumber{8}, std::move(extension)}};
            const auto projectedLive = backendProjection.projectOccurrences(events, source);
            if (!projectedLive || projectedLive.value().snapshotRequired || projectedLive.value().occurrences.size() != 1) {
                throw std::runtime_error("backend-derived future-item live projection failed");
            }
            permanent::model::TypedOccurrenceJournal journal(permanent::model::JournalConfig{
                frontend::DefaultJournalMaxEntries, frontend::DefaultJournalMaxBytes, permanent::model::FrontendSequence{7}});
            const auto appended = journal.appendGroup(projectedLive.value().occurrences.front().occurrence);
            if (appended.records.size() != 1) {
                throw std::runtime_error("backend-derived future-item live occurrence failed journal append");
            }
            const auto liveEvent = permanent::model::encodeLegacyOccurrence(appended.records.front());
            if (!liveEvent) {
                throw std::runtime_error("backend-derived future-item live occurrence failed legacy encoding");
            }
            const frontend::EventBatch batch{
                liveEvent.value().sequence, liveEvent.value().sequence, {liveEvent.value()}, frontend::Json::object()};
            const bool liveAccepted = pair.oracleConnection.receive(frontend::ServerMessage{batch}).accepted &&
                                      pair.permanentClient.receive(pair.generation, frontend::ServerMessage{batch});
            const auto liveState = pair.permanentClient.state();
            const std::string publishedState = liveState ? liveState->serializeForTesting().dump() : std::string{};
            const permanent::model::LegacyItemCompatibility* liveFuture =
                liveState && liveState->snapshot && liveState->snapshot->legacyItems.size() == 1 ? &liveState->snapshot->legacyItems.front()
                                                                                                 : nullptr;
            result.expectTrue(snapshotContained && liveAccepted && liveFuture &&
                                  (!liveFuture->value.safeDetails.has_value() || liveFuture->value.safeDetails->empty()) &&
                                  liveFuture->value.legacyExtensions.empty() &&
                                  (liveFuture->value.contentTruncated || liveFuture->value.truncation.truncated) &&
                                  publishedState.find(OpaqueKey) == std::string::npos &&
                                  publishedState.find(OpaqueValue) == std::string::npos,
                              "backend-derived legacy Snapshot and live reduction retain future-item metadata while opaque data never "
                              "enters PublishedState");

            backend::Snapshot extensionSource = source;
            backend::ItemSnapshot& extensionItem = extensionSource.threads.front().turns.front().items.at(1);
            extensionItem.data = frontend::Json::object();
            extensionItem.extensions = frontend::Json{{OpaqueExtensionKey, OpaqueExtensionValue}};
            const auto projectedExtension = backendProjection.projectSnapshot(extensionSource);
            if (!projectedExtension) {
                throw std::runtime_error("backend-derived future-item extension Snapshot projection failed");
            }
            const auto extensionLegacy = permanent::model::encodeLegacySnapshot(projectedExtension.value());
            Pair extensionPair(false);
            if (extensionLegacy) {
                extensionPair.makeReadyWithSnapshot(extensionLegacy.value());
            }
            const auto extensionState = extensionPair.permanentClient.state();
            const permanent::model::LegacyItemCompatibility* extensionFuture =
                extensionState && extensionState->snapshot && extensionState->snapshot->legacyItems.size() == 1
                    ? &extensionState->snapshot->legacyItems.front()
                    : nullptr;
            const std::string serializedExtensionState = extensionState ? extensionState->serializeForTesting().dump() : std::string{};
            result.expectTrue(
                extensionLegacy && extensionFuture &&
                    (!extensionFuture->value.safeDetails.has_value() || extensionFuture->value.safeDetails->empty()) &&
                    extensionFuture->value.legacyExtensions.empty() &&
                    (extensionFuture->value.contentTruncated || extensionFuture->value.truncation.truncated) &&
                    serializedExtensionState.find(OpaqueExtensionKey) == std::string::npos &&
                    serializedExtensionState.find(OpaqueExtensionValue) == std::string::npos,
                "backend-derived safe-looking unknown item extensions are omitted before legacy client reduction and PublishedState");
        }
        for (std::size_t index = 0; index < decoded.pendingRequests.size(); ++index) {
            permanent::model::CanonicalSnapshot snapshot;
            snapshot.sequence = permanent::model::FrontendSequence{7};
            snapshot.provider.lifecycle = permanent::model::ProviderLifecycle::Ready;
            snapshot.provider.desiredRunning = true;
            permanent::model::PendingRequest pending = decoded.pendingRequests[index];
            const std::string identity = std::to_string(index + 100U);
            std::visit(
                [&identity](auto& value) {
                    value.value.id = permanent::model::PendingRequestIdentity{identity};
                    value.value.threadId.reset();
                    value.value.turnId.reset();
                    value.value.itemId.reset();
                },
                pending);
            const std::string kind(frontend::toString(permanent::model::pendingRequestKind(pending)));
            const frontend::PendingRequestKind typedKind = permanent::model::pendingRequestKind(pending);
            snapshot.pendingRequests.push_back(std::move(pending));
            const auto legacy = permanent::model::encodeLegacySnapshot(snapshot);
            if (!legacy) {
                throw std::runtime_error("canonical pending request failed legacy projection for " + kind);
            }
            Pair pair(false);
            const bool legacyTyped = typedKind == frontend::PendingRequestKind::CommandExecutionApproval ||
                                     typedKind == frontend::PendingRequestKind::FileChangeApproval ||
                                     typedKind == frontend::PendingRequestKind::UserInput ||
                                     typedKind == frontend::PendingRequestKind::Authentication;
            if (legacyTyped) {
                pair.makeReadyWithSnapshot(legacy.value());
                const oracle::State oldState = pair.oracleClient.state();
                const std::shared_ptr<const permanent::PublishedState> newState = pair.permanentClient.state();
                const oracle::PendingRequestState* oldPending = oldState.pendingRequest(oracle::PendingRequestId{identity});
                const permanent::model::PendingRequest* newPending = newState ? newState->pendingRequest(identity) : nullptr;
                std::string stateMismatch;
                result.expectTrue(oldPending != nullptr && newPending != nullptr && frontend::toString(oldPending->kind) == kind &&
                                      frontend::toString(permanent::model::pendingRequestKind(*newPending)) == kind &&
                                      normalizedStateParity(oldState, newState, stateMismatch),
                                  "legacy-v1 pending-request reduction parity for " + kind +
                                      (stateMismatch.empty() ? std::string{} : " stateMismatch=" + stateMismatch));
            } else {
                const std::vector<frontend::FrontendMethod> methods = allMethods();
                const frontend::Welcome welcome{"1",
                                                frontend::SessionRole::Observer,
                                                legacy.value().sequence,
                                                frontend::SyncMode::Snapshot,
                                                frontend::Json::object(),
                                                advertisement(false),
                                                methods,
                                                methods,
                                                "p2-differential"};
                const bool oldWelcome = pair.oracleConnection.receive(frontend::ServerMessage{welcome}).accepted;
                const bool newWelcome = pair.permanentClient.receive(pair.generation, frontend::ServerMessage{welcome});
                const bool oldSnapshot = oldWelcome && pair.oracleConnection.receive(frontend::ServerMessage{legacy.value()}).accepted;
                const bool newSnapshot =
                    newWelcome && pair.permanentClient.receive(pair.generation, frontend::ServerMessage{legacy.value()});
                const bool synchronized = newSnapshot && pair.permanentClient.receive(
                                                                  pair.generation,
                                                                  frontend::ServerMessage{frontend::SyncComplete{legacy.value().sequence}});
                const auto newState = pair.permanentClient.state();
                const bool correctedCompatibility = oldWelcome && newWelcome && !oldSnapshot && newSnapshot && synchronized &&
                                                    pair.oracleHarness.closes == 1 && pair.permanentHarness.closes == 0 && newState &&
                                                    newState->snapshot->legacyPendingRequests.size() == 1 &&
                                                    newState->snapshot->legacyPendingRequests.front().value.id.value() == identity;
                result.expectTrue(correctedCompatibility,
                                  "legacy-v1 pending-request corrected compatibility for " + kind + " accepted=" + std::to_string(oldSnapshot) +
                                      "/" + std::to_string(newSnapshot) +
                                      " state=" + std::to_string(static_cast<unsigned>(pair.oracleClient.connectionState())) + "/" +
                                      std::to_string(static_cast<unsigned>(pair.permanentClient.connectionState())) + " closes=" +
                                      std::to_string(pair.oracleHarness.closes) + "/" + std::to_string(pair.permanentHarness.closes));
            }
        }
    }

    void testLifecycleCaseTable(tests::support::TestResult& result,
                                tests::codex::FrontendDifferentialExecutionLedger& ledger,
                                const frontend::Json& fixture) {
        enum class CaseKind {
            SecondPhysicalGeneration,
            PendingCommandReconnect,
            ControllerReconnect,
            ReverseResponseReconnect,
            StalePhysicalGeneration,
            SubmissionBeforeSynchronization,
            OrdinaryCommandError,
            DuplicateResponse,
            UnexpectedResponse,
        };
        struct LifecycleCase {
            std::string_view identity;
            CaseKind kind;
            std::optional<generated::MethodId> method;
            bool sensitive = false;
            std::string_view coverageIdentity;
        };
        const std::vector<LifecycleCase> cases{
            {"second physical attachment advances the generation",
             CaseKind::SecondPhysicalGeneration,
             std::nullopt,
             false,
             "lifecycle:physical-generation:client"},
            {"accepted pending command is not retried on reconnect",
             CaseKind::PendingCommandReconnect,
             generated::MethodId::ModelList,
             false,
             "lifecycle:command-retry:client"},
            {"controller acquisition is not automatically restored",
             CaseKind::ControllerReconnect,
             std::nullopt,
             false,
             "lifecycle:controller-restore:client"},
            {"reverse response is not resubmitted on reconnect",
             CaseKind::ReverseResponseReconnect,
             generated::MethodId::ApprovalRespond,
             true,
             "lifecycle:reverse-response-retry:client"},
            {"message from the stale physical generation is rejected",
             CaseKind::StalePhysicalGeneration,
             std::nullopt,
             false,
             "callback:stale-generation:client"},
            {"submission before synchronization has parity",
             CaseKind::SubmissionBeforeSynchronization,
             generated::MethodId::ModelList,
             false,
             {}},
            {"ordinary command error has parity without closing",
             CaseKind::OrdinaryCommandError,
             generated::MethodId::ModelList,
             false,
             {}},
            {"duplicate response has connection-local parity", CaseKind::DuplicateResponse, generated::MethodId::ModelList, false, {}},
            {"unexpected response has connection-local parity", CaseKind::UnexpectedResponse, generated::MethodId::ModelList, false, {}},
        };

        for (const LifecycleCase& testCase : cases) {
            bool parity = false;
            std::string stateMismatch;
            const auto stateParity = [&stateMismatch](const Pair& pair, std::string_view stage) {
                std::string mismatch;
                if (completeStateParity(pair, mismatch)) {
                    return true;
                }
                stateMismatch += " " + std::string(stage) + "=" + mismatch;
                return false;
            };

            switch (testCase.kind) {
                case CaseKind::SecondPhysicalGeneration:
                case CaseKind::PendingCommandReconnect:
                case CaseKind::ControllerReconnect:
                case CaseKind::ReverseResponseReconnect:
                case CaseKind::StalePhysicalGeneration: {
                    Pair pair(true);
                    if (testCase.kind == CaseKind::ControllerReconnect) {
                        frontend::Json state = emptyExpandedState();
                        state["controller"] = frontend::Json{{"controllerSessionId", "1"}, {"present", true}};
                        pair.makeReadyWithSnapshot(
                            frontend::Snapshot{frontend::SequenceNumber{7}, std::move(state)}, frontend::SessionRole::Controller, "1");
                    } else if (testCase.kind == CaseKind::ReverseResponseReconnect) {
                        frontend::Json state = emptyExpandedState();
                        state["pendingRequests"].push_back(
                            frontend::Json{{"pendingRequestId", "1"}, {"kind", "command_execution_approval"}, {"truncated", false}});
                        pair.makeReadyWithSnapshot(frontend::Snapshot{frontend::SequenceNumber{7}, std::move(state)});
                    } else {
                        pair.makeReady(true);
                    }

                    const std::optional<oracle::SessionInfo> initialOldSession = pair.oracleClient.session();
                    const std::shared_ptr<const permanent::PublishedState> initialNewState = pair.permanentClient.state();
                    const bool initialRoleParity =
                        testCase.kind != CaseKind::ControllerReconnect ||
                        (initialOldSession.has_value() && initialNewState && initialNewState->session.has_value() &&
                         initialOldSession->role == frontend::SessionRole::Controller &&
                         initialNewState->session->role == frontend::SessionRole::Controller);
                    const bool initialStateParity = stateParity(pair, "ready");
                    const std::uint64_t firstOracleGeneration = pair.oracleConnection.generation();
                    const permanent::PhysicalGeneration firstPermanentGeneration = pair.generation;
                    frontend::Json oldCommandAttempts = frontend::Json::array();
                    frontend::Json newCommandAttempts = frontend::Json::array();
                    frontend::Json oldDeliveredCallbacks = frontend::Json::array();
                    frontend::Json newDeliveredCallbacks = frontend::Json::array();
                    pair.oracleHarness.outbound.clear();
                    pair.permanentHarness.outbound.clear();
                    pair.oracleHarness.completions.clear();
                    pair.permanentHarness.completions.clear();

                    bool submissionParity = true;
                    if (testCase.method.has_value()) {
                        const frontend::Json& methodFixture = fixtureForMethod(fixture, *testCase.method);
                        const generated::CompleteCommandParameters parameters =
                            generated::makeParameters(*testCase.method, methodFixture.at("minimalParams"));
                        const oracle::Submission oldSubmission =
                            pair.oracleClient.submit(parameters, [&pair](const oracle::GeneratedOperationResult& completion) {
                                pair.oracleHarness.completions.push_back(completion);
                            });
                        const permanent::Submission newSubmission =
                            pair.permanentClient.submit(parameters, [&pair](const permanent::OperationResult& completion) {
                                pair.permanentHarness.completions.push_back(completion);
                            });
                        const auto oldCommand =
                            pair.oracleHarness.outbound.size() == 1
                                ? frontend::Codec::decodeDefinedCommand(std::string_view(pair.oracleHarness.outbound.front().compactJson))
                                : frontend::CodecResult<generated::DefinedCommand>{frontend::CodecError{}};
                        const generated::DefinedCommand* newCommand = pair.permanentHarness.outbound.size() == 1
                                                                          ? &permanentCommand(pair.permanentHarness.outbound.front())
                                                                          : nullptr;
                        submissionParity = oldSubmission && newSubmission && oldSubmission.requestId && newSubmission.requestId &&
                                           oldSubmission.requestId->value() == *newSubmission.requestId && oldCommand &&
                                           newCommand != nullptr && oldCommand.value() == *newCommand &&
                                           pair.oracleHarness.outbound.front().sensitive == testCase.sensitive &&
                                           pair.permanentHarness.outbound.front().sensitive == testCase.sensitive;
                        appendCommandAttempts(pair.oracleHarness, firstOracleGeneration, oldCommandAttempts);
                        appendCommandAttempts(pair.permanentHarness, firstPermanentGeneration, newCommandAttempts);
                    }

                    oracle::Connection staleOracleConnection = std::move(pair.oracleConnection);
                    staleOracleConnection.transportDisconnected(oracle::TransportError{"differential disconnect", true});
                    pair.permanentClient.transportDisconnected(firstPermanentGeneration,
                                                               permanent::TransportError{"differential disconnect", true});
                    bool completionParity =
                        pair.oracleClient.pendingOperationCount() == 0 && pair.permanentClient.pendingOperationCount() == 0;
                    if (testCase.method.has_value()) {
                        completionParity = completionParity && pair.oracleHarness.completions.size() == 1 &&
                                           pair.permanentHarness.completions.size() == 1 &&
                                           pair.oracleHarness.completions.front().error.has_value() &&
                                           pair.permanentHarness.completions.front().error.has_value() &&
                                           pair.oracleHarness.completions.front().requestId.value() ==
                                               pair.permanentHarness.completions.front().requestId &&
                                           pair.permanentHarness.completions.front().method == *testCase.method &&
                                           sameClientError(*pair.oracleHarness.completions.front().error,
                                                           *pair.permanentHarness.completions.front().error);
                    } else {
                        completionParity =
                            completionParity && pair.oracleHarness.completions.empty() && pair.permanentHarness.completions.empty();
                    }
                    const bool disconnectedStateParity = stateParity(pair, "disconnected");

                    pair.oracleHarness.outbound.clear();
                    pair.permanentHarness.outbound.clear();
                    pair.attachNext();
                    const std::uint64_t secondOracleGeneration = pair.oracleConnection.generation();
                    const permanent::PhysicalGeneration secondPermanentGeneration = pair.generation;
                    const bool generationParity = firstOracleGeneration == firstPermanentGeneration &&
                                                  secondOracleGeneration == secondPermanentGeneration &&
                                                  secondOracleGeneration == firstOracleGeneration + 1 && singleHelloParity(pair);
                    pair.makeReadyWithReplay();
                    appendCommandAttempts(pair.oracleHarness, secondOracleGeneration, oldCommandAttempts);
                    appendCommandAttempts(pair.permanentHarness, secondPermanentGeneration, newCommandAttempts);

                    const std::optional<oracle::SessionInfo> reconnectedOldSession = pair.oracleClient.session();
                    const std::shared_ptr<const permanent::PublishedState> reconnectedNewState = pair.permanentClient.state();
                    const bool noAutomaticResubmission = pair.oracleHarness.outbound.size() == 1 &&
                                                         pair.permanentHarness.outbound.size() == 1 && pair.oracleClient.isReady() &&
                                                         pair.permanentClient.ready() && reconnectedOldSession.has_value() &&
                                                         reconnectedNewState && reconnectedNewState->session.has_value() &&
                                                         reconnectedOldSession->role == frontend::SessionRole::Observer &&
                                                         reconnectedNewState->session->role == frontend::SessionRole::Observer;
                    const bool reconnectedStateParity = stateParity(pair, "reconnected");

                    bool staleGenerationParity = true;
                    if (testCase.kind == CaseKind::StalePhysicalGeneration) {
                        const std::uint64_t oldRevision = pair.oracleClient.state().revision();
                        const std::uint64_t newRevision = pair.permanentClient.state()->revision;
                        const std::size_t oldOutbound = pair.oracleHarness.outbound.size();
                        const std::size_t newOutbound = pair.permanentHarness.outbound.size();
                        const std::size_t oldObserved = pair.oracleHarness.observed.size();
                        const std::size_t newObserved = pair.permanentHarness.observed.size();
                        const std::size_t oldCloses = pair.oracleHarness.closes;
                        const std::size_t newCloses = pair.permanentHarness.closes;
                        const std::size_t oldTrace = pair.oracleHarness.trace.size();
                        const std::size_t newTrace = pair.permanentHarness.trace.size();
                        staleOracleConnection.transportConnected();
                        pair.permanentClient.transportConnected(firstPermanentGeneration);
                        const frontend::ServerMessage staleMessage = frontend::ProtocolErrorMessage{frontend::ErrorCode::RateLimited,
                                                                                                    "stale generation sentinel",
                                                                                                    {},
                                                                                                    false,
                                                                                                    std::nullopt,
                                                                                                    std::nullopt,
                                                                                                    frontend::Json::object()};
                        const bool oldAccepted = staleOracleConnection.receive(staleMessage).accepted;
                        const bool newAccepted = pair.permanentClient.receive(firstPermanentGeneration, staleMessage);
                        for (std::size_t index = oldTrace; index < pair.oracleHarness.trace.size(); ++index) {
                            oldDeliveredCallbacks.push_back(
                                {{"name", pair.oracleHarness.trace[index]}, {"physicalGeneration", firstOracleGeneration}});
                        }
                        for (std::size_t index = newTrace; index < pair.permanentHarness.trace.size(); ++index) {
                            newDeliveredCallbacks.push_back(
                                {{"name", pair.permanentHarness.trace[index]}, {"physicalGeneration", firstPermanentGeneration}});
                        }
                        const bool staleCallbackStateParity = stateParity(pair, "stale-callback");
                        staleGenerationParity =
                            !oldAccepted && !newAccepted && pair.oracleClient.state().revision() == oldRevision &&
                            pair.permanentClient.state()->revision == newRevision && pair.oracleHarness.outbound.size() == oldOutbound &&
                            pair.permanentHarness.outbound.size() == newOutbound && pair.oracleHarness.observed.size() == oldObserved &&
                            pair.permanentHarness.observed.size() == newObserved && pair.oracleHarness.closes == oldCloses &&
                            pair.permanentHarness.closes == newCloses && pair.oracleHarness.trace.size() == oldTrace &&
                            pair.permanentHarness.trace.size() == newTrace &&
                            pair.permanentClient.activeGeneration() == secondPermanentGeneration && staleCallbackStateParity;
                    }

                    frontend::Json oldControllerOwnership = frontend::Json::object();
                    oldControllerOwnership[std::to_string(firstOracleGeneration)] =
                        initialOldSession && initialOldSession->role == frontend::SessionRole::Controller;
                    oldControllerOwnership[std::to_string(secondOracleGeneration)] =
                        reconnectedOldSession && reconnectedOldSession->role == frontend::SessionRole::Controller;
                    frontend::Json newControllerOwnership = frontend::Json::object();
                    newControllerOwnership[std::to_string(firstPermanentGeneration)] =
                        initialNewState && initialNewState->session && initialNewState->session->role == frontend::SessionRole::Controller;
                    newControllerOwnership[std::to_string(secondPermanentGeneration)] =
                        reconnectedNewState && reconnectedNewState->session &&
                        reconnectedNewState->session->role == frontend::SessionRole::Controller;
                    const frontend::Json oldBorder = lifecycleBorderObservation(secondOracleGeneration,
                                                                                std::move(oldCommandAttempts),
                                                                                std::move(oldControllerOwnership),
                                                                                std::move(oldDeliveredCallbacks));
                    const frontend::Json newBorder = lifecycleBorderObservation(secondPermanentGeneration,
                                                                                std::move(newCommandAttempts),
                                                                                std::move(newControllerOwnership),
                                                                                std::move(newDeliveredCallbacks));
                    const auto lifecycleMismatch = tests::codex::differential::firstMismatch(oldBorder, newBorder);
                    const bool sharedBorderParity = !lifecycleMismatch;
                    if (lifecycleMismatch) {
                        stateMismatch += " shared-border=" + lifecycleMismatch->path + " old=" + lifecycleMismatch->oldValue +
                                         " new=" + lifecycleMismatch->newValue;
                    }

                    parity = initialRoleParity && initialStateParity && submissionParity && completionParity && disconnectedStateParity &&
                             generationParity && noAutomaticResubmission && reconnectedStateParity && staleGenerationParity &&
                             sharedBorderParity;
                    break;
                }
                case CaseKind::SubmissionBeforeSynchronization: {
                    Pair pair(true);
                    const std::vector<frontend::FrontendMethod> methods = allMethods();
                    const frontend::Welcome welcome{"1",
                                                    frontend::SessionRole::Observer,
                                                    frontend::SequenceNumber{7},
                                                    frontend::SyncMode::Snapshot,
                                                    frontend::Json::object(),
                                                    advertisement(true),
                                                    methods,
                                                    methods,
                                                    "p2-differential"};
                    const bool oldWelcome = pair.oracleConnection.receive(frontend::ServerMessage{welcome}).accepted;
                    const bool newWelcome = pair.permanentClient.receive(pair.generation, frontend::ServerMessage{welcome});
                    const std::size_t oldOutboundBefore = pair.oracleHarness.outbound.size();
                    const std::size_t newOutboundBefore = pair.permanentHarness.outbound.size();
                    std::size_t oldCompletions = 0;
                    std::size_t newCompletions = 0;
                    const frontend::Json& methodFixture = fixtureForMethod(fixture, *testCase.method);
                    const generated::CompleteCommandParameters parameters =
                        generated::makeParameters(*testCase.method, methodFixture.at("minimalParams"));
                    const oracle::Submission oldSubmission = pair.oracleClient.submit(parameters, [&](const auto&) {
                        ++oldCompletions;
                    });
                    const permanent::Submission newSubmission = pair.permanentClient.submit(parameters, [&](const auto&) {
                        ++newCompletions;
                    });
                    const bool synchronizingStateParity = stateParity(pair, "synchronizing");
                    parity = oldWelcome && newWelcome && !oldSubmission && !newSubmission && oldSubmission.error && newSubmission.error &&
                             sameClientError(*oldSubmission.error, *newSubmission.error) &&
                             oldSubmission.error->clientCode == oracle::ClientErrorCode::NotReady &&
                             newSubmission.error->clientCode == permanent::ClientErrorCode::NotReady && oldCompletions == 0 &&
                             newCompletions == 0 && pair.oracleHarness.outbound.size() == oldOutboundBefore &&
                             pair.permanentHarness.outbound.size() == newOutboundBefore &&
                             pair.oracleClient.connectionState() == oracle::ConnectionState::Synchronizing &&
                             pair.permanentClient.connectionState() == permanent::ConnectionState::Synchronizing &&
                             pair.oracleHarness.closes == 0 && pair.permanentHarness.closes == 0 && synchronizingStateParity;
                    break;
                }
                case CaseKind::OrdinaryCommandError: {
                    Pair pair(true);
                    pair.makeReady(true);
                    pair.oracleHarness.outbound.clear();
                    pair.permanentHarness.outbound.clear();
                    pair.oracleHarness.completions.clear();
                    pair.permanentHarness.completions.clear();
                    const frontend::Json& methodFixture = fixtureForMethod(fixture, *testCase.method);
                    const generated::CompleteCommandParameters parameters =
                        generated::makeParameters(*testCase.method, methodFixture.at("minimalParams"));
                    const oracle::Submission oldSubmission = pair.oracleClient.submit(parameters, [&pair](const auto& completion) {
                        pair.oracleHarness.completions.push_back(completion);
                    });
                    const permanent::Submission newSubmission = pair.permanentClient.submit(parameters, [&pair](const auto& completion) {
                        pair.permanentHarness.completions.push_back(completion);
                    });
                    const frontend::Json remoteDetails{{"category", "differential"}, {"retryable", false}};
                    const frontend::ServerMessage response =
                        frontend::Response::failure(oldSubmission.requestId ? oldSubmission.requestId->value() : std::string{},
                                                    frontend::CommandError{frontend::ErrorCode::NotFound,
                                                                           "ordinary differential failure",
                                                                           std::optional<frontend::Json>{remoteDetails},
                                                                           frontend::Json::object()});
                    const bool oldAccepted = oldSubmission && pair.oracleConnection.receive(response).accepted;
                    const bool newAccepted = newSubmission && pair.permanentClient.receive(pair.generation, response);
                    const bool commandErrorStateParity = stateParity(pair, "command-error");
                    parity =
                        oldAccepted && newAccepted && oldSubmission.requestId && newSubmission.requestId &&
                        oldSubmission.requestId->value() == *newSubmission.requestId && pair.oracleHarness.completions.size() == 1 &&
                        pair.permanentHarness.completions.size() == 1 && pair.oracleHarness.completions.front().error &&
                        pair.permanentHarness.completions.front().error &&
                        sameClientError(*pair.oracleHarness.completions.front().error, *pair.permanentHarness.completions.front().error) &&
                        pair.permanentHarness.completions.front().method == *testCase.method &&
                        pair.oracleClient.pendingOperationCount() == 0 && pair.permanentClient.pendingOperationCount() == 0 &&
                        pair.oracleClient.isReady() && pair.permanentClient.ready() && pair.oracleConnection.isOpen() &&
                        pair.permanentClient.activeGeneration().has_value() && pair.oracleHarness.closes == 0 &&
                        pair.permanentHarness.closes == 0 && pair.oracleHarness.observed == pair.permanentHarness.observed &&
                        commandErrorStateParity;
                    break;
                }
                case CaseKind::DuplicateResponse:
                case CaseKind::UnexpectedResponse: {
                    Pair pair(true);
                    pair.makeReady(true);
                    pair.oracleHarness.completions.clear();
                    pair.permanentHarness.completions.clear();
                    pair.oracleHarness.observed.clear();
                    pair.permanentHarness.observed.clear();
                    const frontend::Json& methodFixture = fixtureForMethod(fixture, *testCase.method);
                    const generated::CompleteCommandParameters parameters =
                        generated::makeParameters(*testCase.method, methodFixture.at("minimalParams"));
                    const bool duplicate = testCase.kind == CaseKind::DuplicateResponse;
                    std::string requestId = "c1-r999";
                    bool setupParity = true;
                    if (duplicate) {
                        const oracle::Submission oldSubmission = pair.oracleClient.submit(parameters, [&pair](const auto& completion) {
                            pair.oracleHarness.completions.push_back(completion);
                        });
                        const permanent::Submission newSubmission =
                            pair.permanentClient.submit(parameters, [&pair](const auto& completion) {
                                pair.permanentHarness.completions.push_back(completion);
                            });
                        setupParity = oldSubmission && newSubmission && oldSubmission.requestId && newSubmission.requestId &&
                                      oldSubmission.requestId->value() == *newSubmission.requestId;
                        if (setupParity) {
                            requestId = oldSubmission.requestId->value();
                            const frontend::ServerMessage first = frontend::Response::success(requestId, methodFixture.at("minimalResult"));
                            const bool oldFirstAccepted = pair.oracleConnection.receive(first).accepted;
                            const bool newFirstAccepted = pair.permanentClient.receive(pair.generation, first);
                            setupParity = oldFirstAccepted && newFirstAccepted && pair.oracleHarness.completions.size() == 1 &&
                                          pair.permanentHarness.completions.size() == 1;
                        }
                    }

                    const std::size_t oldObserved = pair.oracleHarness.observed.size();
                    const std::size_t newObserved = pair.permanentHarness.observed.size();
                    const frontend::ServerMessage rejected = frontend::Response::success(requestId, methodFixture.at("minimalResult"));
                    const bool oldAccepted = pair.oracleConnection.receive(rejected).accepted;
                    const bool newAccepted = pair.permanentClient.receive(pair.generation, rejected);
                    const oracle::Error* oldError = latestConnectionError(pair.oracleHarness);
                    const permanent::ClientError* newError = latestConnectionError(pair.permanentHarness);
                    const std::size_t expectedCompletions = duplicate ? 1U : 0U;
                    const bool correlationFailureStateParity = stateParity(pair, "correlation-failure");
                    parity = setupParity && !oldAccepted && !newAccepted && oldError != nullptr && newError != nullptr &&
                             sameClientError(*oldError, *newError) && oldError->origin == oracle::ErrorOrigin::Protocol &&
                             oldError->clientCode == oracle::ClientErrorCode::UnexpectedMessage &&
                             newError->origin == permanent::ErrorOrigin::Protocol &&
                             newError->clientCode == permanent::ClientErrorCode::UnexpectedMessage &&
                             pair.oracleClient.connectionState() == oracle::ConnectionState::Disconnected &&
                             pair.permanentClient.connectionState() == permanent::ConnectionState::Disconnected &&
                             pair.oracleHarness.closes == 1 && pair.permanentHarness.closes == 1 &&
                             pair.oracleHarness.observed.size() == oldObserved && pair.permanentHarness.observed.size() == newObserved &&
                             pair.oracleHarness.completions.size() == expectedCompletions &&
                             pair.permanentHarness.completions.size() == expectedCompletions &&
                             pair.oracleClient.pendingOperationCount() == 0 && pair.permanentClient.pendingOperationCount() == 0 &&
                             correlationFailureStateParity;
                    break;
                }
            }
            const bool recorded =
                testCase.coverageIdentity.empty() ? parity : ledger.matched(std::string(testCase.coverageIdentity), parity);
            result.expectTrue(recorded,
                              std::string(testCase.identity) + " across the old oracle and permanent client core" + stateMismatch);
        }
    }

    void testCapabilityValidation(tests::support::TestResult& result, tests::codex::FrontendDifferentialExecutionLedger& ledger) {
        Pair pair(true);
        pair.makeReady(true);
        for (const generated::CapabilityMetadata& metadata : generated::AllCapabilities) {
            const auto capability = static_cast<frontend::FrontendCapability>(metadata.id);
            const oracle::CapabilityStatus oldStatus = pair.oracleClient.capabilityStatus(capability);
            const permanent::CapabilityStatus newStatus = pair.permanentClient.capabilityStatus(capability);
            const bool parity = oldStatus.capability == newStatus.capability &&
                                static_cast<unsigned>(oldStatus.defined) == static_cast<unsigned>(newStatus.defined) &&
                                static_cast<unsigned>(oldStatus.implemented) == static_cast<unsigned>(newStatus.implemented) &&
                                static_cast<unsigned>(oldStatus.permitted) == static_cast<unsigned>(newStatus.permitted);
            result.expectTrue(ledger.matched(coverageCase("capability", metadata.key, "client-validation"), parity),
                              "capability validation parity for " + std::string(metadata.key));
        }
    }

    void testProtocolErrorMessage(tests::support::TestResult& result, tests::codex::FrontendDifferentialExecutionLedger& ledger) {
        Pair pair(true);
        pair.makeReady(true);
        pair.oracleHarness.trace.clear();
        pair.permanentHarness.trace.clear();
        const frontend::ServerMessage message =
            frontend::ProtocolErrorMessage{frontend::ErrorCode::RateLimited,
                                           "bounded differential warning",
                                           {},
                                           false,
                                           std::nullopt,
                                           std::optional<frontend::Json>{frontend::Json{{"retryable", true}}},
                                           frontend::Json::object()};
        const bool oldAccepted = pair.oracleConnection.receive(message).accepted;
        const bool newAccepted = pair.permanentClient.receive(pair.generation, message);
        const bool parity = oldAccepted && newAccepted && pair.oracleClient.isReady() && pair.permanentClient.ready() &&
                            pair.oracleHarness.observed == pair.permanentHarness.observed &&
                            pair.oracleHarness.trace == pair.permanentHarness.trace && pair.oracleHarness.closes == 0 &&
                            pair.permanentHarness.closes == 0;
        result.expectTrue(ledger.matched("message:protocol.error:client", parity), "non-closing protocol-error handling parity");
        result.expectTrue(ledger.matched("message:protocol.error:decode", parity), "protocol-error decoding parity");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    tests::codex::FrontendDifferentialExecutionLedger ledger{"client"};
    try {
        const frontend::Json fixture = loadFixture();
        const frontend::Json coverage = loadCoverageFixture();
        const std::set<generated::MethodId> completedMethods = testAllGeneratedOperations(result, ledger, fixture, coverage);
        testLegacyRepresentationAndStaleTransition(result, ledger);
        testLiveStateCallbackInvalidation(result, fixture);
        testOptionalMetadataPresenceParity(result);
        testAllExpandedEventFamilies(result, ledger, fixture);
        testAllLegacyEventFamilies(result, ledger, fixture);
        testAllThreadItemDiscriminators(result, ledger, fixture);
        testAllPendingRequestKinds(result, ledger, fixture, completedMethods);
        testLegacyItemAndPendingReduction(result, ledger, fixture);
        testLifecycleCaseTable(result, ledger, fixture);
        testCapabilityValidation(result, ledger);
        testProtocolErrorMessage(result, ledger);
    } catch (const std::exception& error) {
        result.expectTrue(false, std::string("client differential harness failure: ") + error.what());
    }
    const int status = result.processResult();
    if (status == 0) {
        try {
            ledger.write(AISUITE_CODEX_FRONTEND_CLIENT_DIFFERENTIAL_LEDGER);
        } catch (const std::exception& error) {
            std::cerr << "CodexFrontendClientDifferentialTest: cannot write execution ledger: " << error.what() << '\n';
            return 1;
        }
    }
    return status;
}
