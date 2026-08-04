/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_UNIXPEERCREDENTIALS_H
#define APPS_CODEX_BACKEND_UNIXPEERCREDENTIALS_H

#include "ai/openai/codex/frontend/Security.h"

#include <cstdint>
#include <filesystem>

namespace apps::codex_backend {

    enum class UnixListenerTrustFailure { None, Missing, MetadataUnavailable, NotSocket, WrongOwner, InsecureMode };

    struct UnixListenerTrustResult {
        bool verified = false;
        UnixListenerTrustFailure failure = UnixListenerTrustFailure::MetadataUnavailable;

        bool operator==(const UnixListenerTrustResult&) const = default;
    };

    // Returns a Unix peer context whose localPeer bit is true only when the
    // operating system supplied verified credentials for the accepted socket.
    [[nodiscard]] ai::openai::codex::frontend::FrontendPeerContext unixPeerContextFromFileDescriptor(int descriptor) noexcept;

    // Adds the bound-listener pathname ownership/mode check required before
    // FrontendService may interpret localPeer as verified local trust.
    [[nodiscard]] ai::openai::codex::frontend::FrontendPeerContext
    verifiedUnixPeerContextFromFileDescriptor(int descriptor, std::uint64_t effectiveUserId) noexcept;

    // Verify the bound pathname independently from peer credentials. Both
    // checks must succeed before the reference authenticator grants the
    // local_trusted profile.
    [[nodiscard]] UnixListenerTrustResult verifyUnixListenerPath(const std::filesystem::path& path, std::uint64_t effectiveUserId) noexcept;

} // namespace apps::codex_backend

#endif // APPS_CODEX_BACKEND_UNIXPEERCREDENTIALS_H
