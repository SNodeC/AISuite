/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/FrontendService.h"
#include "apps/codex-backend/Configuration.h"
#include "support/TestResult.h"

#include <fstream>
#include <iterator>
#include <string>

namespace {

    std::string readFile(const char* path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

} // namespace

int main() {
    namespace app = apps::codex_backend;
    namespace frontend = ai::openai::codex::frontend;

    tests::support::TestResult result;
    result.expectTrue(app::isLoopbackFrontendAddress("127.0.0.1", false) && app::isLoopbackFrontendAddress("127.255.10.7", false) &&
                          !app::isLoopbackFrontendAddress("0.0.0.0", false) && !app::isLoopbackFrontendAddress("localhost", false),
                      "plaintext IPv4 policy accepts only numeric loopback addresses by default");
    result.expectTrue(app::isLoopbackFrontendAddress("::1", true) && !app::isLoopbackFrontendAddress("::", true) &&
                          !app::isLoopbackFrontendAddress("localhost", true),
                      "plaintext IPv6 policy accepts only the numeric loopback address by default");

    const frontend::FrontendServiceOptions serviceDefaults;
    result.expectTrue(serviceDefaults.maxConnections == 128 && serviceDefaults.maxUnauthenticatedConnections == 16 &&
                          serviceDefaults.maximumInboundMessageBytes == frontend::DefaultFrontendMaximumInboundMessageBytes &&
                          frontend::DefaultFrontendMaximumInboundMessageBytes == 8U * 1024U * 1024U &&
                          serviceDefaults.maxOutboundBytesPerConnection ==
                              frontend::DefaultFrontendMaximumProviderResponseBytes +
                                  frontend::DefaultFrontendMaximumReplayBytes,
                      "FrontendService retains its independent protocol resource limits");
    result.expectTrue(app::SocketFrontendOptions{}.maximumFrameSize == frontend::DefaultFrontendMaximumInboundMessageBytes &&
                          app::DEFAULT_MAXIMUM_OUTBOUND_BYTES ==
                              frontend::DefaultFrontendServiceMaxOutboundBytes +
                                  app::DEFAULT_TRANSPORT_FRAMING_HEADROOM_BYTES,
                      "native framing and the SNode.C writer queue are derived from the reusable frontend service bounds");

    const std::string configuration = readFile(CODEX_BACKEND_CONFIGURATION_SOURCE);
    const std::string main = readFile(CODEX_BACKEND_MAIN_SOURCE);
    const std::string streamContext = readFile(CODEX_BACKEND_STREAM_CONTEXT_SOURCE);
    const std::string unixCredentials = readFile(CODEX_BACKEND_UNIX_CREDENTIAL_SOURCE);
    result.expectTrue(!configuration.empty() && !main.empty() && !streamContext.empty() && !unixCredentials.empty(),
                      "the reference configuration and transport sources are readable");
    result.expectTrue(configuration.find("--frontend-bearer-token\"") == std::string::npos &&
                          configuration.find("--frontend-bearer-token-file") != std::string::npos,
                      "only the protected bearer-token file is an AISuite authentication option");

    static constexpr const char* RemovedListenerOptions[] = {
        "--frontend-unix-enabled",
        "--frontend-unix-path",
        "--frontend-ipv4-enabled",
        "--frontend-ipv4-address",
        "--frontend-ipv4-port",
        "--frontend-ipv6-enabled",
        "--frontend-tls-ipv4-enabled",
        "--frontend-tls-ipv4-certificate",
        "--frontend-rfcomm-enabled",
        "--frontend-rfcomm-address",
        "--frontend-rfcomm-channel",
        "--frontend-websocket-ipv4-enabled",
        "--frontend-wss-ipv4-enabled",
        "--frontend-wss-ipv4-certificate",
    };
    bool duplicateOptionsAbsent = true;
    for (const char* option : RemovedListenerOptions) {
        duplicateOptionsAbsent = duplicateOptionsAbsent && configuration.find(option) == std::string::npos;
    }
    result.expectTrue(duplicateOptionsAbsent, "AISuite exposes no duplicate listener enable/address/port/TLS/RFCOMM options");
    result.expectTrue(main.find("setMaximumWriteQueueBytes(apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES)") != std::string::npos &&
                          main.find("FrontendTransportBindDeclaration") == std::string::npos &&
                          main.find("ListenerStartupBarrier") == std::string::npos,
                      "the canonical application configures the SNode.C writer policy without a transport registry or startup barrier");
    result.expectTrue(
        main.find("net::in::stream::legacy::Server<apps::codex_backend::FrontendStreamSocketContextFactory>") != std::string::npos &&
            main.find("net::in6::stream::legacy::Server<apps::codex_backend::FrontendStreamSocketContextFactory>") != std::string::npos &&
            main.find("apps::codex_backend::ipv4FrontendServer") == std::string::npos &&
            main.find("apps::codex_backend::ipv6FrontendServer") == std::string::npos &&
            main.find("apps::codex_backend::rfcommFrontendServer") == std::string::npos,
        "main constructs every native SNode.C listener explicitly instead of hiding instance configuration in wrappers");
    result.expectTrue(streamContext.find("trySendToPeer(frame) == core::socket::stream::QueueResult::Queued") != std::string::npos &&
                          streamContext.find("totalQueued") == std::string::npos && streamContext.find("totalSent") == std::string::npos,
                      "stream transport admission uses SNode.C QueueResult without duplicate queue arithmetic");
    result.expectTrue(unixCredentials.find("net::un::peerCredentials(") != std::string::npos &&
                          unixCredentials.find("SO_PEERCRED") == std::string::npos &&
                          unixCredentials.find("getpeereid") == std::string::npos,
                      "Unix transport facts come exclusively from the SNode.C peer-credentials API");

    return result.processResult();
}
