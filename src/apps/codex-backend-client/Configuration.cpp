/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/Configuration.h"

#include "utils/Config.h"

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <unistd.h>

namespace apps::codex_backend_client {

    ClientPolicyConfiguration::ClientPolicyConfiguration() {
        auto& root = utils::Config::configRoot;
        maximumQueuedCommandsOption =
            root.setConfigurable(root.addOption("--maximum-queued-commands",
                                                "Maximum commands retained by the frontend client before SDK acceptance",
                                                "COUNT",
                                                DEFAULT_MAXIMUM_QUEUED_COMMANDS,
                                                CLI::TypeValidator<std::size_t>()),
                                 true);
        maximumQueuedCommandBytesOption = root.setConfigurable(root.addOption("--maximum-queued-command-bytes",
                                                                              "Maximum retained frontend client command input bytes",
                                                                              "BYTES",
                                                                              DEFAULT_MAXIMUM_QUEUED_COMMAND_BYTES,
                                                                              CLI::TypeValidator<std::size_t>()),
                                                               true);
    }

    CommandQueueLimits ClientPolicyConfiguration::commandQueueLimits() const {
        return {.maximumCommands = maximumQueuedCommandsOption->as<std::size_t>(),
                .maximumCommandBytes = maximumQueuedCommandBytesOption->as<std::size_t>()};
    }

    std::string defaultSocketPath() {
        const char* runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");
        if (runtimeDirectory != nullptr && *runtimeDirectory != '\0') {
            return (std::filesystem::path(runtimeDirectory) / "snodec-codex-backend.sock").string();
        }

        return (std::filesystem::path("/tmp") / ("snodec-codex-backend-" + std::to_string(::getuid()) + ".sock")).string();
    }

    bool OutgoingTransportPreflight::accepted() const noexcept {
        return enabledCount == 1 && selectedIndex.has_value();
    }

    OutgoingTransportPreflight preflightOutgoingTransports(std::span<const bool> disabledStates) noexcept {
        OutgoingTransportPreflight result;
        for (std::size_t index = 0; index < disabledStates.size(); ++index) {
            if (disabledStates[index]) {
                continue;
            }
            if (result.enabledCount != std::numeric_limits<std::size_t>::max()) {
                ++result.enabledCount;
            }
            if (result.enabledCount == 1) {
                result.selectedIndex = index;
            } else {
                result.selectedIndex.reset();
            }
        }
        return result;
    }

} // namespace apps::codex_backend_client
