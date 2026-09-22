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
        agent_ = setConfigurable(addOption("--agent", "Agent runtime", "AGENT", std::string{"codex"}, CLI::IsMember({"codex"})), true);
        modelProvider_ = setConfigurable(addOption("--model-provider",
                                                   "Model provider (OpenAI stays direct)",
                                                   "PROVIDER",
                                                   std::string{"openai"},
                                                   CLI::IsMember({"openai", "anthropic"})),
                                         true);
        model_ = setConfigurable(addOption("--model", "Model ID; required for Anthropic", "MODEL", std::string{}, CLI::Validator{}), true);
        anthropicKeyEnvironment_ = setConfigurable(addOption("--anthropic-api-key-env",
                                                             "Environment variable containing the provider API key",
                                                             "NAME",
                                                             std::string{"ANTHROPIC_API_KEY"},
                                                             CLI::Validator{}),
                                                   true);
        anthropicBaseUrl_ = setConfigurable(
            addOption(
                "--anthropic-base-url", "Native Messages API base URL", "URL", std::string{"https://api.anthropic.com"}, CLI::Validator{}),
            true);
        maximumOutputTokens_ = setConfigurable(addOption("--model-maximum-output-tokens",
                                                         "Anthropic maximum output tokens per inference",
                                                         "TOKENS",
                                                         std::uint64_t{8192},
                                                         CLI::PositiveNumber),
                                               true);
        modelContextWindow_ = setConfigurable(addOption("--model-context-window",
                                                        "Configured provider model context capacity",
                                                        "TOKENS",
                                                        std::uint64_t{200000},
                                                        CLI::PositiveNumber),
                                              true);
        appServerExecutable_ = setConfigurable(
            addOption(
                "--app-server-executable", "Codex executable used to spawn the app-server", "PATH", std::string{"codex"}, CLI::Validator{}),
            true);
        appServerTransport_ = setConfigurable(addOption("--app-server-transport",
                                                        "Transport used between codex-bridge and the app-server",
                                                        "TRANSPORT",
                                                        std::string{"stdio"},
                                                        CLI::IsMember({"stdio", "unix", "websocket-ipv4", "websocket-ipv6"})),
                                              true);
        codexHome_ = setConfigurable(
            addOption("--codex-home", "Set CODEX_HOME only for the spawned app-server", "PATH", std::string{}, CLI::Validator{}), true);
        maximumFrameBytes_ = setConfigurable(addOption("--bridge-maximum-frame-bytes",
                                                       "Maximum native or wrapped JSON message size",
                                                       "BYTES",
                                                       DefaultMaximumFrameBytes,
                                                       CLI::PositiveNumber),
                                             true);
        maximumProviderInputQueueBytes_ = setConfigurable(addOption("--app-server-maximum-queued-input-bytes",
                                                                    "Maximum queued bytes waiting for app-server stdin",
                                                                    "BYTES",
                                                                    DefaultMaximumProviderInputQueueBytes,
                                                                    CLI::PositiveNumber),
                                                          true);
        firstFrontendController_ = setConfigurable(addFlag("--bridge-first-frontend-controller{true}",
                                                           "Assign control to the first frontend connection",
                                                           "BOOL",
                                                           "true",
                                                           CLI::IsMember({"true", "false"})),
                                                   true);
        observerReads_ = setConfigurable(addFlag("--bridge-observer-reads{true}",
                                                 "Allow observers to submit explicitly classified read-only requests",
                                                 "BOOL",
                                                 "true",
                                                 CLI::IsMember({"true", "false"})),
                                         true);
        webSocketEndpoint_ = setConfigurable(addOption("--bridge-websocket-endpoint",
                                                       "HTTP path used by Codex bridge WebSocket listeners",
                                                       "PATH",
                                                       std::string{"/codex"},
                                                       CLI::Validator{}),
                                             true);
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        webRoot_ = setConfigurable(addOption("--bridge-web-root",
                                             "Directory containing the built CodexWebUI static files",
                                             "PATH",
                                             std::string{AISUITE_CODEXUI_WEB_ROOT},
                                             CLI::Validator{}),
                                   true);
#endif
    }

    Configuration::~Configuration() = default;
    ai::agent::Selection Configuration::selection() const {
        return {agent_->as<std::string>(), modelProvider_->as<std::string>(), model_->as<std::string>()};
    }
    std::string Configuration::anthropicKeyEnvironment() const {
        return anthropicKeyEnvironment_->as<std::string>();
    }
    std::string Configuration::anthropicBaseUrl() const {
        return anthropicBaseUrl_->as<std::string>();
    }
    std::uint64_t Configuration::maximumOutputTokens() const {
        return maximumOutputTokens_->as<std::uint64_t>();
    }
    std::uint64_t Configuration::modelContextWindow() const {
        return modelContextWindow_->as<std::uint64_t>();
    }

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
        const auto selected = selection();
        if (selected.provider == "openai" && !selected.model.empty()) {
            options.arguments.insert(options.arguments.end(), {"-c", "model=" + nlohmann::json(selected.model).dump()});
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
