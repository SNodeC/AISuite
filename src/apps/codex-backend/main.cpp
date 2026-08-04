/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/FrontendService.h"
#include "ai/openai/codex/stdio/Client.h"
#include "apps/codex-backend/Configuration.h"
#include "apps/codex-backend/FrontendStreamSocketContextFactory.h"
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
#include "apps/codex-backend/FrontendRuntimeBridge.h"
#include "apps/codex-backend/FrontendWebApplication.h"
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
#include "net/rc/stream/legacy/SocketServer.h"
#include "net/rc/stream/tls/SocketServer.h"
#endif
#include "apps/codex-backend/ReferenceAuthentication.h"
#include "apps/codex-backend/UnixPeerCredentials.h"
#include "core/SNodeC.h"
#include "core/socket/State.h"
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
#include "express/legacy/in/WebApp.h"
#include "express/legacy/in6/WebApp.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "express/tls/in/WebApp.h"
#include "express/tls/in6/WebApp.h"
#endif
#include "web/http/ConfigHttpParser.h"
#include "web/http/ConfigWebSocket.h"
#include "web/http/server/ConfigHttpServer.h"
#endif
#include "net/in/SocketAddress.h"
#include "net/in/stream/legacy/SocketServer.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "net/in/stream/tls/SocketServer.h"
#endif
#include "net/in6/SocketAddress.h"
#include "net/in6/stream/legacy/SocketServer.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "net/in6/stream/tls/SocketServer.h"
#endif
#include "net/config/ConfigInstance.h"
#include "net/un/stream/legacy/SocketServer.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <variant>

namespace {

    using ai::openai::codex::frontend::FrontendTransportKind;

    template <typename Address>
    void reportListener(std::string_view name, const Address& address, const core::socket::State& state) {
        if (state == core::socket::State::OK) {
            std::cout << "codex-backend: frontend listener " << name << " bound at " << address.toString() << '\n';
        } else if (state == core::socket::State::DISABLED) {
            std::cout << "codex-backend: frontend listener " << name << " is disabled\n";
        } else {
            std::cerr << "codex-backend: frontend listener " << name << " failed: " << state.what() << '\n';
        }
    }

#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
    void configureWebPolicy(net::config::ConfigInstance* config, std::size_t maximumMessageBytes) {
        auto* http = config->getSubCommand<web::http::server::ConfigHttpServer>();
        http->setMaximumPendingRequests(1)->setAllowChunkedTransfer(false)->setAllowPipelining(false);
        http->getParserConfig()
            ->setMaximumStartLineBytes(8192)
            ->setMaximumHeaderLineBytes(8192)
            ->setMaximumHeaderBytes(65536)
            ->setMaximumHeaderFields(128)
            ->setMaximumBodyBytes(1);
        config->getSubCommand<web::http::ConfigWebSocket>()
            ->setMaximumFrameBytes(maximumMessageBytes)
            ->setMaximumMessageBytes(maximumMessageBytes)
            ->setMaximumFragments(4096);
    }

#endif

} // namespace

int main(int argc, char* argv[]) {
    apps::codex_backend::ProviderRecoveryConfiguration recoveryConfiguration;
    apps::codex_backend::ReferenceAuthenticationConfiguration authenticationConfiguration;
    apps::codex_backend::FrontendRuntimeConfiguration frontendRuntimeConfiguration;
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
    apps::codex_backend::FrontendWebConfiguration webConfiguration;
#endif
    core::SNodeC::init(argc, argv);

    int result = 1;
    {
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        const apps::codex_backend::FrontendWebOptions webOptions = webConfiguration.options();
        if (const std::optional<std::string> error = webOptions.validationError()) {
            std::cerr << "codex-backend: " << *error << '\n';
            core::SNodeC::free();
            return 1;
        }
#endif

        ai::openai::codex::frontend::FrontendServiceOptions frontendOptions;
        if (const std::optional<std::string> error = frontendRuntimeConfiguration.apply(frontendOptions)) {
            std::cerr << "codex-backend: " << *error << '\n';
            core::SNodeC::free();
            return 1;
        }
        const apps::codex_backend::SocketFrontendOptions streamBounds{
            .maximumFrameSize = frontendOptions.maximumInboundMessageBytes,
        };

        if (authenticationConfiguration.verifiedLocalTrustEnabled() && !apps::codex_backend::unixPeerCredentialsSupported() &&
            !authenticationConfiguration.insecureLocalTrustOverride()) {
            std::cerr << "codex-backend: WARNING: SNode.C reports Unix peer credentials unavailable; verified local trust is disabled and "
                         "bearer authentication is required\n";
        }

        std::optional<apps::codex_backend::ProtectedBearerToken> bearerToken;
        const std::string bearerTokenFile = authenticationConfiguration.bearerTokenFile();
        if (!bearerTokenFile.empty()) {
            apps::codex_backend::ProtectedTokenFileResult loadedToken = apps::codex_backend::loadProtectedBearerTokenFile(bearerTokenFile);
            if (auto* error = std::get_if<apps::codex_backend::ProtectedTokenFileError>(&loadedToken)) {
                std::cerr << "codex-backend: " << error->message << '\n';
                core::SNodeC::free();
                return 1;
            }
            bearerToken.emplace(std::move(std::get<apps::codex_backend::ProtectedBearerToken>(loadedToken)));
        }
        apps::codex_backend::ReferenceAuthenticator authenticator(std::move(bearerToken), authenticationConfiguration.options());

        frontendOptions.allowVerifiedLocalTrust = authenticationConfiguration.verifiedLocalTrustEnabled();
        frontendOptions.allowInsecureLocalTrust = authenticationConfiguration.insecureLocalTrustOverride();
        if (frontendOptions.allowVerifiedLocalTrust) {
            frontendOptions.trustedLocalUserId = static_cast<std::uint64_t>(::geteuid());
        }
        frontendOptions.authenticator = [&authenticator](const ai::openai::codex::frontend::FrontendPeerContext& peer,
                                                         const ai::openai::codex::frontend::AuthenticationCredential& credential) {
            return authenticator.authenticate(peer, credential);
        };
        if (frontendOptions.allowInsecureLocalTrust) {
            std::cerr << "codex-backend: WARNING: insecure Unix frontend local-trust override is enabled\n";
        }

        ai::openai::codex::backend::BackendCoreOptions backendOptions;
        backendOptions.recovery = recoveryConfiguration.options();
        ai::openai::codex::backend::BackendCore<ai::openai::codex::stdio::Client> backend(std::move(backendOptions));
        ai::openai::codex::frontend::FrontendService frontendService(backend, std::move(frontendOptions));
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        if (!apps::codex_backend::installFrontendRuntime(frontendService)) {
            std::cerr << "codex-backend: failed to install the frontend runtime bridge\n";
            core::SNodeC::free();
            return 1;
        }
#endif

        apps::codex_backend::FrontendStreamSocketContextFactoryOptions unixStreamOptions;
        unixStreamOptions.transport = FrontendTransportKind::Unix;
        unixStreamOptions.socket = streamBounds;
        unixStreamOptions.resolvePeer = [](core::socket::stream::SocketConnection& connection) {
            return apps::codex_backend::verifiedUnixPeerContextFromFileDescriptor(connection.getFd(),
                                                                                  static_cast<std::uint64_t>(::geteuid()));
        };
        auto unixServer = net::un::stream::legacy::Server<apps::codex_backend::FrontendStreamSocketContextFactory>(
            "codex-backend",
            [](net::un::stream::legacy::config::ConfigSocketServer* config) {
                config->Local::setSunPath(apps::codex_backend::defaultSocketPath());
                config->Connection::setMaximumWriteQueueBytes(apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
            },
            frontendService,
            std::move(unixStreamOptions));
        auto ipv4Server = net::in::stream::legacy::Server<apps::codex_backend::FrontendStreamSocketContextFactory>(
            "codex-backend-ipv4",
            [](net::in::stream::legacy::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Local::setHost("127.0.0.1");
                config->Connection::setMaximumWriteQueueBytes(apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
            },
            frontendService,
            apps::codex_backend::FrontendStreamSocketContextFactoryOptions{
                .transport = FrontendTransportKind::Ipv4, .socket = streamBounds, .resolvePeer = {}});
        auto ipv6Server = net::in6::stream::legacy::Server<apps::codex_backend::FrontendStreamSocketContextFactory>(
            "codex-backend-ipv6",
            [](net::in6::stream::legacy::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Local::setHost("::1");
                config->Connection::setMaximumWriteQueueBytes(apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
            },
            frontendService,
            apps::codex_backend::FrontendStreamSocketContextFactoryOptions{
                .transport = FrontendTransportKind::Ipv6, .socket = streamBounds, .resolvePeer = {}});
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        auto tlsIpv4Server = net::in::stream::tls::Server<apps::codex_backend::FrontendStreamSocketContextFactory>(
            "codex-backend-tls-ipv4",
            [](net::in::stream::tls::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Local::setHost("127.0.0.1");
                config->Connection::setMaximumWriteQueueBytes(apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
            },
            frontendService,
            apps::codex_backend::FrontendStreamSocketContextFactoryOptions{
                .transport = FrontendTransportKind::TcpTls, .socket = streamBounds, .resolvePeer = {}});
        auto tlsIpv6Server = net::in6::stream::tls::Server<apps::codex_backend::FrontendStreamSocketContextFactory>(
            "codex-backend-tls-ipv6",
            [](net::in6::stream::tls::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Local::setHost("::1");
                config->Connection::setMaximumWriteQueueBytes(apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
            },
            frontendService,
            apps::codex_backend::FrontendStreamSocketContextFactoryOptions{
                .transport = FrontendTransportKind::TcpTls, .socket = streamBounds, .resolvePeer = {}});
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        auto rfcommServer = net::rc::stream::legacy::Server<apps::codex_backend::FrontendStreamSocketContextFactory>(
            "codex-backend-rfcomm",
            [](net::rc::stream::legacy::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Connection::setMaximumWriteQueueBytes(apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
            },
            frontendService,
            apps::codex_backend::FrontendStreamSocketContextFactoryOptions{
                .transport = FrontendTransportKind::Rfcomm, .socket = streamBounds, .resolvePeer = {}});
        auto rfcommTlsServer = net::rc::stream::tls::Server<apps::codex_backend::FrontendStreamSocketContextFactory>(
            "codex-backend-rfcomm-tls",
            [](net::rc::stream::tls::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Connection::setMaximumWriteQueueBytes(apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
            },
            frontendService,
            apps::codex_backend::FrontendStreamSocketContextFactoryOptions{
                .transport = FrontendTransportKind::RfcommTls, .socket = streamBounds, .resolvePeer = {}});
#endif

#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        express::legacy::in::WebApp webSocketIpv4App("codex-backend-websocket-ipv4");
        express::legacy::in6::WebApp webSocketIpv6App("codex-backend-websocket-ipv6");
        webSocketIpv4App.getConfig()->Instance::setDisabled(true);
        webSocketIpv4App.getConfig()->Local::setHost("127.0.0.1");
        webSocketIpv4App.getConfig()->Connection::setMaximumWriteQueueBytes(apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
        configureWebPolicy(webSocketIpv4App.getConfig(), streamBounds.maximumFrameSize);
        webSocketIpv6App.getConfig()->Instance::setDisabled(true);
        webSocketIpv6App.getConfig()->Local::setHost("::1");
        webSocketIpv6App.getConfig()->Connection::setMaximumWriteQueueBytes(apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
        configureWebPolicy(webSocketIpv6App.getConfig(), streamBounds.maximumFrameSize);
        apps::codex_backend::FrontendWebApplication webSocketIpv4Application(frontendService,
                                                                             {.endpoint = webOptions.endpoint,
                                                                              .staticRoot = webOptions.staticRoot,
                                                                              .allowedOrigins = webOptions.allowedOrigins,
                                                                              .transport = FrontendTransportKind::WebSocket,
                                                                              .encrypted = false});
        apps::codex_backend::FrontendWebApplication webSocketIpv6Application(frontendService,
                                                                             {.endpoint = webOptions.endpoint,
                                                                              .staticRoot = webOptions.staticRoot,
                                                                              .allowedOrigins = webOptions.allowedOrigins,
                                                                              .transport = FrontendTransportKind::WebSocket,
                                                                              .encrypted = false});
        webSocketIpv4Application.configure(webSocketIpv4App);
        webSocketIpv6Application.configure(webSocketIpv6App);
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        express::tls::in::WebApp webSocketTlsIpv4App("codex-backend-wss-ipv4");
        express::tls::in6::WebApp webSocketTlsIpv6App("codex-backend-wss-ipv6");
        webSocketTlsIpv4App.getConfig()->Instance::setDisabled(true);
        webSocketTlsIpv4App.getConfig()->Local::setHost("127.0.0.1");
        webSocketTlsIpv4App.getConfig()->Connection::setMaximumWriteQueueBytes(apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
        configureWebPolicy(webSocketTlsIpv4App.getConfig(), streamBounds.maximumFrameSize);
        webSocketTlsIpv6App.getConfig()->Instance::setDisabled(true);
        webSocketTlsIpv6App.getConfig()->Local::setHost("::1");
        webSocketTlsIpv6App.getConfig()->Connection::setMaximumWriteQueueBytes(apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
        configureWebPolicy(webSocketTlsIpv6App.getConfig(), streamBounds.maximumFrameSize);
        apps::codex_backend::FrontendWebApplication webSocketTlsIpv4Application(frontendService,
                                                                                {.endpoint = webOptions.endpoint,
                                                                                 .staticRoot = webOptions.staticRoot,
                                                                                 .allowedOrigins = webOptions.allowedOrigins,
                                                                                 .transport = FrontendTransportKind::WebSocketTls,
                                                                                 .encrypted = true});
        apps::codex_backend::FrontendWebApplication webSocketTlsIpv6Application(frontendService,
                                                                                {.endpoint = webOptions.endpoint,
                                                                                 .staticRoot = webOptions.staticRoot,
                                                                                 .allowedOrigins = webOptions.allowedOrigins,
                                                                                 .transport = FrontendTransportKind::WebSocketTls,
                                                                                 .encrypted = true});
        webSocketTlsIpv4Application.configure(webSocketTlsIpv4App);
        webSocketTlsIpv6Application.configure(webSocketTlsIpv6App);
#endif
#endif

        unixServer.listen([](const net::un::SocketAddress& address, const core::socket::State& state) {
            if (state == core::socket::State::OK) {
                const std::string socketPath = address.getSunPath();
                const bool ownerOnly =
                    !socketPath.empty() && ::chmod(socketPath.c_str(), S_IRUSR | S_IWUSR) == 0 &&
                    apps::codex_backend::verifyUnixListenerPath(socketPath, static_cast<std::uint64_t>(::geteuid())).verified;
                if (!ownerOnly) {
                    std::cerr << "codex-backend: failed to secure the Unix frontend listener pathname\n";
                    core::SNodeC::stop();
                    return;
                }
            }
            reportListener("unix", address, state);
        });
        ipv4Server.listen([allowInsecure = authenticationConfiguration.allowInsecureRemote()](const net::in::SocketAddress& address,
                                                                                              const core::socket::State& state) {
            if (state == core::socket::State::OK && !allowInsecure &&
                !apps::codex_backend::isLoopbackFrontendAddress(address.getHost(), false)) {
                std::cerr << "codex-backend: rejected a non-loopback plaintext IPv4 frontend bind\n";
                core::SNodeC::stop();
                return;
            }
            reportListener("ipv4", address, state);
        });
        ipv6Server.listen([allowInsecure = authenticationConfiguration.allowInsecureRemote()](const net::in6::SocketAddress& address,
                                                                                              const core::socket::State& state) {
            if (state == core::socket::State::OK && !allowInsecure &&
                !apps::codex_backend::isLoopbackFrontendAddress(address.getHost(), true)) {
                std::cerr << "codex-backend: rejected a non-loopback plaintext IPv6 frontend bind\n";
                core::SNodeC::stop();
                return;
            }
            reportListener("ipv6", address, state);
        });
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        tlsIpv4Server.listen([](const net::in::SocketAddress& address, const core::socket::State& state) {
            reportListener("tls-ipv4", address, state);
        });
        tlsIpv6Server.listen([](const net::in6::SocketAddress& address, const core::socket::State& state) {
            reportListener("tls-ipv6", address, state);
        });
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        rfcommServer.listen([](const net::rc::SocketAddress& address, const core::socket::State& state) {
            reportListener("rfcomm", address, state);
        });
        rfcommTlsServer.listen([](const net::rc::SocketAddress& address, const core::socket::State& state) {
            reportListener("rfcomm-tls", address, state);
        });
#endif
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        webSocketIpv4App.listen([allowInsecure = authenticationConfiguration.allowInsecureRemote()](const net::in::SocketAddress& address,
                                                                                                    const core::socket::State& state) {
            if (state == core::socket::State::OK && !allowInsecure &&
                !apps::codex_backend::isLoopbackFrontendAddress(address.getHost(), false)) {
                std::cerr << "codex-backend: rejected a non-loopback plaintext WebSocket bind\n";
                core::SNodeC::stop();
                return;
            }
            reportListener("websocket-ipv4", address, state);
        });
        webSocketIpv6App.listen([allowInsecure = authenticationConfiguration.allowInsecureRemote()](const net::in6::SocketAddress& address,
                                                                                                    const core::socket::State& state) {
            if (state == core::socket::State::OK && !allowInsecure &&
                !apps::codex_backend::isLoopbackFrontendAddress(address.getHost(), true)) {
                std::cerr << "codex-backend: rejected a non-loopback plaintext WebSocket bind\n";
                core::SNodeC::stop();
                return;
            }
            reportListener("websocket-ipv6", address, state);
        });
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        webSocketTlsIpv4App.listen([](const net::in::SocketAddress& address, const core::socket::State& state) {
            reportListener("wss-ipv4", address, state);
        });
        webSocketTlsIpv6App.listen([](const net::in6::SocketAddress& address, const core::socket::State& state) {
            reportListener("wss-ipv6", address, state);
        });
#endif
#endif

        backend.start();
        result = core::SNodeC::start();
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        apps::codex_backend::uninstallFrontendRuntime(frontendService);
#endif
        frontendService.close("codex-backend is stopping");
    }

    core::SNodeC::free();
    return result;
}
