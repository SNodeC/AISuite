/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_CLIENT_CONFIGURATION_H
#define APPS_CODEX_BACKEND_CLIENT_CONFIGURATION_H

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace apps::codex_backend_client {

    inline constexpr std::size_t DEFAULT_MAXIMUM_OUTBOUND_BYTES = 13U * 1024U * 1024U;

    // Matches the reference backend's default exactly. The value is installed
    // as a ConfigSocketClient default; SNode.C CLI/config overrides remain
    // authoritative.
    [[nodiscard]] std::string defaultSocketPath();

    struct OutgoingTransportPreflight {
        std::size_t enabledCount = 0;
        std::optional<std::size_t> selectedIndex;

        [[nodiscard]] bool accepted() const noexcept;
    };

    // SNode.C applies named-instance command-line/config-file values during
    // bootstrap. Call this from the first event-loop turn with each instance's
    // effective ConfigInstance::getDisabled() value, before calling connect().
    [[nodiscard]] OutgoingTransportPreflight preflightOutgoingTransports(std::span<const bool> disabledStates) noexcept;

} // namespace apps::codex_backend_client

#endif // APPS_CODEX_BACKEND_CLIENT_CONFIGURATION_H
