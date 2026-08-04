/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/UnixPeerCredentials.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace apps::codex_backend {

    ai::openai::codex::frontend::FrontendPeerContext unixPeerContextFromFileDescriptor(int descriptor) noexcept {
        ai::openai::codex::frontend::FrontendPeerContext peer;
        peer.transport = ai::openai::codex::frontend::FrontendTransportKind::Unix;

        if (descriptor < 0) {
            return peer;
        }

#if defined(__linux__) && defined(SO_PEERCRED)
        struct ucred credentials{};
        socklen_t size = sizeof(credentials);
        if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 && size == sizeof(credentials) &&
            credentials.uid != static_cast<uid_t>(-1)) {
            peer.localPeer = true;
            peer.unixUserId = static_cast<std::uint64_t>(credentials.uid);
        }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
        uid_t userId = static_cast<uid_t>(-1);
        gid_t groupId = static_cast<gid_t>(-1);
        if (::getpeereid(descriptor, &userId, &groupId) == 0 && userId != static_cast<uid_t>(-1)) {
            peer.localPeer = true;
            peer.unixUserId = static_cast<std::uint64_t>(userId);
        }
#else
        static_cast<void>(descriptor);
#endif
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
