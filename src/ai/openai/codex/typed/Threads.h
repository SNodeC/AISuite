/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_THREADS_H
#define AI_OPENAI_CODEX_TYPED_THREADS_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/Results.h"
#include "ai/openai/codex/typed/Reviews.h"
#include "ai/openai/codex/typed/Turns.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ai::openai::codex::typed {

    struct GitInfo {
        OptionalNullable<std::string> branch;
        OptionalNullable<std::string> originUrl;
        OptionalNullable<std::string> sha;
        Json raw = nullptr;
    };

    struct Thread {
        ThreadId id;
        std::string preview;
        bool ephemeral = false;
        std::string modelProvider;
        std::int64_t createdAt = 0;
        std::int64_t updatedAt = 0;
        ThreadStatus status;
        OptionalNullable<std::string> path;
        AbsolutePath cwd;
        std::string cliVersion;
        SessionSource source;
        OptionalNullable<std::string> agentRole;
        OptionalNullable<std::string> agentNickname;
        std::vector<Turn> turns;
        OptionalNullable<std::string> name;
        OptionalNullable<GitInfo> gitInfo;
        OptionalNullable<ThreadId> forkedFromId;
        OptionalNullable<ThreadId> parentThreadId;
        SessionId sessionId;
        OptionalNullable<std::int64_t> recencyAt;
        OptionalNullable<ThreadSource> threadSource;
        // Application context: `title` mirrors a valued wire `name`; `model`
        // is populated by operation and event projections. Neither member is
        // an independent Thread schema field.
        std::optional<std::string> title;
        std::optional<ModelId> model;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadArchiveParams {
        ThreadId threadId;
    };

    struct ThreadCompactStartParams {
        ThreadId threadId;
    };

    struct ThreadDeleteParams {
        ThreadId threadId;
    };

    using ProtocolConfiguration = std::map<std::string, Json>;

    struct ThreadForkParams {
        ThreadId threadId;
        OptionalNullable<AskForApproval> approvalPolicy;
        OptionalNullable<ApprovalsReviewer> approvalsReviewer;
        OptionalNullable<std::string> baseInstructions;
        OptionalNullable<ProtocolConfiguration> config;
        OptionalNullable<std::string> cwd;
        OptionalNullable<std::string> developerInstructions;
        std::optional<bool> ephemeral;
        OptionalNullable<std::string> serviceTier;
        OptionalNullable<TurnId> lastTurnId;
        OptionalNullable<ModelId> model;
        OptionalNullable<std::string> modelProvider;
        OptionalNullable<ThreadSource> threadSource;
        OptionalNullable<SandboxMode> sandbox;
    };

    struct ThreadGoalClearParams {
        ThreadId threadId;
    };

    struct ThreadGoalGetParams {
        ThreadId threadId;
    };

    struct ThreadGoalSetParams {
        ThreadId threadId;
        OptionalNullable<std::string> objective;
        OptionalNullable<ThreadGoalStatus> status;
        OptionalNullable<std::int64_t> tokenBudget;
    };

    struct ThreadInjectItemsParams {
        ThreadId threadId;
        // ProtocolDefinedOpaqueJson: the protocol explicitly defines each
        // injected Responses API item as arbitrary JSON.
        std::vector<Json> items;
    };

    struct ThreadListCwdFilter {
        std::variant<std::string, std::vector<std::string>> value;
    };

    struct ThreadListParams {
        OptionalNullable<std::vector<ThreadSourceKind>> sourceKinds;
        OptionalNullable<bool> archived;
        OptionalNullable<std::string> cursor;
        OptionalNullable<ThreadListCwdFilter> cwd;
        OptionalNullable<std::uint32_t> limit;
        OptionalNullable<std::vector<std::string>> modelProviders;
        std::optional<bool> useStateDbOnly;
        OptionalNullable<std::string> searchTerm;
        OptionalNullable<SortDirection> sortDirection;
        OptionalNullable<ThreadSortKey> sortKey;
    };

    struct ThreadLoadedListParams {
        OptionalNullable<std::string> cursor;
        OptionalNullable<std::uint32_t> limit;
    };

    struct ThreadMetadataGitInfoUpdateParams {
        OptionalNullable<std::string> branch;
        OptionalNullable<std::string> originUrl;
        OptionalNullable<std::string> sha;
    };

    struct ThreadMetadataUpdateParams {
        ThreadId threadId;
        OptionalNullable<ThreadMetadataGitInfoUpdateParams> gitInfo;
    };

    struct ThreadReadParams {
        ThreadId threadId;
        std::optional<bool> includeTurns;
    };

    struct ThreadResumeParams {
        ThreadId threadId;
        OptionalNullable<AskForApproval> approvalPolicy;
        OptionalNullable<ApprovalsReviewer> approvalsReviewer;
        OptionalNullable<std::string> baseInstructions;
        OptionalNullable<ProtocolConfiguration> config;
        OptionalNullable<std::string> cwd;
        OptionalNullable<std::string> developerInstructions;
        OptionalNullable<Personality> personality;
        OptionalNullable<std::string> serviceTier;
        OptionalNullable<ModelId> model;
        OptionalNullable<std::string> modelProvider;
        OptionalNullable<SandboxMode> sandbox;
    };

    struct ThreadRollbackParams {
        ThreadId threadId;
        std::uint32_t numTurns = 0;
    };

    struct ThreadSetNameParams {
        ThreadId threadId;
        std::string name;
    };

    struct ThreadShellCommandParams {
        ThreadId threadId;
        std::string command;
    };

    struct ThreadStartParams {
        OptionalNullable<ThreadStartSource> sessionStartSource;
        OptionalNullable<AskForApproval> approvalPolicy;
        OptionalNullable<ApprovalsReviewer> approvalsReviewer;
        OptionalNullable<std::string> baseInstructions;
        OptionalNullable<ProtocolConfiguration> config;
        OptionalNullable<std::string> cwd;
        OptionalNullable<std::string> developerInstructions;
        OptionalNullable<std::string> serviceName;
        OptionalNullable<Personality> personality;
        OptionalNullable<bool> ephemeral;
        OptionalNullable<ThreadSource> threadSource;
        OptionalNullable<SandboxMode> sandbox;
        OptionalNullable<std::string> serviceTier;
        OptionalNullable<ModelId> model;
        OptionalNullable<std::string> modelProvider;
    };

    struct ThreadUnarchiveParams {
        ThreadId threadId;
    };

    struct ThreadUnsubscribeParams {
        ThreadId threadId;
    };

    struct ThreadGoal {
        std::int64_t createdAt = 0;
        std::string objective;
        ThreadGoalStatus status;
        ThreadId threadId;
        std::int64_t timeUsedSeconds = 0;
        OptionalNullable<std::int64_t> tokenBudget;
        std::int64_t tokensUsed = 0;
        std::int64_t updatedAt = 0;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadForkResponse {
        OptionalNullable<std::string> serviceTier;
        AskForApproval approvalPolicy;
        ApprovalsReviewer approvalsReviewer;
        AbsolutePath cwd;
        // The schema default is the empty array; `raw` retains whether the
        // field was physically omitted on the wire.
        std::vector<PathString> instructionSources;
        ModelId model;
        std::string modelProvider;
        SandboxPolicy sandbox;
        OptionalNullable<ReasoningEffort> reasoningEffort;
        Thread thread;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadGoalClearResponse {
        bool cleared = false;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadGoalGetResponse {
        OptionalNullable<ThreadGoal> goal;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadGoalSetResponse {
        ThreadGoal goal;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadListResponse {
        std::vector<Thread> data;
        OptionalNullable<std::string> nextCursor;
        OptionalNullable<std::string> backwardsCursor;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadLoadedListResponse {
        // The wire schema describes these strings semantically as thread ids.
        std::vector<ThreadId> data;
        OptionalNullable<std::string> nextCursor;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadMetadataUpdateResponse {
        Thread thread;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadReadResponse {
        Thread thread;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadResumeResponse {
        SandboxPolicy sandbox;
        AskForApproval approvalPolicy;
        ApprovalsReviewer approvalsReviewer;
        AbsolutePath cwd;
        OptionalNullable<std::string> serviceTier;
        std::vector<PathString> instructionSources;
        ModelId model;
        std::string modelProvider;
        Thread thread;
        OptionalNullable<ReasoningEffort> reasoningEffort;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadRollbackResponse {
        Thread thread;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadStartResponse {
        OptionalNullable<std::string> serviceTier;
        AskForApproval approvalPolicy;
        ApprovalsReviewer approvalsReviewer;
        AbsolutePath cwd;
        std::vector<PathString> instructionSources;
        ModelId model;
        std::string modelProvider;
        SandboxPolicy sandbox;
        OptionalNullable<ReasoningEffort> reasoningEffort;
        Thread thread;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadUnarchiveResponse {
        Thread thread;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ThreadUnsubscribeResponse {
        ThreadUnsubscribeStatus status;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    class Threads {
    public:
        Threads(const Threads&) = delete;
        Threads(Threads&&) = delete;
        Threads& operator=(const Threads&) = delete;
        Threads& operator=(Threads&&) = delete;

        Submission archive(ThreadArchiveParams params, DoneHandler handler);
        Submission approveGuardianDeniedAction(ThreadApproveGuardianDeniedActionParams params, DoneHandler handler);
        Submission startCompaction(ThreadCompactStartParams params, DoneHandler handler);
        Submission remove(ThreadDeleteParams params, DoneHandler handler);
        Submission fork(ThreadForkParams params, CompletionHandler<ThreadForkResponse> handler);
        Submission clearGoal(ThreadGoalClearParams params, CompletionHandler<ThreadGoalClearResponse> handler);
        Submission getGoal(ThreadGoalGetParams params, CompletionHandler<ThreadGoalGetResponse> handler);
        Submission setGoal(ThreadGoalSetParams params, CompletionHandler<ThreadGoalSetResponse> handler);
        Submission injectItems(ThreadInjectItemsParams params, DoneHandler handler);
        Submission list(ThreadListParams params, CompletionHandler<ThreadListResponse> handler);
        Submission list(CompletionHandler<ThreadListResponse> handler);
        Submission listLoaded(ThreadLoadedListParams params, CompletionHandler<ThreadLoadedListResponse> handler);
        Submission listLoaded(CompletionHandler<ThreadLoadedListResponse> handler);
        Submission updateMetadata(ThreadMetadataUpdateParams params, CompletionHandler<ThreadMetadataUpdateResponse> handler);
        Submission setName(ThreadSetNameParams params, DoneHandler handler);
        Submission read(ThreadReadParams params, CompletionHandler<ThreadReadResponse> handler);
        Submission read(ThreadId threadId, CompletionHandler<ThreadReadResponse> handler);
        Submission resume(ThreadResumeParams params, CompletionHandler<ThreadResumeResponse> handler);
        Submission resume(ThreadId threadId, CompletionHandler<ThreadResumeResponse> handler);
        [[deprecated("thread/rollback is deprecated by the stable App Server protocol")]]
        Submission rollback(ThreadRollbackParams params, CompletionHandler<ThreadRollbackResponse> handler);
        Submission shellCommand(ThreadShellCommandParams params, DoneHandler handler);
        Submission start(ThreadStartParams params, CompletionHandler<ThreadStartResponse> handler);
        Submission start(CompletionHandler<ThreadStartResponse> handler);
        Submission start(AbsolutePath cwd, CompletionHandler<ThreadStartResponse> handler);
        Submission unarchive(ThreadUnarchiveParams params, CompletionHandler<ThreadUnarchiveResponse> handler);
        Submission unsubscribe(ThreadUnsubscribeParams params, CompletionHandler<ThreadUnsubscribeResponse> handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Threads(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_THREADS_H
