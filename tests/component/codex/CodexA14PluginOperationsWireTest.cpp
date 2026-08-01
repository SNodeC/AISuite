/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/Api.h"
#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "ai/openai/codex/detail/Transport.h"
#include "ai/openai/codex/typed/Plugins.h"
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

    using Submission = codex::Submission;
    constexpr std::size_t OperationCount = 7;

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
            {"userAgent", "codex-a1-4-plugin-operations-wire/1"},
        };
    }

    codex::Json installResult() {
        return {
            {"appsNeedingAuth", codex::Json::array()},
            {"authPolicy", "ON_INSTALL"},
            {"futureResultField", true},
        };
    }

    codex::Json checkoutResult() {
        return {
            {"marketplaceName", "synthetic-marketplace"},
            {"marketplacePath", "/synthetic/marketplaces/synthetic-marketplace"},
            {"pluginId", "synthetic-plugin"},
            {"pluginName", "Synthetic Plugin"},
            {"pluginPath", "/synthetic/plugins/synthetic-plugin"},
            {"remotePluginId", "synthetic-remote-plugin"},
            {"remoteVersion", nullptr},
            {"futureResultField", true},
        };
    }

    codex::Json saveResult() {
        return {
            {"remotePluginId", "synthetic-remote-plugin"},
            {"shareUrl", "https://example.invalid/shares/synthetic-plugin"},
            {"futureResultField", true},
        };
    }

    codex::Json updateTargetsResult() {
        return {
            {"discoverability", "PRIVATE"},
            {"principals", codex::Json::array()},
            {"futureResultField", true},
        };
    }

    codex::Json readSkillResult() {
        return {
            {"contents", "Synthetic skill contents"},
            {"futureResultField", true},
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
                                 {"message", "synthetic plugin remote failure"},
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
                              {"codex_a1_4_plugin_operations_wire_test", "Codex A1.4 Plugin Operations Wire Test", "1"}) {
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
            expect(socketReady, "plugin-operation wire test opens its isolated AF_UNIX socketpair");
            if (!socketReady) {
                finished = true;
                core::SNodeC::stop();
                return;
            }

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
        OperationCase makeUnitOperation(std::string method, Params params, codex::Json expectedParams, Submit submit) {
            const codex::Json expectedResult = codex::Json::object();
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
            typed::PluginInstallParams install{};
            install.marketplacePath =
                typed::OptionalNullable<typed::AbsolutePath>::withValue(typed::AbsolutePath{"/synthetic/marketplace"});
            install.pluginName = "synthetic-plugin";
            install.remoteMarketplaceName = typed::OptionalNullable<std::string>::explicitNull();

            typed::PluginShareCheckoutParams checkout{};
            checkout.remotePluginId = "synthetic-remote-plugin";

            typed::PluginShareDeleteParams deleteParams{};
            deleteParams.remotePluginId = "synthetic-remote-plugin";

            typed::PluginShareTarget shareTarget{};
            shareTarget.principalId = "synthetic-principal";
            shareTarget.principalType = typed::PluginSharePrincipalType::user();
            shareTarget.role = typed::PluginShareTargetRole::reader();

            typed::PluginShareSaveParams save{};
            save.discoverability = typed::OptionalNullable<typed::PluginShareDiscoverability>::explicitNull();
            save.pluginPath = typed::AbsolutePath{"/synthetic/plugins/synthetic-plugin"};
            save.remotePluginId = typed::OptionalNullable<std::string>::withValue("synthetic-remote-plugin");
            save.shareTargets = typed::OptionalNullable<std::vector<typed::PluginShareTarget>>::withValue({shareTarget});

            typed::PluginShareUpdateTargetsParams updateTargets{};
            updateTargets.discoverability = typed::PluginShareUpdateDiscoverability::privateVisibility();
            updateTargets.remotePluginId = "synthetic-remote-plugin";
            updateTargets.shareTargets = {shareTarget};

            typed::PluginSkillReadParams readSkill{};
            readSkill.remoteMarketplaceName = "synthetic-marketplace";
            readSkill.remotePluginId = "synthetic-remote-plugin";
            readSkill.skillName = "synthetic-skill";

            typed::PluginUninstallParams uninstall{};
            uninstall.pluginId = "synthetic-plugin";

            auto& plugins = client->plugins();
            cases.push_back(makeConcreteOperation<typed::PluginInstallResponse>("plugin/install",
                                                                                std::move(install),
                                                                                {
                                                                                    {"marketplacePath", "/synthetic/marketplace"},
                                                                                    {"pluginName", "synthetic-plugin"},
                                                                                    {"remoteMarketplaceName", nullptr},
                                                                                },
                                                                                installResult(),
                                                                                [&plugins](auto params, auto handler) {
                                                                                    return plugins.install(std::move(params),
                                                                                                           std::move(handler));
                                                                                }));
            cases.push_back(makeConcreteOperation<typed::PluginShareCheckoutResponse>("plugin/share/checkout",
                                                                                      std::move(checkout),
                                                                                      {{"remotePluginId", "synthetic-remote-plugin"}},
                                                                                      checkoutResult(),
                                                                                      [&plugins](auto params, auto handler) {
                                                                                          return plugins.shareCheckout(std::move(params),
                                                                                                                       std::move(handler));
                                                                                      }));
            cases.push_back(makeUnitOperation("plugin/share/delete",
                                              std::move(deleteParams),
                                              {{"remotePluginId", "synthetic-remote-plugin"}},
                                              [&plugins](auto params, auto handler) {
                                                  return plugins.shareDelete(std::move(params), std::move(handler));
                                              }));
            cases.push_back(makeConcreteOperation<typed::PluginShareSaveResponse>(
                "plugin/share/save",
                std::move(save),
                {
                    {"discoverability", nullptr},
                    {"pluginPath", "/synthetic/plugins/synthetic-plugin"},
                    {"remotePluginId", "synthetic-remote-plugin"},
                    {"shareTargets",
                     codex::Json::array({{{"principalId", "synthetic-principal"}, {"principalType", "user"}, {"role", "reader"}}})},
                },
                saveResult(),
                [&plugins](auto params, auto handler) {
                    return plugins.shareSave(std::move(params), std::move(handler));
                }));
            cases.push_back(makeConcreteOperation<typed::PluginShareUpdateTargetsResponse>(
                "plugin/share/updateTargets",
                std::move(updateTargets),
                {
                    {"discoverability", "PRIVATE"},
                    {"remotePluginId", "synthetic-remote-plugin"},
                    {"shareTargets",
                     codex::Json::array({{{"principalId", "synthetic-principal"}, {"principalType", "user"}, {"role", "reader"}}})},
                },
                updateTargetsResult(),
                [&plugins](auto params, auto handler) {
                    return plugins.shareUpdateTargets(std::move(params), std::move(handler));
                }));
            cases.push_back(makeConcreteOperation<typed::PluginSkillReadResponse>("plugin/skill/read",
                                                                                  std::move(readSkill),
                                                                                  {
                                                                                      {"remoteMarketplaceName", "synthetic-marketplace"},
                                                                                      {"remotePluginId", "synthetic-remote-plugin"},
                                                                                      {"skillName", "synthetic-skill"},
                                                                                  },
                                                                                  readSkillResult(),
                                                                                  [&plugins](auto params, auto handler) {
                                                                                      return plugins.readSkill(std::move(params),
                                                                                                               std::move(handler));
                                                                                  }));
            cases.push_back(makeUnitOperation(
                "plugin/uninstall", std::move(uninstall), {{"pluginId", "synthetic-plugin"}}, [&plugins](auto params, auto handler) {
                    return plugins.uninstall(std::move(params), std::move(handler));
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
                   "all seven plugin-operation operations reject locally without a callback or transport bytes");
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
                        if (method == "plugin/install") {
                            throw std::runtime_error("intentional plugin-operation operation callback failure");
                        }
                        if (successCallbacks == OperationCount) {
                            submitReentrant();
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

            expect(state->outbound.size() == before + OperationCount,
                   "all seven plugin-operation operations cross the AF_UNIX transport exactly once");
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

        void submitReentrant() {
            if (reentrantSubmitted) {
                return;
            }
            reentrantSubmitted = true;
            typed::PluginSkillReadParams params{};
            params.remoteMarketplaceName = "synthetic-marketplace";
            params.remotePluginId = "synthetic-remote-plugin";
            params.skillName = "synthetic-skill";
            insideSubmission = true;
            const Submission submission = client->plugins().readSkill(
                std::move(params), [this](const typed::OperationResult<typed::PluginSkillReadResponse>& operation) {
                    expect(!insideSubmission, "reentrant plugin/skill/read completion remains asynchronous");
                    expect(operation && operation.raw == readSkillResult(),
                           "reentrant plugin/skill/read shares the same decoder and RawProtocol");
                    ++reentrantCallbacks;
                    probeDuplicate();
                });
            insideSubmission = false;
            expect(submission && submission.id && submission.id->value() == 8,
                   "a plugin completion submits reentrantly with the exact next request ID");
            const codex::Json expectedEnvelope{
                {"id", 8},
                {"method", "plugin/skill/read"},
                {"params",
                 {
                     {"remoteMarketplaceName", "synthetic-marketplace"},
                     {"remotePluginId", "synthetic-remote-plugin"},
                     {"skillName", "synthetic-skill"},
                 }},
            };
            const OutboundRecord& record = state->outbound.back();
            expect(record.envelope == expectedEnvelope && record.line == expectedEnvelope.dump() + "\n",
                   "reentrant plugin/skill/read emits exact JSONL through the same transport");
        }

        void probeDuplicate() {
            if (duplicateProbeStarted) {
                return;
            }
            duplicateProbeStarted = true;
            bool exactlyOnce = successCallbacks == OperationCount && reentrantCallbacks == 1;
            for (const OperationCase& operation : cases) {
                exactlyOnce = exactlyOnce && successCallbackCounts[operation.method] == 1;
            }
            expect(exactlyOnce, "every plugin-operation success callback runs exactly once despite one throwing");
            const bool injected = !initialIds.empty() && state->inject({{"id", initialIds.front().value()}, {"result", installResult()}});
            expect(injected, "a duplicate completed plugin response crosses the socket for the one-completion probe");
            core::EventReceiver::atNextTick([this]() {
                expect(successCallbacks == OperationCount && successCallbackCounts["plugin/install"] == 1,
                       "a duplicate response cannot complete plugin/install twice");
                beginRemoteError();
            });
        }

        void beginRemoteError() {
            state->replyMode = UnixTransportState::ReplyMode::RemoteError;
            typed::PluginInstallParams params{};
            params.pluginName = "synthetic-plugin";
            insideSubmission = true;
            const Submission submission =
                client->plugins().install(std::move(params), [this](const typed::OperationResult<typed::PluginInstallResponse>& operation) {
                    const codex::Json expectedError{
                        {"code", -32'500},
                        {"message", "synthetic plugin remote failure"},
                        {"data", {{"operation", "plugin/install"}}},
                        {"futureErrorField", true},
                    };
                    expect(!insideSubmission && operation.kind == typed::OperationResult<typed::PluginInstallResponse>::Kind::RemoteError &&
                               !operation.value && operation.remoteError && operation.remoteError->raw == expectedError &&
                               operation.raw.is_null(),
                           "plugin/install preserves its exact remote error asynchronously");
                    ++remoteCallbacks;
                    core::EventReceiver::atNextTick([this]() {
                        beginCancellation();
                    });
                });
            insideSubmission = false;
            expect(submission && submission.id && submission.id->value() == 9, "remote-error probe uses the exact next request ID");
        }

        void beginCancellation() {
            state->replyMode = UnixTransportState::ReplyMode::Hold;
            typed::PluginShareSaveParams params{};
            params.pluginPath = typed::AbsolutePath{"/synthetic/plugins/synthetic-plugin"};
            insideSubmission = true;
            const Submission submission = client->plugins().shareSave(
                std::move(params), [this](const typed::OperationResult<typed::PluginShareSaveResponse>& operation) {
                    expect(!insideSubmission && operation.kind == typed::OperationResult<typed::PluginShareSaveResponse>::Kind::Cancelled &&
                               !operation.value && operation.localError &&
                               operation.localError->category == codex::Error::Category::Cancelled,
                           "held plugin/share/save is cancelled once by an intentional stop");
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
            typed::PluginShareCheckoutParams params{};
            params.remotePluginId = "synthetic-remote-plugin";
            insideSubmission = true;
            const Submission submission = client->plugins().shareCheckout(
                std::move(params), [this](const typed::OperationResult<typed::PluginShareCheckoutResponse>& operation) {
                    expect(!insideSubmission &&
                               operation.kind == typed::OperationResult<typed::PluginShareCheckoutResponse>::Kind::Cancelled &&
                               !operation.value && operation.localError &&
                               operation.localError->category == codex::Error::Category::Cancelled,
                           "unexpected process disconnect cancels held plugin/share/checkout exactly once");
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
            expect(successCallbacks == OperationCount && reentrantCallbacks == 1 && remoteCallbacks == 1 && cancellationCallbacks == 1 &&
                       disconnectCallbacks == 1 && unexpectedLocalCallbacks == 0,
                   "success, reentrancy, error, cancellation, and disconnect each complete exactly once");
            core::SNodeC::stop();
        }

        tests::support::TestResult& result;
        std::shared_ptr<UnixTransportState> state;
        bool socketReady = false;
        std::unique_ptr<TestClient> client;
        std::vector<OperationCase> cases;
        std::vector<codex::ClientRequestId> initialIds;
        std::map<std::string, std::size_t> successCallbackCounts;
        std::size_t successCallbacks = 0;
        std::size_t reentrantCallbacks = 0;
        std::size_t remoteCallbacks = 0;
        std::size_t cancellationCallbacks = 0;
        std::size_t disconnectCallbacks = 0;
        std::size_t unexpectedLocalCallbacks = 0;
        int readyCount = 0;
        bool insideSubmission = false;
        bool reentrantSubmitted = false;
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

    result.expectTrue(!timedOut && runner.isFinished(), "plugin-operation AF_UNIX lifecycle matrix completes before the watchdog");
    result.expectEqual(0, startResult, "plugin-operation AF_UNIX lifecycle matrix stops the event loop cleanly");
    core::SNodeC::free();
    return result.processResult();
}
