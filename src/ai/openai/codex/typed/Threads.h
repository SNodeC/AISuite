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
        AbsolutePathBuf cwd;
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
        // Deprecated source-compatibility context. `title` mirrors a valued
        // wire `name`; `model` is populated only by launch-result wrappers,
        // because neither member is an independent Thread schema field.
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
        AbsolutePathBuf cwd;
        // The schema default is the empty array; `raw` retains whether the
        // field was physically omitted on the wire.
        std::vector<LegacyAppPathString> instructionSources;
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

    using ThreadPage = ThreadListResponse;

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
        AbsolutePathBuf cwd;
        OptionalNullable<std::string> serviceTier;
        std::vector<LegacyAppPathString> instructionSources;
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
        AbsolutePathBuf cwd;
        std::vector<LegacyAppPathString> instructionSources;
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
        using Submission = AppServerClient::RawProtocol::Submission;
        using UnitResultHandler = std::function<void(const OperationResult<Unit>&)>;
        using ThreadForkResultHandler = std::function<void(const OperationResult<ThreadForkResponse>&)>;
        using ThreadGoalClearResultHandler = std::function<void(const OperationResult<ThreadGoalClearResponse>&)>;
        using ThreadGoalGetResultHandler = std::function<void(const OperationResult<ThreadGoalGetResponse>&)>;
        using ThreadGoalSetResultHandler = std::function<void(const OperationResult<ThreadGoalSetResponse>&)>;
        using ThreadListResultHandler = std::function<void(const OperationResult<ThreadListResponse>&)>;
        using ThreadLoadedListResultHandler = std::function<void(const OperationResult<ThreadLoadedListResponse>&)>;
        using ThreadMetadataUpdateResultHandler = std::function<void(const OperationResult<ThreadMetadataUpdateResponse>&)>;
        using ThreadReadResultHandler = std::function<void(const OperationResult<ThreadReadResponse>&)>;
        using ThreadResumeResultHandler = std::function<void(const OperationResult<ThreadResumeResponse>&)>;
        using ThreadRollbackResultHandler = std::function<void(const OperationResult<ThreadRollbackResponse>&)>;
        using ThreadStartResultHandler = std::function<void(const OperationResult<ThreadStartResponse>&)>;
        using ThreadUnarchiveResultHandler = std::function<void(const OperationResult<ThreadUnarchiveResponse>&)>;
        using ThreadUnsubscribeResultHandler = std::function<void(const OperationResult<ThreadUnsubscribeResponse>&)>;

        Submission archive(ThreadArchiveParams params, UnitResultHandler handler);
        Submission approveGuardianDeniedAction(ThreadApproveGuardianDeniedActionParams params, UnitResultHandler handler);
        Submission startCompaction(ThreadCompactStartParams params, UnitResultHandler handler);
        Submission remove(ThreadDeleteParams params, UnitResultHandler handler);
        Submission fork(ThreadForkParams params, ThreadForkResultHandler handler);
        Submission clearGoal(ThreadGoalClearParams params, ThreadGoalClearResultHandler handler);
        Submission getGoal(ThreadGoalGetParams params, ThreadGoalGetResultHandler handler);
        Submission setGoal(ThreadGoalSetParams params, ThreadGoalSetResultHandler handler);
        Submission injectItems(ThreadInjectItemsParams params, UnitResultHandler handler);
        Submission list(ThreadListParams params, ThreadListResultHandler handler);
        Submission listLoaded(ThreadLoadedListParams params, ThreadLoadedListResultHandler handler);
        Submission updateMetadata(ThreadMetadataUpdateParams params, ThreadMetadataUpdateResultHandler handler);
        Submission setName(ThreadSetNameParams params, UnitResultHandler handler);
        Submission read(ThreadReadParams params, ThreadReadResultHandler handler);
        Submission resume(ThreadResumeParams params, ThreadResumeResultHandler handler);
        [[deprecated("thread/rollback is deprecated by the stable App Server protocol")]]
        Submission rollback(ThreadRollbackParams params, ThreadRollbackResultHandler handler);
        Submission shellCommand(ThreadShellCommandParams params, UnitResultHandler handler);
        Submission start(ThreadStartParams params, ThreadStartResultHandler handler);
        Submission unarchive(ThreadUnarchiveParams params, ThreadUnarchiveResultHandler handler);
        Submission unsubscribe(ThreadUnsubscribeParams params, ThreadUnsubscribeResultHandler handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Threads(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_THREADS_H
