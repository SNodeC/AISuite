/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_CLIENT_COMMANDPARSER_H
#define APPS_CODEX_BACKEND_CLIENT_COMMANDPARSER_H

#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/typed/Threads.h"
#include "ai/openai/codex/typed/Turns.h"

#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace apps::codex_backend_client {

    struct NoopCommand {
        bool operator==(const NoopCommand&) const = default;
    };

    struct HelpCommand {
        bool operator==(const HelpCommand&) const = default;
    };

    struct QuitCommand {
        bool operator==(const QuitCommand&) const = default;
    };

    struct WatchCommand {
        bool enabled = true;

        bool operator==(const WatchCommand&) const = default;
    };

    struct SnapshotCommand {};

    struct ReplayCommand {
        ai::openai::codex::frontend::SequenceNumber after;
    };

    struct ControllerAcquireCommand {};
    struct ControllerReleaseCommand {};

    struct ThreadListCommand {
        ai::openai::codex::typed::ThreadListParams parameters;
    };

    struct ThreadStartCommand {
        ai::openai::codex::typed::ThreadStartParams parameters;
    };

    struct ThreadResumeCommand {
        ai::openai::codex::typed::ThreadResumeParams parameters;
    };

    struct ThreadReadCommand {
        ai::openai::codex::typed::ThreadReadParams parameters;
    };

    struct TurnStartCommand {
        ai::openai::codex::typed::TurnStartParams parameters;
    };

    struct TurnInterruptCommand {
        ai::openai::codex::typed::TurnInterruptParams parameters;
    };

    struct RawCommand {
        ai::openai::codex::frontend::generated::CompleteCommandParameters parameters;
    };

    using RemoteCommand = std::variant<SnapshotCommand,
                                       ReplayCommand,
                                       ControllerAcquireCommand,
                                       ControllerReleaseCommand,
                                       ThreadListCommand,
                                       ThreadStartCommand,
                                       ThreadResumeCommand,
                                       ThreadReadCommand,
                                       TurnStartCommand,
                                       TurnInterruptCommand,
                                       RawCommand>;

    struct NewCommand {
        ai::openai::codex::typed::ThreadStartParams options;
        std::string prompt;
    };

    struct CommandParseError {
        std::string message;

        bool operator==(const CommandParseError&) const = default;
    };

    using ParsedCommand = std::variant<NoopCommand, HelpCommand, QuitCommand, WatchCommand, RemoteCommand, NewCommand, CommandParseError>;

    class CommandParser {
    public:
        CommandParser() = default;

        [[nodiscard]] ParsedCommand parse(std::string_view line);
        [[nodiscard]] static std::string helpText();
    };

} // namespace apps::codex_backend_client

#endif // APPS_CODEX_BACKEND_CLIENT_COMMANDPARSER_H
