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

namespace CLI {
    class Option;
}

namespace apps::codex_backend_client {

    // A complete backend snapshot may be larger than the command-side 8 MiB
    // limit. Match the reference backend's bounded Unix writer capacity.
    inline constexpr std::size_t DEFAULT_MAXIMUM_FRAME_SIZE = 13U * 1024U * 1024U;
    inline constexpr std::size_t DEFAULT_MAXIMUM_OUTBOUND_BYTES = 13U * 1024U * 1024U;
    inline constexpr std::size_t DEFAULT_MAXIMUM_QUEUED_COMMANDS = 256U;
    inline constexpr std::size_t DEFAULT_MAXIMUM_QUEUED_COMMAND_BYTES = 16U * 1024U * 1024U;

    struct CommandQueueLimits {
        std::size_t maximumCommands = DEFAULT_MAXIMUM_QUEUED_COMMANDS;
        std::size_t maximumCommandBytes = DEFAULT_MAXIMUM_QUEUED_COMMAND_BYTES;

        bool operator==(const CommandQueueLimits&) const = default;
    };

    class ClientPolicyConfiguration {
    public:
        ClientPolicyConfiguration();

        [[nodiscard]] CommandQueueLimits commandQueueLimits() const;

    private:
        CLI::Option* maximumQueuedCommandsOption = nullptr;
        CLI::Option* maximumQueuedCommandBytesOption = nullptr;
    };

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
