/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#include "ai/providers/anthropic/AnthropicProvider.h"

#include "ai/http/SseDecoder.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace ai::providers::anthropic {
    using Json = nlohmann::json;
    namespace {
        void require(bool condition, const std::string& message) {
            if (!condition)
                throw std::invalid_argument(message);
        }
        bool validName(const std::string& name) {
            return !name.empty() && name.size() <= 128 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
            });
        }
        std::uint64_t count(const Json& object, const char* key, std::uint64_t fallback = 0) {
            if (!object.contains(key))
                return fallback;
            const auto& value = object.at(key);
            require(value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0),
                    std::string("invalid token count: ") + key);
            return value.get<std::uint64_t>();
        }
        model::StopReason stopReason(const std::string& reason) {
            if (reason == "end_turn")
                return model::StopReason::EndTurn;
            if (reason == "tool_use")
                return model::StopReason::ToolUse;
            if (reason == "max_tokens" || reason == "model_context_window_exceeded")
                return model::StopReason::OutputLimit;
            if (reason == "stop_sequence")
                return model::StopReason::StopSequence;
            if (reason == "refusal")
                return model::StopReason::Refusal;
            if (reason == "pause_turn")
                return model::StopReason::Pause;
            throw std::invalid_argument("unsupported Anthropic stop reason: " + reason);
        }
    } // namespace

    PreparedRequest prepareRequest(const model::Request& request, const Configuration& configuration) {
        require(!request.model.empty(), "Anthropic model is required");
        require(std::any_of(configuration.models.begin(),
                            configuration.models.end(),
                            [&](const auto& m) {
                                return m.id == request.model;
                            }),
                "Anthropic model is not registered: " + request.model);
        const auto maximum = request.maximumOutputTokens == 0 ? configuration.maximumOutputTokens : request.maximumOutputTokens;
        require(maximum > 0, "Anthropic maximum output tokens must be positive");
        require(configuration.thinkingBudgetTokens == 0 ||
                    (configuration.thinkingBudgetTokens >= 1024 && configuration.thinkingBudgetTokens < maximum),
                "Anthropic thinking budget must be at least 1024 and below maximum output tokens");
        require(configuration.thinkingBudgetTokens == 0 || request.toolChoice != model::ToolChoice::Required,
                "Anthropic thinking does not support forced tool use");
        PreparedRequest result;
        std::map<std::string, std::string> names;
        // Aliases must not change when tools are reordered between inferences.
        // Preserve readable names, add a deterministic suffix, and reject any
        // collision instead of ever dispatching a different semantic tool.
        auto name = [&](const std::string& value) {
            require(!value.empty(), "empty tool name");
            if (names.contains(value))
                return names.at(value);
            std::string wire = value;
            if (!validName(wire)) {
                std::uint64_t hash = 14695981039346656037ULL;
                for (unsigned char c : value) {
                    hash ^= c;
                    hash *= 1099511628211ULL;
                }
                wire = "aisuite_";
                for (unsigned char c : value) {
                    if (wire.size() == 96)
                        break;
                    wire += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
                                ? static_cast<char>(c)
                                : '_';
                }
                wire += '_';
                constexpr char hex[] = "0123456789abcdef";
                for (int shift = 60; shift >= 0; shift -= 4)
                    wire += hex[(hash >> shift) & 15U];
            }
            require(result.toolNames.emplace(wire, value).second, "Anthropic tool name encoding collision");
            names.emplace(value, wire);
            return wire;
        };
        Json tools = Json::array();
        std::set<std::string> declarations;
        for (const auto& tool : request.tools) {
            require(declarations.insert(tool.name).second, "duplicate tool declaration: " + tool.name);
            require(tool.inputSchema.is_object() && tool.inputSchema.value("type", "") == "object", "tool input schema must be an object");
            tools.push_back({{"name", name(tool.name)}, {"description", tool.description}, {"input_schema", tool.inputSchema}});
        }
        Json messages = Json::array();
        std::set<std::string> pending;
        std::set<std::string> calls;
        for (const auto& message : request.messages) {
            require(!message.content.empty(), "empty Anthropic message");
            if (!messages.empty() && messages.back().at("role") == "user" && message.role == model::Role::Assistant)
                require(pending.empty(), "assistant continuation before all tool results");
            Json blocks = Json::array();
            bool ordinaryUserContent = false;
            for (const auto& part : message.content) {
                std::visit(
                    [&](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, model::Text>) {
                            if (message.role == model::Role::User) {
                                require(pending.empty(), "tool results must precede user text");
                                ordinaryUserContent = true;
                            }
                            require(!value.text.empty(), "empty text block");
                            blocks.push_back({{"type", "text"}, {"text", value.text}});
                        } else if constexpr (std::is_same_v<T, model::ToolCall>) {
                            require(message.role == model::Role::Assistant && !value.id.empty() && value.input.is_object(),
                                    "invalid tool call");
                            require(calls.insert(value.id).second, "duplicate tool call ID: " + value.id);
                            pending.insert(value.id);
                            blocks.push_back({{"type", "tool_use"}, {"id", value.id}, {"name", name(value.name)}, {"input", value.input}});
                        } else if constexpr (std::is_same_v<T, model::ToolResult>) {
                            require(message.role == model::Role::User && !ordinaryUserContent, "tool result must lead a user message");
                            require(pending.erase(value.callId) == 1, "unmatched tool result: " + value.callId);
                            blocks.push_back({{"type", "tool_result"},
                                              {"tool_use_id", value.callId},
                                              {"content", value.text},
                                              {"is_error", value.isError}});
                        } else if constexpr (std::is_same_v<T, model::Reasoning>) {
                            require(message.role == model::Role::Assistant && value.provider == "anthropic" &&
                                        configuration.thinkingBudgetTokens != 0,
                                    "incompatible reasoning continuation");
                            Json block = Json::parse(value.continuation);
                            const auto type = block.value("type", "");
                            require(type == "thinking" || type == "redacted_thinking", "invalid Anthropic reasoning continuation");
                            if (type == "thinking")
                                require(block.at("thinking") == value.text && !block.at("signature").get<std::string>().empty(),
                                        "modified or unsigned Anthropic thinking");
                            else
                                require(block.at("data").is_string(), "invalid redacted thinking");
                            blocks.push_back(std::move(block));
                        }
                    },
                    part);
            }
            const std::string role = message.role == model::Role::User ? "user" : "assistant";
            if (!messages.empty() && messages.back().at("role") == role) {
                for (auto& block : blocks)
                    messages.back()["content"].push_back(std::move(block));
            } else
                messages.push_back({{"role", role}, {"content", std::move(blocks)}});
        }
        require(!messages.empty() && messages.front().at("role") == "user", "Anthropic conversation must start with user content");
        require(messages.back().at("role") == "user" && pending.empty(), "Anthropic continuation requires all pending tool results");
        result.body = {{"model", request.model}, {"max_tokens", maximum}, {"stream", true}, {"messages", std::move(messages)}};
        if (!request.instructions.empty())
            result.body["system"] = request.instructions;
        if (!tools.empty()) {
            result.body["tools"] = std::move(tools);
            result.body["tool_choice"] = {{"type",
                                           request.toolChoice == model::ToolChoice::None       ? "none"
                                           : request.toolChoice == model::ToolChoice::Required ? "any"
                                                                                               : "auto"}};
            if (request.toolChoice != model::ToolChoice::None)
                result.body["tool_choice"]["disable_parallel_tool_use"] = !request.parallelTools;
        } else
            require(request.toolChoice != model::ToolChoice::Required, "required tool use without tools");
        result.body["thinking"] = configuration.thinkingBudgetTokens == 0
                                      ? Json{{"type", "disabled"}}
                                      : Json{{"type", "enabled"}, {"budget_tokens", configuration.thinkingBudgetTokens}};
        // Historical names are needed for replay, but are not permission to
        // invoke a tool absent from this inference's declarations.
        std::erase_if(result.toolNames, [&](const auto& entry) {
            return !declarations.contains(entry.second);
        });
        return result;
    }

    MessageStream::MessageStream(model::Provider::Receiver receiver, std::map<std::string, std::string> toolNames, bool thinkingEnabled)
        : receiver_(std::move(receiver))
        , toolNames_(std::move(toolNames))
        , thinkingEnabled_(thinkingEnabled) {
    }
    void MessageStream::emit(const model::Event& event) {
        const auto receiver = receiver_;
        if (!terminal_ && receiver)
            receiver(event);
    }
    bool MessageStream::terminal() const noexcept {
        return terminal_;
    }
    void MessageStream::cancel() noexcept {
        terminal_ = true;
        receiver_ = {};
        blocks_.clear();
    }
    void MessageStream::fail(std::string code, std::string message, bool retryable) {
        if (terminal_)
            return;
        terminal_ = true;
        auto receiver = std::exchange(receiver_, {});
        blocks_.clear();
        if (receiver)
            receiver(model::Error{std::move(code), std::move(message), retryable});
    }
    void MessageStream::finish() {
        if (!terminal_)
            fail("incomplete_stream", "Anthropic stream ended without message_stop", true);
    }
    void MessageStream::event(std::string_view name, std::string_view data) {
        if (terminal_)
            return;
        try {
            require(data.size() <= 64U * 1024U * 1024U - bytes_, "Anthropic stream exceeds byte limit");
            bytes_ += data.size();
            const auto value = Json::parse(data);
            require(value.at("type").get<std::string>() == name, "Anthropic SSE event/type mismatch");
            parse(value);
        } catch (const std::exception& e) {
            fail("invalid_stream", e.what());
        }
    }
    model::Content MessageStream::content(const Block& block) const {
        const auto type = block.value.at("type").get<std::string>();
        if (type == "text")
            return model::Text{block.value.at("text").get<std::string>()};
        if (type == "tool_use") {
            const auto wire = block.value.at("name").get<std::string>();
            require(toolNames_.contains(wire), "Anthropic returned undeclared tool: " + wire);
            return model::ToolCall{block.value.at("id").get<std::string>(), toolNames_.at(wire), block.value.at("input")};
        }
        return model::Reasoning{block.value.value("thinking", ""), "anthropic", block.value.dump()};
    }
    void MessageStream::parse(const Json& event) {
        const auto type = event.at("type").get<std::string>();
        if (type.starts_with("content_block_"))
            require(event.contains("index"), "missing Anthropic content index");
        if (type == "ping")
            return;
        if (type == "error") {
            const auto& error = event.at("error");
            const auto code = error.at("type").get<std::string>();
            fail(code,
                 error.at("message").get<std::string>(),
                 code == "overloaded_error" || code == "rate_limit_error" || code == "api_error");
            return;
        }
        if (type == "message_start") {
            require(!started_, "duplicate Anthropic message_start");
            const auto& message = event.at("message");
            require(message.at("role") == "assistant" && message.at("content").is_array() && message.at("content").empty() &&
                        !message.at("id").get<std::string>().empty(),
                    "invalid Anthropic message_start");
            started_ = true;
            emit(model::Started{message.at("id").get<std::string>()});
            if (terminal_)
                return;
            const auto& usage = message.at("usage");
            usage_ = {count(usage, "input_tokens"),
                      count(usage, "output_tokens"),
                      count(usage, "cache_read_input_tokens"),
                      count(usage, "cache_creation_input_tokens")};
            emit(usage_);
        } else if (type == "content_block_start") {
            require(started_ && !stop_ && blocks_.empty(), "out-of-order Anthropic content block");
            const auto index = count(event, "index");
            require(index == nextIndex_++, "invalid Anthropic content index");
            Block block{event.at("content_block"), {}};
            const auto kind = block.value.at("type").get<std::string>();
            require(kind == "text" || kind == "tool_use" || kind == "thinking" || kind == "redacted_thinking",
                    "unsupported Anthropic block: " + kind);
            require(thinkingEnabled_ || (kind != "thinking" && kind != "redacted_thinking"),
                    "unexpected Anthropic thinking while disabled");
            if (kind == "tool_use") {
                require(block.value.at("input").is_object() && !block.value.at("id").get<std::string>().empty(), "invalid tool_use");
                require(callIds_.insert(block.value.at("id").get<std::string>()).second, "duplicate Anthropic tool ID");
            }
            if (kind == "redacted_thinking")
                require(block.value.at("data").is_string(), "invalid redacted thinking");
            const auto item = content(block);
            blocks_.emplace(index, std::move(block));
            emit(model::BlockStarted{index, item});
        } else if (type == "content_block_delta") {
            auto& block = blocks_.at(count(event, "index"));
            const auto index = count(event, "index");
            const auto& delta = event.at("delta");
            const auto kind = delta.at("type").get<std::string>();
            if (kind == "text_delta") {
                require(block.value.at("type") == "text", "text delta for non-text block");
                auto text = delta.at("text").get<std::string>();
                block.value["text"].get_ref<std::string&>() += text;
                emit(model::TextDelta{index, std::move(text)});
            } else if (kind == "input_json_delta") {
                require(block.value.at("type") == "tool_use", "tool delta for non-tool block");
                auto text = delta.at("partial_json").get<std::string>();
                block.input += text;
                emit(model::ToolInputDelta{index, std::move(text)});
            } else if (kind == "thinking_delta" || kind == "signature_delta") {
                require(block.value.at("type") == "thinking", "thinking delta for non-thinking block");
                const char* key = kind == "thinking_delta" ? "thinking" : "signature";
                auto text = delta.at(key).get<std::string>();
                if (!block.value.contains(key))
                    block.value[key] = "";
                block.value[key].get_ref<std::string&>() += text;
                if (kind == "thinking_delta")
                    emit(model::ReasoningDelta{index, std::move(text)});
            } else
                throw std::invalid_argument("unsupported Anthropic content delta: " + kind);
        } else if (type == "content_block_stop") {
            const auto index = count(event, "index");
            auto block = std::move(blocks_.at(index));
            blocks_.erase(index);
            if (!block.input.empty()) {
                block.value["input"] = Json::parse(block.input);
                require(block.value.at("input").is_object(), "tool input is not an object");
            }
            if (block.value.at("type") == "thinking")
                require(!block.value.value("signature", "").empty(), "unsigned thinking block");
            emit(model::BlockFinished{index, content(block)});
        } else if (type == "message_delta") {
            require(started_ && blocks_.empty(), "message_delta with unfinished content");
            const auto& delta = event.at("delta");
            if (delta.contains("stop_reason") && !delta.at("stop_reason").is_null())
                stop_ = stopReason(delta.at("stop_reason").get<std::string>());
            if (event.contains("usage")) {
                const auto& usage = event.at("usage");
                usage_ = {count(usage, "input_tokens", usage_.inputTokens),
                          count(usage, "output_tokens", usage_.outputTokens),
                          count(usage, "cache_read_input_tokens", usage_.cacheReadTokens),
                          count(usage, "cache_creation_input_tokens", usage_.cacheWriteTokens)};
                emit(usage_);
            }
        } else if (type == "message_stop") {
            require(started_ && stop_.has_value() && blocks_.empty(), "incomplete Anthropic message_stop");
            terminal_ = true;
            auto receiver = std::exchange(receiver_, {});
            if (receiver)
                receiver(model::Completed{*stop_});
        }
        // Anthropic permits new ancillary SSE event types. Unknown content is NOT ignored.
    }

    namespace {
        class Inference final : public model::Operation {
        public:
            struct State {
                MessageStream stream;
                http::SseDecoder sse;
                unsigned status = 0;
                std::string errorBody;
                State(model::Provider::Receiver receiver, PreparedRequest prepared, bool thinking)
                    : stream(std::move(receiver), std::move(prepared.toolNames), thinking)
                    , sse([this](auto name, auto data) {
                        stream.event(name, data);
                    }) {
                }
            };
            explicit Inference(std::shared_ptr<State> state)
                : state_(std::move(state)) {
            }
            ~Inference() override {
                cancel();
            }
            void cancel() noexcept override {
                state_->stream.cancel();
                if (transport)
                    transport->cancel();
            }
            std::unique_ptr<model::Operation> transport;

        private:
            std::shared_ptr<State> state_;
        };
    } // namespace
    AnthropicProvider::AnthropicProvider(Configuration configuration, http::StreamingClient& client)
        : configuration_(std::move(configuration))
        , client_(client)
        , info_{"anthropic", "Anthropic", configuration_.models} {
        require(!configuration_.apiKey.empty() && configuration_.apiKey.find_first_of("\r\n") == std::string::npos,
                "Anthropic API key is missing or invalid");
        require(!info_.models.empty(), "register at least one Anthropic model");
        std::set<std::string> models;
        for (const auto& model : info_.models)
            require(!model.id.empty() && models.insert(model.id).second, "empty or duplicate registered model");
        while (configuration_.baseUrl.ends_with('/'))
            configuration_.baseUrl.pop_back();
        require(configuration_.baseUrl.starts_with("https://") || configuration_.baseUrl.starts_with("http://127.0.0.1:"),
                "Anthropic requires HTTPS (HTTP loopback is allowed for protocol tests)");
    }
    const model::ProviderInfo& AnthropicProvider::info() const noexcept {
        return info_;
    }
    std::unique_ptr<model::Operation> AnthropicProvider::start(const model::Request& request, Receiver receiver) {
        PreparedRequest prepared;
        try {
            prepared = prepareRequest(request, configuration_);
        } catch (const std::exception& e) {
            if (receiver)
                receiver(model::Error{"invalid_request", e.what(), false});
            return {};
        }
        http::Request wire{configuration_.baseUrl + "/v1/messages",
                           {{"x-api-key", configuration_.apiKey},
                            {"anthropic-version", "2023-06-01"},
                            {"Content-Type", "application/json"},
                            {"Accept", "text/event-stream"}},
                           prepared.body.dump()};
        auto state = std::make_shared<Inference::State>(std::move(receiver), std::move(prepared), configuration_.thinkingBudgetTokens != 0);
        auto operation = std::make_unique<Inference>(state);
        operation->transport =
            client_.post(std::move(wire),
                         {[state](unsigned status, const auto& headers) {
                              state->status = status;
                              if (status == 200) {
                                  const auto type = headers.find("content-type");
                                  if (type == headers.end() || !type->second.starts_with("text/event-stream"))
                                      state->stream.fail("invalid_response", "Anthropic response is not text/event-stream");
                              }
                          },
                          [state](std::string_view data) {
                              if (state->stream.terminal())
                                  return;
                              if (state->status != 200) {
                                  if (data.size() > 65536U - state->errorBody.size())
                                      state->stream.fail("api_error", "Anthropic error body exceeds limit");
                                  else
                                      state->errorBody += data;
                              } else {
                                  try {
                                      state->sse.consume(data);
                                  } catch (const std::exception& e) {
                                      state->stream.fail("invalid_stream", e.what());
                                  }
                              }
                          },
                          [state] {
                              if (state->status != 200) {
                                  auto error = Json::parse(state->errorBody, nullptr, false);
                                  std::string code = "http_" + std::to_string(state->status);
                                  std::string message = "Anthropic HTTP " + std::to_string(state->status);
                                  if (error.is_object() && error.contains("error") && error["error"].is_object()) {
                                      if (error["error"].value("type", Json{}).is_string())
                                          code = error["error"]["type"].get<std::string>();
                                      if (error["error"].value("message", Json{}).is_string())
                                          message = error["error"]["message"].get<std::string>();
                                  }
                                  state->stream.fail(std::move(code), std::move(message), state->status == 429 || state->status >= 500);
                              } else
                                  state->stream.finish();
                          },
                          [state](std::string error) {
                              state->stream.fail("transport_error", std::move(error), true);
                          }});
        return operation;
    }
} // namespace ai::providers::anthropic
