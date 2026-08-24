/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BRIDGE_CLIENT_COMMANDPARSER_H
#define APPS_CODEX_BRIDGE_CLIENT_COMMANDPARSER_H

#include "ai/openai/codex/protocol/generated/ProtocolTypes.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

namespace apps::codex_bridge_client {

    struct NoopCommand {};
    struct HelpCommand {};
    struct QuitCommand {};
    struct ReconnectCommand {};
    struct WatchCommand { bool enabled = true; };
    struct SnapshotCommand {};
    struct ReplayCommand { std::uint64_t sequence = 0; };
    struct ControllerAcquireCommand {};
    struct ControllerReleaseCommand {};
    struct ThreadListCommand { ai::openai::codex::generated::v2::ThreadListParams parameters; };
    struct ThreadStartCommand { ai::openai::codex::generated::v2::ThreadStartParams parameters; };
    struct ThreadResumeCommand { ai::openai::codex::generated::v2::ThreadResumeParams parameters; };
    struct ThreadReadCommand { ai::openai::codex::generated::v2::ThreadReadParams parameters; };
    struct TurnStartCommand { ai::openai::codex::generated::v2::TurnStartParams parameters; };
    struct TurnInterruptCommand { ai::openai::codex::generated::v2::TurnInterruptParams parameters; };
    struct RawCommand { nlohmann::json message; };
    struct NewCommand {
        ai::openai::codex::generated::v2::ThreadStartParams options;
        std::string prompt;
    };
    struct CommandParseError { std::string message; };

    using ParsedCommand = std::variant<NoopCommand,
                                       HelpCommand,
                                       QuitCommand,
                                       ReconnectCommand,
                                       WatchCommand,
                                       SnapshotCommand,
                                       ReplayCommand,
                                       ControllerAcquireCommand,
                                       ControllerReleaseCommand,
                                       ThreadListCommand,
                                       ThreadStartCommand,
                                       ThreadResumeCommand,
                                       ThreadReadCommand,
                                       TurnStartCommand,
                                       TurnInterruptCommand,
                                       RawCommand,
                                       NewCommand,
                                       CommandParseError>;

    class CommandParser {
    public:
        ParsedCommand parse(std::string_view line) const;
        static std::string helpText();
    };

} // namespace apps::codex_bridge_client

#endif
