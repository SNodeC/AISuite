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
#include "ai/openai/codex/typed/Types.h"
#include "apps/codex-backend/ReferenceAuthentication.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CLI {
    class Option;
}

namespace apps::codex_backend {

    [[nodiscard]] ai::openai::codex::typed::InitializeParams appServerInitializeParams();

    inline constexpr std::size_t DEFAULT_MAXIMUM_FRAME_SIZE =
        ai::openai::codex::frontend::DefaultFrontendMaximumInboundMessageBytes;

    // Keep transport framing and already-delivered writer data outside the
    // reusable service's bounded queue. This remains finite while ensuring a
    // freshly connected Unix client can accept a maximum-sized default replay.
    inline constexpr std::size_t DEFAULT_MAXIMUM_OUTBOUND_BYTES = 13 * 1024 * 1024;

    static_assert(DEFAULT_MAXIMUM_OUTBOUND_BYTES >= ai::openai::codex::frontend::DefaultFrontendServiceMaxOutboundBytes +
                                                        ai::openai::codex::frontend::DefaultFrontendServiceMaxOutboundMessages);

    struct SocketFrontendOptions {
        std::size_t maximumFrameSize = DEFAULT_MAXIMUM_FRAME_SIZE;
    };

    class AppServerProcessConfiguration {
    public:
        AppServerProcessConfiguration();

        [[nodiscard]] std::vector<std::pair<std::string, std::string>> environmentOverrides() const;

    private:
        CLI::Option* codexHomeOption = nullptr;
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
        [[nodiscard]] bool allowInsecureRemote() const;

    private:
        CLI::Option* verifiedLocalTrustOption = nullptr;
        CLI::Option* insecureLocalTrustOverrideOption = nullptr;
        CLI::Option* bearerTokenFileOption = nullptr;
        CLI::Option* remotePrincipalIdOption = nullptr;
        CLI::Option* remoteScopeProfileOption = nullptr;
        CLI::Option* allowInsecureRemoteOption = nullptr;
    };

    struct FrontendWebOptions {
        std::string endpoint = "/frontend";
        std::optional<std::filesystem::path> staticRoot;
        std::vector<std::string> allowedOrigins;

        [[nodiscard]] std::optional<std::string> validationError() const;
    };

    class FrontendWebConfiguration {
    public:
        FrontendWebConfiguration();

        [[nodiscard]] FrontendWebOptions options() const;

    private:
        CLI::Option* endpointOption = nullptr;
        CLI::Option* staticRootOption = nullptr;
        CLI::Option* allowedOriginsOption = nullptr;
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
