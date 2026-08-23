/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-bridge/Configuration.h"

#include "utils/Config.h"

#include <string>
#include <utility>

namespace apps::codex_bridge {

    Configuration::Configuration(utils::SubCommand* parent)
        : utils::SubCommand(parent, this, "Applications") {
        appServerExecutable_ = setConfigurable(
            addOption("--app-server-executable",
                      "Codex executable used to spawn the app-server",
                      "PATH",
                      std::string{"codex"},
                      CLI::Validator{}),
            true);
        appServerTransport_ = setConfigurable(
            addOption("--app-server-transport",
                      "Transport used between codex-bridge and the app-server",
                      "TRANSPORT",
                      std::string{"stdio"},
                      CLI::IsMember({"stdio", "unix", "websocket-ipv4", "websocket-ipv6"})),
            true);
        codexHome_ = setConfigurable(
            addOption("--codex-home", "Set CODEX_HOME only for the spawned app-server", "PATH", std::string{}, CLI::Validator{}), true);
        maximumFrameBytes_ = setConfigurable(
            addOption("--bridge-maximum-frame-bytes",
                      "Maximum native or wrapped JSON message size",
                      "BYTES",
                      DefaultMaximumFrameBytes,
                      CLI::PositiveNumber),
            true);
        firstFrontendController_ = setConfigurable(
            addFlag("--bridge-first-frontend-controller{true}",
                    "Assign control to the first frontend connection",
                    "BOOL",
                    "true",
                    CLI::IsMember({"true", "false"})),
            true);
        observerReads_ = setConfigurable(
            addFlag("--bridge-observer-reads{true}",
                    "Allow observers to submit explicitly classified read-only requests",
                    "BOOL",
                    "true",
                    CLI::IsMember({"true", "false"})),
            true);
        webSocketEndpoint_ = setConfigurable(
            addOption("--bridge-websocket-endpoint",
                      "HTTP path used by Codex bridge WebSocket listeners",
                      "PATH",
                      std::string{"/codex"},
                      CLI::Validator{}),
            true);
    }

    Configuration::~Configuration() = default;

    ai::openai::codex2::bridge::CodexBridgeOptions Configuration::bridgeOptions() const {
        return {.firstFrontendBecomesController = firstFrontendController_->as<bool>(), .observersMayRead = observerReads_->as<bool>()};
    }

    AppServerTransport Configuration::appServerTransport() const {
        const std::string transport = appServerTransport_->as<std::string>();
        if (transport == "unix") {
            return AppServerTransport::Unix;
        }
        if (transport == "websocket-ipv4") {
            return AppServerTransport::WebSocketIPv4;
        }
        if (transport == "websocket-ipv6") {
            return AppServerTransport::WebSocketIPv6;
        }
        return AppServerTransport::Stdio;
    }

    ai::openai::codex2::provider::StdioAppServerOptions Configuration::stdioAppServerOptions() const {
        ai::openai::codex2::provider::StdioAppServerOptions options;
        options.executable = appServerExecutable_->as<std::string>();
        options.maximumFrameBytes = maximumFrameBytes();
        options.maximumQueuedInputBytes = maximumFrameBytes();
        const std::string codexHome = codexHome_->as<std::string>();
        if (!codexHome.empty()) {
            options.environment.emplace_back("CODEX_HOME", codexHome);
        }
        return options;
    }

    std::size_t Configuration::maximumFrameBytes() const {
        return maximumFrameBytes_->as<std::size_t>();
    }

    std::string Configuration::webSocketEndpoint() const {
        return webSocketEndpoint_->as<std::string>();
    }

} // namespace apps::codex_bridge
