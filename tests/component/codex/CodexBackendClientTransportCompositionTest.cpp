/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/Client.h"
#include "apps/codex-backend-client/ClientConnection.h"
#include "apps/codex-backend-client/FrontendWebSocketClient.h"
#include "support/TestResult.h"
#include "web/websocket/SubProtocolFactory.h"
#include "web/websocket/client/SubProtocol.h"
#include "web/websocket/client/SubProtocolFactorySelector.h"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace apps::codex_backend_client {

    struct ClientConnectionAttemptTestAccess {
        static bool accepts(ClientConnection& connection, const PhysicalConnectionAttemptGate::Generation generation) {
            return connection.acceptsAttemptGeneration(generation);
        }
    };

    struct FrontendWebSocketClientRuntimeTestAccess {
        static std::uint64_t claim(FrontendWebSocketClientRuntime& runtime, const core::socket::stream::SocketConnection* transport) {
            return runtime.claimAttempt(transport);
        }
    };

} // namespace apps::codex_backend_client

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

    sdk::Client firstClient(options());
    sdk::Client secondClient(options());
    std::size_t webSocketFailures = 0;
    std::size_t webSocketAttemptFailures = 0;
    app::FrontendWebSocketClientRuntime firstRuntime(
        firstClient,
        app::FrontendWebSocketClientCallbacks{.onConnected = {},
                                              .onDisconnected = {},
                                              .onFailure =
                                                  [&webSocketFailures](std::string) {
                                                      ++webSocketFailures;
                                                  },
                                              .onAttemptConnected = {},
                                              .onAttemptDisconnected = {},
                                              .onAttemptFailure =
                                                  [&webSocketAttemptFailures](std::uint64_t, std::string) {
                                                      ++webSocketAttemptFailures;
                                                  },
                                              .onBeforeTransportConnected = {},
                                              .onLocalShutdown = {}});
    app::FrontendWebSocketClientRuntime secondRuntime(secondClient);
    result.expectTrue(firstRuntime.install() && !secondRuntime.install(),
                      "the application-private WebSocket runtime bridge permits exactly one SDK owner");
    firstRuntime.uninstall();
    result.expectTrue(secondRuntime.install(), "the runtime bridge can be transferred after deterministic uninstall");
    secondRuntime.uninstall();

    app::PhysicalConnectionAttemptGate attempts;
    const auto firstGeneration = attempts.begin();
    result.expectTrue(firstGeneration == 1 && attempts.active() && !attempts.begin() && !attempts.complete(2) &&
                          attempts.isCurrent(*firstGeneration),
                      "one physical attempt generation is authoritative until that exact generation detaches");
    result.expectTrue(attempts.complete(*firstGeneration) && !attempts.active() && attempts.begin() == 2,
                      "a completed physical attempt permits exactly one later monotonically increasing generation");

    app::ClientConnection nativeConnection(firstClient);
    result.expectTrue(nativeConnection.prepareAttempt(11) && !nativeConnection.prepareAttempt(12) && !nativeConnection.hasAttachment(11),
                      "a native adapter cannot prepare an overlapping physical attempt");
    nativeConnection.cancelPreparedAttempt(12);

    app::ClientConnection generationAwareNative(firstClient,
                                                app::ClientConnectionCallbacks{.onConnected = {},
                                                                               .onDisconnected = {},
                                                                               .onFailure = {},
                                                                               .onAttemptConnected =
                                                                                   [](std::uint64_t) {
                                                                                   },
                                                                               .onAttemptDisconnected = {},
                                                                               .onAttemptFailure = {},
                                                                               .onOutbound = {},
                                                                               .verifiedLocalUnix = false,
                                                                               .onBeforeTransportConnected = {},
                                                                               .onLocalShutdown = {}});
    result.expectTrue(generationAwareNative.prepareAttempt(31) &&
                          app::ClientConnectionAttemptTestAccess::accepts(generationAwareNative, 31) &&
                          !app::ClientConnectionAttemptTestAccess::accepts(generationAwareNative, 0) &&
                          !app::ClientConnectionAttemptTestAccess::accepts(generationAwareNative, 30),
                      "a generation-aware native factory accepts only its exactly prepared physical attempt");
    generationAwareNative.cancelPreparedAttempt(31);
    result.expectTrue(generationAwareNative.prepareAttempt(32) &&
                          !app::ClientConnectionAttemptTestAccess::accepts(generationAwareNative, 31) &&
                          app::ClientConnectionAttemptTestAccess::accepts(generationAwareNative, 32),
                      "a late native factory from the retired generation cannot consume the next prepared generation");
    generationAwareNative.cancelPreparedAttempt(32);
    result.expectTrue(!nativeConnection.prepareAttempt(12), "a stale native cancellation cannot release the current generation");
    nativeConnection.cancelPreparedAttempt(11);
    result.expectTrue(nativeConnection.prepareAttempt(12), "the exact cancelled native generation permits the next attempt");
    nativeConnection.cancelPreparedAttempt(12);

    alignas(void*) std::byte firstTransportStorage{};
    alignas(void*) std::byte secondTransportStorage{};
    const auto* const firstTransport = reinterpret_cast<const core::socket::stream::SocketConnection*>(&firstTransportStorage);
    const auto* const secondTransport = reinterpret_cast<const core::socket::stream::SocketConnection*>(&secondTransportStorage);
    result.expectTrue(firstRuntime.prepareAttempt(21) && firstRuntime.bindAttemptTransport(21, firstTransport) &&
                          !firstRuntime.bindAttemptTransport(21, secondTransport) && !firstRuntime.prepareAttempt(22),
                      "the WebSocket runtime rejects an overlapping prepared attempt");
    result.expectTrue(app::FrontendWebSocketClientRuntimeTestAccess::claim(firstRuntime, secondTransport) == 0 &&
                          app::FrontendWebSocketClientRuntimeTestAccess::claim(firstRuntime, firstTransport) == 21,
                      "a WebSocket subprotocol may claim only the exact HTTP transport bound to its generation");
    firstRuntime.reportAttemptFailure(20, "stale");
    firstRuntime.reportAttemptFailure(21, "current");
    result.expectTrue(webSocketFailures == 1 && webSocketAttemptFailures == 1,
                      "stale WebSocket failure callbacks cannot affect the current application attempt");
    firstRuntime.abandonAttempt(20);
    result.expectTrue(!firstRuntime.prepareAttempt(22), "a stale WebSocket detach cannot release the current generation");
    firstRuntime.abandonAttempt(21);
    result.expectTrue(firstRuntime.prepareAttempt(22) && firstRuntime.bindAttemptTransport(22, secondTransport) &&
                          app::FrontendWebSocketClientRuntimeTestAccess::claim(firstRuntime, firstTransport) == 0 &&
                          app::FrontendWebSocketClientRuntimeTestAccess::claim(firstRuntime, secondTransport) == 22,
                      "a late WebSocket factory from the retired transport cannot claim the next prepared generation");
    firstRuntime.abandonAttempt(22);

    app::linkFrontendWebSocketClient();
    auto* selector = web::websocket::client::SubProtocolFactorySelector::instance();
    auto* factory = selector->select(
        "codex",
        web::websocket::SubProtocolFactorySelector<web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>>::Role::CLIENT);
    result.expectTrue(factory != nullptr && factory->getName() == "codex",
                      "the linked SNode.C client subprotocol factory resolves the exact codex token");
    if (factory != nullptr) {
        selector->unload(factory);
    }

    std::ifstream sourceFile(CODEX_BACKEND_CLIENT_MAIN_SOURCE);
    const std::string source{std::istreambuf_iterator<char>(sourceFile), std::istreambuf_iterator<char>()};
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
        "setRetry(false)",
        "setReconnect(false)",
        "beginConnectionAttempt",
        "PhysicalConnectionAttemptGate",
        "physicalAttempts.active()",
        "physicalAttempts.isCurrent(generation)",
        "prepareAttempt(*generation)",
        "startStreamAttempt",
        "startWebSocketAttempt",
        "beginWebSocketUpgrade(*generation)",
        "endWebSocketHttp(*generation)",
        "bindAttemptTransport(generation, transport)",
        "std::make_shared<Attempt>",
        "connectionAttemptFailed",
        "lifecycle.disconnected()",
        "applicationShutdownActive()",
        "connectionHandle->shutdown()",
        "webSocketRuntimeHandle->shutdown()",
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

    result.expectTrue(!contains(source, "connectionHandle->disconnect()") && !contains(source, "webSocketRuntimeHandle->disconnect()") &&
                          !contains(source, "request->disconnect()") && !contains(source, "activeRequest->disconnect()"),
                      "application shutdown uses explicit intentional transport teardown rather than the ordinary disconnect path");
    result.expectTrue(!contains(source, "physicalAttempts.current()"),
                      "WebSocket HTTP callbacks use their captured originating generation instead of mutable current-attempt state");

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
