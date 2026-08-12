/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/Client.h"
#include "apps/codex-backend-client/ClientAuthentication.h"
#include "support/TestResult.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace sdk = ai::openai::codex::frontend::client;
    namespace app = apps::codex_backend_client;

    std::string readFile(const char* path) {
        std::ifstream stream(path);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

    std::size_t countOccurrences(std::string_view value, std::string_view token) {
        std::size_t result = 0;
        for (std::size_t offset = 0; (offset = value.find(token, offset)) != std::string_view::npos; offset += token.size()) {
            ++result;
        }
        return result;
    }

    bool provideThrows(app::ClientAuthentication& authentication, std::optional<frontend::BearerCredential> credential = std::nullopt) {
        try {
            (void) authentication.provide(std::move(credential), "verified-local:test");
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    }
} // namespace

int main() {
    tests::support::TestResult result;

    app::ClientAuthentication authentication;
    result.expectTrue(provideThrows(authentication), "authentication fails closed when no physical transport prepared it");

    authentication.prepare(true);
    const sdk::AuthenticationContext local = authentication.provide(std::nullopt, "verified-local:1000");
    result.expectTrue(std::holds_alternative<frontend::NoCredential>(local.credential) &&
                          local.continuityKey == std::optional<std::string>{"verified-local:1000"} && provideThrows(authentication),
                      "one verified local Unix selection permits exactly one credential-free authentication context");

    authentication.prepare(false);
    result.expectTrue(provideThrows(authentication), "a remote transport without a bearer credential fails closed");

    authentication.prepare(false);
    const sdk::AuthenticationContext remote = authentication.provide(frontend::BearerCredential{"BEARER_SENTINEL"}, "unused-local-key");
    const auto* remoteBearer = std::get_if<frontend::BearerCredential>(&remote.credential);
    result.expectTrue(remoteBearer != nullptr && remoteBearer->token == "BEARER_SENTINEL" &&
                          remote.continuityKey == std::optional<std::string>{"bearer-profile:configured"},
                      "a prepared remote transport receives only the configured bearer profile context");

    app::ClientAuthentication containedAuthentication;
    sdk::ClientOptions options;
    options.credentialProvider = [&containedAuthentication] {
        return containedAuthentication.provide(std::nullopt, "verified-local:test");
    };
    std::vector<sdk::OutboundMessage> outbound;
    std::size_t closes = 0;
    sdk::Client client(std::move(options));
    sdk::Connection connection = client.openConnection({[&outbound](sdk::OutboundMessage message) {
                                                            outbound.push_back(std::move(message));
                                                            return sdk::SendResult{sdk::SendStatus::Accepted, std::nullopt};
                                                        },
                                                        [&closes](std::string) {
                                                            ++closes;
                                                        }});
    containedAuthentication.prepare(false);
    connection.transportConnected();
    result.expectTrue(outbound.empty() && closes == 1 && !connection.isTransportConnected() &&
                          client.connectionState() == sdk::ConnectionState::Disconnected,
                      "the SDK contains remote credential-provider failure, emits no Hello, and closes only that connection");

    const std::string mainSource = readFile(CODEX_BACKEND_CLIENT_MAIN_SOURCE);
    const std::string connectionSource = readFile(CODEX_BACKEND_CLIENT_CONNECTION_SOURCE);
    const std::string webSocketSource = readFile(CODEX_BACKEND_CLIENT_WEBSOCKET_SOURCE);
    const std::string_view remoteSelections[] = {
        "\"IPv4 JSONL\", &ipv4ConnectionHandle, false",
        "\"IPv6 JSONL\", &ipv6ConnectionHandle, false",
        "\"IPv4 TLS JSONL\", &tlsIpv4ConnectionHandle, false",
        "\"IPv6 TLS JSONL\", &tlsIpv6ConnectionHandle, false",
        "\"RFCOMM JSONL\", &rfcommConnectionHandle, false",
        "\"RFCOMM TLS JSONL\", &rfcommTlsConnectionHandle, false",
    };
    bool explicitTrust = mainSource.find("\"Unix JSONL\", &unixConnectionHandle, true") != std::string::npos;
    for (const std::string_view selection : remoteSelections) {
        explicitTrust = explicitTrust && mainSource.find(selection) != std::string::npos;
    }
    result.expectTrue(explicitTrust && mainSource.find("authentication.provide(") != std::string::npos,
                      "the reference CLI marks only Unix as verified local and prepares every remote JSONL family as remote");

    const std::size_t streamPrepare = connectionSource.find("callbacks.onBeforeTransportConnected(callbacks.verifiedLocalUnix)");
    const std::size_t streamConnect = connectionSource.find("protocolConnection.transportConnected()");
    const std::size_t webSocketPrepare = webSocketSource.find("binding->callbacks.onBeforeTransportConnected(false)");
    const std::size_t webSocketConnect = webSocketSource.find("protocolConnection.transportConnected()");
    result.expectTrue(streamPrepare != std::string::npos && streamConnect != std::string::npos && streamPrepare < streamConnect &&
                          webSocketPrepare != std::string::npos && webSocketConnect != std::string::npos &&
                          webSocketPrepare < webSocketConnect,
                      "stream and WebSocket adapters select transport authentication immediately before SDK transportConnected");

    result.expectTrue(countOccurrences(mainSource, "tlsIpv4Client.getConfig()->Instance::setDisabled(true);") == 1,
                      "the IPv4 TLS client has exactly one native disabled-default configuration statement");

    return result.processResult();
}
