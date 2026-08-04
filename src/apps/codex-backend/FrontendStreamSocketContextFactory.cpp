/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/FrontendStreamSocketContextFactory.h"

#include "apps/codex-backend/FrontendStreamSocketContext.h"
#include "core/socket/SocketAddress.h"
#include "core/socket/stream/SocketConnection.h"

#include <arpa/inet.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <utility>

namespace apps::codex_backend {

    namespace {

        bool ipv4Loopback(const in_addr& address) noexcept {
            return (ntohl(address.s_addr) & 0xFF000000U) == 0x7F000000U;
        }

        bool ipv4MappedLoopback(const in6_addr& address) noexcept {
            constexpr std::array<std::uint8_t, 12> Prefix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
            if (std::memcmp(address.s6_addr, Prefix.data(), Prefix.size()) != 0) {
                return false;
            }
            in_addr mapped{};
            std::memcpy(&mapped, address.s6_addr + Prefix.size(), sizeof(mapped));
            return ipv4Loopback(mapped);
        }

        std::string ipv4Address(const sockaddr_in& address) {
            std::array<char, INET_ADDRSTRLEN> text{};
            if (::inet_ntop(AF_INET, &address.sin_addr, text.data(), static_cast<socklen_t>(text.size())) == nullptr) {
                return {};
            }
            // Admission accounting is per peer address, not per ephemeral
            // source port. A reconnect therefore cannot reset the failed-auth
            // budget by selecting another port.
            return std::string(text.data());
        }

        std::string ipv6Address(const sockaddr_in6& address) {
            std::array<char, INET6_ADDRSTRLEN> text{};
            if (::inet_ntop(AF_INET6, &address.sin6_addr, text.data(), static_cast<socklen_t>(text.size())) == nullptr) {
                return {};
            }
            return std::string(text.data());
        }

    } // namespace

    ai::openai::codex::frontend::FrontendPeerContext
    streamPeerContextFromFileDescriptor(int descriptor, ai::openai::codex::frontend::FrontendTransportKind transport) noexcept {
        ai::openai::codex::frontend::FrontendPeerContext peer;
        peer.transport = transport;
        peer.encrypted = isEncryptedTransport(transport);

        if (descriptor < 0) {
            return peer;
        }

        sockaddr_storage address{};
        socklen_t size = sizeof(address);
        if (::getpeername(descriptor, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            return peer;
        }

        try {
            if (address.ss_family == AF_INET && size >= sizeof(sockaddr_in)) {
                const auto& ipv4 = reinterpret_cast<const sockaddr_in&>(address);
                peer.remoteAddress = ipv4Address(ipv4);
                peer.loopback = ipv4Loopback(ipv4.sin_addr);
            } else if (address.ss_family == AF_INET6 && size >= sizeof(sockaddr_in6)) {
                const auto& ipv6 = reinterpret_cast<const sockaddr_in6&>(address);
                peer.remoteAddress = ipv6Address(ipv6);
                peer.loopback = IN6_IS_ADDR_LOOPBACK(&ipv6.sin6_addr) != 0 || ipv4MappedLoopback(ipv6.sin6_addr);
            }
        } catch (...) {
            peer.remoteAddress.reset();
            peer.loopback = false;
        }
        return peer;
    }

    FrontendStreamSocketContextFactory::FrontendStreamSocketContextFactory(ai::openai::codex::frontend::FrontendService& service,
                                                                           FrontendStreamSocketContextFactoryOptions options)
        : service(service)
        , options(std::move(options)) {
        if (!isJsonLineStreamTransport(this->options.transport)) {
            throw std::invalid_argument("the selected frontend transport does not use JSONL stream framing");
        }
    }

    core::socket::stream::SocketContext*
    FrontendStreamSocketContextFactory::create(core::socket::stream::SocketConnection* socketConnection) {
        if (socketConnection == nullptr) {
            throw std::invalid_argument("a frontend stream requires an accepted socket connection");
        }

        ai::openai::codex::frontend::FrontendPeerContext peer;
        if (options.resolvePeer) {
            try {
                peer = options.resolvePeer(*socketConnection);
            } catch (...) {
                // Peer extraction is admission metadata, not a reason to let
                // an application callback unwind into SNode.C. An empty peer
                // remains untrusted and therefore requires authentication.
                peer = {};
            }
            // The listener family is authoritative. A custom resolver may add
            // verified identity/address fields but cannot relabel transport or
            // claim encryption for a plaintext listener.
            peer.transport = options.transport;
            peer.encrypted = isEncryptedTransport(options.transport);
            if (options.transport != ai::openai::codex::frontend::FrontendTransportKind::Unix) {
                peer.localPeer = false;
                peer.unixUserId.reset();
            }
        } else {
            peer = streamPeerContextFromFileDescriptor(socketConnection->getFd(), options.transport);
            if (!peer.remoteAddress.has_value()) {
                try {
                    const std::string remoteAddress = socketConnection->getRemoteAddress().toString();
                    if (!remoteAddress.empty() && remoteAddress.size() <= 256) {
                        peer.remoteAddress = remoteAddress;
                    }
                } catch (...) {
                }
            }
        }
        if (peer.remoteAddress && peer.remoteAddress->size() > 256) {
            peer.remoteAddress.reset();
        }

        return new FrontendStreamSocketContext(socketConnection, service, std::move(peer), options.socket);
    }

    ai::openai::codex::frontend::FrontendService* FrontendStreamSocketContextFactory::serviceIdentity() const noexcept {
        return std::addressof(service);
    }

    ai::openai::codex::frontend::FrontendTransportKind FrontendStreamSocketContextFactory::transport() const noexcept {
        return options.transport;
    }

} // namespace apps::codex_backend
