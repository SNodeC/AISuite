/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#ifndef AISUITE_AI_MODEL_PROVIDER_H
#define AISUITE_AI_MODEL_PROVIDER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>
#include <vector>

namespace ai::model {

    struct Text {
        std::string text;
    };
    struct ToolCall {
        std::string id;
        std::string name;
        nlohmann::json input;
    };
    struct ToolResult {
        std::string callId;
        std::string text;
        bool isError = false;
    };
    // A provider owns the interpretation of continuation. Consumers must preserve it
    // verbatim, not interpret it as another provider's reasoning representation.
    struct Reasoning {
        std::string text;
        std::string provider;
        std::string continuation;
    };
    using Content = std::variant<Text, ToolCall, ToolResult, Reasoning>;
    enum class Role { User, Assistant };
    struct Message {
        Role role;
        std::vector<Content> content;
    };
    struct Tool {
        std::string name;
        std::string description;
        nlohmann::json inputSchema;
    };
    enum class ToolChoice { Auto, None, Required };
    struct Request {
        std::string model;
        std::string instructions;
        std::vector<Message> messages;
        std::vector<Tool> tools;
        ToolChoice toolChoice = ToolChoice::Auto;
        bool parallelTools = true;
        std::uint64_t maximumOutputTokens = 0; // zero selects the provider default
    };
    enum class StopReason { EndTurn, ToolUse, OutputLimit, StopSequence, Refusal, Pause };
    struct Started {
        std::string id;
    };
    struct BlockStarted {
        std::size_t index;
        Content content;
    };
    struct TextDelta {
        std::size_t index;
        std::string text;
    };
    struct ReasoningDelta {
        std::size_t index;
        std::string text;
    };
    struct ToolInputDelta {
        std::size_t index;
        std::string text;
    };
    struct BlockFinished {
        std::size_t index;
        Content content;
    };
    struct Usage {
        std::uint64_t inputTokens = 0; // uncached input; caches accounted separately
        std::uint64_t outputTokens = 0;
        std::uint64_t cacheReadTokens = 0;
        std::uint64_t cacheWriteTokens = 0;
    };
    struct Completed {
        StopReason reason;
    };
    struct Error {
        std::string code;
        std::string message;
        bool retryable = false;
    };
    using Event = std::variant<Started, BlockStarted, TextDelta, ReasoningDelta, ToolInputDelta, BlockFinished, Usage, Completed, Error>;
    struct Model {
        std::string id;
        std::string displayName;
    };
    struct ProviderInfo {
        std::string id;
        std::string displayName;
        std::vector<Model> models;
    };

    class Operation {
    public:
        virtual ~Operation() = default;
        // Event-loop confined, idempotent. No callbacks after cancel returns.
        // Concrete operation destructors also cancel outstanding work.
        virtual void cancel() noexcept = 0;
    };
    class Provider {
    public:
        using Receiver = std::function<void(const Event&)>;
        virtual ~Provider() = default;
        virtual const ProviderInfo& info() const noexcept = 0;
        // Never runs an event loop or blocks. Validation may complete synchronously.
        // One Completed or Error, unless cancelled. Caller retains the operation.
        virtual std::unique_ptr<Operation> start(const Request& request, Receiver receiver) = 0;
    };
} // namespace ai::model
#endif
