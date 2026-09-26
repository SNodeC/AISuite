/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CommunicationTrace.h"
#include "TestHarness.h"
#include "ai/openai/codex/bridge/CodexBridge.h"
#include "ai/openai/codex/bridge/Endpoint.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    namespace bridge = ai::openai::codex::bridge;

    class Frontend final : public bridge::FrontendEndpoint {
    public:
        explicit Frontend(std::string label)
            : label(std::move(label)) {
        }

        bool send(const nlohmann::json& message) override {
            tests::codex::traceCommunication("BridgeRouting", label, "bridge-to-frontend", "send", message);
            messages.push_back(message);
            return acceptWrites;
        }

        void close(std::string_view reason) override {
            tests::codex::traceCommunication("BridgeRouting", label, "bridge-to-frontend", "close", {{"reason", reason}});
            ++closeCount;
            closeReason = reason;
        }

        const nlohmann::json* lastPayload() const {
            const auto iterator = std::find_if(messages.rbegin(), messages.rend(), [](const nlohmann::json& message) {
                return message.value("kind", std::string{}) == "appserver" && message.contains("payload");
            });
            return iterator == messages.rend() ? nullptr : &iterator->at("payload");
        }

        std::vector<nlohmann::json> messages;
        std::string closeReason;
        std::size_t closeCount = 0;
        bool acceptWrites = true;
        std::string label;
    };

    class Provider final : public bridge::AppServerEndpoint {
    public:
        bool send(const nlohmann::json& message) override {
            tests::codex::traceCommunication("BridgeRouting", "provider", "bridge-to-provider", "send", message);
            messages.push_back(message);
            return acceptWrites;
        }

        bool isConnected() const noexcept override {
            return connected;
        }

        std::vector<nlohmann::json> messages;
        bool connected = true;
        bool acceptWrites = true;
    };

    nlohmann::json envelope(nlohmann::json payload) {
        return {{"kind", "appserver"}, {"payload", std::move(payload)}};
    }

    void protocolRouting(tests::codex::TestHarness& test) {
        using Json = nlohmann::json;
        namespace generated = ai::openai::codex::generated;
#define REQUEST_METHOD(operation, name) generated::client_requests::operation::method,
        constexpr std::array requests{AI_OPENAI_CODEX_CLIENT_REQUESTS(REQUEST_METHOD)};
#undef REQUEST_METHOD
        // A protocol update requires reviewing new operations, not silently granting access.
        static_assert(requests.size() == 159);
#define NOTIFICATION_METHOD(operation, name) generated::server_notifications::operation::method,
        constexpr std::array notifications{AI_OPENAI_CODEX_SERVER_NOTIFICATIONS(NOTIFICATION_METHOD)};
#undef NOTIFICATION_METHOD
#define SERVER_REQUEST_METHOD(operation, name) generated::server_requests::operation::method,
        constexpr std::array serverRequests{AI_OPENAI_CODEX_SERVER_REQUESTS(SERVER_REQUEST_METHOD)};
#undef SERVER_REQUEST_METHOD
        // Independent permission expectations with representative opaque routing payloads.
        // These fixtures test routing, not app-server parameter/result validation.
        const std::vector<std::pair<std::string, Json>> reads{
            {"account/bedrock/discover", Json::object()},
            {"account/read", {{"refreshToken", false}}},
            {"account/rateLimits/read", Json::object()},
            {"account/usage/read", Json::object()},
            {"account/workspaceMessages/read", Json::object()},
            {"app/installed", Json::object()},
            {"app/list", {{"cursor", "apps-page"}}},
            {"app/read", {{"id", "app-a"}}},
            {"collaborationMode/list", Json::object()},
            {"config/read", {{"includeLayers", true}}},
            {"configRequirements/read", Json::object()},
            {"environment/status", {{"environmentId", "environment-a"}}},
            {"experimentalFeature/list", {{"cursor", "features-page"}}},
            {"externalAgentConfig/import/readHistories", Json::object()},
            {"fs/getMetadata", {{"path", "/workspace/file"}}},
            {"fs/readDirectory", {{"path", "/workspace"}}},
            {"fs/readFile", {{"path", "/workspace/file"}}},
            {"hooks/list", {{"cwds", Json::array({"/workspace"})}}},
            {"mcpServer/resource/read", {{"server", "server-a"}, {"uri", "resource://a"}}},
            {"mcpServerStatus/list", {{"cursor", "mcp-page"}}},
            {"model/list", {{"includeHidden", true}}},
            {"modelProvider/capabilities/read", {{"modelProvider", "provider-a"}, {"model", "model-a"}}},
            {"permissionProfile/list", {{"cwd", "/workspace"}}},
            {"plugin/installed", Json::object()},
            {"plugin/list", {{"cwds", Json::array({"/workspace"})}}},
            {"plugin/read", {{"pluginId", "plugin-a"}}},
            {"plugin/search", {{"searchTerm", "review"}, {"scope", "workspace"}, {"cursor", "plugins-page"}}},
            {"plugin/share/list", Json::object()},
            {"plugin/skill/read", {{"skillName", "skill-a"}}},
            {"project/list", {{"cursor", "next-page"}, {"limit", 20}, {"sortKey", "position"}}},
            {"project/read", {{"projectId", "project-a"}}},
            {"remoteControl/client/list", {{"environmentId", "environment-a"}, {"cursor", "clients-page"}}},
            {"remoteControl/status/read", Json::object()},
            {"server/diagnostics", Json::object()},
            {"skills/list", {{"cwds", Json::array({"/workspace"})}}},
            {"thread/backgroundTerminals/list", {{"threadId", "thread-a"}, {"cursor", "terminals-page"}}},
            {"thread/goal/get", {{"threadId", "thread-a"}}},
            {"thread/list", {{"projectId", "project-a"}, {"sectionId", "section-a"}, {"cursor", "next-page"}}},
            {"thread/loaded/list", {{"limit", 20}}},
            {"thread/items/list", {{"threadId", "thread-a"}, {"turnId", "turn-a"}, {"cursor", "items-page"}}},
            {"thread/queue/list", {{"threadId", "thread-a"}, {"cursor", "0"}, {"limit", 20}}},
            {"thread/read", {{"threadId", "thread-a"}, {"includeTurns", false}}},
            {"thread/realtime/listVoices", Json::object()},
            {"thread/search", {{"searchTerm", "needle"}, {"archived", true}, {"sortKey", "recency_at"}}},
            {"thread/searchOccurrences", {{"threadId", "thread-a"}, {"searchTerm", "needle"}, {"cursor", "occurrences-page"}}},
            {"thread/timeline/list", {{"threadId", "thread-a"}, {"cursor", "timeline-page"}}},
            {"thread/turns/list", {{"threadId", "thread-a"}, {"itemsView", "summary"}, {"cursor", "turns-page"}}},
            {"threadSection/list", {{"cursor", "next-page"}, {"limit", 20}}},
            {"userVerification/status", Json::object()},
            {"windowsSandbox/readiness", Json::object()},
        };
        test.expect(reads.size() == 50, "34 established reads and 16 newly reviewed reads are covered");
        for (const auto& [method, params] : reads) {
            static_cast<void>(params);
            test.expect(std::ranges::find(requests, method) != requests.end(), method + " exists in the generated request catalog");
            test.expect(std::ranges::count_if(reads,
                                              [&](const auto& entry) {
                                                  return entry.first == method;
                                              }) == 1,
                        method + " has one permission expectation");
        }
        for (const bool observersMayRead : {true, false}) {
            bridge::CodexBridgeOptions options;
            options.observersMayRead = observersMayRead;
            bridge::CodexBridge router(options);
            Provider provider;
            router.setAppServer(&provider);
            router.appServerConnected();
            router.setAppServerReady();
            Frontend controller("policy-controller"), observer("policy-observer");
            const auto controllerId = router.registerFrontend(controller);
            const auto observerId = router.registerFrontend(observer);
            for (const auto& [method, params] : reads) {
                const auto before = provider.messages.size();
                const Json request{{"jsonrpc", "2.0"}, {"id", 42}, {"method", method}, {"params", params}};
                router.receiveFromFrontend(observerId, envelope(request));
                test.expect(provider.messages.size() == before + (observersMayRead ? 1U : 0U), method + " obeys the observer-read switch");
                if (!observersMayRead) {
                    test.expect(observer.lastPayload() && observer.lastPayload()->value("id", 0) == 42 &&
                                    observer.lastPayload()->contains("error") &&
                                    observer.lastPayload()->at("error").value("code", 0) == -32001,
                                method + " rejects disabled observer reads with the original id");
                    continue;
                }
                if (provider.messages.size() != before + 1)
                    continue;
                const Json upstreamId = provider.messages.back().at("id");
                Json expected = request;
                expected["id"] = upstreamId;
                test.expect(upstreamId != 42 && provider.messages.back() == expected,
                            method + " preserves parameters and remaps only the request id");
                // Both clients can use the same frontend id concurrently.
                router.receiveFromFrontend(controllerId, envelope(request));
                test.expect(provider.messages.size() == before + 2 && provider.messages.back().at("id") != upstreamId,
                            method + " keeps controller/observer correlation distinct");
                if (provider.messages.size() != before + 2)
                    continue;
                const Json controllerUpstreamId = provider.messages.back().at("id");
                const auto controllerCount = controller.messages.size();
                const Json result{{"data", Json::array()}, {"nextCursor", "page-2"}};
                router.receiveFromAppServer({{"id", upstreamId}, {"result", result}});
                test.expect(observer.lastPayload() && *observer.lastPayload() == Json{{"id", 42}, {"result", result}} &&
                                controller.messages.size() == controllerCount,
                            method + " returns the unchanged result only to its observer owner");
                const auto observerCount = observer.messages.size();
                const Json error{{"code", -32601}, {"message", "unsupported by this server"}};
                router.receiveFromAppServer({{"id", controllerUpstreamId}, {"error", error}});
                test.expect(controller.lastPayload() && *controller.lastPayload() == Json{{"id", 42}, {"error", error}} &&
                                observer.messages.size() == observerCount,
                            method + " returns provider errors only to their controller owner");
            }
            for (const auto method : requests) {
                const std::string label(method);
                const bool read = std::ranges::any_of(reads, [&](const auto& entry) {
                    return entry.first == method;
                });
                const bool handshake = method == "initialize";
                const auto before = provider.messages.size();
                const Json request{
                    {"jsonrpc", "2.0"}, {"id", label}, {"method", label}, {"params", {{"opaque", Json::array({1, nullptr, "x"})}}}};
                if (!read) {
                    Json forged = envelope(request);
                    forged["role"] = "controller";
                    router.receiveFromFrontend(observerId, forged);
                    test.expect(provider.messages.size() == before && observer.lastPayload() && observer.lastPayload()->at("id") == label &&
                                    observer.lastPayload()->contains("error") &&
                                    observer.lastPayload()->at("error").value("code", 0) == (handshake ? -32003 : -32001),
                                label + " rejects observers even with a forged envelope role");
                }
                router.receiveFromFrontend(controllerId, envelope(request));
                if (handshake) {
                    test.expect(provider.messages.size() == before && controller.lastPayload() &&
                                    controller.lastPayload()->at("error").value("code", 0) == -32003,
                                "the provider handshake remains bridge-owned");
                } else {
                    test.expect(provider.messages.size() == before + 1,
                                label + " remains available to the controller with either read policy");
                    if (provider.messages.size() != before + 1)
                        continue;
                    Json expected = request;
                    expected["id"] = provider.messages.back().at("id");
                    test.expect(expected == provider.messages.back(), label + " forwards controller payloads without rewriting fields");
                    const auto observerCount = observer.messages.size();
                    router.receiveFromAppServer({{"id", expected.at("id")}, {"result", {{"opaque", true}}}});
                    test.expect(controller.lastPayload() &&
                                    *controller.lastPayload() == Json{{"id", label}, {"result", {{"opaque", true}}}} &&
                                    observer.messages.size() == observerCount,
                                label + " returns controller results only to their owner");
                }
                const auto beforeNotification = provider.messages.size();
                router.receiveFromFrontend(observerId, envelope({{"jsonrpc", "2.0"}, {"method", label}}));
                test.expect(provider.messages.size() == beforeNotification, label + " cannot be sent as an observer notification");
            }
            for (const auto method : notifications) {
                const Json notification{{"method", method}, {"params", {{"projectId", "project-a"}, {"threadId", "thread-a"}}}};
                router.receiveFromAppServer(notification);
                test.expect(controller.lastPayload() && observer.lastPayload() && *controller.lastPayload() == notification &&
                                *observer.lastPayload() == notification,
                            std::string(method) + " is broadcast unchanged to both roles");
            }
            for (const auto method : serverRequests) {
                const auto observerCount = observer.messages.size();
                const Json request{{"id", "server-request"}, {"method", method}, {"params", {{"threadId", "thread-a"}}}};
                router.receiveFromAppServer(request);
                test.expect(controller.lastPayload() && *controller.lastPayload() == request && observer.messages.size() == observerCount,
                            std::string(method) + " is delivered only to the controller");
                const auto before = provider.messages.size();
                const Json response{{"jsonrpc", "2.0"}, {"id", "server-request"}, {"result", {{"opaque", true}}}};
                router.receiveFromFrontend(observerId, envelope(response));
                test.expect(provider.messages.size() == before, std::string(method) + " cannot be answered by an observer");
                router.receiveFromFrontend(controllerId, envelope(response));
                test.expect(provider.messages.size() == before + 1 && provider.messages.back() == response,
                            std::string(method) + " forwards its controller response unchanged");
            }
            for (const auto& [method, params] : reads) {
                for (const std::string& altered : {method + "/extra", "prefix/" + method}) {
                    const auto before = provider.messages.size();
                    router.receiveFromFrontend(observerId,
                                               envelope({{"jsonrpc", "2.0"}, {"id", 99}, {"method", altered}, {"params", params}}));
                    test.expect(provider.messages.size() == before && observer.lastPayload() &&
                                    observer.lastPayload()->at("error").value("code", 0) == -32001,
                                altered + " cannot bypass exact method matching");
                }
            }
        }
    }

    void requestLifecycleRouting(tests::codex::TestHarness& test) {
        using Json = nlohmann::json;
        bridge::CodexBridge router;
        Provider provider;
        router.setAppServer(&provider);
        router.appServerConnected();
        Frontend controller("lifecycle-controller"), observer("lifecycle-observer");
        const auto controllerId = router.registerFrontend(controller);
        const auto observerId = router.registerFrontend(observer);
        const Json request{{"jsonrpc", "2.0"}, {"id", 42}, {"method", "thread/search"}, {"params", {{"searchTerm", "needle"}}}};
        const auto expectError = [&](const Frontend& frontend, int code, std::string_view label) {
            const Json* payload = frontend.lastPayload();
            test.expect(payload && payload->value("id", Json()) == 42 && payload->contains("error") &&
                            payload->at("error").value("code", 0) == code,
                        label);
        };
        router.receiveFromFrontend(observerId, envelope(request));
        expectError(observer, -32002, "new reads still require provider initialization");
        test.expect(provider.messages.empty(), "uninitialized reads do not reach the provider");
        router.setAppServerReady();

        provider.acceptWrites = false;
        router.receiveFromFrontend(observerId, envelope(request));
        expectError(observer, -32005, "provider write rejection reaches the observer");
        provider.acceptWrites = true;
        router.receiveFromFrontend(observerId, envelope(request));
        test.expect(provider.messages.size() == 2, "observer read retry reaches the provider after a rejected write");
        if (provider.messages.size() != 2)
            return;
        const Json upstreamId = provider.messages.back().at("id");
        test.expect(provider.messages.size() == 2 && upstreamId != 42,
                    "failed sends release ownership so the same request id can be retried");
        router.receiveFromFrontend(observerId, envelope(request));
        expectError(observer, -32600, "duplicate outstanding observer request ids are rejected");
        test.expect(provider.messages.size() == 2, "duplicate ids do not create additional upstream requests");

        Json stringIdRequest = request;
        stringIdRequest["id"] = "42";
        router.receiveFromFrontend(observerId, envelope(stringIdRequest));
        const Json stringUpstreamId = provider.messages.back().at("id");
        test.expect(provider.messages.size() == 3 && stringUpstreamId != upstreamId, "string and numeric frontend ids remain distinct");
        router.receiveFromAppServer({{"id", stringUpstreamId}, {"result", {{"value", "string-id"}}}});
        test.expect(observer.lastPayload() && observer.lastPayload()->at("id") == "42", "string request id type survives the round trip");

        router.receiveFromFrontend(controllerId,
                                   {{"kind", "bridge.controller"}, {"action", "transfer"}, {"targetConnectionId", observerId}});
        test.expect(router.controllerConnectionId() == observerId, "controller transfer remains available");
        const auto previousControllerCount = controller.messages.size();
        const Json hit{{"thread", {{"id", "thread-a"}}}, {"snippet", "needle"}};
        const Json result{{"data", Json::array({hit})}, {"nextCursor", nullptr}};
        router.receiveFromAppServer({{"id", upstreamId}, {"result", result}});
        test.expect(observer.lastPayload() && *observer.lastPayload() == Json{{"id", 42}, {"result", result}} &&
                        controller.messages.size() == previousControllerCount,
                    "role changes do not transfer ownership of outstanding read responses");
        router.receiveFromFrontend(controllerId, envelope(request));
        const Json disconnectedUpstreamId = provider.messages.back().at("id");
        router.appServerDisconnected("local test disconnect");
        expectError(controller, -32002, "provider disconnect fails an outstanding observer read with its original id");
        provider.connected = false;
        const auto beforeDisconnectedRead = provider.messages.size();
        router.receiveFromFrontend(controllerId, envelope(request));
        test.expect(provider.messages.size() == beforeDisconnectedRead, "disconnected reads do not reach the provider");
        provider.connected = true;
        router.appServerConnected();
        router.setAppServerReady();
        router.receiveFromFrontend(controllerId, envelope(request));
        const Json reconnectedUpstreamId = provider.messages.back().at("id");
        test.expect(reconnectedUpstreamId != disconnectedUpstreamId, "reconnect uses a distinct upstream request identity");
        const Json beforeStaleResponse = *controller.lastPayload();
        router.receiveFromAppServer({{"id", disconnectedUpstreamId}, {"result", {{"stale", true}}}});
        test.expect(controller.lastPayload() && *controller.lastPayload() == beforeStaleResponse,
                    "old-generation responses cannot answer a new read");
        router.receiveFromAppServer({{"id", reconnectedUpstreamId}, {"error", {{"code", -32601}, {"message", "method unavailable"}}}});
        expectError(controller, -32601, "older app-server unsupported-method errors remain unchanged");

        router.receiveFromFrontend(controllerId, envelope(request));
        const Json abandonedId = provider.messages.back().at("id");
        router.unregisterFrontend(controllerId);
        const Json beforeAbandonedResponse = *observer.lastPayload();
        router.receiveFromAppServer({{"id", abandonedId}, {"result", {{"abandoned", true}}}});
        test.expect(observer.lastPayload() && *observer.lastPayload() == beforeAbandonedResponse,
                    "a disconnected requester cannot leak its late response to another frontend");
        Frontend replacement("lifecycle-replacement");
        const auto replacementId = router.registerFrontend(replacement);
        const auto beforeUnknown = provider.messages.size();
        const Json unknown{{"jsonrpc", "2.0"}, {"id", 42}, {"method", "future/read"}, {"params", {{"opaque", true}}}};
        router.receiveFromFrontend(replacementId, envelope(unknown));
        expectError(replacement, -32001, "unknown observer methods remain default-deny");
        test.expect(provider.messages.size() == beforeUnknown, "unknown observer methods do not reach the provider");
        router.receiveFromFrontend(observerId, envelope(unknown));
        test.expect(provider.messages.size() == beforeUnknown + 1 && provider.messages.back().at("method") == "future/read",
                    "unknown controller methods remain forward-compatible");
        for (const auto& id : {observerId, replacementId}) {
            const auto before = provider.messages.size();
            router.receiveFromFrontend(id, envelope({{"jsonrpc", "2.0"}, {"method", "initialized"}}));
            test.expect(provider.messages.size() == before, "neither frontend role can send the bridge-owned initialized notification");
        }
    }
} // namespace

int main() {
    tests::codex::TestHarness test;
    protocolRouting(test);
    requestLifecycleRouting(test);
    bridge::CodexBridge router;
    Provider provider;
    router.setAppServer(&provider);
    router.appServerConnected();
    router.setAppServerReady();

    Frontend first("frontend-A");
    Frontend second("frontend-B");
    const std::string firstId = router.registerFrontend(first);
    const std::string secondId = router.registerFrontend(second);
    test.expect(router.frontendCount() == 2 && router.controllerConnectionId() == firstId,
                "first frontend is controller and second frontend is observer");

    router.receiveFromFrontend(secondId,
                               envelope({{"jsonrpc", "2.0"}, {"id", 10}, {"method", "thread/list"}, {"params", nlohmann::json::object()}}));
    test.expect(provider.messages.size() == 1 && provider.messages.back().at("method") == "thread/list",
                "observer read request is forwarded to the app-server");
    const nlohmann::json readUpstreamId = provider.messages.back().at("id");
    test.expect(readUpstreamId != 10, "frontend request id is remapped before provider forwarding");
    router.receiveFromAppServer({{"jsonrpc", "2.0"}, {"id", readUpstreamId}, {"result", {{"data", nlohmann::json::array()}}}});
    test.expect(second.lastPayload() != nullptr && second.lastPayload()->at("id") == 10,
                "provider response is routed only to its owner with the original id restored");

    for (const std::string_view method : {std::string_view{"thread/turns/list"}, std::string_view{"thread/items/list"}}) {
        const std::size_t beforePage = provider.messages.size();
        router.receiveFromFrontend(
            secondId,
            envelope({{"jsonrpc", "2.0"}, {"id", std::string(method)}, {"method", method}, {"params", {{"threadId", "thread-a"}}}}));
        test.expect(provider.messages.size() == beforePage + 1 && provider.messages.back().at("method") == method,
                    std::string("observer pagination forwards ") + std::string(method));
        if (provider.messages.size() == beforePage + 1) {
            const nlohmann::json upstreamId = provider.messages.back().at("id");
            router.receiveFromAppServer({{"jsonrpc", "2.0"}, {"id", upstreamId}, {"result", {{"data", nlohmann::json::array()}}}});
            test.expect(second.lastPayload() != nullptr && second.lastPayload()->at("id") == method,
                        std::string("observer pagination restores the frontend id for ") + std::string(method));
        }
    }

    const std::size_t providerCount = provider.messages.size();
    router.receiveFromFrontend(
        secondId, envelope({{"jsonrpc", "2.0"}, {"id", 11}, {"method", "thread/start"}, {"params", nlohmann::json::object()}}));
    test.expect(provider.messages.size() == providerCount && second.lastPayload() != nullptr &&
                    second.lastPayload()->at("error").at("code") == -32001,
                "observer mutation is rejected locally without provider traffic");

    router.receiveFromFrontend(firstId,
                               envelope({{"jsonrpc", "2.0"}, {"id", 12}, {"method", "initialize"}, {"params", nlohmann::json::object()}}));
    test.expect(provider.messages.size() == providerCount && first.lastPayload() != nullptr &&
                    first.lastPayload()->at("error").at("code") == -32003,
                "frontend initialize cannot compete with the bridge-owned provider handshake");

    router.receiveFromAppServer(
        {{"jsonrpc", "2.0"}, {"method", "item/started"}, {"params", {{"threadId", "thread-a"}, {"item", {{"id", "item-a"}}}}}});
    test.expect(first.lastPayload() != nullptr && second.lastPayload() != nullptr && first.lastPayload()->at("method") == "item/started" &&
                    second.lastPayload()->at("method") == "item/started",
                "provider notifications fan out unchanged to controller and observer");

    router.receiveFromAppServer({{"jsonrpc", "2.0"},
                                 {"id", "approval-1"},
                                 {"method", "item/commandExecution/requestApproval"},
                                 {"params", {{"threadId", "thread-a"}, {"turnId", "turn-a"}, {"itemId", "item-a"}}}});
    test.expect(first.lastPayload() != nullptr && first.lastPayload()->at("id") == "approval-1" &&
                    second.lastPayload()->at("method") == "item/started",
                "server request is delivered only to the active controller");
    router.receiveFromFrontend(secondId, envelope({{"jsonrpc", "2.0"}, {"id", "approval-1"}, {"result", {}}}));
    test.expect(provider.messages.back().at("id") != "approval-1" || !provider.messages.back().contains("result"),
                "an observer cannot answer the controller-owned server request");
    router.receiveFromFrontend(firstId, envelope({{"jsonrpc", "2.0"}, {"id", "approval-1"}, {"result", {{"decision", "accept"}}}}));
    test.expect(provider.messages.back().at("id") == "approval-1" && provider.messages.back().contains("result"),
                "the owning controller response reaches the provider unchanged");

    router.receiveFromFrontend(firstId, {{"kind", "bridge.controller"}, {"action", "release"}});
    test.expect(!router.controllerConnectionId(), "controller release leaves ownership vacant");
    router.unregisterFrontend(firstId);
    test.expect(!router.controllerConnectionId() && router.frontendCount() == 1, "disconnect never auto-promotes the remaining observer");
    router.receiveFromFrontend(secondId, {{"kind", "bridge.controller"}, {"action", "claim"}});
    test.expect(router.controllerConnectionId() == std::optional<std::string>{secondId},
                "remaining observer can acquire control explicitly");

    router.receiveFromAppServer({{"jsonrpc", "2.0"},
                                 {"id", "approval-2"},
                                 {"method", "item/commandExecution/requestApproval"},
                                 {"params", {{"threadId", "thread-b"}}}});
    const std::size_t messagesBeforeControllerLoss = provider.messages.size();
    router.unregisterFrontend(secondId);
    test.expect(provider.messages.size() == messagesBeforeControllerLoss + 1 && provider.messages.back().at("id") == "approval-2" &&
                    provider.messages.back().at("error").at("code") == -32011,
                "controller disconnect resolves its outstanding provider request with an explicit error");
    test.expect(router.frontendCount() == 0 && !router.controllerConnectionId(),
                "disconnect cleanup removes frontend and controller ownership");

    router.unregisterFrontend(secondId);
    test.expect(provider.messages.size() == messagesBeforeControllerLoss + 1,
                "duplicate frontend cleanup cannot fail the provider request twice");

    Frontend replacement("frontend-C");
    const std::string replacementId = router.registerFrontend(replacement);
    const std::size_t messagesBeforeStaleResponse = provider.messages.size();
    router.receiveFromFrontend(replacementId, envelope({{"jsonrpc", "2.0"}, {"id", "approval-2"}, {"result", {{"decision", "accept"}}}}));
    test.expect(provider.messages.size() == messagesBeforeStaleResponse && replacement.lastPayload() != nullptr &&
                    replacement.lastPayload()->at("error").at("code") == -32004,
                "a replacement controller cannot answer the retired server request");

    router.receiveFromAppServer({{"jsonrpc", "2.0"},
                                 {"id", "approval-2"},
                                 {"method", "item/commandExecution/requestApproval"},
                                 {"params", {{"threadId", "thread-c"}}}});
    test.expect(replacement.lastPayload() != nullptr && replacement.lastPayload()->at("method") == "item/commandExecution/requestApproval",
                "the provider may reuse a retired server-request id for a new request");
    router.receiveFromFrontend(replacementId, envelope({{"jsonrpc", "2.0"}, {"id", "approval-2"}, {"result", {{"decision", "decline"}}}}));
    test.expect(provider.messages.back().at("id") == "approval-2" && provider.messages.back().at("result").at("decision") == "decline",
                "the replacement controller owns only the newly delivered request");

    router.receiveFromAppServer({{"jsonrpc", "2.0"},
                                 {"id", "approval-3"},
                                 {"method", "item/commandExecution/requestApproval"},
                                 {"params", {{"threadId", "thread-d"}}}});
    const std::size_t messagesBeforeProviderResolution = provider.messages.size();
    router.receiveFromAppServer(
        {{"jsonrpc", "2.0"}, {"method", "serverRequest/resolved"}, {"params", {{"requestId", "approval-3"}, {"threadId", "thread-d"}}}});
    test.expect(replacement.lastPayload() != nullptr && replacement.lastPayload()->at("method") == "serverRequest/resolved",
                "provider-side server-request resolution is forwarded to the controller");
    router.receiveFromFrontend(replacementId, envelope({{"jsonrpc", "2.0"}, {"id", "approval-3"}, {"result", {{"decision", "accept"}}}}));
    test.expect(provider.messages.size() == messagesBeforeProviderResolution && replacement.lastPayload() != nullptr &&
                    replacement.lastPayload()->at("error").at("code") == -32004,
                "provider-side resolution retires frontend response ownership");

    router.receiveFromAppServer({{"jsonrpc", "2.0"},
                                 {"id", "approval-3"},
                                 {"method", "item/commandExecution/requestApproval"},
                                 {"params", {{"threadId", "thread-e"}}}});
    router.receiveFromFrontend(replacementId, envelope({{"jsonrpc", "2.0"}, {"id", "approval-3"}, {"result", {{"decision", "decline"}}}}));
    test.expect(provider.messages.back().at("id") == "approval-3" && provider.messages.back().at("result").at("decision") == "decline",
                "a provider-resolved request id can be reused without stale ownership");

    return test.result();
}
