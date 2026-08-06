/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/Client.h"
#include "apps/codex-backend-client/FrontendWebSocketClient.h"
#include "support/TestResult.h"
#include "web/websocket/SubProtocolFactory.h"
#include "web/websocket/client/SubProtocol.h"
#include "web/websocket/client/SubProtocolFactorySelector.h"

#include <fstream>
#include <iterator>
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

    sdk::Client firstClient(options());
    sdk::Client secondClient(options());
    app::FrontendWebSocketClientRuntime firstRuntime(firstClient);
    app::FrontendWebSocketClientRuntime secondRuntime(secondClient);
    result.expectTrue(firstRuntime.install() && !secondRuntime.install(),
                      "the application-private WebSocket runtime bridge permits exactly one SDK owner");
    firstRuntime.uninstall();
    result.expectTrue(secondRuntime.install(), "the runtime bridge can be transferred after deterministic uninstall");
    secondRuntime.uninstall();

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
        "Sec-WebSocket-Protocol\", \"codex",
    };
    bool complete = sourceFile.good() || sourceFile.eof();
    for (const std::string_view token : requiredComposition) {
        complete = contains(source, token) && complete;
    }
    result.expectTrue(
        complete, "the reference CLI composes every compiled named SNode.C client, preflights effective configuration, and requests codex");

    return result.processResult();
}
