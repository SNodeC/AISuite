/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/Configuration.h"

#include "apps/codex-backend/ReferenceInvocationPolicy.h"
#include "utils/Config.h"

#include <arpa/inet.h>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>

namespace apps::codex_backend {

    namespace {

        CLI::Option* configurableBoolean(std::string name, std::string description, bool defaultValue) {
            auto& root = utils::Config::configRoot;
            const std::string defaultText = defaultValue ? "true" : "false";
            return root.setConfigurable(
                root.addFlag(std::move(name) + "{true}", std::move(description), "BOOL", defaultText, CLI::IsMember({"true", "false"})),
                true);
        }

        CLI::Option* configurableString(std::string name, std::string description, std::string defaultValue, std::string type) {
            auto& root = utils::Config::configRoot;
            return root.setConfigurable(
                root.addOption(std::move(name), std::move(description), std::move(type), std::move(defaultValue), CLI::Validator{}), true);
        }

        template <typename Value>
        CLI::Option* configurableUnsigned(std::string name, std::string description, std::string type, Value defaultValue) {
            auto& root = utils::Config::configRoot;
            return root.setConfigurable(
                root.addOption(std::move(name), std::move(description), std::move(type), defaultValue, CLI::TypeValidator<Value>()), true);
        }

        bool regularReadableFile(const std::string& path) {
            std::error_code error;
            return !path.empty() && std::filesystem::is_regular_file(path, error) && !error && ::access(path.c_str(), R_OK) == 0;
        }

    } // namespace

    ProviderRecoveryConfiguration::ProviderRecoveryConfiguration() {
        auto& root = utils::Config::configRoot;

        enabledOption = root.setConfigurable(root.addFlag("--provider-recovery-enabled{true}",
                                                          "Automatically recover the Codex provider after transport or process failures",
                                                          "BOOL",
                                                          "true",
                                                          CLI::IsMember({"true", "false"})),
                                             true);
        maximumAttemptsOption = root.setConfigurable(root.addOption("--provider-recovery-maximum-attempts",
                                                                    "Maximum automatic recovery attempts (zero means unlimited)",
                                                                    "UINT",
                                                                    std::uint32_t{0},
                                                                    CLI::TypeValidator<std::uint32_t>()),
                                                     true);
        initialDelayOption = root.setConfigurable(root.addOption("--provider-recovery-initial-delay-ms",
                                                                 "Initial provider recovery delay in milliseconds",
                                                                 "UINT64",
                                                                 std::uint64_t{1000},
                                                                 CLI::TypeValidator<std::uint64_t>()),
                                                  true);
        maximumDelayOption = root.setConfigurable(root.addOption("--provider-recovery-maximum-delay-ms",
                                                                 "Maximum provider recovery delay in milliseconds",
                                                                 "UINT64",
                                                                 std::uint64_t{30000},
                                                                 CLI::TypeValidator<std::uint64_t>()),
                                                  true);
        multiplierOption = root.setConfigurable(root.addOption("--provider-recovery-multiplier",
                                                               "Provider recovery exponential-delay multiplier",
                                                               "UINT",
                                                               std::uint32_t{2},
                                                               CLI::TypeValidator<std::uint32_t>()),
                                                true);
    }

    ai::openai::codex::backend::RecoveryOptions ProviderRecoveryConfiguration::options() const {
        return {.enabled = enabledOption->as<bool>(),
                .maximumAttempts = maximumAttemptsOption->as<std::uint32_t>(),
                .initialDelayMs = initialDelayOption->as<std::uint64_t>(),
                .maximumDelayMs = maximumDelayOption->as<std::uint64_t>(),
                .multiplier = multiplierOption->as<std::uint32_t>()};
    }

    ReferenceAuthenticationConfiguration::ReferenceAuthenticationConfiguration() {
        auto& root = utils::Config::configRoot;

        verifiedLocalTrustOption =
            root.setConfigurable(root.addFlag("--frontend-unix-verified-local-trust{true}",
                                              "Allow owner-only Unix listeners to authenticate verified same-user peers",
                                              "BOOL",
                                              "true",
                                              CLI::IsMember({"true", "false"})),
                                 true);
        insecureLocalTrustOverrideOption =
            root.setConfigurable(root.addFlag("--frontend-unix-insecure-local-trust{true}",
                                              "Explicitly trust Unix peers when OS peer credentials are unavailable (unsafe)",
                                              "BOOL",
                                              "false",
                                              CLI::IsMember({"true", "false"})),
                                 true);
        bearerTokenFileOption = root.setConfigurable(root.addOption("--frontend-bearer-token-file",
                                                                    "Protected file containing the remote frontend bearer token",
                                                                    "PATH",
                                                                    std::string{},
                                                                    CLI::Validator{}),
                                                     true);
        remotePrincipalIdOption = root.setConfigurable(root.addOption("--frontend-remote-principal-id",
                                                                      "Principal identifier assigned after bearer authentication",
                                                                      "ID",
                                                                      std::string{"remote"},
                                                                      CLI::Validator{}),
                                                       true);
        remoteScopeProfileOption = root.setConfigurable(root.addOption("--frontend-remote-scope-profile",
                                                                       "Scope profile assigned after bearer authentication",
                                                                       "PROFILE",
                                                                       std::string{"default_remote"},
                                                                       CLI::IsMember({"default_remote", "local_trusted"})),
                                                        true);
        allowInsecureRemoteOption =
            configurableBoolean("--frontend-allow-insecure-remote",
                                "Allow authenticated plaintext frontend listeners on non-loopback addresses (unsafe)",
                                false);
    }

    ReferenceAuthenticationOptions ReferenceAuthenticationConfiguration::options() const {
        ReferenceAuthenticationOptions authenticationOptions = defaultReferenceAuthenticationOptions();
        authenticationOptions.remotePrincipalId = remotePrincipalIdOption->as<std::string>();
        authenticationOptions.remoteProfile = remoteScopeProfileOption->as<std::string>();
        if (const auto scopes = referenceScopesForProfile(authenticationOptions.remoteProfile)) {
            authenticationOptions.remoteScopes = *scopes;
        } else {
            authenticationOptions.remoteScopes.clear();
        }
        return authenticationOptions;
    }

    std::string ReferenceAuthenticationConfiguration::bearerTokenFile() const {
        return bearerTokenFileOption->as<std::string>();
    }

    bool ReferenceAuthenticationConfiguration::verifiedLocalTrustEnabled() const {
        return verifiedLocalTrustOption->as<bool>();
    }

    bool ReferenceAuthenticationConfiguration::insecureLocalTrustOverride() const {
        return insecureLocalTrustOverrideOption->as<bool>();
    }

    std::size_t NativeFrontendListenerOptions::enabledListenerCount() const noexcept {
        return static_cast<std::size_t>(unixEnabled) + static_cast<std::size_t>(ipv4Enabled) + static_cast<std::size_t>(ipv6Enabled) +
               static_cast<std::size_t>(tlsIpv4Enabled) + static_cast<std::size_t>(tlsIpv6Enabled) +
               static_cast<std::size_t>(rfcommEnabled) + static_cast<std::size_t>(rfcommTlsEnabled);
    }

    bool NativeFrontendListenerOptions::remoteAuthenticationRequired() const noexcept {
        return ipv4Enabled || ipv6Enabled || tlsIpv4Enabled || tlsIpv6Enabled || rfcommEnabled || rfcommTlsEnabled;
    }

    std::optional<std::string> NativeFrontendListenerOptions::validationError() const {
        if (ipv4Enabled && !allowInsecureRemote && !isLoopbackFrontendAddress(ipv4Address, false)) {
            return "the plaintext IPv4 frontend requires a loopback bind or the explicit insecure-remote override";
        }
        if (ipv4Enabled && ipv4Port == 0) {
            return "the enabled IPv4 frontend requires a nonzero configured port";
        }
        if (ipv6Enabled && !allowInsecureRemote && !isLoopbackFrontendAddress(ipv6Address, true)) {
            return "the plaintext IPv6 frontend requires a loopback bind or the explicit insecure-remote override";
        }
        if (ipv6Enabled && ipv6Port == 0) {
            return "the enabled IPv6 frontend requires a nonzero configured port";
        }
#if !defined(AISUITE_CODEX_FRONTEND_TLS)
        if (tlsIpv4Enabled || tlsIpv6Enabled) {
            return "TLS frontend listeners were enabled but TLS support was not compiled";
        }
#else
        if (tlsIpv4Enabled && tlsIpv4Port == 0) {
            return "the enabled IPv4 TLS frontend requires a nonzero configured port";
        }
        if (tlsIpv4Enabled && (!regularReadableFile(tlsIpv4Certificate) || !regularReadableFile(tlsIpv4PrivateKey))) {
            return "the enabled IPv4 TLS frontend requires readable certificate and private-key files";
        }
        if (tlsIpv6Enabled && tlsIpv6Port == 0) {
            return "the enabled IPv6 TLS frontend requires a nonzero configured port";
        }
        if (tlsIpv6Enabled && (!regularReadableFile(tlsIpv6Certificate) || !regularReadableFile(tlsIpv6PrivateKey))) {
            return "the enabled IPv6 TLS frontend requires readable certificate and private-key files";
        }
#endif
#if !defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        if (rfcommEnabled || rfcommTlsEnabled) {
            return "RFCOMM frontend listeners were enabled but RFCOMM support was not compiled";
        }
#else
        if ((rfcommEnabled && (rfcommChannel == 0 || rfcommChannel > 30)) ||
            (rfcommTlsEnabled && (rfcommTlsChannel == 0 || rfcommTlsChannel > 30))) {
            return "RFCOMM frontend channels must be between 1 and 30";
        }
        if (rfcommTlsEnabled && (!regularReadableFile(rfcommTlsCertificate) || !regularReadableFile(rfcommTlsPrivateKey))) {
            return "the enabled RFCOMM TLS frontend requires readable certificate and private-key files";
        }
#endif
        return std::nullopt;
    }

    NativeFrontendConfiguration::NativeFrontendConfiguration() {
        unixEnabledOption = configurableBoolean("--frontend-unix-enabled", "Enable the Unix JSONL frontend listener", true);
        ipv4EnabledOption = configurableBoolean("--frontend-ipv4-enabled", "Enable the IPv4 JSONL frontend listener", false);
        ipv4AddressOption = configurableString("--frontend-ipv4-address", "IPv4 JSONL frontend bind address", "127.0.0.1", "ADDRESS");
        ipv4PortOption = configurableUnsigned<std::uint16_t>("--frontend-ipv4-port", "IPv4 JSONL frontend bind port", "PORT", 0);
        ipv6EnabledOption = configurableBoolean("--frontend-ipv6-enabled", "Enable the IPv6 JSONL frontend listener", false);
        ipv6AddressOption = configurableString("--frontend-ipv6-address", "IPv6 JSONL frontend bind address", "::1", "ADDRESS");
        ipv6PortOption = configurableUnsigned<std::uint16_t>("--frontend-ipv6-port", "IPv6 JSONL frontend bind port", "PORT", 0);
        tlsIpv4EnabledOption = configurableBoolean("--frontend-tls-ipv4-enabled", "Enable the IPv4 TLS JSONL frontend listener", false);
        tlsIpv4AddressOption = configurableString("--frontend-tls-ipv4-address", "IPv4 TLS frontend bind address", "127.0.0.1", "ADDRESS");
        tlsIpv4PortOption = configurableUnsigned<std::uint16_t>("--frontend-tls-ipv4-port", "IPv4 TLS frontend bind port", "PORT", 0);
        tlsIpv4CertificateOption = configurableString("--frontend-tls-ipv4-certificate", "IPv4 TLS frontend certificate file", {}, "PATH");
        tlsIpv4PrivateKeyOption = configurableString("--frontend-tls-ipv4-private-key", "IPv4 TLS frontend private-key file", {}, "PATH");
        tlsIpv6EnabledOption = configurableBoolean("--frontend-tls-ipv6-enabled", "Enable the IPv6 TLS JSONL frontend listener", false);
        tlsIpv6AddressOption = configurableString("--frontend-tls-ipv6-address", "IPv6 TLS frontend bind address", "::1", "ADDRESS");
        tlsIpv6PortOption = configurableUnsigned<std::uint16_t>("--frontend-tls-ipv6-port", "IPv6 TLS frontend bind port", "PORT", 0);
        tlsIpv6CertificateOption = configurableString("--frontend-tls-ipv6-certificate", "IPv6 TLS frontend certificate file", {}, "PATH");
        tlsIpv6PrivateKeyOption = configurableString("--frontend-tls-ipv6-private-key", "IPv6 TLS frontend private-key file", {}, "PATH");
        rfcommEnabledOption = configurableBoolean("--frontend-rfcomm-enabled", "Enable the RFCOMM JSONL frontend listener", false);
        rfcommAddressOption =
            configurableString("--frontend-rfcomm-address", "RFCOMM JSONL frontend bind address", "00:00:00:00:00:00", "ADDRESS");
        rfcommChannelOption =
            configurableUnsigned<std::uint16_t>("--frontend-rfcomm-channel", "RFCOMM JSONL frontend channel", "CHANNEL", 1);
        rfcommTlsEnabledOption =
            configurableBoolean("--frontend-rfcomm-tls-enabled", "Enable the RFCOMM TLS JSONL frontend listener", false);
        rfcommTlsAddressOption =
            configurableString("--frontend-rfcomm-tls-address", "RFCOMM TLS frontend bind address", "00:00:00:00:00:00", "ADDRESS");
        rfcommTlsChannelOption =
            configurableUnsigned<std::uint16_t>("--frontend-rfcomm-tls-channel", "RFCOMM TLS frontend channel", "CHANNEL", 1);
        rfcommTlsCertificateOption =
            configurableString("--frontend-rfcomm-tls-certificate", "RFCOMM TLS frontend certificate file", {}, "PATH");
        rfcommTlsPrivateKeyOption =
            configurableString("--frontend-rfcomm-tls-private-key", "RFCOMM TLS frontend private-key file", {}, "PATH");
        allowInsecureRemoteOption =
            configurableBoolean("--frontend-allow-insecure-remote",
                                "Allow authenticated plaintext frontend listeners on non-loopback addresses (unsafe)",
                                false);
    }

    NativeFrontendListenerOptions NativeFrontendConfiguration::options() const {
        return {.unixEnabled = unixEnabledOption->as<bool>(),
                .ipv4Enabled = ipv4EnabledOption->as<bool>(),
                .ipv4Address = ipv4AddressOption->as<std::string>(),
                .ipv4Port = ipv4PortOption->as<std::uint16_t>(),
                .ipv6Enabled = ipv6EnabledOption->as<bool>(),
                .ipv6Address = ipv6AddressOption->as<std::string>(),
                .ipv6Port = ipv6PortOption->as<std::uint16_t>(),
                .tlsIpv4Enabled = tlsIpv4EnabledOption->as<bool>(),
                .tlsIpv4Address = tlsIpv4AddressOption->as<std::string>(),
                .tlsIpv4Port = tlsIpv4PortOption->as<std::uint16_t>(),
                .tlsIpv4Certificate = tlsIpv4CertificateOption->as<std::string>(),
                .tlsIpv4PrivateKey = tlsIpv4PrivateKeyOption->as<std::string>(),
                .tlsIpv6Enabled = tlsIpv6EnabledOption->as<bool>(),
                .tlsIpv6Address = tlsIpv6AddressOption->as<std::string>(),
                .tlsIpv6Port = tlsIpv6PortOption->as<std::uint16_t>(),
                .tlsIpv6Certificate = tlsIpv6CertificateOption->as<std::string>(),
                .tlsIpv6PrivateKey = tlsIpv6PrivateKeyOption->as<std::string>(),
                .rfcommEnabled = rfcommEnabledOption->as<bool>(),
                .rfcommAddress = rfcommAddressOption->as<std::string>(),
                .rfcommChannel = rfcommChannelOption->as<std::uint16_t>(),
                .rfcommTlsEnabled = rfcommTlsEnabledOption->as<bool>(),
                .rfcommTlsAddress = rfcommTlsAddressOption->as<std::string>(),
                .rfcommTlsChannel = rfcommTlsChannelOption->as<std::uint16_t>(),
                .rfcommTlsCertificate = rfcommTlsCertificateOption->as<std::string>(),
                .rfcommTlsPrivateKey = rfcommTlsPrivateKeyOption->as<std::string>(),
                .allowInsecureRemote = allowInsecureRemoteOption->as<bool>()};
    }

    FrontendRuntimeConfiguration::FrontendRuntimeConfiguration() {
        filesystemReadOption =
            configurableBoolean("--frontend-filesystem-read-enabled", "Enable provider filesystem read/search/watch methods", false);
        filesystemWriteOption =
            configurableBoolean("--frontend-filesystem-write-enabled", "Enable provider filesystem mutation methods", false);
        filesystemRootOption = configurableString(
            "--frontend-filesystem-root", "Canonical root permitted for enabled provider filesystem methods", {}, "PATH");
        commandExecutionOption =
            configurableBoolean("--frontend-command-execution-enabled", "Enable provider command-execution methods", false);
        commandExecutableOption = configurableString(
            "--frontend-command-executable", "Exact argv[0] permitted for command.exec when command execution is enabled", {}, "PATH");
        shellCommandOption = configurableString(
            "--frontend-shell-command", "Exact thread.shellCommand program permitted when command execution is enabled", {}, "COMMAND");
        maxConnectionsOption =
            configurableUnsigned<std::size_t>("--frontend-max-connections", "Maximum frontend connections", "COUNT", 128);
        maxUnauthenticatedConnectionsOption = configurableUnsigned<std::size_t>(
            "--frontend-max-unauthenticated-connections", "Maximum unauthenticated frontend connections", "COUNT", 16);
        handshakeTimeoutOption = configurableUnsigned<std::uint64_t>(
            "--frontend-handshake-timeout-ms", "Frontend authentication handshake timeout", "MILLISECONDS", 10000);
        maximumInboundMessageBytesOption = configurableUnsigned<std::size_t>(
            "--frontend-maximum-message-bytes", "Maximum inbound frontend message bytes", "BYTES", 1024U * 1024U);
        maxInboundMessagesPerSecondOption =
            configurableUnsigned<std::size_t>("--frontend-max-messages-per-second", "Sustained inbound frontend message rate", "COUNT", 50);
        maxInboundBurstOption =
            configurableUnsigned<std::size_t>("--frontend-max-message-burst", "Maximum inbound frontend message burst", "COUNT", 100);
        maxOutstandingCommandsOption = configurableUnsigned<std::size_t>(
            "--frontend-max-outstanding-commands", "Maximum outstanding commands per frontend connection", "COUNT", 256);
        maximumFailedAuthenticationsOption = configurableUnsigned<std::size_t>(
            "--frontend-max-failed-authentications-per-peer", "Failed authentications admitted per peer window", "COUNT", 3);
        failedAuthenticationWindowOption = configurableUnsigned<std::uint64_t>(
            "--frontend-failed-authentication-window-ms", "Failed-authentication peer window", "MILLISECONDS", 60000);
    }

    std::optional<std::string> FrontendRuntimeConfiguration::apply(ai::openai::codex::frontend::FrontendServiceOptions& options) const {
        options.enableFilesystemReadMethods = filesystemReadOption->as<bool>();
        options.enableFilesystemWriteMethods = filesystemWriteOption->as<bool>();
        options.enableCommandExecutionMethods = commandExecutionOption->as<bool>();
        options.maxConnections = maxConnectionsOption->as<std::size_t>();
        options.maxUnauthenticatedConnections = maxUnauthenticatedConnectionsOption->as<std::size_t>();
        options.handshakeTimeoutMs = handshakeTimeoutOption->as<std::uint64_t>();
        options.maximumInboundMessageBytes = maximumInboundMessageBytesOption->as<std::size_t>();
        options.maxInboundMessagesPerSecond = maxInboundMessagesPerSecondOption->as<std::size_t>();
        options.maxInboundBurst = maxInboundBurstOption->as<std::size_t>();
        options.maxOutstandingCommandsPerConnection = maxOutstandingCommandsOption->as<std::size_t>();
        options.maximumFailedAuthenticationsPerPeer = maximumFailedAuthenticationsOption->as<std::size_t>();
        options.failedAuthenticationWindowMs = failedAuthenticationWindowOption->as<std::uint64_t>();

        const std::string filesystemRoot = filesystemRootOption->as<std::string>();
        const std::string commandExecutable = commandExecutableOption->as<std::string>();
        const std::string shellCommand = shellCommandOption->as<std::string>();
        if ((options.enableFilesystemReadMethods || options.enableFilesystemWriteMethods) && filesystemRoot.empty()) {
            return "enabled filesystem frontend methods require --frontend-filesystem-root";
        }
        if (options.enableCommandExecutionMethods && commandExecutable.empty() && shellCommand.empty()) {
            return "enabled command-execution frontend methods require an exact executable or shell-command allowlist entry";
        }

        ReferenceInvocationPolicyOptions policyOptions;
        if (!filesystemRoot.empty()) {
            policyOptions.filesystemRoots.emplace_back(filesystemRoot);
        }
        if (!commandExecutable.empty()) {
            policyOptions.commands.executables.push_back(commandExecutable);
        }
        if (!shellCommand.empty()) {
            policyOptions.commands.shellCommands.push_back(shellCommand);
        }
        std::shared_ptr<ReferenceInvocationPolicy> policy;
        try {
            policy = std::make_shared<ReferenceInvocationPolicy>(std::move(policyOptions));
        } catch (const std::exception&) {
            return "frontend filesystem or command allowlist policy is invalid";
        }

        if (options.enableFilesystemReadMethods) {
            options.filesystemReadPolicy = [policy](const auto&, std::string_view method, const auto& parameters) {
                return policy->allowsFilesystemRead(method, parameters);
            };
        }
        if (options.enableFilesystemWriteMethods) {
            options.filesystemWritePolicy = [policy](const auto&, std::string_view method, const auto& parameters) {
                return policy->allowsFilesystemWrite(method, parameters);
            };
        }
        if (options.enableCommandExecutionMethods) {
            options.commandExecutionPolicy = [policy](const auto&, std::string_view method, const auto& parameters) {
                return policy->allowsCommandExecution(method, parameters);
            };
        }
        return std::nullopt;
    }

    bool isLoopbackFrontendAddress(std::string_view address, bool ipv6) noexcept {
        // Plaintext listener admission is deliberately limited to numeric
        // addresses so DNS aliases cannot change meaning between validation
        // and bind. The bind callback repeats this check against SNode.C's
        // effective numeric socket address.
        try {
            const std::string text(address);
            if (ipv6) {
                in6_addr parsed{};
                return ::inet_pton(AF_INET6, text.c_str(), &parsed) == 1 && IN6_IS_ADDR_LOOPBACK(&parsed) != 0;
            }
            in_addr parsed{};
            return ::inet_pton(AF_INET, text.c_str(), &parsed) == 1 && (ntohl(parsed.s_addr) & 0xFF000000U) == 0x7F000000U;
        } catch (...) {
            return false;
        }
    }

    std::string defaultSocketPath() {
        const char* runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");
        if (runtimeDirectory != nullptr && *runtimeDirectory != '\0') {
            return (std::filesystem::path(runtimeDirectory) / "snodec-codex-backend.sock").string();
        }

        return (std::filesystem::path("/tmp") / ("snodec-codex-backend-" + std::to_string(::getuid()) + ".sock")).string();
    }

} // namespace apps::codex_backend
