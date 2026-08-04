/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_CONFIGURATION_H
#define APPS_CODEX_BACKEND_CONFIGURATION_H

#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/FrontendService.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "apps/codex-backend/JsonLineFramer.h"
#include "apps/codex-backend/ReferenceAuthentication.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace CLI {
    class Option;
}

namespace apps::codex_backend {

    // Keep transport framing and already-delivered writer data outside the
    // reusable service's bounded queue. This remains finite while ensuring a
    // freshly connected Unix client can accept a maximum-sized default replay.
    inline constexpr std::size_t DEFAULT_MAXIMUM_OUTBOUND_BYTES = 13 * 1024 * 1024;

    static_assert(DEFAULT_MAXIMUM_OUTBOUND_BYTES >= ai::openai::codex::frontend::DefaultFrontendServiceMaxOutboundBytes +
                                                        ai::openai::codex::frontend::DefaultFrontendServiceMaxOutboundMessages);

    struct SocketFrontendOptions {
        std::size_t maximumFrameSize = DEFAULT_MAXIMUM_FRAME_SIZE;
        std::size_t maximumOutboundBytes = DEFAULT_MAXIMUM_OUTBOUND_BYTES;
    };

    class ProviderRecoveryConfiguration {
    public:
        ProviderRecoveryConfiguration();

        [[nodiscard]] ai::openai::codex::backend::RecoveryOptions options() const;

    private:
        CLI::Option* enabledOption = nullptr;
        CLI::Option* maximumAttemptsOption = nullptr;
        CLI::Option* initialDelayOption = nullptr;
        CLI::Option* maximumDelayOption = nullptr;
        CLI::Option* multiplierOption = nullptr;
    };

    class ReferenceAuthenticationConfiguration {
    public:
        ReferenceAuthenticationConfiguration();

        [[nodiscard]] ReferenceAuthenticationOptions options() const;
        [[nodiscard]] std::string bearerTokenFile() const;
        [[nodiscard]] bool verifiedLocalTrustEnabled() const;
        [[nodiscard]] bool insecureLocalTrustOverride() const;

    private:
        CLI::Option* verifiedLocalTrustOption = nullptr;
        CLI::Option* insecureLocalTrustOverrideOption = nullptr;
        CLI::Option* bearerTokenFileOption = nullptr;
        CLI::Option* remotePrincipalIdOption = nullptr;
        CLI::Option* remoteScopeProfileOption = nullptr;
    };

    struct NativeFrontendListenerOptions {
        bool unixEnabled = true;
        bool ipv4Enabled = false;
        std::string ipv4Address = "127.0.0.1";
        std::uint16_t ipv4Port = 0;
        bool ipv6Enabled = false;
        std::string ipv6Address = "::1";
        std::uint16_t ipv6Port = 0;
        bool tlsIpv4Enabled = false;
        std::string tlsIpv4Address = "127.0.0.1";
        std::uint16_t tlsIpv4Port = 0;
        std::string tlsIpv4Certificate;
        std::string tlsIpv4PrivateKey;
        bool tlsIpv6Enabled = false;
        std::string tlsIpv6Address = "::1";
        std::uint16_t tlsIpv6Port = 0;
        std::string tlsIpv6Certificate;
        std::string tlsIpv6PrivateKey;
        bool rfcommEnabled = false;
        std::string rfcommAddress = "00:00:00:00:00:00";
        std::uint16_t rfcommChannel = 1;
        bool rfcommTlsEnabled = false;
        std::string rfcommTlsAddress = "00:00:00:00:00:00";
        std::uint16_t rfcommTlsChannel = 1;
        std::string rfcommTlsCertificate;
        std::string rfcommTlsPrivateKey;
        bool allowInsecureRemote = false;

        [[nodiscard]] std::size_t enabledListenerCount() const noexcept;
        [[nodiscard]] bool remoteAuthenticationRequired() const noexcept;
        [[nodiscard]] std::optional<std::string> validationError() const;
    };

    class NativeFrontendConfiguration {
    public:
        NativeFrontendConfiguration();

        [[nodiscard]] NativeFrontendListenerOptions options() const;

    private:
        CLI::Option* unixEnabledOption = nullptr;
        CLI::Option* ipv4EnabledOption = nullptr;
        CLI::Option* ipv4AddressOption = nullptr;
        CLI::Option* ipv4PortOption = nullptr;
        CLI::Option* ipv6EnabledOption = nullptr;
        CLI::Option* ipv6AddressOption = nullptr;
        CLI::Option* ipv6PortOption = nullptr;
        CLI::Option* tlsIpv4EnabledOption = nullptr;
        CLI::Option* tlsIpv4AddressOption = nullptr;
        CLI::Option* tlsIpv4PortOption = nullptr;
        CLI::Option* tlsIpv4CertificateOption = nullptr;
        CLI::Option* tlsIpv4PrivateKeyOption = nullptr;
        CLI::Option* tlsIpv6EnabledOption = nullptr;
        CLI::Option* tlsIpv6AddressOption = nullptr;
        CLI::Option* tlsIpv6PortOption = nullptr;
        CLI::Option* tlsIpv6CertificateOption = nullptr;
        CLI::Option* tlsIpv6PrivateKeyOption = nullptr;
        CLI::Option* rfcommEnabledOption = nullptr;
        CLI::Option* rfcommAddressOption = nullptr;
        CLI::Option* rfcommChannelOption = nullptr;
        CLI::Option* rfcommTlsEnabledOption = nullptr;
        CLI::Option* rfcommTlsAddressOption = nullptr;
        CLI::Option* rfcommTlsChannelOption = nullptr;
        CLI::Option* rfcommTlsCertificateOption = nullptr;
        CLI::Option* rfcommTlsPrivateKeyOption = nullptr;
        CLI::Option* allowInsecureRemoteOption = nullptr;
    };

    class FrontendRuntimeConfiguration {
    public:
        FrontendRuntimeConfiguration();

        [[nodiscard]] std::optional<std::string> apply(ai::openai::codex::frontend::FrontendServiceOptions& options) const;

    private:
        CLI::Option* filesystemReadOption = nullptr;
        CLI::Option* filesystemWriteOption = nullptr;
        CLI::Option* filesystemRootOption = nullptr;
        CLI::Option* commandExecutionOption = nullptr;
        CLI::Option* commandExecutableOption = nullptr;
        CLI::Option* shellCommandOption = nullptr;
        CLI::Option* maxConnectionsOption = nullptr;
        CLI::Option* maxUnauthenticatedConnectionsOption = nullptr;
        CLI::Option* handshakeTimeoutOption = nullptr;
        CLI::Option* maximumInboundMessageBytesOption = nullptr;
        CLI::Option* maxInboundMessagesPerSecondOption = nullptr;
        CLI::Option* maxInboundBurstOption = nullptr;
        CLI::Option* maxOutstandingCommandsOption = nullptr;
        CLI::Option* maximumFailedAuthenticationsOption = nullptr;
        CLI::Option* failedAuthenticationWindowOption = nullptr;
    };

    [[nodiscard]] bool isLoopbackFrontendAddress(std::string_view address, bool ipv6) noexcept;
    std::string defaultSocketPath();

} // namespace apps::codex_backend

#endif // APPS_CODEX_BACKEND_CONFIGURATION_H
