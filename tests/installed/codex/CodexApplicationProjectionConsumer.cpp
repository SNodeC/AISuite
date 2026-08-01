/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *               2020, 2021, 2022, 2023, 2024, 2025, 2026
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include <ai/openai/codex/stdio/Client.h>
#include <ai/openai/codex/typed/Client.h>
#include <ai/openai/codex/typed/Conversation.h>
#include <ai/openai/codex/typed/Events.h>
#include <ai/openai/codex/typed/Items.h>
#include <ai/openai/codex/typed/ServerRequests.h>
#include <ai/openai/codex/typed/Threads.h>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

int main() {
    namespace typed = ai::openai::codex::typed;

    static_assert(std::is_same_v<typed::TextInput, typed::TextUserInput>);
    static_assert(std::is_same_v<typed::ImageUrlInput, typed::ImageUserInput>);
    static_assert(std::is_same_v<typed::LocalImageInput, typed::LocalImageUserInput>);
    static_assert(std::is_same_v<typed::SkillInput, typed::SkillUserInput>);
    static_assert(std::is_same_v<typed::MentionInput, typed::MentionUserInput>);
    static_assert(std::is_same_v<typed::UnknownTurnInput, typed::UnknownUserInput>);
    static_assert(std::is_same_v<typed::TurnInput, typed::UserInput>);
    static_assert(std::is_same_v<typed::AgentMessageItem, typed::AgentMessageThreadItem>);
    static_assert(std::is_same_v<typed::CommandExecutionItem, typed::CommandExecutionThreadItem>);
    static_assert(std::is_same_v<typed::FileChangeItem, typed::FileChangeThreadItem>);
    static_assert(std::is_same_v<typed::ToolCallItem, typed::McpToolCallThreadItem>);
    static_assert(std::is_same_v<typed::ReasoningItem, typed::ReasoningThreadItem>);
    static_assert(std::is_same_v<typed::UserMessageItem, typed::UserMessageThreadItem>);
    static_assert(std::is_same_v<typed::WebSearchItem, typed::WebSearchThreadItem>);
    static_assert(std::is_same_v<typed::Item, typed::ThreadItem>);
    static_assert(std::is_same_v<typed::ThreadPage, typed::ThreadListResponse>);
    static_assert(std::is_same_v<typed::ExternalSandboxPolicy, typed::ExternalSandboxSandboxPolicy>);
    static_assert(std::is_same_v<typed::PermissionsRequestApprovalRequest, typed::PermissionsApprovalRequest>);
    static_assert(std::is_same_v<typed::ChatgptAuthTokensRefreshRequest, typed::AuthenticationRequest>);
    static_assert(std::is_class_v<typed::CommandApprovalRequest>);
    static_assert(std::is_class_v<typed::FileChangeApprovalRequest>);
    static_assert(std::is_class_v<typed::UserInputRequest>);
    static_assert(std::is_class_v<typed::UserInputQuestion>);
    static_assert(std::is_class_v<typed::UserInputOption>);
    static_assert(std::is_class_v<typed::UserInputAnswer>);
    static_assert(std::is_class_v<typed::AuthenticationRequest>);
    static_assert(std::is_class_v<typed::AuthenticationResponse>);
    static_assert(std::is_class_v<typed::ApprovalDecision>);
    static_assert(std::is_class_v<typed::ThreadStarted>);
    static_assert(std::is_class_v<typed::ThreadStatusChanged>);
    static_assert(std::is_class_v<typed::TurnStarted>);
    static_assert(std::is_class_v<typed::TurnCompleted>);
    static_assert(std::is_class_v<typed::TurnFailed>);
    static_assert(std::is_class_v<typed::ItemStarted>);
    static_assert(std::is_class_v<typed::ItemCompleted>);
    static_assert(std::is_class_v<typed::AgentMessageDelta>);
    static_assert(std::is_class_v<typed::ReasoningDelta>);
    static_assert(std::is_class_v<typed::CommandOutputDelta>);
    static_assert(std::is_class_v<typed::FileChangeUpdated>);
    static_assert(std::is_class_v<typed::TokenUsageUpdated>);
    static_assert(std::is_class_v<typed::ModelRerouted>);
    static_assert(std::is_class_v<typed::TurnErrorEvent>);
    static_assert(std::is_convertible_v<std::string, typed::AbsolutePathBuf>);
    static_assert(std::is_convertible_v<std::string, typed::LegacyAppPathString>);

    [[maybe_unused]] typed::TurnInput legacyInput =
        typed::ImageUrlInput{.url = "https://example.test/image", .detail = std::optional{typed::ImageDetail::high()}};
    [[maybe_unused]] auto threadTitleMember = &typed::Thread::title;
    [[maybe_unused]] auto threadModelMember = &typed::Thread::model;
    [[maybe_unused]] auto turnThreadMember = &typed::Turn::threadId;

    ai::openai::codex::stdio::Client client;
    const ai::openai::codex::AppServerClient& constClient = client;

    const bool sameObjects =
        &client.threads() == &client.typed().threads() && &client.turns() == &client.typed().turns() &&
        &client.events() == &client.typed().events() && &client.requests() == &client.typed().requests() &&
        &constClient.threads() == &constClient.typed().threads() && &constClient.turns() == &constClient.typed().turns() &&
        &constClient.events() == &constClient.typed().events() && &constClient.requests() == &constClient.typed().requests();

    const typed::ThreadId threadId{"thread-compatibility"};
    const auto rollbackHandler = [](const typed::OperationResult<typed::ThreadRollbackResponse>&) {
    };

    (void) client.threads().rollback({.threadId = threadId, .numTurns = 1}, rollbackHandler);

    return sameObjects ? 0 : 1;
}
