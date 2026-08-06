/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/ReferenceAuthentication.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace apps::codex_backend {

    namespace {

        using ai::openai::codex::frontend::AuthenticationFailure;
        using ai::openai::codex::frontend::AuthenticationFailureCode;
        using ai::openai::codex::frontend::AuthenticationResult;
        using ai::openai::codex::frontend::AuthenticationSuccess;
        using ai::openai::codex::frontend::BearerCredential;
        using ai::openai::codex::frontend::FrontendPeerContext;
        using ai::openai::codex::frontend::FrontendPrincipal;
        using ai::openai::codex::frontend::NoCredential;

        void eraseSecret(std::string& value) noexcept {
            volatile char* bytes = value.empty() ? nullptr : value.data();
            for (std::size_t index = 0; index < value.size(); ++index) {
                bytes[index] = 0;
            }
            value.clear();
        }

        class FileDescriptor {
        public:
            explicit FileDescriptor(int value) noexcept
                : value(value) {
            }

            FileDescriptor(const FileDescriptor&) = delete;
            FileDescriptor& operator=(const FileDescriptor&) = delete;

            ~FileDescriptor() {
                if (value >= 0) {
                    ::close(value);
                }
            }

            [[nodiscard]] int get() const noexcept {
                return value;
            }

        private:
            int value = -1;
        };

        ProtectedTokenFileError fileError(ProtectedTokenFileErrorCode code, std::string message) {
            return ProtectedTokenFileError{code, std::move(message)};
        }

        AuthenticationResult authenticationFailure(AuthenticationFailureCode code) {
            return AuthenticationFailure{code};
        }

    } // namespace

    ProtectedBearerToken::ProtectedBearerToken(std::string source) noexcept {
        value.swap(source);
        eraseSecret(source);
    }

    ProtectedBearerToken::ProtectedBearerToken(ProtectedBearerToken&& other) noexcept {
        value.swap(other.value);
        eraseSecret(other.value);
    }

    ProtectedBearerToken& ProtectedBearerToken::operator=(ProtectedBearerToken&& other) noexcept {
        if (this != &other) {
            eraseSecret(value);
            value.swap(other.value);
            eraseSecret(other.value);
        }
        return *this;
    }

    ProtectedBearerToken::~ProtectedBearerToken() {
        eraseSecret(value);
    }

    bool ProtectedBearerToken::matches(std::string_view candidate) const noexcept {
        return constantTimeEqual(value, candidate);
    }

    std::size_t ProtectedBearerToken::size() const noexcept {
        return value.size();
    }

    ai::openai::codex::frontend::BearerCredential ProtectedBearerToken::credential() const {
        return ai::openai::codex::frontend::BearerCredential{value};
    }

    bool constantTimeEqual(std::string_view left, std::string_view right) noexcept {
        const std::size_t comparedSize = std::max(left.size(), right.size());
        std::size_t difference = left.size() ^ right.size();

        for (std::size_t index = 0; index < comparedSize; ++index) {
            const auto leftByte = static_cast<unsigned char>(index < left.size() ? left[index] : 0);
            const auto rightByte = static_cast<unsigned char>(index < right.size() ? right[index] : 0);
            difference |= static_cast<std::size_t>(leftByte ^ rightByte);
        }

        return difference == 0;
    }

    ProtectedTokenFileResult loadProtectedBearerTokenFile(const std::filesystem::path& path, std::size_t maximumBytes) {
        if (path.empty()) {
            return fileError(ProtectedTokenFileErrorCode::Missing, "bearer-token file path is not configured");
        }

        int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        const FileDescriptor descriptor(::open(path.c_str(), flags));
        if (descriptor.get() < 0) {
            const ProtectedTokenFileErrorCode code =
                errno == ENOENT ? ProtectedTokenFileErrorCode::Missing : ProtectedTokenFileErrorCode::Unreadable;
            return fileError(code, "bearer-token file cannot be opened");
        }

        struct stat status{};
        if (::fstat(descriptor.get(), &status) != 0) {
            return fileError(ProtectedTokenFileErrorCode::Unreadable, "bearer-token file metadata cannot be read");
        }
        if (!S_ISREG(status.st_mode)) {
            return fileError(ProtectedTokenFileErrorCode::NotRegularFile, "bearer-token file is not a regular file");
        }
        if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            return fileError(ProtectedTokenFileErrorCode::InsecurePermissions, "bearer-token file grants group or other access");
        }
        if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > maximumBytes) {
            return fileError(ProtectedTokenFileErrorCode::TooLarge, "bearer-token file exceeds the configured size limit");
        }

        std::string value;
        value.reserve(std::min<std::size_t>(maximumBytes, static_cast<std::size_t>(status.st_size)));
        std::array<char, 4096> buffer{};
        while (true) {
            const ssize_t count = ::read(descriptor.get(), buffer.data(), buffer.size());
            if (count == 0) {
                break;
            }
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                eraseSecret(value);
                return fileError(ProtectedTokenFileErrorCode::ReadFailure, "bearer-token file cannot be read completely");
            }

            const std::size_t byteCount = static_cast<std::size_t>(count);
            if (byteCount > maximumBytes || value.size() > maximumBytes - byteCount) {
                eraseSecret(value);
                std::fill(buffer.begin(), buffer.end(), 0);
                return fileError(ProtectedTokenFileErrorCode::TooLarge, "bearer-token file exceeds the configured size limit");
            }
            value.append(buffer.data(), byteCount);
        }
        std::fill(buffer.begin(), buffer.end(), 0);

        if (value.find('\0') != std::string::npos) {
            eraseSecret(value);
            return fileError(ProtectedTokenFileErrorCode::EmbeddedNull, "bearer-token file contains an embedded NUL byte");
        }
        if (!value.empty() && value.back() == '\n') {
            value.pop_back();
            if (!value.empty() && value.back() == '\r') {
                value.pop_back();
            }
        }
        if (value.empty()) {
            eraseSecret(value);
            return fileError(ProtectedTokenFileErrorCode::Empty, "bearer-token file contains no token bytes");
        }

        return ProtectedBearerToken(std::move(value));
    }

    std::optional<std::vector<ai::openai::codex::frontend::FrontendScope>> referenceScopesForProfile(std::string_view profile) {
        using ai::openai::codex::frontend::DefaultRemoteScopeProfile;
        using ai::openai::codex::frontend::LocalTrustedScopeProfile;

        if (profile == DefaultRemoteScopeProfile.name) {
            return std::vector(DefaultRemoteScopeProfile.scopes.begin(), DefaultRemoteScopeProfile.scopes.end());
        }
        if (profile == LocalTrustedScopeProfile.name) {
            return std::vector(LocalTrustedScopeProfile.scopes.begin(), LocalTrustedScopeProfile.scopes.end());
        }
        return std::nullopt;
    }

    ReferenceAuthenticationOptions defaultReferenceAuthenticationOptions() {
        ReferenceAuthenticationOptions options;
        options.remoteScopes = *referenceScopesForProfile(options.remoteProfile);
        return options;
    }

    ReferenceAuthenticator::ReferenceAuthenticator(std::optional<ProtectedBearerToken> bearerToken, ReferenceAuthenticationOptions options)
        : bearerToken(std::move(bearerToken))
        , options(std::move(options)) {
        const std::optional<std::vector<ai::openai::codex::frontend::FrontendScope>> profileScopes =
            referenceScopesForProfile(this->options.remoteProfile);
        if (!profileScopes) {
            throw std::invalid_argument("unsupported reference authentication scope profile");
        }
        this->options.remoteScopes = *profileScopes;
    }

    AuthenticationResult
    ReferenceAuthenticator::authenticate([[maybe_unused]] const FrontendPeerContext& peer,
                                         const ai::openai::codex::frontend::AuthenticationCredential& credential) const {
        if (std::holds_alternative<NoCredential>(credential)) {
            return authenticationFailure(AuthenticationFailureCode::AuthenticationRequired);
        }
        const BearerCredential& provided = std::get<BearerCredential>(credential);
        if (!bearerToken.has_value() || !bearerToken->matches(provided.token)) {
            return authenticationFailure(AuthenticationFailureCode::AuthenticationFailed);
        }
        return AuthenticationSuccess{remotePrincipal()};
    }

    ReferenceAuthenticationDiagnostics ReferenceAuthenticator::diagnostics() const {
        return {bearerToken.has_value(), options.remotePrincipalId, options.remoteProfile};
    }

    FrontendPrincipal ReferenceAuthenticator::remotePrincipal() const {
        return FrontendPrincipal{options.remotePrincipalId, options.remoteScopes, options.remoteProfile, false};
    }

} // namespace apps::codex_backend
