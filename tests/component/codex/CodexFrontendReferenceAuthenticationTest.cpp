/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Security.h"
#include "apps/codex-backend/ReferenceAuthentication.h"
#include "apps/codex-backend/UnixPeerCredentials.h"
#include "support/TestResult.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <variant>

namespace {

    namespace frontend = ai::openai::codex::frontend;
    namespace app = apps::codex_backend;

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            std::string value = (std::filesystem::temp_directory_path() / "aisuite-auth-test-XXXXXX").string();
            if (char* created = ::mkdtemp(value.data()); created != nullptr) {
                path = created;
            }
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        [[nodiscard]] const std::filesystem::path& get() const noexcept {
            return path;
        }

    private:
        std::filesystem::path path;
    };

    bool writeFile(const std::filesystem::path& path, std::string_view value, mode_t mode = S_IRUSR | S_IWUSR) {
        const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
        if (descriptor < 0) {
            return false;
        }
        std::size_t offset = 0;
        while (offset < value.size()) {
            const ssize_t count = ::write(descriptor, value.data() + offset, value.size() - offset);
            if (count <= 0) {
                ::close(descriptor);
                return false;
            }
            offset += static_cast<std::size_t>(count);
        }
        return ::close(descriptor) == 0;
    }

    std::optional<app::ProtectedBearerToken> loadToken(const std::filesystem::path& path) {
        app::ProtectedTokenFileResult loaded = app::loadProtectedBearerTokenFile(path);
        if (auto* token = std::get_if<app::ProtectedBearerToken>(&loaded)) {
            return std::optional<app::ProtectedBearerToken>(std::move(*token));
        }
        return std::nullopt;
    }

    bool failureIs(const frontend::AuthenticationResult& value, frontend::AuthenticationFailureCode code) {
        const auto* failure = std::get_if<frontend::AuthenticationFailure>(&value);
        return failure != nullptr && failure->code == code;
    }

    const frontend::FrontendPrincipal* successPrincipal(const frontend::AuthenticationResult& value) {
        const auto* success = std::get_if<frontend::AuthenticationSuccess>(&value);
        return success == nullptr ? nullptr : &success->principal;
    }

} // namespace

int main() {
    tests::support::TestResult result;
    TemporaryDirectory temporary;
    result.expectTrue(!temporary.get().empty(), "an isolated authentication-test directory is created");

    result.expectTrue(app::constantTimeEqual("same-value", "same-value") && !app::constantTimeEqual("same-value", "Same-value") &&
                          !app::constantTimeEqual("same-value", "same-valuE") &&
                          !app::constantTimeEqual("same-value", "same-value-longer") && !app::constantTimeEqual("same-value", "same"),
                      "the constant-time helper compares all content and length differences without early equality shortcuts");

    const std::filesystem::path missingPath = temporary.get() / "missing.token";
    const app::ProtectedTokenFileResult missing = app::loadProtectedBearerTokenFile(missingPath);
    result.expectTrue(std::holds_alternative<app::ProtectedTokenFileError>(missing) &&
                          std::get<app::ProtectedTokenFileError>(missing).code == app::ProtectedTokenFileErrorCode::Missing,
                      "a missing bearer-token file is rejected structurally");

    const std::filesystem::path emptyPath = temporary.get() / "empty.token";
    result.expectTrue(writeFile(emptyPath, ""), "the empty protected token fixture is written");
    const app::ProtectedTokenFileResult empty = app::loadProtectedBearerTokenFile(emptyPath);
    result.expectTrue(std::holds_alternative<app::ProtectedTokenFileError>(empty) &&
                          std::get<app::ProtectedTokenFileError>(empty).code == app::ProtectedTokenFileErrorCode::Empty,
                      "an empty bearer-token file is rejected");

    const std::filesystem::path crlfPath = temporary.get() / "crlf.token";
    result.expectTrue(writeFile(crlfPath, "reference-token\r\n"), "the CRLF token fixture is written");
    std::optional<app::ProtectedBearerToken> crlfToken = loadToken(crlfPath);
    result.expectTrue(crlfToken && crlfToken->matches("reference-token") && !crlfToken->matches("reference-token\r\n"),
                      "exactly one terminal CRLF is trimmed from protected token material");

    const std::filesystem::path oneLinePath = temporary.get() / "one-line.token";
    result.expectTrue(writeFile(oneLinePath, "preserved\n\n"), "the repeated-final-newline fixture is written");
    std::optional<app::ProtectedBearerToken> oneLineToken = loadToken(oneLinePath);
    result.expectTrue(oneLineToken && oneLineToken->matches("preserved\n") && !oneLineToken->matches("preserved"),
                      "only one final LF is trimmed and every other byte remains exact");

    const std::filesystem::path nullPath = temporary.get() / "null.token";
    const std::string embeddedNull("never-log-this-secret\0suffix", 28);
    result.expectTrue(writeFile(nullPath, embeddedNull), "the embedded-NUL token fixture is written");
    const app::ProtectedTokenFileResult embeddedNullResult = app::loadProtectedBearerTokenFile(nullPath);
    result.expectTrue(
        std::holds_alternative<app::ProtectedTokenFileError>(embeddedNullResult) &&
            std::get<app::ProtectedTokenFileError>(embeddedNullResult).code == app::ProtectedTokenFileErrorCode::EmbeddedNull &&
            std::get<app::ProtectedTokenFileError>(embeddedNullResult).message.find("never-log-this-secret") == std::string::npos,
        "an embedded NUL is rejected without copying token bytes into diagnostics");

    const std::filesystem::path publicPath = temporary.get() / "public.token";
    result.expectTrue(writeFile(publicPath, "not-private") && ::chmod(publicPath.c_str(), S_IRUSR | S_IWUSR | S_IRGRP) == 0,
                      "the group-readable token fixture is prepared");
    const app::ProtectedTokenFileResult publicResult = app::loadProtectedBearerTokenFile(publicPath);
    result.expectTrue(std::holds_alternative<app::ProtectedTokenFileError>(publicResult) &&
                          std::get<app::ProtectedTokenFileError>(publicResult).code ==
                              app::ProtectedTokenFileErrorCode::InsecurePermissions,
                      "group-readable bearer-token material is rejected");

    const std::filesystem::path oversizedPath = temporary.get() / "oversized.token";
    result.expectTrue(writeFile(oversizedPath, "12345"), "the bounded token fixture is written");
    const app::ProtectedTokenFileResult oversized = app::loadProtectedBearerTokenFile(oversizedPath, 4);
    result.expectTrue(std::holds_alternative<app::ProtectedTokenFileError>(oversized) &&
                          std::get<app::ProtectedTokenFileError>(oversized).code == app::ProtectedTokenFileErrorCode::TooLarge,
                      "token storage is bounded before admission");

    std::array<int, 2> unixPair{-1, -1};
    result.expectTrue(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, unixPair.data()) == 0,
                      "a deterministic accepted-Unix-socket pair is created");
    const frontend::FrontendPeerContext verifiedPeer = app::unixPeerContextFromFileDescriptor(unixPair[0]);
    result.expectTrue(verifiedPeer.transport == frontend::FrontendTransportKind::Unix && verifiedPeer.localPeer &&
                          verifiedPeer.unixUserId == static_cast<std::uint64_t>(::geteuid()),
                      "SO_PEERCRED populates both localPeer and the verified effective Unix user ID");
    const frontend::FrontendPeerContext unavailablePeer = app::unixPeerContextFromFileDescriptor(-1);
    result.expectTrue(unavailablePeer.transport == frontend::FrontendTransportKind::Unix && !unavailablePeer.localPeer &&
                          !unavailablePeer.unixUserId.has_value(),
                      "missing peer credentials never silently become local trust");
    if (unixPair[0] >= 0) {
        ::close(unixPair[0]);
    }
    if (unixPair[1] >= 0) {
        ::close(unixPair[1]);
    }

    const std::filesystem::path socketPath = temporary.get() / "frontend.sock";
    const int listeningSocket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string socketPathString = socketPath.string();
    std::copy(socketPathString.begin(), socketPathString.end(), address.sun_path);
    result.expectTrue(listeningSocket >= 0 &&
                          ::bind(listeningSocket,
                                 reinterpret_cast<const sockaddr*>(&address),
                                 static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socketPathString.size() + 1)) == 0 &&
                          ::chmod(socketPath.c_str(), S_IRUSR | S_IWUSR) == 0 && ::listen(listeningSocket, 1) == 0,
                      "an owner-only Unix listener pathname is prepared");
    result.expectTrue(app::verifyUnixListenerPath(socketPath, static_cast<std::uint64_t>(::geteuid())).verified,
                      "the reference policy verifies socket type, effective owner, and owner-only mode");

    const int connectingSocket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const bool connected =
        connectingSocket >= 0 && ::connect(connectingSocket,
                                           reinterpret_cast<const sockaddr*>(&address),
                                           static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socketPathString.size() + 1)) == 0;
    const int acceptedSocket = connected ? ::accept(listeningSocket, nullptr, nullptr) : -1;
    const frontend::FrontendPeerContext fullyVerifiedPeer =
        app::verifiedUnixPeerContextFromFileDescriptor(acceptedSocket, static_cast<std::uint64_t>(::geteuid()));
    result.expectTrue(fullyVerifiedPeer.localPeer && fullyVerifiedPeer.unixUserId == static_cast<std::uint64_t>(::geteuid()),
                      "an accepted same-user peer becomes local only after its owner-only listener pathname also verifies");

    result.expectTrue(::chmod(socketPath.c_str(), S_IRUSR | S_IWUSR | S_IRGRP) == 0 &&
                          app::verifyUnixListenerPath(socketPath, static_cast<std::uint64_t>(::geteuid())).failure ==
                              app::UnixListenerTrustFailure::InsecureMode,
                      "a group-readable Unix listener is not eligible for local trust");
    result.expectTrue(!app::verifiedUnixPeerContextFromFileDescriptor(acceptedSocket, static_cast<std::uint64_t>(::geteuid())).localPeer,
                      "valid peer credentials cannot bypass an insecure listener pathname");
    if (acceptedSocket >= 0) {
        ::close(acceptedSocket);
    }
    if (connectingSocket >= 0) {
        ::close(connectingSocket);
    }
    if (listeningSocket >= 0) {
        ::close(listeningSocket);
    }

    frontend::FrontendPeerContext wrongPeer = verifiedPeer;
    wrongPeer.unixUserId = static_cast<std::uint64_t>(::geteuid()) + 1;
    result.expectTrue(wrongPeer.localPeer && wrongPeer.unixUserId != static_cast<std::uint64_t>(::geteuid()),
                      "a verified but different Unix identity remains distinguishable for FrontendService policy");

    app::ReferenceAuthenticator bearerOnlyAuthenticator;
    result.expectTrue(failureIs(bearerOnlyAuthenticator.authenticate(verifiedPeer, frontend::NoCredential{}),
                                frontend::AuthenticationFailureCode::AuthenticationRequired) &&
                          failureIs(bearerOnlyAuthenticator.authenticate(unavailablePeer, frontend::NoCredential{}),
                                    frontend::AuthenticationFailureCode::AuthenticationRequired),
                      "the app authenticator remains bearer-only so FrontendService is the sole local-trust policy authority");

    const std::filesystem::path bearerPath = temporary.get() / "bearer.token";
    result.expectTrue(writeFile(bearerPath, "remote-reference-token\n"), "the remote bearer fixture is written");
    app::ReferenceAuthenticationOptions remoteOptions = app::defaultReferenceAuthenticationOptions();
    remoteOptions.remotePrincipalId = "configured-remote";
    app::ReferenceAuthenticator remoteAuthenticator(loadToken(bearerPath), std::move(remoteOptions));
    frontend::FrontendPeerContext remotePeer;
    remotePeer.transport = frontend::FrontendTransportKind::Ipv4;
    remotePeer.loopback = true;
    result.expectTrue(failureIs(remoteAuthenticator.authenticate(remotePeer, frontend::NoCredential{}),
                                frontend::AuthenticationFailureCode::AuthenticationRequired) &&
                          failureIs(remoteAuthenticator.authenticate(remotePeer, frontend::BearerCredential{"wrong-token"}),
                                    frontend::AuthenticationFailureCode::AuthenticationFailed),
                      "missing and incorrect remote bearer credentials fail without revealing comparison details");
    const frontend::AuthenticationResult authenticatedRemote =
        remoteAuthenticator.authenticate(remotePeer, frontend::BearerCredential{"remote-reference-token"});
    const frontend::FrontendPrincipal* remotePrincipal = successPrincipal(authenticatedRemote);
    const app::ReferenceAuthenticationDiagnostics diagnostics = remoteAuthenticator.diagnostics();
    result.expectTrue(remotePrincipal && remotePrincipal->id == "configured-remote" && !remotePrincipal->localTrusted &&
                          remotePrincipal->profile == "default_remote" && remotePrincipal->scopes.size() == 2 &&
                          diagnostics.bearerConfigured && diagnostics.remotePrincipalId == "configured-remote" &&
                          diagnostics.remoteProfile == "default_remote",
                      "a correct bearer receives exactly the configured default_remote principal and credential-omitting diagnostics");
    result.expectTrue(diagnostics.remotePrincipalId.find("remote-reference-token") == std::string::npos &&
                          diagnostics.remoteProfile.find("remote-reference-token") == std::string::npos,
                      "reference-authenticator diagnostics never expose bearer material");

    return result.processResult();
}
