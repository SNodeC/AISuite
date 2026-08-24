/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-bridge-client/ClientSession.h"

#include "ai/openai/codex/frontend/CodexBridge.h"
#include "apps/codex-bridge-client/Presenter.h"

#include <type_traits>
#include <utility>

namespace apps::codex_bridge_client {

    namespace codex = ai::openai::codex;

    ClientSession::ClientSession(codex::frontend::CodexBridge& sdk,
                                 Presenter& presenter,
                                 std::function<void()> reconnect,
                                 std::function<void()> quit)
        : sdk_(sdk)
        , presenter_(presenter)
        , reconnect_(std::move(reconnect))
        , quit_(std::move(quit)) {
        sdk_.onRawJson([this](codex::protocol::AppServerDirection direction, const nlohmann::json& message) {
            if (direction == codex::protocol::AppServerDirection::FromAppServer) {
                presenter_.appServerMessage(message);
            }
        });
        sdk_.onBridgeEvent([this](const nlohmann::json& message) { presenter_.bridgeEvent(message); });
    }

    void ClientSession::execute(ParsedCommand command) {
        std::visit(
            [this]<typename Command>(Command&& value) {
                using T = std::remove_cvref_t<Command>;
                if constexpr (std::is_same_v<T, NoopCommand>) {
                    return;
                } else if constexpr (std::is_same_v<T, HelpCommand>) {
                    presenter_.localMessage(CommandParser::helpText());
                } else if constexpr (std::is_same_v<T, QuitCommand>) {
                    quit_();
                } else if constexpr (std::is_same_v<T, ReconnectCommand>) {
                    reconnect_();
                } else if constexpr (std::is_same_v<T, WatchCommand>) {
                    presenter_.setWatchEnabled(value.enabled);
                    presenter_.localMessage(value.enabled ? "watch on" : "watch off");
                } else if constexpr (std::is_same_v<T, SnapshotCommand>) {
                    presenter_.localMessage("snapshot is a transient fresh thread/list query");
                    sdk_.threadList(codex::generated::v2::ThreadListParams(nlohmann::json::object()),
                                    [this](auto& response) { reportError(response); });
                } else if constexpr (std::is_same_v<T, ReplayCommand>) {
                    static_cast<void>(value.sequence);
                    presenter_.error("replay is unavailable because codex-bridge retains no event log");
                } else if constexpr (std::is_same_v<T, ControllerAcquireCommand>) {
                    if (!sdk_.claimController()) {
                        presenter_.error("controller claim could not be sent");
                    }
                } else if constexpr (std::is_same_v<T, ControllerReleaseCommand>) {
                    if (!sdk_.releaseController()) {
                        presenter_.error("controller release could not be sent");
                    }
                } else if constexpr (std::is_same_v<T, ThreadListCommand>) {
                    sdk_.threadList(value.parameters, [this](auto& response) { reportError(response); });
                } else if constexpr (std::is_same_v<T, ThreadStartCommand>) {
                    sdk_.threadStart(value.parameters, [this](auto& response) { reportError(response); });
                } else if constexpr (std::is_same_v<T, ThreadResumeCommand>) {
                    sdk_.threadResume(value.parameters, [this](auto& response) { reportError(response); });
                } else if constexpr (std::is_same_v<T, ThreadReadCommand>) {
                    sdk_.threadRead(value.parameters, [this](auto& response) { reportError(response); });
                } else if constexpr (std::is_same_v<T, TurnStartCommand>) {
                    sdk_.turnStart(value.parameters, [this](auto& response) { reportError(response); });
                } else if constexpr (std::is_same_v<T, TurnInterruptCommand>) {
                    sdk_.turnInterrupt(value.parameters, [this](auto& response) { reportError(response); });
                } else if constexpr (std::is_same_v<T, RawCommand>) {
                    if (!sdk_.sendRawJson(value.message)) {
                        presenter_.error("raw app-server message was rejected");
                    }
                } else if constexpr (std::is_same_v<T, NewCommand>) {
                    startNew(std::move(value));
                } else if constexpr (std::is_same_v<T, CommandParseError>) {
                    presenter_.error(value.message);
                }
            },
            std::move(command));
    }

    void ClientSession::reportSdkError(std::string message) {
        presenter_.error(message);
    }

    void ClientSession::startNew(NewCommand command) {
        sdk_.threadStart(command.options, [this, prompt = std::move(command.prompt)](auto& response) mutable {
            if (!response) {
                reportError(response);
                return;
            }
            const std::optional<std::string> threadId = response.thread().id();
            if (!threadId) {
                presenter_.error("thread/start response omitted thread id");
                return;
            }
            codex::generated::v2::TurnStartParams turn(
                {{"threadId", *threadId}, {"input", {{{"type", "text"}, {"text", std::move(prompt)}}}}});
            sdk_.turnStart(turn, [this](auto& turnResponse) { reportError(turnResponse); });
        });
    }

} // namespace apps::codex_bridge_client
