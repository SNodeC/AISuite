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

    using Submission = codex::AppServerClient::RawProtocol::Submission;
    constexpr std::size_t OperationCount = 4;

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
            {"userAgent", "codex-a1-4-user-integrations-commit-5-wire/1"},
        };
    }

    codex::Json remotePluginSummary() {
        return {
            {"authPolicy", "ON_USE"},
            {"enabled", true},
            {"id", "synthetic-plugin"},
            {"installPolicy", "AVAILABLE"},
            {"installed", true},
            {"name", "Synthetic Plugin"},
            {"source", {{"type", "remote"}, {"futureSourceField", true}}},
            {"futurePluginField", true},
        };
    }

    codex::Json installedResult() {
        return {
            {"marketplaceLoadErrors", codex::Json::array()},
            {"marketplaces", codex::Json::array()},
            {"futureResultField", true},
        };
    }

    codex::Json listResult() {
        return {
            {"featuredPluginIds", codex::Json::array({"synthetic-plugin"})},
            {"marketplaceLoadErrors", codex::Json::array()},
            {"marketplaces", codex::Json::array()},
            {"futureResultField", true},
        };
    }

    codex::Json readResult() {
        return {
            {"plugin",
             {
                 {"appTemplates", codex::Json::array()},
                 {"apps", codex::Json::array()},
                 {"hooks", codex::Json::array()},
                 {"marketplaceName", "synthetic-marketplace"},
                 {"mcpServers", codex::Json::array()},
                 {"skills", codex::Json::array()},
                 {"summary", remotePluginSummary()},
                 {"futureDetailField", true},
             }},
            {"futureResultField", true},
        };
    }

    codex::Json shareListResult() {
        return {
            {"data", codex::Json::array()},
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
                                 {"message", "synthetic plugin catalog failure"},
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
                              {"codex_a1_4_user_integrations_commit_5_wire_test", "Codex A1.4 User Integrations Commit 5 Wire Test", "1"}) {
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
            expect(socketReady, "Commit-5 wire test opens its isolated AF_UNIX socketpair");
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

        void buildCases() {
            typed::PluginInstalledParams installed{};
            installed.cwds =
                typed::OptionalNullable<std::vector<typed::AbsolutePathBuf>>::withValue({typed::AbsolutePathBuf{"/synthetic/workspace"}});
            installed.installSuggestionPluginNames = typed::OptionalNullable<std::vector<std::string>>::explicitNull();
            installed.raw = {{"futureInstalledField", true}};

            typed::PluginListParams list{};
            list.cwds = typed::OptionalNullable<std::vector<typed::AbsolutePathBuf>>::explicitNull();
            list.marketplaceKinds = typed::OptionalNullable<std::vector<typed::PluginListMarketplaceKind>>::withValue(
                {typed::PluginListMarketplaceKind::local(), typed::PluginListMarketplaceKind::createdByMeRemote()});
            list.raw = {{"futureListField", true}};

            typed::PluginReadParams read{};
            read.marketplacePath =
                typed::OptionalNullable<typed::AbsolutePathBuf>::withValue(typed::AbsolutePathBuf{"/synthetic/marketplace"});
            read.pluginName = "synthetic-plugin";
            read.remoteMarketplaceName = typed::OptionalNullable<std::string>::explicitNull();
            read.raw = {{"futureReadField", true}};

            typed::PluginShareListParams shareList{};
            shareList.raw = {{"futureShareListField", true}};

            auto& plugins = client->typed().plugins();
            cases.push_back(
                makeConcreteOperation<typed::PluginInstalledResponse>("plugin/installed",
                                                                      std::move(installed),
                                                                      {
                                                                          {"cwds", codex::Json::array({"/synthetic/workspace"})},
                                                                          {"futureInstalledField", true},
                                                                          {"installSuggestionPluginNames", nullptr},
                                                                      },
                                                                      installedResult(),
                                                                      [&plugins](auto params, auto handler) {
                                                                          return plugins.installed(std::move(params), std::move(handler));
                                                                      }));
            cases.push_back(makeConcreteOperation<typed::PluginListResponse>(
                "plugin/list",
                std::move(list),
                {
                    {"cwds", nullptr},
                    {"futureListField", true},
                    {"marketplaceKinds", codex::Json::array({"local", "created-by-me-remote"})},
                },
                listResult(),
                [&plugins](auto params, auto handler) {
                    return plugins.list(std::move(params), std::move(handler));
                }));
            cases.push_back(makeConcreteOperation<typed::PluginReadResponse>("plugin/read",
                                                                             std::move(read),
                                                                             {
                                                                                 {"futureReadField", true},
                                                                                 {"marketplacePath", "/synthetic/marketplace"},
                                                                                 {"pluginName", "synthetic-plugin"},
                                                                                 {"remoteMarketplaceName", nullptr},
                                                                             },
                                                                             readResult(),
                                                                             [&plugins](auto params, auto handler) {
                                                                                 return plugins.read(std::move(params), std::move(handler));
                                                                             }));
            cases.push_back(makeConcreteOperation<typed::PluginShareListResponse>("plugin/share/list",
                                                                                  std::move(shareList),
                                                                                  {{"futureShareListField", true}},
                                                                                  shareListResult(),
                                                                                  [&plugins](auto params, auto handler) {
                                                                                      return plugins.shareList(std::move(params),
                                                                                                               std::move(handler));
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
                   "all four Commit-5 operations reject locally without a callback or transport bytes");
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
                        if (method == "plugin/installed") {
                            throw std::runtime_error("intentional Commit-5 operation callback failure");
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
                   "all four Commit-5 operations cross the AF_UNIX transport exactly once");
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
            typed::PluginListParams params{};
            params.marketplaceKinds = typed::OptionalNullable<std::vector<typed::PluginListMarketplaceKind>>::withValue(
                {typed::PluginListMarketplaceKind::sharedWithMe()});
            insideSubmission = true;
            const Submission submission =
                client->typed().plugins().list(std::move(params), [this](const typed::Plugins::ListResult& operation) {
                    expect(!insideSubmission, "reentrant plugin/list completion remains asynchronous");
                    expect(operation && operation.raw == listResult(), "reentrant plugin/list shares the same decoder and RawProtocol");
                    ++reentrantCallbacks;
                    probeDuplicate();
                });
            insideSubmission = false;
            expect(submission && submission.id && submission.id->value() == 5,
                   "a C5 completion submits reentrantly with the exact next request ID");
            const codex::Json expectedEnvelope{
                {"id", 5},
                {"method", "plugin/list"},
                {"params", {{"marketplaceKinds", codex::Json::array({"shared-with-me"})}}},
            };
            const OutboundRecord& record = state->outbound.back();
            expect(record.envelope == expectedEnvelope && record.line == expectedEnvelope.dump() + "\n",
                   "reentrant plugin/list emits exact JSONL through the same transport");
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
            expect(exactlyOnce, "every Commit-5 success callback runs exactly once despite one throwing");
            const bool injected = !initialIds.empty() && state->inject({{"id", initialIds.front().value()}, {"result", installedResult()}});
            expect(injected, "a duplicate completed plugin/installed response crosses the socket for the one-completion probe");
            core::EventReceiver::atNextTick([this]() {
                expect(successCallbacks == OperationCount && successCallbackCounts["plugin/installed"] == 1,
                       "a duplicate response cannot complete plugin/installed twice");
                beginRemoteError();
            });
        }

        void beginRemoteError() {
            state->replyMode = UnixTransportState::ReplyMode::RemoteError;
            typed::PluginInstalledParams params{};
            insideSubmission = true;
            const Submission submission =
                client->typed().plugins().installed(std::move(params), [this](const typed::Plugins::InstalledResult& operation) {
                    const codex::Json expectedError{
                        {"code", -32'500},
                        {"message", "synthetic plugin catalog failure"},
                        {"data", {{"operation", "plugin/installed"}}},
                        {"futureErrorField", true},
                    };
                    expect(!insideSubmission && operation.kind == typed::Plugins::InstalledResult::Kind::RemoteError && !operation.value &&
                               operation.remoteError && operation.remoteError->raw == expectedError && operation.raw.is_null(),
                           "plugin/installed preserves its exact remote error asynchronously");
                    ++remoteCallbacks;
                    core::EventReceiver::atNextTick([this]() {
                        beginCancellation();
                    });
                });
            insideSubmission = false;
            expect(submission && submission.id && submission.id->value() == 6, "remote-error probe uses the exact next request ID");
        }

        void beginCancellation() {
            state->replyMode = UnixTransportState::ReplyMode::Hold;
            typed::PluginShareListParams params{};
            insideSubmission = true;
            const Submission submission =
                client->typed().plugins().shareList(std::move(params), [this](const typed::Plugins::ShareListResult& operation) {
                    expect(!insideSubmission && operation.kind == typed::Plugins::ShareListResult::Kind::Cancelled && !operation.value &&
                               operation.localError && operation.localError->category == codex::Error::Category::Cancelled,
                           "held plugin/share/list is cancelled once by an intentional stop");
                    ++cancellationCallbacks;
                    maybeRestart();
                });
            insideSubmission = false;
            expect(submission && submission.id && submission.id->value() == 7,
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
            typed::PluginReadParams params{};
            params.pluginName = "synthetic-plugin";
            insideSubmission = true;
            const Submission submission =
                client->typed().plugins().read(std::move(params), [this](const typed::Plugins::ReadResult& operation) {
                    expect(!insideSubmission && operation.kind == typed::Plugins::ReadResult::Kind::Cancelled && !operation.value &&
                               operation.localError && operation.localError->category == codex::Error::Category::Cancelled,
                           "unexpected process disconnect cancels held plugin/read exactly once");
                    ++disconnectCallbacks;
                    maybeFinishDisconnect();
                });
            insideSubmission = false;
            expect(submission && submission.id && submission.id->value() == 9,
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
                   "success, reentrancy, error, cancellation, reconnect invalidation, and disconnect each complete exactly once");
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

    result.expectTrue(!timedOut && runner.isFinished(), "Commit-5 AF_UNIX lifecycle matrix completes before the watchdog");
    result.expectEqual(0, startResult, "Commit-5 AF_UNIX lifecycle matrix stops the event loop cleanly");
    core::SNodeC::free();
    return result.processResult();
}
