/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_REFERENCEAUTHENTICATION_H
#define APPS_CODEX_BACKEND_REFERENCEAUTHENTICATION_H

#include "ai/openai/codex/frontend/Security.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace apps::codex_backend {

    inline constexpr std::size_t DEFAULT_MAXIMUM_BEARER_TOKEN_BYTES = 64U * 1024U;

    enum class ProtectedTokenFileErrorCode {
        Missing,
        Unreadable,
        NotRegularFile,
        InsecurePermissions,
        TooLarge,
        Empty,
        EmbeddedNull,
        ReadFailure
    };

    struct ProtectedTokenFileError {
        ProtectedTokenFileErrorCode code = ProtectedTokenFileErrorCode::ReadFailure;
        std::string message;

        bool operator==(const ProtectedTokenFileError&) const = default;
    };

    // Move-only storage prevents accidental copies of the reference bearer
    // credential. The retained bytes are erased before their allocation is
    // released and are never exposed through diagnostics or serialization.
    class ProtectedBearerToken {
    public:
        ProtectedBearerToken(const ProtectedBearerToken&) = delete;
        ProtectedBearerToken(ProtectedBearerToken&& other) noexcept;

        ProtectedBearerToken& operator=(const ProtectedBearerToken&) = delete;
        ProtectedBearerToken& operator=(ProtectedBearerToken&& other) noexcept;

        ~ProtectedBearerToken();

        [[nodiscard]] bool matches(std::string_view candidate) const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

    private:
        explicit ProtectedBearerToken(std::string value) noexcept;

        std::string value;

        friend std::variant<ProtectedBearerToken, ProtectedTokenFileError> loadProtectedBearerTokenFile(const std::filesystem::path&,
                                                                                                        std::size_t);
    };

    using ProtectedTokenFileResult = std::variant<ProtectedBearerToken, ProtectedTokenFileError>;

    [[nodiscard]] bool constantTimeEqual(std::string_view left, std::string_view right) noexcept;

    [[nodiscard]] ProtectedTokenFileResult loadProtectedBearerTokenFile(const std::filesystem::path& path,
                                                                        std::size_t maximumBytes = DEFAULT_MAXIMUM_BEARER_TOKEN_BYTES);

    struct ReferenceAuthenticationOptions {
        std::string remotePrincipalId = "remote";
        std::vector<ai::openai::codex::frontend::FrontendScope> remoteScopes;
        std::string remoteProfile = "default_remote";
    };

    [[nodiscard]] std::optional<std::vector<ai::openai::codex::frontend::FrontendScope>>
    referenceScopesForProfile(std::string_view profile);

    [[nodiscard]] ReferenceAuthenticationOptions defaultReferenceAuthenticationOptions();

    [[nodiscard]] constexpr bool unixFrontendRequiresBearer(bool unixEnabled,
                                                            bool verifiedLocalTrustEnabled,
                                                            bool insecureLocalTrustOverride,
                                                            bool peerCredentialsSupported) noexcept {
        return unixEnabled && !insecureLocalTrustOverride && !(verifiedLocalTrustEnabled && peerCredentialsSupported);
    }

    struct ReferenceAuthenticationDiagnostics {
        bool bearerConfigured = false;
        std::string remotePrincipalId;
        std::string remoteProfile;

        bool operator==(const ReferenceAuthenticationDiagnostics&) const = default;
    };

    class ReferenceAuthenticator {
    public:
        explicit ReferenceAuthenticator(std::optional<ProtectedBearerToken> bearerToken = std::nullopt,
                                        ReferenceAuthenticationOptions options = defaultReferenceAuthenticationOptions());

        [[nodiscard]] ai::openai::codex::frontend::AuthenticationResult
        authenticate(const ai::openai::codex::frontend::FrontendPeerContext& peer,
                     const ai::openai::codex::frontend::AuthenticationCredential& credential) const;

        [[nodiscard]] ReferenceAuthenticationDiagnostics diagnostics() const;

    private:
        [[nodiscard]] ai::openai::codex::frontend::FrontendPrincipal remotePrincipal() const;

        std::optional<ProtectedBearerToken> bearerToken;
        ReferenceAuthenticationOptions options;
    };

} // namespace apps::codex_backend

#endif // APPS_CODEX_BACKEND_REFERENCEAUTHENTICATION_H
