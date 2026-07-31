/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "ai/openai/codex/detail/Transport.h"
#include "ai/openai/codex/typed/Client.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace codex = ai::openai::codex;
    namespace detail = ai::openai::codex::detail;
    namespace typed = ai::openai::codex::typed;

    using Submission = codex::AppServerClient::RawProtocol::Submission;
    constexpr std::size_t OperationCount = 7;
    constexpr std::size_t NotificationCount = 3;

    bool writeFully(int descriptor, std::string_view bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
            } else if (written < 0 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
        return true;
    }

    std::optional<std::string> readLine(int descriptor) {
        std::string line;
        for (;;) {
            char byte = '\0';
            const ssize_t received = ::read(descriptor, &byte, 1);
            if (received == 1) {
                line.push_back(byte);
                if (byte == '\n') {
                    return line;
                }
            } else if (received < 0 && errno == EINTR) {
                continue;
            } else {
                return std::nullopt;
            }
        }
    }

    codex::Json initializeResult() {
        return {
            {"codexHome", "/synthetic/codex-home"},
            {"platformFamily", "unix"},
            {"platformOs", "linux"},
            {"userAgent", "codex-a1-4-hooks-marketplace-skills-wire/1"},
        };
    }

    codex::Json hooksResult() {
        return {
            {"data", codex::Json::array()},
            {"futureResultField", true},
        };
    }

    codex::Json marketplaceAddResult() {
        return {
            {"alreadyAdded", false},
            {"installedRoot", "/synthetic/marketplaces/synthetic-marketplace"},
            {"marketplaceName", "synthetic-marketplace"},
            {"futureResultField", true},
        };
    }

    codex::Json marketplaceRemoveResult() {
        return {
            {"installedRoot", nullptr},
            {"marketplaceName", "synthetic-marketplace"},
            {"futureResultField", true},
        };
    }

    codex::Json marketplaceUpgradeResult() {
        return {
            {"errors", codex::Json::array()},
            {"selectedMarketplaces", codex::Json::array({"synthetic-marketplace"})},
            {"upgradedRoots", codex::Json::array({"/synthetic/marketplaces/synthetic-marketplace"})},
            {"futureResultField", true},
        };
    }

    codex::Json skillsWriteResult() {
        return {
            {"effectiveEnabled", false},
            {"futureResultField", true},
        };
    }

    codex::Json skillsListResult() {
        return {
            {"data", codex::Json::array()},
            {"futureResultField", true},
        };
    }

    codex::Json hookRun() {
        return {
            {"displayOrder", 1},
            {"entries", codex::Json::array()},
            {"eventName", "preToolUse"},
            {"executionMode", "sync"},
            {"handlerType", "command"},
            {"id", "synthetic-hook-run"},
            {"scope", "thread"},
            {"sourcePath", "/synthetic/hooks/pre-tool"},
            {"startedAt", 100},
            {"status", "running"},
            {"futureRunField", true},
        };
    }

    struct OutboundRecord {
        std::string line;
        codex::Json envelope;
    };

    struct UnixTransportState {
        enum class ReplyMode { Success, RemoteError, Hold };

        detail::TransportCallbacks callbacks;
        std::vector<OutboundRecord> outbound;
        std::map<std::string, codex::Json> successResults;
        ReplyMode replyMode = ReplyMode::Success;
        int clientDescriptor = -1;
        int serverDescriptor = -1;
        bool running = false;

        ~UnixTransportState() {
            if (clientDescriptor >= 0) {
                ::close(clientDescriptor);
            }
            if (serverDescriptor >= 0) {
                ::close(serverDescriptor);
            }
        }

        bool open() {
            int descriptors[2]{-1, -1};
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) != 0) {
                return false;
            }
            clientDescriptor = descriptors[0];
            serverDescriptor = descriptors[1];
            return true;
        }

        bool inject(const codex::Json& envelope) {
            const std::string serverLine = envelope.dump() + "\n";
            if (!writeFully(serverDescriptor, serverLine)) {
                return false;
            }
            const std::optional<std::string> received = readLine(clientDescriptor);
            if (!received || *received != serverLine || !callbacks.onMessage) {
                return false;
            }
            callbacks.onMessage(received->substr(0, received->size() - 1));
            return true;
        }

        bool send(std::string message) {
            const std::string clientLine = message + "\n";
            if (!writeFully(clientDescriptor, clientLine)) {
                return false;
            }
            const std::optional<std::string> received = readLine(serverDescriptor);
            if (!received || *received != clientLine) {
                return false;
            }

            codex::Json envelope = codex::Json::parse(received->begin(), received->end() - 1, nullptr, false);
            if (envelope.is_discarded()) {
                return false;
            }
            outbound.push_back({*received, envelope});

            const auto method = envelope.find("method");
            if (method == envelope.end() || !method->is_string()) {
                return true;
            }
            if (*method == "initialize") {
                const auto id = envelope.find("id");
                return id != envelope.end() && inject({{"id", *id}, {"result", initializeResult()}});
            }
            if (*method == "initialized" || envelope.find("id") == envelope.end()) {
                return true;
            }
            if (replyMode == ReplyMode::Hold) {
                return true;
            }

            const std::string operation = *method;
            if (replyMode == ReplyMode::RemoteError) {
                return inject({{"id", envelope.at("id")},
                               {"error",
                                {{"code", -32'500},
                                 {"message", "synthetic user-integration remote failure"},
                                 {"data", {{"operation", operation}}},
                                 {"futureErrorField", true}}}});
            }

            const auto result = successResults.find(operation);
            return result != successResults.end() && inject({{"id", envelope.at("id")}, {"result", result->second}});
        }
    };

    class UnixTranscriptTransport final : public detail::Transport {
    public:
        explicit UnixTranscriptTransport(std::shared_ptr<UnixTransportState> state)
            : state(std::move(state)) {
        }

        void setCallbacks(detail::TransportCallbacks callbacks) override {
            state->callbacks = std::move(callbacks);
        }

        void start() override {
            state->running = true;
            if (state->callbacks.onStarted) {
                state->callbacks.onStarted();
            }
        }

        bool send(std::string message) override {
            return state->send(std::move(message));
        }

        void stop() override {
            if (!std::exchange(state->running, false)) {
                return;
            }
            if (state->callbacks.onExited) {
                state->callbacks.onExited(detail::ProcessExit{true, 0, false, 0});
            }
        }

    private:
        std::shared_ptr<UnixTransportState> state;
    };

    class TestClient final : public codex::AppServerClient {
    public:
        explicit TestClient(const std::shared_ptr<UnixTransportState>& state)
            : AppServerClient(std::make_unique<UnixTranscriptTransport>(state),
                              {"codex_a1_4_hooks_marketplace_skills_wire_test", "Codex A1.4 Hooks Marketplace Skills Wire Test", "1"}) {
        }
    };

    struct OperationCase {
        std::string method;
        codex::Json expectedParams;
        codex::Json expectedResult;
        std::function<Submission(std::function<void(bool, const codex::Json&)>)> invoke;
    };

    class WireRunner {
    public:
        explicit WireRunner(tests::support::TestResult& result)
            : result(result)
            , state(std::make_shared<UnixTransportState>())
            , socketReady(state->open())
            , client(std::make_unique<TestClient>(state)) {
            buildCases();
        }

        void start() {
            expect(socketReady, "hooks/marketplace/skills wire test opens its isolated AF_UNIX socketpair");
            if (!socketReady) {
                finished = true;
                core::SNodeC::stop();
                return;
            }

            client->typed().events().setOnEvent([this](const typed::Event& event) {
                std::string method;
                if (std::holds_alternative<typed::HookCompletedNotification>(event)) {
                    method = "hook/completed";
                } else if (std::holds_alternative<typed::HookStartedNotification>(event)) {
                    method = "hook/started";
                } else if (std::holds_alternative<typed::SkillsChangedNotification>(event)) {
                    method = "skills/changed";
                } else {
                    return;
                }
                eventOrder.push_back("typed-" + method);
                ++typedNotifications;
                if (method == "hook/started" && !reentrantSubmitted) {
                    submitReentrant();
                    typedCallbackThrew = true;
                    throw std::runtime_error("intentional hooks/marketplace/skills typed notification callback failure");
                }
                maybeCompleteSuccess();
            });
            client->raw().setOnNotification([this](const codex::Notification& wire) {
                if (wire.method == "hook/completed" || wire.method == "hook/started" || wire.method == "skills/changed") {
                    eventOrder.push_back("raw-" + wire.method);
                    ++rawNotifications;
                    maybeCompleteSuccess();
                }
            });
            client->setOnStateChanged([this](const codex::StateChange& change) {
                if (change.current == codex::State::Ready) {
                    ++readyCount;
                    if (readyCount == 1) {
                        beginSuccess();
                    } else if (readyCount == 2) {
                        beginDisconnect();
                    }
                } else if (change.current == codex::State::Stopped && readyCount == 1) {
                    cancellationStopObserved = true;
                    maybeRestart();
                } else if (change.current == codex::State::Failed && readyCount == 2) {
                    disconnectFailureObserved = change.error.has_value() && change.error->category == codex::Error::Category::Process;
                    maybeFinishDisconnect();
                }
            });

            invokeLocalRejections();
            client->start();
        }

        bool isFinished() const noexcept {
            return finished;
        }

    private:
        void expect(bool condition, const std::string& description) {
            result.expectTrue(condition, description);
        }

        template <typename Result, typename Params, typename Submit>
        OperationCase
        makeConcreteOperation(std::string method, Params params, codex::Json expectedParams, codex::Json expectedResult, Submit submit) {
            state->successResults.emplace(method, expectedResult);
            return {
                method,
                std::move(expectedParams),
                expectedResult,
                [params = std::move(params), submit = std::move(submit)](std::function<void(bool, const codex::Json&)> completion) mutable {
                    return submit(params, [completion = std::move(completion)](const typed::OperationResult<Result>& operation) {
                        completion(operation.kind == typed::OperationResult<Result>::Kind::Success && operation.value.has_value() &&
                                       operation.value->raw == operation.raw,
                                   operation.raw);
                    });
                },
            };
        }

        template <typename Params, typename Submit>
        OperationCase
        makeUnitOperation(std::string method, Params params, codex::Json expectedParams, codex::Json expectedResult, Submit submit) {
            state->successResults.emplace(method, expectedResult);
            return {
                method,
                std::move(expectedParams),
                expectedResult,
                [params = std::move(params), submit = std::move(submit)](std::function<void(bool, const codex::Json&)> completion) mutable {
                    return submit(params, [completion = std::move(completion)](const typed::OperationResult<typed::Unit>& operation) {
                        completion(operation.kind == typed::OperationResult<typed::Unit>::Kind::Success && operation.value.has_value() &&
                                       operation.raw == codex::Json::object(),
                                   operation.raw);
                    });
                },
            };
        }

        void buildCases() {
            typed::HooksListParams hooksParams{};
            hooksParams.cwds = std::vector<std::string>{"/synthetic/workspace"};

            typed::MarketplaceAddParams addParams{};
            addParams.refName = typed::OptionalNullable<std::string>::explicitNull();
            addParams.source = "https://example.invalid/synthetic-marketplace.git";
            addParams.sparsePaths = typed::OptionalNullable<std::vector<std::string>>::withValue({});

            typed::MarketplaceRemoveParams removeParams{};
            removeParams.marketplaceName = "synthetic-marketplace";

            typed::MarketplaceUpgradeParams upgradeParams{};

            typed::SkillsConfigWriteParams writeParams{};
            writeParams.enabled = false;
            writeParams.name = typed::OptionalNullable<std::string>::explicitNull();
            writeParams.path =
                typed::OptionalNullable<typed::AbsolutePathBuf>::withValue(typed::AbsolutePathBuf{"/synthetic/skills/synthetic-skill"});

            typed::SkillsExtraRootsSetParams rootsParams{};
            rootsParams.extraRoots = {typed::AbsolutePathBuf{"/synthetic/skills/one"}};

            typed::SkillsListParams listParams{};
            listParams.cwds = std::vector<std::string>{};
            listParams.forceReload = false;

            auto& hooks = client->typed().hooks();
            auto& marketplace = client->typed().marketplace();
            auto& skills = client->typed().skills();

            cases.push_back(makeConcreteOperation<typed::HooksListResponse>("hooks/list",
                                                                            std::move(hooksParams),
                                                                            {{"cwds", codex::Json::array({"/synthetic/workspace"})}},
                                                                            hooksResult(),
                                                                            [&hooks](auto params, auto handler) {
                                                                                return hooks.list(std::move(params), std::move(handler));
                                                                            }));
            cases.push_back(
                makeConcreteOperation<typed::MarketplaceAddResponse>("marketplace/add",
                                                                     std::move(addParams),
                                                                     {
                                                                         {"refName", nullptr},
                                                                         {"source", "https://example.invalid/synthetic-marketplace.git"},
                                                                         {"sparsePaths", codex::Json::array()},
                                                                     },
                                                                     marketplaceAddResult(),
                                                                     [&marketplace](auto params, auto handler) {
                                                                         return marketplace.add(std::move(params), std::move(handler));
                                                                     }));
            cases.push_back(makeConcreteOperation<typed::MarketplaceRemoveResponse>("marketplace/remove",
                                                                                    std::move(removeParams),
                                                                                    {{"marketplaceName", "synthetic-marketplace"}},
                                                                                    marketplaceRemoveResult(),
                                                                                    [&marketplace](auto params, auto handler) {
                                                                                        return marketplace.remove(std::move(params),
                                                                                                                  std::move(handler));
                                                                                    }));
            cases.push_back(makeConcreteOperation<typed::MarketplaceUpgradeResponse>("marketplace/upgrade",
                                                                                     std::move(upgradeParams),
                                                                                     codex::Json::object(),
                                                                                     marketplaceUpgradeResult(),
                                                                                     [&marketplace](auto params, auto handler) {
                                                                                         return marketplace.upgrade(std::move(params),
                                                                                                                    std::move(handler));
                                                                                     }));
            cases.push_back(makeConcreteOperation<typed::SkillsConfigWriteResponse>("skills/config/write",
                                                                                    std::move(writeParams),
                                                                                    {
                                                                                        {"enabled", false},
                                                                                        {"name", nullptr},
                                                                                        {"path", "/synthetic/skills/synthetic-skill"},
                                                                                    },
                                                                                    skillsWriteResult(),
                                                                                    [&skills](auto params, auto handler) {
                                                                                        return skills.writeConfig(std::move(params),
                                                                                                                  std::move(handler));
                                                                                    }));
            cases.push_back(makeUnitOperation("skills/extraRoots/set",
                                              std::move(rootsParams),
                                              {{"extraRoots", codex::Json::array({"/synthetic/skills/one"})}},
                                              codex::Json::object(),
                                              [&skills](auto params, auto handler) {
                                                  return skills.setExtraRoots(std::move(params), std::move(handler));
                                              }));
            cases.push_back(makeConcreteOperation<typed::SkillsListResponse>("skills/list",
                                                                             std::move(listParams),
                                                                             {{"cwds", codex::Json::array()}, {"forceReload", false}},
                                                                             skillsListResult(),
                                                                             [&skills](auto params, auto handler) {
                                                                                 return skills.list(std::move(params), std::move(handler));
                                                                             }));
        }

        void invokeLocalRejections() {
            const std::size_t before = state->outbound.size();
            std::size_t exact = 0;
            for (OperationCase& operation : cases) {
                insideSubmission = true;
                const Submission submission = operation.invoke([this, method = operation.method](bool, const codex::Json&) {
                    ++unexpectedLocalCallbacks;
                    expect(false, method + " local rejection must not invoke its asynchronous callback");
                });
                insideSubmission = false;
                const detail::ProtocolSurfaceEntry* row =
                    detail::findSurface(detail::SurfaceCategory::ClientRequest, "ClientRequest", "method", operation.method);
                const bool registryExact = row != nullptr && std::holds_alternative<detail::ClientRequestTarget>(row->runtimeTarget);
                const bool rejected =
                    !submission && !submission.id && submission.error && submission.error->category == codex::Error::Category::InvalidState;
                exact += rejected && registryExact ? 1U : 0U;
                expect(rejected, operation.method + " rejects synchronously before RawProtocol is Ready");
                expect(registryExact, operation.method + " resolves through its exact registry identity");
            }
            expect(exact == OperationCount && unexpectedLocalCallbacks == 0 && state->outbound.size() == before,
                   "all seven local rejections produce no callback or transport bytes");
        }

        void beginSuccess() {
            state->replyMode = UnixTransportState::ReplyMode::Success;
            const std::size_t before = state->outbound.size();

            for (std::size_t index = 0; index < cases.size(); ++index) {
                OperationCase& operation = cases[index];
                insideSubmission = true;
                const Submission submission = operation.invoke(
                    [this, method = operation.method, expected = operation.expectedResult](bool typedSuccess, const codex::Json& raw) {
                        expect(!insideSubmission, method + " completion remains asynchronous");
                        expect(typedSuccess && raw == expected, method + " decodes its exact typed result and retains raw JSON");
                        ++successCallbackCounts[method];
                        ++successCallbacks;
                        if (method == "marketplace/add") {
                            throw std::runtime_error("intentional hooks/marketplace/skills operation callback failure");
                        }
                        if (successCallbacks == OperationCount) {
                            injectNotifications();
                        }
                    });
                insideSubmission = false;
                expect(submission && submission.id && !submission.error, operation.method + " is accepted by the one RawProtocol engine");
                if (submission.id) {
                    initialIds.push_back(*submission.id);
                    expect(submission.id->value() == static_cast<std::int64_t>(index + 1),
                           operation.method + " receives the exact sequential request ID");
                }
            }

            expect(state->outbound.size() == before + OperationCount, "all seven operations cross the AF_UNIX transport exactly once");
            for (std::size_t index = 0; index < cases.size(); ++index) {
                const codex::Json expectedEnvelope{
                    {"id", static_cast<std::int64_t>(index + 1)},
                    {"method", cases[index].method},
                    {"params", cases[index].expectedParams},
                };
                const OutboundRecord& record = state->outbound[before + index];
                expect(record.envelope == expectedEnvelope && record.line == expectedEnvelope.dump() + "\n",
                       cases[index].method + " sends exact request-id/method/params JSONL bytes");
            }
        }

        void injectNotifications() {
            if (notificationsInjected) {
                return;
            }
            notificationsInjected = true;
            const std::vector<codex::Json> envelopes{
                {
                    {"jsonrpc", "2.0"},
                    {"method", "hook/completed"},
                    {"params", {{"run", hookRun()}, {"threadId", "synthetic-thread"}, {"turnId", nullptr}, {"futureParam", "completed"}}},
                },
                {
                    {"jsonrpc", "2.0"},
                    {"method", "hook/started"},
                    {"params",
                     {{"run", hookRun()}, {"threadId", "synthetic-thread"}, {"turnId", "synthetic-turn"}, {"futureParam", "started"}}},
                },
                {
                    {"jsonrpc", "2.0"},
                    {"method", "skills/changed"},
                    {"params", {{"futureSkillField", "synthetic-future-skill"}}},
                },
            };
            for (const codex::Json& envelope : envelopes) {
                expect(state->inject(envelope), envelope.at("method").get<std::string>() + " crosses the same AF_UNIX transport");
            }
            maybeCompleteSuccess();
        }

        void submitReentrant() {
            reentrantSubmitted = true;
            typed::SkillsListParams params{};
            insideSubmission = true;
            const Submission submission =
                client->typed().skills().list(std::move(params), [this](const typed::Skills::ListResult& operation) {
                    expect(!insideSubmission, "reentrant skills/list completion remains asynchronous");
                    expect(operation && operation.raw == skillsListResult(),
                           "reentrant skills/list shares the same result decoder and RawProtocol");
                    ++reentrantCallbacks;
                    maybeCompleteSuccess();
                });
            insideSubmission = false;
            expect(submission && submission.id && submission.id->value() == 8,
                   "typed hook callback submits a reentrant request with the next exact request ID");
            const codex::Json expectedEnvelope{
                {"id", 8},
                {"method", "skills/list"},
                {"params", codex::Json::object()},
            };
            const OutboundRecord& record = state->outbound.back();
            expect(record.envelope == expectedEnvelope && record.line == expectedEnvelope.dump() + "\n",
                   "reentrant skills/list emits exact JSONL through the same transport");
        }

        void maybeCompleteSuccess() {
            if (successCallbacks != OperationCount || typedNotifications != NotificationCount || rawNotifications != NotificationCount ||
                reentrantCallbacks != 1 || duplicateProbeStarted) {
                return;
            }
            duplicateProbeStarted = true;
            bool exactlyOnce = true;
            for (const OperationCase& operation : cases) {
                exactlyOnce = exactlyOnce && successCallbackCounts[operation.method] == 1;
            }
            expect(exactlyOnce && typedCallbackThrew,
                   "every hooks/marketplace/skills operation completes once and typed notification exceptions remain contained");
            const std::vector<std::string> expectedOrder{
                "typed-hook/completed",
                "raw-hook/completed",
                "typed-hook/started",
                "raw-hook/started",
                "typed-skills/changed",
                "raw-skills/changed",
            };
            expect(eventOrder == expectedOrder, "typed notification delivery precedes the coexisting raw observer for every method");

            const bool injected = !initialIds.empty() && state->inject({{"id", initialIds.front().value()}, {"result", hooksResult()}});
            expect(injected, "a duplicate completed response crosses the socket for the one-completion probe");
            core::EventReceiver::atNextTick([this]() {
                expect(successCallbacks == OperationCount && successCallbackCounts["hooks/list"] == 1,
                       "a duplicate response cannot complete hooks/list twice");
                beginRemoteError();
            });
        }

        void beginRemoteError() {
            state->replyMode = UnixTransportState::ReplyMode::RemoteError;
            typed::MarketplaceUpgradeParams params{};
            insideSubmission = true;
            const Submission submission =
                client->typed().marketplace().upgrade(std::move(params), [this](const typed::Marketplace::UpgradeResult& operation) {
                    const codex::Json expectedError{
                        {"code", -32'500},
                        {"message", "synthetic user-integration remote failure"},
                        {"data", {{"operation", "marketplace/upgrade"}}},
                        {"futureErrorField", true},
                    };
                    expect(!insideSubmission && operation.kind == typed::Marketplace::UpgradeResult::Kind::RemoteError &&
                               !operation.value && operation.remoteError && operation.remoteError->raw == expectedError &&
                               operation.raw.is_null(),
                           "marketplace/upgrade retains its exact remote error asynchronously");
                    ++remoteCallbacks;
                    core::EventReceiver::atNextTick([this]() {
                        beginCancellation();
                    });
                });
            insideSubmission = false;
            expect(submission && submission.id && submission.id->value() == 9,
                   "remote-error probe is accepted with the next exact request ID");
        }

        void beginCancellation() {
            state->replyMode = UnixTransportState::ReplyMode::Hold;
            typed::SkillsListParams params{};
            insideSubmission = true;
            const Submission submission =
                client->typed().skills().list(std::move(params), [this](const typed::Skills::ListResult& operation) {
                    expect(!insideSubmission && operation.kind == typed::Skills::ListResult::Kind::Cancelled && !operation.value &&
                               operation.localError && operation.localError->category == codex::Error::Category::Cancelled,
                           "held skills/list is cancelled once by an intentional stop");
                    ++cancellationCallbacks;
                    maybeRestart();
                });
            insideSubmission = false;
            expect(submission && submission.id && submission.id->value() == 10,
                   "cancellation probe is pending at the exact next request ID");
            client->stop();
        }

        void maybeRestart() {
            if (restartScheduled || !cancellationStopObserved || cancellationCallbacks != 1 || remoteCallbacks != 1) {
                return;
            }
            restartScheduled = true;
            core::EventReceiver::atNextTick([this]() {
                client->start();
            });
        }

        void beginDisconnect() {
            state->replyMode = UnixTransportState::ReplyMode::Hold;
            typed::HooksListParams params{};
            insideSubmission = true;
            const Submission submission =
                client->typed().hooks().list(std::move(params), [this](const typed::Hooks::ListResult& operation) {
                    expect(!insideSubmission && operation.kind == typed::Hooks::ListResult::Kind::Cancelled && !operation.value &&
                               operation.localError && operation.localError->category == codex::Error::Category::Cancelled,
                           "unexpected process disconnect cancels the held hooks/list exactly once");
                    ++disconnectCallbacks;
                    maybeFinishDisconnect();
                });
            insideSubmission = false;
            expect(submission && submission.id && submission.id->value() == 12,
                   "post-reconnect disconnect probe uses the preserved request-ID allocator");
            core::EventReceiver::atNextTick([this]() {
                if (state->callbacks.onExited) {
                    state->callbacks.onExited(detail::ProcessExit{true, 23, false, 0});
                }
            });
        }

        void maybeFinishDisconnect() {
            if (finished || disconnectCallbacks != 1 || !disconnectFailureObserved) {
                return;
            }
            finished = true;
            expect(successCallbacks == OperationCount && typedNotifications == NotificationCount && rawNotifications == NotificationCount &&
                       reentrantCallbacks == 1 && remoteCallbacks == 1 && cancellationCallbacks == 1 && disconnectCallbacks == 1,
                   "success, notifications, reentrancy, error, cancellation, and disconnect each complete exactly once");
            core::SNodeC::stop();
        }

        tests::support::TestResult& result;
        std::shared_ptr<UnixTransportState> state;
        bool socketReady = false;
        std::unique_ptr<TestClient> client;
        std::vector<OperationCase> cases;
        std::vector<codex::ClientRequestId> initialIds;
        std::map<std::string, std::size_t> successCallbackCounts;
        std::vector<std::string> eventOrder;
        std::size_t successCallbacks = 0;
        std::size_t typedNotifications = 0;
        std::size_t rawNotifications = 0;
        std::size_t reentrantCallbacks = 0;
        std::size_t remoteCallbacks = 0;
        std::size_t cancellationCallbacks = 0;
        std::size_t disconnectCallbacks = 0;
        std::size_t unexpectedLocalCallbacks = 0;
        int readyCount = 0;
        bool insideSubmission = false;
        bool notificationsInjected = false;
        bool reentrantSubmitted = false;
        bool typedCallbackThrew = false;
        bool duplicateProbeStarted = false;
        bool cancellationStopObserved = false;
        bool restartScheduled = false;
        bool disconnectFailureObserved = false;
        bool finished = false;
    };
} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    core::SNodeC::init(argc, argv);

    WireRunner runner(result);
    runner.start();

    bool timedOut = false;
    [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
        [&]() {
            timedOut = true;
            core::SNodeC::stop();
        },
        utils::Timeval({12, 0}));
    const int startResult = core::SNodeC::start(utils::Timeval({13, 0}));

    result.expectTrue(!timedOut && runner.isFinished(), "hooks/marketplace/skills AF_UNIX lifecycle matrix completes before the watchdog");
    result.expectEqual(0, startResult, "hooks/marketplace/skills AF_UNIX lifecycle matrix stops the event loop cleanly");
    core::SNodeC::free();
    return result.processResult();
}
