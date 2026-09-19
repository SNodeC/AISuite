/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#include "ai/openai/codex/frontend/CodexAgent.h"

#include "TestHarness.h"

#include <vector>
int main() {
    using Json = nlohmann::json;
    namespace a = ai::agent;
    tests::codex::TestHarness test;
    std::vector<Json> sent;
    ai::openai::codex::frontend::CodexAgent agent([&](const Json& message) {
        sent.push_back(message);
        return true;
    });
    auto& backend = agent.backend();
    backend.receive({{"kind", "bridge.connection"}, {"event", "opened"}, {"connectionId", "ui"}, {"role", "controller"}, {"seq", 1}});
    backend.receive({{"kind", "bridge.provider"}, {"state", "ready"}, {"providerGeneration", 1}, {"seq", 2}});
    std::string threadId;
    a::Agent& neutral = agent;
    neutral.createConversation({"codex", "anthropic", "configured-model"}, "/tmp", [&](a::Result result) {
        threadId = result.id;
    });
    test.expect(sent.back()["payload"]["params"]["modelProvider"] == "anthropic" &&
                    sent.back()["payload"]["params"]["model"] == "configured-model",
                "normalized selection reaches existing backend API");
    const auto id = sent.back()["payload"]["id"];
    backend.receive({{"kind", "appserver"}, {"payload", {{"id", id}, {"result", {{"thread", {{"id", "thread"}}}}}}}});
    test.expect(threadId == "thread", "conversation ID comes from authoritative agent response");
    std::vector<a::Event> events;
    neutral.onEvent([&](const auto& e) {
        events.push_back(e);
    });
    auto notify = [&](const char* method, Json parameters) {
        backend.receive({{"kind", "appserver"}, {"payload", {{"method", method}, {"params", std::move(parameters)}}}});
    };
    notify("turn/started", {{"threadId", "thread"}, {"turn", {{"id", "turn"}}}});
    notify("item/agentMessage/delta", {{"threadId", "thread"}, {"turnId", "turn"}, {"delta", "hello"}});
    notify("item/completed", {{"threadId", "thread"}, {"turnId", "turn"}, {"item", {{"id", "tool"}, {"type", "commandExecution"}}}});
    notify("turn/completed", {{"threadId", "thread"}, {"turn", {{"id", "turn"}, {"status", "completed"}}}});
    test.expect(events.size() == 4 && std::get<a::TextDelta>(events[1]).text == "hello" && std::get<a::ToolActivity>(events[2]).completed &&
                    std::get<a::ToolActivity>(events[2]).kind == a::ToolKind::Command &&
                    std::get<a::TurnFinished>(events[3]).status == a::TurnStatus::Completed,
                "agent events normalized without provider wire details or a second state store");
    neutral.startTurn("thread", "read file", {});
    test.expect(sent.back()["payload"]["method"] == "turn/start", "neutral turn uses existing request router");
    bool disconnected = false;
    neutral.interrupt("thread", "turn", [&](a::Result result) {
        disconnected = !result;
    });
    backend.transportDisconnected("test disconnect");
    test.expect(disconnected, "pending normalized action receives transport failure");
    return test.result();
}
