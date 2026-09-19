/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#ifndef AISUITE_CODEX_FRONTEND_CODEXAGENT_H
#define AISUITE_CODEX_FRONTEND_CODEXAGENT_H
#include "ai/agent/Agent.h"
#include "ai/openai/codex/frontend/CodexBridge.h"
namespace ai::openai::codex::frontend {
    class CodexAgent final : public ai::agent::Agent {
    public:
        explicit CodexAgent(CodexBridge::Sender sender);
        void createConversation(const ai::agent::Selection&, const std::string& workingDirectory, Callback) override;
        void startTurn(const std::string& conversationId, const std::string& text, Callback) override;
        void interrupt(const std::string& conversationId, const std::string& turnId, Callback) override;
        void onEvent(Receiver) override;
        // Existing transport and rich Codex UI APIs remain available. The facade
        // owns the normalized-event handlers; do not replace them via this extension.
        CodexBridge& backend() noexcept;

    private:
        void emit(const ai::agent::Event& event);
        CodexBridge bridge_;
        Receiver receiver_;
    };
} // namespace ai::openai::codex::frontend
#endif
