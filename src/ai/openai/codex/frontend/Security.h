/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_SECURITY_H
#define AI_OPENAI_CODEX_FRONTEND_SECURITY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ai::openai::codex::frontend {

    enum class FrontendScope {
        Observe,
        Control,
        ProviderLifecycle,
        AccountManagement,
        ConfigurationWrite,
        CommandExecution,
        FilesystemRead,
        FilesystemWrite,
        ExtensionManagement,
        McpInvoke,
        SensitiveResponse,
        UnknownRequestResponse
    };

    inline constexpr std::array<FrontendScope, 2> DefaultRemoteScopes{
        FrontendScope::Observe,
        FrontendScope::Control,
    };

    inline constexpr std::array<FrontendScope, 12> LocalTrustedScopes{
        FrontendScope::Observe,
        FrontendScope::Control,
        FrontendScope::ProviderLifecycle,
        FrontendScope::AccountManagement,
        FrontendScope::ConfigurationWrite,
        FrontendScope::CommandExecution,
        FrontendScope::FilesystemRead,
        FrontendScope::FilesystemWrite,
        FrontendScope::ExtensionManagement,
        FrontendScope::McpInvoke,
        FrontendScope::SensitiveResponse,
        FrontendScope::UnknownRequestResponse,
    };

    [[nodiscard]] constexpr std::string_view toString(FrontendScope scope) noexcept {
        switch (scope) {
            case FrontendScope::Observe:
                return "observe";
            case FrontendScope::Control:
                return "control";
            case FrontendScope::ProviderLifecycle:
                return "provider_lifecycle";
            case FrontendScope::AccountManagement:
                return "account_management";
            case FrontendScope::ConfigurationWrite:
                return "configuration_write";
            case FrontendScope::CommandExecution:
                return "command_execution";
            case FrontendScope::FilesystemRead:
                return "filesystem_read";
            case FrontendScope::FilesystemWrite:
                return "filesystem_write";
            case FrontendScope::ExtensionManagement:
                return "extension_management";
            case FrontendScope::McpInvoke:
                return "mcp_invoke";
            case FrontendScope::SensitiveResponse:
                return "sensitive_response";
            case FrontendScope::UnknownRequestResponse:
                return "unknown_request_response";
        }
        return {};
    }

    [[nodiscard]] constexpr std::optional<FrontendScope> frontendScopeFromString(std::string_view value) noexcept {
        for (const FrontendScope scope : LocalTrustedScopes) {
            if (value == toString(scope)) {
                return scope;
            }
        }
        return std::nullopt;
    }

    struct FrontendScopeProfile {
        std::string_view name;
        std::span<const FrontendScope> scopes;
        bool remote = false;

        [[nodiscard]] constexpr bool operator==(const FrontendScopeProfile& other) const noexcept {
            if (name != other.name || remote != other.remote || scopes.size() != other.scopes.size()) {
                return false;
            }
            for (std::size_t index = 0; index < scopes.size(); ++index) {
                if (scopes[index] != other.scopes[index]) {
                    return false;
                }
            }
            return true;
        }
    };

    inline constexpr FrontendScopeProfile DefaultRemoteScopeProfile{
        "default_remote",
        std::span<const FrontendScope>{DefaultRemoteScopes},
        true,
    };

    inline constexpr FrontendScopeProfile LocalTrustedScopeProfile{
        "local_trusted",
        std::span<const FrontendScope>{LocalTrustedScopes},
        false,
    };

    enum class FrontendTransportKind { Unix, Ipv4, Ipv6, TcpTls, WebSocket, WebSocketTls, Rfcomm, RfcommTls, InMemory };

    inline constexpr std::array<FrontendTransportKind, 9> FrontendTransportKinds{
        FrontendTransportKind::Unix,
        FrontendTransportKind::Ipv4,
        FrontendTransportKind::Ipv6,
        FrontendTransportKind::TcpTls,
        FrontendTransportKind::WebSocket,
        FrontendTransportKind::WebSocketTls,
        FrontendTransportKind::Rfcomm,
        FrontendTransportKind::RfcommTls,
        FrontendTransportKind::InMemory,
    };

    [[nodiscard]] constexpr std::string_view toString(FrontendTransportKind kind) noexcept {
        switch (kind) {
            case FrontendTransportKind::Unix:
                return "unix";
            case FrontendTransportKind::Ipv4:
                return "ipv4";
            case FrontendTransportKind::Ipv6:
                return "ipv6";
            case FrontendTransportKind::TcpTls:
                return "tcp_tls";
            case FrontendTransportKind::WebSocket:
                return "websocket";
            case FrontendTransportKind::WebSocketTls:
                return "websocket_tls";
            case FrontendTransportKind::Rfcomm:
                return "rfcomm";
            case FrontendTransportKind::RfcommTls:
                return "rfcomm_tls";
            case FrontendTransportKind::InMemory:
                return "in_memory";
        }
        return {};
    }

    [[nodiscard]] constexpr std::optional<FrontendTransportKind> frontendTransportKindFromString(std::string_view value) noexcept {
        for (const FrontendTransportKind kind : FrontendTransportKinds) {
            if (value == toString(kind)) {
                return kind;
            }
        }
        return std::nullopt;
    }

    struct FrontendPeerContext {
        FrontendTransportKind transport = FrontendTransportKind::InMemory;
        bool encrypted = false;
        bool loopback = false;
        bool localPeer = false;
        std::optional<std::string> remoteAddress;
        std::optional<std::string> origin;
        std::optional<std::uint64_t> unixUserId;

        bool operator==(const FrontendPeerContext&) const = default;
    };

    struct NoCredential {
        bool operator==(const NoCredential&) const = default;
    };

    struct BearerCredential {
        std::string token;

        bool operator==(const BearerCredential&) const = default;
    };

    using AuthenticationCredential = std::variant<NoCredential, BearerCredential>;

    struct FrontendPrincipal {
        std::string id;
        std::vector<FrontendScope> scopes;
        std::string profile;
        bool localTrusted = false;

        bool operator==(const FrontendPrincipal&) const = default;
    };

    enum class AuthenticationFailureCode {
        AuthenticationRequired,
        AuthenticationFailed,
        OriginRejected,
        TransportSecurityRequired,
        RateLimited
    };

    inline constexpr std::array<AuthenticationFailureCode, 5> AuthenticationFailureCodes{
        AuthenticationFailureCode::AuthenticationRequired,
        AuthenticationFailureCode::AuthenticationFailed,
        AuthenticationFailureCode::OriginRejected,
        AuthenticationFailureCode::TransportSecurityRequired,
        AuthenticationFailureCode::RateLimited,
    };

    [[nodiscard]] constexpr std::string_view toString(AuthenticationFailureCode code) noexcept {
        switch (code) {
            case AuthenticationFailureCode::AuthenticationRequired:
                return "authentication_required";
            case AuthenticationFailureCode::AuthenticationFailed:
                return "authentication_failed";
            case AuthenticationFailureCode::OriginRejected:
                return "origin_rejected";
            case AuthenticationFailureCode::TransportSecurityRequired:
                return "transport_security_required";
            case AuthenticationFailureCode::RateLimited:
                return "rate_limited";
        }
        return {};
    }

    [[nodiscard]] constexpr std::optional<AuthenticationFailureCode> authenticationFailureCodeFromString(std::string_view value) noexcept {
        for (const AuthenticationFailureCode code : AuthenticationFailureCodes) {
            if (value == toString(code)) {
                return code;
            }
        }
        return std::nullopt;
    }

    struct AuthenticationSuccess {
        FrontendPrincipal principal;

        bool operator==(const AuthenticationSuccess&) const = default;
    };

    struct AuthenticationFailure {
        AuthenticationFailureCode code = AuthenticationFailureCode::AuthenticationFailed;

        bool operator==(const AuthenticationFailure&) const = default;
    };

    using AuthenticationResult = std::variant<AuthenticationSuccess, AuthenticationFailure>;

    // Scope possession authorizes a capability. Controller ownership is a
    // separate serialization policy: the control scope neither acquires nor
    // proves controller ownership, and controller ownership grants no scope.

    namespace security_detail {

        template <std::size_t Size>
        [[nodiscard]] consteval bool uniqueScopes(const std::array<FrontendScope, Size>& scopes) {
            for (std::size_t left = 0; left < scopes.size(); ++left) {
                for (std::size_t right = left + 1; right < scopes.size(); ++right) {
                    if (scopes[left] == scopes[right]) {
                        return false;
                    }
                }
            }
            return true;
        }

        template <std::size_t Size>
        [[nodiscard]] consteval bool uniqueSpellings(const std::array<FrontendScope, Size>& scopes) {
            for (std::size_t left = 0; left < scopes.size(); ++left) {
                if (toString(scopes[left]).empty()) {
                    return false;
                }
                for (std::size_t right = left + 1; right < scopes.size(); ++right) {
                    if (toString(scopes[left]) == toString(scopes[right])) {
                        return false;
                    }
                }
            }
            return true;
        }

        template <std::size_t Size>
        [[nodiscard]] consteval bool allRoundTrip(const std::array<FrontendScope, Size>& scopes) {
            for (const FrontendScope scope : scopes) {
                if (frontendScopeFromString(toString(scope)) != scope) {
                    return false;
                }
            }
            return true;
        }

    } // namespace security_detail

    static_assert(LocalTrustedScopes.size() == 12);
    static_assert(static_cast<std::size_t>(FrontendScope::UnknownRequestResponse) + 1 == LocalTrustedScopes.size());
    static_assert(DefaultRemoteScopes.size() == 2);
    static_assert(security_detail::uniqueScopes(LocalTrustedScopes));
    static_assert(security_detail::uniqueScopes(DefaultRemoteScopes));
    static_assert(security_detail::uniqueSpellings(LocalTrustedScopes));
    static_assert(security_detail::allRoundTrip(LocalTrustedScopes));
    static_assert(DefaultRemoteScopes[0] == FrontendScope::Observe && DefaultRemoteScopes[1] == FrontendScope::Control);
    static_assert(DefaultRemoteScopeProfile.name == "default_remote" && DefaultRemoteScopeProfile.remote &&
                  DefaultRemoteScopeProfile.scopes.size() == 2);
    static_assert(LocalTrustedScopeProfile.name == "local_trusted" && !LocalTrustedScopeProfile.remote &&
                  LocalTrustedScopeProfile.scopes.size() == 12);
    static_assert(FrontendTransportKinds.size() == 9);
    static_assert(static_cast<std::size_t>(FrontendTransportKind::InMemory) + 1 == FrontendTransportKinds.size());
    static_assert([] {
        for (const FrontendTransportKind kind : FrontendTransportKinds) {
            if (toString(kind).empty() || frontendTransportKindFromString(toString(kind)) != kind) {
                return false;
            }
        }
        return true;
    }());
    static_assert(AuthenticationFailureCodes.size() == 5);
    static_assert(static_cast<std::size_t>(AuthenticationFailureCode::RateLimited) + 1 == AuthenticationFailureCodes.size());
    static_assert([] {
        for (const AuthenticationFailureCode code : AuthenticationFailureCodes) {
            if (toString(code).empty() || authenticationFailureCodeFromString(toString(code)) != code) {
                return false;
            }
        }
        return true;
    }());

} // namespace ai::openai::codex::frontend

#endif // AI_OPENAI_CODEX_FRONTEND_SECURITY_H
