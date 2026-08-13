/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/State.h"

#include <algorithm>
#include <array>
#include <limits>

namespace ai::openai::codex::frontend::client {

    namespace {
        std::optional<std::string> text(const frontend::Json& value, std::string_view key) {
            if (!value.is_object()) {
                return std::nullopt;
            }
            const auto member = value.find(std::string(key));
            return member != value.end() && member->is_string() ? std::optional(member->get<std::string>()) : std::nullopt;
        }

        std::optional<bool> boolean(const frontend::Json& value, std::string_view key) {
            if (!value.is_object()) {
                return std::nullopt;
            }
            const auto member = value.find(std::string(key));
            return member != value.end() && member->is_boolean() ? std::optional(member->get<bool>()) : std::nullopt;
        }

        std::optional<std::int64_t> integer(const frontend::Json& value, std::string_view key) {
            if (!value.is_object()) {
                return std::nullopt;
            }
            const auto member = value.find(std::string(key));
            if (member == value.end()) {
                return std::nullopt;
            }
            if (member->is_number_integer()) {
                return member->get<std::int64_t>();
            }
            if (member->is_number_unsigned()) {
                const std::uint64_t decoded = member->get<std::uint64_t>();
                if (decoded <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    return static_cast<std::int64_t>(decoded);
                }
            }
            return std::nullopt;
        }

        std::optional<std::uint64_t> unsignedInteger(const frontend::Json& value, std::string_view key) {
            const std::optional<std::int64_t> signedValue = integer(value, key);
            return signedValue && *signedValue >= 0 ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(*signedValue)} : std::nullopt;
        }

        std::optional<std::size_t> sizeValue(const frontend::Json& value, std::string_view key) {
            const std::optional<std::uint64_t> decoded = unsignedInteger(value, key);
            return decoded && *decoded <= std::numeric_limits<std::size_t>::max()
                       ? std::optional<std::size_t>{static_cast<std::size_t>(*decoded)}
                       : std::nullopt;
        }

        std::optional<SourceStamp> sourceStamp(const frontend::Json& value) {
            const std::optional<std::uint64_t> generation = unsignedInteger(value, "generation");
            const std::optional<std::string> freshness = text(value, "freshness");
            if (!generation || !freshness) {
                return std::nullopt;
            }
            const std::optional<frontend::StateFreshness> decodedFreshness = frontend::stateFreshnessFromString(*freshness);
            if (!decodedFreshness) {
                return std::nullopt;
            }
            SourceStamp result{*generation, *decodedFreshness};
            for (auto member = value.begin(); member != value.end(); ++member) {
                if (member.key() != "generation" && member.key() != "freshness") {
                    result.extensions[member.key()] = member.value();
                }
            }
            return result;
        }

        bool containsRedaction(const frontend::Json& value) {
            if (value.is_string()) {
                return value.get_ref<const std::string&>() == "[redacted]";
            }
            if (value.is_array()) {
                return std::ranges::any_of(value, containsRedaction);
            }
            if (value.is_object()) {
                return std::ranges::any_of(value, [](const frontend::Json& member) {
                    return containsRedaction(member);
                });
            }
            return false;
        }

        TokenCountsView tokenCounts(const frontend::Json& value) {
            return {integer(value, "cachedInputTokens"),
                    integer(value, "inputTokens"),
                    integer(value, "outputTokens"),
                    integer(value, "reasoningOutputTokens"),
                    integer(value, "totalTokens")};
        }

        bool knownCodexError(std::string_view value) {
            static constexpr std::array<std::string_view, 16> Known{
                "contextWindowExceeded",
                "sessionBudgetExceeded",
                "usageLimitExceeded",
                "serverOverloaded",
                "cyberPolicy",
                "internalServerError",
                "unauthorized",
                "badRequest",
                "threadRollbackFailed",
                "sandboxError",
                "other",
                "httpConnectionFailed",
                "responseStreamConnectionFailed",
                "responseStreamDisconnected",
                "responseTooManyFailedAttempts",
                "activeTurnNotSteerable",
            };
            return std::ranges::find(Known, value) != Known.end();
        }

        const frontend::Json* semanticObject(const State& state, std::string_view key) {
            const frontend::Json& compatibility = state.compatibilityExtensions();
            if (const auto direct = compatibility.find(std::string(key)); direct != compatibility.end() && direct->is_object()) {
                return &*direct;
            }
            if (const auto nested = compatibility.find("state"); nested != compatibility.end() && nested->is_object()) {
                const auto projection = nested->find(std::string(key));
                if (projection != nested->end() && projection->is_object()) {
                    return &*projection;
                }
            }
            return nullptr;
        }

        const frontend::Json* arrayElement(const frontend::Json& value, std::string_view key, std::size_t index) {
            const auto array = value.find(std::string(key));
            return array != value.end() && array->is_array() && index < array->size() ? &array->at(index) : nullptr;
        }

        std::optional<std::string> arrayText(const frontend::Json& value, std::string_view key, std::size_t index) {
            const frontend::Json* member = arrayElement(value, key, index);
            return member != nullptr && member->is_string() ? std::optional(member->get<std::string>()) : std::nullopt;
        }

        std::optional<std::size_t> arraySize(const frontend::Json& value, std::string_view key, std::size_t index) {
            const frontend::Json* member = arrayElement(value, key, index);
            if (member == nullptr) {
                return std::nullopt;
            }
            const frontend::Json wrapper{{"value", *member}};
            return sizeValue(wrapper, "value");
        }

        std::optional<std::uint64_t> arrayUnsigned(const frontend::Json& value, std::string_view key, std::size_t index) {
            const frontend::Json* member = arrayElement(value, key, index);
            if (member == nullptr) {
                return std::nullopt;
            }
            const frontend::Json wrapper{{"value", *member}};
            return unsignedInteger(wrapper, "value");
        }

        std::optional<bool> arrayBool(const frontend::Json& value, std::string_view key, std::size_t index) {
            const frontend::Json* member = arrayElement(value, key, index);
            return member != nullptr && member->is_boolean() ? std::optional(member->get<bool>()) : std::nullopt;
        }

        std::optional<SourceStamp>
        parallelStamp(const frontend::Json& value, std::string_view generationKey, std::string_view freshnessKey, std::size_t index) {
            const std::optional<std::uint64_t> generation = arrayUnsigned(value, generationKey, index);
            const std::optional<std::string> freshness = arrayText(value, freshnessKey, index);
            const std::optional<frontend::StateFreshness> decoded =
                freshness ? frontend::stateFreshnessFromString(*freshness) : std::nullopt;
            return generation && decoded ? std::optional<SourceStamp>{SourceStamp{*generation, *decoded}} : std::nullopt;
        }

        std::optional<ProviderDomainSemanticState> domainState(const frontend::Json& value) {
            if (!value.is_object()) {
                return std::nullopt;
            }
            ProviderDomainSemanticState result;
            result.truncated = boolean(value, "truncated").value_or(false);
            result.omittedResults = sizeValue(value, "omittedResults").value_or(0);
            result.omittedNotifications = sizeValue(value, "omittedNotifications").value_or(0);
            const auto resultMethods = value.find("resultMethods");
            const std::size_t resultCount = resultMethods != value.end() && resultMethods->is_array() ? resultMethods->size() : 0;
            for (std::size_t index = 0; index < resultCount; ++index) {
                const auto method = arrayText(value, "resultMethods", index);
                const auto alternative = arraySize(value, "resultAlternatives", index);
                const auto status = arrayText(value, "resultStatuses", index);
                const auto stamp = parallelStamp(value, "resultGenerations", "resultFreshness", index);
                if (!method || !alternative || !status || !stamp) {
                    result.truncated = true;
                    continue;
                }
                const std::optional<std::string> subject = arrayBool(value, "resultSubjectIdPresent", index).value_or(false)
                                                               ? arrayText(value, "resultSubjectIds", index)
                                                               : std::nullopt;
                const std::optional<std::string> cursor = arrayBool(value, "resultNextCursorPresent", index).value_or(false)
                                                              ? arrayText(value, "resultNextCursors", index)
                                                              : std::nullopt;
                result.latestResults.push_back({*method,
                                                *alternative,
                                                *status,
                                                subject,
                                                cursor,
                                                arraySize(value, "resultItemCounts", index).value_or(0),
                                                arrayBool(value, "resultComplete", index).value_or(true),
                                                *stamp});
            }
            const auto notificationMethods = value.find("notificationMethods");
            const std::size_t notificationCount =
                notificationMethods != value.end() && notificationMethods->is_array() ? notificationMethods->size() : 0;
            for (std::size_t index = 0; index < notificationCount; ++index) {
                const auto method = arrayText(value, "notificationMethods", index);
                const auto alternative = arraySize(value, "notificationAlternatives", index);
                const auto stamp = parallelStamp(value, "notificationGenerations", "notificationFreshness", index);
                if (!method || !alternative || !stamp) {
                    result.truncated = true;
                    continue;
                }
                result.latestNotifications.push_back({*method, *alternative, *stamp});
            }
            return result;
        }

        std::optional<GoalMutationSemanticState> goalMutation(const frontend::Json& value, std::string_view prefix) {
            const std::string name{prefix};
            const auto operation = text(value, name + "Operation");
            const auto threadId = text(value, name + "ThreadId");
            const auto generation = unsignedInteger(value, name + "Generation");
            const auto freshness = text(value, name + "Freshness");
            const auto decodedFreshness = freshness ? frontend::stateFreshnessFromString(*freshness) : std::nullopt;
            const std::optional<SourceStamp> stamp =
                generation && decodedFreshness ? std::optional<SourceStamp>{SourceStamp{*generation, *decodedFreshness}} : std::nullopt;
            if (!operation || !threadId || !stamp) {
                return std::nullopt;
            }
            return GoalMutationSemanticState{*operation,
                                             typed::ThreadId{*threadId},
                                             text(value, name + "Objective"),
                                             text(value, name + "Status"),
                                             boolean(value, name + "Cleared"),
                                             *stamp};
        }
    } // namespace

    std::optional<TurnTokenUsageView> tokenUsageView(const TurnState& turn) {
        const bool hasSidecar = turn.extensions.contains("tokenUsageLast") || turn.extensions.contains("tokenUsageTotal") ||
                                turn.extensions.contains("tokenUsageModelContextWindowPresent");
        if ((!turn.tokenUsage || !turn.tokenUsage->is_object()) && !hasSidecar) {
            return std::nullopt;
        }
        const frontend::Json value = turn.tokenUsage && turn.tokenUsage->is_object() ? *turn.tokenUsage : frontend::Json::object();
        TurnTokenUsageView result;
        if (const auto last = value.find("last"); last != value.end() && last->is_object()) {
            result.last = tokenCounts(*last);
        }
        if (const auto total = value.find("total"); total != value.end() && total->is_object()) {
            result.total = tokenCounts(*total);
        }
        if (const auto context = value.find("modelContextWindow"); context != value.end()) {
            result.modelContextWindowPresent = true;
            result.modelContextWindow = integer(value, "modelContextWindow");
            result.truncated = !context->is_null() && !result.modelContextWindow.has_value();
        }
        if (const auto last = turn.extensions.find("tokenUsageLast"); last != turn.extensions.end() && last->is_object()) {
            result.last = tokenCounts(*last);
        }
        if (const auto total = turn.extensions.find("tokenUsageTotal"); total != turn.extensions.end() && total->is_object()) {
            result.total = tokenCounts(*total);
        }
        if (boolean(turn.extensions, "tokenUsageModelContextWindowPresent").value_or(false)) {
            result.modelContextWindowPresent = true;
            result.modelContextWindow = integer(turn.extensions, "tokenUsageModelContextWindow");
        }
        result.truncated = result.truncated || boolean(value, "projectionOmitted").value_or(false) ||
                           boolean(turn.extensions, "tokenUsageProjectionOmitted").value_or(false);
        if (result.truncated) {
            result.omittedFields.push_back("/tokenUsage");
        }
        for (auto member = value.begin(); member != value.end(); ++member) {
            if (member.key() != "last" && member.key() != "total" && member.key() != "modelContextWindow" &&
                member.key() != "projectionOmitted") {
                result.extensions[member.key()] = member.value();
            }
        }
        return result;
    }

    std::optional<TurnFailureView> failureView(const TurnState& turn) {
        const bool hasSidecar = turn.extensions.contains("failureMessage") || turn.extensions.contains("failureCodexErrorInfoPresent") ||
                                turn.extensions.contains("failureDecodingOmitted");
        if ((!turn.failure || !turn.failure->is_object()) && !hasSidecar) {
            return std::nullopt;
        }
        const frontend::Json value = turn.failure && turn.failure->is_object() ? *turn.failure : frontend::Json::object();
        TurnFailureView result;
        result.message = text(value, "message");
        if (const auto details = value.find("additionalDetails"); details != value.end()) {
            result.additionalDetailsPresent = true;
            result.additionalDetailsNull = details->is_null();
            if (details->is_string()) {
                result.additionalDetails = details->get<std::string>();
            } else if (!details->is_null()) {
                result.decodingOmitted = true;
            }
        }
        if (const auto info = value.find("codexErrorInfo"); info != value.end()) {
            result.codexErrorInfoPresent = true;
            result.codexErrorInfoNull = info->is_null();
            const frontend::Json* nested = nullptr;
            std::optional<std::string> discriminator;
            if (info->is_string()) {
                discriminator = info->get<std::string>();
            } else if (info->is_object() && info->size() == 1) {
                discriminator = info->begin().key();
                nested = &info->begin().value();
            } else if (!info->is_null()) {
                result.decodingOmitted = true;
            }
            if (discriminator) {
                if (knownCodexError(*discriminator)) {
                    result.codexErrorCategory = *discriminator;
                } else {
                    result.unknownErrorDiscriminator = *discriminator;
                }
            }
            if (nested && nested->is_object()) {
                if (const auto status = nested->find("httpStatusCode"); status != nested->end()) {
                    result.httpStatusCodePresent = true;
                    result.httpStatusCode = integer(*nested, "httpStatusCode");
                    result.decodingOmitted = result.decodingOmitted || (!status->is_null() && !result.httpStatusCode.has_value());
                }
                result.nonSteerableTurnKind = text(*nested, "turnKind");
            }
        }
        result.decodingOmitted = result.decodingOmitted || boolean(value, "projectionOmitted").value_or(false);
        result.redacted = boolean(value, "redacted").value_or(false) || containsRedaction(value);
        if (const auto sidecarMessage = text(turn.extensions, "failureMessage")) {
            result.message = sidecarMessage;
        }
        if (boolean(turn.extensions, "failureAdditionalDetailsPresent").value_or(false)) {
            result.additionalDetailsPresent = true;
            const auto additional = turn.extensions.find("failureAdditionalDetails");
            result.additionalDetailsNull = additional != turn.extensions.end() && additional->is_null();
            result.additionalDetails = text(turn.extensions, "failureAdditionalDetails");
        }
        if (boolean(turn.extensions, "failureCodexErrorInfoPresent").value_or(false)) {
            result.codexErrorInfoPresent = true;
            result.codexErrorInfoNull = boolean(turn.extensions, "failureCodexErrorInfoNull").value_or(false);
            if (const auto discriminator = text(turn.extensions, "failureCodexErrorDiscriminator")) {
                if (knownCodexError(*discriminator)) {
                    result.codexErrorCategory = discriminator;
                    result.unknownErrorDiscriminator.reset();
                } else {
                    result.unknownErrorDiscriminator = discriminator;
                    result.codexErrorCategory.reset();
                }
            }
        }
        if (boolean(turn.extensions, "failureHttpStatusCodePresent").value_or(false)) {
            result.httpStatusCodePresent = true;
            result.httpStatusCode = integer(turn.extensions, "failureHttpStatusCode");
        }
        if (const auto kind = text(turn.extensions, "failureNonSteerableTurnKind")) {
            result.nonSteerableTurnKind = kind;
        }
        result.decodingOmitted = result.decodingOmitted || boolean(turn.extensions, "failureDecodingOmitted").value_or(false);
        result.redacted = result.redacted || boolean(turn.extensions, "failureRedacted").value_or(false);
        for (auto member = value.begin(); member != value.end(); ++member) {
            if (member.key() != "message" && member.key() != "additionalDetails" && member.key() != "codexErrorInfo" &&
                member.key() != "projectionOmitted" && member.key() != "redacted") {
                result.extensions[member.key()] = member.value();
            }
        }
        return result;
    }

    ThreadRealtimeSemanticView realtimeSemanticView(const ThreadRealtimeState& realtime) {
        ThreadRealtimeSemanticView result;
        result.lifecycle = realtime.lifecycle;
        result.transcript = realtime.transcript;
        result.lastError = text(realtime.extensions, "lastError");
        result.sessionId = realtime.sessionId;
        result.version = realtime.version;
        result.lastSdpBytes = realtime.lastSdpBytes;
        result.itemCount = realtime.itemCount;
        result.receivedAudioBytes = realtime.receivedAudioBytes;
        result.droppedAudioBytes = realtime.droppedAudioBytes;
        result.transcriptTruncated = realtime.transcriptTruncated;
        if (const auto stamp = realtime.extensions.find("stamp"); stamp != realtime.extensions.end()) {
            result.stamp = sourceStamp(*stamp);
        } else if (const auto generation = unsignedInteger(realtime.extensions, "sourceGeneration"); generation) {
            const auto freshness = text(realtime.extensions, "sourceFreshness");
            const auto decoded = freshness ? frontend::stateFreshnessFromString(*freshness) : std::nullopt;
            if (decoded) {
                result.stamp = SourceStamp{*generation, *decoded};
            }
        }
        result.errorRedacted =
            boolean(realtime.extensions, "errorRedacted").value_or(false) || (result.lastError && *result.lastError == "[redacted]");
        result.errorOmitted = realtime.errorDetailsOmitted.value_or(false);
        return result;
    }

    std::optional<ItemSemanticView> itemSemanticView(const ItemState& item) {
        if (!item.data || !item.data->is_object()) {
            return std::nullopt;
        }
        const frontend::Json& data = *item.data;
        std::optional<ItemSemanticDetails> details;
        if (item.kind.is(frontend::ThreadItemKind::AgentMessage)) {
            details = AgentMessageSemanticView{text(data, "phase")};
        } else if (item.kind.is(frontend::ThreadItemKind::CommandExecution)) {
            CommandExecutionSemanticView value;
            value.command = text(data, "command");
            if (const auto cwd = text(data, "cwd")) {
                value.cwd = typed::AbsolutePath{*cwd};
            }
            value.status = text(data, "status");
            value.processId = text(data, "processId");
            value.exitCode = integer(data, "exitCode");
            value.durationMs = unsignedInteger(data, "durationMs");
            details = std::move(value);
        } else if (item.kind.is(frontend::ThreadItemKind::FileChange)) {
            FileChangeSemanticView value;
            value.status = text(data, "status");
            value.changeCount = sizeValue(data, "changeCount");
            value.changesTruncated = boolean(data, "changesTruncated").value_or(false);
            if (const auto changes = data.find("changes"); changes != data.end() && changes->is_array()) {
                for (const frontend::Json& change : *changes) {
                    value.changes.push_back({sizeValue(change, "kindAlternative"),
                                             unsignedInteger(change, "pathBytes"),
                                             boolean(change, "pathRedacted").value_or(false),
                                             unsignedInteger(change, "diffBytes"),
                                             boolean(change, "diffOmitted").value_or(false)});
                }
            }
            details = std::move(value);
        } else if (item.kind.is(frontend::ThreadItemKind::McpToolCall) || item.kind.is(frontend::ThreadItemKind::DynamicToolCall)) {
            details = ToolCallSemanticView{
                text(data, "namespace"), text(data, "server"), text(data, "tool"), text(data, "status"), boolean(data, "hasResult")};
        } else if (item.kind.is(frontend::ThreadItemKind::WebSearch)) {
            details = WebSearchSemanticView{text(data, "query")};
        } else if (item.kind.is(frontend::ThreadItemKind::CollabAgentToolCall)) {
            CollabAgentToolCallSemanticView value;
            value.tool = text(data, "tool");
            value.status = text(data, "status");
            if (const auto sender = text(data, "senderThreadId")) {
                value.senderThreadId = typed::ThreadId{*sender};
            }
            value.receiverCount = sizeValue(data, "receiverCount");
            value.agentStateCount = sizeValue(data, "agentStateCount");
            value.hasPrompt = boolean(data, "hasPrompt");
            value.promptBytes = unsignedInteger(data, "promptBytes");
            details = std::move(value);
        } else if (item.kind.is(frontend::ThreadItemKind::Plan)) {
            details = PlanSemanticView{text(data, "text"), boolean(data, "textTruncated").value_or(false)};
        } else if (item.kind.is(frontend::ThreadItemKind::SubAgentActivity)) {
            SubAgentActivitySemanticView value;
            value.agentPath = text(data, "agentPath");
            if (const auto threadId = text(data, "agentThreadId")) {
                value.agentThreadId = typed::ThreadId{*threadId};
            }
            value.kind = text(data, "kind");
            details = std::move(value);
        }
        if (!details) {
            return std::nullopt;
        }
        return ItemSemanticView{item.id,
                                item.threadId,
                                item.turnId,
                                item.kind,
                                std::move(*details),
                                item.truncated,
                                item.omittedFields,
                                item.connectionInvalidated,
                                item.stamp};
    }

    PendingRequestPresentationView pendingRequestPresentation(const PendingRequestState& request) {
        PendingRequestPresentationView result;
        result.id = request.id;
        result.kind = request.kind;
        result.threadId = request.threadId;
        result.turnId = request.turnId;
        result.itemId = request.itemId;
        result.truncated = request.truncated;
        result.omittedFields = request.omittedFields;
        if (!request.opaqueDetails || !request.opaqueDetails->is_object()) {
            return result;
        }
        const frontend::Json& details = *request.opaqueDetails;
        result.commandBytes = unsignedInteger(details, "commandBytes");
        result.commandRedacted = boolean(details, "commandRedacted").value_or(false);
        result.cwdBytes = unsignedInteger(details, "cwdBytes");
        result.cwdRedacted = boolean(details, "cwdRedacted").value_or(false);
        result.reasonBytes = unsignedInteger(details, "reasonBytes");
        result.reasonRedacted = boolean(details, "reasonRedacted").value_or(false);
        result.grantRootBytes = unsignedInteger(details, "grantRootBytes");
        result.grantRootRedacted = boolean(details, "grantRootRedacted").value_or(false);
        result.authenticationReason = text(details, "reason");
        result.previousAccountId = text(details, "previousAccountId");
        if (const auto summary = details.find("summary"); summary != details.end() && summary->is_object()) {
            result.fileChangeCount = sizeValue(*summary, "fileChangeCount");
            result.commandArgumentCount = sizeValue(*summary, "commandArgumentCount");
            result.parsedCommandCount = sizeValue(*summary, "parsedCommandCount");
            result.hasReason = boolean(*summary, "hasReason");
            result.hasGrantRoot = boolean(*summary, "hasGrantRoot");
            result.hasApprovalId = boolean(*summary, "hasApprovalId");
            result.hasEnvironmentId = boolean(*summary, "hasEnvironmentId");
        }
        result.truncated = result.truncated || boolean(details, "paramsTruncated").value_or(false);
        return result;
    }

    std::optional<ProviderOperationCollectionSemanticState> State::providerOperations() const {
        const frontend::Json* operations = semanticObject(*this, "providerOperationsSemantic");
        if (operations == nullptr) {
            return std::nullopt;
        }
        ProviderOperationCollectionSemanticState result;
        result.truncated = boolean(*operations, "truncated").value_or(false);
        result.omittedEntries = sizeValue(*operations, "omittedEntries").value_or(0);
        const auto methods = operations->find("methods");
        const std::size_t count = methods != operations->end() && methods->is_array() ? methods->size() : 0;
        for (std::size_t index = 0; index < count; ++index) {
            const auto method = arrayText(*operations, "methods", index);
            const auto alternative = arraySize(*operations, "resultAlternatives", index);
            const auto stamp = parallelStamp(*operations, "generations", "freshness", index);
            if (method && alternative && stamp) {
                result.entries.push_back({*method, *alternative, *stamp});
            } else {
                result.truncated = true;
            }
        }
        return result;
    }

    std::optional<ConversationSemanticState> State::conversations() const {
        const frontend::Json* value = semanticObject(*this, "conversationSemantic");
        if (value == nullptr) {
            return std::nullopt;
        }
        const std::optional<ProviderDomainSemanticState> domain = domainState(*value);
        if (!domain) {
            return std::nullopt;
        }
        return ConversationSemanticState{*domain,
                                         goalMutation(*value, "latestGoal"),
                                         goalMutation(*value, "latestGoalClear"),
                                         goalMutation(*value, "latestGoalSet"),
                                         goalMutation(*value, "latestUnsubscribe")};
    }

    std::optional<ProviderDomainSemanticState> State::filesystemProvider() const {
        const frontend::Json* value = semanticObject(*this, "filesystemProviderSemantic");
        return value != nullptr ? domainState(*value) : std::nullopt;
    }

    std::optional<CapacityProvenanceState> State::capacityProvenance() const {
        const frontend::Json* value = semanticObject(*this, "capacityProvenance");
        if (value == nullptr) {
            return std::nullopt;
        }
        return CapacityProvenanceState{sizeValue(*value, "sourceSessionCount").value_or(0),
                                       sizeValue(*value, "sourcePendingRequestCount").value_or(0),
                                       sizeValue(*value, "omittedThreads").value_or(0),
                                       sizeValue(*value, "omittedTurns").value_or(0),
                                       sizeValue(*value, "omittedItems").value_or(0),
                                       boolean(*value, "truncated").value_or(false),
                                       boolean(*value, "mandatoryCoreExceedsLimit").value_or(false)};
    }

} // namespace ai::openai::codex::frontend::client
