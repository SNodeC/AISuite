/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#ifndef AISUITE_AI_AGENT_AGENT_H
#define AISUITE_AI_AGENT_AGENT_H
#include "ai/agent/Runtime.h"

#include <functional>
#include <variant>
namespace ai::agent {
    struct Result {
        std::string id;
        std::string error;
        explicit operator bool() const noexcept {
            return error.empty();
        }
    };
    struct TurnStarted {
        std::string conversationId;
        std::string turnId;
    };
    struct TextDelta {
        std::string conversationId;
        std::string turnId;
        std::string text;
    };
    enum class ToolKind { Command, FileChange, External };
    struct ToolActivity {
        std::string conversationId;
        std::string turnId;
        std::string id;
        ToolKind kind;
        bool completed;
    };
    enum class TurnStatus { Completed, Interrupted, Failed };
    struct TurnFinished {
        std::string conversationId;
        std::string turnId;
        TurnStatus status;
        std::string error;
    };
    using Event = std::variant<TurnStarted, TextDelta, ToolActivity, TurnFinished>;
    // Common coding-turn facade, not a second conversation store. Rich backend
    // operations (approval UI, resume, history, etc.) remain backend extensions.
    class Agent {
    public:
        using Callback = std::function<void(Result)>;
        using Receiver = std::function<void(const Event&)>;
        virtual ~Agent() = default;
        virtual void createConversation(const Selection&, const std::string& workingDirectory, Callback) = 0;
        virtual void startTurn(const std::string& conversationId, const std::string& text, Callback) = 0;
        virtual void interrupt(const std::string& conversationId, const std::string& turnId, Callback) = 0;
        virtual void onEvent(Receiver) = 0;
    };
} // namespace ai::agent
#endif
