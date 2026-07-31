/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/typed/Client.h"
#include "ai/openai/codex/typed/WindowsSandbox.h"
#include "component/codex/CodexBackendTestSupport.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    namespace codex = ai::openai::codex;
    namespace typed = ai::openai::codex::typed;

    class WireRunner {
    public:
        explicit WireRunner(tests::support::TestResult& result)
            : result(result)
            , transport(std::make_shared<tests::codex::FakeTransportState>()) {
        }

        void start() {
            tests::codex::installInitializingFake(
                transport, [this](const codex::Json& message, const codex::detail::TransportCallbacks& callbacks) {
                    const std::string method = message.value("method", "");
                    if (method != "windowsSandbox/readiness" && method != "windowsSandbox/setupStart") {
                        return;
                    }

                    result.expectTrue(message.contains("id") && message["id"].is_number_integer(),
                                      method + " carries an allocated integer JSON-RPC ID");
                    if (message.contains("id") && message["id"].is_number_integer()) {
                        wireIds.push_back(message["id"].get<std::int64_t>());
                    }

                    if (method == "windowsSandbox/readiness") {
                        result.expectTrue(message.value("params", codex::Json::object()).is_null(),
                                          "windowsSandbox/readiness encodes Unit without application construction");
                        tests::codex::inject(callbacks, {{"id", message.at("id")}, {"result", {{"status", "ready"}}}});
                        return;
                    }

                    const codex::Json expected =
                        setupRequests++ == 0 ? codex::Json{{"cwd", nullptr}, {"mode", "elevated"}} : codex::Json{{"mode", "unelevated"}};
                    result.expectTrue(message.value("params", codex::Json()) == expected,
                                      "windowsSandbox/setupStart carries exact typed omitted/null/value parameters");
                    tests::codex::inject(callbacks, {{"id", message.at("id")}, {"result", {{"started", true}}}});
                });

            client = std::make_unique<tests::codex::FakeAppServerClient>(transport);
            client->setOnStateChanged([this](const codex::StateChange& change) {
                if (change.current == codex::State::Ready && !submitted) {
                    submitted = true;
                    submitPrimary();
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

        void verifyFinal() {
            result.expectEqual(std::size_t{3},
                               operationCallbacks,
                               "two primary WindowsSandbox operations and one reentrant operation complete exactly once");
            result.expectTrue(wireIds == std::vector<std::int64_t>{1, 2, 3},
                              "WindowsSandbox operations reuse the one monotonic JSON-RPC request-ID allocator");
        }

    private:
        template <typename Operation>
        void completed(const Operation& operation, std::string_view method) {
            result.expectTrue(!insideSubmission && operation.kind == Operation::Kind::Success && operation.value.has_value() &&
                                  operation.requestId.has_value(),
                              std::string(method) + " completes asynchronously with its correlated typed result");
            ++operationCallbacks;
            maybeFinish();
        }

        void submitPrimary() {
            typed::WindowsSandboxSetupStartParams params{};
            params.cwd = typed::OptionalNullable<typed::AbsolutePathBuf>::explicitNull();
            params.mode = typed::WindowsSandboxSetupMode::elevated();

            insideSubmission = true;
            const auto readiness =
                client->typed().windowsSandbox().checkReadiness([this](const typed::WindowsSandbox::CheckReadinessResult& operation) {
                    completed(operation, "windowsSandbox/readiness");
                    submitReentrant();
                    throw std::runtime_error("synthetic WindowsSandbox completion callback");
                });
            const auto setup = client->typed().windowsSandbox().startSetup(
                std::move(params), [this](const typed::WindowsSandbox::StartSetupResult& operation) {
                    completed(operation, "windowsSandbox/setupStart");
                });
            insideSubmission = false;

            result.expectTrue(readiness && setup && readiness.id && setup.id && readiness.id->value() == 1 && setup.id->value() == 2,
                              "WindowsSandbox facade methods schedule work and return exact submissions immediately");
        }

        void submitReentrant() {
            if (reentrantSubmitted) {
                return;
            }
            reentrantSubmitted = true;
            typed::WindowsSandboxSetupStartParams params{};
            params.cwd = typed::OptionalNullable<typed::AbsolutePathBuf>::omitted();
            params.mode = typed::WindowsSandboxSetupMode::unelevated();
            const auto submission = client->typed().windowsSandbox().startSetup(
                std::move(params), [this](const typed::WindowsSandbox::StartSetupResult& operation) {
                    completed(operation, "windowsSandbox/setupStart");
                });
            result.expectTrue(submission && submission.id && submission.id->value() == 3,
                              "a WindowsSandbox completion callback may submit another typed operation reentrantly");
        }

        void maybeFinish() {
            if (stopping || operationCallbacks != 3) {
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
        std::vector<std::int64_t> wireIds;
        std::size_t setupRequests = 0;
        std::size_t operationCallbacks = 0;
        bool submitted = false;
        bool insideSubmission = false;
        bool reentrantSubmitted = false;
        bool stopping = false;
        bool finished = false;
    };

} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    core::SNodeC::init(argc, argv);

    bool timedOut = false;
    WireRunner runner(result);
    [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
        [&timedOut]() {
            timedOut = true;
            core::SNodeC::stop();
        },
        utils::Timeval({7, 0}));

    runner.start();
    const int eventLoopResult = core::SNodeC::start(utils::Timeval({9, 0}));
    result.expectTrue(!timedOut && runner.isFinished(),
                      "the asynchronous WindowsSandbox wire scenario completes without polling or sleeps");
    result.expectEqual(0, eventLoopResult, "the WindowsSandbox wire scenario stops the SNode.C EventLoop cleanly");
    runner.verifyFinal();
    core::SNodeC::free();
    return result.processResult();
}
