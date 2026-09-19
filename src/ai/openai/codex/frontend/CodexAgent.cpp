/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#include "ai/openai/codex/frontend/CodexAgent.h"

#include <utility>
namespace ai::openai::codex::frontend {
    namespace a = ai::agent;
    namespace v2 = generated::v2;
    namespace {
        template <typename Response>
        a::Result result(const Response& response, const char* field) {
            if (!response)
                return {{}, response.jsonRpcErrorMessage().value_or("agent request failed")};
            if (!field)
                return {};
            const auto& payload = response.getPayload();
            if (!payload.contains(field) || !payload.at(field).contains("id") || !payload.at(field).at("id").is_string())
                return {{}, "invalid agent response: missing ID"};
            return {payload.at(field).at("id").template get<std::string>(), {}};
        }
    } // namespace
    CodexAgent::CodexAgent(CodexBridge::Sender sender)
        : bridge_(std::move(sender)) {
        bridge_.onTurnStarted([this](v2::TurnStartedNotification& event) {
            emit(a::TurnStarted{event.threadId().value_or(""), event.turn().id().value_or("")});
        });
        bridge_.onItemAgentMessageDelta([this](v2::AgentMessageDeltaNotification& event) {
            emit(a::TextDelta{event.threadId().value_or(""), event.turnId().value_or(""), event.delta().value_or("")});
        });
        auto activity = [this](const nlohmann::json& event, bool completed) {
            const auto& item = event.at("item");
            const auto type = item.value("type", "");
            if (type == "commandExecution" || type == "fileChange" || type == "mcpToolCall" || type == "dynamicToolCall")
                emit(a::ToolActivity{event.value("threadId", ""),
                                     event.value("turnId", ""),
                                     item.value("id", ""),
                                     type == "commandExecution" ? a::ToolKind::Command
                                     : type == "fileChange"     ? a::ToolKind::FileChange
                                                                : a::ToolKind::External,
                                     completed});
        };
        bridge_.onItemStarted([activity](v2::ItemStartedNotification& event) {
            activity(event.getPayload(), false);
        });
        bridge_.onItemCompleted([activity](v2::ItemCompletedNotification& event) {
            activity(event.getPayload(), true);
        });
        bridge_.onTurnCompleted([this](v2::TurnCompletedNotification& event) {
            const auto& turn = event.getPayload().at("turn");
            const auto status = turn.value("status", "failed");
            std::string error;
            if (turn.contains("error") && turn["error"].is_object())
                error = turn["error"].value("message", "agent turn failed");
            emit(a::TurnFinished{event.threadId().value_or(""),
                                 turn.value("id", ""),
                                 status == "completed"     ? a::TurnStatus::Completed
                                 : status == "interrupted" ? a::TurnStatus::Interrupted
                                                           : a::TurnStatus::Failed,
                                 error});
        });
    }
    void CodexAgent::createConversation(const a::Selection& selection, const std::string& cwd, Callback callback) {
        if (selection.agent != "codex") {
            if (callback)
                callback({{}, "CodexAgent requires agent=codex"});
            return;
        }
        nlohmann::json parameters = nlohmann::json::object();
        if (!cwd.empty())
            parameters["cwd"] = cwd;
        if (!selection.model.empty())
            parameters["model"] = selection.model;
        if (!selection.provider.empty())
            parameters["modelProvider"] = selection.provider;
        bridge_.threadStart(v2::ThreadStartParams(parameters), [callback = std::move(callback)](v2::ThreadStartResponse& response) {
            if (callback)
                callback(result(response, "thread"));
        });
    }
    void CodexAgent::startTurn(const std::string& id, const std::string& text, Callback callback) {
        bridge_.turnStart(v2::TurnStartParams({{"threadId", id}, {"input", {{{"type", "text"}, {"text", text}}}}}),
                          [callback = std::move(callback)](v2::TurnStartResponse& response) {
                              if (callback)
                                  callback(result(response, "turn"));
                          });
    }
    void CodexAgent::interrupt(const std::string& id, const std::string& turn, Callback callback) {
        bridge_.turnInterrupt(v2::TurnInterruptParams({{"threadId", id}, {"turnId", turn}}),
                              [callback = std::move(callback)](v2::TurnInterruptResponse& response) {
                                  if (callback)
                                      callback(result(response, nullptr));
                              });
    }
    void CodexAgent::onEvent(Receiver receiver) {
        receiver_ = std::move(receiver);
    }
    CodexBridge& CodexAgent::backend() noexcept {
        return bridge_;
    }
    void CodexAgent::emit(const a::Event& event) {
        const auto receiver = receiver_;
        if (receiver)
            receiver(event);
    }
} // namespace ai::openai::codex::frontend
