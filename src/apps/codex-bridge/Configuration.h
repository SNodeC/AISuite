/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BRIDGE_CONFIGURATION_H
#define APPS_CODEX_BRIDGE_CONFIGURATION_H

#include "ai/agent/Runtime.h"
#include "ai/openai/codex/bridge/CodexBridge.h"
#include "ai/openai/codex/provider/StdioAppServer.h"
#include "utils/SubCommand.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace CLI {
    class Option;
}

namespace apps::codex_bridge {

    inline constexpr std::size_t DefaultMaximumFrameBytes = 64U * 1024U * 1024U;
    inline constexpr std::size_t DefaultMaximumWriteQueueBytes = 128U * 1024U * 1024U;
    inline constexpr std::size_t DefaultMaximumProviderInputQueueBytes = 128U * 1024U * 1024U;

    enum class AppServerTransport { Stdio, Unix, WebSocketIPv4, WebSocketIPv6 };

    class Configuration : public utils::SubCommand {
    public:
        constexpr static std::string_view NAME{"codex"};
        constexpr static std::string_view DESCRIPTION{"Codex app-server bridge"};

        explicit Configuration(utils::SubCommand* parent);
        ~Configuration() override;

        ai::openai::codex::bridge::CodexBridgeOptions bridgeOptions() const;
        AppServerTransport appServerTransport() const;
        ai::openai::codex::provider::StdioAppServerOptions stdioAppServerOptions() const;
        std::size_t maximumFrameBytes() const;
        std::string webSocketEndpoint() const;
        std::string webRoot() const;
        ai::agent::Selection selection() const;
        std::string anthropicKeyEnvironment() const;
        std::string anthropicBaseUrl() const;
        std::uint64_t maximumOutputTokens() const;
        std::uint64_t modelContextWindow() const;

    private:
        CLI::Option* agent_ = nullptr;
        CLI::Option* modelProvider_ = nullptr;
        CLI::Option* model_ = nullptr;
        CLI::Option* anthropicKeyEnvironment_ = nullptr;
        CLI::Option* anthropicBaseUrl_ = nullptr;
        CLI::Option* maximumOutputTokens_ = nullptr;
        CLI::Option* modelContextWindow_ = nullptr;
        CLI::Option* appServerExecutable_ = nullptr;
        CLI::Option* appServerTransport_ = nullptr;
        CLI::Option* codexHome_ = nullptr;
        CLI::Option* maximumFrameBytes_ = nullptr;
        CLI::Option* maximumProviderInputQueueBytes_ = nullptr;
        CLI::Option* firstFrontendController_ = nullptr;
        CLI::Option* observerReads_ = nullptr;
        CLI::Option* webSocketEndpoint_ = nullptr;
        CLI::Option* webRoot_ = nullptr;
    };

} // namespace apps::codex_bridge

#endif
