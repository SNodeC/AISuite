/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/backend/internal/ProviderOperationSupport.h"
#include "ai/openai/codex/backend/internal/ReverseResponsePreflight.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace typed = ai::openai::codex::typed;

    using ai::openai::codex::Diagnostic;
    using ai::openai::codex::Error;
    using ai::openai::codex::Json;
    using ai::openai::codex::detail::TransportCallbacks;

    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;

    std::set<std::string> stableApplicationMethods() {
        std::set<std::string> methods;
        for (const ai::openai::codex::detail::ProtocolSurfaceEntry& entry : ai::openai::codex::detail::protocolSurfaceRegistry()) {
            if (entry.stability == ai::openai::codex::detail::Stability::Stable &&
                entry.key.category == ai::openai::codex::detail::SurfaceCategory::ClientRequest && entry.key.name != "initialize") {
                methods.emplace(entry.key.name);
            }
        }
        return methods;
    }

    void testProviderOperationHelperEdges(tests::support::TestResult& result) {
        typed::OperationResult<typed::Unit> valueLessSuccess;
        const backend::CommandResult missing = backend::detail::providerOperationFailure(valueLessSuccess);
        result.expectTrue(missing.error && missing.error->code == backend::CommandErrorCode::TypedDecodingFailure,
                          "a value-less Success result maps to typed_decoding_failure rather than an empty success");

        typed::OperationResult<typed::Unit> cancelled;
        cancelled.kind = typed::OperationResult<typed::Unit>::Kind::Cancelled;
        cancelled.localError = Error{Error::Category::Cancelled, ECANCELED, "synthetic callback cancellation"};
        const backend::CommandResult cancellation = backend::detail::providerOperationFailure(cancelled);
        result.expectTrue(cancellation.error && cancellation.error->code == backend::CommandErrorCode::Cancelled &&
                              cancellation.error->message == "synthetic callback cancellation",
                          "a callback-delivered Cancelled result preserves the stable cancelled command error");

        typed::OperationResult<typed::Unit> decodingFailure;
        decodingFailure.kind = typed::OperationResult<typed::Unit>::Kind::LocalError;
        decodingFailure.localError = Error{Error::Category::Protocol, EPROTO, "synthetic typed decoding failure"};
        const backend::CommandResult decoded = backend::detail::providerOperationFailure(decodingFailure);
        result.expectTrue(decoded.error && decoded.error->code == backend::CommandErrorCode::TypedDecodingFailure,
                          "a protocol-local callback failure maps to typed_decoding_failure");

        typed::FsReadFileResponse large;
        large.dataBase64.assign(4'096, 'A');
        large.raw = Json{{"dataBase64", large.dataBase64}};
        const std::size_t rawBytes = large.raw.dump().size();
        const std::size_t retainedBytes = backend::detail::providerOperationRetainedBytes(large);
        result.expectTrue(retainedBytes >= sizeof(typed::FsReadFileResponse) + rawBytes * 2,
                          "large exact results account for both retained raw JSON and their decoded typed projection");

        const backend::ProviderOperationValue exactVariant{large};
        result.expectEqual(retainedBytes,
                           backend::detail::providerOperationRetainedBytes(exactVariant),
                           "variant result accounting visits the exact provider-result alternative");
        result.expectEqual(std::size_t{0},
                           backend::detail::providerOperationRetainedBytes(typed::Unit{}),
                           "Unit results add no variable command-completion bytes");
    }

    void testReverseResponseSequencePreflight(tests::support::TestResult& result) {
        backend::BackendState state;
        state.sequence = backend::SequenceNumber{std::numeric_limits<std::uint64_t>::max()};
        const backend::PendingRequestId pendingId{1};
        typed::UnknownServerRequest request{ai::openai::codex::ServerRequestId{std::int64_t{1}},
                                            ai::openai::codex::ServerRequestToken{1},
                                            "future/sequence-preflight",
                                            Json::object(),
                                            Json::object(),
                                            std::nullopt};
        state.pendingRequests.emplace(pendingId, backend::PendingRequestState{pendingId, typed::TypedServerRequest{std::move(request)}, 0});

        std::size_t typedResponseCalls = 0;
        ai::openai::codex::SendResult send;
        const backend::detail::ReverseResponsePreflightStatus status =
            backend::detail::submitReverseResponseIfSequenceAvailable(state, send, [&]() {
                ++typedResponseCalls;
                return ai::openai::codex::SendResult{true, std::nullopt};
            });

        result.expectTrue(status == backend::detail::ReverseResponsePreflightStatus::SequenceUnavailable && typedResponseCalls == 0,
                          "reverse-response sequence preflight does not invoke typed respond/reject when the backend sequence is "
                          "exhausted");
        result.expectTrue(state.pendingRequests.contains(pendingId) && state.pendingRequests.size() == 1,
                          "reverse-response sequence preflight retains pending occurrence ownership when retirement cannot be "
                          "sequenced");
    }

    void testCompleteProviderCommandPolicy(tests::support::TestResult& result) {
        struct Entry {
            backend::BackendCommand command;
            backend::CommandAccess access = backend::CommandAccess::Controller;
            bool stateful = false;
            std::string method;
        };

        std::vector<Entry> entries;
        entries.reserve(86);
#define CODEX_BACKEND_PROVIDER_OPERATION(COMMAND, RESULT, DOMAIN, METHOD, ACCESS, STATEFUL, WIRE_METHOD)                                   \
    entries.push_back({backend::COMMAND{}, backend::CommandAccess::ACCESS, STATEFUL, WIRE_METHOD});
#define CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(COMMAND, RESULT, DOMAIN, METHOD, ACCESS, STATEFUL, WIRE_METHOD)                             \
    CODEX_BACKEND_PROVIDER_OPERATION(COMMAND, RESULT, DOMAIN, METHOD, ACCESS, STATEFUL, WIRE_METHOD)
#include "ai/openai/codex/backend/internal/ProviderOperations.inc"
#undef CODEX_BACKEND_PROVIDER_OPERATION_EMPTY
#undef CODEX_BACKEND_PROVIDER_OPERATION

        result.expectEqual(std::size_t{86}, entries.size(), "the private dispatch ledger covers exactly 86 stable provider operations");
        std::set<std::string> methods;
        std::set<std::string> actionOnlyMethods;
        std::set<std::string> observerMethods;
        std::size_t stateful = 0;
        std::size_t observerReadable = 0;
        for (const Entry& entry : entries) {
            methods.insert(entry.method);
            stateful += entry.stateful ? 1U : 0U;
            if (!entry.stateful) {
                actionOnlyMethods.insert(entry.method);
            }
            observerReadable += entry.access == backend::CommandAccess::Observer ? 1U : 0U;
            if (entry.access == backend::CommandAccess::Observer) {
                observerMethods.insert(entry.method);
            }
            const backend::CommandPolicy policy = backend::commandPolicy(entry.command);
            result.expectTrue(policy.access == entry.access && policy.requiresProviderReady,
                              "every provider command has its explicit access and Ready policy: " + entry.method);
        }
        const std::set<std::string> expectedObserverMethods{
            "account/rateLimits/read",
            "account/read",
            "account/usage/read",
            "account/workspaceMessages/read",
            "app/list",
            "config/read",
            "configRequirements/read",
            "experimentalFeature/list",
            "externalAgentConfig/detect",
            "externalAgentConfig/import/readHistories",
            "fs/getMetadata",
            "fs/readDirectory",
            "fs/readFile",
            "fuzzyFileSearch",
            "hooks/list",
            "mcpServer/resource/read",
            "mcpServerStatus/list",
            "model/list",
            "modelProvider/capabilities/read",
            "permissionProfile/list",
            "plugin/installed",
            "plugin/list",
            "plugin/read",
            "plugin/share/list",
            "plugin/skill/read",
            "skills/list",
            "thread/goal/get",
            "thread/list",
            "thread/loaded/list",
            "thread/read",
            "windowsSandbox/readiness",
        };
        const std::set<std::string> expectedActionOnlyMethods{
            "account/sendAddCreditsNudgeEmail",
            "feedback/upload",
            "fs/copy",
            "fs/createDirectory",
            "fs/getMetadata",
            "fs/readDirectory",
            "fs/readFile",
            "fs/remove",
            "fs/writeFile",
            "mcpServer/resource/read",
            "mcpServer/tool/call",
            "thread/shellCommand",
            "turn/interrupt",
        };
        const std::set<std::string> registryMethods = stableApplicationMethods();
        result.expectTrue(methods.size() == 86 && registryMethods.size() == 86 && methods == registryMethods,
                          "the provider dispatch ledger is an exact bijection with the independent stable application-request registry");
        result.expectEqual(std::size_t{73}, stateful, "the provider dispatch ledger has exactly 73 stateful operations");
        result.expectTrue(actionOnlyMethods.size() == 13 && actionOnlyMethods == expectedActionOnlyMethods,
                          "the provider dispatch ledger uses exactly the independently frozen 13 action-only operations");
        result.expectTrue(observerReadable == expectedObserverMethods.size() && observerMethods == expectedObserverMethods,
                          "the trusted BackendCore observer-readable policy matches the independently frozen exact 31-operation set");
        result.expectTrue(methods.contains("mcpServer/oauth/login") && !methods.contains("mcpServer/oauthLogin"),
                          "the provider command ledger uses the exact MCP OAuth request method");

        backend::AccountRead refreshing;
        refreshing.params.refreshToken = true;
        result.expectTrue(backend::commandPolicy(backend::BackendCommand{refreshing}).access == backend::CommandAccess::Controller,
                          "account/read with refreshToken=true is controller-only");
        refreshing.params.refreshToken = false;
        result.expectTrue(backend::commandPolicy(backend::BackendCommand{refreshing}).access == backend::CommandAccess::Observer,
                          "account/read without token refresh is observer-readable");

        const std::vector<backend::BackendCommand> responses = {backend::ApprovalRespond{},
                                                                backend::UserInputRespond{},
                                                                backend::AuthenticationRespond{},
                                                                backend::UnknownRequestRespondRaw{},
                                                                backend::UnknownRequestReject{},
                                                                backend::ApplyPatchApprovalRespond{},
                                                                backend::ExecCommandApprovalRespond{},
                                                                backend::PermissionsApprovalRespond{},
                                                                backend::AttestationGenerateRespond{},
                                                                backend::DynamicToolCallRespond{},
                                                                backend::McpServerElicitationRespond{},
                                                                backend::KnownRequestReject{}};
        for (const backend::BackendCommand& response : responses) {
            const backend::CommandPolicy policy = backend::commandPolicy(response);
            result.expectTrue(policy.access == backend::CommandAccess::Controller && policy.requiresProviderReady,
                              "every reverse-request response requires controller ownership and Ready");
        }
        for (const backend::BackendCommand& control : {backend::BackendCommand{backend::ControllerAcquire{}},
                                                       backend::BackendCommand{backend::ControllerRelease{}},
                                                       backend::BackendCommand{backend::SnapshotGet{}}}) {
            const backend::CommandPolicy policy = backend::commandPolicy(control);
            result.expectTrue(policy.access == backend::CommandAccess::Observer && !policy.requiresProviderReady,
                              "control commands remain observer-accessible without provider readiness");
        }
    }

    Json agentItemValue(const std::string& id, const std::string& text = {}) {
        return {{"type", "agentMessage"}, {"id", id}, {"text", text}};
    }

    typed::TextInput textInput(std::string text) {
        typed::TextInput input;
        input.text = std::move(text);
        return input;
    }

    class BackendCoreConstructionClient;

    struct ClientOwnershipProbe {
        std::shared_ptr<tests::codex::FakeTransportState> transport = std::make_shared<tests::codex::FakeTransportState>();
        BackendCoreConstructionClient* client = nullptr;
        std::size_t constructions = 0;
        std::size_t destructions = 0;
        int forwardedValue = 0;
        bool runtimeStoppedClientBeforeDestruction = false;
    };

    class BackendCoreConstructionClient final : public ai::openai::codex::AppServerClient {
    public:
        BackendCoreConstructionClient()
            : AppServerClient(std::make_unique<tests::codex::FakeTransport>(std::make_shared<tests::codex::FakeTransportState>()),
                              {"backend_core_default_test", "BackendCore Default Test", "1"}) {
            ++defaultConstructions;
            lastDefaultClient = this;
        }

        BackendCoreConstructionClient(std::shared_ptr<ClientOwnershipProbe>& probe, std::unique_ptr<int> forwardedValue)
            : AppServerClient(std::make_unique<tests::codex::FakeTransport>(probe->transport),
                              {"backend_core_forwarding_test", "BackendCore Forwarding Test", "1"})
            , probe(probe)
            , forwardedValue(std::move(forwardedValue)) {
            probe->client = this;
            ++probe->constructions;
            probe->forwardedValue = this->forwardedValue ? *this->forwardedValue : 0;
        }

        ~BackendCoreConstructionClient() override {
            if (probe) {
                ++probe->destructions;
                probe->runtimeStoppedClientBeforeDestruction =
                    getState() == ai::openai::codex::State::Stopping || getState() == ai::openai::codex::State::Stopped;
                probe->client = nullptr;
            } else {
                ++defaultDestructions;
                lastDefaultClient = nullptr;
            }
        }

        static inline std::size_t defaultConstructions = 0;
        static inline std::size_t defaultDestructions = 0;
        static inline BackendCoreConstructionClient* lastDefaultClient = nullptr;

    private:
        std::shared_ptr<ClientOwnershipProbe> probe;
        std::unique_ptr<int> forwardedValue;
    };

    void testTemplatedConstructionAndOwnership(tests::support::TestResult& result) {
        using ProbeBackendCore = backend::BackendCore<BackendCoreConstructionClient>;

        BackendCoreConstructionClient::defaultConstructions = 0;
        BackendCoreConstructionClient::defaultDestructions = 0;
        BackendCoreConstructionClient::lastDefaultClient = nullptr;
        {
            ProbeBackendCore backendCore;
            result.expectTrue(BackendCoreConstructionClient::defaultConstructions == 1,
                              "BackendCore default-constructs its directly owned default-constructible client");
            result.expectTrue(BackendCoreConstructionClient::lastDefaultClient != nullptr,
                              "BackendCore directly owns the default-constructed concrete client subobject");
        }
        result.expectTrue(BackendCoreConstructionClient::defaultDestructions == 1,
                          "BackendCore destroys its default-constructed client exactly once");

        auto noOptionsProbe = std::make_shared<ClientOwnershipProbe>();
        auto noOptionsValue = std::make_unique<int>(21);
        {
            ProbeBackendCore backendCore(noOptionsProbe, std::move(noOptionsValue));
            result.expectTrue(!noOptionsValue && noOptionsProbe->client != nullptr && noOptionsProbe->forwardedValue == 21,
                              "BackendCore perfectly forwards client arguments without backend options");
        }
        result.expectTrue(noOptionsProbe->destructions == 1 && noOptionsProbe->client == nullptr,
                          "BackendCore destroys its forwarded client exactly once without exposing mutable access");

        auto optionsProbe = std::make_shared<ClientOwnershipProbe>();
        auto optionsValue = std::make_unique<int>(42);
        backend::BackendCoreOptions options;
        options.initialThreadListLimit = 7;
        {
            ProbeBackendCore backendCore(std::move(options), optionsProbe, std::move(optionsValue));
            result.expectTrue(!optionsValue && optionsProbe->constructions == 1 && optionsProbe->forwardedValue == 42,
                              "BackendCore perfectly forwards lvalue and move-only arguments after backend options");

            backendCore.start();
            result.expectTrue(optionsProbe->client != nullptr && optionsProbe->client->getState() == ai::openai::codex::State::Starting,
                              "BackendCore lifecycle methods operate on the directly owned concrete client");
        }
        result.expectTrue(optionsProbe->destructions == 1 && optionsProbe->runtimeStoppedClientBeforeDestruction &&
                              optionsProbe->client == nullptr,
                          "BackendCore shuts down its non-template runtime before destroying the owned client");
    }

    class BackendCoreRunner {
    public:
        static constexpr std::size_t LargeResultQueueBytes = 256U * 1024U;
        static constexpr std::size_t LargeResultPayloadBytes = 192U * 1024U;

        explicit BackendCoreRunner(tests::support::TestResult& result)
            : result(result) {
        }

        void start() {
            loadProviderResultFixtures();
            transport = std::make_shared<tests::codex::FakeTransportState>();
            tests::codex::installInitializingFake(transport, [this](const Json& message, const TransportCallbacks& callbacks) {
                handleOutgoing(message, callbacks);
            });

            backend::BackendCoreOptions options;
            options.initialThreadListLimit = 2;
            options.maxEventsPerCallback = 32;
            options.maxSessionQueueBytes = LargeResultQueueBytes;
            backendCore = std::make_unique<FakeBackendCore>(std::move(options), transport, &appServerClient);

            controller = backendCore->openSession(sessionCallbacks(controllerEvents, false));
            observer = backendCore->openSession(sessionCallbacks(observerEvents, true));
            expect(controller.role() == backend::SessionRole::Observer && observer.role() == backend::SessionRole::Observer,
                   "BackendCore sessions start as observers before lifecycle startup");
            expect(static_cast<bool>(controller.submit("acquire-initial", backend::ControllerAcquire{})),
                   "initial controller acquisition is accepted before backend readiness");

            backendCore->start();
            waitUntil(
                "backend reaches Ready and completes exactly one bounded initial refresh",
                [this]() {
                    const backend::Snapshot snapshot = backendCore->snapshot();
                    return backendCore->isReady() && snapshot.threadList.pagesLoaded >= 1 && completionCount("acquire-initial") == 1;
                },
                [this]() {
                    verifyInitialHydration();
                    beginCompleteProviderDispatchAudit();
                });
        }

        bool isFinished() const noexcept {
            return finished;
        }

    private:
        struct ProviderDispatchAuditEntry {
            backend::BackendCommand command;
            std::string method;
            std::function<bool(const backend::CommandValue&)> hasExactResult;
            bool stateful = false;
        };

        void loadProviderResultFixtures() {
            try {
                std::filesystem::path repositoryRoot = std::filesystem::path{__FILE__};
                for (std::size_t parent = 0; parent < 4; ++parent) {
                    repositoryRoot = repositoryRoot.parent_path();
                }
                const std::filesystem::path fixtureRoot = repositoryRoot / "tools/codex/app-server-fixtures/0.144.6";
                std::ifstream indexInput(fixtureRoot / "index.json");
                Json index;
                indexInput >> index;
                const std::set<std::string> expectedMethods = stableApplicationMethods();
                for (const Json& fixture : index.at("fixtures")) {
                    if (fixture.value("role", "") != "client_request_result") {
                        continue;
                    }
                    const Json& key = fixture.at("protocol_surface_key");
                    const std::string method = key.value("name", "");
                    if (key.value("category", "") != "client_request" || !expectedMethods.contains(method)) {
                        continue;
                    }
                    std::ifstream fixtureInput(fixtureRoot / fixture.at("file").get<std::string>());
                    Json value;
                    fixtureInput >> value;
                    providerResultFixtures.insert_or_assign(method, std::move(value));
                }
                expect(providerResultFixtures.size() == 86,
                       "the runtime dispatch audit loaded one independently schema-validated result fixture for every stable provider "
                       "operation");
            } catch (const std::exception& error) {
                expect(false, std::string{"the runtime dispatch audit could not load the checked-in result fixtures: "} + error.what());
            }
        }

        struct EventLog {
            std::vector<std::vector<std::uint64_t>> batches;
            std::vector<backend::ProviderLifecycleChanged> lifecycles;
            std::vector<std::uint64_t> invalidations;
            std::vector<std::pair<std::uint64_t, backend::PendingRequestRemoved>> pendingRequestRemovals;
            std::vector<backend::CodexExtensionReceived> extensions;
            std::size_t snapshots = 0;
        };

        static void defer(std::function<void()> callback) {
            core::EventReceiver::atNextTick(std::move(callback));
        }

        void expect(bool condition, const std::string& message) {
            result.expectTrue(condition, message);
        }

        void
        waitUntil(std::string description, std::function<bool()> predicate, std::function<void()> next, std::size_t remaining = 4'000) {
            defer([this,
                   description = std::move(description),
                   predicate = std::move(predicate),
                   next = std::move(next),
                   remaining]() mutable {
                if (finished) {
                    return;
                }
                if (predicate()) {
                    next();
                    return;
                }
                if (remaining == 0) {
                    expect(false, description);
                    finish();
                    return;
                }
                waitUntil(std::move(description), std::move(predicate), std::move(next), remaining - 1);
            });
        }

        void afterTicks(std::size_t count, std::function<void()> next) {
            if (count == 0) {
                next();
                return;
            }
            defer([this, count, next = std::move(next)]() mutable {
                afterTicks(count - 1, std::move(next));
            });
        }

        backend::FrontendSessionCallbacks sessionCallbacks(EventLog& log, bool stopOnCommand) {
            return backend::FrontendSessionCallbacks{
                [this, &log](const std::vector<backend::SequencedBackendEvent>& events) {
                    std::vector<std::uint64_t> sequences;
                    sequences.reserve(events.size());
                    for (const backend::SequencedBackendEvent& event : events) {
                        sequences.push_back(event.sequence.value());
                        if (const auto* lifecycle = std::get_if<backend::ProviderLifecycleChanged>(&event.event)) {
                            log.lifecycles.push_back(*lifecycle);
                        } else if (std::holds_alternative<backend::ProviderConnectionInvalidated>(event.event)) {
                            log.invalidations.push_back(event.sequence.value());
                        } else if (const auto* removal = std::get_if<backend::PendingRequestRemoved>(&event.event)) {
                            log.pendingRequestRemovals.emplace_back(event.sequence.value(), *removal);
                        } else if (const auto* extension = std::get_if<backend::CodexExtensionReceived>(&event.event)) {
                            log.extensions.push_back(*extension);
                        }
                    }
                    expect(sequences.size() <= 32, "BackendCore event callback obeys maxEventsPerCallback");
                    expect(std::is_sorted(sequences.begin(), sequences.end()), "BackendCore event callback preserves sequence order");
                    log.batches.push_back(std::move(sequences));
                },
                [&log](const backend::Snapshot&) {
                    ++log.snapshots;
                },
                [this, stopOnCommand](const backend::CommandCompletion& completion) {
                    completions.insert_or_assign(completion.requestId, completion);
                    ++completionCounts[completion.requestId];

                    if (completion.requestId == "start-success" && completionCounts[completion.requestId] == 1) {
                        backend::TurnStart command;
                        command.params.threadId = typed::ThreadId{"thread-success"};
                        command.params.input = {textInput("submitted reentrantly")};
                        expect(static_cast<bool>(controller.submit("turn-reentrant", std::move(command))),
                               "a command completion callback can submit another typed command reentrantly");
                    }
                    if (stopOnCommand && completion.requestId == "stop-now" && backendCore) {
                        backendCore->stop();
                    }
                },
                [this](const std::string&) {
                    ++closedCallbacks;
                }};
        }

        std::size_t completionCount(const std::string& requestId) const {
            const auto iterator = completionCounts.find(requestId);
            return iterator == completionCounts.end() ? 0 : iterator->second;
        }

        const backend::CommandCompletion* completion(const std::string& requestId) const {
            const auto iterator = completions.find(requestId);
            return iterator == completions.end() ? nullptr : &iterator->second;
        }

        bool hasSuccess(const std::string& requestId) const {
            const backend::CommandCompletion* value = completion(requestId);
            return value && !value->result.error;
        }

        bool hasError(const std::string& requestId, backend::CommandErrorCode code) const {
            const backend::CommandCompletion* value = completion(requestId);
            return value && value->result.error && value->result.error->code == code;
        }

        std::uint64_t lastSequence(const EventLog& log) const {
            std::uint64_t last = 0;
            for (const auto& batch : log.batches) {
                if (!batch.empty()) {
                    last = std::max(last, batch.back());
                }
            }
            return last;
        }

        std::vector<std::uint64_t> sequencesAfter(const EventLog& log, std::uint64_t sequence) const {
            std::vector<std::uint64_t> values;
            for (const auto& batch : log.batches) {
                for (const std::uint64_t value : batch) {
                    if (value > sequence) {
                        values.push_back(value);
                    }
                }
            }
            return values;
        }

        void handleOutgoing(const Json& message, const TransportCallbacks& callbacks) {
            const auto method = message.find("method");
            const auto id = message.find("id");
            if (method == message.end() || !method->is_string() || id == message.end()) {
                return;
            }

            const std::string methodName = method->get<std::string>();
            const Json params = message.value("params", Json::object());
            if (providerDispatchAuditIndex < providerDispatchAudit.size() &&
                providerDispatchAudit[providerDispatchAuditIndex].method == methodName) {
                const auto fixture = providerResultFixtures.find(methodName);
                expect(fixture != providerResultFixtures.end(), methodName + " has a checked-in successful result fixture");
                if (fixture != providerResultFixtures.end()) {
                    tests::codex::inject(callbacks, Json{{"id", *id}, {"result", fixture->second}});
                }
                return;
            }
            if (methodName == "fs/readFile" && params.value("path", "") == "/synthetic/large-result") {
                tests::codex::inject(callbacks,
                                     Json{{"id", *id}, {"result", Json{{"dataBase64", std::string(LargeResultPayloadBytes, 'A')}}}});
            } else if (methodName == "thread/list") {
                ++threadListRequests;
                if (!params.contains("cursor")) {
                    if (params.value("limit", 0U) == 2U) {
                        ++boundedInitialRefreshes;
                    }
                    tests::codex::inject(callbacks,
                                         Json{{"id", *id},
                                              {"result",
                                               {{"data", Json::array({tests::codex::threadValue("thread-initial")})},
                                                {"nextCursor", "initial-next"},
                                                {"backwardsCursor", nullptr}}}});
                } else {
                    tests::codex::inject(callbacks,
                                         Json{{"id", *id},
                                              {"result",
                                               {{"data", Json::array({tests::codex::threadValue("thread-page")})},
                                                {"nextCursor", nullptr},
                                                {"backwardsCursor", "explicit-before"}}}});
                }
            } else if (methodName == "thread/start") {
                const std::string cwd = params.value("cwd", "");
                if (cwd == "/malformed") {
                    tests::codex::inject(callbacks, Json{{"id", *id}, {"result", Json{{"malformed", true}}}});
                } else if (cwd == "/deferred-close") {
                    deferredClosedCallbacks = callbacks;
                    deferredClosedId = *id;
                } else if (cwd == "/old-generation") {
                    staleCallbacks = callbacks;
                    staleId = *id;
                } else if (cwd == "/destroyed-backend") {
                    destroyedCallbacks = callbacks;
                    destroyedId = *id;
                } else {
                    const std::string threadId = cwd == "/fresh"              ? "thread-fresh"
                                                 : cwd == "/unsolicited-stop" ? "thread-unsolicited-scheduled-completion"
                                                                              : "thread-success";
                    tests::codex::inject(callbacks, Json{{"id", *id}, {"result", tests::codex::threadOperationResult(threadId)}});
                }
            } else if (methodName == "thread/resume") {
                if (params.value("threadId", "") == "thread-successful-resume") {
                    tests::codex::inject(callbacks,
                                         Json{{"id", *id}, {"result", tests::codex::threadOperationResult("thread-successful-resume")}});
                } else {
                    tests::codex::inject(
                        callbacks,
                        Json{{"id", *id},
                             {"error", {{"code", -32'010}, {"message", "deterministic remote failure"}, {"data", {{"source", "fake"}}}}}});
                }
            } else if (methodName == "thread/read") {
                const std::string threadId = params.value("threadId", "thread-read");
                const Json turns = Json::array(
                    {tests::codex::turnValue(threadId, "turn-read", "completed", Json::array({agentItemValue("item-read", "read text")}))});
                tests::codex::inject(callbacks, Json{{"id", *id}, {"result", {{"thread", tests::codex::threadValue(threadId, turns)}}}});
            } else if (methodName == "turn/start") {
                const std::string turnId = params.value("input", Json::array()).empty() ? "turn-start" : "turn-reentrant";
                tests::codex::inject(callbacks, Json{{"id", *id}, {"result", tests::codex::turnOperationResult("thread-success", turnId)}});
            } else if (methodName == "turn/interrupt") {
                tests::codex::inject(callbacks, Json{{"id", *id}, {"result", Json::object()}});
            }
        }

        void beginCompleteProviderDispatchAudit() {
            providerDispatchAudit.reserve(86);
#define CODEX_BACKEND_PROVIDER_OPERATION(COMMAND, RESULT, DOMAIN, METHOD, ACCESS, STATEFUL, WIRE_METHOD)                                   \
    providerDispatchAudit.push_back({backend::COMMAND{},                                                                                   \
                                     WIRE_METHOD,                                                                                          \
                                     [](const backend::CommandValue& value) {                                                              \
                                         return std::holds_alternative<typed::RESULT>(value);                                              \
                                     },                                                                                                    \
                                     STATEFUL});
#define CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(COMMAND, RESULT, DOMAIN, METHOD, ACCESS, STATEFUL, WIRE_METHOD)                             \
    CODEX_BACKEND_PROVIDER_OPERATION(COMMAND, RESULT, DOMAIN, METHOD, ACCESS, STATEFUL, WIRE_METHOD)
#include "ai/openai/codex/backend/internal/ProviderOperations.inc"
#undef CODEX_BACKEND_PROVIDER_OPERATION_EMPTY
#undef CODEX_BACKEND_PROVIDER_OPERATION

            for (ProviderDispatchAuditEntry& entry : providerDispatchAudit) {
                if (auto* command = std::get_if<backend::CommandExec>(&entry.command)) {
                    command->params.command = {"synthetic-command"};
                } else if (auto* command = std::get_if<backend::AccountSendAddCreditsNudgeEmail>(&entry.command)) {
                    command->params.creditType = typed::AddCreditsNudgeCreditType::credits();
                } else if (auto* command = std::get_if<backend::ConfigValueWrite>(&entry.command)) {
                    command->params.mergeStrategy = typed::MergeStrategy::replace();
                } else if (auto* command = std::get_if<backend::PluginShareUpdateTargets>(&entry.command)) {
                    command->params.discoverability = typed::PluginShareUpdateDiscoverability::unlisted();
                } else if (auto* command = std::get_if<backend::WindowsSandboxSetupStart>(&entry.command)) {
                    command->params.mode = typed::WindowsSandboxSetupMode::unelevated();
                } else if (auto* command = std::get_if<backend::ThreadRollback>(&entry.command)) {
                    command->params.numTurns = 1;
                } else if (auto* command = std::get_if<backend::FsCopy>(&entry.command)) {
                    command->params.sourcePath = typed::AbsolutePath{"/synthetic/source"};
                    command->params.destinationPath = typed::AbsolutePath{"/synthetic/destination"};
                } else if (auto* command = std::get_if<backend::FsCreateDirectory>(&entry.command)) {
                    command->params.path = typed::AbsolutePath{"/synthetic/directory"};
                } else if (auto* command = std::get_if<backend::FsGetMetadata>(&entry.command)) {
                    command->params.path = typed::AbsolutePath{"/synthetic/metadata"};
                } else if (auto* command = std::get_if<backend::FsReadDirectory>(&entry.command)) {
                    command->params.path = typed::AbsolutePath{"/synthetic/read-directory"};
                } else if (auto* command = std::get_if<backend::FsReadFile>(&entry.command)) {
                    command->params.path = typed::AbsolutePath{"/synthetic/read-file"};
                } else if (auto* command = std::get_if<backend::FsRemove>(&entry.command)) {
                    command->params.path = typed::AbsolutePath{"/synthetic/remove"};
                } else if (auto* command = std::get_if<backend::FsWatch>(&entry.command)) {
                    command->params.path = typed::AbsolutePath{"/synthetic/watch"};
                    command->params.watchId = typed::FsWatchId{"synthetic-watch"};
                } else if (auto* command = std::get_if<backend::FsWriteFile>(&entry.command)) {
                    command->params.path = typed::AbsolutePath{"/synthetic/write-file"};
                } else if (auto* command = std::get_if<backend::PluginShareSave>(&entry.command)) {
                    command->params.pluginPath = typed::AbsolutePath{"/synthetic/plugin"};
                }
            }
            expect(providerDispatchAudit.size() == 86, "the runtime dispatch audit contains every stable provider operation exactly once");
            dispatchNextProviderOperation();
        }

        void dispatchNextProviderOperation() {
            if (providerDispatchAuditIndex == providerDispatchAudit.size()) {
                const std::set<std::string> registryMethods = stableApplicationMethods();
                expect(attemptedProviderMethods.size() == 86 && attemptedProviderMethods == registryMethods,
                       "all 86 BackendCommands dispatch the exact stable registry wire-method set");
                submitOperations();
                return;
            }

            ProviderDispatchAuditEntry& entry = providerDispatchAudit[providerDispatchAuditIndex];
            const std::string requestId = "dispatch-audit-" + std::to_string(providerDispatchAuditIndex);
            const std::string expectedMethod = entry.method;
            const std::size_t outgoingBefore = transport->outgoing.size();
            expect(static_cast<bool>(controller.submit(requestId, std::move(entry.command))),
                   "the runtime dispatch audit accepts " + expectedMethod);
            expect(completionCount(requestId) == 0, expectedMethod + " does not complete inline from submit()");

            waitUntil(
                "the runtime dispatch audit receives an asynchronous exact success for " + expectedMethod,
                [this, requestId]() {
                    return completionCount(requestId) == 1;
                },
                [this, requestId, expectedMethod, outgoingBefore]() {
                    const ProviderDispatchAuditEntry& completedEntry = providerDispatchAudit[providerDispatchAuditIndex];
                    const backend::CommandCompletion* completed = completion(requestId);
                    expect(completed && !completed->result.error && completedEntry.hasExactResult(completed->result.value),
                           expectedMethod + " returns its exact typed result alternative");
                    expect(transport->outgoing.size() == outgoingBefore + 1,
                           expectedMethod + " attempts exactly one typed transport request");
                    if (transport->outgoing.size() == outgoingBefore + 1) {
                        const std::string actualMethod = transport->outgoing.back().value("method", std::string{});
                        expect(actualMethod == expectedMethod,
                               expectedMethod + " dispatches through its exact direct typed façade wire method");
                        attemptedProviderMethods.insert(actualMethod);
                    }
                    const backend::BackendState current = backendCore->state();
                    const bool retained = current.providerOperations.contains(expectedMethod);
                    expect(retained == completedEntry.stateful,
                           expectedMethod + " applies its frozen stateful/action-only disposition before command completion");
                    ++providerDispatchAuditIndex;
                    dispatchNextProviderOperation();
                });
        }

        void verifyInitialHydration() {
            const backend::Snapshot snapshot = backendCore->snapshot();
            expect(snapshot.provider.lifecycle == backend::ProviderLifecycle::Ready && snapshot.threads.size() == 1 &&
                       snapshot.threads[0].id == "thread-initial" && snapshot.threadList.nextCursor == "initial-next",
                   "Ready performs one bounded initial list-page hydration and retains its cursor");
            expect(threadListRequests == 1 && boundedInitialRefreshes == 1,
                   "initial hydration submits exactly one list request with the configured limit");
            expect(controller.role() == backend::SessionRole::Controller && observer.role() == backend::SessionRole::Observer,
                   "explicit controller acquisition remains in force after backend startup");
        }

        void submitOperations() {
            backend::ThreadStart start;
            start.params.cwd = std::string{"/success"};
            expect(static_cast<bool>(controller.submit("start-success", std::move(start))), "controller submits thread/start");

            backend::ThreadResume resume;
            resume.params.threadId = typed::ThreadId{"thread-remote"};
            expect(static_cast<bool>(controller.submit("resume-remote", std::move(resume))), "controller submits thread/resume");

            backend::ThreadResume successfulResume;
            successfulResume.params.threadId = typed::ThreadId{"thread-successful-resume"};
            expect(static_cast<bool>(controller.submit("resume-success", std::move(successfulResume))),
                   "controller submits a successful thread/resume exact-wrapper regression");

            backend::ThreadList list;
            list.params.cursor = std::string{"explicit"};
            list.params.limit = 3;
            expect(static_cast<bool>(controller.submit("list-explicit", std::move(list))), "controller submits thread/list");

            backend::ThreadRead read;
            read.params.threadId = typed::ThreadId{"thread-read"};
            read.params.includeTurns = true;
            expect(static_cast<bool>(controller.submit("read-full", std::move(read))), "controller submits thread/read");

            backend::ThreadRead summaryRead;
            summaryRead.params.threadId = typed::ThreadId{"thread-read-summary"};
            summaryRead.params.includeTurns = false;
            expect(static_cast<bool>(controller.submit("read-summary", std::move(summaryRead))),
                   "controller submits thread/read without complete turn history");

            backend::TurnStart startTurn;
            startTurn.params.threadId = typed::ThreadId{"thread-success"};
            startTurn.params.input = {textInput("start a turn")};
            expect(static_cast<bool>(controller.submit("turn-start", std::move(startTurn))), "controller submits turn/start");

            backend::TurnInterrupt interrupt;
            interrupt.params.threadId = typed::ThreadId{"thread-success"};
            interrupt.params.turnId = typed::TurnId{"turn-start"};
            expect(static_cast<bool>(controller.submit("turn-interrupt", std::move(interrupt))), "controller submits turn/interrupt");

            backend::ThreadStart malformed;
            malformed.params.cwd = std::string{"/malformed"};
            expect(static_cast<bool>(controller.submit("decode-error", std::move(malformed))),
                   "malformed typed-result operation is accepted before typed decoding completion");

            transport->rejectNextSend = true;
            backend::ThreadStart localFailure;
            localFailure.params.cwd = std::string{"/local-enqueue-failure"};
            expect(static_cast<bool>(controller.submit("local-error", std::move(localFailure))),
                   "transport enqueue failure remains an asynchronous correlated command completion");

            waitUntil(
                "all typed BackendCore command paths complete exactly once",
                [this]() {
                    static const std::vector<std::string> ids = {"start-success",
                                                                 "resume-remote",
                                                                 "resume-success",
                                                                 "list-explicit",
                                                                 "read-full",
                                                                 "read-summary",
                                                                 "turn-start",
                                                                 "turn-interrupt",
                                                                 "decode-error",
                                                                 "local-error",
                                                                 "turn-reentrant"};
                    return std::all_of(ids.begin(), ids.end(), [this](const std::string& id) {
                        return completionCount(id) == 1;
                    });
                },
                [this]() {
                    verifyOperationResultsAndStreamEvents();
                });
        }

        void verifyOperationResultsAndStreamEvents() {
            expect(hasSuccess("start-success") && hasSuccess("resume-success") && hasSuccess("list-explicit") && hasSuccess("read-full") &&
                       hasSuccess("read-summary") && hasSuccess("turn-start") && hasSuccess("turn-interrupt") &&
                       hasSuccess("turn-reentrant"),
                   "successful typed thread and turn operations produce one successful command response each");
            expect(std::holds_alternative<typed::ThreadStartResponse>(completion("start-success")->result.value) &&
                       std::holds_alternative<typed::ThreadResumeResponse>(completion("resume-success")->result.value) &&
                       std::holds_alternative<typed::ThreadListResponse>(completion("list-explicit")->result.value) &&
                       std::holds_alternative<typed::ThreadReadResponse>(completion("read-full")->result.value) &&
                       std::holds_alternative<typed::TurnStartResponse>(completion("turn-start")->result.value) &&
                       std::holds_alternative<typed::Unit>(completion("turn-interrupt")->result.value),
                   "BackendCore preserves exact operation response wrappers instead of flattening their primary entities");
            expect(hasError("resume-remote", backend::CommandErrorCode::RemoteAppServerError) &&
                       completion("resume-remote")->result.error->remoteCode == -32'010,
                   "remote App Server error preserves stable category and optional remote code");
            expect(hasError("decode-error", backend::CommandErrorCode::TypedDecodingFailure),
                   "malformed successful result maps to typed_decoding_failure");
            expect(hasError("local-error", backend::CommandErrorCode::LocalSubmissionFailure),
                   "transport enqueue rejection maps to local_submission_failure");

            const backend::Snapshot hydrated = backendCore->snapshot();
            const auto findThread = [&hydrated](const std::string& id) {
                return std::find_if(hydrated.threads.begin(), hydrated.threads.end(), [&id](const backend::ThreadSnapshot& value) {
                    return value.id == id;
                });
            };
            const auto started = findThread("thread-success");
            const auto resumed = findThread("thread-successful-resume");
            const auto read = findThread("thread-read");
            const auto summaryRead = findThread("thread-read-summary");
            expect(started != hydrated.threads.end() && !started->fullyLoaded && resumed != hydrated.threads.end() &&
                       !resumed->fullyLoaded && read != hydrated.threads.end() && read->fullyLoaded && read->turns.size() == 1 &&
                       read->turns[0].items.size() == 1 && summaryRead != hydrated.threads.end() && !summaryRead->fullyLoaded,
                   "start/resume and read(includeTurns=false) retain summary load state while read(includeTurns=true) is fully loaded");
            expect(threadListRequests == 2 && hydrated.threadList.complete,
                   "explicit thread/list merges its page and updates completeness independently of initial hydration");

            streamStartSequence = hydrated.sequence.value();
            transport->inject({{"method", "thread/started"}, {"params", {{"thread", tests::codex::threadValue("thread-success")}}}});
            transport->inject({{"method", "thread/started"}, {"params", {{"thread", tests::codex::threadValue("thread-stream")}}}});
            transport->inject(
                {{"method", "turn/started"},
                 {"params", {{"threadId", "thread-stream"}, {"turn", tests::codex::turnValue("thread-stream", "turn-stream")}}}});
            transport->inject({{"method", "item/started"},
                               {"params",
                                {{"threadId", "thread-stream"},
                                 {"turnId", "turn-stream"},
                                 {"item", agentItemValue("item-stream")},
                                 {"startedAtMs", 10}}}});
            streamedText.clear();
            for (std::size_t index = 0; index < 100; ++index) {
                const std::string delta = std::to_string(index % 10);
                streamedText += delta;
                transport->inject(
                    {{"method", "item/agentMessage/delta"},
                     {"params", {{"threadId", "thread-stream"}, {"turnId", "turn-stream"}, {"itemId", "item-stream"}, {"delta", delta}}}});
            }
            transport->inject({{"method", "item/completed"},
                               {"params",
                                {{"threadId", "thread-stream"},
                                 {"turnId", "turn-stream"},
                                 {"item", agentItemValue("item-stream", streamedText)},
                                 {"completedAtMs", 20}}}});
            transport->inject(
                {{"method", "turn/completed"},
                 {"params",
                  {{"threadId", "thread-stream"}, {"turn", tests::codex::turnValue("thread-stream", "turn-stream", "completed")}}}});
            if (transport->callbacks.onDiagnostic) {
                transport->callbacks.onDiagnostic(Diagnostic{"deterministic backend diagnostic"});
            }
            transport->inject({{"method", "warning"}, {"params", {{"message", std::string(100'000, 'w')}}}});

            waitUntil(
                "streamed typed events reduce into exact terminal state and drain to both sessions",
                [this]() {
                    const backend::Snapshot snapshot = backendCore->snapshot();
                    for (const backend::ThreadSnapshot& thread : snapshot.threads) {
                        if (thread.id == "thread-stream" && !thread.turns.empty() && thread.turns[0].terminal &&
                            !thread.turns[0].items.empty() && thread.turns[0].items[0].agentText == streamedText &&
                            snapshot.diagnostics.received != 0 && !observerEvents.extensions.empty()) {
                            return lastSequence(controllerEvents) >= snapshot.sequence.value() &&
                                   lastSequence(observerEvents) >= snapshot.sequence.value();
                        }
                    }
                    return false;
                },
                [this]() {
                    verifyStreamAndPendingRequest();
                });
        }

        void verifyStreamAndPendingRequest() {
            const backend::Snapshot snapshot = backendCore->snapshot();
            expect(snapshot.diagnostics.recent.back() == "deterministic backend diagnostic",
                   "transport diagnostics are summarized in canonical backend state");
            expect(std::count_if(snapshot.threads.begin(),
                                 snapshot.threads.end(),
                                 [](const backend::ThreadSnapshot& thread) {
                                     return thread.id == "thread-success";
                                 }) == 1,
                   "operation result plus authoritative notification upsert one thread without duplicate order entries");
            const std::vector<std::uint64_t> controllerStream = sequencesAfter(controllerEvents, streamStartSequence);
            const std::vector<std::uint64_t> observerStream = sequencesAfter(observerEvents, streamStartSequence);
            expect(controllerStream == observerStream && !controllerStream.empty(),
                   "controller and observer receive identical ordered event sequences for their common streamed interval");
            const auto warning = std::find_if(
                observerEvents.extensions.rbegin(), observerEvents.extensions.rend(), [](const backend::CodexExtensionReceived& event) {
                    return event.method == "warning";
                });
            expect(warning != observerEvents.extensions.rend() && warning->safeProjection && !warning->typedEvent.has_value() &&
                       warning->payloadTruncated && warning->originalPayloadBytes.value_or(0) > 100'000 &&
                       warning->payload.dump().size() < 1'024,
                   "typed notifications reduce exactly once and session queues retain only a bounded extension marker");

            transport->inject({{"method", "item/commandExecution/requestApproval"},
                               {"id", "approval-1"},
                               {"params",
                                {{"threadId", "thread-stream"},
                                 {"turnId", "turn-stream"},
                                 {"itemId", "command-approval"},
                                 {"startedAtMs", 30},
                                 {"command", "make test"},
                                 {"cwd", "/tmp/project"}}}});
            waitUntil(
                "typed server request is retained and delivered promptly",
                [this]() {
                    const backend::Snapshot current = backendCore->snapshot();
                    return current.pendingRequests.size() == 1 && lastSequence(controllerEvents) >= current.sequence.value() &&
                           lastSequence(observerEvents) >= current.sequence.value();
                },
                [this]() {
                    disconnectControllerWithPendingRequest();
                });
        }

        void disconnectControllerWithPendingRequest() {
            const backend::Snapshot beforeClose = backendCore->snapshot();
            pendingApprovalId = beforeClose.pendingRequests.front().id;
            backend::ThreadStart pending;
            pending.params.cwd = std::string{"/deferred-close"};
            expect(static_cast<bool>(controller.submit("closed-operation", std::move(pending))),
                   "controller starts an operation whose response will arrive after session close");
            controller.close("controller disconnected intentionally");
            expect(!backendCore->snapshot().controller && backendCore->snapshot().pendingRequests.size() == 1,
                   "controller disconnect releases ownership but retains unanswered server requests");
            expect(static_cast<bool>(observer.submit("observer-acquire", backend::ControllerAcquire{})),
                   "remaining observer explicitly acquires controller ownership");

            waitUntil(
                "remaining observer becomes controller",
                [this]() {
                    return hasSuccess("observer-acquire") && observer.role() == backend::SessionRole::Controller;
                },
                [this]() {
                    completeClosedSessionOperationAndAnswerPending();
                });
        }

        void completeClosedSessionOperationAndAnswerPending() {
            expect(deferredClosedCallbacks.has_value() && deferredClosedId.has_value(),
                   "fake transport retained the closed session operation response");
            if (deferredClosedCallbacks && deferredClosedId) {
                tests::codex::inject(
                    *deferredClosedCallbacks,
                    Json{{"id", *deferredClosedId}, {"result", tests::codex::threadOperationResult("thread-from-closed-session")}});
            }

            transport->rejectNextSend = true;
            expect(static_cast<bool>(observer.submit("approval-enqueue-fail",
                                                     backend::ApprovalRespond{pendingApprovalId, typed::ApprovalDecision::decline()})),
                   "new controller can attempt to answer the retained pending approval");
            waitUntil(
                "failed response enqueue retains request and closed session suppresses operation completion",
                [this]() {
                    return hasError("approval-enqueue-fail", backend::CommandErrorCode::LocalSubmissionFailure);
                },
                [this]() {
                    const backend::Snapshot snapshot = backendCore->snapshot();
                    const bool hydrated = std::any_of(snapshot.threads.begin(), snapshot.threads.end(), [](const auto& thread) {
                        return thread.id == "thread-from-closed-session";
                    });
                    expect(completionCount("closed-operation") == 0 && !hydrated && snapshot.pendingRequests.size() == 1,
                           "session close retires its operation and suppresses its late state mutation; enqueue failure retains request");
                    expect(static_cast<bool>(observer.submit(
                               "approval-success", backend::ApprovalRespond{pendingApprovalId, typed::ApprovalDecision::decline()})),
                           "pending approval can be retried after local enqueue failure");
                    waitUntil(
                        "successful approval response removes pending request exactly once",
                        [this]() {
                            return hasSuccess("approval-success") && backendCore->snapshot().pendingRequests.empty();
                        },
                        [this]() {
                            beginReverseRequestCompleteness();
                        });
                });
        }

        static Json commandApprovalParams(std::string itemId) {
            return {{"itemId", std::move(itemId)}, {"startedAtMs", 101}, {"threadId", "thread-reverse"}, {"turnId", "turn-reverse"}};
        }

        static Json fileChangeApprovalParams(std::string itemId) {
            return {{"itemId", std::move(itemId)}, {"startedAtMs", 102}, {"threadId", "thread-reverse"}, {"turnId", "turn-reverse"}};
        }

        static Json userInputParams(std::string itemId, std::string questionId) {
            return {{"itemId", std::move(itemId)},
                    {"questions",
                     Json::array({{{"header", "Choice"},
                                   {"id", std::move(questionId)},
                                   {"isOther", false},
                                   {"isSecret", false},
                                   {"options", Json::array({{{"description", "Select alpha"}, {"label", "alpha"}}})},
                                   {"question", "Choose a value"}}})},
                    {"threadId", "thread-reverse"},
                    {"turnId", "turn-reverse"}};
        }

        static Json authenticationParams(std::string accountId) {
            return {{"previousAccountId", std::move(accountId)}, {"reason", "unauthorized"}};
        }

        static Json applyPatchApprovalParams() {
            return {{"callId", "call-apply"},
                    {"conversationId", "thread-reverse"},
                    {"fileChanges", Json::object()},
                    {"grantRoot", nullptr},
                    {"reason", nullptr}};
        }

        static Json execCommandApprovalParams() {
            return {{"approvalId", nullptr},
                    {"callId", "call-exec"},
                    {"command", Json::array({"synthetic-command"})},
                    {"conversationId", "thread-reverse"},
                    {"cwd", "/synthetic/reverse"},
                    {"parsedCmd", Json::array()},
                    {"reason", nullptr}};
        }

        static Json permissionsApprovalParams() {
            return {{"cwd", "/synthetic/reverse"},
                    {"itemId", "item-permissions"},
                    {"permissions", Json::object()},
                    {"startedAtMs", 103},
                    {"threadId", "thread-reverse"},
                    {"turnId", "turn-reverse"}};
        }

        static Json dynamicToolParams(std::string callId, Json arguments) {
            return {{"arguments", std::move(arguments)},
                    {"callId", std::move(callId)},
                    {"threadId", "thread-reverse"},
                    {"tool", "synthetic_tool"},
                    {"turnId", "turn-reverse"}};
        }

        static Json mcpElicitationParams(std::string message) {
            return {{"message", std::move(message)},
                    {"mode", "form"},
                    {"requestedSchema",
                     {{"properties", {{"value", {{"type", "string"}}}}}, {"required", Json::array({"value"})}, {"type", "object"}}},
                    {"serverName", "synthetic-mcp"},
                    {"threadId", "thread-reverse"}};
        }

        std::size_t terminalResponseCount() const {
            return std::count_if(transport->outgoing.begin(), transport->outgoing.end(), [](const Json& envelope) {
                return !envelope.contains("method") && envelope.contains("id") &&
                       (envelope.contains("result") || envelope.contains("error"));
            });
        }

        const Json* terminalResponse(const Json& id) const {
            const auto iterator = std::find_if(transport->outgoing.rbegin(), transport->outgoing.rend(), [&id](const Json& envelope) {
                return !envelope.contains("method") && envelope.contains("id") && envelope.at("id") == id &&
                       (envelope.contains("result") || envelope.contains("error"));
            });
            return iterator == transport->outgoing.rend() ? nullptr : &*iterator;
        }

        void beginReverseRequestCompleteness() {
            reverseTerminalBaseline = terminalResponseCount();
            const std::vector<Json> requests{
                {{"id", "reverse-command"},
                 {"method", "item/commandExecution/requestApproval"},
                 {"params", commandApprovalParams("item-command")}},
                {{"id", "reverse-file"}, {"method", "item/fileChange/requestApproval"}, {"params", fileChangeApprovalParams("item-file")}},
                {{"id", "reverse-user-vector"},
                 {"method", "item/tool/requestUserInput"},
                 {"params", userInputParams("item-user-vector", "choice-vector")}},
                {{"id", "reverse-user-typed"},
                 {"method", "item/tool/requestUserInput"},
                 {"params", userInputParams("item-user-typed", "choice-typed")}},
                {{"id", "reverse-auth-legacy"},
                 {"method", "account/chatgptAuthTokens/refresh"},
                 {"params", authenticationParams("legacy-account")}},
                {{"id", "reverse-auth-canonical"},
                 {"method", "account/chatgptAuthTokens/refresh"},
                 {"params", authenticationParams("canonical-account")}},
                {{"id", "reverse-apply"}, {"method", "applyPatchApproval"}, {"params", applyPatchApprovalParams()}},
                {{"id", "reverse-exec"}, {"method", "execCommandApproval"}, {"params", execCommandApprovalParams()}},
                {{"id", "reverse-permissions"}, {"method", "item/permissions/requestApproval"}, {"params", permissionsApprovalParams()}},
                {{"id", "reverse-attestation"},
                 {"method", "attestation/generate"},
                 {"params", {{"privateAttestationChallenge", "A16B_ATTESTATION_REQUEST_SECRET"}}}},
                {{"id", "reverse-dynamic"},
                 {"method", "item/tool/call"},
                 {"params", dynamicToolParams("call-dynamic", {{"secret", "A16B_DYNAMIC_ARGUMENT_SECRET"}})}},
                {{"id", "reverse-mcp"},
                 {"method", "mcpServer/elicitation/request"},
                 {"params", mcpElicitationParams("A16B_MCP_ELICITATION_SECRET")}},
            };
            for (const Json& request : requests) {
                transport->inject(request);
            }
            waitUntil(
                "all ten stable reverse-request alternatives and both compatibility response forms are retained",
                [this]() {
                    return backendCore->snapshot().pendingRequests.size() == 12;
                },
                [this]() {
                    verifyReverseRequestSnapshotAndFailures();
                });
        }

        void verifyReverseRequestSnapshotAndFailures() {
            const backend::Snapshot snapshot = backendCore->snapshot();
            const std::vector<std::string> expectedTypes{"command_approval",
                                                         "file_change_approval",
                                                         "user_input",
                                                         "user_input",
                                                         "authentication",
                                                         "authentication",
                                                         "apply_patch_approval",
                                                         "exec_command_approval",
                                                         "permissions_approval",
                                                         "attestation",
                                                         "dynamic_tool_call",
                                                         "mcp_elicitation"};
            reversePendingIds.clear();
            std::vector<std::string> actualTypes;
            std::string pendingDetails;
            for (const backend::PendingRequestSnapshot& pending : snapshot.pendingRequests) {
                reversePendingIds.push_back(pending.id);
                actualTypes.push_back(pending.type);
                pendingDetails += pending.details.dump();
            }
            expect(actualTypes == expectedTypes && reversePendingIds.size() == expectedTypes.size(),
                   "all ten stable typed request alternatives retain their exact safe backend request kind");
            expect(terminalResponseCount() == reverseTerminalBaseline,
                   "retaining typed requests never emits an automatic approval, answer, or rejection");
            expect(pendingDetails.find("A16B_ATTESTATION_REQUEST_SECRET") == std::string::npos &&
                       pendingDetails.find("A16B_DYNAMIC_ARGUMENT_SECRET") == std::string::npos &&
                       pendingDetails.find("A16B_MCP_ELICITATION_SECRET") == std::string::npos,
                   "safe pending snapshots omit attestation, dynamic-tool argument, and MCP elicitation secrets");
            if (reversePendingIds.size() != expectedTypes.size()) {
                finish();
                return;
            }

            transport->rejectNextSend = true;
            expect(static_cast<bool>(
                       observer.submit("reverse-enqueue-failure",
                                       backend::ApplyPatchApprovalRespond{
                                           reversePendingIds[6], typed::ApplyPatchApprovalResponse{typed::DeniedReviewDecision{}}})),
                   "an exact typed reverse response is accepted before deterministic transport rejection");
            expect(static_cast<bool>(
                       observer.submit("reverse-wrong-type",
                                       backend::AttestationGenerateRespond{
                                           reversePendingIds[0], typed::AttestationGenerateResponse{"must-not-send", Json::object()}})),
                   "a wrong exact response command enters the asynchronous backend command lifecycle");
            expect(static_cast<bool>(observer.submit("reverse-known-raw",
                                                     backend::UnknownRequestRespondRaw{reversePendingIds[0], Json{{"mustNot", "send"}}})),
                   "unknown raw response is checked against known occurrence ownership");
            expect(static_cast<bool>(
                       observer.submit("reverse-known-unknown-reject",
                                       backend::UnknownRequestReject{reversePendingIds[1],
                                                                     {-32'150, "must not reject known request as unknown", std::nullopt}})),
                   "unknown rejection is checked against known occurrence ownership");
            expect(static_cast<bool>(observer.submit(
                       "reverse-known-unsupported-reject",
                       backend::KnownRequestReject{reversePendingIds[0], {-32'151, "unsupported known typed rejection", std::nullopt}})),
                   "typed rejection policy is checked for an unsupported known request");
            waitUntil(
                "reverse response failures retain every pending occurrence",
                [this]() {
                    return hasError("reverse-enqueue-failure", backend::CommandErrorCode::LocalSubmissionFailure) &&
                           hasError("reverse-wrong-type", backend::CommandErrorCode::InvalidCommand) &&
                           hasError("reverse-known-raw", backend::CommandErrorCode::InvalidCommand) &&
                           hasError("reverse-known-unknown-reject", backend::CommandErrorCode::InvalidCommand) &&
                           hasError("reverse-known-unsupported-reject", backend::CommandErrorCode::InvalidCommand);
                },
                [this]() {
                    expect(backendCore->snapshot().pendingRequests.size() == 12,
                           "enqueue failure, wrong response type, and unsupported rejection retain occurrence ownership");
                    submitExactReverseResponses();
                });
        }

        void submitExactReverseResponses() {
            typed::ToolRequestUserInputResponse typedAnswers;
            typedAnswers.answers["choice-typed"].answers = {"beta"};
            const std::vector<std::pair<std::string, backend::BackendCommand>> commands{
                {"reverse-command-success",
                 backend::ApprovalRespond{reversePendingIds[0],
                                          backend::ApprovalResponse{typed::CommandExecutionRequestApprovalResponse{
                                              typed::DeclineCommandExecutionApprovalDecision{}}}}},
                {"reverse-file-success",
                 backend::ApprovalRespond{
                     reversePendingIds[1],
                     backend::ApprovalResponse{typed::FileChangeRequestApprovalResponse{typed::FileChangeApprovalDecision::cancel()}}}},
                {"reverse-user-vector-success",
                 backend::UserInputRespond{reversePendingIds[2],
                                           backend::UserInputResponse{
                                               std::vector<typed::UserInputAnswer>{{"choice-vector", std::vector<std::string>{"alpha"}}}}}},
                {"reverse-user-typed-success",
                 backend::UserInputRespond{reversePendingIds[3], backend::UserInputResponse{std::move(typedAnswers)}}},
                {"reverse-auth-legacy-success",
                 backend::AuthenticationRespond{reversePendingIds[4],
                                                backend::AuthenticationResponsePayload{typed::AuthenticationResponse{
                                                    "A16B_LEGACY_ACCESS_TOKEN_SECRET", "legacy-account", std::string{"plus"}}}}},
                {"reverse-auth-canonical-success",
                 backend::AuthenticationRespond{reversePendingIds[5],
                                                backend::AuthenticationResponsePayload{typed::ChatgptAuthTokensRefreshResponse{
                                                    "A16B_CANONICAL_ACCESS_TOKEN_SECRET",
                                                    typed::AccountId{"canonical-account"},
                                                    typed::OptionalNullable<typed::PlanType>::withValue(typed::PlanType::plus())}}}},
                {"reverse-apply-success",
                 backend::ApplyPatchApprovalRespond{reversePendingIds[6],
                                                    typed::ApplyPatchApprovalResponse{typed::DeniedReviewDecision{}}}},
                {"reverse-exec-success",
                 backend::ExecCommandApprovalRespond{reversePendingIds[7],
                                                     typed::ExecCommandApprovalResponse{typed::TimedOutReviewDecision{}}}},
                {"reverse-permissions-success",
                 backend::PermissionsApprovalRespond{
                     reversePendingIds[8],
                     typed::PermissionsRequestApprovalResponse{{}, std::nullopt, typed::OptionalNullable<bool>::omitted()}}},
                {"reverse-attestation-success",
                 backend::AttestationGenerateRespond{
                     reversePendingIds[9], typed::AttestationGenerateResponse{"A16B_ATTESTATION_RESPONSE_SECRET", Json::object()}}},
                {"reverse-dynamic-success",
                 backend::DynamicToolCallRespond{reversePendingIds[10], typed::DynamicToolCallResponse{{}, true, Json::object()}}},
                {"reverse-mcp-success",
                 backend::McpServerElicitationRespond{
                     reversePendingIds[11],
                     typed::McpServerElicitationRequestResponse{typed::McpServerElicitationAction::decline(),
                                                                typed::OptionalNullable<Json>::omitted(),
                                                                typed::OptionalNullable<Json>::omitted(),
                                                                Json::object()}}},
            };
            for (const auto& [requestId, command] : commands) {
                expect(static_cast<bool>(observer.submit(requestId, command)), requestId + " is accepted for exact typed routing");
            }
            waitUntil(
                "all exact typed response commands complete and retire their occurrences",
                [this,
                 ids = std::vector<std::string>{"reverse-command-success",
                                                "reverse-file-success",
                                                "reverse-user-vector-success",
                                                "reverse-user-typed-success",
                                                "reverse-auth-legacy-success",
                                                "reverse-auth-canonical-success",
                                                "reverse-apply-success",
                                                "reverse-exec-success",
                                                "reverse-permissions-success",
                                                "reverse-attestation-success",
                                                "reverse-dynamic-success",
                                                "reverse-mcp-success"}]() {
                    return backendCore->snapshot().pendingRequests.empty() &&
                           std::all_of(ids.begin(), ids.end(), [this](const std::string& id) {
                               return hasSuccess(id);
                           });
                },
                [this]() {
                    verifyExactReverseResponses();
                });
        }

        void verifyExactReverseResponses() {
            const std::vector<std::pair<Json, Json>> expected{
                {"reverse-command", {{"decision", "decline"}}},
                {"reverse-file", {{"decision", "cancel"}}},
                {"reverse-user-vector", {{"answers", {{"choice-vector", {{"answers", Json::array({"alpha"})}}}}}}},
                {"reverse-user-typed", {{"answers", {{"choice-typed", {{"answers", Json::array({"beta"})}}}}}}},
                {"reverse-auth-legacy",
                 {{"accessToken", "A16B_LEGACY_ACCESS_TOKEN_SECRET"}, {"chatgptAccountId", "legacy-account"}, {"chatgptPlanType", "plus"}}},
                {"reverse-auth-canonical",
                 {{"accessToken", "A16B_CANONICAL_ACCESS_TOKEN_SECRET"},
                  {"chatgptAccountId", "canonical-account"},
                  {"chatgptPlanType", "plus"}}},
                {"reverse-apply", {{"decision", "denied"}}},
                {"reverse-exec", {{"decision", "timed_out"}}},
                {"reverse-permissions", {{"permissions", Json::object()}}},
                {"reverse-attestation", {{"token", "A16B_ATTESTATION_RESPONSE_SECRET"}}},
                {"reverse-dynamic", {{"contentItems", Json::array()}, {"success", true}}},
                {"reverse-mcp", {{"action", "decline"}}},
            };
            for (const auto& [id, resultValue] : expected) {
                const Json* response = terminalResponse(id);
                expect(response && response->value("result", Json{}) == resultValue,
                       "exact typed Requests facade emits the expected response for " + id.get<std::string>());
            }
            const backend::Snapshot snapshot = backendCore->snapshot();
            const bool accessTokenInDiagnostics =
                std::any_of(snapshot.diagnostics.recent.begin(), snapshot.diagnostics.recent.end(), [](const std::string& diagnostic) {
                    return diagnostic.find("A16B_LEGACY_ACCESS_TOKEN_SECRET") != std::string::npos ||
                           diagnostic.find("A16B_CANONICAL_ACCESS_TOKEN_SECRET") != std::string::npos;
                });
            expect(!accessTokenInDiagnostics,
                   "authentication response secrets are absent from bounded backend diagnostics and canonical pending state");
            beginTypedRejectionsAndUnknowns();
        }

        void beginTypedRejectionsAndUnknowns() {
            reverseTerminalBaseline = terminalResponseCount();
            const std::vector<Json> requests{
                {{"id", "reject-user"},
                 {"method", "item/tool/requestUserInput"},
                 {"params", userInputParams("item-reject-user", "choice-reject")}},
                {{"id", "reject-attestation"}, {"method", "attestation/generate"}, {"params", Json::object()}},
                {{"id", "reject-dynamic"}, {"method", "item/tool/call"}, {"params", dynamicToolParams("call-reject", Json::object())}},
                {{"id", "reject-mcp"},
                 {"method", "mcpServer/elicitation/request"},
                 {"params", mcpElicitationParams("Decline this request")}},
                {{"id", "unknown-raw"}, {"method", "future/respondRaw"}, {"params", {{"future", true}}}},
                {{"id", "unknown-reject"}, {"method", "future/reject"}, {"params", {{"future", true}}}},
            };
            for (const Json& request : requests) {
                transport->inject(request);
            }
            waitUntil(
                "typed-rejectable and unknown request occurrences are retained",
                [this]() {
                    return backendCore->snapshot().pendingRequests.size() == 6;
                },
                [this]() {
                    const backend::Snapshot snapshot = backendCore->snapshot();
                    rejectPendingIds.clear();
                    for (const backend::PendingRequestSnapshot& pending : snapshot.pendingRequests) {
                        rejectPendingIds.push_back(pending.id);
                    }
                    expect(terminalResponseCount() == reverseTerminalBaseline && rejectPendingIds.size() == 6,
                           "typed-rejectable and unknown occurrences are never answered automatically");
                    if (rejectPendingIds.size() != 6) {
                        finish();
                        return;
                    }
                    const ai::openai::codex::ProtocolError knownError{
                        -32'160, "A1.6b typed request rejected", std::optional<Json>{Json{{"reason", "test"}}}};
                    expect(static_cast<bool>(
                               observer.submit("reject-user-success", backend::KnownRequestReject{rejectPendingIds[0], knownError})),
                           "user-input request uses exact typed rejection");
                    expect(static_cast<bool>(
                               observer.submit("reject-attestation-success", backend::KnownRequestReject{rejectPendingIds[1], knownError})),
                           "attestation request uses exact typed rejection");
                    expect(static_cast<bool>(
                               observer.submit("reject-dynamic-success", backend::KnownRequestReject{rejectPendingIds[2], knownError})),
                           "dynamic-tool request uses exact typed rejection");
                    expect(static_cast<bool>(
                               observer.submit("reject-mcp-success", backend::KnownRequestReject{rejectPendingIds[3], knownError})),
                           "MCP elicitation request uses exact typed rejection");
                    expect(
                        static_cast<bool>(observer.submit(
                            "unknown-raw-success", backend::UnknownRequestRespondRaw{rejectPendingIds[4], Json{{"futureResult", true}}})),
                        "unknown occurrence accepts only the raw response command");
                    expect(
                        static_cast<bool>(observer.submit(
                            "unknown-reject-success",
                            backend::UnknownRequestReject{rejectPendingIds[5], {-32'161, "A1.6b unknown request rejected", std::nullopt}})),
                        "unknown occurrence accepts only the unknown rejection command");
                    waitUntil(
                        "typed and unknown rejections retire each occurrence exactly once",
                        [this,
                         ids = std::vector<std::string>{"reject-user-success",
                                                        "reject-attestation-success",
                                                        "reject-dynamic-success",
                                                        "reject-mcp-success",
                                                        "unknown-raw-success",
                                                        "unknown-reject-success"}]() {
                            return backendCore->snapshot().pendingRequests.empty() &&
                                   std::all_of(ids.begin(), ids.end(), [this](const std::string& id) {
                                       return hasSuccess(id);
                                   });
                        },
                        [this]() {
                            verifyTypedRejectionResults();
                        });
                });
        }

        void verifyTypedRejectionResults() {
            for (const char* id : {"reject-user", "reject-attestation", "reject-dynamic", "reject-mcp"}) {
                const Json* response = terminalResponse(id);
                expect(response && response->contains("error") && response->at("error").value("code", 0) == -32'160,
                       std::string{id} + " routes through the exact typed reject facade");
            }
            const Json* raw = terminalResponse("unknown-raw");
            const Json* rejected = terminalResponse("unknown-reject");
            expect(raw && raw->value("result", Json{}) == Json{{"futureResult", true}},
                   "unknown raw response preserves the application-provided JSON result");
            expect(rejected && rejected->contains("error") && rejected->at("error").value("code", 0) == -32'161,
                   "unknown rejection preserves the application-provided protocol error");
            beginUnsolicitedStopScenario();
        }

        void beginUnsolicitedStopScenario() {
            backend::ThreadStart operation;
            operation.params.cwd = std::string{"/unsolicited-stop"};
            expect(static_cast<bool>(observer.submit("unsolicited-stop-operation", std::move(operation))),
                   "controller submits an operation whose typed completion is scheduled but not yet delivered");

            const std::size_t refreshesBeforeRestart = boundedInitialRefreshes;
            appServerClient->stop();
            waitUntil(
                "unsolicited App Server Stopping and Stopped cancel the accepted operation exactly once",
                [this]() {
                    const auto hasLifecycle = [](backend::ProviderLifecycle expected) {
                        return [expected](const backend::ProviderLifecycleChanged& value) {
                            return value.provider.lifecycle == expected;
                        };
                    };
                    const auto stopping = std::find_if(observerEvents.lifecycles.begin(),
                                                       observerEvents.lifecycles.end(),
                                                       hasLifecycle(backend::ProviderLifecycle::Stopping));
                    const auto stopped =
                        stopping == observerEvents.lifecycles.end()
                            ? observerEvents.lifecycles.end()
                            : std::find_if(stopping, observerEvents.lifecycles.end(), hasLifecycle(backend::ProviderLifecycle::Stopped));
                    return backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Stopped &&
                           stopping != observerEvents.lifecycles.end() && stopped != observerEvents.lifecycles.end() &&
                           completionCount("unsolicited-stop-operation") == 1;
                },
                [this, refreshesBeforeRestart]() {
                    const backend::CommandCompletion* cancelled = completion("unsolicited-stop-operation");
                    expect(cancelled && cancelled->result.error && cancelled->result.error->code == backend::CommandErrorCode::Cancelled,
                           "an unsolicited orderly connection invalidation reports a stable cancelled command result");
                    expect(cancelled && cancelled->result.error &&
                               cancelled->result.error->message == "The App Server connection stopped before the operation completed.",
                           "BackendCore completes its accepted-operation ledger instead of relying on a suppressed typed callback");
                    const auto& threads = backendCore->snapshot().threads;
                    expect(std::none_of(threads.begin(),
                                        threads.end(),
                                        [](const backend::ThreadSnapshot& thread) {
                                            return thread.id == "thread-unsolicited-scheduled-completion";
                                        }),
                           "the invalidated scheduled typed completion does not mutate canonical state");

                    const std::uint64_t invalidatedGeneration = observerEvents.lifecycles.back().provider.generation;
                    backendCore->start();
                    waitUntil(
                        "BackendCore restarts after the unsolicited Stopped lifecycle in a fresh generation",
                        [this, refreshesBeforeRestart, invalidatedGeneration]() {
                            const bool observedFreshReady =
                                std::any_of(observerEvents.lifecycles.begin(),
                                            observerEvents.lifecycles.end(),
                                            [invalidatedGeneration](const backend::ProviderLifecycleChanged& value) {
                                                return value.provider.lifecycle == backend::ProviderLifecycle::Ready &&
                                                       value.provider.generation > invalidatedGeneration;
                                            });
                            return backendCore->isReady() && boundedInitialRefreshes == refreshesBeforeRestart + 1 && observedFreshReady;
                        },
                        [this]() {
                            afterTicks(8, [this]() {
                                const auto& restartedThreads = backendCore->snapshot().threads;
                                const bool staleHydrated = std::any_of(
                                    restartedThreads.begin(), restartedThreads.end(), [](const backend::ThreadSnapshot& thread) {
                                        return thread.id == "thread-unsolicited-scheduled-completion";
                                    });
                                const auto refreshed =
                                    std::find_if(restartedThreads.begin(), restartedThreads.end(), [](const auto& thread) {
                                        return thread.id == "thread-initial";
                                    });
                                const auto retained =
                                    std::find_if(restartedThreads.begin(), restartedThreads.end(), [](const auto& thread) {
                                        return thread.id == "thread-success";
                                    });
                                expect(!staleHydrated && completionCount("unsolicited-stop-operation") == 1,
                                       "restart generation suppresses the invalidated typed completion and duplicate response");
                                expect(refreshed != restartedThreads.end() && refreshed->stamp.freshness == backend::Freshness::Current &&
                                           retained != restartedThreads.end() && retained->stamp.freshness == backend::Freshness::Stale,
                                       "bounded rehydration marks only current-generation confirmed entities Current");
                                beginStopRestartGenerationScenario();
                            });
                        });
                });
        }

        void beginStopRestartGenerationScenario() {
            backend::ThreadStart stale;
            stale.params.cwd = std::string{"/old-generation"};
            expect(static_cast<bool>(observer.submit("old-generation-operation", std::move(stale))),
                   "controller submits operation retained across explicit stop boundary");
            expect(static_cast<bool>(observer.submit("stop-now", backend::SnapshotGet{})),
                   "callback-stop scenario queues a read-only completion");

            waitUntil(
                "command callback stops backend and cancels active operation once",
                [this]() {
                    return backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Stopped &&
                           hasError("old-generation-operation", backend::CommandErrorCode::Cancelled) &&
                           completionCount("old-generation-operation") == 1;
                },
                [this]() {
                    expect(staleCallbacks.has_value() && staleId.has_value(),
                           "fake transport retained the prior-generation operation response");
                    backendCore->start();
                    waitUntil(
                        "backend explicitly restarts and performs one bounded refresh in the new generation",
                        [this]() {
                            return backendCore->isReady() && threadListRequests >= 3 && boundedInitialRefreshes >= 2;
                        },
                        [this]() {
                            verifyStaleCompletionSuppression();
                        });
                });
        }

        void verifyStaleCompletionSuppression() {
            if (staleCallbacks && staleId) {
                tests::codex::inject(*staleCallbacks,
                                     Json{{"id", *staleId}, {"result", tests::codex::threadOperationResult("thread-stale-generation")}});
            }
            afterTicks(8, [this]() {
                const backend::Snapshot snapshot = backendCore->snapshot();
                const bool staleHydrated = std::any_of(snapshot.threads.begin(), snapshot.threads.end(), [](const auto& thread) {
                    return thread.id == "thread-stale-generation";
                });
                expect(!staleHydrated && completionCount("old-generation-operation") == 1,
                       "prior-generation typed completion neither mutates state nor duplicates command completion");

                backend::ThreadStart fresh;
                fresh.params.cwd = std::string{"/fresh"};
                expect(static_cast<bool>(observer.submit("fresh-generation-operation", std::move(fresh))),
                       "new generation accepts fresh typed operations");
                waitUntil(
                    "fresh generation typed operation completes",
                    [this]() {
                        return hasSuccess("fresh-generation-operation");
                    },
                    [this]() {
                        failConnectionWithPendingRequest();
                    });
            });
        }

        void failConnectionWithPendingRequest() {
            transport->inject({{"method", "future/pending"}, {"id", "unknown-pending"}, {"params", {{"future", true}}}});
            waitUntil(
                "unknown typed request is retained before connection invalidation",
                [this]() {
                    return backendCore->snapshot().pendingRequests.size() == 1;
                },
                [this]() {
                    pendingRemovalsBeforeInvalidation = observerEvents.pendingRequestRemovals.size();
                    invalidationsBeforePendingFailure = observerEvents.invalidations.size();
                    invalidatedPendingRequestId = backendCore->snapshot().pendingRequests.front().id;
                    if (transport->callbacks.onError) {
                        transport->callbacks.onError(Error{Error::Category::Transport, 91, "deterministic connection failure"});
                    }
                    waitUntil(
                        "connection failure clears pending ownership and exposes lifecycle failure",
                        [this]() {
                            const backend::Snapshot snapshot = backendCore->snapshot();
                            return snapshot.provider.lifecycle == backend::ProviderLifecycle::Failed && snapshot.pendingRequests.empty() &&
                                   snapshot.provider.lastError.has_value() &&
                                   observerEvents.pendingRequestRemovals.size() == pendingRemovalsBeforeInvalidation + 1 &&
                                   observerEvents.invalidations.size() == invalidationsBeforePendingFailure + 1;
                        },
                        [this]() {
                            const backend::Snapshot failed = backendCore->snapshot();
                            expect(failed.provider.lastError->message == "deterministic connection failure",
                                   "BackendCore retains lifecycle failure details while clearing invalid request ownership");
                            const auto& [removalSequence, removal] = observerEvents.pendingRequestRemovals.back();
                            expect(removal.id == invalidatedPendingRequestId && removalSequence > observerEvents.invalidations.back(),
                                   "provider invalidation emits one ordered pending-request removal after canonical ownership clears");
                            const std::size_t startsBeforeFailedStart = transport->startCount;
                            const std::uint64_t generationBeforeRestart = failed.provider.generation;
                            backendCore->start();
                            afterTicks(4, [this, startsBeforeFailedStart, generationBeforeRestart]() {
                                const backend::Snapshot stillFailed = backendCore->snapshot();
                                expect(transport->startCount == startsBeforeFailedStart &&
                                           stillFailed.provider.lifecycle == backend::ProviderLifecycle::Failed &&
                                           stillFailed.provider.generation == generationBeforeRestart,
                                       "start() never calls AppServerClient::start() directly from Failed");
                                expect(observer.isOpen() && observer.role() == backend::SessionRole::Controller,
                                       "provider failure retains the frontend session and controller");
                                backendCore->restart();
                                waitUntil(
                                    "restart performs the stopped-to-start transition exactly once",
                                    [this, startsBeforeFailedStart, generationBeforeRestart]() {
                                        const backend::Snapshot snapshot = backendCore->snapshot();
                                        return backendCore->isReady() && !snapshot.provider.lastError &&
                                               snapshot.provider.generation == generationBeforeRestart + 1 &&
                                               transport->startCount == startsBeforeFailedStart + 1 && boundedInitialRefreshes >= 3 &&
                                               snapshot.threadList.pagesLoaded == 1 &&
                                               snapshot.threadList.stamp.generation == snapshot.provider.generation &&
                                               snapshot.threadList.stamp.freshness == backend::Freshness::Current;
                                    },
                                    [this]() {
                                        const backend::Snapshot refreshed = backendCore->snapshot();
                                        expect(refreshed.threadList.pagesLoaded == 1 &&
                                                   refreshed.threadList.stamp ==
                                                       backend::SourceStamp{refreshed.provider.generation, backend::Freshness::Current},
                                               "restart hydration resets pagination metadata for the new provider generation");
                                        expect(static_cast<bool>(
                                                   observer.submit("stale-pending-response",
                                                                   backend::UnknownRequestRespondRaw{invalidatedPendingRequestId,
                                                                                                     Json{{"mustNot", "respond"}}})),
                                               "a prior-generation backend pending ID enters the normal command lifecycle");
                                        waitUntil(
                                            "invalidated prior-generation pending ownership cannot respond after restart",
                                            [this]() {
                                                return hasError("stale-pending-response", backend::CommandErrorCode::NotFound);
                                            },
                                            [this]() {
                                                verifyLargeResultQueueAccounting();
                                            });
                                    });
                            });
                        });
                });
        }

        void verifyLargeResultQueueAccounting() {
            largeResultSession = backendCore->openSession({[](const std::vector<backend::SequencedBackendEvent>&) {
                                                           },
                                                           [](const backend::Snapshot&) {
                                                           },
                                                           [this](const backend::CommandCompletion&) {
                                                               ++largeResultCompletions;
                                                           },
                                                           [this](const std::string&) {
                                                               ++largeResultClosedCallbacks;
                                                           }});

            backend::FsReadFile large;
            large.params.path = typed::AbsolutePath{"/synthetic/large-result"};
            expect(static_cast<bool>(largeResultSession.submit("large-exact-result", std::move(large))),
                   "a sacrificial observer session submits a large exact provider result");
            waitUntil(
                "a conservatively accounted large exact result closes only its over-capacity session",
                [this]() {
                    return !largeResultSession.isOpen() && largeResultClosedCallbacks == 1;
                },
                [this]() {
                    expect(largeResultCompletions == 0 && backendCore->isReady() && observer.isOpen(),
                           "large exact result queue accounting includes decoded and raw payload retention without affecting the "
                           "provider or another session");
                    verifyBackendDestructionSuppressesLateCompletion();
                });
        }

        void verifyBackendDestructionSuppressesLateCompletion() {
            backend::ThreadStart pending;
            pending.params.cwd = std::string{"/destroyed-backend"};
            expect(static_cast<bool>(observer.submit("destroyed-backend-operation", std::move(pending))),
                   "the controller starts an operation retained until BackendCore destruction");
            waitUntil(
                "the fake transport retains a provider completion across BackendCore destruction",
                [this]() {
                    return destroyedCallbacks.has_value() && destroyedId.has_value();
                },
                [this]() {
                    const TransportCallbacks callbacks = *destroyedCallbacks;
                    const Json id = *destroyedId;
                    backendCore.reset();
                    tests::codex::inject(
                        callbacks, Json{{"id", id}, {"result", tests::codex::threadOperationResult("thread-after-backend-destruction")}});
                    afterTicks(4, [this]() {
                        expect(completionCount("destroyed-backend-operation") == 0,
                               "a callback delivered after BackendCore destruction cannot complete or mutate the destroyed backend");
                        finish();
                    });
                });
        }

        void stopCleanly() {
            backendCore->stop();
            waitUntil(
                "final BackendCore stop reaches Stopped",
                [this]() {
                    return backendCore->snapshot().provider.lifecycle == backend::ProviderLifecycle::Stopped;
                },
                [this]() {
                    observer.close("test complete");
                    backendCore.reset();
                    afterTicks(2, [this]() {
                        finish();
                    });
                });
        }

        void finish() {
            if (finished) {
                return;
            }
            finished = true;
            if (backendCore) {
                backendCore->stop();
                backendCore.reset();
            }
            core::SNodeC::stop();
        }

        tests::support::TestResult& result;
        std::shared_ptr<tests::codex::FakeTransportState> transport;
        tests::codex::FakeAppServerClient* appServerClient = nullptr;
        std::unique_ptr<FakeBackendCore> backendCore;
        backend::FrontendSession controller;
        backend::FrontendSession observer;
        EventLog controllerEvents;
        EventLog observerEvents;
        std::map<std::string, backend::CommandCompletion> completions;
        std::map<std::string, std::size_t> completionCounts;
        std::size_t threadListRequests = 0;
        std::size_t boundedInitialRefreshes = 0;
        std::size_t closedCallbacks = 0;
        std::map<std::string, Json> providerResultFixtures;
        std::vector<ProviderDispatchAuditEntry> providerDispatchAudit;
        std::set<std::string> attemptedProviderMethods;
        std::size_t providerDispatchAuditIndex = 0;
        std::uint64_t streamStartSequence = 0;
        std::string streamedText;
        backend::PendingRequestId pendingApprovalId;
        backend::PendingRequestId invalidatedPendingRequestId;
        std::vector<backend::PendingRequestId> reversePendingIds;
        std::vector<backend::PendingRequestId> rejectPendingIds;
        std::size_t reverseTerminalBaseline = 0;
        std::size_t pendingRemovalsBeforeInvalidation = 0;
        std::size_t invalidationsBeforePendingFailure = 0;
        std::optional<TransportCallbacks> deferredClosedCallbacks;
        std::optional<Json> deferredClosedId;
        std::optional<TransportCallbacks> staleCallbacks;
        std::optional<Json> staleId;
        backend::FrontendSession largeResultSession;
        std::size_t largeResultCompletions = 0;
        std::size_t largeResultClosedCallbacks = 0;
        std::optional<TransportCallbacks> destroyedCallbacks;
        std::optional<Json> destroyedId;
        bool finished = false;
    };
} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    int returnCode = tests::support::cTestSkipReturnCode;

    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexBackendCoreTest");
    } else {
        core::SNodeC::init(argc, argv);
        testCompleteProviderCommandPolicy(result);
        testProviderOperationHelperEdges(result);
        testReverseResponseSequencePreflight(result);
        testTemplatedConstructionAndOwnership(result);
        bool timedOut = false;
        BackendCoreRunner runner(result);
        [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
            [&timedOut]() {
                timedOut = true;
                core::SNodeC::stop();
            },
            utils::Timeval({10, 0}));

        runner.start();
        const int eventLoopResult = core::SNodeC::start(utils::Timeval({12, 0}));
        result.expectTrue(!timedOut, "BackendCore deterministic scenario finishes before watchdog");
        result.expectTrue(runner.isFinished(), "BackendCore deterministic scenario reaches clean terminal state");
        result.expectEqual(0, eventLoopResult, "BackendCore event loop exits cleanly");
        core::SNodeC::free();
        returnCode = result.processResult();
    }

    return returnCode;
}
