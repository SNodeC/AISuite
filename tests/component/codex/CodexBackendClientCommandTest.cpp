/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "apps/codex-backend-client/CommandParser.h"
#include "support/TestResult.h"

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace {
    namespace client = apps::codex_backend_client;
    namespace frontend = ai::openai::codex::frontend;
    namespace typed = ai::openai::codex::typed;

    const client::RemoteCommand* remoteCommand(const client::ParsedCommand& parsed) {
        return std::get_if<client::RemoteCommand>(&parsed);
    }

    template <typename Command>
    const Command* typedCommand(const client::ParsedCommand& parsed) {
        const client::RemoteCommand* const remote = remoteCommand(parsed);
        return remote == nullptr ? nullptr : std::get_if<Command>(remote);
    }

    const frontend::Json* rawParameters(const client::ParsedCommand& parsed, frontend::generated::MethodId& method) {
        const client::RawCommand* const raw = typedCommand<client::RawCommand>(parsed);
        if (raw == nullptr) {
            return nullptr;
        }
        return std::visit(
            [&method](const auto& value) -> const frontend::Json* {
                method = value.Method;
                return &value.value;
            },
            raw->parameters);
    }
} // namespace

int main() {
    tests::support::TestResult result;
    client::CommandParser parser;

    result.expectTrue(std::holds_alternative<client::NoopCommand>(parser.parse("  \t ")), "blank input remains a local no-op");
    result.expectTrue(std::holds_alternative<client::HelpCommand>(parser.parse("help")), "help remains local");
    result.expectTrue(std::holds_alternative<client::QuitCommand>(parser.parse("quit")), "quit remains local");
    result.expectTrue(std::holds_alternative<client::ReconnectCommand>(parser.parse("reconnect")), "reconnect remains local");
    result.expectTrue(std::holds_alternative<client::CommandParseError>(parser.parse("reconnect now")),
                      "reconnect rejects unexpected arguments locally");

    result.expectTrue(typedCommand<client::SnapshotCommand>(parser.parse("snapshot")) != nullptr,
                      "snapshot produces a native Synchronization façade command");
    result.expectTrue(typedCommand<client::ControllerAcquireCommand>(parser.parse("acquire")) != nullptr &&
                          typedCommand<client::ControllerReleaseCommand>(parser.parse("release")) != nullptr,
                      "controller commands target the native Controller façade");
    result.expectTrue(typedCommand<client::ThreadListCommand>(parser.parse("threads")) != nullptr,
                      "thread listing targets the typed Threads façade");
    const client::ParsedCommand replayParsed = parser.parse("replay 41");
    const client::ReplayCommand* const replay = typedCommand<client::ReplayCommand>(replayParsed);
    result.expectTrue(replay != nullptr && replay->after == frontend::SequenceNumber(41),
                      "replay preserves the global cursor in a typed Synchronization façade command");

    const client::ParsedCommand startParsed = parser.parse("start --cwd /work --model gpt-5 --sandbox-mode workspace-write --ephemeral");
    const client::ThreadStartCommand* const start = typedCommand<client::ThreadStartCommand>(startParsed);
    result.expectTrue(start != nullptr && start->parameters.cwd.hasValue() && *start->parameters.cwd == "/work" &&
                          start->parameters.model.hasValue() && start->parameters.model->value == "gpt-5" &&
                          start->parameters.sandbox.hasValue() && start->parameters.sandbox->value == "workspace-write" &&
                          start->parameters.ephemeral.hasValue() && *start->parameters.ephemeral,
                      "thread.start is represented by the domain-typed Threads façade parameters");

    const client::ParsedCommand resumeParsed = parser.parse("resume thread-7 --model gpt-5");
    const client::ThreadResumeCommand* const resume = typedCommand<client::ThreadResumeCommand>(resumeParsed);
    result.expectTrue(resume != nullptr && resume->parameters.threadId == typed::ThreadId{"thread-7"} &&
                          resume->parameters.model.hasValue() && resume->parameters.model->value == "gpt-5",
                      "thread.resume preserves strong IDs and typed options");

    const client::ParsedCommand readParsed = parser.parse("read thread-7");
    const client::ThreadReadCommand* const read = typedCommand<client::ThreadReadCommand>(readParsed);
    result.expectTrue(read != nullptr && read->parameters.threadId == typed::ThreadId{"thread-7"},
                      "thread.read preserves its strong thread ID");

    const client::ParsedCommand turnParsed = parser.parse("turn thread-7 hello world");
    const client::TurnStartCommand* const turn = typedCommand<client::TurnStartCommand>(turnParsed);
    const typed::TextInput* const text =
        turn != nullptr && turn->parameters.input.size() == 1 ? std::get_if<typed::TextInput>(&turn->parameters.input.front()) : nullptr;
    result.expectTrue(turn != nullptr && turn->parameters.threadId == typed::ThreadId{"thread-7"} && text != nullptr &&
                          text->text == "hello world",
                      "turn.start is represented by the domain-typed Turns façade parameters");

    const client::ParsedCommand interruptParsed = parser.parse("interrupt thread-7 turn-9");
    const client::TurnInterruptCommand* const interrupt = typedCommand<client::TurnInterruptCommand>(interruptParsed);
    result.expectTrue(interrupt != nullptr && interrupt->parameters.threadId == typed::ThreadId{"thread-7"} &&
                          interrupt->parameters.turnId == typed::TurnId{"turn-9"},
                      "turn.interrupt preserves strong thread and turn IDs");

    const client::ParsedCommand compound = parser.parse("new --cwd /work -- prompt text");
    const auto* const newCommand = std::get_if<client::NewCommand>(&compound);
    result.expectTrue(newCommand != nullptr && newCommand->options.cwd.hasValue() && *newCommand->options.cwd == "/work" &&
                          newCommand->prompt == "prompt text",
                      "new remains a CLI-owned two-operation typed workflow without application request IDs");

    const std::string knownRaw =
        R"({"protocol":"snodec.codex-frontend","version":1,"kind":"command","requestId":"ignored-by-sdk","method":"snapshot.get","params":{}})";
    const client::ParsedCommand rawParsed = parser.parse("raw " + knownRaw);
    frontend::generated::MethodId rawMethod = frontend::generated::MethodId::ControllerAcquire;
    const frontend::Json* const raw = rawParameters(rawParsed, rawMethod);
    result.expectTrue(raw != nullptr && rawMethod == frontend::generated::MethodId::SnapshotGet && *raw == frontend::Json::object(),
                      "raw alone retains the schema-validated generated operation API and discards its caller request ID");

    const std::string unknownRaw =
        R"({"protocol":"snodec.codex-frontend","version":1,"kind":"command","requestId":"raw","method":"unknown.method","params":{}})";
    result.expectTrue(std::holds_alternative<client::CommandParseError>(parser.parse("raw " + unknownRaw)),
                      "raw rejects unknown string-and-JSON methods");

    for (const std::string_view invalid : {"replay nope", "read", "turn thread-7", "interrupt thread-7", "watch maybe", "raw {not-json"}) {
        result.expectTrue(std::holds_alternative<client::CommandParseError>(parser.parse(invalid)),
                          "invalid input is rejected locally: " + std::string(invalid));
    }

    constexpr std::string_view SensitiveMarker = "bearer-secret-must-not-be-echoed";
    for (const std::string& input :
         {std::string(SensitiveMarker), "start --" + std::string(SensitiveMarker), "start " + std::string(SensitiveMarker)}) {
        const client::ParsedCommand parsed = parser.parse(input);
        const auto* const error = std::get_if<client::CommandParseError>(&parsed);
        result.expectTrue(error != nullptr && error->message.find(SensitiveMarker) == std::string::npos,
                          "parser diagnostics reject arbitrary input without echoing command or parameter material");
    }

    const std::string help = client::CommandParser::helpText();
    result.expectTrue(help.find("reconnect") != std::string::npos && help.find("snapshot") != std::string::npos &&
                          help.find("new") != std::string::npos && help.find("raw") != std::string::npos,
                      "help documents the migrated CLI operations");

    return result.processResult();
}
