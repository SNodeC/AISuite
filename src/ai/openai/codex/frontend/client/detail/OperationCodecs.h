/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_DETAIL_OPERATIONCODECS_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_DETAIL_OPERATIONCODECS_H

#include "ai/openai/codex/frontend/client/Controller.h"
#include "ai/openai/codex/frontend/client/Results.h"
#include "ai/openai/codex/frontend/client/Synchronization.h"
#include "ai/openai/codex/frontend/client/Types.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ai::openai::codex::frontend::client::detail {

    struct SynchronizationDecodeInput {
        frontend::SyncMode streamMode = frontend::SyncMode::Snapshot;
        frontend::SequenceNumber synchronizedThrough{};
        State state;
        std::size_t receivedEvents = 0;
        std::size_t appliedEvents = 0;
        std::size_t ignoredAlreadyAppliedEvents = 0;
        bool snapshotFallback = false;
    };

    [[nodiscard]] std::optional<frontend::Json> encodeUnitParams(const typed::Unit&, std::string& error) noexcept;
    [[nodiscard]] std::optional<frontend::Json> encodeEventsReplayParams(frontend::SequenceNumber after, std::string& error) noexcept;

    [[nodiscard]] std::optional<ControllerResult>
    decodeControllerResult(const frontend::Json& value, std::optional<std::string_view> currentSessionId, std::string& error) noexcept;
    [[nodiscard]] std::optional<SynchronizationResult> decodeSnapshotSynchronizationResult(SynchronizationDecodeInput input,
                                                                                           std::string& error) noexcept;
    [[nodiscard]] std::optional<SynchronizationResult> decodeReplaySynchronizationResult(SynchronizationDecodeInput input,
                                                                                         std::string& error) noexcept;

    [[nodiscard]] std::optional<frontend::Json> encodeApprovalRespondParams(const ApprovalRespondParams& value,
                                                                            std::string& error) noexcept;
    [[nodiscard]] std::optional<frontend::Json> encodeUserInputRespondParams(const UserInputRespondParams& value,
                                                                             std::string& error) noexcept;
    [[nodiscard]] std::optional<frontend::Json> encodeAuthenticationRespondParams(const AuthenticationRespondParams& value,
                                                                                  std::string& error) noexcept;
    [[nodiscard]] std::optional<frontend::Json> encodeUnknownRequestRespondParams(const UnknownRequestRespondParams& value,
                                                                                  std::string& error) noexcept;
    [[nodiscard]] std::optional<frontend::Json> encodeUnknownRequestRejectParams(const UnknownRequestRejectParams& value,
                                                                                 std::string& error) noexcept;
    [[nodiscard]] std::optional<frontend::Json> encodeApplyPatchApprovalRespondParams(const ApplyPatchApprovalRespondParams& value,
                                                                                      std::string& error) noexcept;
    [[nodiscard]] std::optional<frontend::Json> encodeAttestationRespondParams(const AttestationRespondParams& value,
                                                                               std::string& error) noexcept;
    [[nodiscard]] std::optional<frontend::Json> encodeDynamicToolRespondParams(const DynamicToolRespondParams& value,
                                                                               std::string& error) noexcept;
    [[nodiscard]] std::optional<frontend::Json> encodeExecCommandApprovalRespondParams(const ExecCommandApprovalRespondParams& value,
                                                                                       std::string& error) noexcept;
    [[nodiscard]] std::optional<frontend::Json> encodeKnownRequestRejectParams(const KnownRequestRejectParams& value,
                                                                               std::string& error) noexcept;
    [[nodiscard]] std::optional<frontend::Json> encodeMcpElicitationRespondParams(const McpElicitationRespondParams& value,
                                                                                  std::string& error) noexcept;
    [[nodiscard]] std::optional<frontend::Json> encodePermissionsApprovalRespondParams(const PermissionsApprovalRespondParams& value,
                                                                                       std::string& error) noexcept;

    [[nodiscard]] std::optional<typed::Unit> decodeUnitResult(const frontend::Json& value, std::string& error) noexcept;
    [[nodiscard]] std::optional<ThreadResultState> decodeOperationThreadResultState(const frontend::Json& value,
                                                                                    std::string& error) noexcept;
    [[nodiscard]] std::optional<TurnResultState> decodeOperationTurnResultState(const frontend::Json& value, std::string& error) noexcept;
    [[nodiscard]] std::optional<ThreadStartResult> decodeThreadStartResult(const frontend::Json& value, std::string& error) noexcept;
    [[nodiscard]] std::optional<ThreadResumeResult> decodeThreadResumeResult(const frontend::Json& value, std::string& error) noexcept;
    [[nodiscard]] std::optional<ThreadListResult> decodeThreadListResult(const frontend::Json& value, std::string& error) noexcept;
    [[nodiscard]] std::optional<ThreadReadResult> decodeThreadReadResult(const frontend::Json& value, std::string& error) noexcept;
    [[nodiscard]] std::optional<TurnStartResult> decodeTurnStartResult(const frontend::Json& value, std::string& error) noexcept;

} // namespace ai::openai::codex::frontend::client::detail

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_DETAIL_OPERATIONCODECS_H
