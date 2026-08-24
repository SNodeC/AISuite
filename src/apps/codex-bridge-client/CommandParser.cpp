/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-bridge-client/CommandParser.h"

#include "ai/openai/codex/protocol/Envelope.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <system_error>
#include <unordered_set>

namespace apps::codex_bridge_client {

    namespace generated = ai::openai::codex::generated;

    namespace {

        bool isSpace(char value) noexcept {
            return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
        }

        std::string_view trim(std::string_view value) noexcept {
            while (!value.empty() && isSpace(value.front())) {
                value.remove_prefix(1);
            }
            while (!value.empty() && isSpace(value.back())) {
                value.remove_suffix(1);
            }
            return value;
        }

        std::string_view takeWord(std::string_view& value) noexcept {
            value = trim(value);
            std::size_t length = 0;
            while (length < value.size() && !isSpace(value[length])) {
                ++length;
            }
            const std::string_view word = value.substr(0, length);
            value.remove_prefix(length);
            value = trim(value);
            return word;
        }

        CommandParseError error(std::string message) {
            return {std::move(message)};
        }

        std::optional<std::uint64_t> unsignedInteger(std::string_view value) noexcept {
            std::uint64_t result = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()
                ? std::optional<std::uint64_t>(result)
                : std::nullopt;
        }

        bool parseThreadOptions(std::string_view& input,
                                nlohmann::json& parameters,
                                bool ephemeralAllowed,
                                bool stopAtSeparator,
                                bool& separator,
                                std::string& failure) {
            std::unordered_set<std::string> seen;
            while (!input.empty()) {
                const std::string_view option = takeWord(input);
                if (option == "--") {
                    if (stopAtSeparator) {
                        separator = true;
                        return true;
                    }
                    failure = "unexpected '--' separator";
                    return false;
                }
                const bool known = option == "--cwd" || option == "--model" || option == "--model-provider" ||
                    option == "--approval-policy" || option == "--sandbox-mode" || option == "--ephemeral";
                if (!known) {
                    failure = "unknown thread option";
                    return false;
                }
                if (!seen.emplace(option).second) {
                    failure = "thread option may only be specified once";
                    return false;
                }
                if (option == "--ephemeral") {
                    if (!ephemeralAllowed) {
                        failure = "option '--ephemeral' is not supported by resume";
                        return false;
                    }
                    parameters["ephemeral"] = true;
                    continue;
                }
                const std::string_view value = takeWord(input);
                if (value.empty() || value.starts_with("--")) {
                    failure = "thread option requires a value";
                    return false;
                }
                const std::string key = option == "--cwd" ? "cwd"
                    : option == "--model"                 ? "model"
                    : option == "--model-provider"        ? "modelProvider"
                    : option == "--approval-policy"       ? "approvalPolicy"
                                                           : "sandbox";
                parameters[key] = value;
            }
            return true;
        }

        generated::v2::TurnStartParams turnParameters(std::string threadId, std::string prompt) {
            return generated::v2::TurnStartParams(
                {{"threadId", std::move(threadId)}, {"input", {{{"type", "text"}, {"text", std::move(prompt)}}}}});
        }

    } // namespace

    ParsedCommand CommandParser::parse(std::string_view line) const {
        line = trim(line);
        if (line.empty()) {
            return NoopCommand{};
        }
        std::string_view remainder = line;
        const std::string_view name = takeWord(remainder);
        if (name == "help") {
            return remainder.empty() ? ParsedCommand(HelpCommand{}) : ParsedCommand(error("usage: help"));
        }
        if (name == "quit") {
            return remainder.empty() ? ParsedCommand(QuitCommand{}) : ParsedCommand(error("usage: quit"));
        }
        if (name == "reconnect") {
            return remainder.empty() ? ParsedCommand(ReconnectCommand{}) : ParsedCommand(error("usage: reconnect"));
        }
        if (name == "watch") {
            const std::string_view value = takeWord(remainder);
            return remainder.empty() && (value == "on" || value == "off")
                ? ParsedCommand(WatchCommand{value == "on"})
                : ParsedCommand(error("usage: watch on|off"));
        }
        if (name == "snapshot") {
            return remainder.empty() ? ParsedCommand(SnapshotCommand{}) : ParsedCommand(error("usage: snapshot"));
        }
        if (name == "replay") {
            const std::string_view value = takeWord(remainder);
            const auto sequence = unsignedInteger(value);
            return remainder.empty() && sequence ? ParsedCommand(ReplayCommand{*sequence})
                                                 : ParsedCommand(error("usage: replay <sequence>"));
        }
        if (name == "acquire") {
            return remainder.empty() ? ParsedCommand(ControllerAcquireCommand{}) : ParsedCommand(error("usage: acquire"));
        }
        if (name == "release") {
            return remainder.empty() ? ParsedCommand(ControllerReleaseCommand{}) : ParsedCommand(error("usage: release"));
        }
        if (name == "threads") {
            return remainder.empty()
                ? ParsedCommand(ThreadListCommand{generated::v2::ThreadListParams(nlohmann::json::object())})
                                     : ParsedCommand(error("usage: threads"));
        }
        if (name == "start") {
            nlohmann::json parameters = nlohmann::json::object();
            bool separator = false;
            std::string failure;
            return parseThreadOptions(remainder, parameters, true, false, separator, failure)
                ? ParsedCommand(ThreadStartCommand{generated::v2::ThreadStartParams(std::move(parameters))})
                : ParsedCommand(error("start: " + failure + "; enter 'help' for command syntax"));
        }
        if (name == "resume") {
            const std::string_view threadId = takeWord(remainder);
            if (threadId.empty() || threadId.starts_with("--")) {
                return error("usage: resume <thread-id> [thread-resume-options]");
            }
            nlohmann::json parameters{{"threadId", std::string(threadId)}};
            bool separator = false;
            std::string failure;
            return parseThreadOptions(remainder, parameters, false, false, separator, failure)
                ? ParsedCommand(ThreadResumeCommand{generated::v2::ThreadResumeParams(std::move(parameters))})
                : ParsedCommand(error("resume: " + failure + "; enter 'help' for command syntax"));
        }
        if (name == "new") {
            if (remainder.empty()) {
                return error("usage: new [thread-start-options] -- <prompt> | new <prompt>");
            }
            nlohmann::json parameters = nlohmann::json::object();
            std::string prompt;
            std::string_view probe = remainder;
            if (!takeWord(probe).starts_with("--")) {
                prompt = std::string(remainder);
            } else {
                bool separator = false;
                std::string failure;
                if (!parseThreadOptions(remainder, parameters, true, true, separator, failure)) {
                    return error("new: " + failure + "; enter 'help' for command syntax");
                }
                if (!separator || trim(remainder).empty()) {
                    return error("new: thread options require '-- <prompt>'");
                }
                prompt = std::string(trim(remainder));
            }
            return NewCommand{generated::v2::ThreadStartParams(std::move(parameters)), std::move(prompt)};
        }
        if (name == "read") {
            const std::string_view threadId = takeWord(remainder);
            return !threadId.empty() && remainder.empty()
                ? ParsedCommand(ThreadReadCommand{generated::v2::ThreadReadParams(
                      {{"threadId", std::string(threadId)}, {"includeTurns", true}})})
                : ParsedCommand(error("usage: read <thread-id>"));
        }
        if (name == "turn") {
            const std::string_view threadId = takeWord(remainder);
            return !threadId.empty() && !remainder.empty()
                ? ParsedCommand(TurnStartCommand{turnParameters(std::string(threadId), std::string(remainder))})
                : ParsedCommand(error("usage: turn <thread-id> <prompt>"));
        }
        if (name == "interrupt") {
            const std::string_view threadId = takeWord(remainder);
            const std::string_view turnId = takeWord(remainder);
            return !threadId.empty() && !turnId.empty() && remainder.empty()
                ? ParsedCommand(TurnInterruptCommand{generated::v2::TurnInterruptParams(
                      {{"threadId", std::string(threadId)}, {"turnId", std::string(turnId)}})})
                : ParsedCommand(error("usage: interrupt <thread-id> <turn-id>"));
        }
        if (name == "raw") {
            if (remainder.empty()) {
                return error("usage: raw <json-rpc-message>");
            }
            nlohmann::json message = nlohmann::json::parse(remainder, nullptr, false);
            return !message.is_discarded() && ai::openai::codex::protocol::classifyJsonRpc(message) !=
                    ai::openai::codex::protocol::JsonRpcKind::Invalid
                ? ParsedCommand(RawCommand{std::move(message)})
                : ParsedCommand(error("raw requires one valid app-server JSON-RPC message"));
        }
        return error("unknown command; enter 'help' for available commands");
    }

    std::string CommandParser::helpText() {
        return "Commands:\n"
               "  help\n"
               "  quit\n"
               "  reconnect\n"
               "  snapshot\n"
               "  replay <sequence>\n"
               "  acquire\n"
               "  release\n"
               "  threads\n"
               "  start [--cwd <path>] [--model <model>]\n"
               "        [--model-provider <provider>] [--approval-policy <policy>]\n"
               "        [--sandbox-mode <mode>] [--ephemeral]\n"
               "  resume <thread-id> [thread-start-options except --ephemeral]\n"
               "  new [thread-start-options] -- <prompt>\n"
               "  new <prompt>\n"
               "  read <thread-id>\n"
               "  turn <thread-id> <prompt>\n"
               "  interrupt <thread-id> <turn-id>\n"
               "  raw <json-rpc-message>\n"
               "  watch on|off";
    }

} // namespace apps::codex_bridge_client
