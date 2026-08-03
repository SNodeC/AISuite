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
#include <optional>
#include <span>
#include <string_view>

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

} // namespace ai::openai::codex::frontend

#endif // AI_OPENAI_CODEX_FRONTEND_SECURITY_H
