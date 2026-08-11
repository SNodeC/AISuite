/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/FrontendService.h"
#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/detail/BackendProjectionBuilder.h"
#include "ai/openai/codex/frontend/detail/FrontendProjection.h"
#include "ai/openai/codex/frontend/internal/server/BackendProjection.h"
#include "ai/openai/codex/frontend/internal/server/ServerCore.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include "CodexFrontendDifferentialComparison.h"
#include "CodexFrontendDifferentialExecutionLedger.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef AISUITE_CODEX_FRONTEND_DIFFERENTIAL_FIXTURE
#error "AISUITE_CODEX_FRONTEND_DIFFERENTIAL_FIXTURE is required"
#endif

#ifndef AISUITE_CODEX_FRONTEND_DIFFERENTIAL_COVERAGE_FIXTURE
#error "AISUITE_CODEX_FRONTEND_DIFFERENTIAL_COVERAGE_FIXTURE is required"
#endif

#ifndef AISUITE_CODEX_FRONTEND_SERVER_DIFFERENTIAL_LEDGER
#error "AISUITE_CODEX_FRONTEND_SERVER_DIFFERENTIAL_LEDGER is required"
#endif

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace legacy = ai::openai::codex::frontend::detail;
    namespace model = ai::openai::codex::frontend::internal::model;
    namespace permanent = ai::openai::codex::frontend::internal::server;
    using JsonMismatch = tests::codex::differential::JsonMismatch;
    using tests::codex::differential::firstMismatch;

    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;

    constexpr std::uint64_t TrustedUser = 4242;
    constexpr std::string_view DifferentialBearer = "p2-differential-bearer";

    tests::codex::FrontendDifferentialExecutionLedger* executionLedger = nullptr;

    std::string coverageCase(std::string_view family, std::string_view identity, std::string_view dimension) {
        return std::string(family) + ":" + std::string(identity) + ":" + std::string(dimension);
    }

    bool recordCase(std::string identity, bool matched) {
        return executionLedger == nullptr ? matched : executionLedger->matched(std::move(identity), matched);
    }

    class ManualScheduler {
    public:
        void schedule(std::function<void()> callback) {
            callbacks.push_back(std::move(callback));
        }

        void drain() {
            std::size_t count = 0;
            while (!callbacks.empty()) {
                std::function<void()> callback = std::move(callbacks.front());
                callbacks.pop_front();
                callback();
                if (++count > 100'000) {
                    throw std::runtime_error("server differential scheduler did not quiesce");
                }
            }
        }

    private:
        std::deque<std::function<void()>> callbacks;
    };

    class PermanentBackend final : public permanent::BackendPort {
    public:
        [[nodiscard]] bool providerReady() const noexcept override {
            return ready;
        }

        [[nodiscard]] model::CanonicalSnapshot snapshot() const override {
            model::CanonicalSnapshot current = state;
            current.sessions.clear();
            current.sessions.reserve(sessions.size());
            for (const std::string& id : sessions) {
                const std::optional<model::SessionIdentity> parsed = model::SessionIdentity::parse(id);
                if (!parsed.has_value()) {
                    continue;
                }
                model::SessionState session(*parsed);
                session.role = controller == id ? frontend::SessionRole::Controller : frontend::SessionRole::Observer;
                current.sessions.push_back(std::move(session));
            }
            current.controller.session.reset();
            if (controller.has_value()) {
                current.controller.session = model::SessionIdentity::parse(*controller);
            }
            current.capacity.sessions = current.sessions.size();
            current.capacity.observers = static_cast<std::size_t>(
                std::count_if(current.sessions.begin(), current.sessions.end(), [](const model::SessionState& session) {
                    return session.role == frontend::SessionRole::Observer;
                }));
            return current;
        }

        [[nodiscard]] permanent::BackendSubmitStatus submit(permanent::BackendInvocation invocation) override {
            invocations.push_back(std::move(invocation));
            return submitStatus;
        }

        [[nodiscard]] bool performProviderLifecycleAction(permanent::ProviderLifecycleAction action) override {
            lifecycleActions.push_back(action);
            return lifecycleSucceeds;
        }

        void sessionOpened(const model::SessionIdentity& session, const frontend::FrontendPrincipal&) override {
            sessions.push_back(session.value());
            advanceRevision();
        }

        void sessionClosed(const model::SessionIdentity& session) noexcept override {
            std::erase(sessions, session.value());
            advanceRevision();
        }

        void controllerChanged(const std::optional<model::SessionIdentity>& session) noexcept override {
            controller = session ? std::optional<std::string>{session->value()} : std::nullopt;
            advanceRevision();
        }

        bool ready = false;
        bool lifecycleSucceeds = true;
        permanent::BackendSubmitStatus submitStatus = permanent::BackendSubmitStatus::Unavailable;
        model::CanonicalSnapshot state;
        std::vector<permanent::BackendInvocation> invocations;
        std::vector<permanent::ProviderLifecycleAction> lifecycleActions;
        std::vector<std::string> sessions;
        std::optional<std::string> controller;

    private:
        void advanceRevision() noexcept {
            std::uint64_t revision = state.backendCursor.backendRevision.value_or(0);
            if (revision != std::numeric_limits<std::uint64_t>::max()) {
                ++revision;
            }
            state.backendCursor.backendRevision = revision;
        }
    };

    frontend::FrontendPeerContext trustedPeer() {
        frontend::FrontendPeerContext peer;
        peer.transport = frontend::FrontendTransportKind::Unix;
        peer.loopback = true;
        peer.localPeer = true;
        peer.unixUserId = TrustedUser;
        return peer;
    }

    frontend::FrontendPeerContext remotePeer() {
        frontend::FrontendPeerContext peer;
        peer.transport = frontend::FrontendTransportKind::Ipv4;
        peer.loopback = true;
        peer.remoteAddress = "127.0.0.42";
        return peer;
    }

    frontend::Json loadFixture() {
        std::ifstream input(AISUITE_CODEX_FRONTEND_DIFFERENTIAL_FIXTURE);
        if (!input) {
            throw std::runtime_error("cannot open generated server differential fixture");
        }
        return frontend::Json::parse(input);
    }

    frontend::Json loadCoverageFixture() {
        std::ifstream input(AISUITE_CODEX_FRONTEND_DIFFERENTIAL_COVERAGE_FIXTURE);
        if (!input) {
            throw std::runtime_error("cannot open authority-derived server differential coverage fixture");
        }
        return frontend::Json::parse(input);
    }

    const frontend::Json& coverageMethod(const frontend::Json& coverage, std::string_view method) {
        const auto found = std::find_if(coverage.at("methods").begin(), coverage.at("methods").end(), [method](const frontend::Json& row) {
            return row.at("method").get_ref<const std::string&>() == method;
        });
        if (found == coverage.at("methods").end()) {
            throw std::runtime_error("server differential coverage omits method " + std::string(method));
        }
        return *found;
    }

    const frontend::Json& fixtureMethod(const frontend::Json& fixture, std::string_view method) {
        const auto found = std::find_if(fixture.at("methods").begin(), fixture.at("methods").end(), [method](const frontend::Json& row) {
            return row.at("method").get_ref<const std::string&>() == method;
        });
        if (found == fixture.at("methods").end()) {
            throw std::runtime_error("generated server differential fixture omits method " + std::string(method));
        }
        return *found;
    }

    generated::DefinedCommand definedCommand(const frontend::Json& fixture,
                                               generated::MethodId method,
                                               std::string requestId) {
        const generated::MethodMetadata& metadata = generated::AllMethods[static_cast<std::size_t>(method)];
        return {std::move(requestId), generated::makeParameters(method, fixtureMethod(fixture, metadata.method).at("minimalParams"))};
    }

    struct OldObservations {
        std::vector<frontend::ServerMessage> messages;
        std::vector<std::string> closes;
        bool acceptMessages = true;
    };

    struct NewObservations {
        std::vector<frontend::ServerMessage> messages;
        std::vector<permanent::ConnectionClose> closes;
        bool acceptMessages = true;
    };

    frontend::FrontendConnectionCallbacks oldCallbacks(OldObservations& observations) {
        return {[&observations](const frontend::OutboundMessage& message) {
                    observations.messages.push_back(message.message);
                    return observations.acceptMessages;
                },
                [&observations](const std::string& reason) {
                    observations.closes.push_back(reason);
                }};
    }

    permanent::ConnectionCallbacks newCallbacks(NewObservations& observations) {
        return {[&observations](const frontend::ServerMessage& message) {
                    observations.messages.push_back(message);
                    return observations.acceptMessages;
                },
                [&observations](const permanent::ConnectionClose& close) {
                    observations.closes.push_back(close);
                }};
    }

    std::string messageKind(const frontend::ServerMessage& message) {
        return std::visit(
            []<typename Message>(const Message&) {
                if constexpr (std::is_same_v<Message, frontend::Welcome>) {
                    return std::string(frontend::kind::Welcome);
                } else if constexpr (std::is_same_v<Message, frontend::SyncComplete>) {
                    return std::string(frontend::kind::SyncComplete);
                } else if constexpr (std::is_same_v<Message, frontend::Snapshot>) {
                    return std::string(frontend::kind::Snapshot);
                } else if constexpr (std::is_same_v<Message, frontend::EventBatch>) {
                    return std::string(frontend::kind::Events);
                } else if constexpr (std::is_same_v<Message, frontend::Response>) {
                    return std::string(frontend::kind::Response);
                } else {
                    return std::string(frontend::kind::ProtocolError);
                }
            },
            message);
    }

    frontend::Json encodeServer(const frontend::ServerMessage& message) {
        const auto encoded = frontend::Codec::encodeServer(message);
        if (!encoded) {
            throw std::runtime_error("server differential could not encode " + messageKind(message) + ": " + encoded.error().message);
        }
        return encoded.value();
    }

    bool compareMessages(tests::support::TestResult& result,
                         const std::vector<frontend::ServerMessage>& oldMessages,
                         const std::vector<frontend::ServerMessage>& newMessages,
                         std::string_view identity) {
        if (oldMessages.size() != newMessages.size()) {
            result.expectTrue(false,
                              std::string(identity) + " message-count mismatch: old=" + std::to_string(oldMessages.size()) +
                                  " new=" + std::to_string(newMessages.size()));
            return false;
        }
        for (std::size_t index = 0; index < oldMessages.size(); ++index) {
            const frontend::Json oldEncoded = encodeServer(oldMessages[index]);
            const frontend::Json newEncoded = encodeServer(newMessages[index]);
            if (const auto mismatch = firstMismatch(oldEncoded, newEncoded)) {
                result.expectTrue(false,
                                  std::string(identity) + " mismatch at message " + std::to_string(index) + " (" +
                                      messageKind(oldMessages[index]) + ") path " + mismatch->path + ": old=" + mismatch->oldValue +
                                      " new=" + mismatch->newValue);
                return false;
            }
        }
        return true;
    }

    bool compareCloses(tests::support::TestResult& result,
                       const OldObservations& oldObservations,
                       const NewObservations& newObservations,
                       std::string_view identity) {
        if (oldObservations.closes.size() != newObservations.closes.size()) {
            result.expectTrue(false,
                              std::string(identity) + " close-count mismatch: old=" +
                                  std::to_string(oldObservations.closes.size()) + " new=" +
                                  std::to_string(newObservations.closes.size()));
            return false;
        }
        for (std::size_t index = 0; index < oldObservations.closes.size(); ++index) {
            if (oldObservations.closes[index] != newObservations.closes[index].reason) {
                result.expectTrue(false,
                                  std::string(identity) + " close-reason mismatch at " + std::to_string(index) + ": old=" +
                                      oldObservations.closes[index] + " new=" + newObservations.closes[index].reason);
                return false;
            }
        }
        return true;
    }

    const frontend::Response* responseAfter(const std::vector<frontend::ServerMessage>& messages,
                                            std::size_t baseline,
                                            std::string_view requestId) {
        for (std::size_t index = baseline; index < messages.size(); ++index) {
            if (const auto* response = std::get_if<frontend::Response>(&messages[index]);
                response != nullptr && response->requestId == requestId) {
                return response;
            }
        }
        return nullptr;
    }

    std::string messageSequenceSummary(const std::vector<frontend::ServerMessage>& messages) {
        std::string summary;
        for (std::size_t index = 0; index < messages.size(); ++index) {
            if (!summary.empty()) {
                summary += ";";
            }
            summary += std::to_string(index) + ":" + messageKind(messages[index]);
            if (const auto* batch = std::get_if<frontend::EventBatch>(&messages[index])) {
                summary += "[" + std::to_string(batch->fromSequence.value()) + "-" +
                           std::to_string(batch->toSequence.value()) + ":";
                for (std::size_t eventIndex = 0; eventIndex < batch->events.size(); ++eventIndex) {
                    if (eventIndex != 0) {
                        summary += ",";
                    }
                    summary += std::to_string(batch->events[eventIndex].sequence.value()) + "/" +
                               batch->events[eventIndex].type;
                }
                summary += "]";
            }
        }
        return summary;
    }

    struct PairSettings {
        frontend::FrontendPeerContext peer = trustedPeer();
        bool readyBackend = false;
        bool acceptMessages = true;
        std::size_t journalMaximumEntries = frontend::DefaultJournalMaxEntries;
        std::size_t journalMaximumBytes = frontend::DefaultJournalMaxBytes;
        std::uint64_t journalInitialSequence = 0;
        std::size_t maxDirtyEntities = frontend::DefaultMaxDirtyEntities;
        std::size_t maxOutboundMessages = frontend::DefaultFrontendServiceMaxOutboundMessages;
        std::size_t maxOutboundBytes = frontend::DefaultFrontendServiceMaxOutboundBytes;
        std::size_t maxMessagesPerDelivery = frontend::DefaultFrontendServiceMaxMessagesPerDelivery;
        std::size_t maxEventsPerBatch = frontend::DefaultBatchMaxEvents;
        std::size_t maxBatchBytes = frontend::DefaultBatchMaxBytes;
        std::size_t maximumInboundMessageBytes = 1024U * 1024U;
        std::size_t maxInboundMessagesPerSecond = 50;
        std::size_t maxInboundBurst = 100;
        std::size_t maxOutstandingCommands = 256;
        bool enableFilesystemRead = false;
        bool enableFilesystemWrite = false;
        bool enableCommandExecution = false;
        frontend::FrontendAuthenticator oldAuthenticator;
        permanent::Authenticator newAuthenticator;
        frontend::FrontendInvocationPolicy oldFilesystemReadPolicy;
        permanent::InvocationPolicy newFilesystemReadPolicy;
    };

    struct Pair {
        explicit Pair(PairSettings pairSettings = {})
            : settings(std::move(pairSettings))
            , transport(std::make_shared<tests::codex::FakeTransportState>())
            , oldBackend(oldBackendOptions(), transport) {
            if (settings.readyBackend) {
                tests::codex::installInitializingFake(
                    transport, [](const frontend::Json& message, const ai::openai::codex::detail::TransportCallbacks& callbacks) {
                        const auto method = message.find("method");
                        const auto id = message.find("id");
                        if (method != message.end() && method->is_string() && *method == "thread/list" && id != message.end()) {
                            tests::codex::inject(callbacks,
                                                 frontend::Json{{"id", *id},
                                                                {"result",
                                                                 {{"data", frontend::Json::array()},
                                                                  {"nextCursor", nullptr},
                                                                  {"backwardsCursor", nullptr}}}});
                        }
                    });
                oldBackend.start();
                oldScheduler.drain();
            } else {
                const auto projected = backendProjection.projectSnapshot(oldBackend.snapshot());
                if (!projected) {
                    throw std::runtime_error("stopped server differential snapshot projection failed at " +
                                             projected.error().path + ": " + projected.error().message);
                }
                newBackend.state = projected.value();
                initializeServers();
            }
        }

        void initializeServers() {
            if (oldServer || newServer) {
                return;
            }
            oldServer = std::make_unique<frontend::FrontendService>(oldBackend, oldOptions());
            newServer = std::make_unique<permanent::ServerCore>(newBackend, newOptions());
            newServer->start();

            if (settings.readyBackend) {
                capturedSubscription.emplace(oldBackend.subscribe(
                    backend::BackendObserverCallbacks{[this](const std::vector<backend::SequencedBackendEvent>& events) {
                                                          capturedEvents.insert(capturedEvents.end(), events.begin(), events.end());
                                                      },
                                                      [this](const backend::Snapshot&) {
                                                          capturedResynchronization = true;
                                                      }}));
            }

            oldObservations.acceptMessages = settings.acceptMessages;
            newObservations.acceptMessages = settings.acceptMessages;
            oldConnection = oldServer->openConnection(settings.peer, oldCallbacks(oldObservations));
            const std::optional<permanent::ConnectionIdentity> opened =
                newServer->openConnection(settings.peer, newCallbacks(newObservations));
            if (!oldConnection.isOpen() || !opened.has_value()) {
                throw std::runtime_error("server differential refused its first connection");
            }
            newConnection = *opened;
        }

        [[nodiscard]] bool advanceReadyInitialization() {
            if (!settings.readyBackend) {
                return oldServer && newServer;
            }
            oldScheduler.drain();
            if (oldServer && newServer) {
                return true;
            }
            if (!oldBackend.isReady()) {
                return false;
            }
            const auto projected = backendProjection.projectSnapshot(oldBackend.snapshot());
            if (!projected) {
                throw std::runtime_error("ready server differential snapshot projection failed at " + projected.error().path +
                                         ": " + projected.error().message);
            }
            newBackend.state = projected.value();
            newBackend.ready = true;
            newBackend.submitStatus = permanent::BackendSubmitStatus::Accepted;
            initializeServers();
            return true;
        }

        backend::BackendCoreOptions oldBackendOptions() {
            backend::BackendCoreOptions options;
            options.scheduler = [this](std::function<void()> callback) {
                oldScheduler.schedule(std::move(callback));
            };
            return options;
        }

        frontend::FrontendServiceOptions oldOptions() {
            frontend::FrontendServiceOptions options;
            options.scheduler = [this](std::function<void()> callback) {
                oldScheduler.schedule(std::move(callback));
            };
            options.timerScheduler = [](std::uint64_t, std::function<void()>) {
                return frontend::FrontendTimerCancellation{[] {}};
            };
            options.monotonicClockMs = [] {
                return std::uint64_t{1000};
            };
            options.trustedLocalUserId = TrustedUser;
            options.journal = {settings.journalMaximumEntries,
                               settings.journalMaximumBytes,
                               frontend::SequenceNumber{settings.journalInitialSequence}};
            options.coalescer = {settings.maxDirtyEntities};
            options.batches = {settings.maxEventsPerBatch, settings.maxBatchBytes};
            options.maxOutboundMessagesPerConnection = settings.maxOutboundMessages;
            options.maxOutboundBytesPerConnection = settings.maxOutboundBytes;
            options.maxMessagesPerDelivery = settings.maxMessagesPerDelivery;
            options.maximumInboundMessageBytes = settings.maximumInboundMessageBytes;
            options.maxInboundMessagesPerSecond = settings.maxInboundMessagesPerSecond;
            options.maxInboundBurst = settings.maxInboundBurst;
            options.maxOutstandingCommandsPerConnection = settings.maxOutstandingCommands;
            options.enableFilesystemReadMethods = settings.enableFilesystemRead;
            options.enableFilesystemWriteMethods = settings.enableFilesystemWrite;
            options.enableCommandExecutionMethods = settings.enableCommandExecution;
            options.authenticator = settings.oldAuthenticator;
            options.filesystemReadPolicy = settings.oldFilesystemReadPolicy;
            return options;
        }

        permanent::ServerCoreOptions newOptions() {
            permanent::ServerCoreOptions options;
            options.scheduler = [this](std::function<void()> callback) {
                newScheduler.schedule(std::move(callback));
            };
            options.timerScheduler = [](std::uint64_t, std::function<void()>) {
                return permanent::TimerCancellation{[] {}};
            };
            options.monotonicClockMs = [] {
                return std::uint64_t{1000};
            };
            options.trustedLocalUserId = TrustedUser;
            options.journalMaximumEntries = settings.journalMaximumEntries;
            options.journalMaximumBytes = settings.journalMaximumBytes;
            options.journalInitialSequence = model::FrontendSequence{settings.journalInitialSequence};
            options.maxDirtyEntities = settings.maxDirtyEntities;
            options.maxPendingDeliveryGroups = settings.maxDirtyEntities;
            options.maxOutboundMessagesPerConnection = settings.maxOutboundMessages;
            options.maxOutboundBytesPerConnection = settings.maxOutboundBytes;
            options.maxMessagesPerDelivery = settings.maxMessagesPerDelivery;
            options.maxEventsPerBatch = settings.maxEventsPerBatch;
            options.maxBatchBytes = settings.maxBatchBytes;
            options.maximumInboundMessageBytes = settings.maximumInboundMessageBytes;
            options.maxInboundMessagesPerSecond = settings.maxInboundMessagesPerSecond;
            options.maxInboundBurst = settings.maxInboundBurst;
            options.maxOutstandingCommandsPerConnection = settings.maxOutstandingCommands;
            options.enableFilesystemReadMethods = settings.enableFilesystemRead;
            options.enableFilesystemWriteMethods = settings.enableFilesystemWrite;
            options.enableCommandExecutionMethods = settings.enableCommandExecution;
            options.authenticator = settings.newAuthenticator;
            options.filesystemReadPolicy = settings.newFilesystemReadPolicy;
            return options;
        }

        void drain() {
            oldScheduler.drain();
            newScheduler.drain();
        }

        std::pair<frontend::ConnectionReceiveResult, permanent::ReceiveResult> receive(const frontend::ClientMessage& message) {
            refreshNewBackend();
            const frontend::ConnectionReceiveResult oldResult = oldConnection.receive(message);
            if (std::holds_alternative<frontend::Hello>(message)) {
                oldScheduler.drain();
                stageCapturedBackendOccurrences(oldBackend.snapshot());
            }
            const permanent::ReceiveResult newResult = newServer->receive(newConnection, message);
            drain();
            return {oldResult, newResult};
        }

        std::pair<frontend::ConnectionReceiveResult, permanent::ReceiveResult> receive(const frontend::Json& message) {
            refreshNewBackend();
            const frontend::ConnectionReceiveResult oldResult = oldConnection.receive(message);
            const permanent::ReceiveResult newResult = newServer->receive(newConnection, message);
            drain();
            return {oldResult, newResult};
        }

        std::pair<frontend::ConnectionReceiveResult, permanent::ReceiveResult> receive(std::string_view message) {
            refreshNewBackend();
            const frontend::ConnectionReceiveResult oldResult = oldConnection.receive(message);
            const permanent::ReceiveResult newResult = newServer->receive(newConnection, message);
            drain();
            return {oldResult, newResult};
        }

        std::pair<frontend::ConnectionReceiveResult, permanent::ReceiveResult>
        command(const generated::DefinedCommand& commandValue) {
            refreshNewBackend();
            const auto encoded = frontend::Codec::encodeDefinedCommand(commandValue);
            if (!encoded) {
                throw std::runtime_error("generated differential command failed protocol encoding");
            }
            const frontend::ConnectionReceiveResult oldResult = oldConnection.receive(encoded.value());
            const permanent::ReceiveResult newResult = newServer->receiveDefinedCommand(newConnection, commandValue);
            drain();
            return {oldResult, newResult};
        }

        void hello(std::vector<frontend::FrontendCapability> capabilities = {},
                   std::optional<frontend::SequenceNumber> resumeAfter = std::nullopt,
                   std::optional<frontend::AuthenticationCredential> authentication = std::nullopt) {
            frontend::Hello helloMessage;
            helloMessage.resumeAfter = resumeAfter;
            if (!capabilities.empty()) {
                helloMessage.capabilities = std::move(capabilities);
            }
            helloMessage.authentication = std::move(authentication);
            const auto received = receive(frontend::ClientMessage{std::move(helloMessage)});
            if (!received.first.accepted() || !received.second.accepted()) {
                throw std::runtime_error("server differential Hello was rejected (old status=" +
                                         std::to_string(static_cast<int>(received.first.status)) + ", new status=" +
                                         std::to_string(static_cast<int>(received.second.status)) + ", old messages=" +
                                         std::to_string(oldObservations.messages.size()) + ", new messages=" +
                                         std::to_string(newObservations.messages.size()) + ", new server open=" +
                                         std::to_string(newServer->isOpen()) + ", new connection open=" +
                                         std::to_string(newServer->connectionOpen(newConnection)) + ", new closes=" +
                                         std::to_string(newObservations.closes.size()) +
                                         (newObservations.closes.empty() ? std::string{} :
                                                                          ", new close reason=" +
                                                                              newObservations.closes.back().reason) +
                                         ")");
            }
        }

        void clearObservations() {
            oldObservations.messages.clear();
            oldObservations.closes.clear();
            newObservations.messages.clear();
            newObservations.closes.clear();
        }

        void refreshNewBackend() {
            const backend::Snapshot snapshot = oldBackend.snapshot();
            const auto projected = backendProjection.projectSnapshot(snapshot);
            if (!projected) {
                throw std::runtime_error("live server differential snapshot projection failed at " + projected.error().path +
                                         ": " + projected.error().message);
            }
            newBackend.state = projected.value();
            newBackend.ready = oldBackend.isReady();
            newBackend.submitStatus = newBackend.ready ? permanent::BackendSubmitStatus::Accepted
                                                       : permanent::BackendSubmitStatus::Unavailable;

            stageCapturedBackendOccurrences(snapshot);
        }

        void stageCapturedBackendOccurrences(const backend::Snapshot& snapshot) {
            std::vector<backend::SequencedBackendEvent> pending = std::move(capturedEvents);
            capturedEvents.clear();
            std::erase_if(pending, [](const backend::SequencedBackendEvent& event) {
                return std::holds_alternative<backend::SessionChanged>(event.event) ||
                       std::holds_alternative<backend::ControllerChanged>(event.event);
            });
            if (pending.empty()) {
                return;
            }
            const auto projectedSnapshot = backendProjection.projectSnapshot(snapshot);
            const auto projectedOccurrences = backendProjection.projectOccurrences(pending, snapshot);
            if (!projectedSnapshot || !projectedOccurrences) {
                throw std::runtime_error("pending server differential occurrence projection failed at " +
                                         (projectedOccurrences ? projectedSnapshot.error().path : projectedOccurrences.error().path));
            }
            for (const permanent::ProjectedBackendOccurrence& occurrence : projectedOccurrences.value().occurrences) {
                const permanent::OccurrenceStageResult staged =
                    newServer->stageGroup(occurrence.key, occurrence.occurrence, occurrence.urgency);
                if (!staged.accepted()) {
                    throw std::runtime_error("pending backend occurrence could not be staged in the permanent server");
                }
            }
            if (projectedOccurrences.value().snapshotRequired) {
                static_cast<void>(newServer->publishSnapshot(projectedSnapshot.value()));
            }
        }

        void beginBackendNotification(const frontend::Json& notification) {
            if (!settings.readyBackend || !capturedSubscription || !capturedSubscription->isOpen()) {
                throw std::runtime_error("backend notification injection requires the ready differential pair");
            }
            capturedEvents.clear();
            capturedResynchronization = false;
            transport->inject(notification);
        }

        [[nodiscard]] bool finishBackendNotification() {
            oldScheduler.drain();
            if (capturedResynchronization) {
                throw std::runtime_error("backend notification unexpectedly required observer resynchronization");
            }
            if (capturedEvents.empty()) {
                return false;
            }
            std::vector<backend::SequencedBackendEvent> events = std::move(capturedEvents);
            capturedEvents.clear();
            const backend::Snapshot snapshot = oldBackend.snapshot();
            const auto projectedSnapshot = backendProjection.projectSnapshot(snapshot);
            const auto projectedOccurrences = backendProjection.projectOccurrences(events, snapshot);
            if (!projectedSnapshot || !projectedOccurrences) {
                throw std::runtime_error("backend notification projection failed");
            }
            newBackend.state = projectedSnapshot.value();
            for (const permanent::ProjectedBackendOccurrence& occurrence : projectedOccurrences.value().occurrences) {
                const permanent::OccurrenceStageResult staged =
                    newServer->stageGroup(occurrence.key, occurrence.occurrence, occurrence.urgency);
                if (!staged.accepted()) {
                    throw std::runtime_error("backend notification occurrence could not be staged in the permanent server");
                }
            }
            if (projectedOccurrences.value().snapshotRequired) {
                static_cast<void>(newServer->publishSnapshot(projectedSnapshot.value()));
            }
            newScheduler.drain();
            return true;
        }

        PairSettings settings;
        std::shared_ptr<tests::codex::FakeTransportState> transport;
        ManualScheduler oldScheduler;
        ManualScheduler newScheduler;
        FakeBackendCore oldBackend;
        PermanentBackend newBackend;
        permanent::BackendProjection backendProjection;
        std::unique_ptr<frontend::FrontendService> oldServer;
        std::unique_ptr<permanent::ServerCore> newServer;
        OldObservations oldObservations;
        NewObservations newObservations;
        frontend::FrontendConnection oldConnection;
        permanent::ConnectionIdentity newConnection;
        std::optional<backend::BackendObserverSubscription> capturedSubscription;
        std::vector<backend::SequencedBackendEvent> capturedEvents;
        bool capturedResynchronization = false;
    };

    bool comparePair(tests::support::TestResult& result, const Pair& pair, std::string_view identity) {
        const bool messages = compareMessages(result, pair.oldObservations.messages, pair.newObservations.messages, identity);
        const bool closes = compareCloses(result, pair.oldObservations, pair.newObservations, identity);
        const bool open = pair.oldConnection.isOpen() == pair.newServer->connectionOpen(pair.newConnection);
        if (!open) {
            result.expectTrue(false, std::string(identity) + " connection-open mismatch");
        }
        return messages && closes && open;
    }

    frontend::AuthenticationResult observeOnlyAuthentication(const frontend::FrontendPeerContext&,
                                                              const frontend::AuthenticationCredential&) {
        return frontend::AuthenticationSuccess{frontend::FrontendPrincipal{
            "remote-observer", {frontend::FrontendScope::Observe}, "differential_observer", false}};
    }

    frontend::AuthenticationResult remoteControlAuthentication(const frontend::FrontendPeerContext&,
                                                                const frontend::AuthenticationCredential&) {
        return frontend::AuthenticationSuccess{frontend::FrontendPrincipal{
            "remote-controller",
            {frontend::FrontendScope::Observe, frontend::FrontendScope::Control},
            "differential_controller",
            false}};
    }

    frontend::AuthenticationCredential bearerCredential() {
        return frontend::BearerCredential{std::string(DifferentialBearer)};
    }

    bool sameReceiveDisposition(const frontend::ConnectionReceiveResult& oldResult, const permanent::ReceiveResult& newResult) {
        switch (oldResult.status) {
            case frontend::ConnectionReceiveStatus::Accepted:
                return newResult.status == permanent::ReceiveStatus::Accepted;
            case frontend::ConnectionReceiveStatus::Rejected:
                return newResult.status == permanent::ReceiveStatus::Rejected;
            case frontend::ConnectionReceiveStatus::Closing:
                return newResult.status == permanent::ReceiveStatus::Closing;
            case frontend::ConnectionReceiveStatus::Closed:
                return newResult.status == permanent::ReceiveStatus::Closed;
        }
        return false;
    }

    void testHandshakeAndProtocolFailures(tests::support::TestResult& result, const frontend::Json& fixture) {
        Pair early;
        const auto earlyReceive = early.command(definedCommand(fixture, generated::MethodId::SnapshotGet, "early"));
        result.expectTrue(sameReceiveDisposition(earlyReceive.first, earlyReceive.second) && comparePair(result, early, "pre-Hello command"),
                          "old and permanent servers apply the exact terminal pre-Hello command policy");

        Pair ready;
        ready.hello();
        const std::optional<model::SessionIdentity> newSession = ready.newServer->session(ready.newConnection);
        const bool readyParity = ready.oldConnection.helloComplete() && ready.newServer->helloComplete(ready.newConnection) &&
                                 ready.oldConnection.sessionId().has_value() && newSession.has_value() &&
                                 *ready.oldConnection.sessionId() == newSession->value() &&
                                 comparePair(result, ready, "legacy initial synchronization");
        result.expectTrue(readyParity,
                          "legacy Welcome/Snapshot/SyncComplete and session identity are byte-semantic equivalents");
        for (const std::string_view identity : {"message:hello:server",
                                                "message:hello:encode",
                                                "message:welcome:server",
                                                "message:welcome:encode",
                                                "message:snapshot:server",
                                                "message:snapshot:encode",
                                                "message:sync.complete:server",
                                                "message:sync.complete:encode",
                                                "representation:legacy-v1:server",
                                                "representation:legacy-v1:snapshot",
                                                "synchronization:snapshot:server",
                                                "synchronization:snapshot:sequence",
                                                "synchronization:snapshot:projection"}) {
            static_cast<void>(recordCase(std::string(identity), readyParity));
        }

        Pair malformed;
        const auto malformedReceive = malformed.receive(std::string_view{"{"});
        const bool malformedParity = sameReceiveDisposition(malformedReceive.first, malformedReceive.second) &&
                                     comparePair(result, malformed, "malformed JSON");
        result.expectTrue(malformedParity,
                          "malformed JSON produces the exact protocol error and close policy");
        static_cast<void>(recordCase("message:protocol.error:server", malformedParity));
        static_cast<void>(recordCase("message:protocol.error:encode", malformedParity));

        Pair wrongVersion;
        const frontend::Json wrongVersionHello{
            {"protocol", frontend::ProtocolIdentity}, {"version", 2}, {"kind", frontend::kind::Hello}};
        const auto wrongVersionReceive = wrongVersion.receive(wrongVersionHello);
        result.expectTrue(sameReceiveDisposition(wrongVersionReceive.first, wrongVersionReceive.second) &&
                              comparePair(result, wrongVersion, "unsupported version"),
                          "unsupported protocol versions produce the exact supported-version error and close policy");

        Pair duplicateHello;
        duplicateHello.hello();
        duplicateHello.clearObservations();
        const auto duplicateReceive = duplicateHello.receive(frontend::ClientMessage{frontend::Hello{}});
        result.expectTrue(sameReceiveDisposition(duplicateReceive.first, duplicateReceive.second) &&
                              comparePair(result, duplicateHello, "duplicate Hello"),
                          "a second Hello produces the exact terminal protocol failure");

        PairSettings authenticationSettings;
        authenticationSettings.peer = remotePeer();
        authenticationSettings.oldAuthenticator = [](const frontend::FrontendPeerContext&,
                                                       const frontend::AuthenticationCredential&) -> frontend::AuthenticationResult {
            return frontend::AuthenticationFailure{frontend::AuthenticationFailureCode::AuthenticationFailed};
        };
        authenticationSettings.newAuthenticator = authenticationSettings.oldAuthenticator;
        Pair authentication(std::move(authenticationSettings));
        frontend::Hello authenticatedHello;
        authenticatedHello.authentication = bearerCredential();
        const auto authenticationReceive = authentication.receive(frontend::ClientMessage{std::move(authenticatedHello)});
        result.expectTrue(sameReceiveDisposition(authenticationReceive.first, authenticationReceive.second) &&
                              comparePair(result, authentication, "authentication failure"),
                          "authentication failure limits expose the exact error without reflecting credentials");
    }

    void testAllGeneratedMethods(tests::support::TestResult& result,
                                 const frontend::Json& fixture,
                                 const frontend::Json& coverage) {
        std::size_t nativeCount = 0;
        std::size_t providerCount = 0;
        std::size_t reverseCount = 0;
        for (const generated::MethodMetadata& metadata : generated::AllMethods) {
            nativeCount += metadata.frontendNative ? 1U : 0U;
            providerCount += metadata.category == generated::MethodCategory::ProviderOperation ? 1U : 0U;
            reverseCount += metadata.category == generated::MethodCategory::ReverseResponse ? 1U : 0U;

            // Each authority member receives a fresh physical/server
            // generation. Lifecycle methods therefore cannot perturb the
            // readiness classification of later cases.
            Pair pair;
            pair.hello();
            pair.clearObservations();
            bool controllerSetupParity = true;
            if (metadata.controllerRequired) {
                const auto acquire =
                    pair.command(definedCommand(fixture, generated::MethodId::ControllerAcquire, "method-controller-setup"));
                const frontend::Response* oldAcquire =
                    responseAfter(pair.oldObservations.messages, 0, "method-controller-setup");
                const frontend::Response* newAcquire =
                    responseAfter(pair.newObservations.messages, 0, "method-controller-setup");
                if (!sameReceiveDisposition(acquire.first, acquire.second) || oldAcquire == nullptr || newAcquire == nullptr ||
                    !oldAcquire->ok || *oldAcquire != *newAcquire) {
                    result.expectTrue(false, "controller setup failed for generated method " + std::string(metadata.method));
                    controllerSetupParity = false;
                }
                pair.clearObservations();
            }
            const std::string requestId = "method-" + std::to_string(static_cast<std::size_t>(metadata.id));
            const auto receive = pair.command(definedCommand(fixture, metadata.id, requestId));
            const frontend::Response* oldResponse = responseAfter(pair.oldObservations.messages, 0, requestId);
            const frontend::Response* newResponse = responseAfter(pair.newObservations.messages, 0, requestId);
            const bool responseParity = oldResponse != nullptr && newResponse != nullptr
                                            ? *oldResponse == *newResponse
                                            : !metadata.defaultEnabled && oldResponse == nullptr && newResponse == nullptr;
            const bool borderParity =
                comparePair(result, pair, std::string("generated method ") + std::string(metadata.method));
            const bool methodParity = controllerSetupParity && sameReceiveDisposition(receive.first, receive.second) && responseParity &&
                                      borderParity;
            result.expectTrue(methodParity,
                              "exact server dispatch/error/close parity for " + std::string(metadata.method) + " status=" +
                                  std::to_string(static_cast<int>(receive.first.status)) + "/" +
                                  std::to_string(static_cast<int>(receive.second.status)) + " response=" +
                                  std::to_string(oldResponse != nullptr) + "/" + std::to_string(newResponse != nullptr) +
                                  " responseParity=" + std::to_string(responseParity) +
                                  " borderParity=" + std::to_string(borderParity));
            const std::string coverageId = coverageMethod(coverage, metadata.method).at("id").get<std::string>();
            for (const std::string_view dimension : {"server-dispatch", "parameter-schema", "security", "readiness", "controller"}) {
                static_cast<void>(recordCase(coverageCase("method", coverageId, dimension), methodParity));
            }
            static_cast<void>(recordCase("message:command:server", methodParity));
            static_cast<void>(recordCase("message:command:encode", methodParity));
            static_cast<void>(recordCase("message:response:server", methodParity));
            static_cast<void>(recordCase("message:response:encode", methodParity));
        }

        result.expectTrue(generated::AllMethods.size() == 105 && nativeCount == 7 && providerCount == 86 && reverseCount == 12,
                          "authority-driven server differential executes exact 105/7/86/12 method split");
    }

    void testSnapshotLiveReplayRepresentations(tests::support::TestResult& result, const frontend::Json& fixture) {
        const std::vector expandedCapabilities{frontend::FrontendCapability::CompleteBackendDomains,
                                               frontend::FrontendCapability::CompleteThreadItems,
                                               frontend::FrontendCapability::DedicatedPendingRequests,
                                               frontend::FrontendCapability::DedicatedNotificationEvents,
                                               frontend::FrontendCapability::ScopeProjectedState};

        Pair legacy;
        legacy.hello();
        const bool legacyInitial = comparePair(result, legacy, "legacy snapshot synchronization");
        const auto legacySnapshot = std::find_if(legacy.oldObservations.messages.begin(),
                                                 legacy.oldObservations.messages.end(),
                                                 [](const frontend::ServerMessage& message) {
                                                     return std::holds_alternative<frontend::Snapshot>(message);
                                                 });
        result.expectTrue(legacyInitial && legacySnapshot != legacy.oldObservations.messages.end() &&
                              std::get<frontend::Snapshot>(*legacySnapshot).state.contains("backendRevision") &&
                              !std::get<frontend::Snapshot>(*legacySnapshot).state.contains("provider"),
                          "legacy v1 snapshot representation is compared without normalization");
        static_cast<void>(recordCase("representation:legacy-v1:live", legacyInitial));

        OldObservations legacyReplayOld;
        NewObservations legacyReplayNew;
        frontend::FrontendConnection legacyReplayOldConnection =
            legacy.oldServer->openConnection(trustedPeer(), oldCallbacks(legacyReplayOld));
        const auto legacyReplayNewConnection = legacy.newServer->openConnection(trustedPeer(), newCallbacks(legacyReplayNew));
        frontend::Hello legacyReplayHello;
        legacyReplayHello.resumeAfter = frontend::SequenceNumber{0};
        const auto legacyOldReceive = legacyReplayOldConnection.receive(frontend::ClientMessage{legacyReplayHello});
        const auto legacyNewReceive =
            legacy.newServer->receive(*legacyReplayNewConnection, frontend::ClientMessage{std::move(legacyReplayHello)});
        legacy.drain();
        const frontend::Welcome* legacyReplayWelcome =
            !legacyReplayOld.messages.empty() ? std::get_if<frontend::Welcome>(&legacyReplayOld.messages.front()) : nullptr;
        const bool legacyReplayParity = sameReceiveDisposition(legacyOldReceive, legacyNewReceive) && legacyReplayWelcome &&
                                        legacyReplayWelcome->syncMode == frontend::SyncMode::Replay &&
                                        compareMessages(result,
                                                        legacyReplayOld.messages,
                                                        legacyReplayNew.messages,
                                                        "legacy reconnect replay") &&
                                        compareCloses(result, legacyReplayOld, legacyReplayNew, "legacy reconnect replay");
        result.expectTrue(legacyReplayParity, "legacy initial replay retains exact sequence and batch boundaries");
        static_cast<void>(recordCase("representation:legacy-v1:replay", legacyReplayParity));

        Pair expanded;
        expanded.hello(expandedCapabilities);
        const bool expandedInitial = comparePair(result, expanded, "expanded snapshot synchronization");
        const auto expandedSnapshot = std::find_if(expanded.oldObservations.messages.begin(),
                                                   expanded.oldObservations.messages.end(),
                                                   [](const frontend::ServerMessage& message) {
                                                       return std::holds_alternative<frontend::Snapshot>(message);
                                                   });
        bool expandedDecodes = false;
        if (expandedSnapshot != expanded.oldObservations.messages.end()) {
            const frontend::Snapshot& snapshot = std::get<frontend::Snapshot>(*expandedSnapshot);
            const frontend::Json envelope = encodeServer(frontend::ServerMessage{snapshot});
            expandedDecodes = frontend::Codec::decodeExpandedSnapshot(envelope).hasValue();
        }
        result.expectTrue(expandedInitial && expandedDecodes,
                          "expanded v1 snapshot representation is exact and schema-decodes on the shared protocol boundary");
        static_cast<void>(recordCase("representation:expanded-v1:server", expandedInitial && expandedDecodes));
        static_cast<void>(recordCase("representation:expanded-v1:snapshot", expandedInitial && expandedDecodes));

        expanded.clearObservations();
        const std::uint64_t replayAnchor = expanded.oldServer->currentSequence().value();
        const auto acquire = expanded.command(definedCommand(fixture, generated::MethodId::ControllerAcquire, "expanded-acquire"));
        const bool liveParity = sameReceiveDisposition(acquire.first, acquire.second) &&
                                comparePair(result, expanded, "expanded controller live delivery");
        bool hasExpandedController = false;
        for (const frontend::ServerMessage& message : expanded.oldObservations.messages) {
            if (const auto* batch = std::get_if<frontend::EventBatch>(&message)) {
                hasExpandedController = hasExpandedController ||
                                        std::any_of(batch->events.begin(), batch->events.end(), [](const frontend::FrontendEvent& event) {
                                            return event.type == "controller.updated";
                                        });
            }
        }
        result.expectTrue(liveParity && hasExpandedController,
                          "expanded controller occurrence uses the same live sequence and projected event content");
        static_cast<void>(recordCase("representation:expanded-v1:live", liveParity && hasExpandedController));
        static_cast<void>(recordCase("message:events:server", liveParity && hasExpandedController));
        static_cast<void>(recordCase("message:events:encode", liveParity && hasExpandedController));

        OldObservations replayOld;
        NewObservations replayNew;
        frontend::FrontendConnection replayOldConnection =
            expanded.oldServer->openConnection(trustedPeer(), oldCallbacks(replayOld));
        const auto replayNewConnection = expanded.newServer->openConnection(trustedPeer(), newCallbacks(replayNew));
        frontend::Hello replayHello;
        replayHello.resumeAfter = frontend::SequenceNumber{replayAnchor};
        replayHello.capabilities = expandedCapabilities;
        const frontend::ConnectionReceiveResult oldReplayReceive = replayOldConnection.receive(frontend::ClientMessage{replayHello});
        const permanent::ReceiveResult newReplayReceive =
            expanded.newServer->receive(*replayNewConnection, frontend::ClientMessage{std::move(replayHello)});
        expanded.drain();
        const bool replayMessages = compareMessages(result, replayOld.messages, replayNew.messages, "expanded reconnect replay");
        const bool replayCloses = compareCloses(result, replayOld, replayNew, "expanded reconnect replay");
        const auto replayWelcome = !replayOld.messages.empty() ? std::get_if<frontend::Welcome>(&replayOld.messages.front()) : nullptr;
        const bool replayComplete = std::any_of(replayOld.messages.begin(), replayOld.messages.end(), [](const frontend::ServerMessage& message) {
            return std::holds_alternative<frontend::SyncComplete>(message);
        });
        const bool replayParity = sameReceiveDisposition(oldReplayReceive, newReplayReceive) && replayMessages && replayCloses &&
                                  replayWelcome && replayWelcome->syncMode == frontend::SyncMode::Replay && replayComplete;
        result.expectTrue(replayParity,
                          "expanded initial replay uses the exact retained live occurrence, batch boundary, and SyncComplete cursor");
        for (const std::string_view identity : {"representation:expanded-v1:replay",
                                                "synchronization:replay:server",
                                                "synchronization:replay:sequence",
                                                "synchronization:replay:projection"}) {
            static_cast<void>(recordCase(std::string(identity), replayParity));
        }

        expanded.clearObservations();
        const auto snapshot = expanded.command(definedCommand(fixture, generated::MethodId::SnapshotGet, "explicit-snapshot"));
        result.expectTrue(sameReceiveDisposition(snapshot.first, snapshot.second) &&
                              comparePair(result, expanded, "explicit live Snapshot"),
                          "explicit snapshot.get emits the exact live Snapshot and response ordering");
    }

    class ReadyBackendDifferentialRunner {
    public:
        ReadyBackendDifferentialRunner(tests::support::TestResult& result, const frontend::Json& fixture)
            : result(result)
            , fixture(fixture)
            , expanded(expandedSettings())
            , legacy(readySettings()) {
        }

        void start() {
            scheduleAdvance();
        }

        [[nodiscard]] bool isFinished() const noexcept {
            return finished;
        }

        [[nodiscard]] const std::string& waitingStage() const noexcept {
            return stage;
        }

    private:
        static PairSettings readySettings() {
            PairSettings settings;
            settings.readyBackend = true;
            return settings;
        }

        static PairSettings expandedSettings() {
            PairSettings settings = readySettings();
            settings.maxOutstandingCommands = 1;
            return settings;
        }

        static std::vector<frontend::FrontendCapability> expandedCapabilities() {
            return {frontend::FrontendCapability::CompleteBackendDomains,
                    frontend::FrontendCapability::DedicatedNotificationEvents,
                    frontend::FrontendCapability::ScopeProjectedState};
        }

        static frontend::Json warningNotification() {
            return {{"method", "configWarning"},
                    {"params",
                     {{"summary", "differential configuration warning"},
                      {"details", "bounded detail"},
                      {"path", "/workspace/config.toml"},
                      {"range", nullptr}}}};
        }

        static frontend::Json approvalNotification(std::string id, std::string itemId, std::int64_t startedAtMs, std::string command) {
            return {{"method", "item/commandExecution/requestApproval"},
                    {"id", std::move(id)},
                    {"params",
                     {{"threadId", "pending-thread"},
                      {"turnId", "pending-turn"},
                      {"itemId", std::move(itemId)},
                      {"startedAtMs", startedAtMs},
                      {"command", std::move(command)},
                      {"cwd", "/workspace"}}}};
        }

        void scheduleAdvance() {
            core::EventReceiver::atNextTick([this]() {
                advance();
            });
        }

        void advance() noexcept {
            if (finished) {
                return;
            }
            try {
                if (remainingTicks == 0) {
                    throw std::runtime_error("ready-backend differential did not quiesce at " + stage);
                }
                --remainingTicks;
                switch (step) {
                    case Step::WaitForReady:
                        stage = "waiting for deterministic fake providers";
                        if (!expanded.advanceReadyInitialization() || !legacy.advanceReadyInitialization()) {
                            scheduleAdvance();
                            return;
                        }
                        beginExpandedOccurrence();
                        return;
                    case Step::WaitForExpandedOccurrence:
                        stage = "waiting for expanded configWarning normalization";
                        if (!expanded.finishBackendNotification()) {
                            scheduleAdvance();
                            return;
                        }
                        verifyExpandedOccurrence();
                        beginLegacyOccurrence();
                        return;
                    case Step::WaitForLegacyOccurrence:
                        stage = "waiting for legacy configWarning normalization";
                        if (!legacy.finishBackendNotification()) {
                            scheduleAdvance();
                            return;
                        }
                        verifyLegacyOccurrence();
                        beginFirstPendingOccurrence();
                        return;
                    case Step::WaitForFirstPendingOccurrence:
                        stage = "waiting for first unresolved approval";
                        if (!legacy.finishBackendNotification()) {
                            scheduleAdvance();
                            return;
                        }
                        verifyFirstPendingOccurrence();
                        beginSecondPendingOccurrence();
                        return;
                    case Step::WaitForSecondPendingOccurrence:
                        stage = "waiting for second unresolved approval";
                        if (!legacy.finishBackendNotification()) {
                            scheduleAdvance();
                            return;
                        }
                        verifySecondPendingOccurrence();
                        beginUnknownOccurrence();
                        return;
                    case Step::WaitForUnknownOccurrence:
                        stage = "waiting for unknown-extension containment";
                        if (!legacy.finishBackendNotification()) {
                            scheduleAdvance();
                            return;
                        }
                        verifyUnknownOccurrence();
                        finish();
                        return;
                }
            } catch (const std::exception& error) {
                result.expectTrue(false, std::string("ready-backend server differential failure: ") + error.what());
                finish();
            } catch (...) {
                result.expectTrue(false, "ready-backend server differential failure: unknown exception");
                finish();
            }
        }

        void beginExpandedOccurrence() {
            stage = "expanded ready snapshot";
            expanded.hello(expandedCapabilities());
            result.expectTrue(comparePair(result, expanded, "ready expanded snapshot"),
                              "ready backend snapshot has exact expanded old/new projection");
            expanded.clearObservations();
            expandedReplayAnchor = expanded.oldServer->currentSequence().value();
            expanded.beginBackendNotification(warningNotification());
            step = Step::WaitForExpandedOccurrence;
            scheduleAdvance();
        }

        void verifyExpandedOccurrence() {
            const bool expandedParity = comparePair(result, expanded, "configWarning expanded occurrence");
            const frontend::EventBatch* expandedBatch = nullptr;
            for (const frontend::ServerMessage& message : expanded.oldObservations.messages) {
                if (const auto* batch = std::get_if<frontend::EventBatch>(&message)) {
                    expandedBatch = batch;
                }
            }
            bool equalSequenceGroup = false;
            if (expandedBatch) {
                for (std::size_t index = 1; index < expandedBatch->events.size(); ++index) {
                    equalSequenceGroup = equalSequenceGroup ||
                                         (expandedBatch->events[index - 1].type == "configuration.updated" &&
                                          expandedBatch->events[index].type == "notice.added" &&
                                          expandedBatch->events[index - 1].sequence == expandedBatch->events[index].sequence);
                }
            }
            result.expectTrue(expandedParity && equalSequenceGroup,
                              "one backend configWarning expands to the exact two-family equal-sequence canonical occurrence group; old=" +
                                  messageSequenceSummary(expanded.oldObservations.messages) + "; new=" +
                                  messageSequenceSummary(expanded.newObservations.messages));

            OldObservations replayOld;
            NewObservations replayNew;
            frontend::FrontendConnection oldReplay = expanded.oldServer->openConnection(trustedPeer(), oldCallbacks(replayOld));
            const auto newReplay = expanded.newServer->openConnection(trustedPeer(), newCallbacks(replayNew));
            frontend::Hello replayHello;
            replayHello.resumeAfter = frontend::SequenceNumber{expandedReplayAnchor};
            replayHello.capabilities = expandedCapabilities();
            const auto oldReplayReceive = oldReplay.receive(frontend::ClientMessage{replayHello});
            const auto newReplayReceive = expanded.newServer->receive(*newReplay, frontend::ClientMessage{std::move(replayHello)});
            expanded.drain();
            result.expectTrue(sameReceiveDisposition(oldReplayReceive, newReplayReceive) &&
                                  compareMessages(result, replayOld.messages, replayNew.messages, "configWarning expanded replay") &&
                                  compareCloses(result, replayOld, replayNew, "configWarning expanded replay"),
                              "expanded replay reuses the exact live two-family equal-sequence group");
            oldReplay.close("differential replay complete");
            expanded.newServer->closeConnection(*newReplay, "differential replay complete");
            expanded.drain();

            expanded.clearObservations();
            const auto first =
                expanded.command(definedCommand(fixture, generated::MethodId::ThreadList, "outstanding-first"));
            const bool firstPending = sameReceiveDisposition(first.first, first.second) &&
                                      expanded.oldObservations.messages.empty() && expanded.newObservations.messages.empty();
            const auto second =
                expanded.command(definedCommand(fixture, generated::MethodId::ThreadList, "outstanding-second"));
            result.expectTrue(firstPending && sameReceiveDisposition(second.first, second.second) &&
                                  comparePair(result, expanded, "outstanding command bound"),
                              "provider commands remain pending identically and the next command receives the exact capacity error; first=" +
                                  std::to_string(static_cast<int>(first.first.status)) + "/" +
                                  std::to_string(static_cast<int>(first.second.status)) + "; second=" +
                                  std::to_string(static_cast<int>(second.first.status)) + "/" +
                                  std::to_string(static_cast<int>(second.second.status)) + "; old=" +
                                  messageSequenceSummary(expanded.oldObservations.messages) + "; new=" +
                                  messageSequenceSummary(expanded.newObservations.messages));
        }

        void beginLegacyOccurrence() {
            stage = "legacy ready snapshot";
            legacy.hello();
            const bool readyParity = comparePair(result, legacy, "ready legacy snapshot");
            result.expectTrue(readyParity,
                              "ready backend snapshot has exact legacy old/new projection; old=" +
                                  messageSequenceSummary(legacy.oldObservations.messages) + "; new=" +
                                  messageSequenceSummary(legacy.newObservations.messages));
            legacy.clearObservations();
            legacy.beginBackendNotification(warningNotification());
            step = Step::WaitForLegacyOccurrence;
            scheduleAdvance();
        }

        void verifyLegacyOccurrence() {
            const bool legacyParity = comparePair(result, legacy, "configWarning legacy occurrence");
            const frontend::EventBatch* legacyBatch = nullptr;
            for (const frontend::ServerMessage& message : legacy.oldObservations.messages) {
                if (const auto* batch = std::get_if<frontend::EventBatch>(&message)) {
                    legacyBatch = batch;
                }
            }
            result.expectTrue(legacyParity && legacyBatch && legacyBatch->events.size() == 1 &&
                                  legacyBatch->events.front().type == "codex.extension",
                              "the same canonical configWarning occurrence has one exact legacy compatibility encoding");
        }

        void beginFirstPendingOccurrence() {
            stage = "first unresolved approval injection";
            legacy.clearObservations();
            legacy.beginBackendNotification(approvalNotification("pending-one", "pending-item-one", 1, "printf one"));
            step = Step::WaitForFirstPendingOccurrence;
            scheduleAdvance();
        }

        void verifyFirstPendingOccurrence() {
            const backend::Snapshot snapshot = legacy.oldBackend.snapshot();
            if (snapshot.pendingRequests.size() != 1) {
                throw std::runtime_error("first approval did not create exactly one pending request");
            }
            firstPendingId = snapshot.pendingRequests.front().id;
            result.expectTrue(comparePair(result, legacy, "first unresolved approval"),
                              "first unresolved approval has exact old/new legacy delivery");
        }

        void beginSecondPendingOccurrence() {
            stage = "second unresolved approval injection";
            legacy.clearObservations();
            legacy.beginBackendNotification(approvalNotification("pending-two", "pending-item-two", 2, "printf two"));
            step = Step::WaitForSecondPendingOccurrence;
            scheduleAdvance();
        }

        void verifySecondPendingOccurrence() {
            const bool parity = comparePair(result, legacy, "second unresolved approval with retained first request");
            const backend::Snapshot snapshot = legacy.oldBackend.snapshot();
            if (!firstPendingId.has_value() || snapshot.pendingRequests.size() != 2) {
                throw std::runtime_error("second approval did not retain exactly two pending requests");
            }
            const auto second = std::find_if(
                snapshot.pendingRequests.begin(), snapshot.pendingRequests.end(), [&](const backend::PendingRequestSnapshot& request) {
                    return request.id != *firstPendingId;
                });
            if (second == snapshot.pendingRequests.end()) {
                throw std::runtime_error("second approval did not receive a distinct pending-request identity");
            }
            std::size_t pendingEventCount = 0;
            bool selectedSecond = false;
            for (const frontend::ServerMessage& message : legacy.oldObservations.messages) {
                const auto* batch = std::get_if<frontend::EventBatch>(&message);
                if (batch == nullptr) {
                    continue;
                }
                for (const frontend::FrontendEvent& event : batch->events) {
                    if (event.type == "request.pending") {
                        ++pendingEventCount;
                        selectedSecond = event.data.at("request").at("id") == std::to_string(second->id.value());
                    }
                }
            }
            result.expectTrue(parity && pendingEventCount == 1 && selectedSecond,
                              "the second unresolved approval retains the full typed projection and selects its exact legacy request");
        }

        void beginUnknownOccurrence() {
            stage = "unknown extension injection";
            legacy.clearObservations();
            legacy.beginBackendNotification(
                frontend::Json{{"method", "future/safe-extension"},
                               {"params",
                                {{"safe", "visible"},
                                 {"accessToken", std::string(Secret)},
                                 {"nested", {{"clientSecret", std::string(Secret)}}}}}});
            step = Step::WaitForUnknownOccurrence;
            scheduleAdvance();
        }

        void verifyUnknownOccurrence() {
            const bool unknownParity = comparePair(result, legacy, "unknown extension information ceiling");
            bool secretAbsent = true;
            bool redactionVisible = false;
            for (const frontend::ServerMessage& message : legacy.oldObservations.messages) {
                const std::string encoded = encodeServer(message).dump();
                secretAbsent = secretAbsent && encoded.find(Secret) == std::string::npos;
                redactionVisible = redactionVisible || encoded.find("sensitiveFieldsRedacted") != std::string::npos;
            }
            result.expectTrue(unknownParity && secretAbsent && redactionVisible,
                              "unknown safe extensions preserve exact recursive redaction and never expose a secret value");
        }

        void finish() noexcept {
            if (finished) {
                return;
            }
            finished = true;
            stage = "complete";
            expanded.oldBackend.stop();
            legacy.oldBackend.stop();
            core::SNodeC::stop();
        }

        enum class Step {
            WaitForReady,
            WaitForExpandedOccurrence,
            WaitForLegacyOccurrence,
            WaitForFirstPendingOccurrence,
            WaitForSecondPendingOccurrence,
            WaitForUnknownOccurrence
        };

        static constexpr std::string_view Secret = "P2_DIFFERENTIAL_SECRET_MUST_NOT_ESCAPE";
        tests::support::TestResult& result;
        const frontend::Json& fixture;
        Pair expanded;
        Pair legacy;
        Step step = Step::WaitForReady;
        std::size_t remainingTicks = 10'000;
        std::uint64_t expandedReplayAnchor = 0;
        std::optional<backend::PendingRequestId> firstPendingId;
        bool finished = false;
        std::string stage = "constructed";
    };

    legacy::FrontendProjectionContext localProjectionContext(std::vector<frontend::FrontendCapability> capabilities) {
        legacy::FrontendProjectionContext context;
        context.scopes.assign(frontend::LocalTrustedScopes.begin(), frontend::LocalTrustedScopes.end());
        context.capabilities = std::move(capabilities);
        return context;
    }

    backend::Snapshot behavioralBackendSnapshot() {
        backend::Snapshot snapshot;
        snapshot.provider.lifecycle = backend::ProviderLifecycle::Ready;
        snapshot.provider.generation = 9;
        snapshot.provider.desiredRunning = true;
        snapshot.provider.recovery.status = backend::RecoveryStatus::Idle;
        snapshot.controller = backend::SessionId{1};
        snapshot.sessions.push_back({backend::SessionId{1}, backend::SessionRole::Controller});
        snapshot.threadList.hasLoadedPage = true;
        snapshot.threadList.complete = false;
        snapshot.threadList.pagesLoaded = 1;
        snapshot.threadList.nextCursor = "next-thread-page";
        snapshot.threadList.backwardsCursor = "previous-thread-page";
        snapshot.threadList.stamp = {9, backend::Freshness::Current};

        backend::ItemSnapshot item;
        item.id = "item-1";
        item.type = "commandExecution";
        item.status = "completed";
        item.agentText = "bounded agent text";
        item.reasoningText = "bounded reasoning text";
        item.reasoningSummary = "bounded reasoning summary";
        item.commandOutput = "bounded command output";
        item.data = {{"command", "printf safe"}, {"cwd", "/worktree"}};
        item.stamp = {9, backend::Freshness::Current};

        backend::TurnSnapshot turn;
        turn.id = "turn-1";
        turn.threadId = "thread-1";
        turn.status = "completed";
        turn.terminal = true;
        turn.items.push_back(std::move(item));
        turn.stamp = {9, backend::Freshness::Current};

        backend::ThreadSnapshot thread;
        thread.id = "thread-1";
        thread.title = "Differential projection thread";
        thread.cwd = "/worktree";
        thread.fullyLoaded = true;
        thread.turns.push_back(std::move(turn));
        thread.stamp = {9, backend::Freshness::Current};
        snapshot.threads.push_back(std::move(thread));

        backend::PendingRequestSnapshot pending;
        pending.id = backend::PendingRequestId{1};
        pending.type = "command_approval";
        pending.threadId = "thread-1";
        pending.turnId = "turn-1";
        pending.itemId = "item-1";
        pending.details = {{"summary", "bounded approval summary"}};
        snapshot.pendingRequests.push_back(std::move(pending));

        backend::ProcessSnapshot process;
        process.processHandle = "process-1";
        process.lifecycle = "running";
        process.stdoutBytes = 7;
        process.stamp = {9, backend::Freshness::Current};
        snapshot.processes.push_back(std::move(process));

        backend::FilesystemWatchSnapshot watch;
        watch.watchId = "watch-1";
        watch.root = "/worktree";
        watch.changedPathCount = 1;
        watch.stamp = {9, backend::Freshness::Current};
        snapshot.filesystemWatches.push_back(std::move(watch));

        backend::FuzzySearchSnapshot search;
        search.sessionId = "search-1";
        search.resultCount = 1;
        search.complete = true;
        search.stamp = {9, backend::Freshness::Current};
        snapshot.fuzzySearchSessions.push_back(std::move(search));

        backend::NoticeSnapshot notice;
        notice.occurrence = 1;
        notice.category = backend::NoticeCategory::Warning;
        notice.summary = "bounded notice";
        notice.stamp = {9, backend::Freshness::Current};
        snapshot.notices.push_back(std::move(notice));

        backend::ActivitySnapshot activity;
        activity.key = "activity-1";
        activity.subjectId = "subject-1";
        activity.kind = "tool";
        activity.lifecycle = "running";
        activity.active = true;
        activity.stamp = {9, backend::Freshness::Current};
        snapshot.activities.push_back(std::move(activity));

        snapshot.diagnostics.received = 1;
        snapshot.diagnostics.recent.push_back("bounded diagnostic");
        return snapshot;
    }

    frontend::Json behavioralNotificationParameters() {
        frontend::Json params{{"threadId", "thread-1"},
                              {"turnId", "turn-1"},
                              {"itemId", "item-1"},
                              {"processHandle", "process-1"},
                              {"processId", "process-1"},
                              {"watchId", "watch-1"},
                              {"sessionId", "search-1"},
                              {"summary", "bounded projected notice"},
                              {"message", "bounded projected warning"},
                              {"details", "bounded projected details"},
                              {"failedScan", false},
                              {"samplePaths", frontend::Json::array()},
                              {"extraCount", 0}};
        params["thread"] = frontend::Json{{"id", "thread-1"}};
        params["turn"] = frontend::Json{{"id", "turn-1"}, {"threadId", "thread-1"}};
        params["item"] = frontend::Json{{"id", "item-1"}};
        return params;
    }

    const frontend::Snapshot* firstSnapshot(const std::vector<frontend::ServerMessage>& messages) {
        const auto found = std::find_if(messages.begin(), messages.end(), [](const frontend::ServerMessage& message) {
            return std::holds_alternative<frontend::Snapshot>(message);
        });
        return found == messages.end() ? nullptr : std::get_if<frontend::Snapshot>(&*found);
    }

    std::string requiredProjectedString(const frontend::Json& value, std::string_view member, std::string_view path) {
        const auto found = value.find(member);
        if (found == value.end() || !found->is_string() || found->get_ref<const std::string&>().empty()) {
            throw std::runtime_error("old behavioral projection requires a non-empty string at " + std::string(path));
        }
        return found->get<std::string>();
    }

    frontend::Json legacyPendingRequestFromOldProjection(const frontend::Json& expanded) {
        const std::string id = requiredProjectedString(expanded, "pendingRequestId", "/pendingRequest/pendingRequestId");
        const std::string kind = requiredProjectedString(expanded, "kind", "/pendingRequest/kind");
        std::string legacyKind = "unknown";
        if (kind == "command_execution_approval") {
            legacyKind = "command_approval";
        } else if (kind == "file_change_approval") {
            legacyKind = "file_change_approval";
        } else if (kind == "user_input") {
            legacyKind = "user_input";
        } else if (kind == "authentication") {
            legacyKind = "authentication";
        }
        frontend::Json result{{"id", id},
                              {"type", std::move(legacyKind)},
                              {"details", expanded.value("details", frontend::Json::object())}};
        for (std::string_view member : {"threadId", "turnId", "itemId"}) {
            if (const auto found = expanded.find(member); found != expanded.end()) {
                result[std::string(member)] = *found;
            }
        }
        return result;
    }

    bool metadataOnlyLegacyItem(std::string_view type) {
        return type == "collabAgentToolCall" || type == "contextCompaction" || type == "enteredReviewMode" ||
               type == "exitedReviewMode" || type == "hookPrompt" || type == "imageGeneration" || type == "imageView" ||
               type == "plan" || type == "sleep" || type == "subAgentActivity";
    }

    frontend::Json legacyItemFromOldProjection(const frontend::Json& expanded) {
        const std::string type = requiredProjectedString(expanded, "type", "/item/type");
        frontend::Json data = expanded.value("data", frontend::Json::object());
        if (metadataOnlyLegacyItem(type)) {
            data = frontend::Json{{"codexType", type}};
        }
        frontend::Json result{{"id", requiredProjectedString(expanded, "id", "/item/id")},
                              {"type", type},
                              {"status", expanded.value("status", std::string("unknown"))},
                              {"agentText", expanded.value("agentText", std::string{})},
                              {"reasoningText", expanded.value("reasoningText", std::string{})},
                              {"reasoningSummary", expanded.value("reasoningSummary", std::string{})},
                              {"commandOutput", expanded.value("commandOutput", std::string{})},
                              {"droppedContentBytes", expanded.value("droppedContentBytes", std::uint64_t{0})},
                              {"contentTruncated", expanded.value("contentTruncated", false)},
                              {"data", std::move(data)},
                              {"extensions", frontend::Json::object()}};
        for (std::string_view member : {"startedAtMs", "completedAtMs"}) {
            if (const auto found = expanded.find(member); found != expanded.end()) {
                result[std::string(member)] = *found;
            }
        }
        return result;
    }

    frontend::Json representativeExpandedDocument(const frontend::Json& fixture, std::string_view type) {
        const auto found = std::find_if(fixture.at("expandedEvents").begin(),
                                        fixture.at("expandedEvents").end(),
                                        [type](const frontend::Json& event) {
                                            return event.at("type").get_ref<const std::string&>() == type;
                                        });
        if (found == fixture.at("expandedEvents").end()) {
            throw std::runtime_error("reviewed fixture omits expanded event family " + std::string(type));
        }
        frontend::Json representative = *found;
        if (type == "sessions.updated") {
            representative["data"]["sessions"] =
                frontend::Json::array({frontend::Json{{"sessionId", "1"}, {"role", "observer"}}});
        } else if (type == "pendingRequests.updated") {
            representative["data"]["pendingRequests"] =
                frontend::Json::array({fixture.at("expandedSnapshot").at("state").at("pendingRequests").at(0)});
        } else if (type == "item.upserted") {
            representative["data"]["item"]["threadId"] = "x";
            representative["data"]["item"]["turnId"] = "x";
        } else if (type == "item.content.updated") {
            representative["data"]["channel"] = "agentText";
            representative["data"]["contentTruncated"] = false;
            representative["data"]["droppedContentBytes"] = 0;
        } else if (type == "activity.updated") {
            representative["data"]["activity"] = frontend::Json{{"key", "fixture-activity"},
                                                                  {"kind", "tool"},
                                                                  {"lifecycle", "running"},
                                                                  {"active", true},
                                                                  {"stamp", {{"freshness", "unknown"}, {"generation", 0}}}};
        }
        return representative;
    }

    frontend::ExpandedFrontendEvent representativeExpandedEvent(const frontend::Json& fixture, std::string_view type) {
        const auto decoded = frontend::Codec::decodeExpandedEvent(representativeExpandedDocument(fixture, type));
        if (!decoded) {
            throw std::runtime_error("reviewed expanded event fixture does not decode for " + std::string(type) + ": " +
                                     decoded.error().message);
        }
        return decoded.value();
    }

    std::optional<frontend::FrontendEvent> normalizedLegacyInputFromOldProjection(
        const frontend::ExpandedFrontendEvent& expanded) {
        frontend::Json data;
        switch (expanded.type) {
            case frontend::ExpandedEventType::ProviderUpdated: {
                const frontend::Json provider = expanded.data.value("provider", frontend::Json::object());
                std::string lifecycle = provider.value("lifecycle", std::string("stopped"));
                if (lifecycle == "recovering") {
                    lifecycle = "starting";
                }
                data = frontend::Json{{"lifecycle", std::move(lifecycle)}};
                if (const auto error = provider.find("lastError"); error != provider.end()) {
                    data["error"] = *error;
                }
                return frontend::FrontendEvent{expanded.sequence, "backend.lifecycle.changed", std::move(data), expanded.extensions};
            }
            case frontend::ExpandedEventType::ControllerUpdated: {
                data = frontend::Json::object();
                const frontend::Json controller = expanded.data.value("controller", frontend::Json::object());
                if (const auto session = controller.find("controllerSessionId");
                    session != controller.end() && session->is_string()) {
                    data["controllerSessionId"] = *session;
                }
                return frontend::FrontendEvent{expanded.sequence, "controller.changed", std::move(data), expanded.extensions};
            }
            case frontend::ExpandedEventType::SessionsUpdated: {
                const frontend::Json sessions = expanded.data.value("sessions", frontend::Json::array());
                if (!sessions.is_array() || sessions.size() != 1) {
                    return std::nullopt;
                }
                const frontend::Json& session = sessions.front();
                data = frontend::Json{{"sessionId", requiredProjectedString(session, "sessionId", "/sessions/sessionId")},
                                      {"connected", true},
                                      {"role", session.value("role", std::string("observer"))}};
                return frontend::FrontendEvent{expanded.sequence, "session.changed", std::move(data), expanded.extensions};
            }
            case frontend::ExpandedEventType::ThreadListUpdated:
                return frontend::FrontendEvent{
                    expanded.sequence, "thread.list.updated", expanded.data.value("threadList", frontend::Json::object()), expanded.extensions};
            case frontend::ExpandedEventType::ThreadUpserted: {
                const frontend::Json source = expanded.data.value("thread", frontend::Json::object());
                frontend::Json thread{{"id", requiredProjectedString(source, "id", "/thread/id")},
                                      {"fullyLoaded", source.value("fullyLoaded", false)},
                                      {"turns", frontend::Json::array()},
                                      {"extensions", frontend::Json::object()}};
                for (std::string_view member :
                     {"title", "cwd", "model", "modelProvider", "preview", "status", "createdAt", "updatedAt"}) {
                    if (const auto found = source.find(member); found != source.end()) {
                        thread[std::string(member)] = *found;
                    }
                }
                return frontend::FrontendEvent{
                    expanded.sequence, "thread.updated", frontend::Json{{"thread", std::move(thread)}}, expanded.extensions};
            }
            case frontend::ExpandedEventType::ThreadRemoved:
                return frontend::FrontendEvent{expanded.sequence, "thread.removed", expanded.data, expanded.extensions};
            case frontend::ExpandedEventType::TurnUpserted: {
                const frontend::Json source = expanded.data.value("turn", frontend::Json::object());
                frontend::Json turn{{"id", requiredProjectedString(source, "id", "/turn/id")},
                                    {"threadId", requiredProjectedString(source, "threadId", "/turn/threadId")},
                                    {"status", source.value("status", std::string("unknown"))},
                                    {"active", source.value("active", false)},
                                    {"terminal", source.value("terminal", false)},
                                    {"items", frontend::Json::array()},
                                    {"extensions", frontend::Json::object()}};
                for (std::string_view member : {"failure", "tokenUsage"}) {
                    if (const auto found = source.find(member); found != source.end()) {
                        turn[std::string(member)] = *found;
                    }
                }
                return frontend::FrontendEvent{
                    expanded.sequence, "turn.updated", frontend::Json{{"turn", std::move(turn)}}, expanded.extensions};
            }
            case frontend::ExpandedEventType::ItemUpserted: {
                const frontend::Json source = expanded.data.value("item", frontend::Json::object());
                frontend::Json legacyItem = legacyItemFromOldProjection(source);
                frontend::Json item{{"item", std::move(legacyItem)}};
                if (const auto thread = source.find("threadId"); thread != source.end() && thread->is_string()) {
                    item["threadId"] = *thread;
                }
                if (const auto turn = source.find("turnId"); turn != source.end() && turn->is_string()) {
                    item["turnId"] = *turn;
                }
                return frontend::FrontendEvent{expanded.sequence, "item.updated", std::move(item), expanded.extensions};
            }
            case frontend::ExpandedEventType::ItemContentUpdated:
                return frontend::FrontendEvent{expanded.sequence, "item.content.updated", expanded.data, expanded.extensions};
            case frontend::ExpandedEventType::PendingRequestsUpdated: {
                const frontend::Json pending = expanded.data.value("pendingRequests", frontend::Json::array());
                if (!pending.is_array() || pending.size() != 1) {
                    return std::nullopt;
                }
                return frontend::FrontendEvent{expanded.sequence,
                                               "request.pending",
                                               frontend::Json{{"request", legacyPendingRequestFromOldProjection(pending.front())}},
                                               expanded.extensions};
            }
            case frontend::ExpandedEventType::DiagnosticsUpdated: {
                const frontend::Json diagnostic = expanded.data.value("diagnostic", frontend::Json::object());
                data = frontend::Json{{"received", diagnostic.value("received", std::uint64_t{0})},
                                      {"recent", frontend::Json::array()}};
                if (const auto message = diagnostic.find("message"); message != diagnostic.end() && message->is_string()) {
                    data["recent"].push_back(*message);
                }
                return frontend::FrontendEvent{expanded.sequence, "diagnostics.updated", std::move(data), expanded.extensions};
            }
            default:
                return std::nullopt;
        }
    }

    frontend::FrontendCapability authorityCapability(generated::Capability capability) {
        const auto metadata = std::find_if(generated::AllCapabilities.begin(),
                                           generated::AllCapabilities.end(),
                                           [capability](const generated::CapabilityMetadata& value) {
                                               return value.id == capability;
                                           });
        if (metadata == generated::AllCapabilities.end()) {
            throw std::runtime_error("notification authority references an unknown capability");
        }
        const auto frontendCapability = frontend::frontendCapabilityFromString(metadata->key);
        if (!frontendCapability) {
            throw std::runtime_error("notification authority capability has no frontend identity: " + std::string(metadata->key));
        }
        return *frontendCapability;
    }

    std::string authorityMethod(const generated::ProjectionMetadata& metadata) {
        constexpr std::string_view Prefix = "server_notification:ServerNotification:method:";
        if (!metadata.registryKey.starts_with(Prefix) || metadata.registryKey.size() == Prefix.size()) {
            throw std::runtime_error("notification authority has a malformed registry key: " + std::string(metadata.registryKey));
        }
        return std::string(metadata.registryKey.substr(Prefix.size()));
    }

    bool compareEventVectors(tests::support::TestResult& result,
                             const std::vector<frontend::FrontendEvent>& oldEvents,
                             const std::vector<frontend::FrontendEvent>& newEvents,
                             std::string_view identity) {
        if (oldEvents.empty() || newEvents.empty()) {
            result.expectTrue(false,
                              std::string(identity) + " event-count mismatch: old=" + std::to_string(oldEvents.size()) +
                                  " new=" + std::to_string(newEvents.size()));
            return false;
        }
        const frontend::EventBatch oldBatch{oldEvents.front().sequence,
                                            oldEvents.back().sequence,
                                            oldEvents,
                                            frontend::Json::object()};
        const frontend::EventBatch newBatch{newEvents.front().sequence,
                                            newEvents.back().sequence,
                                            newEvents,
                                            frontend::Json::object()};
        if (const auto mismatch = firstMismatch(encodeServer(frontend::ServerMessage{oldBatch}),
                                                encodeServer(frontend::ServerMessage{newBatch}))) {
            result.expectTrue(false,
                              std::string(identity) + " mismatch at " + mismatch->path + ": old=" + mismatch->oldValue +
                                  " new=" + mismatch->newValue);
            return false;
        }
        return true;
    }

    void testAuthorityProjectionCorpus(tests::support::TestResult& result,
                                       const frontend::Json& fixture,
                                       const frontend::Json& coverage) {
        // The generated document is schema/currentness input only.  Its
        // placeholder values deliberately exercise optional schema shapes and
        // are not old-runtime output.  Behavioral expected values below come
        // exclusively from FrontendService or BackendProjectionBuilder.
        const auto schemaSnapshot = frontend::Codec::decodeExpandedSnapshot(fixture.at("expandedSnapshot"));
        if (!schemaSnapshot) {
            throw std::runtime_error("generated expanded snapshot schema fixture does not decode: " + schemaSnapshot.error().message);
        }
        const auto schemaTypedSnapshot = model::decodeSnapshot(schemaSnapshot.value());
        if (!schemaTypedSnapshot) {
            throw std::runtime_error("generated expanded snapshot schema fixture does not enter the typed inventory at " +
                                     schemaTypedSnapshot.error().path + ": " + schemaTypedSnapshot.error().message);
        }

        const std::vector snapshotCapabilities{frontend::FrontendCapability::CompleteBackendDomains,
                                               frontend::FrontendCapability::CompleteThreadItems,
                                               frontend::FrontendCapability::DedicatedPendingRequests};

        Pair expandedRuntime;
        expandedRuntime.hello(snapshotCapabilities);
        Pair legacyRuntime;
        legacyRuntime.hello();
        const frontend::Snapshot* oldExpandedSnapshot = firstSnapshot(expandedRuntime.oldObservations.messages);
        const frontend::Snapshot* oldLegacySnapshot = firstSnapshot(legacyRuntime.oldObservations.messages);

        const auto compareOldSnapshot = [&](const frontend::Snapshot* oldSnapshot,
                                            const std::vector<frontend::FrontendCapability>& capabilities,
                                            std::string_view representation) {
            if (oldSnapshot == nullptr) {
                result.expectTrue(false, "old FrontendService emitted no " + std::string(representation) + " snapshot");
                return false;
            }
            const auto typed = model::decodeProjectedSnapshot(*oldSnapshot, capabilities);
            if (!typed) {
                result.expectTrue(false,
                                  "old FrontendService " + std::string(representation) + " snapshot failed typed decoding at " +
                                      typed.error().path + ": " + typed.error().message);
                return false;
            }
            const auto encoded = model::encodeProjectedSnapshot(typed.value(), capabilities);
            if (!encoded) {
                result.expectTrue(false,
                                  "typed " + std::string(representation) + " snapshot encoding failed at " + encoded.error().path +
                                      ": " + encoded.error().message);
                return false;
            }
            if (const auto mismatch = firstMismatch(encodeServer(frontend::ServerMessage{*oldSnapshot}),
                                                    encodeServer(frontend::ServerMessage{encoded.value()}))) {
                result.expectTrue(false,
                                  "old FrontendService " + std::string(representation) + " snapshot mismatch at " + mismatch->path +
                                      ": old=" + mismatch->oldValue + " new=" + mismatch->newValue);
                return false;
            }
            return true;
        };

        const bool expandedSnapshotParity = compareOldSnapshot(oldExpandedSnapshot, snapshotCapabilities, "expanded-v1");
        const bool legacySnapshotParity = compareOldSnapshot(oldLegacySnapshot, {}, "legacy-v1");
        result.expectTrue(expandedSnapshotParity && legacySnapshotParity,
                          "actual old FrontendService output defines both snapshot-v1 behavioral borders");

        std::set<std::string> typedItems;
        for (const model::ThreadItem& item : schemaTypedSnapshot.value().items) {
            typedItems.emplace(frontend::toString(model::threadItemKind(item)));
        }
        for (const frontend::Json& row : coverage.at("threadItems")) {
            const std::string discriminator = row.at("type").get<std::string>();
            const bool represented = typedItems.contains(discriminator);
            static_cast<void>(recordCase(
                coverageCase("item", discriminator, "projection-expanded"), expandedSnapshotParity && represented));
            static_cast<void>(recordCase(
                coverageCase("item", discriminator, "projection-legacy"), legacySnapshotParity && represented));
            result.expectTrue(represented, "generated schema inventory contains typed ThreadItem discriminator " + discriminator);
        }

        std::set<std::string> typedPending;
        for (const model::PendingRequest& pending : schemaTypedSnapshot.value().pendingRequests) {
            typedPending.emplace(frontend::toString(model::pendingRequestKind(pending)));
        }
        const std::string encodedRuntimeSnapshots =
            (oldExpandedSnapshot == nullptr ? std::string{} : encodeServer(frontend::ServerMessage{*oldExpandedSnapshot}).dump()) +
            (oldLegacySnapshot == nullptr ? std::string{} : encodeServer(frontend::ServerMessage{*oldLegacySnapshot}).dump());
        const bool snapshotSecretCeiling = encodedRuntimeSnapshots.find("accessToken") == std::string::npos &&
                                           encodedRuntimeSnapshots.find("clientSecret") == std::string::npos;
        for (const frontend::Json& row : coverage.at("pendingRequests")) {
            const std::string kind = row.at("kind").get<std::string>();
            const bool represented = typedPending.contains(kind);
            const bool projectionParity = expandedSnapshotParity && legacySnapshotParity && represented;
            static_cast<void>(recordCase(coverageCase("pending", kind, "projection"), projectionParity));
            static_cast<void>(recordCase(coverageCase("pending", kind, "security"), projectionParity && snapshotSecretCeiling));
            result.expectTrue(represented, "generated schema inventory contains typed pending-request kind " + kind);
        }

        std::map<std::string, bool, std::less<>> schemaEvidence;
        for (const frontend::Json& row : fixture.at("expandedEvents")) {
            const std::string type = row.at("type").get<std::string>();
            const frontend::ExpandedFrontendEvent representative = representativeExpandedEvent(fixture, type);
            const auto expectedType = frontend::expandedEventTypeFromString(type);
            schemaEvidence.emplace(type, expectedType.has_value() && representative.type == *expectedType);
        }

        std::map<std::string, bool, std::less<>> expandedEvidence;
        std::map<std::string, bool, std::less<>> legacyEvidence;
        std::map<std::string, bool, std::less<>> projectionOnlyExpandedEvidence;
        std::map<std::string, bool, std::less<>> projectionOnlyLegacyEvidence;
        std::set<std::string> observedLegacyContracts;
        std::set<std::string> observedSourceMethods;
        std::size_t evaluatedSourceRows = 0;
        std::size_t caseOrdinal = 0;
        const auto accumulate = [](std::map<std::string, bool, std::less<>>& evidence,
                                   const std::string& family,
                                   bool matched) {
            const auto [entry, inserted] = evidence.emplace(family, matched);
            if (!inserted) {
                entry->second = entry->second && matched;
            }
        };

        const auto evaluateOccurrence = [&](std::span<const std::string_view> orderedMappings,
                                            const std::vector<frontend::ExpandedFrontendEvent>& expectedExpanded,
                                            const frontend::FrontendEvent& oldLegacy,
                                            std::string sourceStamp,
                                            std::string legacyContract,
                                            bool sourcePreserved,
                                            bool generatedSource,
                                            bool oracleConformance) {
            const auto finish = [&](bool expandedMatched, bool legacyMatched) {
                if (generatedSource) {
                    static_cast<void>(recordCase(
                        coverageCase("notification", sourceStamp, "mapping"), expandedMatched && legacyMatched));
                }
                for (const std::string_view mapping : orderedMappings) {
                    const std::string family(mapping);
                    if (oracleConformance) {
                        accumulate(expandedEvidence, family, expandedMatched);
                        accumulate(legacyEvidence, family, legacyMatched);
                    } else {
                        accumulate(projectionOnlyExpandedEvidence, family, expandedMatched);
                        accumulate(projectionOnlyLegacyEvidence, family, legacyMatched);
                    }
                }
                ++caseOrdinal;
                return expandedMatched && legacyMatched;
            };

            if (orderedMappings.empty() || expectedExpanded.size() != orderedMappings.size()) {
                throw std::runtime_error("authority occurrence has no expanded mapping: " + sourceStamp);
            }
            std::vector<model::OccurrencePayload> typedPayloads;
            typedPayloads.reserve(orderedMappings.size());
            for (std::size_t index = 0; index < orderedMappings.size(); ++index) {
                const std::string family(orderedMappings[index]);
                if (frontend::toString(expectedExpanded[index].type) != family) {
                    result.expectTrue(false, sourceStamp + " old builder changed ordered expanded mapping " + family);
                    return finish(false, false);
                }
                const model::OccurrenceDecodeContext context{
                    model::OccurrenceGroupIdentity{"authority-case-" + std::to_string(caseOrdinal)},
                    static_cast<std::uint32_t>(index),
                    static_cast<std::uint32_t>(orderedMappings.size()),
                    model::SourceStamp{sourceStamp}};
                const auto decoded = model::decodeExpandedOccurrence(expectedExpanded[index], context);
                if (!decoded || decoded.value().expandedPayloads().size() != 1) {
                    result.expectTrue(false,
                                      "old builder occurrence failed typed decoding for " + family + " from " + sourceStamp +
                                          (decoded ? std::string(": wrong payload count")
                                                   : ": " + decoded.error().path + ": " + decoded.error().message));
                    return finish(false, false);
                }
                typedPayloads.push_back(decoded.value().expandedPayloads().front());
            }

            const model::OccurrenceDecodeContext legacyContext{model::OccurrenceGroupIdentity{"authority-case-" +
                                                                                              std::to_string(caseOrdinal)},
                                                               0,
                                                               static_cast<std::uint32_t>(orderedMappings.size()),
                                                               model::SourceStamp{sourceStamp}};
            const auto decodedLegacy = model::decodeLegacyOccurrence(oldLegacy, legacyContext);
            if (!decodedLegacy) {
                result.expectTrue(false,
                                  "old builder legacy occurrence failed typed decoding for " + sourceStamp + " at " +
                                      decodedLegacy.error().path + ": " + decodedLegacy.error().message);
                return finish(false, false);
            }
            model::LegacyCompatibilityPayload compatibility = decodedLegacy.value().legacyCompatibility();

            const model::OccurrenceIdentity identity{model::FrontendSequence{expectedExpanded.front().sequence.value()},
                                                     model::OccurrenceGroupIdentity{"authority-case-" +
                                                                                    std::to_string(caseOrdinal)},
                                                     0,
                                                     static_cast<std::uint32_t>(typedPayloads.size()),
                                                     model::SourceStamp{sourceStamp}};
            const auto typedGroup = model::makeOccurrenceGroup(identity, std::move(compatibility), std::move(typedPayloads));
            if (!typedGroup) {
                result.expectTrue(false,
                                  "typed occurrence group construction failed for " + sourceStamp + " at " + typedGroup.error().path +
                                      ": " + typedGroup.error().message);
                return finish(false, false);
            }
            const auto newExpanded = model::encodeExpandedOccurrence(typedGroup.value());
            const auto newLegacy = model::encodeLegacyOccurrence(typedGroup.value());
            if (!newExpanded || !newLegacy) {
                result.expectTrue(false,
                                  "typed occurrence encoding failed for " + sourceStamp +
                                      (newExpanded ? std::string{} : " expanded=" + newExpanded.error().path + ": " +
                                                                                      newExpanded.error().message) +
                                      (newLegacy ? std::string{} : " legacy=" + newLegacy.error().path + ": " +
                                                                                  newLegacy.error().message));
                return finish(false, false);
            }
            std::vector<frontend::FrontendEvent> oldExpandedEvents;
            oldExpandedEvents.reserve(expectedExpanded.size());
            for (const frontend::ExpandedFrontendEvent& event : expectedExpanded) {
                oldExpandedEvents.push_back(frontend::FrontendEvent{
                    event.sequence, std::string(frontend::toString(event.type)), event.data, event.extensions});
            }
            std::vector<frontend::FrontendEvent> newExpandedEvents;
            newExpandedEvents.reserve(newExpanded.value().size());
            for (const frontend::ExpandedFrontendEvent& event : newExpanded.value()) {
                newExpandedEvents.push_back(frontend::FrontendEvent{
                    event.sequence, std::string(frontend::toString(event.type)), event.data, event.extensions});
            }

            const std::string diagnostic = sourceStamp + " [" + legacyContract + "]";
            const bool expandedParity =
                sourcePreserved && compareEventVectors(result, oldExpandedEvents, newExpandedEvents, diagnostic + " expanded");
            const bool legacyParity =
                sourcePreserved && compareEventVectors(result, {oldLegacy}, {newLegacy.value()}, diagnostic + " legacy");
            result.expectTrue(sourcePreserved && expandedParity && legacyParity,
                              (oracleConformance ? "old executable oracle preserves " : "projection/schema evidence preserves ") +
                                  diagnostic);
            return finish(sourcePreserved && expandedParity, sourcePreserved && legacyParity);
        };

        const backend::Snapshot backendSnapshot = behavioralBackendSnapshot();
        constexpr std::string_view SecretSentinel = "P2_SERVER_DIFFERENTIAL_SECRET_MUST_NOT_ESCAPE";
        const auto projectedExpandedEvents = [&](const legacy::CanonicalEventRecord& record) {
            const legacy::EventProjection projection =
                legacy::projectEvent(record, localProjectionContext({record.expansionCapability}));
            std::vector<frontend::ExpandedFrontendEvent> events;
            events.reserve(projection.events.size());
            for (const frontend::FrontendEvent& event : projection.events) {
                const auto type = frontend::expandedEventTypeFromString(event.type);
                if (!type.has_value()) {
                    throw std::runtime_error("old BackendProjectionBuilder emitted an unknown expanded event type");
                }
                frontend::ExpandedFrontendEvent expanded{event.sequence, *type, event.data, event.extensions};
                const auto valid = frontend::Codec::encodeExpandedEvent(expanded);
                if (!valid) {
                    throw std::runtime_error("old BackendProjectionBuilder emitted an invalid expanded event: " + valid.error().message);
                }
                events.push_back(std::move(expanded));
            }
            return events;
        };

        const auto projectedLegacyEvent = [&](const legacy::CanonicalEventRecord& record) {
            const legacy::EventProjection projection = legacy::projectEvent(record, localProjectionContext({}));
            if (projection.events.size() != 1) {
                throw std::runtime_error("old BackendProjectionBuilder did not emit exactly one compatibility event");
            }
            return projection.events.front();
        };

        for (std::size_t notificationIndex = 0; notificationIndex < generated::AllNotificationProjections.size(); ++notificationIndex) {
            const generated::ProjectionMetadata& metadata = generated::AllNotificationProjections[notificationIndex];
            const std::string method = authorityMethod(metadata);
            const frontend::SequenceNumber sequence{100 + notificationIndex};
            legacy::CanonicalEventRecord oldRecord = legacy::makeCanonicalEventRecord(
                "codex.extension",
                frontend::Json{{"method", method},
                               {"params", behavioralNotificationParameters()},
                               {"accessToken", SecretSentinel}},
                backendSnapshot,
                sequence);
            const auto capability = authorityCapability(metadata.expansionCapability);
            std::vector<frontend::ExpandedFrontendEvent> expectedExpanded = projectedExpandedEvents(oldRecord);
            const bool exactRequiredScopes =
                oldRecord.requiredScopes.size() == metadata.requiredScopes.size() &&
                std::equal(oldRecord.requiredScopes.begin(), oldRecord.requiredScopes.end(), metadata.requiredScopes.begin());
            bool sourcePreserved = capability == oldRecord.expansionCapability && exactRequiredScopes && oldRecord.registryKey.has_value() &&
                                   *oldRecord.registryKey == metadata.registryKey &&
                                   oldRecord.expandedEvents.size() == metadata.expandedMappings.size() &&
                                   expectedExpanded.size() == metadata.expandedMappings.size() && !oldRecord.snapshotRequired;
            for (std::size_t mappingIndex = 0; mappingIndex < metadata.expandedMappings.size() && sourcePreserved; ++mappingIndex) {
                sourcePreserved = frontend::toString(oldRecord.expandedEvents[mappingIndex].type) == metadata.expandedMappings[mappingIndex] &&
                                  frontend::toString(expectedExpanded[mappingIndex].type) == metadata.expandedMappings[mappingIndex];
            }
            sourcePreserved = sourcePreserved &&
                              legacy::canonicalValueContainsNoKnownStructuredSecrets(oldRecord.legacyData.value) &&
                              std::all_of(oldRecord.expandedEvents.begin(), oldRecord.expandedEvents.end(), [](const auto& event) {
                                  return legacy::canonicalValueContainsNoKnownStructuredSecrets(event.data.value);
                              });

            frontend::FrontendEvent expectedLegacy;
            bool legacyContractPreserved = false;
            if (metadata.legacyContract == "legacy_redacted_extension") {
                expectedLegacy = projectedLegacyEvent(oldRecord);
                legacyContractPreserved = expectedLegacy.type == "codex.extension" &&
                                          expectedLegacy.data.value("method", std::string{}) == method;
            } else if (metadata.legacyContract == "legacy_normalized") {
                if (metadata.expandedMappings.size() != 1 || expectedExpanded.size() != 1) {
                    throw std::runtime_error("normalized notification authority has a non-singleton old projection: " +
                                             std::string(metadata.registryKey));
                }
                // Select the reviewed normalized legacy source shape from the
                // old builder's expanded value, then send that source through
                // the old builder again.  The generated schema fixture never
                // supplies a behavioral expected value on either border.
                const auto normalizedInput = normalizedLegacyInputFromOldProjection(expectedExpanded.front());
                if (!normalizedInput) {
                    throw std::runtime_error("old expanded projection lacks a frozen normalized compatibility driver for " + method);
                }
                const legacy::CanonicalEventRecord normalizedRecord = legacy::makeCanonicalEventRecord(
                    normalizedInput->type, normalizedInput->data, backendSnapshot, sequence);
                expectedLegacy = projectedLegacyEvent(normalizedRecord);
                legacyContractPreserved = expectedLegacy.type == normalizedInput->type;
            } else {
                throw std::runtime_error("reviewed notification authority has unknown legacyContract " +
                                         std::string(metadata.legacyContract));
            }
            const std::string projectedDocuments = expectedLegacy.data.dump() + expectedLegacy.extensions.dump();
            for (const frontend::ExpandedFrontendEvent& event : expectedExpanded) {
                sourcePreserved = sourcePreserved && event.data.dump().find(SecretSentinel) == std::string::npos &&
                                  event.extensions.dump().find(SecretSentinel) == std::string::npos;
            }
            sourcePreserved = sourcePreserved && legacyContractPreserved &&
                              projectedDocuments.find(SecretSentinel) == std::string::npos;

            static_cast<void>(evaluateOccurrence(metadata.expandedMappings,
                                                  expectedExpanded,
                                                  expectedLegacy,
                                                  std::string(metadata.registryKey),
                                                  std::string(metadata.legacyContract),
                                                  sourcePreserved,
                                                  true,
                                                  true));
            observedLegacyContracts.emplace(metadata.legacyContract);
            observedSourceMethods.emplace(method);
            ++evaluatedSourceRows;
        }

        struct BuiltinOccurrence {
            std::string legacyType;
            frontend::Json legacyData;
            std::string_view expandedFamily;
        };
        const std::vector<BuiltinOccurrence> builtins{
            {"backend.lifecycle.changed", {{"lifecycle", "ready"}}, "provider.updated"},
            {"controller.changed", {{"controllerSessionId", "1"}}, "controller.updated"},
            {"session.changed", {{"sessionId", "1"}, {"connected", true}, {"role", "controller"}}, "sessions.updated"},
            {"thread.list.updated", legacy::threadListProjection(backendSnapshot.threadList), "threadList.updated"},
        };
        for (std::size_t index = 0; index < builtins.size(); ++index) {
            const BuiltinOccurrence& source = builtins[index];
            const legacy::CanonicalEventRecord oldRecord = legacy::makeCanonicalEventRecord(
                source.legacyType, source.legacyData, backendSnapshot, frontend::SequenceNumber{1'000 + index});
            const std::vector<frontend::ExpandedFrontendEvent> expectedExpanded = projectedExpandedEvents(oldRecord);
            const frontend::FrontendEvent expectedLegacy = projectedLegacyEvent(oldRecord);
            const std::array<std::string_view, 1> mapping{source.expandedFamily};
            const bool sourcePreserved = expectedExpanded.size() == 1 &&
                                         frontend::toString(expectedExpanded.front().type) == source.expandedFamily &&
                                         !oldRecord.snapshotRequired;
            static_cast<void>(evaluateOccurrence(mapping,
                                                  expectedExpanded,
                                                  expectedLegacy,
                                                  "builtin-event:" + std::string(source.expandedFamily),
                                                  "legacy_normalized",
                                                  sourcePreserved,
                                                  false,
                                                  true));
        }

        std::set<std::string> projectionOnlyFamilies;
        const std::vector<BuiltinOccurrence> projectionOnly{
            {"models.updated", frontend::Json::object(), "models.updated"},
            {"plugins.updated", frontend::Json::object(), "plugins.updated"},
            {"activity.updated",
             {{"activity",
               {{"key", "activity-1"},
                {"subjectId", "subject-1"},
                {"kind", "tool"},
                {"lifecycle", "running"},
                {"active", true},
                {"stamp", {{"generation", 9}, {"freshness", "current"}}}}}},
             "activity.updated"},
            {"capacity.updated", frontend::Json::object(), "capacity.updated"},
        };
        for (std::size_t index = 0; index < projectionOnly.size(); ++index) {
            const BuiltinOccurrence& source = projectionOnly[index];
            const legacy::CanonicalEventRecord oldRecord = legacy::makeCanonicalEventRecord(
                source.legacyType, source.legacyData, backendSnapshot, frontend::SequenceNumber{2'000 + index});
            const std::vector<frontend::ExpandedFrontendEvent> expectedExpanded = projectedExpandedEvents(oldRecord);
            const frontend::FrontendEvent expectedLegacy =
                expectedExpanded.size() == 1
                    ? frontend::FrontendEvent{expectedExpanded.front().sequence,
                                              std::string(frontend::toString(expectedExpanded.front().type)),
                                              expectedExpanded.front().data,
                                              expectedExpanded.front().extensions}
                    : projectedLegacyEvent(oldRecord);
            const std::array<std::string_view, 1> mapping{source.expandedFamily};
            const bool projectionPreserved = expectedExpanded.size() == 1 &&
                                             frontend::toString(expectedExpanded.front().type) == source.expandedFamily &&
                                             !oldRecord.snapshotRequired;
            static_cast<void>(evaluateOccurrence(mapping,
                                                  expectedExpanded,
                                                  expectedLegacy,
                                                  "projection-only-no-source-notification:" +
                                                      std::string(source.expandedFamily),
                                                  "projection_schema_only",
                                                  projectionPreserved,
                                                  false,
                                                  false));
            projectionOnlyFamilies.emplace(source.expandedFamily);
        }

        const bool authorityInventory = evaluatedSourceRows == generated::AllNotificationProjections.size() &&
                                        observedSourceMethods.size() == generated::AllNotificationProjections.size() &&
                                        observedLegacyContracts == std::set<std::string>{"legacy_normalized", "legacy_redacted_extension"};
        result.expectTrue(authorityInventory,
                          "all generated source-notification rows retain exact method, legacyContract, and ordered expanded mappings");
        result.expectTrue(projectionOnlyFamilies == std::set<std::string>{"activity.updated", "capacity.updated", "models.updated", "plugins.updated"},
                          "only four snapshot/provider-result families lack a reviewed source-notification mapping; their fixture evidence is "
                          "explicitly projection-only");

        std::set<std::string> observedEvents;
        for (const auto& [type, schemaValid] : schemaEvidence) {
            const bool projectionOnlyFamily = projectionOnlyFamilies.contains(type);
            const bool expandedParity = projectionOnlyFamily
                                          ? schemaValid && projectionOnlyExpandedEvidence.contains(type) &&
                                                projectionOnlyExpandedEvidence.at(type)
                                          : expandedEvidence.contains(type) && expandedEvidence.at(type);
            const bool legacyParity = projectionOnlyFamily
                                        ? schemaValid && projectionOnlyLegacyEvidence.contains(type) &&
                                              projectionOnlyLegacyEvidence.at(type)
                                        : legacyEvidence.contains(type) && legacyEvidence.at(type);
            static_cast<void>(recordCase(coverageCase("event", type, "server-expanded"), expandedParity));
            static_cast<void>(recordCase(coverageCase("event", type, "server-legacy"), legacyParity));
            result.expectTrue(
                expandedParity && legacyParity,
                (projectionOnlyFamily ? "explicit projection/schema-only evidence (not source-notification oracle conformance) covers "
                                      : "old executable oracle covers both representations for ") +
                    type);
            if (expandedParity && legacyParity) {
                observedEvents.emplace(type);
            }
        }
        result.expectTrue(observedEvents.size() == legacy::AllExpandedEventProjections.size() && observedEvents.size() == 26,
                          "independent authority projection corpus aggregates every one of the 26 closed event families");
    }

    void testScopeAndCapabilityCorpus(tests::support::TestResult& result,
                                      const frontend::Json& fixture,
                                      const frontend::Json& coverage) {
        std::vector<frontend::FrontendCapability> allCapabilities;
        allCapabilities.reserve(generated::AllCapabilities.size());
        for (const generated::CapabilityMetadata& metadata : generated::AllCapabilities) {
            allCapabilities.push_back(static_cast<frontend::FrontendCapability>(metadata.id));
        }

        Pair capabilityPair;
        capabilityPair.hello(allCapabilities);
        const bool capabilityBorderParity = comparePair(result, capabilityPair, "all-capability negotiation");
        const frontend::Welcome* oldWelcome =
            !capabilityPair.oldObservations.messages.empty()
                ? std::get_if<frontend::Welcome>(&capabilityPair.oldObservations.messages.front())
                : nullptr;
        const frontend::Welcome* newWelcome =
            !capabilityPair.newObservations.messages.empty()
                ? std::get_if<frontend::Welcome>(&capabilityPair.newObservations.messages.front())
                : nullptr;
        for (const frontend::Json& row : coverage.at("capabilities")) {
            const std::string key = row.at("key").get<std::string>();
            const auto capability = frontend::frontendCapabilityFromString(key);
            const auto contains = [](const std::vector<frontend::FrontendCapability>& values,
                                     frontend::FrontendCapability searched) {
                return std::find(values.begin(), values.end(), searched) != values.end();
            };
            const bool advertised = capability && oldWelcome && newWelcome && oldWelcome->capabilities && newWelcome->capabilities &&
                                    contains(oldWelcome->capabilities->defined, *capability) &&
                                    contains(newWelcome->capabilities->defined, *capability);
            const bool negotiated = capability && oldWelcome && newWelcome && oldWelcome->capabilities && newWelcome->capabilities &&
                                    contains(oldWelcome->capabilities->permitted, *capability) ==
                                        contains(newWelcome->capabilities->permitted, *capability);
            static_cast<void>(recordCase(
                coverageCase("capability", key, "advertisement"), capabilityBorderParity && advertised));
            static_cast<void>(recordCase(
                coverageCase("capability", key, "negotiation"), capabilityBorderParity && advertised && negotiated));
            result.expectTrue(advertised && negotiated, "capability advertisement/negotiation classification for " + key);
        }

        const std::vector representationCapabilities{frontend::FrontendCapability::CompleteBackendDomains,
                                                      frontend::FrontendCapability::CompleteThreadItems,
                                                      frontend::FrontendCapability::DedicatedPendingRequests,
                                                      frontend::FrontendCapability::DedicatedNotificationEvents,
                                                      frontend::FrontendCapability::ScopeProjectedState};
        for (const frontend::Json& row : coverage.at("scopes")) {
            const std::string scopeName = row.at("scope").get<std::string>();
            const std::optional<frontend::FrontendScope> scope = frontend::frontendScopeFromString(scopeName);
            if (!scope) {
                throw std::runtime_error("coverage fixture contains unknown scope " + scopeName);
            }
            std::vector<frontend::FrontendScope> scopes{frontend::FrontendScope::Observe};
            if (*scope != frontend::FrontendScope::Observe) {
                scopes.push_back(*scope);
            }
            const auto authenticator = [scopes](const frontend::FrontendPeerContext&,
                                                const frontend::AuthenticationCredential&) -> frontend::AuthenticationResult {
                return frontend::AuthenticationSuccess{
                    frontend::FrontendPrincipal{"scope-" + std::string(frontend::toString(scopes.back())),
                                                scopes,
                                                "differential_scope",
                                                false}};
            };
            PairSettings settings;
            settings.peer = remotePeer();
            settings.oldAuthenticator = authenticator;
            settings.newAuthenticator = authenticator;
            Pair pair(std::move(settings));
            pair.hello(representationCapabilities, std::nullopt, bearerCredential());
            const bool snapshotParity = comparePair(result, pair, "scope snapshot " + scopeName);

            // The session occurrence delivered after SyncComplete is the
            // deterministic live projection probe for this exact principal.
            const bool liveParity = snapshotParity && std::any_of(
                pair.oldObservations.messages.begin(), pair.oldObservations.messages.end(), [](const frontend::ServerMessage& message) {
                    return std::holds_alternative<frontend::EventBatch>(message);
                });

            OldObservations replayOld;
            NewObservations replayNew;
            frontend::FrontendConnection oldReplay = pair.oldServer->openConnection(remotePeer(), oldCallbacks(replayOld));
            const auto newReplay = pair.newServer->openConnection(remotePeer(), newCallbacks(replayNew));
            frontend::Hello replayHello;
            replayHello.resumeAfter = frontend::SequenceNumber{0};
            replayHello.capabilities = representationCapabilities;
            replayHello.authentication = bearerCredential();
            const auto oldReplayReceive = oldReplay.receive(frontend::ClientMessage{replayHello});
            const auto newReplayReceive = pair.newServer->receive(*newReplay, frontend::ClientMessage{std::move(replayHello)});
            pair.drain();
            const bool replayParity = sameReceiveDisposition(oldReplayReceive, newReplayReceive) &&
                                      compareMessages(result, replayOld.messages, replayNew.messages, "scope replay " + scopeName) &&
                                      compareCloses(result, replayOld, replayNew, "scope replay " + scopeName);

            const generated::MethodMetadata* method = nullptr;
            for (const generated::MethodMetadata& candidate : generated::AllMethods) {
                if (std::find(candidate.requiredScopes.begin(), candidate.requiredScopes.end(), *scope) !=
                    candidate.requiredScopes.end()) {
                    method = &candidate;
                    break;
                }
            }
            bool methodParity = method != nullptr;
            if (method) {
                pair.clearObservations();
                const std::string requestId = "scope-method-" + scopeName;
                const auto receive = pair.command(definedCommand(fixture, method->id, requestId));
                methodParity = sameReceiveDisposition(receive.first, receive.second) &&
                               comparePair(result, pair, "scope method " + scopeName);
            }

            static_cast<void>(recordCase(coverageCase("scope", scopeName, "snapshot"), snapshotParity));
            static_cast<void>(recordCase(coverageCase("scope", scopeName, "live"), liveParity));
            static_cast<void>(recordCase(coverageCase("scope", scopeName, "replay"), replayParity));
            static_cast<void>(recordCase(coverageCase("scope", scopeName, "method"), methodParity));
            result.expectTrue(snapshotParity && liveParity && replayParity && methodParity,
                              "scope-specific snapshot/live/replay/method differential for " + scopeName);
        }
    }

    void testSecurityControllerAndDiscovery(tests::support::TestResult& result, const frontend::Json& fixture) {
        PairSettings observeSettings;
        observeSettings.peer = remotePeer();
        observeSettings.oldAuthenticator = observeOnlyAuthentication;
        observeSettings.newAuthenticator = observeOnlyAuthentication;
        Pair observeOnly(std::move(observeSettings));
        observeOnly.hello({}, std::nullopt, bearerCredential());
        result.expectTrue(comparePair(result, observeOnly, "observe-only discovery"),
                          "Welcome capability, available-method, and permitted-method discovery is exact for observe-only scope");
        observeOnly.clearObservations();
        const auto deniedAcquire =
            observeOnly.command(definedCommand(fixture, generated::MethodId::ControllerAcquire, "denied-control-scope"));
        result.expectTrue(sameReceiveDisposition(deniedAcquire.first, deniedAcquire.second) &&
                              comparePair(result, observeOnly, "control-scope denial"),
                          "scope possession remains separate from controller ownership with the exact denial response");

        PairSettings controlSettings;
        controlSettings.peer = remotePeer();
        controlSettings.oldAuthenticator = remoteControlAuthentication;
        controlSettings.newAuthenticator = remoteControlAuthentication;
        Pair controllerRequired(std::move(controlSettings));
        controllerRequired.hello({}, std::nullopt, bearerCredential());
        controllerRequired.clearObservations();
        const auto controllerDenied =
            controllerRequired.command(definedCommand(fixture, generated::MethodId::ThreadStart, "controller-required"));
        result.expectTrue(sameReceiveDisposition(controllerDenied.first, controllerDenied.second) &&
                              comparePair(result, controllerRequired, "controller requirement"),
                          "control scope does not imply ownership and both servers emit the exact controller-required denial");

        controllerRequired.clearObservations();
        static_cast<void>(controllerRequired.command(
            definedCommand(fixture, generated::MethodId::ControllerAcquire, "first-controller")));
        OldObservations secondOld;
        NewObservations secondNew;
        frontend::FrontendConnection secondOldConnection =
            controllerRequired.oldServer->openConnection(remotePeer(), oldCallbacks(secondOld));
        const auto secondNewConnection = controllerRequired.newServer->openConnection(remotePeer(), newCallbacks(secondNew));
        frontend::Hello secondHello;
        secondHello.authentication = bearerCredential();
        const auto secondOldHello = secondOldConnection.receive(frontend::ClientMessage{secondHello});
        const auto secondNewHello =
            controllerRequired.newServer->receive(*secondNewConnection, frontend::ClientMessage{std::move(secondHello)});
        controllerRequired.drain();
        secondOld.messages.clear();
        secondNew.messages.clear();
        const generated::DefinedCommand conflict =
            definedCommand(fixture, generated::MethodId::ControllerAcquire, "controller-conflict");
        const auto conflictJson = frontend::Codec::encodeDefinedCommand(conflict);
        const auto secondOldConflict = secondOldConnection.receive(conflictJson.value());
        const auto secondNewConflict = controllerRequired.newServer->receiveDefinedCommand(*secondNewConnection, conflict);
        controllerRequired.drain();
        result.expectTrue(sameReceiveDisposition(secondOldHello, secondNewHello) &&
                              sameReceiveDisposition(secondOldConflict, secondNewConflict) &&
                              compareMessages(result, secondOld.messages, secondNew.messages, "controller conflict") &&
                              compareCloses(result, secondOld, secondNew, "controller conflict"),
                          "controller conflict response and role behavior are exact across two deterministic sessions");

        PairSettings policySettings;
        policySettings.enableFilesystemRead = true;
        policySettings.oldFilesystemReadPolicy =
            [](const frontend::FrontendPrincipal&, std::string_view, const frontend::Json&) {
                return false;
            };
        policySettings.newFilesystemReadPolicy = policySettings.oldFilesystemReadPolicy;
        Pair policy(std::move(policySettings));
        policy.hello();
        policy.clearObservations();
        const auto deniedPolicy = policy.command(definedCommand(fixture, generated::MethodId::FsReadFile, "policy-denied"));
        result.expectTrue(sameReceiveDisposition(deniedPolicy.first, deniedPolicy.second) &&
                              comparePair(result, policy, "invocation policy denial"),
                          "generated filesystem classification and injected invocation policy produce the exact denial");
    }

    void testBoundsAndBackpressure(tests::support::TestResult& result, const frontend::Json& fixture) {
        PairSettings rejectedSendSettings;
        rejectedSendSettings.acceptMessages = false;
        Pair rejectedSend(std::move(rejectedSendSettings));
        const auto rejectedHello = rejectedSend.receive(frontend::ClientMessage{frontend::Hello{}});
        result.expectTrue(sameReceiveDisposition(rejectedHello.first, rejectedHello.second) &&
                              comparePair(result, rejectedSend, "transport send rejection"),
                          "a transport send rejection closes only the affected connection with the exact backpressure reason");

        PairSettings queueSettings;
        queueSettings.maxOutboundMessages = 1;
        Pair queueBound(std::move(queueSettings));
        const auto queueHello = queueBound.receive(frontend::ClientMessage{frontend::Hello{}});
        const bool queueDisposition = sameReceiveDisposition(queueHello.first, queueHello.second);
        const bool queueBorder = comparePair(result, queueBound, "outbound queue bound");
        static_cast<void>(recordCase("queue:outbound-bound:server", queueDisposition && queueBorder));
        result.expectTrue(queueDisposition && queueBorder,
                          "the one-message outbound queue bound has the exact terminal behavior; status=" +
                              std::to_string(static_cast<int>(queueHello.first.status)) + "/" +
                              std::to_string(static_cast<int>(queueHello.second.status)) + "; old=" +
                              messageSequenceSummary(queueBound.oldObservations.messages) + "; new=" +
                              messageSequenceSummary(queueBound.newObservations.messages));

        PairSettings inboundSettings;
        inboundSettings.maximumInboundMessageBytes = 32;
        Pair inboundBound(std::move(inboundSettings));
        const std::string oversized(128, 'x');
        const auto oversizedReceive = inboundBound.receive(std::string_view{oversized});
        result.expectTrue(sameReceiveDisposition(oversizedReceive.first, oversizedReceive.second) &&
                              comparePair(result, inboundBound, "inbound frame bound"),
                          "oversized inbound messages produce the exact frame-too-large response and close policy");

        PairSettings rateSettings;
        rateSettings.maxInboundMessagesPerSecond = 1;
        rateSettings.maxInboundBurst = 1;
        Pair rateLimited(std::move(rateSettings));
        rateLimited.hello();
        rateLimited.clearObservations();
        const auto rateReceive = rateLimited.command(definedCommand(fixture, generated::MethodId::SnapshotGet, "rate-limited"));
        result.expectTrue(sameReceiveDisposition(rateReceive.first, rateReceive.second) &&
                              comparePair(result, rateLimited, "rate limit"),
                          "deterministic monotonic time produces the exact rate-limit terminal policy");

    }
} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    tests::codex::FrontendDifferentialExecutionLedger ledger{"server"};
    executionLedger = &ledger;
    core::SNodeC::init(argc, argv);
    try {
        const frontend::Json fixture = loadFixture();
        const frontend::Json coverage = loadCoverageFixture();
        testHandshakeAndProtocolFailures(result, fixture);
        testAllGeneratedMethods(result, fixture, coverage);
        testSnapshotLiveReplayRepresentations(result, fixture);
        testAuthorityProjectionCorpus(result, fixture, coverage);
        testScopeAndCapabilityCorpus(result, fixture, coverage);
        testSecurityControllerAndDiscovery(result, fixture);
        testBoundsAndBackpressure(result, fixture);

        bool timedOut = false;
        ReadyBackendDifferentialRunner readyRunner(result, fixture);
        [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
            [&timedOut]() {
                timedOut = true;
                core::SNodeC::stop();
            },
            utils::Timeval({10, 0}));
        readyRunner.start();
        const int eventLoopResult = core::SNodeC::start(utils::Timeval({12, 0}));
        result.expectTrue(!timedOut,
                          "ready-backend differential completes before its watchdog (stage: " + readyRunner.waitingStage() + ")");
        result.expectTrue(readyRunner.isFinished(), "ready-backend live/replay differential reaches a terminal state");
        result.expectEqual(0, eventLoopResult, "ready-backend differential event loop exits cleanly");
    } catch (const std::exception& error) {
        result.expectTrue(false, std::string("server differential harness failure: ") + error.what());
    }
    core::SNodeC::free();
    executionLedger = nullptr;
    const int status = result.processResult();
    if (status == 0) {
        try {
            ledger.write(AISUITE_CODEX_FRONTEND_SERVER_DIFFERENTIAL_LEDGER);
        } catch (const std::exception& error) {
            std::cerr << "CodexFrontendServerDifferentialTest: cannot write execution ledger: " << error.what() << '\n';
            return 1;
        }
    }
    return status;
}
