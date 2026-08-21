/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/Api.h"
#include "ai/openai/codex/typed/Events.h"
#include "ai/openai/codex/typed/Mcp.h"
#include "component/codex/CodexBackendTestSupport.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
                    if (!expectedParams.contains(method)) {
                        return;
                    }

                    result.expectTrue(message.contains("id") && message["id"].is_number_integer(),
                                      method + " carries an allocated integer JSON-RPC ID");
                    result.expectTrue(message.value("params", codex::Json()) == expectedParams.at(method),
                                      method + " carries its exact encoded parameters");
                    if (message.contains("id") && message["id"].is_number_integer()) {
                        wireIds.push_back(message["id"].get<std::int64_t>());
                    }
                    tests::codex::inject(callbacks, {{"id", message.at("id")}, {"result", responseFor(method)}});

                    if (method == "mcpServer/oauth/login" && !notificationsInjected) {
                        notificationsInjected = true;
                        tests::codex::inject(
                            callbacks,
                            {{"method", "mcpServer/oauthLogin/completed"},
                             {"params",
                              {{"error", nullptr}, {"name", "synthetic-server"}, {"success", true}, {"threadId", "synthetic-thread"}}}});
                        tests::codex::inject(
                            callbacks,
                            {{"method", "mcpServer/startupStatus/updated"},
                             {"params",
                              {{"failureReason", nullptr}, {"name", "synthetic-server"}, {"status", "ready"}, {"threadId", nullptr}}}});
                    }
                });

            client = std::make_unique<tests::codex::FakeAppServerClient>(transport);
            client->events().setOnEvent([this](const typed::Event& event) {
                if (std::holds_alternative<typed::McpServerOauthLoginCompletedNotification>(event)) {
                    notificationOrder.emplace_back("typed-oauth");
                    submitReentrantList();
                    maybeFinish();
                    throw std::runtime_error("synthetic typed notification callback");
                }
                if (std::holds_alternative<typed::McpServerStatusUpdatedNotification>(event)) {
                    notificationOrder.emplace_back("typed-status");
                    maybeFinish();
                }
            });
            client->raw().setOnNotification([this](const codex::Notification& notification) {
                if (notification.method == "mcpServer/oauthLogin/completed") {
                    notificationOrder.emplace_back("raw-oauth");
                } else if (notification.method == "mcpServer/startupStatus/updated") {
                    notificationOrder.emplace_back("raw-status");
                }
                maybeFinish();
            });
            client->setOnStateChanged([this](const codex::StateChange& change) {
                if (change.current == codex::State::Ready && !submitted) {
                    submitted = true;
                    submitPrimaryOperations();
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
            result.expectEqual(
                std::size_t{5}, operationCallbacks, "the four primary MCP operations and one reentrant operation complete exactly once");
            result.expectTrue(notificationOrder == std::vector<std::string>{"typed-oauth", "raw-oauth", "typed-status", "raw-status"},
                              "typed MCP notifications are delivered before raw notifications even when the typed callback throws");
            result.expectEqual(std::size_t{5}, wireIds.size(), "all five MCP submissions retain one wire ID");
            if (wireIds.size() == 5) {
                result.expectTrue(wireIds == std::vector<std::int64_t>{1, 2, 3, 4, 5},
                                  "MCP operations reuse the single monotonic JSON-RPC request-ID allocator");
            }
        }

    private:
        static codex::Json responseFor(const std::string& method) {
            if (method == "mcpServer/oauth/login") {
                return {{"authorizationUrl", "https://example.invalid/oauth"}};
            }
            if (method == "mcpServer/resource/read") {
                return {{"contents", codex::Json::array({{{"text", "synthetic"}, {"uri", "file:///resource"}}})}};
            }
            if (method == "mcpServer/tool/call") {
                return {{"content", codex::Json::array({{{"type", "text"}, {"text", "synthetic"}}})}, {"isError", false}};
            }
            return {{"data", codex::Json::array()}, {"nextCursor", nullptr}};
        }

        template <typename Operation>
        void completed(const Operation& operation, std::string_view method) {
            result.expectTrue(!insideSubmission && operation.kind == Operation::Kind::Success && operation.value.has_value() &&
                                  operation.requestId.has_value() && operation.raw.is_null() &&
                                  operation.value->raw == responseFor(std::string(method)),
                              std::string(method) + " completes asynchronously with its own exact result and request ID");
            ++operationCallbacks;
            maybeFinish();
        }

        void submitPrimaryOperations() {
            typed::McpServerOauthLoginParams oauth{};
            oauth.name = "synthetic-server";
            oauth.threadId = typed::OptionalNullable<typed::ThreadId>::explicitNull();
            expectedParams["mcpServer/oauth/login"] = {{"name", "synthetic-server"}, {"threadId", nullptr}};

            typed::McpResourceReadParams read{};
            read.server = "synthetic-server";
            read.uri = "file:///resource";
            expectedParams["mcpServer/resource/read"] = {{"server", "synthetic-server"}, {"uri", "file:///resource"}};

            typed::McpServerToolCallParams call{};
            call.arguments = typed::OptionalNullable<codex::Json>::withValue({{"safe", true}});
            call.server = "synthetic-server";
            call.threadId = {"synthetic-thread"};
            call.tool = "tool-name";
            expectedParams["mcpServer/tool/call"] = {
                {"arguments", {{"safe", true}}},
                {"server", "synthetic-server"},
                {"threadId", "synthetic-thread"},
                {"tool", "tool-name"},
            };

            typed::ListMcpServerStatusParams list{};
            list.detail = typed::OptionalNullable<typed::McpServerStatusDetail>::withValue(typed::McpServerStatusDetail::full());
            list.limit = typed::OptionalNullable<std::uint32_t>::withValue(2);
            expectedParams["mcpServerStatus/list"] = {{"detail", "full"}, {"limit", 2}};

            insideSubmission = true;
            const auto oauthSubmission = client->mcp().startOauthLogin(
                std::move(oauth), [this](const typed::OperationResult<typed::McpServerOauthLoginResponse>& operation) {
                    completed(operation, "mcpServer/oauth/login");
                });
            const auto readSubmission = client->mcp().readResource(
                std::move(read), [this](const typed::OperationResult<typed::McpResourceReadResponse>& operation) {
                    completed(operation, "mcpServer/resource/read");
                });
            const auto callSubmission =
                client->mcp().callTool(std::move(call), [this](const typed::OperationResult<typed::McpServerToolCallResponse>& operation) {
                    completed(operation, "mcpServer/tool/call");
                });
            const auto listSubmission = client->mcp().listServers(
                std::move(list), [this](const typed::OperationResult<typed::ListMcpServerStatusResponse>& operation) {
                    completed(operation, "mcpServerStatus/list");
                });
            insideSubmission = false;

            result.expectTrue(oauthSubmission && readSubmission && callSubmission && listSubmission,
                              "all four typed MCP methods schedule work and return submissions immediately");
            result.expectTrue(oauthSubmission.id && readSubmission.id && callSubmission.id && listSubmission.id &&
                                  oauthSubmission.id->value() == 1 && readSubmission.id->value() == 2 && callSubmission.id->value() == 3 &&
                                  listSubmission.id->value() == 4,
                              "the four facade submissions expose their exact correlated request IDs");
        }

        void submitReentrantList() {
            if (reentrantSubmitted) {
                return;
            }
            reentrantSubmitted = true;
            typed::ListMcpServerStatusParams params{};
            params.detail = typed::OptionalNullable<typed::McpServerStatusDetail>::withValue(typed::McpServerStatusDetail::full());
            params.limit = typed::OptionalNullable<std::uint32_t>::withValue(2);
            const auto submission = client->mcp().listServers(
                std::move(params), [this](const typed::OperationResult<typed::ListMcpServerStatusResponse>& operation) {
                    completed(operation, "mcpServerStatus/list");
                });
            result.expectTrue(submission && submission.id && submission.id->value() == 5,
                              "an MCP notification callback may safely submit an ordinary typed client request");
        }

        void maybeFinish() {
            if (stopping || operationCallbacks != 5 || notificationOrder.size() != 4) {
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
        std::map<std::string, codex::Json> expectedParams;
        std::vector<std::int64_t> wireIds;
        std::vector<std::string> notificationOrder;
        std::size_t operationCallbacks = 0;
        bool submitted = false;
        bool insideSubmission = false;
        bool notificationsInjected = false;
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
    result.expectTrue(!timedOut && runner.isFinished(), "the asynchronous MCP wire scenario completes without polling or sleeps");
    result.expectEqual(0, eventLoopResult, "the MCP wire scenario stops the SNode.C EventLoop cleanly");
    runner.verifyFinal();
    core::SNodeC::free();
    return result.processResult();
}
