/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/Client.h"
#include "apps/codex-backend-client/ClientConnection.h"
#include "apps/codex-backend-client/Configuration.h"
#include "apps/codex-backend-client/FrontendWebSocketClient.h"
#include "support/TestResult.h"
#include "web/websocket/SubProtocolFactory.h"
#include "web/websocket/client/SubProtocol.h"
#include "web/websocket/client/SubProtocolFactorySelector.h"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace sdk = frontend::client;
    namespace app = apps::codex_backend_client;

    sdk::ClientOptions options() {
        sdk::ClientOptions result;
        result.credentialProvider = [] {
            return sdk::AuthenticationContext{frontend::NoCredential{}, "verified-local:test"};
        };
        return result;
    }

    bool contains(const std::string& source, const std::string_view token) {
        return source.find(token) != std::string::npos;
    }
} // namespace

int main() {
    tests::support::TestResult result;

    result.expectTrue(
        app::DEFAULT_MAXIMUM_FRAME_SIZE == frontend::DefaultFrontendMaximumServerMessageBytes
            && app::DEFAULT_MAXIMUM_SERVER_MESSAGE_BYTES == frontend::DefaultFrontendMaximumServerMessageBytes,
        "native and WebSocket reference clients derive their receive boundary from the frontend server contract");

    sdk::Client firstClient(options());
    std::size_t webSocketFailures = 0;
    auto firstBinding =
        std::make_shared<app::FrontendWebSocketClientBinding>(firstClient,
                                                              app::FrontendWebSocketClientCallbacks{.onConnected = {},
                                                                                                    .onDisconnected = {},
                                                                                                    .onFailure =
                                                                                                        [&webSocketFailures](std::string) {
                                                                                                            ++webSocketFailures;
                                                                                                        },
                                                                                                    .onBeforeTransportConnected = {},
                                                                                                    .onLocalShutdown = {}});
    sdk::Client secondClient(options());
    auto secondBinding = std::make_shared<app::FrontendWebSocketClientBinding>(secondClient);
    firstBinding->reportFailure("first");
    result.expectTrue(webSocketFailures == 1 && !firstBinding->connected() && !secondBinding->connected(),
                      "independent connection-owned WebSocket bindings coexist without global installation ownership");

    app::linkFrontendWebSocketClient();
    auto* selector = web::websocket::client::SubProtocolFactorySelector::instance();
    auto* factory = selector->select(
        "codex",
        web::websocket::SubProtocolFactorySelector<web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>>::Role::CLIENT);
    auto* repeatedFactory = selector->select(
        "codex",
        web::websocket::SubProtocolFactorySelector<web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>>::Role::CLIENT);
    result.expectTrue(factory != nullptr && factory == repeatedFactory && factory->getName() == "codex",
                      "the linked SNode.C client subprotocol factory is one stateless codex authority");
    if (factory != nullptr) {
        selector->unload(factory);
    }

    std::ifstream sourceFile(CODEX_BACKEND_CLIENT_MAIN_SOURCE);
    const std::string source{std::istreambuf_iterator<char>(sourceFile), std::istreambuf_iterator<char>()};
    std::ifstream webSocketSourceFile(CODEX_BACKEND_CLIENT_WEBSOCKET_SOURCE);
    const std::string webSocketSource{std::istreambuf_iterator<char>(webSocketSourceFile), std::istreambuf_iterator<char>()};
    const std::string_view requiredComposition[] = {
        "codex-backend-client-unix",
        "codex-backend-client-ipv4",
        "codex-backend-client-ipv6",
        "codex-backend-client-tls-ipv4",
        "codex-backend-client-tls-ipv6",
        "codex-backend-client-rfcomm",
        "codex-backend-client-rfcomm-tls",
        "codex-backend-client-websocket-ipv4",
        "codex-backend-client-websocket-ipv6",
        "codex-backend-client-wss-ipv4",
        "codex-backend-client-wss-ipv6",
        "setDisabled(true)",
        "preflightOutgoingTransports",
        "Instance::getDisabled()",
        "EventReceiver::atNextTick",
        "setMaximumWriteQueueBytes",
        "beginConnectionAttempt",
        "startPersistentStreamClient",
        "selectPersistentStreamClient",
        "selectPersistentWebSocketClient",
        "FrontendWebSocketHttpClient",
        "webSocketBinding",
        "configuredClient.connect",
        "configuredClient.getFlowController()->terminateFlow()",
        "connectionAttemptFailed",
        "lifecycle.disconnected()",
        "applicationShutdownActive()",
        "connectionHandle->shutdown()",
        "webSocketBindingHandle->shutdown()",
        "closeWebSocketUpgradeTransport",
        "getSocketContext()->close()",
        "Sec-WebSocket-Protocol\", \"codex",
    };
    bool complete = sourceFile.good() || sourceFile.eof();
    for (const std::string_view token : requiredComposition) {
        complete = contains(source, token) && complete;
    }
    result.expectTrue(
        complete, "the reference CLI composes every compiled named SNode.C client, preflights effective configuration, and requests codex");

    const std::size_t persistentStart = source.find("const auto startPersistentStreamClient");
    const std::size_t persistentEnd = source.find("#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)", persistentStart);
    const std::string persistentComposition = persistentStart != std::string::npos && persistentEnd != std::string::npos
                                                  ? source.substr(persistentStart, persistentEnd - persistentStart)
                                                  : std::string{};
    result.expectTrue(
        contains(persistentComposition, "configuredClient.connect") &&
            contains(persistentComposition, "configuredClient.getFlowController()->terminateFlow()") &&
            !contains(persistentComposition, "PhysicalConnectionAttemptGate") && !contains(persistentComposition, "prepareAttempt") &&
            !contains(persistentComposition, "std::make_shared") && !contains(persistentComposition, "copyEffectiveSocketConfiguration"),
        "native transports reconnect through the same configured SNode.C client without application attempt generations or config copies");
    result.expectTrue(!contains(source, "startStreamAttempt") && !contains(source, "connection.prepareAttempt("),
                      "active native composition no longer constructs or prepares per-attempt clients");

    const std::size_t webSocketStart = source.find("const auto selectPersistentWebSocketClient");
    const std::size_t webSocketEnd = source.find("#endif", webSocketStart);
    const std::string webSocketComposition = webSocketStart != std::string::npos && webSocketEnd != std::string::npos
                                                 ? source.substr(webSocketStart, webSocketEnd - webSocketStart)
                                                 : std::string{};
    result.expectTrue(contains(webSocketComposition, "startPersistentStreamClient(configuredClient") &&
                          contains(webSocketComposition, "configuredClient.getFlowController()->terminateFlow()") &&
                          !contains(webSocketComposition, "PhysicalConnectionAttemptGate") &&
                          !contains(webSocketComposition, "std::make_shared") &&
                          !contains(webSocketComposition, "copyEffectiveHttpConfiguration"),
                      "WebSocket transports reconnect through the same configured SNode.C client without attempt reconstruction");

    result.expectTrue(!contains(source, "FrontendWebSocketClientRuntime") && !contains(source, "PhysicalConnectionAttemptGate") &&
                          !contains(source, "activePhysicalClient") && !contains(source, "copyEffectiveHttpConfiguration") &&
                          !contains(source, "startWebSocketAttempt") && !contains(source, "prepareAttempt(") &&
                          !contains(source, "bindAttemptTransport"),
                      "active WebSocket composition has no global runtime, application attempt gate, or configuration copy");

    result.expectTrue(contains(webSocketSource, "dynamic_cast<FrontendWebSocketHttpSocketContext*>") &&
                          contains(webSocketSource, "connection->getSocketContext()") &&
                          contains(webSocketSource, "httpContext->getSocketConnection() != connection") &&
                          contains(webSocketSource, "static FrontendWebSocketClientFactory factory") &&
                          !contains(webSocketSource, "installedRuntime") && !contains(webSocketSource, "thread_local"),
                      "the stateless factory consumes only the exact connection-owned HTTP binding");

    result.expectTrue(!contains(source, "connectionHandle->disconnect()") && !contains(source, "webSocketBindingHandle->disconnect()") &&
                          !contains(source, "request->disconnect()") && !contains(source, "activeRequest->disconnect()"),
                      "application shutdown uses explicit intentional transport teardown rather than the ordinary disconnect path");

    std::size_t disconnectCallbackCount = 0;
    bool disconnectCallbacksAreNonterminal = true;
    std::size_t cursor = 0;
    while ((cursor = source.find(".onDisconnected =", cursor)) != std::string::npos) {
        const std::size_t callbackEnd = source.find(".onFailure =", cursor);
        if (callbackEnd == std::string::npos) {
            disconnectCallbacksAreNonterminal = false;
            break;
        }
        const std::string_view callbackBlock(source.data() + cursor, callbackEnd - cursor);
        if (contains(std::string(callbackBlock), "core::SNodeC::stop()") &&
            !contains(std::string(callbackBlock), "applicationShutdownActive()")) {
            disconnectCallbacksAreNonterminal = false;
        }
        ++disconnectCallbackCount;
        cursor = callbackEnd;
    }
    result.expectTrue(disconnectCallbackCount == 2 && disconnectCallbacksAreNonterminal,
                      "native and WebSocket physical disconnect callbacks stop the process only during intentional application shutdown");

    return result.processResult();
}
