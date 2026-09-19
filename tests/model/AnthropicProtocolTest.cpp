/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#include "TestHarness.h"
#include "ai/http/SseDecoder.h"
#include "ai/providers/anthropic/AnthropicProvider.h"

#include <iostream>
#include <stdexcept>

namespace {
    using namespace ai;
    using Json = nlohmann::json;
    using providers::anthropic::Configuration;
    using providers::anthropic::MessageStream;
    using providers::anthropic::prepareRequest;
    Json schema() {
        return {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}};
    }
    model::Request request() {
        return {.model = "test-claude",
                .instructions = "Code carefully",
                .messages = {{model::Role::User, {model::Text{"Read a file"}}}},
                .tools = {{"read", "Read a file", schema()}, {"tools.patch", "Patch a file", schema()}}};
    }
    Configuration config() {
        return {.apiKey = "test-not-a-secret", .models = {{"test-claude", "Test Claude"}}};
    }
    void event(MessageStream& stream, const Json& value) {
        stream.event(value.at("type").get<std::string>(), value.dump());
    }
    Json start() {
        return {{"type", "message_start"},
                {"message",
                 {{"id", "msg_1"},
                  {"role", "assistant"},
                  {"content", Json::array()},
                  {"usage", {{"input_tokens", 11}, {"output_tokens", 1}, {"cache_read_input_tokens", 3}}}}}};
    }
    Json blockStart(unsigned index, Json block) {
        return {{"type", "content_block_start"}, {"index", index}, {"content_block", std::move(block)}};
    }
    Json delta(unsigned index, Json value) {
        return {{"type", "content_block_delta"}, {"index", index}, {"delta", std::move(value)}};
    }
    Json blockStop(unsigned index) {
        return {{"type", "content_block_stop"}, {"index", index}};
    }
    void finish(MessageStream& stream, std::string reason) {
        event(stream, {{"type", "message_delta"}, {"delta", {{"stop_reason", std::move(reason)}}}, {"usage", {{"output_tokens", 15}}}});
        event(stream, {{"type", "message_stop"}});
    }
    class MockHttp final : public http::StreamingClient {
    public:
        struct State {
            bool cancelled = false;
            http::Receiver receiver;
        };
        class Operation final : public model::Operation {
        public:
            explicit Operation(std::shared_ptr<State> state)
                : state_(std::move(state)) {
            }
            ~Operation() override {
                cancel();
            }
            void cancel() noexcept override {
                state_->cancelled = true;
                state_->receiver = {};
            }

        private:
            std::shared_ptr<State> state_;
        };
        std::unique_ptr<model::Operation> post(http::Request request, http::Receiver receiver) override {
            wire = std::move(request);
            state = std::make_shared<State>();
            state->receiver = std::move(receiver);
            return std::make_unique<Operation>(state);
        }
        void send(const Json& value) {
            const auto bytes = "event: " + value.at("type").get<std::string>() + "\r\ndata: " + value.dump() + "\r\n\r\n";
            for (char byte : bytes)
                if (state->receiver.data)
                    state->receiver.data(std::string_view(&byte, 1));
        }
        http::Request wire;
        std::shared_ptr<State> state;
    };
} // namespace
int main() {
    tests::codex::TestHarness test;
    auto run = [&](const char* name, auto body) {
        try {
            body();
        } catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            test.expect(false, name);
        }
    };
    run("neutral/native requests", [&] {
        auto r = request();
        const auto c = config();
        auto wire = prepareRequest(r, c);
        test.expect(wire.body.at("system") == r.instructions && wire.body.at("stream") == true &&
                        wire.body.at("thinking").at("type") == "disabled",
                    "native system, streaming and conservative thinking");
        test.expect(wire.body.at("tools")[0].at("input_schema") == schema(), "native tool declaration");
        const auto alias = wire.body.at("tools")[1].at("name").get<std::string>();
        test.expect(wire.toolNames.at(alias) == "tools.patch", "provider-specific name encoding stays at provider boundary");
        auto reordered = r;
        std::swap(reordered.tools[0], reordered.tools[1]);
        test.expect(prepareRequest(reordered, c).body.at("tools")[0].at("name") == alias,
                    "native aliases are stable across tool reordering and continuation");
        auto collision = r;
        collision.tools.push_back({alias, "Conflicting native name", schema()});
        bool collisionRejected = false;
        try {
            prepareRequest(collision, c);
        } catch (...) {
            collisionRejected = true;
        }
        test.expect(collisionRejected, "native name collisions fail instead of routing to the wrong tool");
        r.messages.push_back({model::Role::Assistant,
                              {model::Text{"Reading"},
                               model::ToolCall{"a", "read", {{"path", "a.cpp"}}},
                               model::ToolCall{"b", "read", {{"path", "b.cpp"}}}}});
        r.messages.push_back({model::Role::User, {model::ToolResult{"a", "file a", false}}});
        r.messages.push_back({model::Role::User, {model::ToolResult{"b", "missing file", true}, model::Text{"Fix it"}}});
        wire = prepareRequest(r, c);
        test.expect(wire.body.at("messages").size() == 3 && wire.body.at("messages")[2].at("content").size() == 3,
                    "multiple tool results grouped in one user message");
        test.expect(wire.body.at("messages")[2].at("content")[1].at("is_error") == true, "tool failure preserved");
        auto withoutTools = r;
        withoutTools.tools.clear();
        test.expect(prepareRequest(withoutTools, c).toolNames.empty(), "historical tool names do not authorize new calls to removed tools");
        r.messages.back().content.erase(r.messages.back().content.begin());
        bool rejected = false;
        try {
            static_cast<void>(prepareRequest(r, c));
        } catch (...) {
            rejected = true;
        }
        test.expect(rejected, "missing tool result rejected");
    });
    run("text and tool streaming", [&] {
        std::vector<model::Event> events;
        MessageStream stream(
            [&](const auto& e) {
                events.push_back(e);
            },
            {{"read", "read"}});
        event(stream, start());
        event(stream, blockStart(0, {{"type", "text"}, {"text", ""}}));
        event(stream, delta(0, {{"type", "text_delta"}, {"text", "Reading files"}}));
        event(stream, blockStop(0));
        for (unsigned i = 1; i <= 2; ++i) {
            event(stream,
                  blockStart(i, {{"type", "tool_use"}, {"id", "call_" + std::to_string(i)}, {"name", "read"}, {"input", Json::object()}}));
            event(stream, delta(i, {{"type", "input_json_delta"}, {"partial_json", "{\"path\":"}}));
            event(stream, delta(i, {{"type", "input_json_delta"}, {"partial_json", "\"file.cpp\"}"}}));
            event(stream, blockStop(i));
        }
        finish(stream, "tool_use");
        stream.finish();
        test.expect(std::get<model::Completed>(events.back()).reason == model::StopReason::ToolUse,
                    "tool use completes once with explicit stop reason");
        unsigned calls = 0;
        unsigned fragments = 0;
        for (const auto& e : events) {
            if (const auto* block = std::get_if<model::BlockFinished>(&e)) {
                if (const auto* tool = std::get_if<model::ToolCall>(&block->content)) {
                    ++calls;
                    test.expect(tool->input.at("path") == "file.cpp", "streamed tool input parsed on block completion");
                }
            }
            if (std::holds_alternative<model::ToolInputDelta>(e))
                ++fragments;
        }
        test.expect(calls == 2 && fragments == 4, "multiple tool calls and all input deltas delivered");
        const auto usage = std::get<model::Usage>(events[events.size() - 2]);
        test.expect(usage.inputTokens == 11 && usage.outputTokens == 15 && usage.cacheReadTokens == 3,
                    "cumulative usage not double counted");
    });
    run("thinking continuation", [&] {
        std::vector<model::Event> events;
        MessageStream stream(
            [&](const auto& e) {
                events.push_back(e);
            },
            {},
            true);
        event(stream, start());
        event(stream, blockStart(0, {{"type", "thinking"}, {"thinking", ""}, {"signature", ""}}));
        event(stream, delta(0, {{"type", "thinking_delta"}, {"thinking", "check"}}));
        event(stream, delta(0, {{"type", "signature_delta"}, {"signature", "signed"}}));
        event(stream, blockStop(0));
        auto reasoning = std::get<model::Reasoning>(std::get<model::BlockFinished>(events.back()).content);
        test.expect(reasoning.text == "check" && Json::parse(reasoning.continuation).at("signature") == "signed",
                    "thinking signature preserved opaquely");
        auto r = request();
        auto c = config();
        c.thinkingBudgetTokens = 1024;
        r.messages.push_back({model::Role::Assistant, {reasoning, model::ToolCall{"a", "read", Json::object()}}});
        r.messages.push_back({model::Role::User, {model::ToolResult{"a", "file", false}}});
        test.expect(prepareRequest(r, c).body.at("messages")[1].at("content")[0] == Json::parse(reasoning.continuation),
                    "signed thinking replay is lossless");
    });
    run("stream errors and cancellation", [&] {
        for (const auto& reason : {"end_turn", "max_tokens", "stop_sequence", "refusal", "pause_turn"}) {
            std::vector<model::Event> events;
            MessageStream stream(
                [&](const auto& e) {
                    events.push_back(e);
                },
                {});
            event(stream, start());
            finish(stream, reason);
            test.expect(std::holds_alternative<model::Completed>(events.back()), "known native stop reason preserved");
        }
        std::vector<model::Event> events;
        MessageStream stream(
            [&](const auto& e) {
                events.push_back(e);
            },
            {});
        event(stream, start());
        stream.finish();
        stream.finish();
        test.expect(events.size() == 3 && std::get<model::Error>(events.back()).code == "incomplete_stream",
                    "truncated stream errors exactly once");
        events.clear();
        MessageStream cancelled(
            [&](const auto& e) {
                events.push_back(e);
            },
            {});
        cancelled.cancel();
        event(cancelled, start());
        cancelled.finish();
        test.expect(events.empty(), "no callbacks after cancellation");
        MessageStream invalid(
            [&](const auto& e) {
                events.push_back(e);
            },
            {});
        invalid.event("message_start", "{invalid");
        test.expect(std::holds_alternative<model::Error>(events.back()), "malformed JSON becomes a provider error");
    });
    run("mock HTTP round trip", [&] {
        MockHttp client;
        providers::anthropic::AnthropicProvider provider(config(), client);
        std::vector<model::Event> events;
        auto operation = provider.start(request(), [&](const auto& e) {
            events.push_back(e);
        });
        test.expect(client.wire.url == "https://api.anthropic.com/v1/messages" &&
                        client.wire.headers.at("anthropic-version") == "2023-06-01" &&
                        client.wire.headers.at("x-api-key") == "test-not-a-secret",
                    "native API endpoint and authentication");
        client.state->receiver.headers(200, {{"content-type", "text/event-stream; charset=utf-8"}});
        client.send(start());
        client.send(blockStart(0, {{"type", "text"}, {"text", ""}}));
        client.send(delta(0, {{"type", "text_delta"}, {"text", "done"}}));
        client.send(blockStop(0));
        client.send({{"type", "message_delta"}, {"delta", {{"stop_reason", "end_turn"}}}, {"usage", {{"output_tokens", 2}}}});
        client.send({{"type", "message_stop"}});
        client.state->receiver.completed();
        test.expect(std::holds_alternative<model::Completed>(events.back()), "one-byte HTTP/SSE fragmentation completes");
        operation->cancel();
        test.expect(client.state->cancelled, "cancellation reaches transport");
        events.clear();
        operation = provider.start(request(), [&](const auto& e) {
            events.push_back(e);
        });
        client.state->receiver.headers(429, {{"content-type", "application/json"}});
        client.state->receiver.data(R"({"error":{"type":"rate_limit_error","message":"slow down"}})");
        client.state->receiver.completed();
        test.expect(std::get<model::Error>(events.back()).retryable, "HTTP API error and retryability preserved");
    });
    run("SSE framing bounds", [&] {
        unsigned delivered = 0;
        http::SseDecoder decoder([&](auto name, auto data) {
            test.expect(name == "test" && data == "a\nb", "multiline SSE data");
            ++delivered;
        });
        const std::string bytes = "\xef\xbb\xbf:comment\revent: test\rdata: a\rdata: b\r\r";
        for (char c : bytes)
            decoder.consume(std::string_view(&c, 1));
        test.expect(delivered == 1 && decoder.empty(), "BOM, CR and comments supported");
        http::SseDecoder bounded(
            [](auto, auto) {
            },
            8);
        bool rejected = false;
        try {
            bounded.consume("data: 123456789");
        } catch (...) {
            rejected = true;
        }
        test.expect(rejected, "SSE bounded before unbounded allocation");
    });
    run("native protocol failure boundaries", [&] {
        for (const auto& invalid :
             std::vector<Json>{{{"type", "content_block_start"}, {"content_block", {{"type", "text"}, {"text", ""}}}},
                               blockStart(0, {{"type", "server_tool_use"}}),
                               blockStart(0, {{"type", "tool_use"}, {"id", "call"}, {"name", "undeclared"}, {"input", Json::object()}}),
                               blockStart(0, {{"type", "thinking"}, {"thinking", ""}, {"signature", ""}}),
                               {{"type", "message_stop"}}}) {
            std::vector<model::Event> events;
            MessageStream stream(
                [&](const auto& e) {
                    events.push_back(e);
                },
                {});
            event(stream, start());
            event(stream, invalid);
            stream.finish();
            unsigned errors = 0;
            for (const auto& e : events)
                if (std::holds_alternative<model::Error>(e))
                    ++errors;
            test.expect(errors == 1 && stream.terminal(), "malformed/unsupported native content fails exactly once");
        }
        std::vector<model::Event> events;
        MessageStream tool(
            [&](const auto& e) {
                events.push_back(e);
            },
            {{"read", "read"}});
        event(tool, start());
        event(tool, blockStart(0, {{"type", "tool_use"}, {"id", "call"}, {"name", "read"}, {"input", Json::object()}}));
        event(tool, delta(0, {{"type", "input_json_delta"}, {"partial_json", "{bad"}}));
        event(tool, blockStop(0));
        test.expect(std::holds_alternative<model::Error>(events.back()), "invalid streamed tool JSON never becomes a finished tool call");
        MessageStream overload(
            [&](const auto& e) {
                events.push_back(e);
            },
            {});
        event(overload, {{"type", "error"}, {"error", {{"type", "overloaded_error"}, {"message", "busy"}}}});
        test.expect(std::get<model::Error>(events.back()).retryable, "native SSE API errors preserve retryability");
        MockHttp http;
        providers::anthropic::AnthropicProvider provider(config(), http);
        auto operation = provider.start(request(), [&](const auto& e) {
            events.push_back(e);
        });
        http.state->receiver.headers(200, {{"content-type", "application/json"}});
        test.expect(std::get<model::Error>(events.back()).code == "invalid_response", "non-SSE success response rejected");
    });
    run("transport validates before connecting", [&] {
        auto client = http::makeStreamingClient();
        for (const auto& url : {"ftp://provider/messages",
                                "https://key@provider/messages",
                                "https://provider:0/messages",
                                "https://provider/messages\r\nInjected: yes"}) {
            unsigned errors = 0;
            auto operation = client->post({url, {}, "{}"}, {{}, {}, {}, [&](auto) {
                                                                ++errors;
                                                            }});
            test.expect(!operation && errors == 1, "invalid URL fails without attempting a socket or leaking credentials");
        }
        unsigned errors = 0;
        auto operation =
            client->post({"https://provider/messages", {{"x-api-key", std::string("key\0suffix", 10)}}, "{}"}, {{}, {}, {}, [&](auto) {
                                                                                                                    ++errors;
                                                                                                                }});
        test.expect(!operation && errors == 1, "HTTP credential header control characters rejected");
    });
    return test.result();
}
