/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-bridge-client/Configuration.h"

#include <string>

namespace apps::codex_bridge_client {

    Configuration::Configuration(utils::SubCommand* parent)
        : utils::SubCommand(parent, this, "Applications") {
        jsonOutput_ = setConfigurable(
            addFlag("--json", "Emit one compact JSON object per stdout line", "", CLI::Validator{}), true);
        maximumFrameBytes_ = setConfigurable(
            addOption("--bridge-maximum-frame-bytes",
                      "Maximum encoded bridge envelope size",
                      "BYTES",
                      DefaultMaximumFrameBytes,
                      CLI::PositiveNumber),
            true);
        webSocketEndpoint_ = setConfigurable(
            addOption("--bridge-websocket-endpoint",
                      "HTTP path used for a Codex bridge WebSocket connection",
                      "PATH",
                      std::string{"/codex"},
                      CLI::Validator{}),
            true);
    }

    Configuration::~Configuration() = default;

    bool Configuration::jsonOutput() const {
        return jsonOutput_->as<bool>();
    }

    std::size_t Configuration::maximumFrameBytes() const {
        return maximumFrameBytes_->as<std::size_t>();
    }

    std::string Configuration::webSocketEndpoint() const {
        return webSocketEndpoint_->as<std::string>();
    }

} // namespace apps::codex_bridge_client
