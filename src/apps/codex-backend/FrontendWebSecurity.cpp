/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/FrontendWebSecurity.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <charconv>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace apps::codex_backend {

    namespace {

        constexpr std::array SecurityHeaders{
            WebSecurityHeader{
                "Content-Security-Policy",
                "default-src 'self'; base-uri 'none'; object-src 'none'; frame-ancestors 'none'; form-action 'none'; "
                "script-src 'self'; style-src 'self'; img-src 'self' data:; font-src 'self'; connect-src 'self'",
            },
            WebSecurityHeader{"X-Content-Type-Options", "nosniff"},
            WebSecurityHeader{"Referrer-Policy", "no-referrer"},
        };

        bool hasForbiddenOriginByte(std::string_view value) noexcept {
            return std::ranges::any_of(value, [](unsigned char character) {
                return character <= 0x20 || character == 0x7f || character >= 0x80;
            });
        }

        std::optional<std::uint16_t> parsePort(std::string_view value) noexcept {
            if (value.empty() || !std::ranges::all_of(value, [](unsigned char character) {
                    return std::isdigit(character) != 0;
                })) {
                return std::nullopt;
            }
            unsigned int port = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
            if (error != std::errc{} || end != value.data() + value.size() || port == 0 ||
                port > std::numeric_limits<std::uint16_t>::max()) {
                return std::nullopt;
            }
            return static_cast<std::uint16_t>(port);
        }

        bool validDnsHost(std::string_view host) noexcept {
            if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') {
                return false;
            }
            std::size_t labelLength = 0;
            bool labelStartsWithHyphen = false;
            char previous = 0;
            for (unsigned char character : host) {
                if (character == '.') {
                    if (labelLength == 0 || labelLength > 63 || labelStartsWithHyphen || previous == '-') {
                        return false;
                    }
                    labelLength = 0;
                    labelStartsWithHyphen = false;
                } else {
                    if (std::isalnum(character) == 0 && character != '-') {
                        return false;
                    }
                    if (labelLength == 0) {
                        labelStartsWithHyphen = character == '-';
                    }
                    ++labelLength;
                }
                previous = static_cast<char>(character);
            }
            return labelLength > 0 && labelLength <= 63 && !labelStartsWithHyphen && previous != '-';
        }

        std::optional<std::pair<std::string, bool>> normalizeHost(std::string_view host, bool bracketed) noexcept {
            std::array<unsigned char, 16> address{};
            std::array<char, INET6_ADDRSTRLEN> text{};

            if (bracketed) {
                if (::inet_pton(AF_INET6, std::string(host).c_str(), address.data()) != 1 ||
                    ::inet_ntop(AF_INET6, address.data(), text.data(), text.size()) == nullptr) {
                    return std::nullopt;
                }
                return std::pair{std::string(text.data()), true};
            }

            if (host.find(':') != std::string_view::npos) {
                return std::nullopt;
            }

            if (::inet_pton(AF_INET, std::string(host).c_str(), address.data()) == 1) {
                std::array<char, INET_ADDRSTRLEN> ipv4Text{};
                if (::inet_ntop(AF_INET, address.data(), ipv4Text.data(), ipv4Text.size()) == nullptr) {
                    return std::nullopt;
                }
                return std::pair{std::string(ipv4Text.data()), false};
            }

            if (std::ranges::all_of(host, [](unsigned char character) {
                    return std::isdigit(character) != 0 || character == '.';
                })) {
                return std::nullopt;
            }

            if (!validDnsHost(host)) {
                return std::nullopt;
            }
            std::string normalized(host);
            std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return std::pair{std::move(normalized), false};
        }

        int hexadecimalValue(char character) noexcept {
            if (character >= '0' && character <= '9') {
                return character - '0';
            }
            if (character >= 'a' && character <= 'f') {
                return character - 'a' + 10;
            }
            if (character >= 'A' && character <= 'F') {
                return character - 'A' + 10;
            }
            return -1;
        }

        struct DecodeResult {
            std::string value;
            bool changed = false;
            bool valid = true;
        };

        DecodeResult decodePercentOnce(std::string_view value) {
            DecodeResult result;
            result.value.reserve(value.size());
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (value[index] != '%') {
                    result.value.push_back(value[index]);
                    continue;
                }
                if (index + 2 >= value.size()) {
                    result.valid = false;
                    return result;
                }
                const int high = hexadecimalValue(value[index + 1]);
                const int low = hexadecimalValue(value[index + 2]);
                if (high < 0 || low < 0) {
                    result.valid = false;
                    return result;
                }
                result.value.push_back(static_cast<char>((high << 4) | low));
                result.changed = true;
                index += 2;
            }
            return result;
        }

        std::optional<std::string> decodeRequestPath(std::string_view rawPath) {
            std::string current(rawPath);
            for (std::size_t pass = 0; pass < StaticAssetPolicy::MaximumPercentDecodePasses; ++pass) {
                DecodeResult decoded = decodePercentOnce(current);
                if (!decoded.valid) {
                    return std::nullopt;
                }
                current = std::move(decoded.value);
                if (!decoded.changed) {
                    return current;
                }
            }

            // A still-encoded value would need another decoding pass. Reject
            // instead of allowing an upstream/downstream decoder mismatch.
            const DecodeResult extra = decodePercentOnce(current);
            if (!extra.valid || extra.changed) {
                return std::nullopt;
            }
            return current;
        }

        bool validDecodedPath(std::string_view path) noexcept {
            if (path.empty() || path.front() != '/' || (path.size() > 1 && path[1] == '/') || path.find('\\') != std::string_view::npos ||
                path.find('\0') != std::string_view::npos || path.find('?') != std::string_view::npos ||
                path.find('#') != std::string_view::npos) {
                return false;
            }

            std::size_t start = 1;
            while (start <= path.size()) {
                const std::size_t end = path.find('/', start);
                const std::string_view component = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
                if (component.empty() || component == "." || component == "..") {
                    return false;
                }
                if (end == std::string_view::npos) {
                    break;
                }
                start = end + 1;
            }
            return true;
        }

        std::optional<std::string_view> safeMimeType(const std::filesystem::path& path) noexcept {
            std::string extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });

            static constexpr std::array<std::pair<std::string_view, std::string_view>, 16> MimeTypes{{
                {".css", "text/css; charset=utf-8"},
                {".gif", "image/gif"},
                {".html", "text/html; charset=utf-8"},
                {".ico", "image/x-icon"},
                {".jpeg", "image/jpeg"},
                {".jpg", "image/jpeg"},
                {".js", "text/javascript; charset=utf-8"},
                {".json", "application/json; charset=utf-8"},
                {".mjs", "text/javascript; charset=utf-8"},
                {".png", "image/png"},
                {".svg", "image/svg+xml"},
                {".txt", "text/plain; charset=utf-8"},
                {".wasm", "application/wasm"},
                {".webp", "image/webp"},
                {".woff", "font/woff"},
                {".woff2", "font/woff2"},
            }};
            for (const auto& [candidateExtension, mime] : MimeTypes) {
                if (candidateExtension == extension) {
                    return mime;
                }
            }
            return std::nullopt;
        }

        StaticAssetResolution staticDisposition(StaticAssetDisposition disposition) {
            return {
                .disposition = disposition,
                .mimeType = {},
                .sendBody = false,
                .contentLength = 0,
                .descriptor = {},
            };
        }

        std::optional<StaticAssetDescriptor> openStaticAsset(int rootDescriptor, const std::filesystem::path& relative) noexcept {
            StaticAssetDescriptor current(::fcntl(rootDescriptor, F_DUPFD_CLOEXEC, 0));
            if (current.get() < 0) {
                return std::nullopt;
            }

            for (auto component = relative.begin(); component != relative.end(); ++component) {
                const bool final = std::next(component) == relative.end();
                const std::string name = component->string();
                if (name.empty() || name == "." || name == "..") {
                    return std::nullopt;
                }
                const int flags = final ? O_RDONLY | O_CLOEXEC | O_NOFOLLOW : O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
                StaticAssetDescriptor next(::openat(current.get(), name.c_str(), flags));
                if (next.get() < 0) {
                    return std::nullopt;
                }
                current = std::move(next);
            }
            return current;
        }

    } // namespace

    StaticAssetDescriptor::StaticAssetDescriptor(int descriptor) noexcept
        : descriptor(descriptor) {
    }

    StaticAssetDescriptor::StaticAssetDescriptor(StaticAssetDescriptor&& other) noexcept
        : descriptor(std::exchange(other.descriptor, -1)) {
    }

    StaticAssetDescriptor& StaticAssetDescriptor::operator=(StaticAssetDescriptor&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.descriptor, -1));
        }
        return *this;
    }

    StaticAssetDescriptor::~StaticAssetDescriptor() {
        reset();
    }

    bool StaticAssetDescriptor::isOpen() const noexcept {
        return descriptor >= 0;
    }

    int StaticAssetDescriptor::get() const noexcept {
        return descriptor;
    }

    int StaticAssetDescriptor::release() noexcept {
        return std::exchange(descriptor, -1);
    }

    void StaticAssetDescriptor::reset(int replacement) noexcept {
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
        }
        descriptor = replacement;
    }

    bool normalizedFrontendWebSocketEndpoint(std::string_view endpoint) noexcept {
        if (endpoint.size() < 2 || endpoint.size() > MaximumFrontendWebSocketEndpointBytes || endpoint.front() != '/' ||
            endpoint.back() == '/' || endpoint.find_first_of("?#\\%") != std::string_view::npos ||
            endpoint.find('\0') != std::string_view::npos || endpoint.find("//") != std::string_view::npos) {
            return false;
        }
        std::size_t begin = 1;
        while (begin < endpoint.size()) {
            const std::size_t end = endpoint.find('/', begin);
            const std::string_view component =
                endpoint.substr(begin, end == std::string_view::npos ? endpoint.size() - begin : end - begin);
            if (component.empty() || component == "." || component == "..") {
                return false;
            }
            if (end == std::string_view::npos) {
                break;
            }
            begin = end + 1;
        }
        return true;
    }

    std::string NormalizedWebOrigin::serialize() const {
        const bool defaultPort = (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
        std::string result = scheme + "://";
        if (ipv6) {
            result += '[';
        }
        result += host;
        if (ipv6) {
            result += ']';
        }
        if (!defaultPort) {
            result += ':';
            result += std::to_string(port);
        }
        return result;
    }

    std::optional<NormalizedWebOrigin> normalizeWebOrigin(std::string_view origin) noexcept {
        try {
            if (origin.empty() || hasForbiddenOriginByte(origin) || origin.find_first_of("@?#\\*") != std::string_view::npos) {
                return std::nullopt;
            }

            const std::size_t delimiter = origin.find("://");
            if (delimiter == std::string_view::npos || origin.find("://", delimiter + 3) != std::string_view::npos) {
                return std::nullopt;
            }
            std::string scheme(origin.substr(0, delimiter));
            std::ranges::transform(scheme, scheme.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            if (scheme != "http" && scheme != "https") {
                return std::nullopt;
            }

            const std::string_view authority = origin.substr(delimiter + 3);
            if (authority.empty() || authority.find_first_of("/@?#\\*") != std::string_view::npos) {
                return std::nullopt;
            }

            std::string_view host;
            std::optional<std::uint16_t> port;
            bool bracketed = false;
            if (authority.front() == '[') {
                bracketed = true;
                const std::size_t close = authority.find(']');
                if (close == std::string_view::npos || close == 1) {
                    return std::nullopt;
                }
                host = authority.substr(1, close - 1);
                const std::string_view remainder = authority.substr(close + 1);
                if (!remainder.empty()) {
                    if (remainder.front() != ':') {
                        return std::nullopt;
                    }
                    port = parsePort(remainder.substr(1));
                    if (!port.has_value()) {
                        return std::nullopt;
                    }
                }
            } else {
                const std::size_t colon = authority.rfind(':');
                if (colon != std::string_view::npos) {
                    host = authority.substr(0, colon);
                    port = parsePort(authority.substr(colon + 1));
                    if (!port.has_value()) {
                        return std::nullopt;
                    }
                } else {
                    host = authority;
                }
            }

            const auto normalizedHost = normalizeHost(host, bracketed);
            if (!normalizedHost.has_value()) {
                return std::nullopt;
            }
            return NormalizedWebOrigin{
                .scheme = scheme,
                .host = normalizedHost->first,
                .port = port.value_or(scheme == "https" ? 443 : 80),
                .ipv6 = normalizedHost->second,
            };
        } catch (...) {
            return std::nullopt;
        }
    }

    WebOriginPolicy::WebOriginPolicy(std::vector<std::string> allowedOrigins) {
        allowed.reserve(allowedOrigins.size());
        for (const std::string& origin : allowedOrigins) {
            const auto normalized = normalizeWebOrigin(origin);
            if (!normalized.has_value()) {
                throw std::invalid_argument("frontend Origin allow-list contains an invalid origin");
            }
            allowed.push_back(*normalized);
        }
        std::ranges::sort(allowed);
        allowed.erase(std::unique(allowed.begin(), allowed.end()), allowed.end());
    }

    WebOriginAdmission WebOriginPolicy::evaluate(std::optional<std::string_view> origin,
                                                 const NormalizedWebOrigin& requestOrigin,
                                                 bool encrypted,
                                                 bool loopback) const noexcept {
        if (!origin.has_value()) {
            return WebOriginAdmission::Accepted;
        }
        if (!encrypted && !loopback) {
            return WebOriginAdmission::TransportSecurityRequired;
        }
        const auto normalized = normalizeWebOrigin(*origin);
        if (!normalized.has_value()) {
            return WebOriginAdmission::OriginRejected;
        }
        if (*normalized == requestOrigin || std::ranges::binary_search(allowed, *normalized)) {
            return WebOriginAdmission::Accepted;
        }
        return WebOriginAdmission::OriginRejected;
    }

    const std::vector<NormalizedWebOrigin>& WebOriginPolicy::allowedOrigins() const noexcept {
        return allowed;
    }

    StaticAssetPolicy::StaticAssetPolicy(std::optional<std::filesystem::path> root, std::size_t maximumAssetBytes)
        : maximumAssetBytes(std::min(maximumAssetBytes, MaximumAssetBytes)) {
        if (!root.has_value()) {
            return;
        }
        if (root->empty() || !root->is_absolute()) {
            throw std::invalid_argument("static asset root must be absolute");
        }
        std::error_code error;
        std::filesystem::path canonical = std::filesystem::canonical(*root, error);
        if (error || canonical.empty() || !std::filesystem::is_directory(canonical, error) || error) {
            throw std::invalid_argument("static asset root must identify an existing directory");
        }
        canonical = canonical.lexically_normal();
        if (canonical == canonical.root_path()) {
            throw std::invalid_argument("static asset root may not grant an entire filesystem root");
        }
        canonicalRoot = std::move(canonical);
        rootDescriptor = ::open(canonicalRoot->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (rootDescriptor < 0) {
            throw std::invalid_argument("static asset root could not be retained safely");
        }
    }

    StaticAssetPolicy::~StaticAssetPolicy() {
        if (rootDescriptor >= 0) {
            static_cast<void>(::close(rootDescriptor));
        }
    }

    StaticAssetResolution StaticAssetPolicy::resolve(std::string_view method, std::string_view requestTarget) const noexcept {
        try {
            if (method != "GET" && method != "HEAD") {
                return staticDisposition(StaticAssetDisposition::MethodNotAllowed);
            }
            if (!canonicalRoot.has_value() || rootDescriptor < 0) {
                return staticDisposition(StaticAssetDisposition::NotFound);
            }
            if (requestTarget.empty() || requestTarget.size() > MaximumRequestTargetBytes ||
                requestTarget.find('\0') != std::string_view::npos || requestTarget.find('#') != std::string_view::npos ||
                std::ranges::any_of(requestTarget, [](unsigned char character) {
                    return character < 0x20 || character == 0x7f;
                })) {
                return staticDisposition(StaticAssetDisposition::Rejected);
            }

            const std::string_view rawPath = requestTarget.substr(0, requestTarget.find('?'));
            const std::optional<std::string> decoded = decodeRequestPath(rawPath);
            if (!decoded.has_value() || !validDecodedPath(*decoded)) {
                return staticDisposition(StaticAssetDisposition::Rejected);
            }

            const std::filesystem::path relative = std::filesystem::path(*decoded).relative_path();
            std::optional<StaticAssetDescriptor> descriptor = openStaticAsset(rootDescriptor, relative);
            if (!descriptor.has_value()) {
                return staticDisposition(StaticAssetDisposition::NotFound);
            }
            struct stat metadata{};
            if (::fstat(descriptor->get(), &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
                return staticDisposition(StaticAssetDisposition::NotFound);
            }
            if (metadata.st_size < 0 || static_cast<std::uintmax_t>(metadata.st_size) > maximumAssetBytes) {
                return staticDisposition(StaticAssetDisposition::Rejected);
            }
            const auto mime = safeMimeType(relative);
            if (!mime.has_value()) {
                return staticDisposition(StaticAssetDisposition::UnsupportedMediaType);
            }

            const std::size_t length = static_cast<std::size_t>(metadata.st_size);
            return {
                .disposition = StaticAssetDisposition::Serve,
                .mimeType = *mime,
                .sendBody = method == "GET",
                .contentLength = length,
                .descriptor = std::move(*descriptor),
            };
        } catch (...) {
            return staticDisposition(StaticAssetDisposition::Rejected);
        }
    }

    const std::optional<std::filesystem::path>& StaticAssetPolicy::root() const noexcept {
        return canonicalRoot;
    }

    std::span<const WebSecurityHeader> staticAssetSecurityHeaders() noexcept {
        return SecurityHeaders;
    }

} // namespace apps::codex_backend
