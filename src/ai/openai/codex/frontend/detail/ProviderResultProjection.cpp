/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/detail/ProviderResultProjection.h"

#include "ai/openai/codex/frontend/Codec.h"

#include <array>
#include <type_traits>
#include <utility>
#include <variant>

namespace ai::openai::codex::frontend::detail {

    namespace {

#define AISUITE_CODEX_PROVIDER_RESULT_ALTERNATIVES(X)                                                                                      \
    X(0, typed::Unit, "Unit")                                                                                                              \
    X(1, typed::CancelLoginAccountResponse, "CancelLoginAccountResponse")                                                                  \
    X(2, typed::LoginAccountResponse, "LoginAccountResponse")                                                                              \
    X(3, typed::ConsumeAccountRateLimitResetCreditResponse, "ConsumeAccountRateLimitResetCreditResponse")                                  \
    X(4, typed::ConfigReadResponse, "ConfigReadResponse")                                                                                  \
    X(5, typed::ConfigRequirementsReadResponse, "ConfigRequirementsReadResponse")                                                          \
    X(6, typed::ConfigWriteResponse, "ConfigWriteResponse")                                                                                \
    X(7, typed::ExperimentalFeatureEnablementSetResponse, "ExperimentalFeatureEnablementSetResponse")                                      \
    X(8, typed::ExperimentalFeatureListResponse, "ExperimentalFeatureListResponse")                                                        \
    X(9, typed::GetAccountRateLimitsResponse, "GetAccountRateLimitsResponse")                                                              \
    X(10, typed::GetAccountResponse, "GetAccountResponse")                                                                                 \
    X(11, typed::SendAddCreditsNudgeEmailResponse, "SendAddCreditsNudgeEmailResponse")                                                     \
    X(12, typed::GetAccountTokenUsageResponse, "GetAccountTokenUsageResponse")                                                             \
    X(13, typed::GetWorkspaceMessagesResponse, "GetWorkspaceMessagesResponse")                                                             \
    X(14, typed::ModelListResponse, "ModelListResponse")                                                                                   \
    X(15, typed::ModelProviderCapabilitiesReadResponse, "ModelProviderCapabilitiesReadResponse")                                           \
    X(16, typed::ThreadForkResponse, "ThreadForkResponse")                                                                                 \
    X(17, typed::ThreadGoalClearResponse, "ThreadGoalClearResponse")                                                                       \
    X(18, typed::ThreadGoalGetResponse, "ThreadGoalGetResponse")                                                                           \
    X(19, typed::ThreadGoalSetResponse, "ThreadGoalSetResponse")                                                                           \
    X(20, typed::ThreadListResponse, "ThreadListResponse")                                                                                 \
    X(21, typed::ThreadLoadedListResponse, "ThreadLoadedListResponse")                                                                     \
    X(22, typed::ThreadMetadataUpdateResponse, "ThreadMetadataUpdateResponse")                                                             \
    X(23, typed::ThreadReadResponse, "ThreadReadResponse")                                                                                 \
    X(24, typed::ThreadResumeResponse, "ThreadResumeResponse")                                                                             \
    X(25, typed::ThreadRollbackResponse, "ThreadRollbackResponse")                                                                         \
    X(26, typed::ThreadStartResponse, "ThreadStartResponse")                                                                               \
    X(27, typed::ThreadUnarchiveResponse, "ThreadUnarchiveResponse")                                                                       \
    X(28, typed::ThreadUnsubscribeResponse, "ThreadUnsubscribeResponse")                                                                   \
    X(29, typed::TurnStartResponse, "TurnStartResponse")                                                                                   \
    X(30, typed::TurnSteerResponse, "TurnSteerResponse")                                                                                   \
    X(31, typed::CommandExecResponse, "CommandExecResponse")                                                                               \
    X(32, typed::FsGetMetadataResponse, "FsGetMetadataResponse")                                                                           \
    X(33, typed::FsReadDirectoryResponse, "FsReadDirectoryResponse")                                                                       \
    X(34, typed::FsReadFileResponse, "FsReadFileResponse")                                                                                 \
    X(35, typed::FsWatchResponse, "FsWatchResponse")                                                                                       \
    X(36, typed::FuzzyFileSearchResponse, "FuzzyFileSearchResponse")                                                                       \
    X(37, typed::PermissionProfileListResponse, "PermissionProfileListResponse")                                                           \
    X(38, typed::ReviewStartResponse, "ReviewStartResponse")                                                                               \
    X(39, typed::AppsListResponse, "AppsListResponse")                                                                                     \
    X(40, typed::ExternalAgentConfigDetectResponse, "ExternalAgentConfigDetectResponse")                                                   \
    X(41, typed::ExternalAgentConfigImportResponse, "ExternalAgentConfigImportResponse")                                                   \
    X(42, typed::ExternalAgentConfigImportHistoriesReadResponse, "ExternalAgentConfigImportHistoriesReadResponse")                         \
    X(43, typed::FeedbackUploadResponse, "FeedbackUploadResponse")                                                                         \
    X(44, typed::HooksListResponse, "HooksListResponse")                                                                                   \
    X(45, typed::MarketplaceAddResponse, "MarketplaceAddResponse")                                                                         \
    X(46, typed::MarketplaceRemoveResponse, "MarketplaceRemoveResponse")                                                                   \
    X(47, typed::MarketplaceUpgradeResponse, "MarketplaceUpgradeResponse")                                                                 \
    X(48, typed::PluginInstallResponse, "PluginInstallResponse")                                                                           \
    X(49, typed::PluginShareCheckoutResponse, "PluginShareCheckoutResponse")                                                               \
    X(50, typed::PluginShareSaveResponse, "PluginShareSaveResponse")                                                                       \
    X(51, typed::PluginShareUpdateTargetsResponse, "PluginShareUpdateTargetsResponse")                                                     \
    X(52, typed::PluginSkillReadResponse, "PluginSkillReadResponse")                                                                       \
    X(53, typed::SkillsConfigWriteResponse, "SkillsConfigWriteResponse")                                                                   \
    X(54, typed::SkillsListResponse, "SkillsListResponse")                                                                                 \
    X(55, typed::PluginInstalledResponse, "PluginInstalledResponse")                                                                       \
    X(56, typed::PluginListResponse, "PluginListResponse")                                                                                 \
    X(57, typed::PluginReadResponse, "PluginReadResponse")                                                                                 \
    X(58, typed::PluginShareListResponse, "PluginShareListResponse")                                                                       \
    X(59, typed::McpServerOauthLoginResponse, "McpServerOauthLoginResponse")                                                               \
    X(60, typed::McpResourceReadResponse, "McpResourceReadResponse")                                                                       \
    X(61, typed::McpServerToolCallResponse, "McpServerToolCallResponse")                                                                   \
    X(62, typed::ListMcpServerStatusResponse, "ListMcpServerStatusResponse")                                                               \
    X(63, typed::WindowsSandboxReadinessResponse, "WindowsSandboxReadinessResponse")                                                       \
    X(64, typed::WindowsSandboxSetupStartResponse, "WindowsSandboxSetupStartResponse")

        constexpr std::array<std::string_view, ProviderResultAlternativeCount> ProviderResultTypeNames{{
#define AISUITE_CODEX_PROVIDER_RESULT_NAME(Index, Type, Name) Name,
            AISUITE_CODEX_PROVIDER_RESULT_ALTERNATIVES(AISUITE_CODEX_PROVIDER_RESULT_NAME)
#undef AISUITE_CODEX_PROVIDER_RESULT_NAME
        }};

        static_assert(std::variant_size_v<backend::ProviderOperationValue> == ProviderResultAlternativeCount);
        static_assert(std::variant_size_v<backend::CommandValue> == ProviderResultAlternativeCount + 3);

#define AISUITE_CODEX_PROVIDER_RESULT_ORDER(Index, Type, Name)                                                                             \
    static_assert(std::is_same_v<std::variant_alternative_t<Index, backend::ProviderOperationValue>, Type>);                               \
    static_assert(std::is_same_v<std::variant_alternative_t<Index + 3, backend::CommandValue>, Type>);
        AISUITE_CODEX_PROVIDER_RESULT_ALTERNATIVES(AISUITE_CODEX_PROVIDER_RESULT_ORDER)
#undef AISUITE_CODEX_PROVIDER_RESULT_ORDER

        template <typename T, typename Variant>
        struct VariantContains;

        template <typename T, typename... Alternatives>
        struct VariantContains<T, std::variant<Alternatives...>> : std::bool_constant<(std::is_same_v<T, Alternatives> || ...)> {};

        template <typename T>
        inline constexpr bool IsProviderResult = VariantContains<std::remove_cvref_t<T>, backend::ProviderOperationValue>::value;

        template <typename>
        inline constexpr bool AlwaysFalse = false;

        template <typename Result>
        Json retainedProviderResultJson(const Result& result) {
            if constexpr (std::is_same_v<Result, typed::Unit>) {
                return Json::object();
            } else if constexpr (std::is_same_v<Result, typed::LoginAccountResponse>) {
                return typed::loginAccountResponseRaw(result);
            } else if constexpr (requires { result.raw; }) {
                return result.raw;
            } else {
                static_assert(AlwaysFalse<Result>, "provider result without retained raw data needs an explicit safe encoder");
            }
        }

        const generated::MethodMetadata* metadataFor(generated::MethodId method) noexcept {
            const std::size_t index = static_cast<std::size_t>(method);
            return index < generated::AllMethods.size() ? &generated::AllMethods[index] : nullptr;
        }

    } // namespace

    std::span<const std::string_view, ProviderResultAlternativeCount> providerResultTypeNames() noexcept {
        return ProviderResultTypeNames;
    }

    std::string_view providerResultTypeName(const backend::CommandValue& value) noexcept {
        constexpr std::size_t CommandPrefixAlternatives = 3;
        if (value.index() < CommandPrefixAlternatives) {
            return {};
        }
        const std::size_t providerIndex = value.index() - CommandPrefixAlternatives;
        return providerIndex < ProviderResultTypeNames.size() ? ProviderResultTypeNames[providerIndex] : std::string_view{};
    }

    bool requiresLegacyProviderResultProjection(generated::MethodId method) noexcept {
        const generated::MethodMetadata* metadata = metadataFor(method);
        return metadata != nullptr && metadata->category == generated::MethodCategory::ProviderOperation &&
               metadata->legacyCompatibilityMethod;
    }

    ProviderResultProjection
    projectProviderResult(generated::MethodId method, const backend::CommandValue& value, std::size_t maximumSerializedBytes) noexcept {
        try {
            const generated::MethodMetadata* metadata = metadataFor(method);
            if (metadata == nullptr || metadata->category != generated::MethodCategory::ProviderOperation) {
                return {ProviderResultProjectionStatus::NotProviderResult, Json::object()};
            }

            const std::string_view actualResultType = providerResultTypeName(value);
            if (actualResultType.empty()) {
                return {ProviderResultProjectionStatus::NotProviderResult, Json::object()};
            }
            if (actualResultType != metadata->resultType) {
                return {ProviderResultProjectionStatus::ResultTypeMismatch, Json::object()};
            }
            if (metadata->legacyCompatibilityMethod) {
                return {ProviderResultProjectionStatus::LegacyProjectionRequired, Json::object()};
            }

            Json projected = std::visit(
                []<typename Result>(const Result& result) -> Json {
                    if constexpr (IsProviderResult<Result>) {
                        return retainedProviderResultJson(result);
                    } else {
                        return Json::object();
                    }
                },
                value);
            if (projected.dump().size() > maximumSerializedBytes) {
                return {ProviderResultProjectionStatus::ResultTooLarge, Json::object()};
            }
            if (!Codec::decodeDefinedResult(method, projected)) {
                return {ProviderResultProjectionStatus::InvalidResult, Json::object()};
            }
            return {ProviderResultProjectionStatus::Success, std::move(projected)};
        } catch (...) {
            return {ProviderResultProjectionStatus::InvalidResult, Json::object()};
        }
    }

#undef AISUITE_CODEX_PROVIDER_RESULT_ALTERNATIVES

} // namespace ai::openai::codex::frontend::detail
