/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CommunicationTrace.h"
#include "TestHarness.h"
#include "ai/openai/codex/frontend/CodexBridge.h"
#include "ai/openai/codex/frontend/client/ClientConnection.h"
#include "ai/openai/codex/protocol/generated/ProtocolTypes.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
    namespace codex = ai::openai::codex;

    class Endpoint final : public codex::frontend::client::TransportEndpoint {
    public:
        bool send(const nlohmann::json& message) override {
            tests::codex::traceCommunication("FrontendSdk", "transport-endpoint", "client-to-bridge", "send", message);
            sent.push_back(message);
            return acceptWrites;
        }

        void close(std::string_view reason) noexcept override {
            tests::codex::traceCommunication("FrontendSdk", "transport-endpoint", "local", "close", {{"reason", reason}});
            ++closeCount;
            closeReason = reason;
        }

        std::vector<nlohmann::json> sent;
        std::string closeReason;
        std::size_t closeCount = 0;
        bool acceptWrites = true;
    };
} // namespace

int main() {
    namespace clientRequests = ai::openai::codex::generated::client_requests;
    namespace v2 = ai::openai::codex::generated::v2;
    namespace frontend = ai::openai::codex::frontend;
    namespace client = frontend::client;

    tests::codex::TestHarness test;
    std::size_t connected = 0;
    std::size_t disconnected = 0;
    std::size_t failures = 0;
    std::size_t rawOutbound = 0;
    std::size_t rawInbound = 0;
    std::size_t bridgeEvents = 0;
    std::optional<v2::ThreadListResponse> typedResponse;

    frontend::CodexBridge sdk({});
    sdk.onRawJson([&](auto direction, const nlohmann::json& message) {
        tests::codex::traceCommunication("FrontendSdk",
                                          "frontend-sdk",
                                          direction == ai::openai::codex::protocol::AppServerDirection::ToAppServer ? "to-app-server"
                                                                                                                     : "from-app-server",
                                          "raw-json",
                                          message);
        if (direction == ai::openai::codex::protocol::AppServerDirection::ToAppServer) {
            ++rawOutbound;
        } else {
            ++rawInbound;
        }
    });
    sdk.onBridgeEvent([&](const nlohmann::json& event) {
        tests::codex::traceCommunication("FrontendSdk", "frontend-sdk", "from-bridge", "telemetry", event);
        ++bridgeEvents;
    });
    client::ClientConnection connection(
        sdk,
        {.onConnected =
             [&] {
                 tests::codex::traceCommunication("FrontendSdk", "client-connection", "lifecycle", "connected");
                 ++connected;
             },
         .onDisconnected =
             [&] {
                 tests::codex::traceCommunication("FrontendSdk", "client-connection", "lifecycle", "disconnected");
                 ++disconnected;
             },
         .onFailure =
             [&](std::string reason) {
                 tests::codex::traceCommunication("FrontendSdk", "client-connection", "lifecycle", "failure", {{"reason", reason}});
                 ++failures;
             }});
    Endpoint endpoint;

    test.expect(connection.attach(endpoint), "the production ClientConnection accepts its first transport endpoint");
    connection.connected(endpoint);
    test.expect(connection.online() && connected == 1, "transport connection enters the online callback once");

    connection.receive(
        endpoint, {{"kind", "bridge.connection"}, {"event", "opened"}, {"connectionId", "frontend-7"}, {"role", "observer"}, {"seq", 1}});
    connection.receive(endpoint, {{"kind", "bridge.controller"}, {"controllerConnectionId", "frontend-7"}, {"seq", 2}});
    connection.receive(endpoint,
                       {{"kind", "bridge.provider"},
                        {"state", "ready"},
                        {"providerGeneration", 1},
                        {"seq", 3}});
    test.expect(sdk.connectionId() == std::optional<std::string>{"frontend-7"} && sdk.isController(),
                "bridge telemetry establishes identity and controller role");

    const v2::ThreadListParams params({{"limit", 3}, {"archived", false}});
    const std::string requestId = sdk.threadList(params, [&](v2::ThreadListResponse& response) {
        tests::codex::traceCommunication("FrontendSdk", "typed-callback", "from-app-server", "thread/list", response.getRaw());
        typedResponse = response;
    });
    test.expect(endpoint.sent.size() == 1 && endpoint.sent.front().at("kind") == "appserver",
                "typed SDK command uses the production bridge envelope sender");
    test.expect(endpoint.sent.front().at("payload").at("method") == clientRequests::ThreadList::method &&
                    endpoint.sent.front().at("payload").at("params") == params.getRaw(),
                "typed parameters are represented by the unchanged app-server JSON payload");
    test.expectEqual(endpoint.sent.front().at("payload").at("id").get<std::string>(),
                     requestId,
                     "the typed callback is correlated by the emitted JSON-RPC id");

    const nlohmann::json responsePayload{
        {"jsonrpc", "2.0"}, {"id", requestId}, {"result", {{"data", nlohmann::json::array()}, {"nextCursor", nullptr}}}};
    connection.receive(
        endpoint,
        {{"kind", "appserver"}, {"connectionId", "frontend-7"}, {"role", "controller"}, {"seq", 3}, {"payload", responsePayload}});
    test.expect(typedResponse && typedResponse->getRaw() == responsePayload && typedResponse->data().items().empty(),
                "typed callback and getRaw expose the same complete app-server response");
    test.expect(rawOutbound == 1 && rawInbound == 1, "raw observation reports both SDK directions exactly once");
    test.expect(bridgeEvents == 3, "non-app-server telemetry remains separately observable");

    std::optional<v2::ThreadStartResponse> threadStartResponse;
    const v2::ThreadStartParams startParams({{"cwd", "/tmp"}});
    const std::string startRequestId = sdk.threadStart(startParams, [&](v2::ThreadStartResponse& response) {
        threadStartResponse = response;
    });
    const nlohmann::json appServerResponseWithoutVersion{
        {"id", startRequestId}, {"result", {{"thread", {{"id", "thread-from-real-envelope"}}}}}};
    connection.receive(endpoint,
                       {{"kind", "appserver"},
                        {"connectionId", "frontend-7"},
                        {"role", "controller"},
                        {"seq", 4},
                        {"payload", appServerResponseWithoutVersion}});
    test.expect(threadStartResponse && threadStartResponse->isJsonRpcResponse() &&
                    threadStartResponse->thread().id() == std::optional<std::string>{"thread-from-real-envelope"} &&
                    threadStartResponse->getRaw() == appServerResponseWithoutVersion,
                "typed responses unwrap the real app-server id/result envelope without requiring jsonrpc");

    const nlohmann::json appServerNotificationWithoutVersion{
        {"method", "thread/started"}, {"params", {{"thread", {{"id", "thread-from-real-notification"}}}}}};
    const v2::ThreadStartedNotification notification(appServerNotificationWithoutVersion);
    test.expect(notification.thread().id() == std::optional<std::string>{"thread-from-real-notification"} &&
                    notification.getRaw() == appServerNotificationWithoutVersion,
                "typed notifications unwrap the real app-server method/params envelope without requiring jsonrpc");

    std::optional<v2::ThreadListResponse> disconnectedResponse;
    std::size_t throwingDisconnectCallbacks = 0;
    sdk.threadList(params, [&](v2::ThreadListResponse& response) {
        tests::codex::traceCommunication("FrontendSdk", "typed-callback", "local", "transport-error", response.getRaw());
        disconnectedResponse = response;
    });
    sdk.threadList(params, [&](v2::ThreadListResponse&) {
        ++throwingDisconnectCallbacks;
        throw nlohmann::json::type_error::create(
            302, "disconnect callback failure", nullptr);
    });
    connection.detach(endpoint, "test transport loss");
    test.expect(disconnected == 1 && !connection.online() && !connection.attached(),
                "transport detach publishes one disconnected transition");
    test.expect(disconnectedResponse && disconnectedResponse->jsonRpcErrorCode() == std::optional<std::int64_t>{-32020},
                "transport loss deterministically retires a pending typed callback");
    test.expect(throwingDisconnectCallbacks == 1,
                "a throwing disconnect callback is contained and invoked exactly once");
    connection.detach(endpoint, "duplicate detach");
    test.expect(disconnected == 1, "duplicate detach does not repeat the disconnect callback");
    test.expect(failures == 0 && endpoint.closeCount == 0, "normal detach is not misclassified as failure or local close");

    connection.shutdown();
    return test.result();
}
