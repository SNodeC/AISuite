/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/FrontendWebSecurity.h"
#include "support/TestResult.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>

namespace {

    namespace app = apps::codex_backend;

    template <typename T>
    concept CarriesCompleteStaticBody = requires(T value) { value.content; };

    static_assert(!CarriesCompleteStaticBody<app::StaticAssetResolution>);

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            std::string value = (std::filesystem::temp_directory_path() / "aisuite-web-security-XXXXXX").string();
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

    void writeFile(const std::filesystem::path& path, std::string_view content) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    std::string readDescriptorAtOffsetZero(int descriptor, std::size_t length) {
        std::string content(length, '\0');
        std::size_t offset = 0;
        while (offset < content.size()) {
            const ssize_t count = ::pread(descriptor, content.data() + offset, content.size() - offset, static_cast<off_t>(offset));
            if (count <= 0) {
                return {};
            }
            offset += static_cast<std::size_t>(count);
        }
        return content;
    }

    bool originConstructorRejects(std::vector<std::string> origins) {
        try {
            const app::WebOriginPolicy policy(std::move(origins));
            (void) policy;
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    }

    bool staticRootConstructorRejects(const std::filesystem::path& root) {
        try {
            const app::StaticAssetPolicy policy(root);
            (void) policy;
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    }

} // namespace

int main() {
    tests::support::TestResult result;

    const auto httpsOrigin = app::normalizeWebOrigin("HTTPS://Example.COM");
    const auto explicitHttpsOrigin = app::normalizeWebOrigin("https://example.com:443");
    const app::NormalizedWebOrigin requestOrigin = httpsOrigin.value_or(app::NormalizedWebOrigin{
        .scheme = "https",
        .host = "example.com",
        .port = 443,
        .ipv6 = false,
    });
    result.expectTrue(httpsOrigin && explicitHttpsOrigin && *httpsOrigin == *explicitHttpsOrigin && httpsOrigin->scheme == "https" &&
                          httpsOrigin->host == "example.com" && httpsOrigin->port == 443 &&
                          httpsOrigin->serialize() == "https://example.com",
                      "Origin normalization lowercases scheme and host and freezes the effective HTTPS port");

    const auto ipv4 = app::normalizeWebOrigin("http://127.0.0.1:8080");
    const auto ipv6 = app::normalizeWebOrigin("https://[0:0:0:0:0:0:0:1]:8443");
    result.expectTrue(ipv4 && ipv4->host == "127.0.0.1" && !ipv4->ipv6 && ipv4->serialize() == "http://127.0.0.1:8080" && ipv6 &&
                          ipv6->host == "::1" && ipv6->ipv6 && ipv6->serialize() == "https://[::1]:8443",
                      "numeric IPv4 and bracketed IPv6 origins have canonical host and port forms");

    const std::string nulOrigin{"https://example.com\0.attacker", 29};
    result.expectTrue(
        !app::normalizeWebOrigin("ftp://example.com") && !app::normalizeWebOrigin("https://user@example.com") &&
            !app::normalizeWebOrigin("https://example.com/") && !app::normalizeWebOrigin("https://example.com/path") &&
            !app::normalizeWebOrigin("https://example.com?query") && !app::normalizeWebOrigin("https://example.com#fragment") &&
            !app::normalizeWebOrigin("https://*.example.com") && !app::normalizeWebOrigin("https://999.1.1.1") &&
            !app::normalizeWebOrigin("https://example.com:0") && !app::normalizeWebOrigin("https://example.com:65536") &&
            !app::normalizeWebOrigin("https://bad_host.example") && !app::normalizeWebOrigin("https://example.com\n") &&
            !app::normalizeWebOrigin(nulOrigin),
        "unsupported schemes, credentials, paths, queries, fragments, wildcards, malformed hosts, ports, and control bytes fail closed");

    const app::WebOriginPolicy originPolicy({"https://console.example:8443", "HTTPS://CONSOLE.EXAMPLE:8443"});
    result.expectTrue(originPolicy.allowedOrigins().size() == 1,
                      "the configured Origin allow-list is normalized and deduplicated during construction");
    result.expectTrue(originPolicy.evaluate("https://service.example", requestOrigin, true, false) ==
                          app::WebOriginAdmission::OriginRejected,
                      "an encrypted browser request with an unrelated Origin is rejected");
    result.expectTrue(originPolicy.evaluate("HTTPS://EXAMPLE.COM:443", requestOrigin, true, false) == app::WebOriginAdmission::Accepted,
                      "the normalized same Origin is accepted by default");
    result.expectTrue(originPolicy.evaluate("https://console.example:8443", requestOrigin, true, false) ==
                          app::WebOriginAdmission::Accepted,
                      "an explicitly configured normalized cross-Origin is accepted");
    result.expectTrue(originPolicy.evaluate(std::nullopt, requestOrigin, false, false) == app::WebOriginAdmission::Accepted,
                      "a native client without Origin may continue to the mandatory Hello bearer path");
    result.expectTrue(originPolicy.evaluate("http://example.com", requestOrigin, false, false) ==
                              app::WebOriginAdmission::TransportSecurityRequired &&
                          originPolicy.evaluate("https://example.com", requestOrigin, false, true) == app::WebOriginAdmission::Accepted,
                      "a browser Origin on non-loopback plaintext is rejected while loopback plaintext remains eligible for Origin policy");
    result.expectTrue(originConstructorRejects({"*"}) && originConstructorRejects({"https://example.com/path"}),
                      "wildcard and URL-shaped allow-list entries cannot weaken the exact Origin policy");

    TemporaryDirectory temporary;
    result.expectTrue(!temporary.get().empty(), "an isolated static-root fixture is created");
    const std::filesystem::path root = temporary.get() / "public";
    const std::filesystem::path outside = temporary.get() / "outside";
    std::error_code filesystemError;
    std::filesystem::create_directories(root / "assets", filesystemError);
    std::filesystem::create_directories(root / "directory", filesystemError);
    std::filesystem::create_directories(outside, filesystemError);
    writeFile(root / "index.html", "<!doctype html>");
    writeFile(root / "assets" / "app.js", "export {};");
    writeFile(root / "assets" / "data.unknown", "opaque");
    writeFile(outside / "secret.txt", "not public");
    filesystemError.clear();
    std::filesystem::create_symlink(outside / "secret.txt", root / "assets" / "escape.txt", filesystemError);
    result.expectTrue(!filesystemError && std::filesystem::is_symlink(root / "assets" / "escape.txt"),
                      "the static symlink-escape fixture is created");

    const app::StaticAssetPolicy staticPolicy(root);
    const app::StaticAssetResolution get = staticPolicy.resolve("GET", "/assets/app.js?version=1");
    const app::StaticAssetResolution head = staticPolicy.resolve("HEAD", "/assets/app.js");
    result.expectTrue(get.disposition == app::StaticAssetDisposition::Serve && get.mimeType == "text/javascript; charset=utf-8" &&
                          get.sendBody && get.descriptor.isOpen() && get.contentLength == 10 &&
                          ::lseek(get.descriptor.get(), 0, SEEK_CUR) == 0 && head.disposition == app::StaticAssetDisposition::Serve &&
                          !head.sendBody && head.descriptor.isOpen() && head.contentLength == 10 &&
                          ::lseek(head.descriptor.get(), 0, SEEK_CUR) == 0,
                      "GET and HEAD retain authorized descriptors and exact metadata without reading a representation body");
    const app::StaticAssetPolicy boundedStaticPolicy(root, 9);
    result.expectTrue(boundedStaticPolicy.resolve("GET", "/assets/app.js").disposition == app::StaticAssetDisposition::Rejected,
                      "a static representation larger than the configured maximum asset size is rejected before streaming");
    result.expectTrue(staticPolicy.resolve("POST", "/index.html").disposition == app::StaticAssetDisposition::MethodNotAllowed,
                      "only GET and HEAD are admitted for static assets");
    result.expectTrue(staticPolicy.resolve("GET", "/missing.js").disposition == app::StaticAssetDisposition::NotFound &&
                          staticPolicy.resolve("GET", "/directory").disposition == app::StaticAssetDisposition::NotFound,
                      "missing files and directories do not produce directory listings");
    result.expectTrue(staticPolicy.resolve("GET", "/assets/data.unknown").disposition == app::StaticAssetDisposition::UnsupportedMediaType,
                      "unknown file extensions are rejected instead of receiving a MIME fallback");

    const std::string nulPath{"/assets/app.js\0.txt", 19};
    result.expectTrue(
        staticPolicy.resolve("GET", "/../outside/secret.txt").disposition == app::StaticAssetDisposition::Rejected &&
            staticPolicy.resolve("GET", "/%2e%2e/outside/secret.txt").disposition == app::StaticAssetDisposition::Rejected &&
            staticPolicy.resolve("GET", "/%252e%252e/outside/secret.txt").disposition == app::StaticAssetDisposition::Rejected &&
            staticPolicy.resolve("GET", "/%25252e%25252e/outside/secret.txt").disposition == app::StaticAssetDisposition::Rejected &&
            staticPolicy.resolve("GET", "/assets%5capp.js").disposition == app::StaticAssetDisposition::Rejected &&
            staticPolicy.resolve("GET", "/assets/%00app.js").disposition == app::StaticAssetDisposition::Rejected &&
            staticPolicy.resolve("GET", "/assets/%zzapp.js").disposition == app::StaticAssetDisposition::Rejected &&
            staticPolicy.resolve("GET", nulPath).disposition == app::StaticAssetDisposition::Rejected,
        "raw, encoded, double-encoded, and nested traversal plus backslash, NUL, and malformed escapes fail closed");
    result.expectTrue(staticPolicy.resolve("GET", "/assets/escape.txt").disposition != app::StaticAssetDisposition::Serve,
                      "component-by-component no-follow opening rejects a symlink escape without a check/open race");

    const std::filesystem::path retainedRoot = temporary.get() / "retained-public";
    std::filesystem::rename(root, retainedRoot, filesystemError);
    std::filesystem::create_directories(root / "assets", filesystemError);
    writeFile(root / "assets" / "app.js", "attacker replacement");
    const app::StaticAssetResolution retained = staticPolicy.resolve("GET", "/assets/app.js");
    result.expectTrue(retained.disposition == app::StaticAssetDisposition::Serve && retained.descriptor.isOpen() &&
                          readDescriptorAtOffsetZero(retained.descriptor.get(), retained.contentLength) == "export {};",
                      "the policy retains the verified root descriptor so an ancestor-path replacement cannot redirect later opens");

    const app::StaticAssetPolicy disabledStaticPolicy;
    result.expectTrue(disabledStaticPolicy.resolve("GET", "/index.html").disposition == app::StaticAssetDisposition::NotFound,
                      "an omitted static root keeps ordinary static requests disabled");
    result.expectTrue(staticRootConstructorRejects(std::filesystem::path{"relative"}) &&
                          staticRootConstructorRejects(std::filesystem::path{"/"}) &&
                          staticRootConstructorRejects(temporary.get() / "missing"),
                      "relative, filesystem-wide, and missing static roots cannot create an unsafe policy");

    const auto headers = app::staticAssetSecurityHeaders();
    const auto containsHeader = [&headers](std::string_view name, std::string_view requiredValue) {
        return std::ranges::any_of(headers, [name, requiredValue](const app::WebSecurityHeader& header) {
            return header.name == name && header.value.find(requiredValue) != std::string_view::npos;
        });
    };
    result.expectTrue(headers.size() == 3 && containsHeader("Content-Security-Policy", "frame-ancestors 'none'") &&
                          containsHeader("Content-Security-Policy", "script-src 'self'") &&
                          containsHeader("X-Content-Type-Options", "nosniff") && containsHeader("Referrer-Policy", "no-referrer"),
                      "static responses carry the mandatory framing, executable, MIME-sniffing, and referrer protections");

    return result.processResult();
}
