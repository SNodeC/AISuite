/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#include "ai/openai/codex/model/ResponsesAdapter.h"

#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ai::openai::codex::model {
    namespace neutral = ai::model;
    using Json = nlohmann::json;
    namespace {
        void require(bool value, const std::string& error) {
            if (!value)
                throw std::invalid_argument("Codex Responses profile: " + error);
        }
        std::string name(const ToolIdentity& tool) {
            return tool.nameSpace.empty() ? tool.name : tool.nameSpace + "." + tool.name;
        }
        std::string text(const Json& content) {
            if (content.is_string())
                return content.get<std::string>();
            require(content.is_array(), "content must be text or a text block array");
            std::string result;
            for (const auto& block : content) {
                const auto type = block.at("type").get<std::string>();
                require(type == "input_text" || type == "output_text", "unsupported content: " + type);
                result += block.at("text").get<std::string>();
            }
            return result;
        }
        ToolIdentity identity(const Json& tool, const std::string& ns = {}) {
            return {tool.at("name").get<std::string>(),
                    tool.contains("namespace") && !tool.at("namespace").is_null() ? tool.at("namespace").get<std::string>() : ns,
                    tool.value("type", "") == "custom" || tool.value("type", "") == "custom_tool_call"};
        }
        void merge(neutral::Request& request, neutral::Role role, neutral::Content content) {
            if (!request.messages.empty() && request.messages.back().role == role)
                request.messages.back().content.push_back(std::move(content));
            else
                request.messages.push_back({role, {std::move(content)}});
        }
    } // namespace
    ResponsesRequest parseResponsesRequest(const Json& input) {
        require(input.is_object(), "request must be an object");
        static const std::set<std::string> fields{"model",
                                                  "instructions",
                                                  "input",
                                                  "tools",
                                                  "tool_choice",
                                                  "parallel_tool_calls",
                                                  "reasoning",
                                                  "store",
                                                  "stream",
                                                  "stream_options",
                                                  "include",
                                                  "service_tier",
                                                  "prompt_cache_key",
                                                  "text",
                                                  "client_metadata",
                                                  "max_output_tokens"};
        for (const auto& [key, value] : input.items()) {
            static_cast<void>(value);
            require(fields.contains(key), "unsupported field: " + key);
        }
        require(input.value("stream", false), "stream:true is required");
        require(!input.value("store", false), "stored Responses are unsupported");
        if (input.contains("reasoning") && !input["reasoning"].is_null()) {
            const auto& reasoning = input["reasoning"];
            require(reasoning.is_object(), "invalid reasoning controls");
            for (const auto& [key, value] : reasoning.items())
                require((key == "effort" || key == "summary") && (value.is_null() || value == "none"),
                        "reasoning translation is not supported; disable Codex reasoning");
        }
        // Codex 0.154 unconditionally requests this optional field, even when
        // reasoning is disabled. We produce no reasoning items to enrich.
        if (input.contains("include")) {
            require(input["include"].is_array(), "include must be an array");
            for (const auto& field : input["include"])
                require(field == "reasoning.encrypted_content", "unsupported included data");
        }
        require(!input.contains("stream_options") || input["stream_options"].is_null() || input["stream_options"].empty(),
                "stream_options unsupported");
        require(!input.contains("service_tier") || input["service_tier"].is_null() || input["service_tier"] == "auto" ||
                    input["service_tier"] == "default",
                "service tier unsupported");
        require(!input.contains("text") || input["text"].is_null() || input["text"].empty(),
                "structured output and verbosity controls unsupported");
        ResponsesRequest result;
        auto& request = result.request;
        request.model = input.at("model").get<std::string>();
        require(!request.model.empty(), "empty model");
        request.instructions = input.value("instructions", "");
        request.parallelTools = input.value("parallel_tool_calls", true);
        if (input.contains("max_output_tokens")) {
            require(input["max_output_tokens"].is_number_unsigned() ||
                        (input["max_output_tokens"].is_number_integer() && input["max_output_tokens"].get<std::int64_t>() > 0),
                    "invalid output token limit");
            request.maximumOutputTokens = input["max_output_tokens"].get<std::uint64_t>();
            require(request.maximumOutputTokens > 0, "output token limit must be positive");
        }
        const auto choice = input.value("tool_choice", "auto");
        require(choice == "auto" || choice == "none" || choice == "required", "unsupported tool_choice");
        request.toolChoice = choice == "none"       ? neutral::ToolChoice::None
                             : choice == "required" ? neutral::ToolChoice::Required
                                                    : neutral::ToolChoice::Auto;
        auto addTool = [&](const Json& tool, const std::string& ns, const std::string& scopeDescription) {
            const auto type = tool.at("type").get<std::string>();
            require(type == "function" || type == "custom", "unsupported tool: " + type);
            require(!tool.value("defer_loading", false), "deferred tool loading unsupported");
            require(!tool.value("strict", false), "strict tool constrained decoding unsupported");
            auto id = identity(tool, ns);
            require(!id.name.empty() && result.tools.emplace(name(id), id).second, "duplicate or empty tool name");
            neutral::Tool declaration{
                name(id), (scopeDescription.empty() ? "" : scopeDescription + "\n") + tool.value("description", ""), Json::object()};
            if (id.custom) {
                declaration.inputSchema = {{"type", "object"},
                                           {"properties", {{"input", {{"type", "string"}}}}},
                                           {"required", {"input"}},
                                           {"additionalProperties", false}};
                declaration.description += "\nTransport this tool's entire freeform input verbatim in the JSON string property input.";
                if (tool.contains("format")) {
                    const auto& format = tool.at("format");
                    require(format.value("type", "") == "text" ||
                                (format.value("type", "") == "grammar" && format.value("syntax", "") == "lark"),
                            "unsupported custom tool format");
                    if (format.value("type", "") == "grammar")
                        declaration.description += "\nInput syntax (validated by the tool, not constrained decoding):\n" +
                                                   format.at("definition").get<std::string>();
                }
            } else
                declaration.inputSchema = tool.at("parameters");
            request.tools.push_back(std::move(declaration));
        };
        if (input.contains("tools") && !input["tools"].is_null()) {
            require(input["tools"].is_array(), "tools must be an array");
            for (const auto& tool : input["tools"]) {
                if (tool.value("type", "") == "namespace") {
                    const auto ns = tool.at("name").get<std::string>();
                    require(!ns.empty() && tool.at("tools").is_array(), "invalid tool namespace");
                    for (const auto& child : tool.at("tools"))
                        addTool(child, ns, tool.value("description", ""));
                } else
                    addTool(tool, {}, {});
            }
        }
        const auto& items = input.at("input");
        require(items.is_array(), "input must be an array");
        bool assistantSeen = false;
        std::set<std::string> pending;
        std::set<std::string> calls;
        for (const auto& item : items) {
            const auto type = item.value("type", "message");
            if (type == "message") {
                const auto role = item.at("role").get<std::string>();
                auto value = text(item.at("content"));
                if (role == "system" || role == "developer") {
                    require(!assistantSeen, "mid-conversation system/developer changes unsupported; start a new thread");
                    if (!request.instructions.empty())
                        request.instructions += "\n\n";
                    request.instructions += value;
                } else {
                    require(role == "user" || role == "assistant", "unsupported message role");
                    require(pending.empty() || (role == "assistant" && !request.messages.empty() &&
                                                request.messages.back().role == neutral::Role::Assistant),
                            "missing tool results before new message turn");
                    assistantSeen = assistantSeen || role == "assistant";
                    if (!value.empty())
                        merge(request, role == "user" ? neutral::Role::User : neutral::Role::Assistant, neutral::Text{std::move(value)});
                }
            } else if (type == "function_call" || type == "custom_tool_call") {
                assistantSeen = true;
                require(pending.empty() || (!request.messages.empty() && request.messages.back().role == neutral::Role::Assistant),
                        "new tool call before previous tool results completed");
                require(!item.contains("encrypted_function_args") || item["encrypted_function_args"].is_null(),
                        "encrypted tool input unsupported");
                const auto id = identity(item);
                if (result.tools.contains(name(id)))
                    require(result.tools.at(name(id)).custom == id.custom, "tool kind changed within history");
                const auto call = item.at("call_id").get<std::string>();
                require(!call.empty() && calls.insert(call).second, "duplicate or empty tool call ID");
                pending.insert(call);
                Json args =
                    id.custom ? Json{{"input", item.at("input").get<std::string>()}} : Json::parse(item.at("arguments").get<std::string>());
                require(args.is_object(), "tool arguments must be a JSON object");
                merge(request, neutral::Role::Assistant, neutral::ToolCall{call, name(id), std::move(args)});
            } else if (type == "function_call_output" || type == "custom_tool_call_output") {
                const auto call = item.at("call_id").get<std::string>();
                require(pending.erase(call) == 1, "unmatched tool result: " + call);
                merge(request, neutral::Role::User, neutral::ToolResult{call, text(item.at("output")), false});
            } else
                throw std::invalid_argument("Codex Responses profile: unsupported input item: " + type);
        }
        require(pending.empty(), "all tool results are required for continuation");
        require(!request.messages.empty(), "empty conversation");
        return result;
    }

    std::string ResponsesStream::CustomInput::consume(std::string_view delta) {
        std::string output;
        for (char c : delta) {
            if (state != State::Value && state != State::Key && std::isspace(static_cast<unsigned char>(c)))
                continue;
            switch (state) {
                case State::Open:
                    require(c == '{', "custom tool input must be an object");
                    state = State::Key;
                    break;
                case State::Key:
                    if (key.empty() && std::isspace(static_cast<unsigned char>(c)))
                        break;
                    key += c;
                    require(key.size() <= 32, "custom input key exceeds limit");
                    if (key.size() > 1 && c == '"' && key[key.size() - 2] != '\\') {
                        require(Json::parse(key) == "input", "custom tool requires only the input string");
                        state = State::Colon;
                    }
                    break;
                case State::Colon:
                    require(c == ':', "invalid custom input separator");
                    state = State::Quote;
                    break;
                case State::Quote:
                    require(c == '"', "custom input must be a string");
                    state = State::Value;
                    break;
                case State::Value:
                    if (!escape.empty()) {
                        escape += c;
                        if (escape.size() >= 2 && escape[1] != 'u') {
                            output += Json::parse('"' + escape + '"').get<std::string>();
                            escape.clear();
                        } else if (escape.size() == 6 || escape.size() == 12) {
                            const auto decoded = Json::parse('"' + escape + '"', nullptr, false);
                            if (!decoded.is_discarded()) {
                                output += decoded.get<std::string>();
                                escape.clear();
                            } else if (escape.size() == 12)
                                throw std::invalid_argument("invalid custom input Unicode escape");
                        } else
                            require(escape.size() < 12, "invalid custom input escape");
                    } else if (c == '\\')
                        escape = "\\";
                    else if (c == '"')
                        state = State::Close;
                    else {
                        require(static_cast<unsigned char>(c) >= 32, "invalid custom input character");
                        output += c;
                    }
                    break;
                case State::Close:
                    require(c == '}', "custom tool requires exactly one input property");
                    state = State::Done;
                    break;
                case State::Done:
                    throw std::invalid_argument("trailing custom input data");
            }
        }
        return output;
    }
    ResponsesStream::ResponsesStream(const ResponsesRequest& request, std::string responseId, Sender sender)
        : model_(request.request.model)
        , tools_(request.tools)
        , id_(std::move(responseId))
        , sender_(std::move(sender)) {
    }
    bool ResponsesStream::terminal() const noexcept {
        return terminal_;
    }
    std::string ResponsesStream::itemId(std::size_t index) const {
        return id_ + "_item_" + std::to_string(index);
    }
    void ResponsesStream::send(Json event) {
        event["sequence_number"] = sequence_++;
        if (!sender_(event)) {
            terminal_ = true;
            throw std::runtime_error("Responses receiver disconnected or queue full");
        }
    }
    Json ResponsesStream::item(std::size_t index, const neutral::Content& content, bool completed) const {
        Json result{{"id", itemId(index)}, {"status", completed ? "completed" : "in_progress"}};
        if (const auto* value = std::get_if<neutral::Text>(&content)) {
            result["type"] = "message";
            result["role"] = "assistant";
            result["phase"] = "commentary";
            result["content"] =
                completed ? Json::array({{{"type", "output_text"}, {"text", value->text}, {"annotations", Json::array()}}}) : Json::array();
        } else if (const auto* call = std::get_if<neutral::ToolCall>(&content)) {
            const auto& tool = tools_.at(call->name);
            result["type"] = tool.custom ? "custom_tool_call" : "function_call";
            result["call_id"] = call->id;
            result["name"] = tool.name;
            if (!tool.nameSpace.empty())
                result["namespace"] = tool.nameSpace;
            result[tool.custom ? "input" : "arguments"] =
                completed ? (tool.custom ? call->input.at("input").get<std::string>() : call->input.dump()) : "";
        } else
            throw std::invalid_argument("Codex Responses profile cannot translate reasoning or tool-result output");
        return result;
    }
    void ResponsesStream::finishText(bool final) {
        if (output_.empty() || output_.back()["type"] != "message")
            return;
        auto& message = output_.back();
        message["phase"] = final ? "final_answer" : "commentary";
        send({{"type", "response.output_item.done"}, {"output_index", output_.size() - 1}, {"item", message}});
    }
    void ResponsesStream::fail(const neutral::Error& error) {
        if (terminal_)
            return;
        const std::string code =
            error.retryable ? (error.code == "rate_limit_error" ? "rate_limit_exceeded" : "server_is_overloaded") : "invalid_prompt";
        send({{"type", "response.failed"},
              {"response",
               {{"id", id_}, {"status", "failed"}, {"error", {{"code", code}, {"message", error.code + ": " + error.message}}}}}});
        terminal_ = true;
    }
    void ResponsesStream::receive(const neutral::Event& event) {
        if (terminal_)
            return;
        try {
            std::visit(
                [&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, neutral::Started>) {
                        require(!started_, "duplicate provider response start");
                        started_ = true;
                        send({{"type", "response.created"},
                              {"response", {{"id", id_}, {"object", "response"}, {"status", "in_progress"}, {"output", Json::array()}}}});
                    } else if constexpr (std::is_same_v<T, neutral::BlockStarted>) {
                        require(started_ && active_.empty() && value.index == output_.size(), "out-of-order provider block");
                        finishText(false);
                        if (const neutral::ToolCall* call = std::get_if<neutral::ToolCall>(&value.content)) {
                            require(!call->id.empty() && calls_.insert(call->id).second, "duplicate provider tool call ID");
                        }
                        active_.emplace(value.index, Active{value.content, {}, {}});
                        send({{"type", "response.output_item.added"},
                              {"output_index", value.index},
                              {"item", item(value.index, value.content, false)}});
                        if (const auto* valueText = std::get_if<neutral::Text>(&value.content)) {
                            send({{"type", "response.content_part.added"},
                                  {"item_id", itemId(value.index)},
                                  {"output_index", value.index},
                                  {"content_index", 0},
                                  {"part", {{"type", "output_text"}, {"text", ""}, {"annotations", Json::array()}}}});
                            if (!valueText->text.empty())
                                receive(neutral::TextDelta{value.index, valueText->text});
                        }
                    } else if constexpr (std::is_same_v<T, neutral::TextDelta> || std::is_same_v<T, neutral::ToolInputDelta>) {
                        require(value.text.size() <= 64U * 1024U * 1024U - bytes_, "provider stream exceeds byte limit");
                        bytes_ += value.text.size();
                        auto& active = active_.at(value.index);
                        Json out{{"item_id", itemId(value.index)}, {"output_index", value.index}};
                        if constexpr (std::is_same_v<T, neutral::TextDelta>) {
                            require(std::holds_alternative<neutral::Text>(active.content), "text delta for non-text block");
                            out["type"] = "response.output_text.delta";
                            out["content_index"] = 0;
                            out["delta"] = value.text;
                        } else {
                            const auto& call = std::get<neutral::ToolCall>(active.content);
                            const bool custom = tools_.at(call.name).custom;
                            out["type"] = custom ? "response.custom_tool_call_input.delta" : "response.function_call_arguments.delta";
                            out["call_id"] = call.id;
                            out["delta"] = custom ? active.custom.consume(value.text) : value.text;
                        }
                        active.deltas += out.at("delta").get<std::string>();
                        if (!out.at("delta").get_ref<const std::string&>().empty())
                            send(std::move(out));
                    } else if constexpr (std::is_same_v<T, neutral::BlockFinished>) {
                        auto& active = active_.at(value.index);
                        Json completed = item(value.index, value.content, true);
                        if (const neutral::ToolCall* call = std::get_if<neutral::ToolCall>(&value.content)) {
                            const auto& original = std::get<neutral::ToolCall>(active.content);
                            require(call->id == original.id && call->name == original.name, "tool identity changed during stream");
                            const bool custom = tools_.at(call->name).custom;
                            if (custom) {
                                require(call->input.is_object() && call->input.size() == 1 && call->input.at("input").is_string(),
                                        "invalid custom tool wrapper");
                                if (!active.deltas.empty())
                                    require(active.custom.state == CustomInput::State::Done &&
                                                active.deltas == call->input.at("input").get<std::string>(),
                                            "incomplete custom tool input");
                                else if (!call->input.at("input").get_ref<const std::string&>().empty())
                                    send({{"type", "response.custom_tool_call_input.delta"},
                                          {"item_id", itemId(value.index)},
                                          {"call_id", call->id},
                                          {"output_index", value.index},
                                          {"delta", call->input.at("input")}});
                            } else if (!active.deltas.empty())
                                require(Json::parse(active.deltas) == call->input, "tool input changed during stream");
                        } else if (const auto* valueText = std::get_if<neutral::Text>(&value.content)) {
                            require(valueText->text == active.deltas, "text changed during stream");
                            send({{"type", "response.output_text.done"},
                                  {"item_id", itemId(value.index)},
                                  {"output_index", value.index},
                                  {"content_index", 0},
                                  {"text", valueText->text}});
                        }
                        // Text deltas remain immediate. Resolve the trailing text's
                        // phase at the next block or completion, not before we know
                        // whether this inference continues with tools.
                        if (!std::holds_alternative<neutral::Text>(value.content))
                            send({{"type", "response.output_item.done"}, {"output_index", value.index}, {"item", completed}});
                        output_.push_back(std::move(completed));
                        active_.erase(value.index);
                    } else if constexpr (std::is_same_v<T, neutral::Usage>) {
                        usage_ = value;
                    } else if constexpr (std::is_same_v<T, neutral::Error>) {
                        fail(value);
                    } else if constexpr (std::is_same_v<T, neutral::ReasoningDelta>) {
                        fail({"unsupported_reasoning", "reasoning translation disabled", false});
                    } else if constexpr (std::is_same_v<T, neutral::Completed>) {
                        require(started_ && active_.empty(), "provider completed with unfinished output");
                        if (value.reason == neutral::StopReason::OutputLimit || value.reason == neutral::StopReason::Pause) {
                            fail({"incomplete_response", "provider stopped before completing inference", false});
                            return;
                        }
                        require((value.reason == neutral::StopReason::ToolUse) == !calls_.empty(),
                                "provider stop reason does not match tool calls");
                        const auto maximum = std::numeric_limits<std::int64_t>::max();
                        std::uint64_t total = 0;
                        for (auto count : {usage_.inputTokens, usage_.cacheReadTokens, usage_.cacheWriteTokens, usage_.outputTokens}) {
                            require(count <= static_cast<std::uint64_t>(maximum) - total, "token usage overflow");
                            total += count;
                        }
                        Json usage{{"input_tokens", total - usage_.outputTokens},
                                   {"output_tokens", usage_.outputTokens},
                                   {"total_tokens", total},
                                   {"input_tokens_details",
                                    {{"cached_tokens", usage_.cacheReadTokens}, {"cache_write_tokens", usage_.cacheWriteTokens}}}};
                        finishText(calls_.empty());
                        send({{"type", "response.completed"},
                              {"response",
                               {{"id", id_},
                                {"object", "response"},
                                {"status", "completed"},
                                {"model", model_},
                                {"output", output_},
                                {"usage", std::move(usage)},
                                {"end_turn", calls_.empty()}}}});
                        terminal_ = true;
                    }
                },
                event);
        } catch (const std::exception& e) {
            try {
                fail({"invalid_provider_response", e.what(), false});
            } catch (...) {
                terminal_ = true;
            }
        }
    }
    namespace {
        struct Session {
            ResponsesStream stream;
            std::function<void()> finished;
            bool stopped = false;
            neutral::Operation* upstream = nullptr;
            Session(const ResponsesRequest& request, std::string id, ResponsesStream::Sender sender, std::function<void()> done)
                : stream(request, std::move(id), std::move(sender))
                , finished(std::move(done)) {
            }
        };
        class ResponseOperation final : public neutral::Operation {
        public:
            explicit ResponseOperation(std::shared_ptr<Session> state)
                : state_(std::move(state)) {
            }
            ~ResponseOperation() override {
                cancel();
            }
            void cancel() noexcept override {
                state_->stopped = true;
                state_->finished = {};
                state_->upstream = nullptr;
                if (provider)
                    provider->cancel();
            }
            std::unique_ptr<neutral::Operation> provider;

        private:
            std::shared_ptr<Session> state_;
        };
    } // namespace
    std::unique_ptr<neutral::Operation> startResponse(neutral::Provider& provider,
                                                      ResponsesRequest request,
                                                      std::string responseId,
                                                      ResponsesStream::Sender sender,
                                                      std::function<void()> finished) {
        auto state = std::make_shared<Session>(request, std::move(responseId), std::move(sender), std::move(finished));
        auto operation = std::make_unique<ResponseOperation>(state);
        auto receive = [state](const neutral::Event& event) {
            if (state->stopped)
                return;
            state->stream.receive(event);
            if (state->stream.terminal()) {
                state->stopped = true;
                if (state->upstream)
                    state->upstream->cancel();
                auto done = std::exchange(state->finished, {});
                if (done)
                    done();
            }
        };
        try {
            operation->provider = provider.start(request.request, receive);
            if (!operation->provider && !state->stopped)
                receive(neutral::Error{"provider_start_failed", "provider returned no active operation", false});
        } catch (const std::exception& error) {
            receive(neutral::Error{"provider_start_failed", error.what(), false});
        }
        state->upstream = operation->provider.get();
        if (state->stopped && operation->provider)
            operation->provider->cancel();
        return operation;
    }
} // namespace ai::openai::codex::model
