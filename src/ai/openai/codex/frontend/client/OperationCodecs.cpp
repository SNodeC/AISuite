/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/detail/OperationCodecs.h"

#include "ai/openai/codex/detail/ApprovalCodec.h"
#include "ai/openai/codex/detail/McpReverseRequestCodec.h"
#include "ai/openai/codex/frontend/client/detail/StateReducer.h"

#include <utility>

namespace ai::openai::codex::frontend::client::detail {

    namespace {

        frontend::Json pendingRequestObject(const PendingRequestId& pendingRequestId) {
            frontend::Json result = frontend::Json::object();
            result["pendingRequestId"] = pendingRequestId.value;
            return result;
        }

        void encodeProtocolError(frontend::Json& result, const ::ai::openai::codex::ProtocolError& value) {
            result["code"] = value.code;
            result["message"] = value.message;
            if (value.data) {
                result["data"] = *value.data;
            }
        }

        template <typename Value, typename Encoder>
        std::optional<frontend::Json>
        encodeNestedResponse(const PendingRequestId& pendingRequestId, const Value& value, Encoder encoder, std::string& error) noexcept {
            try {
                std::optional<frontend::Json> encoded = encoder(value, error);
                if (!encoded) {
                    return std::nullopt;
                }
                frontend::Json result = pendingRequestObject(pendingRequestId);
                result["response"] = std::move(*encoded);
                error.clear();
                return std::optional<frontend::Json>{std::move(result)};
            } catch (...) {
                error = "reverse response parameters could not be encoded";
                return std::nullopt;
            }
        }

    } // namespace

    std::optional<frontend::Json> encodeUnitParams(const typed::Unit&, std::string& error) noexcept {
        error.clear();
        frontend::Json result = frontend::Json::object();
        return std::optional<frontend::Json>{std::move(result)};
    }

    std::optional<frontend::Json> encodeEventsReplayParams(frontend::SequenceNumber after, std::string& error) noexcept {
        try {
            frontend::Json result = frontend::Json::object();
            result["after"] = after.value();
            error.clear();
            return std::optional<frontend::Json>{std::move(result)};
        } catch (...) {
            error = "frontend replay parameters could not be encoded";
            return std::nullopt;
        }
    }

    std::optional<ControllerResult>
    decodeControllerResult(const frontend::Json& value, std::optional<std::string_view> currentSessionId, std::string& error) noexcept {
        try {
            if (!value.is_object()) {
                error = "frontend controller result must be an object";
                return std::nullopt;
            }
            const auto role = value.find("role");
            if (role == value.end() || !role->is_string()) {
                error = "frontend controller result must contain a role";
                return std::nullopt;
            }

            ControllerResult result;
            const std::string decodedRole = role->get<std::string>();
            if (decodedRole == "controller") {
                result.role = frontend::SessionRole::Controller;
            } else if (decodedRole == "observer") {
                result.role = frontend::SessionRole::Observer;
            } else {
                error = "frontend controller result contains an unknown role";
                return std::nullopt;
            }

            if (const auto controllerSessionId = value.find("controllerSessionId"); controllerSessionId != value.end()) {
                if (!controllerSessionId->is_string()) {
                    error = "frontend controller result contains an invalid controller session ID";
                    return std::nullopt;
                }
                result.controllerSessionId = controllerSessionId->get<std::string>();
                if (result.controllerSessionId->empty()) {
                    error = "frontend controller result contains an empty controller session ID";
                    return std::nullopt;
                }
            }
            result.ownedByThisClient = result.role == frontend::SessionRole::Controller && currentSessionId && result.controllerSessionId &&
                                       *result.controllerSessionId == *currentSessionId;
            error.clear();
            return result;
        } catch (...) {
            error = "frontend controller result could not be decoded";
            return std::nullopt;
        }
    }

    std::optional<SynchronizationResult> decodeSnapshotSynchronizationResult(SynchronizationDecodeInput input,
                                                                             std::string& error) noexcept {
        if (input.streamMode != frontend::SyncMode::Snapshot) {
            error = "snapshot synchronization completed with a replay stream";
            return std::nullopt;
        }
        error.clear();
        return SynchronizationResult{input.streamMode,
                                     input.synchronizedThrough,
                                     std::move(input.state),
                                     input.receivedEvents,
                                     input.appliedEvents,
                                     input.ignoredAlreadyAppliedEvents,
                                     input.snapshotFallback};
    }

    std::optional<SynchronizationResult> decodeReplaySynchronizationResult(SynchronizationDecodeInput input, std::string& error) noexcept {
        const bool replay = input.streamMode == frontend::SyncMode::Replay && !input.snapshotFallback;
        const bool snapshotFallback = input.streamMode == frontend::SyncMode::Snapshot && input.snapshotFallback;
        if (!replay && !snapshotFallback) {
            error = "replay synchronization completed with an inconsistent stream mode";
            return std::nullopt;
        }
        error.clear();
        return SynchronizationResult{input.streamMode,
                                     input.synchronizedThrough,
                                     std::move(input.state),
                                     input.receivedEvents,
                                     input.appliedEvents,
                                     input.ignoredAlreadyAppliedEvents,
                                     input.snapshotFallback};
    }

    std::optional<frontend::Json> encodeApprovalRespondParams(const ApprovalRespondParams& value, std::string& error) noexcept {
        try {
            frontend::Json result = pendingRequestObject(value.pendingRequestId);
            result["decision"] = value.decision.value;
            error.clear();
            return std::optional<frontend::Json>{std::move(result)};
        } catch (...) {
            error = "approval response parameters could not be encoded";
            return std::nullopt;
        }
    }

    std::optional<frontend::Json> encodeUserInputRespondParams(const UserInputRespondParams& value, std::string& error) noexcept {
        try {
            frontend::Json result = pendingRequestObject(value.pendingRequestId);
            frontend::Json answers = frontend::Json::array();
            for (const typed::UserInputAnswer& answer : value.answers) {
                frontend::Json encodedAnswer = frontend::Json::object();
                encodedAnswer["questionId"] = answer.questionId;
                frontend::Json encodedValues = frontend::Json::array();
                for (const std::string& item : answer.answers) {
                    encodedValues.push_back(item);
                }
                encodedAnswer["answers"] = std::move(encodedValues);
                answers.push_back(std::move(encodedAnswer));
            }
            result["answers"] = std::move(answers);
            error.clear();
            return std::optional<frontend::Json>{std::move(result)};
        } catch (...) {
            error = "user-input response parameters could not be encoded";
            return std::nullopt;
        }
    }

    std::optional<frontend::Json> encodeAuthenticationRespondParams(const AuthenticationRespondParams& value, std::string& error) noexcept {
        try {
            frontend::Json result = pendingRequestObject(value.pendingRequestId);
            result["accessToken"] = value.response.accessToken;
            result["chatgptAccountId"] = value.response.chatgptAccountId;
            if (value.response.chatgptPlanType) {
                result["chatgptPlanType"] = *value.response.chatgptPlanType;
            }
            error.clear();
            return std::optional<frontend::Json>{std::move(result)};
        } catch (...) {
            error = "authentication response parameters could not be encoded";
            return std::nullopt;
        }
    }

    std::optional<frontend::Json> encodeUnknownRequestRespondParams(const UnknownRequestRespondParams& value, std::string& error) noexcept {
        try {
            frontend::Json result = pendingRequestObject(value.pendingRequestId);
            result["result"] = value.result;
            error.clear();
            return std::optional<frontend::Json>{std::move(result)};
        } catch (...) {
            error = "unknown-request result could not be encoded";
            return std::nullopt;
        }
    }

    std::optional<frontend::Json> encodeUnknownRequestRejectParams(const UnknownRequestRejectParams& value, std::string& error) noexcept {
        try {
            frontend::Json result = pendingRequestObject(value.pendingRequestId);
            encodeProtocolError(result, value.error);
            error.clear();
            return std::optional<frontend::Json>{std::move(result)};
        } catch (...) {
            error = "unknown-request rejection could not be encoded";
            return std::nullopt;
        }
    }

    std::optional<frontend::Json> encodeApplyPatchApprovalRespondParams(const ApplyPatchApprovalRespondParams& value,
                                                                        std::string& error) noexcept {
        return encodeNestedResponse(
            value.pendingRequestId, value.response, ::ai::openai::codex::detail::encodeApplyPatchApprovalResponse, error);
    }

    std::optional<frontend::Json> encodeAttestationRespondParams(const AttestationRespondParams& value, std::string& error) noexcept {
        return encodeNestedResponse(
            value.pendingRequestId, value.response, ::ai::openai::codex::detail::encodeAttestationGenerateResponse, error);
    }

    std::optional<frontend::Json> encodeDynamicToolRespondParams(const DynamicToolRespondParams& value, std::string& error) noexcept {
        return encodeNestedResponse(
            value.pendingRequestId, value.response, ::ai::openai::codex::detail::encodeDynamicToolCallResponse, error);
    }

    std::optional<frontend::Json> encodeExecCommandApprovalRespondParams(const ExecCommandApprovalRespondParams& value,
                                                                         std::string& error) noexcept {
        return encodeNestedResponse(
            value.pendingRequestId, value.response, ::ai::openai::codex::detail::encodeExecCommandApprovalResponse, error);
    }

    std::optional<frontend::Json> encodeKnownRequestRejectParams(const KnownRequestRejectParams& value, std::string& error) noexcept {
        try {
            frontend::Json result = pendingRequestObject(value.pendingRequestId);
            frontend::Json encodedError = frontend::Json::object();
            encodeProtocolError(encodedError, value.error);
            result["error"] = std::move(encodedError);
            error.clear();
            return std::optional<frontend::Json>{std::move(result)};
        } catch (...) {
            error = "known-request rejection could not be encoded";
            return std::nullopt;
        }
    }

    std::optional<frontend::Json> encodeMcpElicitationRespondParams(const McpElicitationRespondParams& value, std::string& error) noexcept {
        return encodeNestedResponse(
            value.pendingRequestId, value.response, ::ai::openai::codex::detail::encodeMcpServerElicitationRequestResponse, error);
    }

    std::optional<frontend::Json> encodePermissionsApprovalRespondParams(const PermissionsApprovalRespondParams& value,
                                                                         std::string& error) noexcept {
        return encodeNestedResponse(
            value.pendingRequestId, value.response, ::ai::openai::codex::detail::encodePermissionsRequestApprovalResponse, error);
    }

    std::optional<typed::Unit> decodeUnitResult(const frontend::Json& value, std::string& error) noexcept {
        if (!value.is_object()) {
            error = "frontend Unit result must be an object";
            return std::nullopt;
        }
        // The generated method schema has already validated and bounded any
        // additive safe fields.  Unit is the stable typed value; read-only
        // protocol observability retains the complete server message.
        error.clear();
        return typed::Unit{};
    }

    namespace {

        std::optional<ProjectedThreadResult> decodeProjectedThreadResult(const frontend::Json& value, std::string& error) noexcept {
            try {
                if (!value.is_object()) {
                    error = "frontend projected thread result must be an object";
                    return std::nullopt;
                }
                const auto thread = value.find("thread");
                const auto threadId = value.find("threadId");
                if ((thread != value.end()) == (threadId != value.end())) {
                    error = "frontend projected thread result must contain exactly one of thread or threadId";
                    return std::nullopt;
                }

                ProjectedThreadResult result;
                if (thread != value.end()) {
                    std::optional<ThreadResultState> decoded = StateReducer::decodeThreadResultState(*thread, error);
                    if (!decoded) {
                        return std::nullopt;
                    }
                    result.threadId = decoded->state.id;
                    result.thread = std::move(decoded);
                } else {
                    if (!threadId->is_string()) {
                        error = "frontend projected threadId must be a string";
                        return std::nullopt;
                    }
                    std::string id = threadId->get<std::string>();
                    if (id.empty()) {
                        error = "frontend projected threadId must not be empty";
                        return std::nullopt;
                    }
                    result.threadId = typed::ThreadId{std::move(id)};
                }
                error.clear();
                return result;
            } catch (...) {
                error = "frontend projected thread result could not be decoded";
                return std::nullopt;
            }
        }

    } // namespace

    std::optional<ThreadStartResult> decodeThreadStartResult(const frontend::Json& value, std::string& error) noexcept {
        return decodeProjectedThreadResult(value, error);
    }

    std::optional<ThreadResumeResult> decodeThreadResumeResult(const frontend::Json& value, std::string& error) noexcept {
        return decodeProjectedThreadResult(value, error);
    }

    std::optional<ThreadReadResult> decodeThreadReadResult(const frontend::Json& value, std::string& error) noexcept {
        return decodeProjectedThreadResult(value, error);
    }

    std::optional<ThreadListResult> decodeThreadListResult(const frontend::Json& value, std::string& error) noexcept {
        try {
            if (!value.is_object()) {
                error = "frontend thread-list result must be an object";
                return std::nullopt;
            }
            const auto threads = value.find("threads");
            if (threads == value.end() || !threads->is_array()) {
                error = "frontend thread-list result must contain a threads array";
                return std::nullopt;
            }
            ThreadListResult result;
            for (const frontend::Json& thread : *threads) {
                std::optional<ThreadResultState> decoded = StateReducer::decodeThreadResultState(thread, error);
                if (!decoded) {
                    return std::nullopt;
                }
                result.threads.push_back(std::move(*decoded));
            }
            for (const auto [name, destination] :
                 {std::pair{"nextCursor", &result.nextCursor}, std::pair{"backwardsCursor", &result.backwardsCursor}}) {
                const auto cursor = value.find(name);
                if (cursor != value.end()) {
                    if (!cursor->is_string()) {
                        error = std::string{"frontend thread-list "} + name + " must be a string";
                        return std::nullopt;
                    }
                    *destination = cursor->get<std::string>();
                }
            }
            error.clear();
            return result;
        } catch (...) {
            error = "frontend thread-list result could not be decoded";
            return std::nullopt;
        }
    }

    std::optional<TurnStartResult> decodeTurnStartResult(const frontend::Json& value, std::string& error) noexcept {
        try {
            if (!value.is_object()) {
                error = "frontend turn-start result must be an object";
                return std::nullopt;
            }
            const auto turn = value.find("turn");
            const auto turnId = value.find("turnId");
            if ((turn != value.end()) == (turnId != value.end())) {
                error = "frontend turn-start result must contain exactly one of turn or turnId";
                return std::nullopt;
            }

            TurnStartResult result;
            if (turn != value.end()) {
                std::optional<TurnResultState> decoded = StateReducer::decodeTurnResultState(*turn, error);
                if (!decoded) {
                    return std::nullopt;
                }
                result.turnId = decoded->state.id;
                result.turn = std::move(decoded);
            } else {
                if (!turnId->is_string()) {
                    error = "frontend turnId must be a string";
                    return std::nullopt;
                }
                std::string id = turnId->get<std::string>();
                if (id.empty()) {
                    error = "frontend turnId must not be empty";
                    return std::nullopt;
                }
                result.turnId = typed::TurnId{std::move(id)};
            }
            error.clear();
            return result;
        } catch (...) {
            error = "frontend turn-start result could not be decoded";
            return std::nullopt;
        }
    }

} // namespace ai::openai::codex::frontend::client::detail
