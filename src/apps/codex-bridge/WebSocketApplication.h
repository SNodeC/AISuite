/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BRIDGE_WEBSOCKETAPPLICATION_H
#define APPS_CODEX_BRIDGE_WEBSOCKETAPPLICATION_H

#include <cstddef>
#include <string>

namespace express {
    class Router;
}

namespace ai::openai::codex::bridge {
    class CodexBridge;
}

namespace apps::codex_bridge {

    void configureWebSocketApplication(express::Router& router,
                                       ai::openai::codex::bridge::CodexBridge& bridge,
                                       std::string endpoint,
                                       std::size_t maximumFrameBytes);

} // namespace apps::codex_bridge

#endif
