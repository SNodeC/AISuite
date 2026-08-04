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
#include "apps/codex-backend/NativeIpFrontendServers.h"
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
#include "apps/codex-backend/NativeRfcommFrontendServers.h"
#endif
#include "apps/codex-backend/ReferenceAuthentication.h"
#include "apps/codex-backend/UnixPeerCredentials.h"
#include "core/SNodeC.h"
#include "net/un/stream/legacy/SocketServer.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <variant>

namespace {

    using ai::openai::codex::frontend::FrontendTransportKind;
    using apps::codex_backend::FrontendTransportBindDeclaration;

    class ListenerStartupBarrier {
    public:
        ListenerStartupBarrier(std::set<std::string> expected, std::function<void()> ready)
            : pending(std::move(expected))
            , ready(std::move(ready)) {
        }

        void bound(std::string_view listener) {
            if (failed || started) {
                return;
            }
            pending.erase(std::string(listener));
            if (pending.empty()) {
                started = true;
                ready();
            }
        }

        void bindingFailed(std::string_view listener) {
            if (started) {
                // A listener that disappears after startup changes topology,
                // but it does not replace or stop the single provider/backend.
                return;
            }
            if (!failed) {
                failed = true;
                std::cerr << "codex-backend: required frontend listener failed before startup: " << listener << '\n';
                core::SNodeC::stop();
            }
        }

        [[nodiscard]] bool startupFailed() const noexcept {
            return failed;
        }

    private:
        std::set<std::string> pending;
        std::function<void()> ready;
        bool failed = false;
        bool started = false;
    };

    template <typename Address>
    bool reportBoundListener(std::string_view listener,
                             const Address& address,
                             core::socket::State state,
                             FrontendTransportBindDeclaration& declaration,
                             ListenerStartupBarrier& barrier) {
        if (!declaration.report(state)) {
            std::cerr << "codex-backend: frontend listener " << listener << " is not bound at " << address.toString() << '\n';
            barrier.bindingFailed(listener);
            return false;
        }
        barrier.bound(listener);
        return true;
    }

    std::set<std::string> enabledListenerNames(const apps::codex_backend::NativeFrontendListenerOptions& options) {
        std::set<std::string> names;
        if (options.unixEnabled) {
            names.emplace("unix");
        }
        if (options.ipv4Enabled) {
            names.emplace("ipv4");
        }
        if (options.ipv6Enabled) {
            names.emplace("ipv6");
        }
        if (options.tlsIpv4Enabled) {
            names.emplace("tls-ipv4");
        }
        if (options.tlsIpv6Enabled) {
            names.emplace("tls-ipv6");
        }
        if (options.rfcommEnabled) {
            names.emplace("rfcomm");
        }
        if (options.rfcommTlsEnabled) {
            names.emplace("rfcomm-tls");
        }
        return names;
    }

} // namespace

int main(int argc, char* argv[]) {
    apps::codex_backend::ProviderRecoveryConfiguration recoveryConfiguration;
    apps::codex_backend::ReferenceAuthenticationConfiguration authenticationConfiguration;
    apps::codex_backend::FrontendRuntimeConfiguration frontendRuntimeConfiguration;
    apps::codex_backend::NativeFrontendConfiguration nativeFrontendConfiguration;
    core::SNodeC::init(argc, argv);

    int result = 1;
    {
        const apps::codex_backend::NativeFrontendListenerOptions nativeOptions = nativeFrontendConfiguration.options();
        if (const std::optional<std::string> error = nativeOptions.validationError()) {
            std::cerr << "codex-backend: " << *error << '\n';
            core::SNodeC::free();
            return 1;
        }
        if (nativeOptions.enabledListenerCount() == 0) {
            std::cerr << "codex-backend: at least one frontend listener must be enabled\n";
            core::SNodeC::free();
            return 1;
        }

        ai::openai::codex::frontend::FrontendServiceOptions frontendOptions;
        if (const std::optional<std::string> error = frontendRuntimeConfiguration.apply(frontendOptions)) {
            std::cerr << "codex-backend: " << *error << '\n';
            core::SNodeC::free();
            return 1;
        }
        const apps::codex_backend::SocketFrontendOptions streamBounds{.maximumFrameSize = frontendOptions.maximumInboundMessageBytes,
                                                                      .maximumOutboundBytes =
                                                                          apps::codex_backend::DEFAULT_MAXIMUM_OUTBOUND_BYTES};

        const std::string bearerTokenFile = authenticationConfiguration.bearerTokenFile();
        if (nativeOptions.unixEnabled && authenticationConfiguration.verifiedLocalTrustEnabled() &&
            !apps::codex_backend::unixPeerCredentialsSupported() && !authenticationConfiguration.insecureLocalTrustOverride()) {
            std::cerr << "codex-backend: WARNING: OS Unix peer credentials are unavailable; verified local trust is disabled and bearer "
                         "authentication is required\n";
        }
        if (apps::codex_backend::unixFrontendRequiresBearer(nativeOptions.unixEnabled,
                                                            authenticationConfiguration.verifiedLocalTrustEnabled(),
                                                            authenticationConfiguration.insecureLocalTrustOverride(),
                                                            apps::codex_backend::unixPeerCredentialsSupported()) &&
            bearerTokenFile.empty()) {
            std::cerr << "codex-backend: the Unix frontend requires a protected bearer-token file when verified local trust is unavailable "
                         "or disabled\n";
            core::SNodeC::free();
            return 1;
        }
        if (nativeOptions.remoteAuthenticationRequired() && bearerTokenFile.empty()) {
            std::cerr << "codex-backend: every enabled remote frontend listener requires a protected bearer-token file\n";
            core::SNodeC::free();
            return 1;
        }
        if (nativeOptions.allowInsecureRemote && (nativeOptions.ipv4Enabled || nativeOptions.ipv6Enabled)) {
            std::cerr << "codex-backend: WARNING: authenticated plaintext frontend binding outside loopback is explicitly enabled\n";
        }

        std::optional<apps::codex_backend::ProtectedBearerToken> bearerToken;
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
                // Preserve the established SNode.C --sun-path/config-file
                // option as the authoritative Unix listener path.
                config->Local::setSunPath(apps::codex_backend::defaultSocketPath());
            },
            frontendService,
            std::move(unixStreamOptions));
        auto ipv4Server = apps::codex_backend::ipv4FrontendServer("codex-backend-ipv4", {}, frontendService, streamBounds);
        auto ipv6Server = apps::codex_backend::ipv6FrontendServer("codex-backend-ipv6", {}, frontendService, streamBounds);
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        auto tlsIpv4Server = apps::codex_backend::ipv4TlsFrontendServer(
            "codex-backend-tls-ipv4",
            [&nativeOptions](net::in::stream::tls::config::ConfigSocketServer* config) {
                config->Tls::setCert(nativeOptions.tlsIpv4Certificate);
                config->Tls::setCertKey(nativeOptions.tlsIpv4PrivateKey);
            },
            frontendService,
            streamBounds);
        auto tlsIpv6Server = apps::codex_backend::ipv6TlsFrontendServer(
            "codex-backend-tls-ipv6",
            [&nativeOptions](net::in6::stream::tls::config::ConfigSocketServer* config) {
                config->Tls::setCert(nativeOptions.tlsIpv6Certificate);
                config->Tls::setCertKey(nativeOptions.tlsIpv6PrivateKey);
            },
            frontendService,
            streamBounds);
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        auto rfcommServer = apps::codex_backend::rfcommFrontendServer("codex-backend-rfcomm", {}, frontendService, streamBounds);
        auto rfcommTlsServer = apps::codex_backend::rfcommTlsFrontendServer(
            "codex-backend-rfcomm-tls",
            [&nativeOptions](net::rc::stream::tls::config::ConfigSocketServer* config) {
                config->Tls::setCert(nativeOptions.rfcommTlsCertificate);
                config->Tls::setCertKey(nativeOptions.rfcommTlsPrivateKey);
            },
            frontendService,
            streamBounds);
#endif

        // Every transport is selected by the application-level enable flags.
        // Merely constructing an SNode.C server must therefore not make its
        // configuration subcommand mandatory during the final bootstrap. The
        // enabled listener is still required by ListenerStartupBarrier once
        // listen() is requested below.
        unixServer.getConfig()->Instance::forceUnrequired();
        ipv4Server.getConfig()->Instance::forceUnrequired();
        ipv6Server.getConfig()->Instance::forceUnrequired();
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        tlsIpv4Server.getConfig()->Instance::forceUnrequired();
        tlsIpv6Server.getConfig()->Instance::forceUnrequired();
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        rfcommServer.getConfig()->Instance::forceUnrequired();
        rfcommTlsServer.getConfig()->Instance::forceUnrequired();
#endif

        FrontendTransportBindDeclaration unixDeclaration(frontendService, FrontendTransportKind::Unix);
        FrontendTransportBindDeclaration ipv4Declaration(frontendService, FrontendTransportKind::Ipv4);
        FrontendTransportBindDeclaration ipv6Declaration(frontendService, FrontendTransportKind::Ipv6);
        FrontendTransportBindDeclaration tlsIpv4Declaration(frontendService, FrontendTransportKind::TcpTls);
        FrontendTransportBindDeclaration tlsIpv6Declaration(frontendService, FrontendTransportKind::TcpTls);
        FrontendTransportBindDeclaration rfcommDeclaration(frontendService, FrontendTransportKind::Rfcomm);
        FrontendTransportBindDeclaration rfcommTlsDeclaration(frontendService, FrontendTransportKind::RfcommTls);
        ListenerStartupBarrier startupBarrier(enabledListenerNames(nativeOptions), [&backend] {
            backend.start();
        });

        if (nativeOptions.unixEnabled) {
            unixServer.listen([&startupBarrier, &unixDeclaration](const net::un::SocketAddress& address, core::socket::State state) {
                if (state != core::socket::State::OK) {
                    static_cast<void>(unixDeclaration.report(state));
                    startupBarrier.bindingFailed("unix");
                    return;
                }
                const std::string socketPath = address.getSunPath();
                const bool ownerOnly =
                    !socketPath.empty() && ::chmod(socketPath.c_str(), S_IRUSR | S_IWUSR) == 0 &&
                    apps::codex_backend::verifyUnixListenerPath(socketPath, static_cast<std::uint64_t>(::geteuid())).verified;
                if (!ownerOnly) {
                    std::cerr << "codex-backend: failed to secure the Unix frontend listener pathname\n";
                    startupBarrier.bindingFailed("unix");
                    return;
                }
                static_cast<void>(reportBoundListener("unix", address, state, unixDeclaration, startupBarrier));
            });
        }
        if (nativeOptions.ipv4Enabled) {
            ipv4Server.listen(
                nativeOptions.ipv4Address,
                nativeOptions.ipv4Port,
                [&nativeOptions, &startupBarrier, &ipv4Declaration](const net::in::SocketAddress& address, core::socket::State state) {
                    if (state == core::socket::State::OK && !nativeOptions.allowInsecureRemote &&
                        !apps::codex_backend::isLoopbackFrontendAddress(address.getHost(), false)) {
                        std::cerr << "codex-backend: rejected a non-loopback plaintext IPv4 frontend bind\n";
                        startupBarrier.bindingFailed("ipv4");
                        return;
                    }
                    static_cast<void>(reportBoundListener("ipv4", address, state, ipv4Declaration, startupBarrier));
                });
        }
        if (nativeOptions.ipv6Enabled) {
            ipv6Server.listen(
                nativeOptions.ipv6Address,
                nativeOptions.ipv6Port,
                [&nativeOptions, &startupBarrier, &ipv6Declaration](const net::in6::SocketAddress& address, core::socket::State state) {
                    if (state == core::socket::State::OK && !nativeOptions.allowInsecureRemote &&
                        !apps::codex_backend::isLoopbackFrontendAddress(address.getHost(), true)) {
                        std::cerr << "codex-backend: rejected a non-loopback plaintext IPv6 frontend bind\n";
                        startupBarrier.bindingFailed("ipv6");
                        return;
                    }
                    static_cast<void>(reportBoundListener("ipv6", address, state, ipv6Declaration, startupBarrier));
                });
        }
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        if (nativeOptions.tlsIpv4Enabled) {
            tlsIpv4Server.listen(nativeOptions.tlsIpv4Address,
                                 nativeOptions.tlsIpv4Port,
                                 [&startupBarrier, &tlsIpv4Declaration](const net::in::SocketAddress& address, core::socket::State state) {
                                     static_cast<void>(reportBoundListener("tls-ipv4", address, state, tlsIpv4Declaration, startupBarrier));
                                 });
        }
        if (nativeOptions.tlsIpv6Enabled) {
            tlsIpv6Server.listen(nativeOptions.tlsIpv6Address,
                                 nativeOptions.tlsIpv6Port,
                                 [&startupBarrier, &tlsIpv6Declaration](const net::in6::SocketAddress& address, core::socket::State state) {
                                     static_cast<void>(reportBoundListener("tls-ipv6", address, state, tlsIpv6Declaration, startupBarrier));
                                 });
        }
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        if (nativeOptions.rfcommEnabled) {
            rfcommServer.listen(nativeOptions.rfcommAddress,
                                static_cast<std::uint8_t>(nativeOptions.rfcommChannel),
                                [&startupBarrier, &rfcommDeclaration](const net::rc::SocketAddress& address, core::socket::State state) {
                                    static_cast<void>(reportBoundListener("rfcomm", address, state, rfcommDeclaration, startupBarrier));
                                });
        }
        if (nativeOptions.rfcommTlsEnabled) {
            rfcommTlsServer.listen(
                nativeOptions.rfcommTlsAddress,
                static_cast<std::uint8_t>(nativeOptions.rfcommTlsChannel),
                [&startupBarrier, &rfcommTlsDeclaration](const net::rc::SocketAddress& address, core::socket::State state) {
                    static_cast<void>(reportBoundListener("rfcomm-tls", address, state, rfcommTlsDeclaration, startupBarrier));
                });
        }
#endif

        result = core::SNodeC::start();
        if (startupBarrier.startupFailed()) {
            result = 1;
        }
        frontendService.close("codex-backend is stopping");
    }

    // EventLoop::start performs the primary global shutdown. free() is
    // intentionally repeated after listeners, FrontendService, and BackendCore
    // have been destroyed; the SNode.C lifecycle makes cleanup idempotent.
    core::SNodeC::free();
    return result;
}
