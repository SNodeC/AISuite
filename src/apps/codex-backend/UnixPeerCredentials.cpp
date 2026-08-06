/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/UnixPeerCredentials.h"

#include "net/un/PeerCredentials.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace apps::codex_backend {

    bool unixPeerCredentialsSupported() noexcept {
        return net::un::peerCredentials(-1).status != net::un::PeerCredentialsStatus::Unsupported;
    }

    ai::openai::codex::frontend::FrontendPeerContext unixPeerContextFromFileDescriptor(int descriptor) noexcept {
        ai::openai::codex::frontend::FrontendPeerContext peer;
        peer.transport = ai::openai::codex::frontend::FrontendTransportKind::Unix;

        if (descriptor < 0) {
            return peer;
        }

        const net::un::PeerCredentials credentials = net::un::peerCredentials(descriptor);
        if (credentials.status == net::un::PeerCredentialsStatus::Success) {
            peer.localPeer = true;
            peer.unixUserId = static_cast<std::uint64_t>(credentials.uid);
        }
        return peer;
    }

    ai::openai::codex::frontend::FrontendPeerContext verifiedUnixPeerContextFromFileDescriptor(int descriptor,
                                                                                               std::uint64_t effectiveUserId) noexcept {
        ai::openai::codex::frontend::FrontendPeerContext peer = unixPeerContextFromFileDescriptor(descriptor);
        if (!peer.localPeer || !peer.unixUserId.has_value()) {
            return peer;
        }

        sockaddr_un address{};
        socklen_t addressSize = sizeof(address);
        if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &addressSize) != 0 || address.sun_family != AF_UNIX ||
            addressSize <= offsetof(sockaddr_un, sun_path) || address.sun_path[0] == '\0') {
            peer.localPeer = false;
            return peer;
        }

        const std::size_t available = static_cast<std::size_t>(addressSize) - offsetof(sockaddr_un, sun_path);
        const std::size_t pathSize = ::strnlen(address.sun_path, available);
        if (pathSize == 0 || pathSize == available ||
            !verifyUnixListenerPath(std::filesystem::path(std::string(address.sun_path, pathSize)), effectiveUserId).verified) {
            peer.localPeer = false;
        }
        return peer;
    }

    UnixListenerTrustResult verifyUnixListenerPath(const std::filesystem::path& path, std::uint64_t effectiveUserId) noexcept {
        if (path.empty()) {
            return {false, UnixListenerTrustFailure::Missing};
        }

        struct stat status{};
        if (::lstat(path.c_str(), &status) != 0) {
            return {false, errno == ENOENT ? UnixListenerTrustFailure::Missing : UnixListenerTrustFailure::MetadataUnavailable};
        }
        if (!S_ISSOCK(status.st_mode)) {
            return {false, UnixListenerTrustFailure::NotSocket};
        }
        if (static_cast<std::uint64_t>(status.st_uid) != effectiveUserId) {
            return {false, UnixListenerTrustFailure::WrongOwner};
        }
        if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            return {false, UnixListenerTrustFailure::InsecureMode};
        }
        return {true, UnixListenerTrustFailure::None};
    }

} // namespace apps::codex_backend
