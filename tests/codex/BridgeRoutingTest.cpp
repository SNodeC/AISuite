/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CommunicationTrace.h"
#include "TestHarness.h"
#include "ai/openai/codex/bridge/CodexBridge.h"
#include "ai/openai/codex/bridge/Endpoint.h"

#include <algorithm>
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

    void projectAndSectionRouting(tests::codex::TestHarness& test) {
        using Json = nlohmann::json;
        for (const bool observersMayRead : {true, false}) {
            bridge::CodexBridgeOptions options;
            options.observersMayRead = observersMayRead;
            bridge::CodexBridge router(options);
            Provider provider;
            router.setAppServer(&provider);
            router.appServerConnected();
            router.setAppServerReady();
            Frontend controller("project-controller"), observer("project-observer");
            const auto controllerId = router.registerFrontend(controller);
            const auto observerId = router.registerFrontend(observer);
            const std::vector<std::pair<std::string, Json>> reads{
                {"project/list", {{"cursor", "next-page"}, {"limit", 20}, {"sortKey", "position"}}},
                {"project/read", {{"projectId", "project-a"}}},
                {"threadSection/list", {{"cursor", "next-page"}, {"limit", 20}}},
                {"thread/list", {{"projectId", "project-a"}, {"sectionId", "section-a"}, {"cursor", "next-page"}}},
            };
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
            for (const char* method : {"project/create",
                                       "project/import",
                                       "project/update",
                                       "project/move",
                                       "project/delete",
                                       "threadSection/create",
                                       "threadSection/update",
                                       "threadSection/delete",
                                       "thread/metadata/update",
                                       "thread/section/move",
                                       "thread/start"}) {
                const auto before = provider.messages.size();
                const Json request{{"jsonrpc", "2.0"}, {"id", method}, {"method", method}, {"params", {{"projectId", "project-a"}}}};
                router.receiveFromFrontend(observerId, envelope(request));
                test.expect(provider.messages.size() == before && observer.lastPayload() && observer.lastPayload()->at("id") == method &&
                                observer.lastPayload()->contains("error") && observer.lastPayload()->at("error").value("code", 0) == -32001,
                            std::string(method) + " remains controller-only");
                router.receiveFromFrontend(controllerId, envelope(request));
                test.expect(provider.messages.size() == before + 1 && provider.messages.back().at("method") == method &&
                                provider.messages.back().at("params") == request.at("params"),
                            std::string(method) + " already uses generic controller forwarding");
                if (provider.messages.size() == before + 1)
                    router.receiveFromAppServer({{"id", provider.messages.back().at("id")}, {"result", Json::object()}});
            }
            for (const char* method : {"project/changed", "thread/project/updated"}) {
                const Json notification{{"method", method}, {"params", {{"projectId", "project-a"}, {"threadId", "thread-a"}}}};
                router.receiveFromAppServer(notification);
                test.expect(controller.lastPayload() && observer.lastPayload() && *controller.lastPayload() == notification &&
                                *observer.lastPayload() == notification,
                            std::string(method) + " is broadcast unchanged to both roles");
            }
            const auto before = provider.messages.size();
            router.receiveFromFrontend(observerId, envelope({{"jsonrpc", "2.0"}, {"method", "project/read"}, {"params", Json::object()}}));
            router.receiveFromFrontend(observerId, envelope({{"jsonrpc", "2.0"}, {"id", 99}, {"method", "project/read/extra"}}));
            test.expect(provider.messages.size() == before,
                        "observers cannot send read-method notifications or bypass exact method matching");
        }
    }
} // namespace

int main() {
    tests::codex::TestHarness test;
    projectAndSectionRouting(test);
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
