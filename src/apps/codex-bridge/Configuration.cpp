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
        maximumProviderInputQueueBytes_ = setConfigurable(
            addOption("--app-server-maximum-queued-input-bytes",
                      "Maximum queued bytes waiting for app-server stdin",
                      "BYTES",
                      DefaultMaximumProviderInputQueueBytes,
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
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        webRoot_ = setConfigurable(
            addOption("--bridge-web-root",
                      "Directory containing the built CodexWebUI static files",
                      "PATH",
                      std::string{AISUITE_CODEXUI_WEB_ROOT},
                      CLI::Validator{}),
            true);
#endif
    }

    Configuration::~Configuration() = default;

    ai::openai::codex::bridge::CodexBridgeOptions Configuration::bridgeOptions() const {
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

    ai::openai::codex::provider::StdioAppServerOptions Configuration::stdioAppServerOptions() const {
        ai::openai::codex::provider::StdioAppServerOptions options;
        options.executable = appServerExecutable_->as<std::string>();
        options.maximumFrameBytes = maximumFrameBytes();
        options.maximumQueuedInputBytes = maximumProviderInputQueueBytes_->as<std::size_t>();
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

    std::string Configuration::webRoot() const {
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        return webRoot_->as<std::string>();
#else
        return {};
#endif
    }

} // namespace apps::codex_bridge
