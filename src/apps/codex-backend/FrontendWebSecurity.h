/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_FRONTENDWEBSECURITY_H
#define APPS_CODEX_BACKEND_FRONTENDWEBSECURITY_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apps::codex_backend {

    inline constexpr std::size_t MaximumFrontendWebSocketEndpointBytes = 256;

    // The configured endpoint is an exact, already-decoded absolute path.
    // Encoded aliases, empty/dot components, queries, and fragments are
    // rejected so Express's permissive route matcher cannot widen it.
    [[nodiscard]] bool normalizedFrontendWebSocketEndpoint(std::string_view endpoint) noexcept;

    struct NormalizedWebOrigin {
        std::string scheme;
        std::string host;
        std::uint16_t port = 0;
        bool ipv6 = false;

        [[nodiscard]] std::string serialize() const;

        auto operator<=>(const NormalizedWebOrigin&) const = default;
    };

    // Parses the serialized Origin-header form. Only http and https origins
    // without credentials, path, query, fragment, wildcard, or control bytes
    // are accepted. Default ports are made explicit in the result.
    [[nodiscard]] std::optional<NormalizedWebOrigin> normalizeWebOrigin(std::string_view origin) noexcept;

    enum class WebOriginAdmission {
        Accepted,
        OriginRejected,
        TransportSecurityRequired,
    };

    class WebOriginPolicy {
    public:
        explicit WebOriginPolicy(std::vector<std::string> allowedOrigins = {});

        // A missing Origin identifies a native WebSocket client. It may
        // continue to the mandatory Hello bearer-authentication path. A
        // browser Origin is admitted only when it is same-origin or appears
        // in the explicit allow-list. Browser traffic on a non-loopback
        // plaintext connection is rejected independently of its Origin.
        [[nodiscard]] WebOriginAdmission evaluate(std::optional<std::string_view> origin,
                                                  const NormalizedWebOrigin& requestOrigin,
                                                  bool encrypted,
                                                  bool loopback) const noexcept;

        [[nodiscard]] const std::vector<NormalizedWebOrigin>& allowedOrigins() const noexcept;

    private:
        std::vector<NormalizedWebOrigin> allowed;
    };

    enum class StaticAssetDisposition {
        Serve,
        NotFound,
        MethodNotAllowed,
        Rejected,
        UnsupportedMediaType,
    };

    class StaticAssetDescriptor {
    public:
        StaticAssetDescriptor() noexcept = default;
        explicit StaticAssetDescriptor(int descriptor) noexcept;

        StaticAssetDescriptor(const StaticAssetDescriptor&) = delete;
        StaticAssetDescriptor& operator=(const StaticAssetDescriptor&) = delete;

        StaticAssetDescriptor(StaticAssetDescriptor&& other) noexcept;
        StaticAssetDescriptor& operator=(StaticAssetDescriptor&& other) noexcept;

        ~StaticAssetDescriptor();

        [[nodiscard]] bool isOpen() const noexcept;
        [[nodiscard]] int get() const noexcept;
        [[nodiscard]] int release() noexcept;
        void reset(int replacement = -1) noexcept;

    private:
        int descriptor = -1;
    };

    struct StaticAssetResolution {
        StaticAssetDisposition disposition = StaticAssetDisposition::Rejected;
        std::string_view mimeType;
        bool sendBody = false;
        std::size_t contentLength = 0;
        StaticAssetDescriptor descriptor;
    };

    class StaticAssetPolicy {
    public:
        static constexpr std::size_t MaximumRequestTargetBytes = 8192;
        static constexpr std::size_t MaximumPercentDecodePasses = 4;
        static constexpr std::size_t MaximumAssetBytes = 64U * 1024U * 1024U;

        // A disengaged root deliberately disables static-file service. A
        // configured root must identify an existing directory other than the
        // filesystem root and is canonicalized once during construction.
        explicit StaticAssetPolicy(std::optional<std::filesystem::path> root = std::nullopt,
                                   std::size_t maximumAssetBytes = MaximumAssetBytes);

        StaticAssetPolicy(const StaticAssetPolicy&) = delete;
        StaticAssetPolicy& operator=(const StaticAssetPolicy&) = delete;
        StaticAssetPolicy(StaticAssetPolicy&&) = delete;
        StaticAssetPolicy& operator=(StaticAssetPolicy&&) = delete;

        ~StaticAssetPolicy();

        [[nodiscard]] StaticAssetResolution resolve(std::string_view method, std::string_view requestTarget) const noexcept;
        [[nodiscard]] const std::optional<std::filesystem::path>& root() const noexcept;

    private:
        std::optional<std::filesystem::path> canonicalRoot;
        std::size_t maximumAssetBytes;
        int rootDescriptor = -1;
    };

    struct WebSecurityHeader {
        std::string_view name;
        std::string_view value;
    };

    // These headers apply to every static response, including errors. The CSP
    // deliberately permits no inline executable script and forbids framing.
    [[nodiscard]] std::span<const WebSecurityHeader> staticAssetSecurityHeaders() noexcept;

} // namespace apps::codex_backend

#endif // APPS_CODEX_BACKEND_FRONTENDWEBSECURITY_H
