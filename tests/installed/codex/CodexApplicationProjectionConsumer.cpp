/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *               2020, 2021, 2022, 2023, 2024, 2025, 2026
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include <ai/openai/codex/Api.h>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

int main() {
    namespace typed = ai::openai::codex::typed;

    static_assert(std::variant_size_v<typed::TurnInput> == 6);
    static_assert(!std::is_same_v<typed::ThreadItem, typed::ResponseItem>);
    static_assert(std::is_convertible_v<std::string, typed::AbsolutePath>);
    static_assert(std::is_convertible_v<std::string, typed::PathString>);
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
    [[maybe_unused]] typed::TurnInput turnInput =
        typed::ImageUrlInput{.url = "https://example.test/image", .detail = std::optional{typed::ImageDetail::high()}};
    [[maybe_unused]] auto threadTitleMember = &typed::Thread::title;
    [[maybe_unused]] auto threadModelMember = &typed::Thread::model;
    [[maybe_unused]] auto turnThreadMember = &typed::Turn::threadId;

    ai::openai::codex::stdio::Client client;
    const bool sameObjects = &client.threads() == &client.threads() && &client.turns() == &client.turns() &&
                             &client.events() == &client.events() && &client.requests() == &client.requests();

    const typed::ThreadId threadId{"thread-compatibility"};
    const auto rollbackHandler = [](const typed::OperationResult<typed::ThreadRollbackResponse>&) {
    };

    (void) client.threads().rollback({.threadId = threadId, .numTurns = 1}, rollbackHandler);

    return sameObjects ? 0 : 1;
}
