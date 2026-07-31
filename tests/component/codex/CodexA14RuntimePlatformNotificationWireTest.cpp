/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/typed/Client.h"
#include "ai/openai/codex/typed/Events.h"
#include "component/codex/CodexBackendTestSupport.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    namespace codex = ai::openai::codex;
    namespace typed = ai::openai::codex::typed;

    struct NotificationCase {
        const char* method;
        codex::Json params;
        const char* requiredField;
    };

    std::array<NotificationCase, 8> cases() {
        return {{{"deprecationNotice", {{"details", nullptr}, {"summary", "synthetic"}}, "summary"},
                 {"process/exited",
                  {{"exitCode", 0},
                   {"processHandle", "synthetic-handle"},
                   {"stderr", ""},
                   {"stderrCapReached", false},
                   {"stdout", ""},
                   {"stdoutCapReached", false}},
                  "processHandle"},
                 {"process/outputDelta",
                  {{"capReached", false}, {"deltaBase64", "c3ludGhldGlj"}, {"processHandle", "synthetic-handle"}, {"stream", "stdout"}},
                  "deltaBase64"},
                 {"remoteControl/status/changed",
                  {{"environmentId", nullptr},
                   {"installationId", "synthetic-installation"},
                   {"serverName", "synthetic-server"},
                   {"status", "connected"}},
                  "status"},
                 {"serverRequest/resolved", {{"requestId", "synthetic-request"}, {"threadId", "synthetic-thread"}}, "requestId"},
                 {"warning", {{"message", "synthetic"}, {"threadId", nullptr}}, "message"},
                 {"windows/worldWritableWarning",
                  {{"extraCount", 0}, {"failedScan", false}, {"samplePaths", codex::Json::array()}},
                  "samplePaths"},
                 {"windowsSandbox/setupCompleted", {{"error", nullptr}, {"mode", "elevated"}, {"success", true}}, "success"}}};
    }

    class Runner {
    public:
        explicit Runner(tests::support::TestResult& result)
            : result(result)
            , transport(std::make_shared<tests::codex::FakeTransportState>()) {
        }

        void start() {
            tests::codex::installInitializingFake(
                transport, [this](const codex::Json& message, const codex::detail::TransportCallbacks& callbacks) {
                    if (message.value("method", "") == "windowsSandbox/readiness") {
                        tests::codex::inject(callbacks, {{"id", message.at("id")}, {"result", {{"status", "ready"}}}});
                    }
                });
            client = std::make_unique<tests::codex::FakeAppServerClient>(transport);
            client->typed().events().setOnEvent([this](const typed::Event& event) {
                const std::string marker = std::visit(
                    [](const auto& value) {
                        return value.raw.at("params").at("fixtureCase").template get<std::string>();
                    },
                    event);
                order.emplace_back("typed:" + marker);
                ++typedCallbacks;

                if (marker == "warning-valid" && !reentrantSubmitted) {
                    reentrantSubmitted = true;
                    const auto submission = client->typed().windowsSandbox().checkReadiness(
                        [this](const typed::WindowsSandbox::CheckReadinessResult& operation) {
                            result.expectTrue(operation.kind == typed::WindowsSandbox::CheckReadinessResult::Kind::Success,
                                              "a runtime notification callback may submit an ordinary typed request");
                            ++operationCallbacks;
                            maybeFinish();
                        });
                    result.expectTrue(static_cast<bool>(submission), "reentrant runtime notification submission returns immediately");
                    throw std::runtime_error("synthetic runtime notification callback exception");
                }
                maybeFinish();
            });
            client->raw().setOnNotification([this](const codex::Notification& notification) {
                const std::string marker = notification.params.at("fixtureCase").get<std::string>();
                order.emplace_back("raw:" + marker);
                ++rawCallbacks;
                maybeFinish();
            });
            client->setOnStateChanged([this](const codex::StateChange& change) {
                if (change.current == codex::State::Ready && !injected) {
                    injected = true;
                    injectCases();
                } else if (change.current == codex::State::Stopped && stopping) {
                    core::EventReceiver::atNextTick([this]() {
                        client.reset();
                        finished = true;
                        core::SNodeC::stop();
                    });
                }
            });
            client->start();
        }

        [[nodiscard]] bool isFinished() const noexcept {
            return finished;
        }

        void verify() {
            result.expectEqual(
                std::size_t{16}, typedCallbacks, "all valid and malformed runtime/platform notifications reach the typed observer");
            result.expectEqual(
                std::size_t{16}, rawCallbacks, "all valid and malformed runtime/platform notifications reach the raw observer");
            result.expectEqual(
                std::size_t{1}, operationCallbacks, "the reentrant typed client request completes after a throwing notification callback");
            result.expectEqual(std::size_t{32}, order.size(), "every notification produces one typed/raw observer pair");
            for (std::size_t index = 0; index + 1 < order.size(); index += 2) {
                result.expectTrue(order[index].starts_with("typed:") && order[index + 1].starts_with("raw:") &&
                                      order[index].substr(6) == order[index + 1].substr(4),
                                  "runtime/platform notifications retain typed-before-raw order and raw-envelope identity");
            }
        }

    private:
        void injectCases() {
            for (const auto& item : cases()) {
                codex::Json complete = item.params;
                complete["fixtureCase"] = std::string(item.method) + "-valid";
                transport->inject({{"jsonrpc", "2.0"}, {"method", item.method}, {"params", complete}});

                codex::Json malformed = item.params;
                malformed.erase(item.requiredField);
                malformed["fixtureCase"] = std::string(item.method) + "-malformed";
                transport->inject({{"jsonrpc", "2.0"}, {"method", item.method}, {"params", malformed}});
            }
        }

        void maybeFinish() {
            if (stopping || typedCallbacks != 16 || rawCallbacks != 16 || operationCallbacks != 1) {
                return;
            }
            stopping = true;
            core::EventReceiver::atNextTick([this]() {
                client->stop();
            });
        }

        tests::support::TestResult& result;
        std::shared_ptr<tests::codex::FakeTransportState> transport;
        std::unique_ptr<tests::codex::FakeAppServerClient> client;
        std::vector<std::string> order;
        std::size_t typedCallbacks = 0;
        std::size_t rawCallbacks = 0;
        std::size_t operationCallbacks = 0;
        bool injected = false;
        bool reentrantSubmitted = false;
        bool stopping = false;
        bool finished = false;
    };

} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    core::SNodeC::init(argc, argv);

    bool timedOut = false;
    Runner runner(result);
    [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
        [&timedOut]() {
            timedOut = true;
            core::SNodeC::stop();
        },
        utils::Timeval({7, 0}));

    runner.start();
    const int loopResult = core::SNodeC::start(utils::Timeval({9, 0}));
    result.expectTrue(!timedOut && runner.isFinished(), "runtime/platform observer coverage completes without sleeps or polling");
    result.expectEqual(0, loopResult, "runtime/platform observer coverage stops the event loop cleanly");
    runner.verify();
    core::SNodeC::free();
    return result.processResult();
}
