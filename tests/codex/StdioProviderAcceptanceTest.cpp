/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CommunicationTrace.h"
#include "TestHarness.h"
#include "ai/openai/codex/bridge/CodexBridge.h"
#include "ai/openai/codex/protocol/generated/ProtocolTypes.h"
#include "ai/openai/codex/provider/StdioAppServer.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "utils/Timeval.h"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace {
    namespace codex = ai::openai::codex;
    namespace bridge = codex::bridge;
    namespace provider = codex::provider;
    namespace v2 = codex::generated::v2;

    struct Scenario {
        explicit Scenario(tests::codex::TestHarness& test)
            : test(test) {
        }

        void fail(std::string reason) {
            test.expect(false, reason);
            failed = true;
            core::SNodeC::stop();
        }

        void completeIfReady() {
            if (completed || !responseReceived || !notificationReceived) {
                return;
            }
            completed = true;
            tests::codex::traceCommunication("Provider stdio", "scenario", "local", "complete");
            core::EventReceiver::atNextTick([] {
                core::SNodeC::stop();
            });
        }

        tests::codex::TestHarness& test;
        bool responseReceived = false;
        bool notificationReceived = false;
        bool connected = false;
        bool disconnected = false;
        bool completed = false;
        bool failed = false;
        bool timedOut = false;
        std::size_t rawOutbound = 0;
        std::size_t rawInbound = 0;
    };
} // namespace

int main(int argc, char* argv[]) {
    tests::codex::TestHarness test;
    if (argc != 2) {
        test.expect(false, "expected the fake stdio app-server executable path");
        return test.result();
    }

    core::SNodeC::init(1, argv);
    Scenario scenario(test);
    int eventLoopResult = 1;
    {
        bridge::CodexBridge router;
        router.onRawJson([&scenario](codex::protocol::AppServerDirection direction, const nlohmann::json& message) {
            tests::codex::traceCommunication("Provider stdio",
                                              "backend-sdk",
                                              direction == codex::protocol::AppServerDirection::ToAppServer ? "bridge-to-provider"
                                                                                                             : "provider-to-bridge",
                                              "raw-app-server-json",
                                              message);
            direction == codex::protocol::AppServerDirection::ToAppServer ? ++scenario.rawOutbound : ++scenario.rawInbound;
        });
        router.onThreadNameUpdated([&scenario](v2::ThreadNameUpdatedNotification& notification) {
            tests::codex::traceCommunication(
                "Provider stdio", "typed-handler", "provider-to-bridge", "thread/name/updated", notification.getRaw());
            scenario.test.expect(notification.threadId() == std::optional<std::string>{"stdio-thread"} &&
                                     notification.threadName() == std::optional<std::string>{"stdio-provider"},
                                 "stdio provider notification reaches the generated typed handler");
            scenario.notificationReceived = true;
            scenario.completeIfReady();
        });
        router.onProviderLifecycle([&](bool connected) {
            tests::codex::traceCommunication("Provider stdio", "provider-endpoint", "lifecycle", connected ? "connected" : "disconnected");
            if (!connected) {
                scenario.disconnected = true;
                return;
            }
            scenario.connected = true;
            v2::ThreadListParams params({{"cursor", "stdio-request"}, {"limit", 2}});
            router.threadList(params, [&scenario](v2::ThreadListResponse& response) {
                tests::codex::traceCommunication(
                    "Provider stdio", "typed-callback", "provider-to-bridge", "thread/list", response.getRaw());
                scenario.test.expect(response && response.nextCursor() == std::optional<std::string>{"stdio-response"} &&
                                         response.getRaw().at("result").value("wire", std::string{}) == "stdio-jsonl",
                                     "stdio provider response completes the generated typed callback");
                scenario.responseReceived = true;
                scenario.completeIfReady();
            });
        });

        provider::StdioAppServerOptions options;
        options.executable = argv[1];
        options.arguments.clear();
        options.maximumFrameBytes = 256U * 1024U;
        options.maximumQueuedInputBytes = 256U * 1024U;
        options.onExit = [&scenario](int status) {
            if (!scenario.completed) {
                scenario.fail("fake stdio app-server exited unexpectedly with status " + std::to_string(status));
            }
        };
        provider::StdioAppServer endpoint(router, std::move(options));
        test.expect(endpoint.start() && endpoint.pid() > 0, "production stdio provider spawns its owned protocol peer");
        tests::codex::traceCommunication("Provider stdio", "child-process", "lifecycle", "spawned", {{"pid", endpoint.pid()}});

        [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
            [&scenario] {
                scenario.timedOut = true;
                scenario.fail("stdio provider acceptance timed out");
            },
            utils::Timeval({5, 0}));
        eventLoopResult = core::SNodeC::start(utils::Timeval({7, 0}));
        endpoint.stop();
        tests::codex::traceCommunication("Provider stdio", "child-process", "lifecycle", "stopped");
    }

    test.expect(eventLoopResult == 0 && scenario.completed && !scenario.failed && !scenario.timedOut,
                "stdio provider completes its bounded production lifecycle");
    test.expect(scenario.connected && scenario.disconnected && scenario.rawOutbound == 1 && scenario.rawInbound == 2,
                "stdio provider exposes one request, one response, one notification, and disconnect lifecycle");
    return test.result();
}
