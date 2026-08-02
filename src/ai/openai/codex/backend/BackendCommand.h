/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_BACKEND_BACKENDCOMMAND_H
#define AI_OPENAI_CODEX_BACKEND_BACKENDCOMMAND_H

#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/typed/Threads.h"
#include "ai/openai/codex/typed/Turns.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ai::openai::codex::backend {

    struct ControllerAcquire {};
    struct ControllerRelease {};
    struct SnapshotGet {};

    struct ThreadStart {
        typed::ThreadStartParams params;
    };

    struct ThreadResume {
        typed::ThreadResumeParams params;
    };

    struct ThreadList {
        typed::ThreadListParams params;
    };

    struct ThreadRead {
        typed::ThreadReadParams params;
    };

    struct TurnStart {
        typed::TurnStartParams params;
    };

    struct TurnInterrupt {
        typed::TurnInterruptParams params;
    };

    struct ApprovalRespond {
        PendingRequestId requestId;
        typed::ApprovalDecision decision;
    };

    struct UserInputRespond {
        PendingRequestId requestId;
        std::vector<typed::UserInputAnswer> answers;
    };

    struct AuthenticationRespond {
        PendingRequestId requestId;
        typed::AuthenticationResponse response;
    };

    struct UnknownRequestRespondRaw {
        PendingRequestId requestId;
        Json result = nullptr;
    };

    struct UnknownRequestReject {
        PendingRequestId requestId;
        ProtocolError error;
    };

    using BackendCommand = std::variant<ControllerAcquire,
                                        ControllerRelease,
                                        SnapshotGet,
                                        ThreadStart,
                                        ThreadResume,
                                        ThreadList,
                                        ThreadRead,
                                        TurnStart,
                                        TurnInterrupt,
                                        ApprovalRespond,
                                        UserInputRespond,
                                        AuthenticationRespond,
                                        UnknownRequestRespondRaw,
                                        UnknownRequestReject>;

    enum class CommandErrorCode {
        PermissionDenied,
        InvalidCommand,
        NotFound,
        Conflict,
        LocalSubmissionFailure,
        TypedDecodingFailure,
        RemoteAppServerError,
        Cancelled,
        BackendUnavailable
    };

    struct CommandError {
        CommandErrorCode code = CommandErrorCode::InvalidCommand;
        std::string message;
        std::optional<std::int64_t> remoteCode;
        Json details = nullptr;
    };

    struct ControllerResult {
        std::optional<SessionId> controller;
        SessionRole role = SessionRole::Observer;
    };

    using CommandValue =
        std::variant<std::monostate, Snapshot, ControllerResult, typed::Thread, typed::ThreadListResponse, typed::Turn, typed::Unit>;

    struct CommandResult {
        CommandValue value;
        std::optional<CommandError> error;

        explicit operator bool() const noexcept {
            return !error.has_value();
        }

        static CommandResult succeeded(CommandValue value = std::monostate{});
        static CommandResult
        failed(CommandErrorCode code, std::string message, std::optional<std::int64_t> remoteCode = std::nullopt, Json details = nullptr);
    };

    struct CommandCompletion {
        std::string requestId;
        CommandResult result;
    };

    const char* commandErrorCodeName(CommandErrorCode code) noexcept;

} // namespace ai::openai::codex::backend

#endif // AI_OPENAI_CODEX_BACKEND_BACKENDCOMMAND_H
