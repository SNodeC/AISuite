/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/FrontendWebApplication.h"

#include "apps/codex-backend/FrontendRuntimeBridge.h"
#include "apps/codex-backend/FrontendWebSecurity.h"
#include "core/file/FileReader.h"
#include "core/socket/stream/SocketConnection.h"
#include "express/Request.h"
#include "express/Response.h"
#include "express/Router.h"
#include "web/http/server/SocketContext.h"

#include <arpa/inet.h>
#include <array>
#include <cstdint>
#include <memory>
#include <netinet/in.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <utility>

namespace apps::codex_backend {

    namespace {

        using ai::openai::codex::frontend::AuthenticationFailureCode;
        using ai::openai::codex::frontend::FrontendPeerContext;
        using ai::openai::codex::frontend::FrontendTransportKind;

        struct NumericPeer {
            std::string host;
            bool loopback = false;
        };

        std::optional<NumericPeer> numericPeerFromFileDescriptor(int descriptor) noexcept {
            if (descriptor < 0) {
                return std::nullopt;
            }
            sockaddr_storage address{};
            socklen_t length = sizeof(address);
            if (::getpeername(descriptor, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
                return std::nullopt;
            }
            std::array<char, INET6_ADDRSTRLEN> text{};
            if (address.ss_family == AF_INET && length >= sizeof(sockaddr_in)) {
                const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
                if (::inet_ntop(AF_INET, &ipv4->sin_addr, text.data(), text.size()) == nullptr) {
                    return std::nullopt;
                }
                const std::uint32_t hostOrder = ntohl(ipv4->sin_addr.s_addr);
                return NumericPeer{std::string(text.data()), (hostOrder & 0xff000000U) == 0x7f000000U};
            }
            if (address.ss_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
                const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
                if (::inet_ntop(AF_INET6, &ipv6->sin6_addr, text.data(), text.size()) == nullptr) {
                    return std::nullopt;
                }
                const bool mappedIpv4Loopback = IN6_IS_ADDR_V4MAPPED(&ipv6->sin6_addr) != 0 && ipv6->sin6_addr.s6_addr[12] == 127;
                return NumericPeer{std::string(text.data()), IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr) != 0 || mappedIpv4Loopback};
            }
            return std::nullopt;
        }

        std::optional<NormalizedWebOrigin> requestOrigin(const express::Request& request, bool encrypted) noexcept {
            try {
                const std::string& host = request.get("host");
                if (host.empty() || host.size() > 512) {
                    return std::nullopt;
                }
                return normalizeWebOrigin(std::string(encrypted ? "https://" : "http://") + host);
            } catch (...) {
                return std::nullopt;
            }
        }

        core::socket::stream::SocketConnection* socketConnection(const std::shared_ptr<express::Response>& response) noexcept {
            try {
                if (!response || response->getSocketContext() == nullptr) {
                    return nullptr;
                }
                return response->getSocketContext()->getSocketConnection();
            } catch (...) {
                return nullptr;
            }
        }

        void applySecurityHeaders(const std::shared_ptr<express::Response>& response) noexcept {
            try {
                for (const WebSecurityHeader& header : staticAssetSecurityHeaders()) {
                    response->set(std::string(header.name), std::string(header.value));
                }
            } catch (...) {
            }
        }

        void sendBounded(const std::shared_ptr<express::Response>& response,
                         int status,
                         std::string_view body,
                         std::optional<std::string_view> allow = std::nullopt) noexcept {
            try {
                applySecurityHeaders(response);
                response->set("Cache-Control", "no-store");
                response->set("Content-Type", "text/plain; charset=utf-8");
                response->set("Connection", "close");
                if (allow.has_value()) {
                    response->set("Allow", std::string(*allow));
                }
                response->status(status).send(std::string(body));
            } catch (...) {
                try {
                    if (response && response->isConnected()) {
                        response->end();
                    }
                } catch (...) {
                }
            }
        }

        bool rejectRequestBody(const express::Request& request, const std::shared_ptr<express::Response>& response) noexcept {
            if (request.body.empty()) {
                return false;
            }
            sendBounded(response, 400, "request_body_rejected");
            return true;
        }

        bool hasForbiddenCredentialChannel(const express::Request& request) noexcept {
            return !request.queries.empty() || request.url.find('?') != std::string::npos || !request.cookies.empty() ||
                   !request.get("cookie").empty() || !request.get("authorization").empty() || !request.get("proxy-authorization").empty();
        }

        struct StopFileReader {
            void operator()(core::file::FileReader* reader) const noexcept {
                if (reader != nullptr) {
                    try {
                        reader->stop();
                    } catch (...) {
                    }
                }
            }
        };

        using PendingFileReader = std::unique_ptr<core::file::FileReader, StopFileReader>;

        PendingFileReader adoptStaticAsset(StaticAssetDescriptor& descriptor) {
            return PendingFileReader(core::file::FileReader::adopt(descriptor.release()));
        }

    } // namespace

    FrontendPeerContext frontendWebPeerContextFromFileDescriptor(int descriptor, FrontendTransportKind transport, bool encrypted) noexcept {
        FrontendPeerContext peer;
        peer.transport = transport;
        peer.encrypted = encrypted;
        if (const auto numeric = numericPeerFromFileDescriptor(descriptor)) {
            peer.remoteAddress = numeric->host;
            peer.loopback = numeric->loopback;
        }
        return peer;
    }

    class FrontendWebApplication::State {
    public:
        State(ai::openai::codex::frontend::FrontendService& service, FrontendWebApplicationOptions options)
            : service(service)
            , options(std::move(options))
            , originPolicy(this->options.allowedOrigins)
            , staticPolicy(this->options.staticRoot) {
            if (!normalizedFrontendWebSocketEndpoint(this->options.endpoint)) {
                throw std::invalid_argument("frontend WebSocket endpoint must be one normalized absolute path");
            }
            const bool websocket = this->options.transport == FrontendTransportKind::WebSocket;
            const bool websocketTls = this->options.transport == FrontendTransportKind::WebSocketTls;
            if ((!websocket && !websocketTls) || websocketTls != this->options.encrypted) {
                throw std::invalid_argument("frontend WebSocket transport and encryption settings disagree");
            }
        }

        void handleUpgrade(const std::shared_ptr<express::Request>& request, const std::shared_ptr<express::Response>& response) noexcept {
            core::socket::stream::SocketConnection* connection = socketConnection(response);
            if (!request || connection == nullptr) {
                sendBounded(response, 500, "websocket_upgrade_unavailable");
                return;
            }
            if (request->path != options.endpoint) {
                sendBounded(response, 404, "not_found");
                return;
            }
            if (request->method != "GET") {
                sendBounded(response, 405, "websocket_get_required", "GET");
                return;
            }
            if (rejectRequestBody(*request, response)) {
                return;
            }
            if (hasForbiddenCredentialChannel(*request)) {
                sendBounded(response, 400, "websocket_credential_channel_rejected");
                return;
            }
            if (request->get("sec-websocket-protocol") != FrontendWebSocketSubProtocol) {
                sendBounded(response, 400, "websocket_subprotocol_rejected");
                return;
            }

            FrontendPeerContext peer = frontendWebPeerContextFromFileDescriptor(connection->getFd(), options.transport, options.encrypted);
            std::optional<std::string_view> origin;
            if (const auto found = request->headers.find("origin"); found != request->headers.end()) {
                if (found->second.empty()) {
                    sendAdmissionFailure(response, peer, AuthenticationFailureCode::OriginRejected, 403);
                    return;
                }
                origin = found->second;
                peer.origin = found->second;
            }
            const std::optional<NormalizedWebOrigin> currentOrigin = requestOrigin(*request, options.encrypted);
            if (!currentOrigin.has_value()) {
                sendBounded(response, origin.has_value() ? 403 : 400, origin.has_value() ? "origin_rejected" : "invalid_websocket_upgrade");
                return;
            }
            switch (originPolicy.evaluate(origin, *currentOrigin, options.encrypted, peer.loopback)) {
                case WebOriginAdmission::Accepted:
                    break;
                case WebOriginAdmission::OriginRejected:
                    sendAdmissionFailure(response, peer, AuthenticationFailureCode::OriginRejected, 403);
                    return;
                case WebOriginAdmission::TransportSecurityRequired:
                    sendAdmissionFailure(response, peer, AuthenticationFailureCode::TransportSecurityRequired, 426);
                    return;
            }

            if (!prepareFrontendWebSocket(*connection, std::move(peer))) {
                sendBounded(response, 503, "capacity_exceeded");
                return;
            }
            try {
                response->upgrade(request, [response, connection](const std::string& upgrade) {
                    if (upgrade.empty()) {
                        cancelFrontendWebSocket(*connection);
                        sendBounded(response, 400, "invalid_websocket_upgrade");
                    } else {
                        response->end();
                    }
                });
            } catch (...) {
                cancelFrontendWebSocket(*connection);
                sendBounded(response, 500, "websocket_upgrade_unavailable");
            }
        }

        void sendAdmissionFailure(const std::shared_ptr<express::Response>& response,
                                  const FrontendPeerContext& peer,
                                  AuthenticationFailureCode failure,
                                  int status) noexcept {
            const AuthenticationFailureCode admitted = service.recordPreAuthenticationFailure(peer, failure);
            sendBounded(response,
                        admitted == AuthenticationFailureCode::RateLimited ? 429 : status,
                        ai::openai::codex::frontend::toString(admitted));
        }

        void handleStatic(const std::shared_ptr<express::Request>& request, const std::shared_ptr<express::Response>& response) noexcept {
            if (!request || !response) {
                return;
            }
            applySecurityHeaders(response);
            if (rejectRequestBody(*request, response)) {
                return;
            }
            StaticAssetResolution asset = staticPolicy.resolve(request->method, request->url);
            switch (asset.disposition) {
                case StaticAssetDisposition::Serve:
                    try {
                        response->status(200);
                        response->set("Content-Type", std::string(asset.mimeType));
                        response->set("Cache-Control", "no-cache");
                        response->set("Connection", "close");
                        response->set("Content-Length", std::to_string(asset.contentLength));
                        if (!asset.sendBody) {
                            response->send(nullptr, asset.contentLength);
                            return;
                        }
                        PendingFileReader reader = adoptStaticAsset(asset.descriptor);
                        if (!reader) {
                            sendBounded(response, 503, "capacity_exceeded");
                            return;
                        }
                        if (!response->pipe(reader.get())) {
                            reader.reset();
                            sendBounded(response, 503, "capacity_exceeded");
                            return;
                        }
                        static_cast<void>(reader.release());
                    } catch (...) {
                        sendBounded(response, 503, "capacity_exceeded");
                    }
                    return;
                case StaticAssetDisposition::NotFound:
                    sendBounded(response, 404, "not_found");
                    return;
                case StaticAssetDisposition::MethodNotAllowed:
                    sendBounded(response, 405, "method_not_allowed", "GET, HEAD");
                    return;
                case StaticAssetDisposition::Rejected:
                    sendBounded(response, 404, "not_found");
                    return;
                case StaticAssetDisposition::UnsupportedMediaType:
                    sendBounded(response, 415, "unsupported_media_type");
                    return;
            }
        }

        ai::openai::codex::frontend::FrontendService& service;
        FrontendWebApplicationOptions options;
        WebOriginPolicy originPolicy;
        StaticAssetPolicy staticPolicy;
    };

    FrontendWebApplication::FrontendWebApplication(ai::openai::codex::frontend::FrontendService& service,
                                                   FrontendWebApplicationOptions options)
        : state(std::make_shared<State>(service, std::move(options))) {
    }

    void FrontendWebApplication::configure(express::Router& router) const {
        const std::shared_ptr<State> retained = state;
        router.all(state->options.endpoint, [retained] APPLICATION(request, response) {
            retained->handleUpgrade(request, response);
        });
        router.use([retained] APPLICATION(request, response) {
            retained->handleStatic(request, response);
        });
    }

    ai::openai::codex::frontend::FrontendService* FrontendWebApplication::serviceIdentity() const noexcept {
        return state ? &state->service : nullptr;
    }

} // namespace apps::codex_backend
