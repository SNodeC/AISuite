/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Messages.h"

#include "ai/openai/codex/frontend/Protocol.h"

#include <type_traits>
#include <utility>

namespace ai::openai::codex::frontend {

    std::string_view toString(SessionRole role) noexcept {
        switch (role) {
            case SessionRole::Observer:
                return "observer";
            case SessionRole::Controller:
                return "controller";
        }
        return "observer";
    }

    std::string_view toString(SyncMode mode) noexcept {
        switch (mode) {
            case SyncMode::Replay:
                return "replay";
            case SyncMode::Snapshot:
                return "snapshot";
        }
        return "snapshot";
    }

    std::string_view toString(ErrorCode code) noexcept {
        switch (code) {
            case ErrorCode::PermissionDenied:
                return "permission_denied";
            case ErrorCode::InvalidCommand:
                return "invalid_command";
            case ErrorCode::NotFound:
                return "not_found";
            case ErrorCode::Conflict:
                return "conflict";
            case ErrorCode::LocalSubmissionFailure:
                return "local_submission_failure";
            case ErrorCode::TypedDecodingFailure:
                return "typed_decoding_failure";
            case ErrorCode::RemoteAppServerError:
                return "remote_app_server_error";
            case ErrorCode::Cancelled:
                return "cancelled";
            case ErrorCode::BackendUnavailable:
                return "backend_unavailable";
            case ErrorCode::DuplicateRequestId:
                return "duplicate_request_id";
            case ErrorCode::MalformedJson:
                return "malformed_json";
            case ErrorCode::WrongProtocol:
                return "wrong_protocol";
            case ErrorCode::UnsupportedVersion:
                return "unsupported_version";
            case ErrorCode::MissingField:
                return "missing_field";
            case ErrorCode::InvalidField:
                return "invalid_field";
            case ErrorCode::UnknownKind:
                return "unknown_kind";
            case ErrorCode::UnknownMethod:
                return "unknown_method";
            case ErrorCode::FrameTooLarge:
                return "frame_too_large";
            case ErrorCode::CapacityExceeded:
                return "capacity_exceeded";
            case ErrorCode::SequenceOverflow:
                return "sequence_overflow";
            case ErrorCode::ReplayGap:
                return "replay_gap";
            case ErrorCode::InternalError:
                return "internal_error";
            case ErrorCode::AuthenticationRequired:
                return "authentication_required";
            case ErrorCode::AuthenticationFailed:
                return "authentication_failed";
            case ErrorCode::OriginRejected:
                return "origin_rejected";
            case ErrorCode::TransportSecurityRequired:
                return "transport_security_required";
            case ErrorCode::RateLimited:
                return "rate_limited";
        }
        return "internal_error";
    }

    std::string_view toString(FrontendCapability capability) noexcept {
        switch (capability) {
            case FrontendCapability::MethodDiscovery:
                return "method_discovery";
            case FrontendCapability::SecurityScopes:
                return "security_scopes";
            case FrontendCapability::CompleteProviderOperations:
                return "complete_provider_operations";
            case FrontendCapability::CompleteReverseRequests:
                return "complete_reverse_requests";
            case FrontendCapability::CompleteBackendDomains:
                return "complete_backend_domains";
            case FrontendCapability::ConditionalFilesystem:
                return "conditional_filesystem";
            case FrontendCapability::ConditionalCommandExecution:
                return "conditional_command_execution";
            case FrontendCapability::DedicatedPendingRequests:
                return "dedicated_pending_requests";
            case FrontendCapability::DedicatedNotificationEvents:
                return "dedicated_notification_events";
            case FrontendCapability::CompleteThreadItems:
                return "complete_thread_items";
            case FrontendCapability::AuthenticatedFrontend:
                return "authenticated_frontend";
            case FrontendCapability::ScopeProjectedState:
                return "scope_projected_state";
            case FrontendCapability::ProviderLifecycle:
                return "provider_lifecycle";
            case FrontendCapability::MultiTransport:
                return "multi_transport";
            case FrontendCapability::CppClientSdk:
                return "cpp_client_sdk";
            case FrontendCapability::TypescriptClientSdk:
                return "typescript_client_sdk";
            case FrontendCapability::BrowserUi:
                return "browser_ui";
            case FrontendCapability::QtUi:
                return "qt_ui";
        }
        return {};
    }

    std::string_view toString(ThreadItemKind kind) noexcept {
        switch (kind) {
            case ThreadItemKind::AgentMessage:
                return "agentMessage";
            case ThreadItemKind::CollabAgentToolCall:
                return "collabAgentToolCall";
            case ThreadItemKind::CommandExecution:
                return "commandExecution";
            case ThreadItemKind::ContextCompaction:
                return "contextCompaction";
            case ThreadItemKind::DynamicToolCall:
                return "dynamicToolCall";
            case ThreadItemKind::EnteredReviewMode:
                return "enteredReviewMode";
            case ThreadItemKind::ExitedReviewMode:
                return "exitedReviewMode";
            case ThreadItemKind::FileChange:
                return "fileChange";
            case ThreadItemKind::HookPrompt:
                return "hookPrompt";
            case ThreadItemKind::ImageGeneration:
                return "imageGeneration";
            case ThreadItemKind::ImageView:
                return "imageView";
            case ThreadItemKind::McpToolCall:
                return "mcpToolCall";
            case ThreadItemKind::Plan:
                return "plan";
            case ThreadItemKind::Reasoning:
                return "reasoning";
            case ThreadItemKind::Sleep:
                return "sleep";
            case ThreadItemKind::SubAgentActivity:
                return "subAgentActivity";
            case ThreadItemKind::UserMessage:
                return "userMessage";
            case ThreadItemKind::WebSearch:
                return "webSearch";
        }
        return {};
    }

    std::string_view toString(PendingRequestKind kind) noexcept {
        switch (kind) {
            case PendingRequestKind::CommandExecutionApproval:
                return "command_execution_approval";
            case PendingRequestKind::FileChangeApproval:
                return "file_change_approval";
            case PendingRequestKind::UserInput:
                return "user_input";
            case PendingRequestKind::Authentication:
                return "authentication";
            case PendingRequestKind::ApplyPatchApproval:
                return "apply_patch_approval";
            case PendingRequestKind::ExecCommandApproval:
                return "exec_command_approval";
            case PendingRequestKind::PermissionsApproval:
                return "permissions_approval";
            case PendingRequestKind::Attestation:
                return "attestation";
            case PendingRequestKind::DynamicToolCall:
                return "dynamic_tool_call";
            case PendingRequestKind::McpElicitation:
                return "mcp_elicitation";
        }
        return {};
    }

    std::string_view toString(ExpandedEventType type) noexcept {
        switch (type) {
            case ExpandedEventType::ProviderUpdated:
                return "provider.updated";
            case ExpandedEventType::ControllerUpdated:
                return "controller.updated";
            case ExpandedEventType::SessionsUpdated:
                return "sessions.updated";
            case ExpandedEventType::ThreadUpserted:
                return "thread.upserted";
            case ExpandedEventType::ThreadRemoved:
                return "thread.removed";
            case ExpandedEventType::TurnUpserted:
                return "turn.upserted";
            case ExpandedEventType::ItemUpserted:
                return "item.upserted";
            case ExpandedEventType::ItemContentUpdated:
                return "item.content.updated";
            case ExpandedEventType::PendingRequestsUpdated:
                return "pendingRequests.updated";
            case ExpandedEventType::AccountUpdated:
                return "account.updated";
            case ExpandedEventType::ModelsUpdated:
                return "models.updated";
            case ExpandedEventType::ConfigurationUpdated:
                return "configuration.updated";
            case ExpandedEventType::ProcessUpdated:
                return "process.updated";
            case ExpandedEventType::FilesystemWatchUpdated:
                return "filesystemWatch.updated";
            case ExpandedEventType::FuzzySearchUpdated:
                return "fuzzySearch.updated";
            case ExpandedEventType::ReviewsUpdated:
                return "reviews.updated";
            case ExpandedEventType::IntegrationsUpdated:
                return "integrations.updated";
            case ExpandedEventType::PluginsUpdated:
                return "plugins.updated";
            case ExpandedEventType::SkillsUpdated:
                return "skills.updated";
            case ExpandedEventType::McpUpdated:
                return "mcp.updated";
            case ExpandedEventType::PlatformUpdated:
                return "platform.updated";
            case ExpandedEventType::NoticeAdded:
                return "notice.added";
            case ExpandedEventType::ActivityUpdated:
                return "activity.updated";
            case ExpandedEventType::CapacityUpdated:
                return "capacity.updated";
            case ExpandedEventType::DiagnosticsUpdated:
                return "diagnostics.updated";
        }
        return {};
    }

    std::string_view toString(StateFreshness freshness) noexcept {
        switch (freshness) {
            case StateFreshness::Unknown:
                return "unknown";
            case StateFreshness::Current:
                return "current";
            case StateFreshness::Stale:
                return "stale";
        }
        return {};
    }

    std::optional<SessionRole> sessionRoleFromString(std::string_view value) noexcept {
        if (value == "observer") {
            return SessionRole::Observer;
        }
        if (value == "controller") {
            return SessionRole::Controller;
        }
        return std::nullopt;
    }

    std::optional<SyncMode> syncModeFromString(std::string_view value) noexcept {
        if (value == "replay") {
            return SyncMode::Replay;
        }
        if (value == "snapshot") {
            return SyncMode::Snapshot;
        }
        return std::nullopt;
    }

    std::optional<ErrorCode> errorCodeFromString(std::string_view value) noexcept {
#define FRONTEND_ERROR_CODE(name, spelling)                                                                                                \
    if (value == spelling) {                                                                                                               \
        return ErrorCode::name;                                                                                                            \
    }
        FRONTEND_ERROR_CODE(PermissionDenied, "permission_denied")
        FRONTEND_ERROR_CODE(InvalidCommand, "invalid_command")
        FRONTEND_ERROR_CODE(NotFound, "not_found")
        FRONTEND_ERROR_CODE(Conflict, "conflict")
        FRONTEND_ERROR_CODE(LocalSubmissionFailure, "local_submission_failure")
        FRONTEND_ERROR_CODE(TypedDecodingFailure, "typed_decoding_failure")
        FRONTEND_ERROR_CODE(RemoteAppServerError, "remote_app_server_error")
        FRONTEND_ERROR_CODE(Cancelled, "cancelled")
        FRONTEND_ERROR_CODE(BackendUnavailable, "backend_unavailable")
        FRONTEND_ERROR_CODE(DuplicateRequestId, "duplicate_request_id")
        FRONTEND_ERROR_CODE(MalformedJson, "malformed_json")
        FRONTEND_ERROR_CODE(WrongProtocol, "wrong_protocol")
        FRONTEND_ERROR_CODE(UnsupportedVersion, "unsupported_version")
        FRONTEND_ERROR_CODE(MissingField, "missing_field")
        FRONTEND_ERROR_CODE(InvalidField, "invalid_field")
        FRONTEND_ERROR_CODE(UnknownKind, "unknown_kind")
        FRONTEND_ERROR_CODE(UnknownMethod, "unknown_method")
        FRONTEND_ERROR_CODE(FrameTooLarge, "frame_too_large")
        FRONTEND_ERROR_CODE(CapacityExceeded, "capacity_exceeded")
        FRONTEND_ERROR_CODE(SequenceOverflow, "sequence_overflow")
        FRONTEND_ERROR_CODE(ReplayGap, "replay_gap")
        FRONTEND_ERROR_CODE(InternalError, "internal_error")
        FRONTEND_ERROR_CODE(AuthenticationRequired, "authentication_required")
        FRONTEND_ERROR_CODE(AuthenticationFailed, "authentication_failed")
        FRONTEND_ERROR_CODE(OriginRejected, "origin_rejected")
        FRONTEND_ERROR_CODE(TransportSecurityRequired, "transport_security_required")
        FRONTEND_ERROR_CODE(RateLimited, "rate_limited")
#undef FRONTEND_ERROR_CODE
        return std::nullopt;
    }

    std::optional<FrontendCapability> frontendCapabilityFromString(std::string_view value) noexcept {
#define FRONTEND_CAPABILITY(name, spelling)                                                                                                \
    if (value == spelling) {                                                                                                               \
        return FrontendCapability::name;                                                                                                   \
    }
        FRONTEND_CAPABILITY(MethodDiscovery, "method_discovery")
        FRONTEND_CAPABILITY(SecurityScopes, "security_scopes")
        FRONTEND_CAPABILITY(CompleteProviderOperations, "complete_provider_operations")
        FRONTEND_CAPABILITY(CompleteReverseRequests, "complete_reverse_requests")
        FRONTEND_CAPABILITY(CompleteBackendDomains, "complete_backend_domains")
        FRONTEND_CAPABILITY(ConditionalFilesystem, "conditional_filesystem")
        FRONTEND_CAPABILITY(ConditionalCommandExecution, "conditional_command_execution")
        FRONTEND_CAPABILITY(DedicatedPendingRequests, "dedicated_pending_requests")
        FRONTEND_CAPABILITY(DedicatedNotificationEvents, "dedicated_notification_events")
        FRONTEND_CAPABILITY(CompleteThreadItems, "complete_thread_items")
        FRONTEND_CAPABILITY(AuthenticatedFrontend, "authenticated_frontend")
        FRONTEND_CAPABILITY(ScopeProjectedState, "scope_projected_state")
        FRONTEND_CAPABILITY(ProviderLifecycle, "provider_lifecycle")
        FRONTEND_CAPABILITY(MultiTransport, "multi_transport")
        FRONTEND_CAPABILITY(CppClientSdk, "cpp_client_sdk")
        FRONTEND_CAPABILITY(TypescriptClientSdk, "typescript_client_sdk")
        FRONTEND_CAPABILITY(BrowserUi, "browser_ui")
        FRONTEND_CAPABILITY(QtUi, "qt_ui")
#undef FRONTEND_CAPABILITY
        return std::nullopt;
    }

    std::optional<ThreadItemKind> threadItemKindFromString(std::string_view value) noexcept {
#define FRONTEND_THREAD_ITEM_KIND(name, spelling)                                                                                          \
    if (value == spelling) {                                                                                                               \
        return ThreadItemKind::name;                                                                                                       \
    }
        FRONTEND_THREAD_ITEM_KIND(AgentMessage, "agentMessage")
        FRONTEND_THREAD_ITEM_KIND(CollabAgentToolCall, "collabAgentToolCall")
        FRONTEND_THREAD_ITEM_KIND(CommandExecution, "commandExecution")
        FRONTEND_THREAD_ITEM_KIND(ContextCompaction, "contextCompaction")
        FRONTEND_THREAD_ITEM_KIND(DynamicToolCall, "dynamicToolCall")
        FRONTEND_THREAD_ITEM_KIND(EnteredReviewMode, "enteredReviewMode")
        FRONTEND_THREAD_ITEM_KIND(ExitedReviewMode, "exitedReviewMode")
        FRONTEND_THREAD_ITEM_KIND(FileChange, "fileChange")
        FRONTEND_THREAD_ITEM_KIND(HookPrompt, "hookPrompt")
        FRONTEND_THREAD_ITEM_KIND(ImageGeneration, "imageGeneration")
        FRONTEND_THREAD_ITEM_KIND(ImageView, "imageView")
        FRONTEND_THREAD_ITEM_KIND(McpToolCall, "mcpToolCall")
        FRONTEND_THREAD_ITEM_KIND(Plan, "plan")
        FRONTEND_THREAD_ITEM_KIND(Reasoning, "reasoning")
        FRONTEND_THREAD_ITEM_KIND(Sleep, "sleep")
        FRONTEND_THREAD_ITEM_KIND(SubAgentActivity, "subAgentActivity")
        FRONTEND_THREAD_ITEM_KIND(UserMessage, "userMessage")
        FRONTEND_THREAD_ITEM_KIND(WebSearch, "webSearch")
#undef FRONTEND_THREAD_ITEM_KIND
        return std::nullopt;
    }

    std::optional<PendingRequestKind> pendingRequestKindFromString(std::string_view value) noexcept {
#define FRONTEND_PENDING_REQUEST_KIND(name, spelling)                                                                                      \
    if (value == spelling) {                                                                                                               \
        return PendingRequestKind::name;                                                                                                   \
    }
        FRONTEND_PENDING_REQUEST_KIND(CommandExecutionApproval, "command_execution_approval")
        FRONTEND_PENDING_REQUEST_KIND(FileChangeApproval, "file_change_approval")
        FRONTEND_PENDING_REQUEST_KIND(UserInput, "user_input")
        FRONTEND_PENDING_REQUEST_KIND(Authentication, "authentication")
        FRONTEND_PENDING_REQUEST_KIND(ApplyPatchApproval, "apply_patch_approval")
        FRONTEND_PENDING_REQUEST_KIND(ExecCommandApproval, "exec_command_approval")
        FRONTEND_PENDING_REQUEST_KIND(PermissionsApproval, "permissions_approval")
        FRONTEND_PENDING_REQUEST_KIND(Attestation, "attestation")
        FRONTEND_PENDING_REQUEST_KIND(DynamicToolCall, "dynamic_tool_call")
        FRONTEND_PENDING_REQUEST_KIND(McpElicitation, "mcp_elicitation")
#undef FRONTEND_PENDING_REQUEST_KIND
        return std::nullopt;
    }

    std::optional<ExpandedEventType> expandedEventTypeFromString(std::string_view value) noexcept {
#define FRONTEND_EXPANDED_EVENT_TYPE(name, spelling)                                                                                       \
    if (value == spelling) {                                                                                                               \
        return ExpandedEventType::name;                                                                                                    \
    }
        FRONTEND_EXPANDED_EVENT_TYPE(ProviderUpdated, "provider.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(ControllerUpdated, "controller.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(SessionsUpdated, "sessions.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(ThreadUpserted, "thread.upserted")
        FRONTEND_EXPANDED_EVENT_TYPE(ThreadRemoved, "thread.removed")
        FRONTEND_EXPANDED_EVENT_TYPE(TurnUpserted, "turn.upserted")
        FRONTEND_EXPANDED_EVENT_TYPE(ItemUpserted, "item.upserted")
        FRONTEND_EXPANDED_EVENT_TYPE(ItemContentUpdated, "item.content.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(PendingRequestsUpdated, "pendingRequests.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(AccountUpdated, "account.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(ModelsUpdated, "models.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(ConfigurationUpdated, "configuration.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(ProcessUpdated, "process.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(FilesystemWatchUpdated, "filesystemWatch.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(FuzzySearchUpdated, "fuzzySearch.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(ReviewsUpdated, "reviews.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(IntegrationsUpdated, "integrations.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(PluginsUpdated, "plugins.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(SkillsUpdated, "skills.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(McpUpdated, "mcp.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(PlatformUpdated, "platform.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(NoticeAdded, "notice.added")
        FRONTEND_EXPANDED_EVENT_TYPE(ActivityUpdated, "activity.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(CapacityUpdated, "capacity.updated")
        FRONTEND_EXPANDED_EVENT_TYPE(DiagnosticsUpdated, "diagnostics.updated")
#undef FRONTEND_EXPANDED_EVENT_TYPE
        return std::nullopt;
    }

    std::optional<StateFreshness> stateFreshnessFromString(std::string_view value) noexcept {
#define FRONTEND_STATE_FRESHNESS(name, spelling)                                                                                           \
    if (value == spelling) {                                                                                                               \
        return StateFreshness::name;                                                                                                       \
    }
        FRONTEND_STATE_FRESHNESS(Unknown, "unknown")
        FRONTEND_STATE_FRESHNESS(Current, "current")
        FRONTEND_STATE_FRESHNESS(Stale, "stale")
#undef FRONTEND_STATE_FRESHNESS
        return std::nullopt;
    }

    std::string_view toString(CommandMethod methodValue) noexcept {
        switch (methodValue) {
            case CommandMethod::ControllerAcquire:
                return method::ControllerAcquire;
            case CommandMethod::ControllerRelease:
                return method::ControllerRelease;
            case CommandMethod::SnapshotGet:
                return method::SnapshotGet;
            case CommandMethod::EventsReplay:
                return method::EventsReplay;
            case CommandMethod::ThreadStart:
                return method::ThreadStart;
            case CommandMethod::ThreadResume:
                return method::ThreadResume;
            case CommandMethod::ThreadList:
                return method::ThreadList;
            case CommandMethod::ThreadRead:
                return method::ThreadRead;
            case CommandMethod::TurnStart:
                return method::TurnStart;
            case CommandMethod::TurnInterrupt:
                return method::TurnInterrupt;
            case CommandMethod::ApprovalRespond:
                return method::ApprovalRespond;
            case CommandMethod::UserInputRespond:
                return method::UserInputRespond;
            case CommandMethod::AuthenticationRespond:
                return method::AuthenticationRespond;
            case CommandMethod::UnknownRequestRespond:
                return method::UnknownRequestRespond;
            case CommandMethod::UnknownRequestReject:
                return method::UnknownRequestReject;
        }
        return {};
    }

    std::optional<CommandMethod> commandMethodFromString(std::string_view value) noexcept {
#define FRONTEND_COMMAND_METHOD(name, spelling)                                                                                            \
    if (value == spelling) {                                                                                                               \
        return CommandMethod::name;                                                                                                        \
    }
        FRONTEND_COMMAND_METHOD(ControllerAcquire, method::ControllerAcquire)
        FRONTEND_COMMAND_METHOD(ControllerRelease, method::ControllerRelease)
        FRONTEND_COMMAND_METHOD(SnapshotGet, method::SnapshotGet)
        FRONTEND_COMMAND_METHOD(EventsReplay, method::EventsReplay)
        FRONTEND_COMMAND_METHOD(ThreadStart, method::ThreadStart)
        FRONTEND_COMMAND_METHOD(ThreadResume, method::ThreadResume)
        FRONTEND_COMMAND_METHOD(ThreadList, method::ThreadList)
        FRONTEND_COMMAND_METHOD(ThreadRead, method::ThreadRead)
        FRONTEND_COMMAND_METHOD(TurnStart, method::TurnStart)
        FRONTEND_COMMAND_METHOD(TurnInterrupt, method::TurnInterrupt)
        FRONTEND_COMMAND_METHOD(ApprovalRespond, method::ApprovalRespond)
        FRONTEND_COMMAND_METHOD(UserInputRespond, method::UserInputRespond)
        FRONTEND_COMMAND_METHOD(AuthenticationRespond, method::AuthenticationRespond)
        FRONTEND_COMMAND_METHOD(UnknownRequestRespond, method::UnknownRequestRespond)
        FRONTEND_COMMAND_METHOD(UnknownRequestReject, method::UnknownRequestReject)
#undef FRONTEND_COMMAND_METHOD
        return std::nullopt;
    }

    CommandMethod commandMethod(const CommandParameters& parameters) noexcept {
        return std::visit(
            []<typename Parameters>(const Parameters&) noexcept {
                using T = std::remove_cvref_t<Parameters>;
                if constexpr (std::is_same_v<T, ControllerAcquire>) {
                    return CommandMethod::ControllerAcquire;
                } else if constexpr (std::is_same_v<T, ControllerRelease>) {
                    return CommandMethod::ControllerRelease;
                } else if constexpr (std::is_same_v<T, SnapshotGet>) {
                    return CommandMethod::SnapshotGet;
                } else if constexpr (std::is_same_v<T, ReplayAfter>) {
                    return CommandMethod::EventsReplay;
                } else if constexpr (std::is_same_v<T, ThreadStart>) {
                    return CommandMethod::ThreadStart;
                } else if constexpr (std::is_same_v<T, ThreadResume>) {
                    return CommandMethod::ThreadResume;
                } else if constexpr (std::is_same_v<T, ThreadList>) {
                    return CommandMethod::ThreadList;
                } else if constexpr (std::is_same_v<T, ThreadRead>) {
                    return CommandMethod::ThreadRead;
                } else if constexpr (std::is_same_v<T, TurnStart>) {
                    return CommandMethod::TurnStart;
                } else if constexpr (std::is_same_v<T, TurnInterrupt>) {
                    return CommandMethod::TurnInterrupt;
                } else if constexpr (std::is_same_v<T, ApprovalRespond>) {
                    return CommandMethod::ApprovalRespond;
                } else if constexpr (std::is_same_v<T, UserInputRespond>) {
                    return CommandMethod::UserInputRespond;
                } else if constexpr (std::is_same_v<T, AuthenticationRespond>) {
                    return CommandMethod::AuthenticationRespond;
                } else if constexpr (std::is_same_v<T, UnknownRequestRespond>) {
                    return CommandMethod::UnknownRequestRespond;
                } else {
                    return CommandMethod::UnknownRequestReject;
                }
            },
            parameters);
    }

    Response Response::success(std::string requestId, Json result) {
        Response response;
        response.requestId = std::move(requestId);
        response.ok = true;
        response.result = std::move(result);
        return response;
    }

    Response Response::failure(std::string requestId, CommandError error) {
        Response response;
        response.requestId = std::move(requestId);
        response.error = std::move(error);
        return response;
    }

} // namespace ai::openai::codex::frontend
