/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/detail/BackendCommandMapper.h"

#include "ai/openai/codex/detail/ApprovalCodec.h"
#include "ai/openai/codex/detail/ConversationCodec.h"
#include "ai/openai/codex/detail/ExternalAgentCodec.h"
#include "ai/openai/codex/detail/ReviewCodec.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ai::openai::codex::frontend::detail {

    namespace {
        using Json = ai::openai::codex::Json;
        using generated::MethodId;

        class MappingFailure final : public std::runtime_error {
        public:
            explicit MappingFailure(std::string message)
                : std::runtime_error(std::move(message)) {
            }
        };

        [[noreturn]] void fail(std::string_view field = {}) {
            if (field.empty()) {
                throw MappingFailure("validated frontend parameters could not be converted to the typed backend command");
            }
            throw MappingFailure("validated frontend field '" + std::string(field) +
                                 "' could not be converted to the typed backend command");
        }

        const Json* member(const Json& object, std::string_view name) noexcept {
            const auto found = object.find(name);
            return found == object.end() ? nullptr : &*found;
        }

        const Json& required(const Json& object, std::string_view name) {
            const Json* value = member(object, name);
            if (value == nullptr) {
                fail(name);
            }
            return *value;
        }

        std::string stringValue(const Json& value) {
            return value.get<std::string>();
        }

        std::string requiredString(const Json& object, std::string_view name) {
            return stringValue(required(object, name));
        }

        bool requiredBool(const Json& object, std::string_view name) {
            return required(object, name).get<bool>();
        }

        template <typename Integer>
        Integer integerValue(const Json& value) {
            if constexpr (std::is_unsigned_v<Integer>) {
                const std::uint64_t number = value.get<std::uint64_t>();
                if (number > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
                    fail();
                }
                return static_cast<Integer>(number);
            } else {
                const std::int64_t number = value.get<std::int64_t>();
                if (number < static_cast<std::int64_t>(std::numeric_limits<Integer>::min()) ||
                    number > static_cast<std::int64_t>(std::numeric_limits<Integer>::max())) {
                    fail();
                }
                return static_cast<Integer>(number);
            }
        }

        template <typename T, typename Decode>
        std::optional<T> optional(const Json& object, std::string_view name, Decode&& decode) {
            const Json* value = member(object, name);
            if (value == nullptr) {
                return std::nullopt;
            }
            return std::optional<T>{decode(*value)};
        }

        template <typename T, typename Decode>
        typed::OptionalNullable<T> optionalNullable(const Json& object, std::string_view name, Decode&& decode) {
            const Json* value = member(object, name);
            if (value == nullptr) {
                return typed::OptionalNullable<T>::omitted();
            }
            if (value->is_null()) {
                return typed::OptionalNullable<T>::explicitNull();
            }
            return typed::OptionalNullable<T>::withValue(decode(*value));
        }

        template <typename T, typename Decode>
        std::vector<T> vectorValue(const Json& value, Decode&& decode) {
            std::vector<T> result;
            result.reserve(value.size());
            for (const Json& element : value) {
                result.push_back(decode(element));
            }
            return result;
        }

        std::vector<std::string> stringVector(const Json& value) {
            return vectorValue<std::string>(value, stringValue);
        }

        template <typename Strong>
        Strong strongString(const Json& value) {
            return Strong{stringValue(value)};
        }

        template <typename Strong>
        Strong requiredStrong(const Json& object, std::string_view name) {
            return strongString<Strong>(required(object, name));
        }

        template <typename Strong>
        typed::OptionalNullable<Strong> optionalStrong(const Json& object, std::string_view name) {
            return optionalNullable<Strong>(object, name, strongString<Strong>);
        }

        typed::OptionalNullable<std::string> optionalNullableString(const Json& object, std::string_view name) {
            return optionalNullable<std::string>(object, name, stringValue);
        }

        typed::OptionalNullable<bool> optionalNullableBool(const Json& object, std::string_view name) {
            return optionalNullable<bool>(object, name, [](const Json& value) {
                return value.get<bool>();
            });
        }

        template <typename Integer>
        typed::OptionalNullable<Integer> optionalNullableInteger(const Json& object, std::string_view name) {
            return optionalNullable<Integer>(object, name, integerValue<Integer>);
        }

        std::optional<bool> optionalBool(const Json& object, std::string_view name) {
            return optional<bool>(object, name, [](const Json& value) {
                return value.get<bool>();
            });
        }

        template <typename Integer>
        std::optional<Integer> optionalInteger(const Json& object, std::string_view name) {
            return optional<Integer>(object, name, integerValue<Integer>);
        }

        std::map<std::string, Json> jsonMap(const Json& value) {
            std::map<std::string, Json> result;
            for (const auto& [key, item] : value.items()) {
                result.emplace(key, item);
            }
            return result;
        }

        std::map<std::string, std::string> stringMap(const Json& value) {
            std::map<std::string, std::string> result;
            for (const auto& [key, item] : value.items()) {
                result.emplace(key, item.get<std::string>());
            }
            return result;
        }

        std::map<std::string, std::optional<std::string>> nullableStringMap(const Json& value) {
            std::map<std::string, std::optional<std::string>> result;
            for (const auto& [key, item] : value.items()) {
                result.emplace(key, item.is_null() ? std::nullopt : std::optional<std::string>{item.get<std::string>()});
            }
            return result;
        }

        template <typename T>
        T decodedConversation(ai::openai::codex::detail::ConversationDecodeResult<T> decoded) {
            if (!decoded.value.has_value()) {
                fail();
            }
            return std::move(*decoded.value);
        }

        typed::AskForApproval askForApproval(const Json& value) {
            if (value.is_string()) {
                // ApprovalPolicy is the provider protocol's open scalar
                // compatibility value.  Preserve future non-empty frontend
                // values in that encodable alternative rather than decoding
                // them into the intentionally non-encodable Unknown variant.
                return typed::ApprovalPolicy{value.get<std::string>()};
            }
            return decodedConversation(ai::openai::codex::detail::decodeAskForApproval(value));
        }

        typed::SandboxPolicy sandboxPolicy(const Json& value) {
            return decodedConversation(ai::openai::codex::detail::decodeSandboxPolicy(value));
        }

        typed::TurnInput turnInput(const Json& value) {
            typed::TurnInput result = decodedConversation(ai::openai::codex::detail::decodeUserInput(value));
            if (auto* text = std::get_if<typed::TextInput>(&result); text != nullptr && !text->textElements.has_value()) {
                // Frontend Protocol v1's TextInput deliberately exposes only
                // type/text. Preserve the provider protocol's empty-array
                // default exactly as the legacy BackendAdapter did when that
                // provider-only field is absent from the frontend value.
                text->textElements.emplace();
            }
            return result;
        }

        typed::ReviewTarget reviewTarget(const Json& value) {
            return decodedConversation(ai::openai::codex::detail::decodeReviewTarget(value));
        }

        typed::ReviewDecision reviewDecision(const Json& value) {
            return decodedConversation(ai::openai::codex::detail::decodeReviewDecision(value));
        }

        backend::PendingRequestId pendingRequestId(const Json& object) {
            const std::string encoded = requiredString(object, "pendingRequestId");
            std::uint64_t value = 0;
            const auto [end, error] = std::from_chars(encoded.data(), encoded.data() + encoded.size(), value);
            if (error != std::errc{} || end != encoded.data() + encoded.size() || value == 0) {
                fail("pendingRequestId");
            }
            return backend::PendingRequestId{value};
        }

        const Json& parameterValue(const generated::DefinedCommand& command) {
            return std::visit(
                [](const auto& parameters) -> const Json& {
                    return parameters.value;
                },
                command.parameters);
        }

        typed::CommandExecTerminalSize terminalSize(const Json& value) {
            return {integerValue<std::uint16_t>(required(value, "cols")), integerValue<std::uint16_t>(required(value, "rows"))};
        }

        typed::LoginAccountParams loginAccountParams(const Json& value) {
            const std::string type = requiredString(value, "type");
            if (type == "apiKey") {
                return typed::ApiKeyLoginAccountParams{requiredString(value, "apiKey")};
            }
            if (type == "chatgpt") {
                return typed::ChatgptLoginAccountParams{optionalStrong<typed::LoginAppBrand>(value, "appBrand"),
                                                        optionalBool(value, "codexStreamlinedLogin"),
                                                        optionalBool(value, "useHostedLoginSuccessPage")};
            }
            if (type == "chatgptDeviceCode") {
                return typed::ChatgptDeviceCodeLoginAccountParams{};
            }
            if (type == "chatgptAuthTokens") {
                return typed::ChatgptAuthTokensLoginAccountParams{requiredString(value, "accessToken"),
                                                                  requiredStrong<typed::AccountId>(value, "chatgptAccountId"),
                                                                  optionalStrong<typed::PlanType>(value, "chatgptPlanType")};
            }
            fail("type");
        }

        typed::ConfigEdit configEdit(const Json& value) {
            typed::ConfigEdit result;
            result.keyPath = requiredStrong<typed::ConfigKeyPath>(value, "keyPath");
            result.mergeStrategy = requiredStrong<typed::MergeStrategy>(value, "mergeStrategy");
            const Json& item = required(value, "value");
            result.value = item.is_null() ? std::nullopt : std::optional<Json>{item};
            return result;
        }

        typed::PluginShareTarget pluginShareTarget(const Json& value) {
            typed::PluginShareTarget result;
            result.principalId = requiredString(value, "principalId");
            result.principalType = requiredStrong<typed::PluginSharePrincipalType>(value, "principalType");
            result.role = requiredStrong<typed::PluginShareTargetRole>(value, "role");
            result.raw = value;
            return result;
        }

        typed::ThreadListCwdFilter threadListCwdFilter(const Json& value) {
            if (value.is_string()) {
                return {value.get<std::string>()};
            }
            return {stringVector(value)};
        }

        typed::ThreadMetadataGitInfoUpdateParams metadataGitInfo(const Json& value) {
            return {
                optionalNullableString(value, "branch"), optionalNullableString(value, "originUrl"), optionalNullableString(value, "sha")};
        }

        typed::GrantedPermissionProfile grantedPermissionProfile(const Json& value) {
            std::string error;
            std::optional<typed::GrantedPermissionProfile> decoded =
                ai::openai::codex::detail::decodeGrantedPermissionProfile(value, error);
            if (!decoded.has_value()) {
                fail("permissions");
            }
            return std::move(*decoded);
        }

        typed::DynamicToolCallOutputContentItem dynamicToolOutput(const Json& value) {
            return decodedConversation(ai::openai::codex::detail::decodeDynamicToolCallOutputContentItem(value));
        }

        Json mergeLegacyAliases(const generated::DefinedCommand& command, MethodId method) {
            Json params = parameterValue(command);
            if (method == MethodId::ThreadStart || method == MethodId::ThreadResume) {
                if (params.contains("sandbox") && command.parameterExtensions.contains("sandboxMode")) {
                    fail("sandbox and sandboxMode cannot both be present");
                }
                if (command.parameterExtensions.contains("sandboxMode")) {
                    params["sandbox"] = command.parameterExtensions.at("sandboxMode");
                }
            }
            if (method == MethodId::TurnStart) {
                if (params.contains("effort") && command.parameterExtensions.contains("reasoningEffort")) {
                    fail("effort and reasoningEffort cannot both be present");
                }
                if (command.parameterExtensions.contains("reasoningEffort")) {
                    params["effort"] = command.parameterExtensions.at("reasoningEffort");
                }
            }
            return params;
        }

        backend::BackendCommand mapProviderCommand(MethodId method, const Json& params) {
            switch (method) {
                case MethodId::AccountLoginCancel:
                    return backend::AccountLoginCancel{{requiredStrong<typed::LoginId>(params, "loginId")}};
                case MethodId::AccountLoginStart:
                    return backend::AccountLoginStart{loginAccountParams(params)};
                case MethodId::AccountLogout:
                    return backend::AccountLogout{};
                case MethodId::AccountRateLimitResetCreditConsume:
                    return backend::AccountRateLimitResetCreditConsume{{optionalStrong<typed::RateLimitResetCreditId>(params, "creditId"),
                                                                        requiredStrong<typed::IdempotencyKey>(params, "idempotencyKey")}};
                case MethodId::AccountRateLimitsRead:
                    return backend::AccountRateLimitsRead{};
                case MethodId::AccountRead:
                    return backend::AccountRead{{optionalBool(params, "refreshToken")}};
                case MethodId::AccountSendAddCreditsNudgeEmail:
                    return backend::AccountSendAddCreditsNudgeEmail{
                        {requiredStrong<typed::AddCreditsNudgeCreditType>(params, "creditType")}};
                case MethodId::AccountUsageRead:
                    return backend::AccountUsageRead{};
                case MethodId::AccountWorkspaceMessagesRead:
                    return backend::AccountWorkspaceMessagesRead{};

                case MethodId::ConfigBatchWrite: {
                    typed::ConfigBatchWriteParams value;
                    value.edits = vectorValue<typed::ConfigEdit>(required(params, "edits"), configEdit);
                    value.expectedVersion = optionalNullableString(params, "expectedVersion");
                    value.filePath = optionalStrong<typed::AbsolutePath>(params, "filePath");
                    value.reloadUserConfig = optionalBool(params, "reloadUserConfig");
                    return backend::ConfigBatchWrite{std::move(value)};
                }
                case MethodId::ConfigMcpServerReload:
                    return backend::ConfigMcpServerReload{};
                case MethodId::ConfigRead:
                    return backend::ConfigRead{{optionalNullableString(params, "cwd"), optionalBool(params, "includeLayers")}};
                case MethodId::ConfigValueWrite: {
                    typed::ConfigValueWriteParams value;
                    value.expectedVersion = optionalNullableString(params, "expectedVersion");
                    value.filePath = optionalStrong<typed::AbsolutePath>(params, "filePath");
                    value.keyPath = requiredStrong<typed::ConfigKeyPath>(params, "keyPath");
                    value.mergeStrategy = requiredStrong<typed::MergeStrategy>(params, "mergeStrategy");
                    const Json& item = required(params, "value");
                    value.value = item.is_null() ? std::nullopt : std::optional<Json>{item};
                    return backend::ConfigValueWrite{std::move(value)};
                }
                case MethodId::ConfigRequirementsRead:
                    return backend::ConfigRequirementsRead{};
                case MethodId::ExperimentalFeatureEnablementSet: {
                    typed::ExperimentalFeatureEnablementSetParams value;
                    for (const auto& [key, item] : required(params, "enablement").items()) {
                        value.enablement.emplace(typed::ExperimentalFeatureId{key}, item.get<bool>());
                    }
                    return backend::ExperimentalFeatureEnablementSet{std::move(value)};
                }
                case MethodId::ExperimentalFeatureList:
                    return backend::ExperimentalFeatureList{{optionalNullableString(params, "cursor"),
                                                             optionalNullableInteger<std::uint32_t>(params, "limit"),
                                                             optionalStrong<typed::ThreadId>(params, "threadId")}};

                case MethodId::ModelList:
                    return backend::ModelList{{optionalNullableString(params, "cursor"),
                                               optionalNullableBool(params, "includeHidden"),
                                               optionalNullableInteger<std::uint32_t>(params, "limit")}};
                case MethodId::ModelProviderCapabilitiesRead:
                    return backend::ModelProviderCapabilitiesRead{};

                case MethodId::ThreadStart: {
                    typed::ThreadStartParams value;
                    value.sessionStartSource = optionalStrong<typed::ThreadStartSource>(params, "sessionStartSource");
                    value.approvalPolicy = optionalNullable<typed::AskForApproval>(params, "approvalPolicy", askForApproval);
                    value.approvalsReviewer = optionalStrong<typed::ApprovalsReviewer>(params, "approvalsReviewer");
                    value.baseInstructions = optionalNullableString(params, "baseInstructions");
                    value.config = optionalNullable<typed::ProtocolConfiguration>(params, "config", jsonMap);
                    value.cwd = optionalNullableString(params, "cwd");
                    value.developerInstructions = optionalNullableString(params, "developerInstructions");
                    value.serviceName = optionalNullableString(params, "serviceName");
                    value.personality = optionalStrong<typed::Personality>(params, "personality");
                    value.ephemeral = optionalNullableBool(params, "ephemeral");
                    value.threadSource = optionalStrong<typed::ThreadSource>(params, "threadSource");
                    value.sandbox = optionalStrong<typed::SandboxMode>(params, "sandbox");
                    value.serviceTier = optionalNullableString(params, "serviceTier");
                    value.model = optionalStrong<typed::ModelId>(params, "model");
                    value.modelProvider = optionalNullableString(params, "modelProvider");
                    return backend::ThreadStart{std::move(value)};
                }
                case MethodId::ThreadResume: {
                    typed::ThreadResumeParams value;
                    value.threadId = requiredStrong<typed::ThreadId>(params, "threadId");
                    value.approvalPolicy = optionalNullable<typed::AskForApproval>(params, "approvalPolicy", askForApproval);
                    value.approvalsReviewer = optionalStrong<typed::ApprovalsReviewer>(params, "approvalsReviewer");
                    value.baseInstructions = optionalNullableString(params, "baseInstructions");
                    value.config = optionalNullable<typed::ProtocolConfiguration>(params, "config", jsonMap);
                    value.cwd = optionalNullableString(params, "cwd");
                    value.developerInstructions = optionalNullableString(params, "developerInstructions");
                    value.personality = optionalStrong<typed::Personality>(params, "personality");
                    value.serviceTier = optionalNullableString(params, "serviceTier");
                    value.model = optionalStrong<typed::ModelId>(params, "model");
                    value.modelProvider = optionalNullableString(params, "modelProvider");
                    value.sandbox = optionalStrong<typed::SandboxMode>(params, "sandbox");
                    return backend::ThreadResume{std::move(value)};
                }
                case MethodId::ThreadList: {
                    typed::ThreadListParams value;
                    value.sourceKinds = optionalNullable<std::vector<typed::ThreadSourceKind>>(params, "sourceKinds", [](const Json& item) {
                        return vectorValue<typed::ThreadSourceKind>(item, strongString<typed::ThreadSourceKind>);
                    });
                    value.archived = optionalNullableBool(params, "archived");
                    value.cursor = optionalNullableString(params, "cursor");
                    value.cwd = optionalNullable<typed::ThreadListCwdFilter>(params, "cwd", threadListCwdFilter);
                    value.limit = optionalNullableInteger<std::uint32_t>(params, "limit");
                    value.modelProviders = optionalNullable<std::vector<std::string>>(params, "modelProviders", stringVector);
                    value.useStateDbOnly = optionalBool(params, "useStateDbOnly");
                    value.searchTerm = optionalNullableString(params, "searchTerm");
                    value.sortDirection = optionalStrong<typed::SortDirection>(params, "sortDirection");
                    value.sortKey = optionalStrong<typed::ThreadSortKey>(params, "sortKey");
                    return backend::ThreadList{std::move(value)};
                }
                case MethodId::ThreadRead:
                    return backend::ThreadRead{{requiredStrong<typed::ThreadId>(params, "threadId"), optionalBool(params, "includeTurns")}};
                case MethodId::TurnStart: {
                    typed::TurnStartParams value;
                    value.threadId = requiredStrong<typed::ThreadId>(params, "threadId");
                    value.input = vectorValue<typed::TurnInput>(required(params, "input"), turnInput);
                    value.personality = optionalStrong<typed::Personality>(params, "personality");
                    value.approvalPolicy = optionalNullable<typed::AskForApproval>(params, "approvalPolicy", askForApproval);
                    value.approvalsReviewer = optionalStrong<typed::ApprovalsReviewer>(params, "approvalsReviewer");
                    value.clientUserMessageId = optionalStrong<typed::ClientUserMessageId>(params, "clientUserMessageId");
                    value.serviceTier = optionalNullableString(params, "serviceTier");
                    value.cwd = optionalNullableString(params, "cwd");
                    value.effort = optionalStrong<typed::ReasoningEffort>(params, "effort");
                    value.model = optionalStrong<typed::ModelId>(params, "model");
                    value.summary = optionalStrong<typed::ReasoningSummary>(params, "summary");
                    value.outputSchema = optionalNullable<Json>(params, "outputSchema", [](const Json& item) {
                        return item;
                    });
                    value.sandboxPolicy = optionalNullable<typed::SandboxPolicy>(params, "sandboxPolicy", sandboxPolicy);
                    return backend::TurnStart{std::move(value)};
                }
                case MethodId::TurnInterrupt:
                    return backend::TurnInterrupt{
                        {requiredStrong<typed::ThreadId>(params, "threadId"), requiredStrong<typed::TurnId>(params, "turnId")}};

                case MethodId::ThreadArchive:
                    return backend::ThreadArchive{{requiredStrong<typed::ThreadId>(params, "threadId")}};
                case MethodId::ThreadCompactStart:
                    return backend::ThreadCompactStart{{requiredStrong<typed::ThreadId>(params, "threadId")}};
                case MethodId::ThreadDelete:
                    return backend::ThreadDelete{{requiredStrong<typed::ThreadId>(params, "threadId")}};
                case MethodId::ThreadFork: {
                    typed::ThreadForkParams value;
                    value.threadId = requiredStrong<typed::ThreadId>(params, "threadId");
                    value.approvalPolicy = optionalNullable<typed::AskForApproval>(params, "approvalPolicy", askForApproval);
                    value.approvalsReviewer = optionalStrong<typed::ApprovalsReviewer>(params, "approvalsReviewer");
                    value.baseInstructions = optionalNullableString(params, "baseInstructions");
                    value.config = optionalNullable<typed::ProtocolConfiguration>(params, "config", jsonMap);
                    value.cwd = optionalNullableString(params, "cwd");
                    value.developerInstructions = optionalNullableString(params, "developerInstructions");
                    value.ephemeral = optionalBool(params, "ephemeral");
                    value.serviceTier = optionalNullableString(params, "serviceTier");
                    value.lastTurnId = optionalStrong<typed::TurnId>(params, "lastTurnId");
                    value.model = optionalStrong<typed::ModelId>(params, "model");
                    value.modelProvider = optionalNullableString(params, "modelProvider");
                    value.threadSource = optionalStrong<typed::ThreadSource>(params, "threadSource");
                    value.sandbox = optionalStrong<typed::SandboxMode>(params, "sandbox");
                    return backend::ThreadFork{std::move(value)};
                }
                case MethodId::ThreadGoalClear:
                    return backend::ThreadGoalClear{{requiredStrong<typed::ThreadId>(params, "threadId")}};
                case MethodId::ThreadGoalGet:
                    return backend::ThreadGoalGet{{requiredStrong<typed::ThreadId>(params, "threadId")}};
                case MethodId::ThreadGoalSet:
                    return backend::ThreadGoalSet{{requiredStrong<typed::ThreadId>(params, "threadId"),
                                                   optionalNullableString(params, "objective"),
                                                   optionalStrong<typed::ThreadGoalStatus>(params, "status"),
                                                   optionalNullableInteger<std::int64_t>(params, "tokenBudget")}};
                case MethodId::ThreadInjectItems:
                    return backend::ThreadInjectItems{{requiredStrong<typed::ThreadId>(params, "threadId"),
                                                       vectorValue<Json>(required(params, "items"), [](const Json& item) {
                                                           return item;
                                                       })}};
                case MethodId::ThreadLoadedList:
                    return backend::ThreadLoadedList{
                        {optionalNullableString(params, "cursor"), optionalNullableInteger<std::uint32_t>(params, "limit")}};
                case MethodId::ThreadMetadataUpdate:
                    return backend::ThreadMetadataUpdate{
                        {requiredStrong<typed::ThreadId>(params, "threadId"),
                         optionalNullable<typed::ThreadMetadataGitInfoUpdateParams>(params, "gitInfo", metadataGitInfo)}};
                case MethodId::ThreadSetName:
                    return backend::ThreadSetName{{requiredStrong<typed::ThreadId>(params, "threadId"), requiredString(params, "name")}};
                case MethodId::ThreadRollback:
                    return backend::ThreadRollback{
                        {requiredStrong<typed::ThreadId>(params, "threadId"), integerValue<std::uint32_t>(required(params, "numTurns"))}};
                case MethodId::ThreadShellCommand:
                    return backend::ThreadShellCommand{
                        {requiredStrong<typed::ThreadId>(params, "threadId"), requiredString(params, "command")}};
                case MethodId::ThreadUnarchive:
                    return backend::ThreadUnarchive{{requiredStrong<typed::ThreadId>(params, "threadId")}};
                case MethodId::ThreadUnsubscribe:
                    return backend::ThreadUnsubscribe{{requiredStrong<typed::ThreadId>(params, "threadId")}};
                case MethodId::ThreadApproveGuardianDeniedAction:
                    return backend::ThreadApproveGuardianDeniedAction{
                        {requiredStrong<typed::ThreadId>(params, "threadId"), required(params, "event")}};
                case MethodId::TurnSteer:
                    return backend::TurnSteer{{requiredStrong<typed::ThreadId>(params, "threadId"),
                                               requiredStrong<typed::TurnId>(params, "expectedTurnId"),
                                               vectorValue<typed::TurnInput>(required(params, "input"), turnInput),
                                               optionalStrong<typed::ClientUserMessageId>(params, "clientUserMessageId")}};

                case MethodId::CommandExec: {
                    typed::CommandExecParams value;
                    value.command = stringVector(required(params, "command"));
                    value.cwd = optionalNullableString(params, "cwd");
                    value.disableOutputCap = optionalBool(params, "disableOutputCap");
                    value.disableTimeout = optionalBool(params, "disableTimeout");
                    value.env = optionalNullable<std::map<std::string, std::optional<std::string>>>(params, "env", nullableStringMap);
                    value.outputBytesCap = optionalNullableInteger<std::uint64_t>(params, "outputBytesCap");
                    value.processId = optionalStrong<typed::CommandExecProcessId>(params, "processId");
                    value.sandboxPolicy = optionalNullable<typed::SandboxPolicy>(params, "sandboxPolicy", sandboxPolicy);
                    value.size = optionalNullable<typed::CommandExecTerminalSize>(params, "size", terminalSize);
                    value.streamStdin = optionalBool(params, "streamStdin");
                    value.streamStdoutStderr = optionalBool(params, "streamStdoutStderr");
                    value.timeoutMs = optionalNullableInteger<std::int64_t>(params, "timeoutMs");
                    value.tty = optionalBool(params, "tty");
                    return backend::CommandExec{std::move(value)};
                }
                case MethodId::CommandExecResize:
                    return backend::CommandExecResize{
                        {requiredStrong<typed::CommandExecProcessId>(params, "processId"), terminalSize(required(params, "size"))}};
                case MethodId::CommandExecTerminate:
                    return backend::CommandExecTerminate{{requiredStrong<typed::CommandExecProcessId>(params, "processId")}};
                case MethodId::CommandExecWrite:
                    return backend::CommandExecWrite{{requiredStrong<typed::CommandExecProcessId>(params, "processId"),
                                                      optionalNullableString(params, "deltaBase64"),
                                                      optionalBool(params, "closeStdin")}};

                case MethodId::FsCopy:
                    return backend::FsCopy{{requiredStrong<typed::AbsolutePath>(params, "destinationPath"),
                                            optionalBool(params, "recursive"),
                                            requiredStrong<typed::AbsolutePath>(params, "sourcePath")}};
                case MethodId::FsCreateDirectory:
                    return backend::FsCreateDirectory{
                        {requiredStrong<typed::AbsolutePath>(params, "path"), optionalNullableBool(params, "recursive")}};
                case MethodId::FsGetMetadata:
                    return backend::FsGetMetadata{{requiredStrong<typed::AbsolutePath>(params, "path")}};
                case MethodId::FsReadDirectory:
                    return backend::FsReadDirectory{{requiredStrong<typed::AbsolutePath>(params, "path")}};
                case MethodId::FsReadFile:
                    return backend::FsReadFile{{requiredStrong<typed::AbsolutePath>(params, "path")}};
                case MethodId::FsRemove:
                    return backend::FsRemove{{optionalNullableBool(params, "force"),
                                              requiredStrong<typed::AbsolutePath>(params, "path"),
                                              optionalNullableBool(params, "recursive")}};
                case MethodId::FsUnwatch:
                    return backend::FsUnwatch{{requiredStrong<typed::FsWatchId>(params, "watchId")}};
                case MethodId::FsWatch:
                    return backend::FsWatch{
                        {requiredStrong<typed::AbsolutePath>(params, "path"), requiredStrong<typed::FsWatchId>(params, "watchId")}};
                case MethodId::FsWriteFile:
                    return backend::FsWriteFile{
                        {requiredString(params, "dataBase64"), requiredStrong<typed::AbsolutePath>(params, "path")}};
                case MethodId::FuzzyFileSearch:
                    return backend::FuzzyFileSearch{{optionalNullableString(params, "cancellationToken"),
                                                     requiredString(params, "query"),
                                                     stringVector(required(params, "roots"))}};

                case MethodId::PermissionProfileList:
                    return backend::PermissionProfileList{{optionalNullableString(params, "cursor"),
                                                           optionalNullableString(params, "cwd"),
                                                           optionalNullableInteger<std::uint32_t>(params, "limit")}};
                case MethodId::ReviewStart:
                    return backend::ReviewStart{{requiredStrong<typed::ThreadId>(params, "threadId"),
                                                 reviewTarget(required(params, "target")),
                                                 optionalStrong<typed::ReviewDelivery>(params, "delivery")}};

                case MethodId::AppsList: {
                    typed::AppsListParams value;
                    value.cursor = optionalNullableString(params, "cursor");
                    value.forceRefetch = optionalBool(params, "forceRefetch");
                    value.limit = optionalNullableInteger<std::uint32_t>(params, "limit");
                    value.threadId = optionalStrong<typed::ThreadId>(params, "threadId");
                    value.raw = params;
                    return backend::AppsList{std::move(value)};
                }
                case MethodId::ExternalAgentConfigDetect: {
                    typed::ExternalAgentConfigDetectParams value;
                    value.cwds = optionalNullable<std::vector<std::string>>(params, "cwds", stringVector);
                    value.includeHome = optionalBool(params, "includeHome");
                    value.raw = params;
                    return backend::ExternalAgentConfigDetect{std::move(value)};
                }
                case MethodId::ExternalAgentConfigImport: {
                    typed::ExternalAgentConfigImportParams value;
                    value.raw = params;
                    value.source = optionalNullableString(params, "source");
                    for (const Json& item : required(params, "migrationItems")) {
                        std::string error;
                        auto decoded = ai::openai::codex::detail::decodeExternalAgentConfigMigrationItem(item, error);
                        if (!decoded.has_value()) {
                            fail("migrationItems");
                        }
                        value.migrationItems.push_back(std::move(*decoded));
                    }
                    return backend::ExternalAgentConfigImport{std::move(value)};
                }
                case MethodId::ExternalAgentConfigImportHistoriesRead:
                    return backend::ExternalAgentConfigImportHistoriesRead{};
                case MethodId::FeedbackUpload: {
                    typed::FeedbackUploadParams value;
                    value.classification = requiredString(params, "classification");
                    value.extraLogFiles = optionalNullable<std::vector<std::string>>(params, "extraLogFiles", stringVector);
                    value.includeLogs = optionalBool(params, "includeLogs");
                    value.reason = optionalNullableString(params, "reason");
                    value.tags = optionalNullable<std::map<std::string, std::string>>(params, "tags", stringMap);
                    value.threadId = optionalStrong<typed::ThreadId>(params, "threadId");
                    value.raw = params;
                    return backend::FeedbackUpload{std::move(value)};
                }
                case MethodId::HooksList: {
                    typed::HooksListParams value;
                    value.cwds = optional<std::vector<std::string>>(params, "cwds", stringVector);
                    value.raw = params;
                    return backend::HooksList{std::move(value)};
                }

                case MethodId::MarketplaceAdd: {
                    typed::MarketplaceAddParams value;
                    value.refName = optionalNullableString(params, "refName");
                    value.source = requiredString(params, "source");
                    value.sparsePaths = optionalNullable<std::vector<std::string>>(params, "sparsePaths", stringVector);
                    value.raw = params;
                    return backend::MarketplaceAdd{std::move(value)};
                }
                case MethodId::MarketplaceRemove: {
                    typed::MarketplaceRemoveParams value;
                    value.marketplaceName = requiredString(params, "marketplaceName");
                    value.raw = params;
                    return backend::MarketplaceRemove{std::move(value)};
                }
                case MethodId::MarketplaceUpgrade: {
                    typed::MarketplaceUpgradeParams value;
                    value.marketplaceName = optionalNullableString(params, "marketplaceName");
                    value.raw = params;
                    return backend::MarketplaceUpgrade{std::move(value)};
                }

                case MethodId::PluginInstall: {
                    typed::PluginInstallParams value;
                    value.marketplacePath = optionalStrong<typed::AbsolutePath>(params, "marketplacePath");
                    value.pluginName = requiredString(params, "pluginName");
                    value.remoteMarketplaceName = optionalNullableString(params, "remoteMarketplaceName");
                    value.raw = params;
                    return backend::PluginInstall{std::move(value)};
                }
                case MethodId::PluginInstalled: {
                    typed::PluginInstalledParams value;
                    value.cwds = optionalNullable<std::vector<typed::AbsolutePath>>(params, "cwds", [](const Json& item) {
                        return vectorValue<typed::AbsolutePath>(item, strongString<typed::AbsolutePath>);
                    });
                    value.installSuggestionPluginNames =
                        optionalNullable<std::vector<std::string>>(params, "installSuggestionPluginNames", stringVector);
                    value.raw = params;
                    return backend::PluginInstalled{std::move(value)};
                }
                case MethodId::PluginList: {
                    typed::PluginListParams value;
                    value.cwds = optionalNullable<std::vector<typed::AbsolutePath>>(params, "cwds", [](const Json& item) {
                        return vectorValue<typed::AbsolutePath>(item, strongString<typed::AbsolutePath>);
                    });
                    value.marketplaceKinds =
                        optionalNullable<std::vector<typed::PluginListMarketplaceKind>>(params, "marketplaceKinds", [](const Json& item) {
                            return vectorValue<typed::PluginListMarketplaceKind>(item, strongString<typed::PluginListMarketplaceKind>);
                        });
                    value.raw = params;
                    return backend::PluginList{std::move(value)};
                }
                case MethodId::PluginRead: {
                    typed::PluginReadParams value;
                    value.marketplacePath = optionalStrong<typed::AbsolutePath>(params, "marketplacePath");
                    value.pluginName = requiredString(params, "pluginName");
                    value.remoteMarketplaceName = optionalNullableString(params, "remoteMarketplaceName");
                    value.raw = params;
                    return backend::PluginRead{std::move(value)};
                }
                case MethodId::PluginShareCheckout: {
                    typed::PluginShareCheckoutParams value{requiredString(params, "remotePluginId"), params, {}};
                    return backend::PluginShareCheckout{std::move(value)};
                }
                case MethodId::PluginShareDelete: {
                    typed::PluginShareDeleteParams value{requiredString(params, "remotePluginId"), params, {}};
                    return backend::PluginShareDelete{std::move(value)};
                }
                case MethodId::PluginShareList:
                    return backend::PluginShareList{};
                case MethodId::PluginShareSave: {
                    typed::PluginShareSaveParams value;
                    value.discoverability = optionalStrong<typed::PluginShareDiscoverability>(params, "discoverability");
                    value.pluginPath = requiredStrong<typed::AbsolutePath>(params, "pluginPath");
                    value.remotePluginId = optionalNullableString(params, "remotePluginId");
                    value.shareTargets =
                        optionalNullable<std::vector<typed::PluginShareTarget>>(params, "shareTargets", [](const Json& item) {
                            return vectorValue<typed::PluginShareTarget>(item, pluginShareTarget);
                        });
                    value.raw = params;
                    return backend::PluginShareSave{std::move(value)};
                }
                case MethodId::PluginShareUpdateTargets: {
                    typed::PluginShareUpdateTargetsParams value;
                    value.discoverability = requiredStrong<typed::PluginShareUpdateDiscoverability>(params, "discoverability");
                    value.remotePluginId = requiredString(params, "remotePluginId");
                    value.shareTargets = vectorValue<typed::PluginShareTarget>(required(params, "shareTargets"), pluginShareTarget);
                    value.raw = params;
                    return backend::PluginShareUpdateTargets{std::move(value)};
                }
                case MethodId::PluginSkillRead: {
                    typed::PluginSkillReadParams value{requiredString(params, "remoteMarketplaceName"),
                                                       requiredString(params, "remotePluginId"),
                                                       requiredString(params, "skillName"),
                                                       params,
                                                       {}};
                    return backend::PluginSkillRead{std::move(value)};
                }
                case MethodId::PluginUninstall: {
                    typed::PluginUninstallParams value{requiredString(params, "pluginId"), params, {}};
                    return backend::PluginUninstall{std::move(value)};
                }

                case MethodId::SkillsConfigWrite: {
                    typed::SkillsConfigWriteParams value;
                    value.enabled = requiredBool(params, "enabled");
                    value.name = optionalNullableString(params, "name");
                    value.path = optionalStrong<typed::AbsolutePath>(params, "path");
                    value.raw = params;
                    return backend::SkillsConfigWrite{std::move(value)};
                }
                case MethodId::SkillsExtraRootsSet: {
                    typed::SkillsExtraRootsSetParams value;
                    value.extraRoots = vectorValue<typed::AbsolutePath>(required(params, "extraRoots"), strongString<typed::AbsolutePath>);
                    value.raw = params;
                    return backend::SkillsExtraRootsSet{std::move(value)};
                }
                case MethodId::SkillsList: {
                    typed::SkillsListParams value;
                    value.cwds = optional<std::vector<std::string>>(params, "cwds", stringVector);
                    value.forceReload = optionalBool(params, "forceReload");
                    value.raw = params;
                    return backend::SkillsList{std::move(value)};
                }

                case MethodId::McpServerOauthLogin: {
                    typed::McpServerOauthLoginParams value;
                    value.name = requiredString(params, "name");
                    value.scopes = optionalNullable<std::vector<std::string>>(params, "scopes", stringVector);
                    value.threadId = optionalStrong<typed::ThreadId>(params, "threadId");
                    value.timeoutSecs = optionalNullableInteger<std::int64_t>(params, "timeoutSecs");
                    value.raw = params;
                    return backend::McpServerOauthLogin{std::move(value)};
                }
                case MethodId::McpResourceRead: {
                    typed::McpResourceReadParams value;
                    value.server = requiredString(params, "server");
                    value.threadId = optionalStrong<typed::ThreadId>(params, "threadId");
                    value.uri = requiredString(params, "uri");
                    value.raw = params;
                    return backend::McpResourceRead{std::move(value)};
                }
                case MethodId::McpServerToolCall: {
                    typed::McpServerToolCallParams value;
                    value.meta = optionalNullable<Json>(params, "_meta", [](const Json& item) {
                        return item;
                    });
                    value.arguments = optionalNullable<Json>(params, "arguments", [](const Json& item) {
                        return item;
                    });
                    value.server = requiredString(params, "server");
                    value.threadId = requiredStrong<typed::ThreadId>(params, "threadId");
                    value.tool = requiredString(params, "tool");
                    value.raw = params;
                    return backend::McpServerToolCall{std::move(value)};
                }
                case MethodId::McpServerStatusList: {
                    typed::ListMcpServerStatusParams value;
                    value.cursor = optionalNullableString(params, "cursor");
                    value.detail = optionalStrong<typed::McpServerStatusDetail>(params, "detail");
                    value.limit = optionalNullableInteger<std::uint32_t>(params, "limit");
                    value.threadId = optionalStrong<typed::ThreadId>(params, "threadId");
                    value.raw = params;
                    return backend::McpServerStatusList{std::move(value)};
                }

                case MethodId::WindowsSandboxReadiness:
                    return backend::WindowsSandboxReadiness{};
                case MethodId::WindowsSandboxSetupStart: {
                    typed::WindowsSandboxSetupStartParams value;
                    value.cwd = optionalStrong<typed::AbsolutePath>(params, "cwd");
                    value.mode = requiredStrong<typed::WindowsSandboxSetupMode>(params, "mode");
                    value.raw = params;
                    return backend::WindowsSandboxSetupStart{std::move(value)};
                }

                default:
                    fail();
            }
        }

        backend::BackendCommand mapReverseCommand(MethodId method, const Json& params) {
            const backend::PendingRequestId requestId = pendingRequestId(params);
            switch (method) {
                case MethodId::ApprovalRespond:
                    return backend::ApprovalRespond{requestId, typed::ApprovalDecision{requiredString(params, "decision")}};
                case MethodId::UserInputRespond: {
                    std::vector<typed::UserInputAnswer> answers;
                    for (const Json& item : required(params, "answers")) {
                        answers.push_back({requiredString(item, "questionId"), stringVector(required(item, "answers"))});
                    }
                    return backend::UserInputRespond{requestId, std::move(answers)};
                }
                case MethodId::AuthenticationRespond:
                    return backend::AuthenticationRespond{
                        requestId,
                        typed::AuthenticationResponse{requiredString(params, "accessToken"),
                                                      requiredString(params, "chatgptAccountId"),
                                                      optional<std::string>(params, "chatgptPlanType", stringValue)}};
                case MethodId::UnknownRequestRespond:
                    return backend::UnknownRequestRespondRaw{requestId, required(params, "result")};
                case MethodId::UnknownRequestReject:
                    return backend::UnknownRequestReject{requestId,
                                                         ProtocolError{integerValue<std::int64_t>(required(params, "code")),
                                                                       requiredString(params, "message"),
                                                                       optional<Json>(params, "data", [](const Json& item) {
                                                                           return item;
                                                                       })}};
                case MethodId::ApplyPatchApprovalRespond:
                    return backend::ApplyPatchApprovalRespond{
                        requestId, typed::ApplyPatchApprovalResponse{reviewDecision(required(required(params, "response"), "decision"))}};
                case MethodId::ExecCommandApprovalRespond:
                    return backend::ExecCommandApprovalRespond{
                        requestId, typed::ExecCommandApprovalResponse{reviewDecision(required(required(params, "response"), "decision"))}};
                case MethodId::PermissionsApprovalRespond: {
                    const Json& response = required(params, "response");
                    typed::PermissionsRequestApprovalResponse value;
                    value.permissions = grantedPermissionProfile(required(response, "permissions"));
                    value.scope = optional<typed::PermissionGrantScope>(response, "scope", strongString<typed::PermissionGrantScope>);
                    value.strictAutoReview = optionalNullableBool(response, "strictAutoReview");
                    return backend::PermissionsApprovalRespond{requestId, std::move(value)};
                }
                case MethodId::AttestationRespond: {
                    const Json& response = required(params, "response");
                    return backend::AttestationGenerateRespond{
                        requestId, typed::AttestationGenerateResponse{requiredString(response, "token"), response}};
                }
                case MethodId::DynamicToolRespond: {
                    const Json& response = required(params, "response");
                    typed::DynamicToolCallResponse value;
                    value.contentItems =
                        vectorValue<typed::DynamicToolCallOutputContentItem>(required(response, "contentItems"), dynamicToolOutput);
                    value.success = requiredBool(response, "success");
                    value.raw = response;
                    return backend::DynamicToolCallRespond{requestId, std::move(value)};
                }
                case MethodId::McpElicitationRespond: {
                    const Json& response = required(params, "response");
                    typed::McpServerElicitationRequestResponse value;
                    value.action = requiredStrong<typed::McpServerElicitationAction>(response, "action");
                    value.content = optionalNullable<Json>(response, "content", [](const Json& item) {
                        return item;
                    });
                    value.meta = optionalNullable<Json>(response, "_meta", [](const Json& item) {
                        return item;
                    });
                    value.raw = response;
                    return backend::McpServerElicitationRespond{requestId, std::move(value)};
                }
                case MethodId::KnownRequestReject: {
                    const Json& error = required(params, "error");
                    return backend::KnownRequestReject{requestId,
                                                       ProtocolError{integerValue<std::int64_t>(required(error, "code")),
                                                                     requiredString(error, "message"),
                                                                     optional<Json>(error, "data", [](const Json& item) {
                                                                         return item;
                                                                     })}};
                }
                default:
                    fail();
            }
        }

    } // namespace

    DefinedCommandMapping mapDefinedCommand(const generated::DefinedCommand& command) noexcept {
        try {
            const MethodId method = generated::commandMethod(command.parameters);
            switch (method) {
                case MethodId::ControllerAcquire:
                    return NativeCommandMapping{NativeServiceAction::ControllerAcquire, std::nullopt};
                case MethodId::ControllerRelease:
                    return NativeCommandMapping{NativeServiceAction::ControllerRelease, std::nullopt};
                case MethodId::SnapshotGet:
                    return NativeCommandMapping{NativeServiceAction::SnapshotGet, std::nullopt};
                case MethodId::EventsReplay:
                    return NativeCommandMapping{NativeServiceAction::EventsReplay,
                                                integerValue<std::uint64_t>(required(parameterValue(command), "after"))};
                case MethodId::ProviderStart:
                    return NativeCommandMapping{NativeServiceAction::ProviderStart, std::nullopt};
                case MethodId::ProviderStop:
                    return NativeCommandMapping{NativeServiceAction::ProviderStop, std::nullopt};
                case MethodId::ProviderRestart:
                    return NativeCommandMapping{NativeServiceAction::ProviderRestart, std::nullopt};
                default:
                    break;
            }

            const Json params = mergeLegacyAliases(command, method);
            const generated::MethodCategory category = generated::AllMethods[static_cast<std::size_t>(method)].category;
            if (category == generated::MethodCategory::ProviderOperation) {
                return mapProviderCommand(method, params);
            }
            if (category == generated::MethodCategory::ReverseResponse) {
                return mapReverseCommand(method, params);
            }
            fail();
        } catch (const MappingFailure& error) {
            return BackendCommandMappingError{error.what()};
        } catch (...) {
            return BackendCommandMappingError{"validated frontend parameters could not be converted to the typed backend command"};
        }
    }

    std::string_view backendCommandTypeName(const backend::BackendCommand& command) noexcept {
        return std::visit(
            []<typename Command>(const Command&) -> std::string_view {
                using T = std::remove_cvref_t<Command>;
                if constexpr (std::is_same_v<T, backend::ControllerAcquire>) {
                    return "ControllerAcquire";
                } else if constexpr (std::is_same_v<T, backend::ControllerRelease>) {
                    return "ControllerRelease";
                } else if constexpr (std::is_same_v<T, backend::SnapshotGet>) {
                    return "SnapshotGet";
                }
#define CODEX_BACKEND_PROVIDER_OPERATION(COMMAND, ...)                                                                                     \
    else if constexpr (std::is_same_v<T, backend::COMMAND>) {                                                                              \
        return #COMMAND;                                                                                                                   \
    }
#define CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(COMMAND, ...) CODEX_BACKEND_PROVIDER_OPERATION(COMMAND, __VA_ARGS__)
#include "ai/openai/codex/backend/internal/ProviderOperations.inc"
#undef CODEX_BACKEND_PROVIDER_OPERATION_EMPTY
#undef CODEX_BACKEND_PROVIDER_OPERATION
                else if constexpr (std::is_same_v<T, backend::ApprovalRespond>) {
                    return "ApprovalRespond";
                } else if constexpr (std::is_same_v<T, backend::UserInputRespond>) {
                    return "UserInputRespond";
                } else if constexpr (std::is_same_v<T, backend::AuthenticationRespond>) {
                    return "AuthenticationRespond";
                } else if constexpr (std::is_same_v<T, backend::UnknownRequestRespondRaw>) {
                    return "UnknownRequestRespondRaw";
                } else if constexpr (std::is_same_v<T, backend::UnknownRequestReject>) {
                    return "UnknownRequestReject";
                } else if constexpr (std::is_same_v<T, backend::ApplyPatchApprovalRespond>) {
                    return "ApplyPatchApprovalRespond";
                } else if constexpr (std::is_same_v<T, backend::ExecCommandApprovalRespond>) {
                    return "ExecCommandApprovalRespond";
                } else if constexpr (std::is_same_v<T, backend::PermissionsApprovalRespond>) {
                    return "PermissionsApprovalRespond";
                } else if constexpr (std::is_same_v<T, backend::AttestationGenerateRespond>) {
                    return "AttestationGenerateRespond";
                } else if constexpr (std::is_same_v<T, backend::DynamicToolCallRespond>) {
                    return "DynamicToolCallRespond";
                } else if constexpr (std::is_same_v<T, backend::McpServerElicitationRespond>) {
                    return "McpServerElicitationRespond";
                } else if constexpr (std::is_same_v<T, backend::KnownRequestReject>) {
                    return "KnownRequestReject";
                }
                return {};
            },
            command);
    }

} // namespace ai::openai::codex::frontend::detail
