/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_TURNS_H
#define AI_OPENAI_CODEX_TYPED_TURNS_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/CodexErrorInfo.h"
#include "ai/openai/codex/typed/Conversation.h"
#include "ai/openai/codex/typed/Items.h"
#include "ai/openai/codex/typed/Results.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ai::openai::codex::typed {

    struct Turn {
        TurnId id;
        // Application context populated by operation and event projections.
        // The wire Turn aggregate remains nested under its owning thread.
        ThreadId threadId;
        TurnStatus status;
        std::vector<ThreadItem> items;
        // The protocol omits this field for the legacy full-items view. Keep
        // the established semantic default while `raw` retains wire presence.
        TurnItemsView itemsView = TurnItemsView::full();
        OptionalNullable<TurnError> error;
        OptionalNullable<std::int64_t> startedAt;
        OptionalNullable<std::int64_t> completedAt;
        OptionalNullable<std::int64_t> durationMs;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct TurnInterruptParams {
        ThreadId threadId;
        TurnId turnId;
    };

    struct TurnStartParams {
        ThreadId threadId;
        std::vector<TurnInput> input;
        OptionalNullable<Personality> personality;
        OptionalNullable<AskForApproval> approvalPolicy;
        OptionalNullable<ApprovalsReviewer> approvalsReviewer;
        OptionalNullable<ClientUserMessageId> clientUserMessageId;
        OptionalNullable<std::string> serviceTier;
        OptionalNullable<std::string> cwd;
        OptionalNullable<ReasoningEffort> effort;
        OptionalNullable<ModelId> model;
        OptionalNullable<ReasoningSummary> summary;
        // ProtocolDefinedOpaqueJson: the pinned schema intentionally accepts
        // an arbitrary JSON Schema annotation here.
        OptionalNullable<Json> outputSchema;
        OptionalNullable<SandboxPolicy> sandboxPolicy;
    };

    struct TurnSteerParams {
        ThreadId threadId;
        TurnId expectedTurnId;
        std::vector<TurnInput> input;
        OptionalNullable<ClientUserMessageId> clientUserMessageId;
    };

    struct TurnStartResponse {
        Turn turn;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct TurnSteerResponse {
        TurnId turnId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    class Turns {
    public:
        Turns(const Turns&) = delete;
        Turns(Turns&&) = delete;
        Turns& operator=(const Turns&) = delete;
        Turns& operator=(Turns&&) = delete;

        Submission interrupt(TurnInterruptParams params, DoneHandler handler);
        Submission interrupt(ThreadId threadId, TurnId turnId, DoneHandler handler);
        Submission start(TurnStartParams params, CompletionHandler<TurnStartResponse> handler);
        Submission start(ThreadId threadId, std::vector<TurnInput> input, CompletionHandler<TurnStartResponse> handler);
        Submission start(ThreadId threadId, std::string text, CompletionHandler<TurnStartResponse> handler);
        Submission steer(TurnSteerParams params, CompletionHandler<TurnSteerResponse> handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Turns(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_TURNS_H
