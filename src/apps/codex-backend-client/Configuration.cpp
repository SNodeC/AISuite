/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/Configuration.h"

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <unistd.h>

namespace apps::codex_backend_client {

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
