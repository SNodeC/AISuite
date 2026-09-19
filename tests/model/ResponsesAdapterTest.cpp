/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#include "ai/openai/codex/model/ResponsesAdapter.h"

#include "TestHarness.h"
#include "ai/openai/codex/model/ProviderProfile.h"
#include "ai/providers/anthropic/AnthropicProvider.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>

namespace {
    namespace n = ai::model;
    namespace c = ai::openai::codex::model;
    using Json = nlohmann::json;
    Json request() {
        return {{"model", "test-claude"},
                {"instructions", "Code carefully"},
                {"stream", true},
                {"store", false},
                {"reasoning", {{"effort", "none"}}},
                {"include", {"reasoning.encrypted_content"}},
                {"input",
                 Json::array({{{"role", "developer"}, {"content", Json::array({{{"type", "input_text"}, {"text", "Preserve changes"}}})}},
                              {{"role", "user"}, {"content", Json::array({{{"type", "input_text"}, {"text", "Fix bug"}}})}}})},
                {"tools",
                 Json::array(
                     {{{"type", "namespace"},
                       {"name", "functions"},
                       {"tools",
                        Json::array(
                            {{{"type", "function"}, {"name", "read"}, {"description", "Read file"}, {"parameters", {{"type", "object"}}}},
                             {{"type", "custom"},
                              {"name", "apply_patch"},
                              {"description", "Patch file"},
                              {"format", {{"type", "text"}}}}})}}})}};
    }
    class MockProvider final : public n::Provider {
    public:
        n::ProviderInfo registration{"test", "Test", {{"test-claude", "Test Claude"}}};
        const n::ProviderInfo& info() const noexcept override {
            return registration;
        }
        n::Request last;
        Receiver receiver;
        bool cancelled = false;
        class Operation final : public n::Operation {
        public:
            explicit Operation(MockProvider& owner)
                : owner_(owner) {
            }
            ~Operation() override {
                cancel();
            }
            void cancel() noexcept override {
                owner_.cancelled = true;
                owner_.receiver = {};
            }

        private:
            MockProvider& owner_;
        };
        std::unique_ptr<n::Operation> start(const n::Request& value, Receiver callback) override {
            last = value;
            receiver = std::move(callback);
            cancelled = false;
            return std::make_unique<Operation>(*this);
        }
        void emit(const n::Event& value) {
            const auto callback = receiver;
            if (callback)
                callback(value);
        }
    };
    class MockHttp final : public ai::http::StreamingClient {
    public:
        ai::http::Request request;
        ai::http::Receiver receiver;
        class Operation final : public n::Operation {
        public:
            void cancel() noexcept override {
            }
        };
        std::unique_ptr<n::Operation> post(ai::http::Request value, ai::http::Receiver callback) override {
            request = std::move(value);
            receiver = std::move(callback);
            return std::make_unique<Operation>();
        }
        void event(Json value) {
            const std::string s = "event: " + value.at("type").get<std::string>() + "\ndata: " + value.dump() + "\n\n";
            // Split at HTTP boundaries, including inside JSON escapes and SSE lines.
            for (std::size_t at = 0; at < s.size(); at += 7)
                receiver.data(std::string_view(s).substr(at, 7));
        }
        void respond(Json block, std::string deltaType, std::string deltaKey, std::string delta, std::string stop) {
            receiver.headers(200, {{"content-type", "text/event-stream"}});
            event({{"type", "message_start"},
                   {"message",
                    {{"id", "msg_test"},
                     {"role", "assistant"},
                     {"content", Json::array()},
                     {"usage", {{"input_tokens", 10}, {"output_tokens", 1}}}}}});
            event({{"type", "content_block_start"}, {"index", 0}, {"content_block", std::move(block)}});
            event({{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", deltaType}, {deltaKey, delta}}}});
            event({{"type", "content_block_stop"}, {"index", 0}});
            event({{"type", "message_delta"}, {"delta", {{"stop_reason", stop}}}, {"usage", {{"output_tokens", 5}}}});
            event({{"type", "message_stop"}});
            receiver.completed();
        }
    };
} // namespace
int main() {
    tests::codex::TestHarness test;
    auto run = [&](const char* name, auto body) {
        try {
            body();
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
            test.expect(false, name);
        }
    };
    run("request profile", [&] {
        const auto parsed = c::parseResponsesRequest(request());
        test.expect(parsed.request.instructions == "Code carefully\n\nPreserve changes", "leading developer instructions preserved");
        test.expect(parsed.request.tools.size() == 2 && parsed.request.tools[1].name == "functions.apply_patch",
                    "namespace and custom input schema neutralized");
        for (const auto& field : {"previous_response_id", "background", "conversation"}) {
            auto bad = request();
            bad[field] = "unsupported";
            bool rejected = false;
            try {
                c::parseResponsesRequest(bad);
            } catch (...) {
                rejected = true;
            }
            test.expect(rejected, "unsupported Responses behavior rejected explicitly");
        }
        auto bad = request();
        bad["reasoning"]["effort"] = "high";
        bool rejected = false;
        try {
            c::parseResponsesRequest(bad);
        } catch (...) {
            rejected = true;
        }
        test.expect(rejected, "no fabricated OpenAI/Claude reasoning translation");
    });
    run("unsupported profile semantics", [&] {
        std::vector<Json> cases;
        auto bad = request();
        bad["input"][1]["content"][0] = {{"type", "input_image"}, {"image_url", "https://example.invalid/image"}};
        cases.push_back(bad);
        bad = request();
        bad["tools"] = Json::array({{{"type", "web_search"}}});
        cases.push_back(bad);
        bad = request();
        bad["max_output_tokens"] = std::uint64_t{0};
        cases.push_back(bad);
        bad = request();
        bad["input"].push_back({{"type", "function_call_output"}, {"call_id", "missing"}, {"output", "text"}});
        cases.push_back(bad);
        bad = request();
        bad["text"] = {{"format", {{"type", "json_schema"}}}};
        cases.push_back(bad);
        for (const auto& input : cases) {
            bool rejected = false;
            try {
                c::parseResponsesRequest(input);
            } catch (...) {
                rejected = true;
            }
            test.expect(rejected, "unsupported behavior rejected instead of lossy conversion");
        }
    });
    run("mock provider streaming", [&] {
        MockProvider provider;
        std::vector<Json> events;
        unsigned finished = 0;
        auto operation = c::startResponse(
            provider,
            c::parseResponsesRequest(request()),
            "resp_test",
            [&](const Json& e) {
                events.push_back(e);
                return true;
            },
            [&] {
                ++finished;
            });
        provider.emit(n::Started{"native-id"});
        provider.emit(n::BlockStarted{0, n::Text{""}});
        provider.emit(n::TextDelta{0, "Reading"});
        provider.emit(n::BlockFinished{0, n::Text{"Reading"}});
        provider.emit(n::BlockStarted{1, n::ToolCall{"call_1", "functions.read", Json::object()}});
        provider.emit(n::ToolInputDelta{1, "{\"path\":"});
        provider.emit(n::ToolInputDelta{1, "\"a.cpp\"}"});
        provider.emit(n::BlockFinished{1, n::ToolCall{"call_1", "functions.read", {{"path", "a.cpp"}}}});
        provider.emit(n::Usage{10, 20, 3, 4});
        provider.emit(n::Completed{n::StopReason::ToolUse});
        test.expect(finished == 1 && events.front()["type"] == "response.created" && events.back()["type"] == "response.completed",
                    "response lifecycle completes exactly once");
        const Json response = events.back().at("response");
        test.expect(response["end_turn"] == false && response["usage"]["total_tokens"] == 37 && response["usage"]["input_tokens"] == 17,
                    "usage and tool continuation semantics");
        const auto call = response.at("output")[1];
        test.expect(call["namespace"] == "functions" && call["call_id"] == "call_1" && call["arguments"] == R"({"path":"a.cpp"})",
                    "complete function item has arguments required by Codex parser");
        auto next = request();
        next["input"].push_back(call);
        next["input"].push_back(
            {{"type", "message"}, {"role", "assistant"}, {"content", {{{"type", "output_text"}, {"text", "I am reading that file."}}}}});
        next["input"].push_back({{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "file contents"}});
        const auto continuation = c::parseResponsesRequest(next);
        test.expect(std::get<n::ToolResult>(continuation.request.messages.back().content[0]).text == "file contents",
                    "tool result round trips to neutral history");
        operation->cancel();
        test.expect(provider.cancelled, "disconnect cancellation reaches provider");
        const auto size = events.size();
        provider.emit(n::TextDelta{0, "late"});
        test.expect(events.size() == size, "no events after cancel");
    });
    run("assistant message phases", [&] {
        for (const auto reason : {n::StopReason::EndTurn, n::StopReason::StopSequence, n::StopReason::Refusal,
                                  n::StopReason::ToolUse, n::StopReason::OutputLimit, n::StopReason::Pause}) {
            std::vector<Json> events;
            c::ResponsesStream stream(c::parseResponsesRequest(request()), "resp_phases", [&](const Json& e) {
                events.push_back(e);
                return true;
            });
            stream.receive(n::Started{"id"});
            auto text = [&](std::size_t index) {
                stream.receive(n::BlockStarted{index, n::Text{""}});
                stream.receive(n::TextDelta{index, "Answer"});
                test.expect(events.back()["type"] == "response.output_text.delta", "text streams before its phase is known");
                stream.receive(n::BlockFinished{index, n::Text{"Answer"}});
            };
            text(0);
            if (reason == n::StopReason::ToolUse) {
                stream.receive(n::BlockStarted{1, n::ToolCall{"call", "functions.read", Json::object()}});
                stream.receive(n::BlockFinished{1, n::ToolCall{"call", "functions.read", Json::object()}});
                text(2);
            } else {
                text(1);
            }
            stream.receive(n::Completed{reason});
            const bool failed = reason == n::StopReason::OutputLimit || reason == n::StopReason::Pause;
            unsigned done = 0, finals = 0;
            for (const auto& event : events) {
                if (event["type"] != "response.output_item.done" || event["item"]["type"] != "message")
                    continue;
                const bool final = !failed && reason != n::StopReason::ToolUse && done == 1;
                test.expect(event["item"].value("phase", "") == (final ? "final_answer" : "commentary"),
                            "only the last message of a successfully ended turn is final");
                finals += event["item"].value("phase", "") == "final_answer";
                if (!failed)
                    test.expect(events.back()["response"]["output"][event["output_index"].get<std::size_t>()] == event["item"],
                                "completed response and done item retain identical phase and content");
                ++done;
            }
            test.expect(done == (failed ? 1U : 2U), "each resolved message completes exactly once");
            test.expect(finals == (!failed && reason != n::StopReason::ToolUse ? 1U : 0U),
                        "tool commentary and interrupted responses never become final answers");
        }
    });
    run("custom tool fragmented Unicode", [&] {
        std::vector<Json> events;
        c::ResponsesStream stream(c::parseResponsesRequest(request()), "resp_patch", [&](const Json& e) {
            events.push_back(e);
            return true;
        });
        stream.receive(n::Started{"id"});
        stream.receive(n::BlockStarted{0, n::ToolCall{"call_patch", "functions.apply_patch", Json::object()}});
        const std::string encoded = R"({"input":"*** Begin Patch\n+\uD83D\uDE00\n*** End Patch"})";
        for (char byte : encoded)
            stream.receive(n::ToolInputDelta{0, std::string(1, byte)});
        const Json input = Json::parse(encoded);
        stream.receive(n::BlockFinished{0, n::ToolCall{"call_patch", "functions.apply_patch", input}});
        stream.receive(n::Completed{n::StopReason::ToolUse});
        std::string combined;
        for (const auto& e : events)
            if (e["type"] == "response.custom_tool_call_input.delta")
                combined += e.at("delta").get<std::string>();
        test.expect(combined == input.at("input").get<std::string>() && events.back()["type"] == "response.completed",
                    "custom deltas decode escapes and surrogate pairs incrementally");
        test.expect(events.back()["response"]["output"][0]["input"] == combined, "custom output is freeform, not JSON wrapper");
    });
    run("errors and disconnect", [&] {
        for (const auto reason : {n::StopReason::OutputLimit, n::StopReason::Pause}) {
            std::vector<Json> events;
            c::ResponsesStream stream(c::parseResponsesRequest(request()), "resp_bad", [&](const Json& e) {
                events.push_back(e);
                return true;
            });
            stream.receive(n::Started{"id"});
            stream.receive(n::Completed{reason});
            test.expect(events.back()["type"] == "response.failed", "incomplete inference never reported as success");
        }
        MockProvider provider;
        unsigned finished = 0;
        auto operation = c::startResponse(
            provider,
            c::parseResponsesRequest(request()),
            "id",
            [](const Json&) {
                return false;
            },
            [&] {
                ++finished;
            });
        provider.emit(n::Started{"id"});
        test.expect(finished == 1, "failed SSE admission terminates response");
        operation->cancel();
        test.expect(provider.cancelled, "failed SSE admission cancellation is idempotent");
    });
    run("native Anthropic multi-inference bridge", [&] {
        MockHttp http;
        ai::providers::anthropic::AnthropicProvider provider({.apiKey = "test-key", .models = {{"test-claude", "Test"}}}, http);
        std::vector<Json> events;
        auto sender = [&](const Json& e) {
            events.push_back(e);
            return true;
        };
        auto wire = request();
        auto operation = c::startResponse(provider, c::parseResponsesRequest(wire), "resp_first", sender, [] {
        });
        const auto body = Json::parse(http.request.body);
        const auto nativeName = body["tools"][0]["name"];
        http.respond({{"type", "tool_use"}, {"id", "call_read"}, {"name", nativeName}, {"input", Json::object()}},
                     "input_json_delta",
                     "partial_json",
                     R"({"path":"source.cpp"})",
                     "tool_use");
        test.expect(events.back()["type"] == "response.completed", "Responses -> neutral -> native streaming tool call");
        wire["input"].push_back(events.back()["response"]["output"][0]);
        wire["input"].push_back({{"type", "function_call_output"}, {"call_id", "call_read"}, {"output", "return 1;"}});
        operation.reset();
        events.clear();
        operation = c::startResponse(provider, c::parseResponsesRequest(wire), "resp_second", sender, [] {
        });
        const auto continuation = Json::parse(http.request.body);
        test.expect(continuation["messages"][1]["content"][0]["type"] == "tool_use" &&
                        continuation["messages"][2]["content"][0]["type"] == "tool_result" &&
                        continuation["messages"][2]["content"][0]["tool_use_id"] == "call_read",
                    "native tool state preserved without response cache");
        http.respond({{"type", "text"}, {"text", ""}}, "text_delta", "text", "Inspected source.cpp; return value is 1.", "end_turn");
        test.expect(events.back()["type"] == "response.completed" && events.back()["response"]["end_turn"] == true,
                    "final coding-agent inference terminates cleanly");
        test.expect(events[events.size() - 2]["type"] == "response.output_item.done" &&
                        events[events.size() - 2]["item"]["phase"] == "final_answer",
                    "native single-text answer carries its visible phase on the event consumed by Codex");
    });
    run("provider-owned model catalog", [&] {
        const n::ProviderInfo provider{"anthropic", "Anthropic", {{"configured-model", "Configured model"}}};
        const auto catalog = c::ProviderProfile::catalog(provider, 200000);
        test.expect(catalog["models"][0]["slug"] == "configured-model" &&
                        catalog["models"][0]["supports_reasoning_summary_parameter"] == false,
                    "AISuite owns model discovery and conservative capabilities");
        std::string directory = "/tmp/aisuite-model-test-XXXXXX";
        if (!::mkdtemp(directory.data()))
            throw std::runtime_error("cannot create test runtime directory");
        struct RuntimeDirectory {
            std::string path;
            std::optional<std::string> old;
            ~RuntimeDirectory() {
                if (old)
                    ::setenv("XDG_RUNTIME_DIR", old->c_str(), 1);
                else
                    ::unsetenv("XDG_RUNTIME_DIR");
                std::filesystem::remove_all(path);
            }
        } directoryGuard{directory,
                         std::getenv("XDG_RUNTIME_DIR") ? std::make_optional(std::string(std::getenv("XDG_RUNTIME_DIR"))) : std::nullopt};
        ::setenv("XDG_RUNTIME_DIR", directory.c_str(), 1);
        std::string path;
        {
            c::ProviderProfile profile(provider, 200000);
            path = profile.catalogPath();
            struct stat st{};
            test.expect(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "temporary catalog private");
            ai::openai::codex::provider::StdioAppServerOptions options;
            profile.configure(options, "configured-model", "http://127.0.0.1:1234", "local-token");
            test.expect(options.arguments.front() == "app-server" && options.environment.back().first == "AISUITE_MODEL_TOKEN",
                        "supported Codex custom provider configuration, no fork");
            for (const auto& arg : options.arguments)
                test.expect(arg.find("local-token") == std::string::npos, "credential absent from process arguments");
        }
        test.expect(!std::filesystem::exists(path), "temporary catalog removed by RAII");
    });
    return test.result();
}
