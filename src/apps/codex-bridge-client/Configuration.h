/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BRIDGE_CLIENT_CONFIGURATION_H
#define APPS_CODEX_BRIDGE_CLIENT_CONFIGURATION_H

#include "utils/SubCommand.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace CLI {
    class Option;
}

namespace apps::codex_bridge_client {

    inline constexpr std::size_t DefaultMaximumFrameBytes = 64U * 1024U * 1024U;
    inline constexpr std::size_t DefaultMaximumWriteQueueBytes = 128U * 1024U * 1024U;

    class Configuration final : public utils::SubCommand {
    public:
        constexpr static std::string_view NAME{"codex-client"};
        constexpr static std::string_view DESCRIPTION{"Codex bridge interactive client"};

        explicit Configuration(utils::SubCommand* parent);
        ~Configuration() override;

        bool jsonOutput() const;
        std::size_t maximumFrameBytes() const;
        std::string webSocketEndpoint() const;

    private:
        CLI::Option* jsonOutput_ = nullptr;
        CLI::Option* maximumFrameBytes_ = nullptr;
        CLI::Option* webSocketEndpoint_ = nullptr;
    };

} // namespace apps::codex_bridge_client

#endif
