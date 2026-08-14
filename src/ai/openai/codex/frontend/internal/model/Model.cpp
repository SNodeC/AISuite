/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/internal/model/Model.h"

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Protocol.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <string_view>
#include <type_traits>

namespace ai::openai::codex::frontend::internal::model {
    namespace {
        constexpr std::size_t LegacySnapshotMaximumObjectMembers = 4'096;
        constexpr std::size_t FrontendDetailMaximumStringBytes = 16U * 1024U;

        void setSafeDetailError(SafeDetailError* target, SafeDetailError value) noexcept {
            if (target != nullptr) {
                *target = value;
            }
        }

        std::string normalizedKey(std::string_view key) {
            std::string normalized;
            normalized.reserve(key.size());
            for (const unsigned char character : key) {
                if ((character >= static_cast<unsigned char>('a') && character <= static_cast<unsigned char>('z')) ||
                    (character >= static_cast<unsigned char>('0') && character <= static_cast<unsigned char>('9'))) {
                    normalized.push_back(static_cast<char>(character));
                } else if (character >= static_cast<unsigned char>('A') && character <= static_cast<unsigned char>('Z')) {
                    normalized.push_back(static_cast<char>(character - static_cast<unsigned char>('A') + static_cast<unsigned char>('a')));
                }
            }
            return normalized;
        }

        bool inspectSafeDetail(
            const Json& value, std::size_t depth, std::size_t& members, const SafeDetailLimits& limits, SafeDetailError& error) {
            if (depth > limits.maximumDepth) {
                error = SafeDetailError::DepthLimit;
                return false;
            }
            if (value.is_discarded() || value.is_binary()) {
                error = SafeDetailError::UnsupportedValue;
                return false;
            }
            if (value.is_object()) {
                if (value.size() > limits.maximumMembers - std::min(members, limits.maximumMembers)) {
                    error = SafeDetailError::MemberLimit;
                    return false;
                }
                members += value.size();
                for (auto member = value.begin(); member != value.end(); ++member) {
                    const bool safeSecretClassification = normalizedKey(member.key()) == "issecret" && member.value().is_boolean();
                    if (!safeSecretClassification && SafeDetail::isSecretKey(member.key())) {
                        error = SafeDetailError::SecretKey;
                        return false;
                    }
                    if (!inspectSafeDetail(member.value(), depth + 1, members, limits, error)) {
                        return false;
                    }
                }
            } else if (value.is_array()) {
                if (value.size() > limits.maximumMembers - std::min(members, limits.maximumMembers)) {
                    error = SafeDetailError::MemberLimit;
                    return false;
                }
                members += value.size();
                for (const Json& element : value) {
                    if (!inspectSafeDetail(element, depth + 1, members, limits, error)) {
                        return false;
                    }
                }
            }
            return true;
        }

        struct ModelFailure final : std::exception {
            explicit ModelFailure(ModelError value)
                : error(std::move(value)) {
            }

            ModelError error;
        };

        [[noreturn]] void fail(ModelErrorCode code, std::string path, std::string message) {
            throw ModelFailure(ModelError{code, std::move(path), std::move(message)});
        }

        SafeDetail safeDetail(Json value, const std::string& path) {
            SafeDetailError detailError = SafeDetailError::None;
            auto result = SafeDetail::fromJson(std::move(value), &detailError);
            if (!result.has_value()) {
                fail(ModelErrorCode::UnsafeDetail,
                     path,
                     "unsafe or over-capacity detail (code " + std::to_string(static_cast<unsigned int>(detailError)) + ")");
            }
            return std::move(*result);
        }

        Json objectDetail(const SafeDetail& detail, const std::string& path) {
            if (!detail.json().is_object()) {
                fail(ModelErrorCode::InvalidShape, path, "known-domain detail must be an object");
            }
            return detail.json();
        }

        std::size_t frontendUtf8PrefixLength(std::string_view value, std::size_t maximumBytes) noexcept {
            std::size_t offset = 0;
            while (offset < value.size() && offset < maximumBytes) {
                const unsigned char lead = static_cast<unsigned char>(value[offset]);
                std::size_t width = 0;
                if (lead <= 0x7fU) {
                    width = 1;
                } else if (lead >= 0xc2U && lead <= 0xdfU) {
                    width = 2;
                } else if (lead >= 0xe0U && lead <= 0xefU) {
                    width = 3;
                } else if (lead >= 0xf0U && lead <= 0xf4U) {
                    width = 4;
                } else {
                    break;
                }
                if (offset + width > value.size() || offset + width > maximumBytes) {
                    break;
                }
                bool valid = true;
                for (std::size_t index = 1; index < width; ++index) {
                    valid = valid && (static_cast<unsigned char>(value[offset + index]) & 0xc0U) == 0x80U;
                }
                if (!valid) {
                    break;
                }
                offset += width;
            }
            return offset;
        }

        Json boundedFrontendDetailScalar(const Json& value) {
            if (!value.is_string()) {
                return value;
            }
            const std::string& text = value.get_ref<const std::string&>();
            return std::string(text.substr(0, frontendUtf8PrefixLength(text, FrontendDetailMaximumStringBytes)));
        }

        Json expandedItemDetailObject(const SafeDetail& detail, bool userMessage) {
            // Canonical SafeDetail intentionally retains richer bounded JSON,
            // including user-message content objects. Frontend Protocol v1's
            // ExpandedThreadItem.data admits only scalar leaves or scalar
            // arrays, so normalize exactly once at the expanded-wire seam.
            const Json& value = detail.json();
            Json projected = Json::object();
            if (!value.is_object()) {
                return projected;
            }
            std::size_t retained = 0;
            bool userMessageTextTruncated = false;
            for (auto member = value.begin(); member != value.end() && retained < 64; ++member) {
                const std::size_t keyBytes = frontendUtf8PrefixLength(member.key(), 256);
                if (keyBytes == 0 && !member.key().empty()) {
                    continue;
                }
                const std::string key = member.key().substr(0, keyBytes);
                const bool safeSecretClassification = normalizedKey(key) == "issecret" && member.value().is_boolean();
                if (!safeSecretClassification && SafeDetail::isSecretKey(key)) {
                    // The canonical key may be safe only because of a suffix
                    // beyond the frontend key bound. Recheck the exact emitted
                    // key so truncation can never synthesize a forbidden
                    // Frontend Protocol property name.
                    continue;
                }
                const bool scalar =
                    member.value().is_null() || member.value().is_boolean() || member.value().is_number() || member.value().is_string();
                if (scalar) {
                    projected[key] = boundedFrontendDetailScalar(member.value());
                    if (userMessage && key == "text" && member.value().is_string()) {
                        userMessageTextTruncated = projected[key].get_ref<const std::string&>().size() !=
                                                   member.value().get_ref<const std::string&>().size();
                    }
                    ++retained;
                    continue;
                }
                if (!member.value().is_array()) {
                    continue;
                }
                Json values = Json::array();
                const std::size_t count = std::min<std::size_t>(member.value().size(), 64);
                bool scalarOnly = true;
                for (std::size_t index = 0; index < count; ++index) {
                    const Json& candidate = member.value()[index];
                    if (!candidate.is_null() && !candidate.is_boolean() && !candidate.is_number() && !candidate.is_string()) {
                        scalarOnly = false;
                        break;
                    }
                    values.push_back(boundedFrontendDetailScalar(candidate));
                }
                if (scalarOnly) {
                    projected[key] = std::move(values);
                    ++retained;
                }
            }
            if (userMessage && projected.contains("text") && projected.at("text").is_string()) {
                if (const auto truncated = projected.find("textTruncated"); truncated != projected.end() && truncated->is_boolean()) {
                    *truncated = truncated->get<bool>() || userMessageTextTruncated;
                }
                if (const auto retainedBytes = projected.find("retainedTextBytes");
                    retainedBytes != projected.end() && retainedBytes->is_number_unsigned()) {
                    *retainedBytes = static_cast<std::uint64_t>(projected.at("text").get_ref<const std::string&>().size());
                }
            }
            return projected;
        }

        template <typename Identity>
        Identity requiredIdentity(const Json& value, std::string_view key, const std::string& path) {
            const auto member = value.find(key);
            if (member == value.end() || !member->is_string()) {
                fail(ModelErrorCode::InvalidIdentifier, path + "/" + std::string(key), "required identifier is missing");
            }
            auto identity = Identity::parse(member->get<std::string>());
            if (!identity.has_value()) {
                fail(ModelErrorCode::InvalidIdentifier, path + "/" + std::string(key), "identifier is invalid");
            }
            return std::move(*identity);
        }

        template <typename Identity>
        std::optional<Identity> optionalIdentity(const std::optional<std::string>& value, const std::string& path) {
            if (!value.has_value()) {
                return std::nullopt;
            }
            auto identity = Identity::parse(*value);
            if (!identity.has_value()) {
                fail(ModelErrorCode::InvalidIdentifier, path, "identifier is invalid");
            }
            return std::move(*identity);
        }

        std::optional<std::string> optionalString(const Json& value, std::string_view key) {
            const auto member = value.find(key);
            if (member == value.end() || member->is_null()) {
                return std::nullopt;
            }
            if (!member->is_string()) {
                return std::nullopt;
            }
            return member->get<std::string>();
        }

        std::optional<std::int64_t> optionalSigned(const Json& value, std::string_view key) {
            const auto member = value.find(key);
            if (member == value.end() || !member->is_number_integer()) {
                return std::nullopt;
            }
            return member->get<std::int64_t>();
        }

        std::optional<std::uint64_t> optionalUnsigned(const Json& value, std::string_view key) {
            const auto member = value.find(key);
            if (member == value.end()) {
                return std::nullopt;
            }
            if (member->is_number_unsigned()) {
                return member->get<std::uint64_t>();
            }
            if (member->is_number_integer()) {
                const std::int64_t signedValue = member->get<std::int64_t>();
                if (signedValue >= 0) {
                    return static_cast<std::uint64_t>(signedValue);
                }
            }
            return std::nullopt;
        }

        bool booleanOr(const Json& value, std::string_view key, bool fallback) {
            const auto member = value.find(key);
            return member != value.end() && member->is_boolean() ? member->get<bool>() : fallback;
        }

        StateFreshness protocolFreshness(Freshness freshness) noexcept {
            switch (freshness) {
                case Freshness::Unknown:
                    return StateFreshness::Unknown;
                case Freshness::Current:
                    return StateFreshness::Current;
                case Freshness::Stale:
                    return StateFreshness::Stale;
            }
            return StateFreshness::Unknown;
        }

        Freshness modelFreshness(std::optional<StateFreshness> freshness) noexcept {
            if (!freshness.has_value()) {
                return Freshness::Unknown;
            }
            switch (*freshness) {
                case StateFreshness::Unknown:
                    return Freshness::Unknown;
                case StateFreshness::Current:
                    return Freshness::Current;
                case StateFreshness::Stale:
                    return Freshness::Stale;
            }
            return Freshness::Unknown;
        }

        Json encodeSourceMetadata(const SourceMetadata& stamp) {
            Json encoded = objectDetail(stamp.extensions, "/stamp/extensions");
            encoded["generation"] = stamp.generation;
            encoded["freshness"] = toString(protocolFreshness(stamp.freshness));
            return encoded;
        }

        SourceMetadata decodeSourceMetadata(const Json& value, const std::string& path) {
            if (!value.is_object()) {
                fail(ModelErrorCode::InvalidShape, path, "source stamp must be an object");
            }
            SourceMetadata result;
            result.generation = optionalUnsigned(value, "generation").value_or(0);
            if (const auto freshness = optionalString(value, "freshness"); freshness.has_value()) {
                result.freshness = modelFreshness(stateFreshnessFromString(*freshness));
            }
            Json remaining = value;
            remaining.erase("generation");
            remaining.erase("freshness");
            result.extensions = safeDetail(std::move(remaining), path + "/extensions");
            return result;
        }

        ThreadItem makeThreadItem(ThreadItemKind kind, ItemData data) {
            switch (kind) {
                case ThreadItemKind::AgentMessage:
                    return AgentMessageItem{std::move(data)};
                case ThreadItemKind::CollabAgentToolCall:
                    return CollabAgentToolCallItem{std::move(data)};
                case ThreadItemKind::CommandExecution:
                    return CommandExecutionItem{std::move(data)};
                case ThreadItemKind::ContextCompaction:
                    return ContextCompactionItem{std::move(data)};
                case ThreadItemKind::DynamicToolCall:
                    return DynamicToolCallItem{std::move(data)};
                case ThreadItemKind::EnteredReviewMode:
                    return EnteredReviewModeItem{std::move(data)};
                case ThreadItemKind::ExitedReviewMode:
                    return ExitedReviewModeItem{std::move(data)};
                case ThreadItemKind::FileChange:
                    return FileChangeItem{std::move(data)};
                case ThreadItemKind::HookPrompt:
                    return HookPromptItem{std::move(data)};
                case ThreadItemKind::ImageGeneration:
                    return ImageGenerationItem{std::move(data)};
                case ThreadItemKind::ImageView:
                    return ImageViewItem{std::move(data)};
                case ThreadItemKind::McpToolCall:
                    return McpToolCallItem{std::move(data)};
                case ThreadItemKind::Plan:
                    return PlanItem{std::move(data)};
                case ThreadItemKind::Reasoning:
                    return ReasoningItem{std::move(data)};
                case ThreadItemKind::Sleep:
                    return SleepItem{std::move(data)};
                case ThreadItemKind::SubAgentActivity:
                    return SubAgentActivityItem{std::move(data)};
                case ThreadItemKind::UserMessage:
                    return UserMessageItem{std::move(data)};
                case ThreadItemKind::WebSearch:
                    return WebSearchItem{std::move(data)};
            }
            fail(ModelErrorCode::UnsupportedDiscriminator, "/state/items/type", "unsupported item discriminator");
        }

        PendingRequest makePendingRequest(PendingRequestKind kind, PendingRequestData data) {
            switch (kind) {
                case PendingRequestKind::CommandExecutionApproval:
                    return CommandExecutionApprovalRequest{std::move(data)};
                case PendingRequestKind::FileChangeApproval:
                    return FileChangeApprovalRequest{std::move(data)};
                case PendingRequestKind::UserInput:
                    return UserInputRequest{std::move(data)};
                case PendingRequestKind::Authentication:
                    return AuthenticationRequest{std::move(data)};
                case PendingRequestKind::ApplyPatchApproval:
                    return ApplyPatchApprovalRequest{std::move(data)};
                case PendingRequestKind::ExecCommandApproval:
                    return ExecCommandApprovalRequest{std::move(data)};
                case PendingRequestKind::PermissionsApproval:
                    return PermissionsApprovalRequest{std::move(data)};
                case PendingRequestKind::Attestation:
                    return AttestationRequest{std::move(data)};
                case PendingRequestKind::DynamicToolCall:
                    return DynamicToolCallRequest{std::move(data)};
                case PendingRequestKind::McpElicitation:
                    return McpElicitationRequest{std::move(data)};
            }
            fail(ModelErrorCode::UnsupportedDiscriminator, "/state/pendingRequests/kind", "unsupported pending-request discriminator");
        }

        ExpandedThreadItem encodeItem(const ThreadItem& item) {
            const ItemData& data = itemData(item);
            ExpandedThreadItem encoded;
            encoded.id = data.id.value();
            encoded.type = threadItemKind(item);
            if (data.threadId.has_value()) {
                encoded.threadId = data.threadId->value();
            }
            if (data.turnId.has_value()) {
                encoded.turnId = data.turnId->value();
            }
            encoded.status = data.status.value_or("unknown");
            encoded.summary = data.summary;
            if (data.location.has_value()) {
                encoded.location = data.location->json();
            }
            encoded.agentText = data.agentText;
            encoded.reasoningText = data.reasoningText;
            encoded.reasoningSummary = data.reasoningSummary;
            encoded.commandOutput = data.commandOutput;
            encoded.droppedContentBytes = data.droppedContentBytes;
            encoded.contentTruncated = data.contentTruncated;
            encoded.startedAtMs = data.startedAtMs;
            encoded.completedAtMs = data.completedAtMs;
            if (data.safeDetails.has_value()) {
                Json projected = expandedItemDetailObject(*data.safeDetails, threadItemKind(item) == ThreadItemKind::UserMessage);
                if (!projected.empty()) {
                    encoded.data = std::move(projected);
                }
            }
            encoded.truncated = data.truncation.truncated;
            encoded.omittedFields = data.truncation.omittedPaths;
            encoded.connectionInvalidated = data.connectionInvalidated;
            encoded.generation = data.generation;
            encoded.freshness = protocolFreshness(data.freshness);
            encoded.extensions = data.extensions.json();
            return encoded;
        }

        ThreadItem decodeItem(const ExpandedThreadItem& item, std::size_t index) {
            const std::string path = "/state/items/" + std::to_string(index);
            auto id = ItemIdentity::parse(item.id);
            if (!id.has_value()) {
                fail(ModelErrorCode::InvalidIdentifier, path + "/id", "item identifier is invalid");
            }
            ItemData data{
                std::move(*id),
                optionalIdentity<ThreadIdentity>(item.threadId, path + "/threadId"),
                optionalIdentity<TurnIdentity>(item.turnId, path + "/turnId"),
            };
            data.status = item.status;
            data.summary = item.summary;
            if (item.location.has_value()) {
                data.location = safeDetail(*item.location, path + "/location");
            }
            data.agentText = item.agentText;
            data.reasoningText = item.reasoningText;
            data.reasoningSummary = item.reasoningSummary;
            data.commandOutput = item.commandOutput;
            data.droppedContentBytes = item.droppedContentBytes;
            data.contentTruncated = item.contentTruncated.value_or(false);
            data.startedAtMs = item.startedAtMs;
            data.completedAtMs = item.completedAtMs;
            if (item.data.has_value()) {
                data.safeDetails = safeDetail(*item.data, path + "/data");
            }
            data.truncation.truncated = item.truncated;
            data.truncation.omittedPaths = item.omittedFields;
            data.connectionInvalidated = item.connectionInvalidated;
            data.generation = item.generation;
            data.freshness = modelFreshness(item.freshness);
            data.extensions = safeDetail(item.extensions, path + "/extensions");
            data.sourceIndex = index;
            return makeThreadItem(item.type, std::move(data));
        }

        ExpandedPendingRequest encodePendingRequest(const PendingRequest& request) {
            const PendingRequestData& data = pendingRequestData(request);
            ExpandedPendingRequest encoded;
            encoded.pendingRequestId = data.id.value();
            encoded.kind = pendingRequestKind(request);
            if (data.threadId.has_value()) {
                encoded.threadId = data.threadId->value();
            }
            if (data.turnId.has_value()) {
                encoded.turnId = data.turnId->value();
            }
            if (data.itemId.has_value()) {
                encoded.itemId = data.itemId->value();
            }
            encoded.summary = data.summary;
            if (data.safeDetails.has_value()) {
                encoded.details = data.safeDetails->json();
            }
            if (data.questionsPresent || !data.questions.empty()) {
                std::vector<ExpandedPendingRequestQuestion> questions;
                questions.reserve(data.questions.size());
                for (const PendingRequestQuestion& question : data.questions) {
                    ExpandedPendingRequestQuestion encodedQuestion;
                    encodedQuestion.id = question.id;
                    encodedQuestion.header = question.header;
                    encodedQuestion.prompt = question.prompt;
                    encodedQuestion.allowsFreeText = question.allowsFreeText;
                    encodedQuestion.isSecret = question.secretAnswer;
                    encodedQuestion.extensions = question.extensions.json();
                    encodedQuestion.options.reserve(question.options.size());
                    for (const PendingRequestOption& option : question.options) {
                        encodedQuestion.options.push_back({option.label, option.description, option.extensions.json()});
                    }
                    questions.push_back(std::move(encodedQuestion));
                }
                encoded.questions = std::move(questions);
            }
            encoded.autoResolutionMs = data.autoResolutionMs;
            encoded.truncated = data.truncation.truncated;
            encoded.extensions = data.extensions.json();
            // Frontend Protocol v1 deliberately permits bounded safe-detail
            // extensions on this record.  Keep the stale-generation marker
            // there so the established wire vocabulary remains unchanged.
            if (data.connectionInvalidated) {
                encoded.extensions["connectionInvalidated"] = true;
            }
            return encoded;
        }

        PendingRequest decodePendingRequest(const ExpandedPendingRequest& request, std::size_t index) {
            const std::string path = "/state/pendingRequests/" + std::to_string(index);
            auto id = PendingRequestIdentity::parse(request.pendingRequestId);
            if (!id.has_value()) {
                fail(ModelErrorCode::InvalidIdentifier, path + "/pendingRequestId", "pending-request identifier is invalid");
            }
            PendingRequestData data{
                std::move(*id),
                optionalIdentity<ThreadIdentity>(request.threadId, path + "/threadId"),
                optionalIdentity<TurnIdentity>(request.turnId, path + "/turnId"),
                optionalIdentity<ItemIdentity>(request.itemId, path + "/itemId"),
            };
            data.summary = request.summary;
            if (request.details.has_value()) {
                data.safeDetails = safeDetail(*request.details, path + "/details");
            }
            if (request.questions.has_value()) {
                data.questionsPresent = true;
                data.questions.reserve(request.questions->size());
                for (std::size_t questionIndex = 0; questionIndex < request.questions->size(); ++questionIndex) {
                    const ExpandedPendingRequestQuestion& question = request.questions->at(questionIndex);
                    PendingRequestQuestion decoded{
                        question.id,
                        question.header,
                        question.prompt,
                        question.allowsFreeText,
                        question.isSecret,
                        {},
                        safeDetail(question.extensions, path + "/questions/" + std::to_string(questionIndex) + "/extensions"),
                    };
                    decoded.options.reserve(question.options.size());
                    for (std::size_t optionIndex = 0; optionIndex < question.options.size(); ++optionIndex) {
                        const ExpandedPendingRequestOption& option = question.options[optionIndex];
                        decoded.options.push_back({option.label,
                                                   option.description,
                                                   safeDetail(option.extensions,
                                                              path + "/questions/" + std::to_string(questionIndex) + "/options/" +
                                                                  std::to_string(optionIndex) + "/extensions")});
                    }
                    data.questions.push_back(std::move(decoded));
                }
            }
            data.autoResolutionMs = request.autoResolutionMs;
            data.truncation.truncated = request.truncated;
            Json extensions = request.extensions;
            if (const auto invalidated = extensions.find("connectionInvalidated"); invalidated != extensions.end()) {
                if (!invalidated->is_boolean()) {
                    fail(ModelErrorCode::InvalidShape,
                         path + "/extensions/connectionInvalidated",
                         "connection-invalidated marker must be boolean");
                }
                data.connectionInvalidated = invalidated->get<bool>();
                extensions.erase(invalidated);
            }
            data.extensions = safeDetail(std::move(extensions), path + "/extensions");
            data.sourceIndex = index;
            return makePendingRequest(request.kind, std::move(data));
        }

        Json encodeTruncation(const TruncationMetadata& value);
        Json encodeCompleteTruncation(const TruncationMetadata& value);
        TruncationMetadata decodeTruncation(const Json& value);

        Json encodeDomainResult(const DomainResultSummary& result, const std::string& path) {
            Json encoded = objectDetail(result.extensions, path + "/extensions");
            if (result.method.empty()) {
                fail(ModelErrorCode::InvalidShape, path + "/method", "domain result method must not be empty");
            }
            encoded["method"] = result.method;
            encoded["status"] = result.status;
            if (result.subjectId.has_value()) {
                encoded["subjectId"] = *result.subjectId;
            }
            if (result.nextCursor.has_value()) {
                encoded["nextCursor"] = *result.nextCursor;
            }
            if (result.itemCount.has_value()) {
                encoded["itemCount"] = *result.itemCount;
            }
            if (result.completeKnown) {
                encoded["complete"] = result.complete;
            }
            encoded["stamp"] = encodeSourceMetadata(result.stamp);
            return encoded;
        }

        DomainResultSummary decodeDomainResult(const Json& value, const std::string& path) {
            if (!value.is_object()) {
                fail(ModelErrorCode::InvalidShape, path, "domain result must be an object");
            }
            Json remaining = value;
            DomainResultSummary result;
            result.method = optionalString(value, "method").value_or("");
            if (result.method.empty()) {
                fail(ModelErrorCode::InvalidShape, path + "/method", "domain result method must not be empty");
            }
            result.status = optionalString(value, "status").value_or("");
            result.subjectId = optionalString(value, "subjectId");
            result.nextCursor = optionalString(value, "nextCursor");
            result.itemCount = optionalUnsigned(value, "itemCount");
            result.complete = booleanOr(value, "complete", false);
            result.completeKnown = value.contains("complete");
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                result.stamp = decodeSourceMetadata(*stamp, path + "/stamp");
            }
            constexpr std::array<std::string_view, 7> known{
                "method", "status", "subjectId", "nextCursor", "itemCount", "complete", "stamp"};
            for (std::string_view key : known) {
                remaining.erase(key);
            }
            result.extensions = safeDetail(std::move(remaining), path + "/extensions");
            return result;
        }

        std::optional<Json> encodeDomain(const DomainState& domain, const std::string& path) {
            if (!domain.valid()) {
                fail(ModelErrorCode::InvalidShape, path, "domain information state is invalid");
            }
            switch (domain.information) {
                case InformationState::Absent:
                case InformationState::Omitted:
                    return std::nullopt;
                case InformationState::NullValue:
                    return std::optional<Json>{std::in_place, nullptr};
                case InformationState::Present:
                case InformationState::Truncated:
                case InformationState::Stale:
                case InformationState::Redacted:
                case InformationState::Unavailable:
                case InformationState::Unknown:
                    break;
            }
            Json encoded = objectDetail(domain.extensions, path + "/extensions");
            if (domain.information != InformationState::Present) {
                constexpr std::array<std::string_view, 9> states{
                    "present", "omitted", "redacted", "truncated", "unavailable", "stale", "unknown", "absent", "null"};
                encoded["informationState"] = states[static_cast<std::size_t>(domain.information)];
            }
            if (domain.status.has_value()) {
                encoded["status"] = *domain.status;
            }
            if (domain.summary.has_value()) {
                encoded["summary"] = *domain.summary;
            }
            if (domain.nextCursor.has_value()) {
                encoded["nextCursor"] = *domain.nextCursor;
            }
            if (domain.itemCount.has_value()) {
                encoded["itemCount"] = *domain.itemCount;
            }
            if (domain.completeKnown) {
                encoded["complete"] = domain.complete;
            }
            if (domain.stampKnown) {
                encoded["stamp"] = encodeSourceMetadata(domain.stamp);
            }
            if (domain.latestResultsKnown) {
                encoded["latestResults"] = Json::array();
                for (std::size_t index = 0; index < domain.latestResults.size(); ++index) {
                    encoded["latestResults"].push_back(
                        encodeDomainResult(domain.latestResults[index], path + "/latestResults/" + std::to_string(index)));
                }
            }
            if (domain.truncationKnown) {
                encoded["truncation"] = encodeCompleteTruncation(domain.truncation);
            }
            if (domain.safeDetailsKnown) {
                encoded["details"] = domain.safeDetails.json();
            }
            return std::optional<Json>(std::in_place, std::move(encoded));
        }

        DomainState decodeDomain(const std::optional<Json>& value, const std::string& path) {
            if (!value.has_value()) {
                return {};
            }
            if (value->is_null()) {
                DomainState state;
                state.information = InformationState::NullValue;
                return state;
            }
            if (!value->is_object()) {
                fail(ModelErrorCode::InvalidShape, path, "known domain must be an object or null");
            }
            DomainState state = DomainState::present(Freshness::Unknown);
            Json remaining = *value;
            if (const auto information = optionalString(*value, "informationState"); information.has_value()) {
                constexpr std::array<std::pair<std::string_view, InformationState>, 9> states{{
                    {"present", InformationState::Present},
                    {"omitted", InformationState::Omitted},
                    {"redacted", InformationState::Redacted},
                    {"truncated", InformationState::Truncated},
                    {"unavailable", InformationState::Unavailable},
                    {"stale", InformationState::Stale},
                    {"unknown", InformationState::Unknown},
                    {"absent", InformationState::Absent},
                    {"null", InformationState::NullValue},
                }};
                const auto found = std::find_if(states.begin(), states.end(), [&](const auto& entry) {
                    return entry.first == *information;
                });
                if (found == states.end()) {
                    fail(ModelErrorCode::InvalidShape, path + "/informationState", "information state is invalid");
                }
                state.information = found->second;
            }
            state.status = optionalString(*value, "status");
            state.summary = optionalString(*value, "summary");
            state.nextCursor = optionalString(*value, "nextCursor");
            state.itemCount = optionalUnsigned(*value, "itemCount");
            state.complete = booleanOr(*value, "complete", false);
            state.completeKnown = value->contains("complete");
            state.stampKnown = value->contains("stamp");
            if (const auto stamp = value->find("stamp"); stamp != value->end()) {
                state.stamp = decodeSourceMetadata(*stamp, path + "/stamp");
            }
            if (const auto entries = value->find("latestResults"); entries != value->end()) {
                if (!entries->is_array()) {
                    fail(ModelErrorCode::InvalidShape, path + "/latestResults", "domain latestResults must be an array");
                }
                state.latestResults.reserve(entries->size());
                for (std::size_t index = 0; index < entries->size(); ++index) {
                    state.latestResults.push_back(decodeDomainResult(entries->at(index), path + "/latestResults/" + std::to_string(index)));
                }
            }
            state.latestResultsKnown = value->contains("latestResults");
            if (const auto truncation = value->find("truncation"); truncation != value->end()) {
                state.truncation = decodeTruncation(*truncation);
            }
            state.truncationKnown = value->contains("truncation");
            if (const auto details = value->find("details"); details != value->end()) {
                state.safeDetails = safeDetail(*details, path + "/details");
            }
            state.safeDetailsKnown = value->contains("details");
            constexpr std::array<std::string_view, 10> known{"informationState",
                                                             "status",
                                                             "summary",
                                                             "nextCursor",
                                                             "itemCount",
                                                             "complete",
                                                             "stamp",
                                                             "latestResults",
                                                             "truncation",
                                                             "details"};
            for (std::string_view key : known) {
                remaining.erase(key);
            }
            state.extensions = safeDetail(std::move(remaining), path + "/extensions");
            return state;
        }

        Json encodeTruncation(const TruncationMetadata& value) {
            Json encoded = objectDetail(value.extensions, "/truncation/extensions");
            encoded["truncated"] = value.truncated;
            if (value.omittedEntries.has_value()) {
                encoded["omittedEntries"] = *value.omittedEntries;
            }
            if (value.droppedBytesPresent || value.droppedBytes != 0) {
                encoded["droppedBytes"] = value.droppedBytes;
            }
            if (!value.omittedPaths.empty()) {
                encoded["omittedFields"] = value.omittedPaths;
            }
            return encoded;
        }

        Json encodeCompleteTruncation(const TruncationMetadata& value) {
            Json encoded = encodeTruncation(value);
            encoded["omittedEntries"] = value.omittedEntries.value_or(0);
            encoded["droppedBytes"] = value.droppedBytes;
            return encoded;
        }

        TruncationMetadata decodeTruncation(const Json& value) {
            TruncationMetadata result;
            if (!value.is_object()) {
                return result;
            }
            result.truncated = booleanOr(value, "truncated", false);
            if (const auto omitted = optionalUnsigned(value, "omittedEntries");
                omitted.has_value() && *omitted <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                result.omittedEntries = static_cast<std::size_t>(*omitted);
            }
            result.droppedBytes = optionalUnsigned(value, "droppedBytes").value_or(0);
            result.droppedBytesPresent = value.contains("droppedBytes");
            const auto paths = value.find("omittedFields");
            if (paths != value.end() && paths->is_array()) {
                for (const Json& path : *paths) {
                    if (path.is_string()) {
                        result.omittedPaths.push_back(path.get<std::string>());
                    }
                }
            }
            Json extensions = value;
            for (std::string_view key : {"truncated", "omittedEntries", "droppedBytes", "omittedFields"}) {
                extensions.erase(key);
            }
            result.extensions = safeDetail(std::move(extensions), "/truncation/extensions");
            return result;
        }

        ProjectionMetadata decodeProjection(const Json& value) {
            ProjectionMetadata result;
            if (!value.is_object()) {
                return result;
            }
            if (const auto stamp = optionalString(value, "projectionStamp"); stamp.has_value()) {
                result.projectionStamp = ProjectionStamp::parse(*stamp);
            }
            if (const auto stamp = optionalString(value, "sourceStamp"); stamp.has_value()) {
                result.sourceStamp = SourceStamp::parse(*stamp);
            }
            const std::array<std::pair<std::string_view, std::vector<std::string>*>, 8> paths{{
                {"omittedFields", &result.omittedPaths},
                {"redactedFields", &result.redactedPaths},
                {"truncatedFields", &result.truncatedPaths},
                {"unavailableFields", &result.unavailablePaths},
                {"staleFields", &result.stalePaths},
                {"unknownFields", &result.unknownPaths},
                {"absentFields", &result.absentPaths},
                {"nullFields", &result.nullPaths},
            }};
            for (const auto& [key, destination] : paths) {
                const auto member = value.find(key);
                if (member != value.end() && member->is_array()) {
                    for (const Json& path : *member) {
                        if (path.is_string()) {
                            destination->push_back(path.get<std::string>());
                        }
                    }
                }
            }
            return result;
        }

        Json encodeCapacity(const CapacityState& value) {
            Json encoded = objectDetail(value.extensions, "/state/capacity/extensions");
#define AISUITE_ENCODE_CAPACITY(member)                                                                                                    \
    if (value.member.has_value()) {                                                                                                        \
        encoded[#member] = *value.member;                                                                                                  \
    }
            AISUITE_ENCODE_CAPACITY(sessions)
            AISUITE_ENCODE_CAPACITY(observers)
            AISUITE_ENCODE_CAPACITY(activeOperations)
            AISUITE_ENCODE_CAPACITY(pendingRequests)
            AISUITE_ENCODE_CAPACITY(retainedThreads)
            AISUITE_ENCODE_CAPACITY(retainedTurns)
            AISUITE_ENCODE_CAPACITY(retainedItems)
            AISUITE_ENCODE_CAPACITY(accumulatedContentBytes)
            AISUITE_ENCODE_CAPACITY(retainedNotices)
            AISUITE_ENCODE_CAPACITY(retainedProcesses)
            AISUITE_ENCODE_CAPACITY(accumulatedProcessOutputBytes)
            AISUITE_ENCODE_CAPACITY(retainedFilesystemWatches)
            AISUITE_ENCODE_CAPACITY(retainedFuzzySearchSessions)
            AISUITE_ENCODE_CAPACITY(retainedActivityRecords)
            AISUITE_ENCODE_CAPACITY(evictedNotices)
            AISUITE_ENCODE_CAPACITY(evictedProcesses)
            AISUITE_ENCODE_CAPACITY(droppedProcessOutputBytes)
            AISUITE_ENCODE_CAPACITY(evictedFilesystemWatches)
            AISUITE_ENCODE_CAPACITY(evictedFuzzySearchSessions)
            AISUITE_ENCODE_CAPACITY(evictedActivityRecords)
#undef AISUITE_ENCODE_CAPACITY
            return encoded;
        }

        CapacityState decodeCapacity(const Json& value) {
            if (!value.is_object()) {
                fail(ModelErrorCode::InvalidShape, "/state/capacity", "capacity must be an object");
            }
            CapacityState result;
            Json remaining = value;
            const auto size = [&](std::string_view key) -> std::optional<std::size_t> {
                const auto decoded = optionalUnsigned(value, key);
                if (!decoded.has_value() || *decoded > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                    return std::nullopt;
                }
                return static_cast<std::size_t>(*decoded);
            };
#define AISUITE_DECODE_CAPACITY(member)                                                                                                    \
    result.member = size(#member);                                                                                                         \
    remaining.erase(#member)
            AISUITE_DECODE_CAPACITY(sessions);
            AISUITE_DECODE_CAPACITY(observers);
            AISUITE_DECODE_CAPACITY(activeOperations);
            AISUITE_DECODE_CAPACITY(pendingRequests);
            AISUITE_DECODE_CAPACITY(retainedThreads);
            AISUITE_DECODE_CAPACITY(retainedTurns);
            AISUITE_DECODE_CAPACITY(retainedItems);
            AISUITE_DECODE_CAPACITY(accumulatedContentBytes);
            AISUITE_DECODE_CAPACITY(retainedNotices);
            AISUITE_DECODE_CAPACITY(retainedProcesses);
            AISUITE_DECODE_CAPACITY(accumulatedProcessOutputBytes);
            AISUITE_DECODE_CAPACITY(retainedFilesystemWatches);
            AISUITE_DECODE_CAPACITY(retainedFuzzySearchSessions);
            AISUITE_DECODE_CAPACITY(retainedActivityRecords);
            AISUITE_DECODE_CAPACITY(evictedNotices);
            AISUITE_DECODE_CAPACITY(evictedProcesses);
            AISUITE_DECODE_CAPACITY(evictedFilesystemWatches);
            AISUITE_DECODE_CAPACITY(evictedFuzzySearchSessions);
            AISUITE_DECODE_CAPACITY(evictedActivityRecords);
#undef AISUITE_DECODE_CAPACITY
            result.droppedProcessOutputBytes = optionalUnsigned(value, "droppedProcessOutputBytes");
            remaining.erase("droppedProcessOutputBytes");
            result.extensions = safeDetail(std::move(remaining), "/state/capacity/extensions");
            return result;
        }

        Json encodeFilesystemWatch(const FilesystemWatchRecord& value) {
            Json encoded = objectDetail(value.extensions, "/filesystemWatch/extensions");
            encoded["watchId"] = value.watchId;
            if (value.root.has_value()) {
                encoded["root"] = *value.root;
            }
            if (value.changedPathCount.has_value()) {
                encoded["changedPathCount"] = *value.changedPathCount;
            }
            encoded["stamp"] = encodeSourceMetadata(value.stamp);
            encoded["connectionInvalidated"] = value.connectionInvalidated;
            if (value.truncation.truncated || value.truncation.omittedEntries.has_value() || value.truncation.droppedBytesPresent ||
                value.truncation.droppedBytes != 0 || !value.truncation.omittedPaths.empty()) {
                encoded["truncation"] = encodeTruncation(value.truncation);
            }
            if (!value.safeDetails.empty()) {
                encoded["details"] = value.safeDetails.json();
            }
            return encoded;
        }

        FilesystemWatchRecord decodeFilesystemWatch(const Json& value, const std::string& path) {
            if (!value.is_object()) {
                fail(ModelErrorCode::InvalidShape, path, "filesystem watch must be an object");
            }
            const auto identity = optionalString(value, "watchId");
            if (!identity.has_value() || identity->empty()) {
                fail(ModelErrorCode::InvalidIdentifier, path + "/watchId", "filesystem watch identifier is missing");
            }
            FilesystemWatchRecord result;
            result.watchId = *identity;
            result.root = optionalString(value, "root");
            result.changedPathCount = optionalUnsigned(value, "changedPathCount");
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                result.stamp = decodeSourceMetadata(*stamp, path + "/stamp");
            }
            result.connectionInvalidated = booleanOr(value, "connectionInvalidated", false);
            if (const auto truncation = value.find("truncation"); truncation != value.end()) {
                result.truncation = decodeTruncation(*truncation);
            }
            if (const auto details = value.find("details"); details != value.end()) {
                result.safeDetails = safeDetail(*details, path + "/details");
            }
            Json remaining = value;
            for (std::string_view key :
                 {"watchId", "root", "changedPathCount", "stamp", "connectionInvalidated", "truncation", "details"}) {
                remaining.erase(key);
            }
            result.extensions = safeDetail(std::move(remaining), path + "/extensions");
            Json publicExtensions = value;
            for (std::string_view key : {"watchId", "root", "changedPathCount", "stamp", "connectionInvalidated", "stateUnavailable"}) {
                publicExtensions.erase(key);
            }
            result.publicExtensions = safeDetail(std::move(publicExtensions), path + "/publicExtensions");
            result.publicExtensionsKnown = true;
            return result;
        }

        Json encodeFuzzySearch(const FuzzySearchRecord& value) {
            Json encoded = objectDetail(value.extensions, "/fuzzySearch/extensions");
            encoded["sessionId"] = value.sessionId;
            if (value.resultCount.has_value()) {
                encoded["resultCount"] = *value.resultCount;
            }
            encoded["complete"] = value.complete;
            encoded["stamp"] = encodeSourceMetadata(value.stamp);
            encoded["connectionInvalidated"] = value.connectionInvalidated;
            if (value.truncation.truncated || value.truncation.omittedEntries.has_value() || value.truncation.droppedBytesPresent ||
                value.truncation.droppedBytes != 0 || !value.truncation.omittedPaths.empty()) {
                encoded["truncation"] = encodeTruncation(value.truncation);
            }
            if (!value.safeDetails.empty()) {
                encoded["details"] = value.safeDetails.json();
            }
            return encoded;
        }

        FuzzySearchRecord decodeFuzzySearch(const Json& value, const std::string& path) {
            if (!value.is_object()) {
                fail(ModelErrorCode::InvalidShape, path, "fuzzy search must be an object");
            }
            const auto identity = optionalString(value, "sessionId");
            if (!identity.has_value() || identity->empty()) {
                fail(ModelErrorCode::InvalidIdentifier, path + "/sessionId", "fuzzy-search identifier is missing");
            }
            FuzzySearchRecord result;
            result.sessionId = *identity;
            result.resultCount = optionalUnsigned(value, "resultCount");
            result.complete = booleanOr(value, "complete", false);
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                result.stamp = decodeSourceMetadata(*stamp, path + "/stamp");
            }
            result.connectionInvalidated = booleanOr(value, "connectionInvalidated", false);
            if (const auto truncation = value.find("truncation"); truncation != value.end()) {
                result.truncation = decodeTruncation(*truncation);
            }
            if (const auto details = value.find("details"); details != value.end()) {
                result.safeDetails = safeDetail(*details, path + "/details");
            }
            Json remaining = value;
            for (std::string_view key :
                 {"sessionId", "resultCount", "complete", "stamp", "connectionInvalidated", "truncation", "details"}) {
                remaining.erase(key);
            }
            result.extensions = safeDetail(std::move(remaining), path + "/extensions");
            Json publicExtensions = value;
            for (std::string_view key : {"sessionId", "resultCount", "complete", "stamp", "connectionInvalidated", "stateUnavailable"}) {
                publicExtensions.erase(key);
            }
            result.publicExtensions = safeDetail(std::move(publicExtensions), path + "/publicExtensions");
            result.publicExtensionsKnown = true;
            return result;
        }

        Json encodeNotice(const NoticeRecord& value) {
            Json encoded = objectDetail(value.extensions, "/notice/extensions");
            if (value.occurrence != 0) {
                encoded["occurrence"] = value.occurrence;
            }
            encoded["category"] = value.category;
            encoded["summary"] = value.summary;
            if (value.details.has_value()) {
                encoded["details"] = *value.details;
            }
            if (value.threadId.has_value()) {
                encoded["threadId"] = value.threadId->value();
            }
            encoded["stamp"] = encodeSourceMetadata(value.stamp);
            if (!value.safeDetails.empty()) {
                encoded["safeDetails"] = value.safeDetails.json();
            }
            return encoded;
        }

        NoticeRecord decodeNotice(const Json& value, const std::string& path) {
            if (!value.is_object()) {
                fail(ModelErrorCode::InvalidShape, path, "notice must be an object");
            }
            NoticeRecord result;
            result.occurrence = optionalUnsigned(value, "occurrence").value_or(0);
            result.category = optionalString(value, "category").value_or("");
            result.summary = optionalString(value, "summary").value_or("");
            result.details = optionalString(value, "details");
            if (const auto thread = optionalString(value, "threadId"); thread.has_value()) {
                result.threadId = ThreadIdentity::parse(*thread);
                if (!result.threadId.has_value()) {
                    fail(ModelErrorCode::InvalidIdentifier, path + "/threadId", "notice thread identifier is invalid");
                }
            }
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                result.stamp = decodeSourceMetadata(*stamp, path + "/stamp");
            }
            if (const auto details = value.find("safeDetails"); details != value.end()) {
                result.safeDetails = safeDetail(*details, path + "/safeDetails");
            }
            Json remaining = value;
            for (std::string_view key : {"occurrence", "category", "summary", "details", "threadId", "stamp", "safeDetails"}) {
                remaining.erase(key);
            }
            result.extensions = safeDetail(std::move(remaining), path + "/extensions");
            return result;
        }

        Json encodeActivity(const ActivityRecord& value) {
            Json encoded = objectDetail(value.extensions, "/activity/extensions");
            if (!value.key.empty()) {
                encoded["key"] = value.key;
            }
            if (!value.subjectId.empty()) {
                encoded["subjectId"] = value.subjectId;
            }
            encoded["kind"] = value.kind;
            encoded["lifecycle"] = value.lifecycle;
            encoded["active"] = value.active;
            if (value.summary.has_value()) {
                encoded["summary"] = *value.summary;
            }
            if (value.details.has_value()) {
                encoded["details"] = *value.details;
            }
            if (value.threadId.has_value()) {
                encoded["threadId"] = value.threadId->value();
            }
            if (value.turnId.has_value()) {
                encoded["turnId"] = value.turnId->value();
            }
            encoded["stamp"] = encodeSourceMetadata(value.stamp);
            if (!value.safeDetails.empty()) {
                encoded["safeDetails"] = value.safeDetails.json();
            }
            return encoded;
        }

        ActivityRecord decodeActivity(const Json& value, const std::string& path) {
            if (!value.is_object()) {
                fail(ModelErrorCode::InvalidShape, path, "activity must be an object");
            }
            ActivityRecord result;
            result.key = optionalString(value, "key").value_or("");
            result.subjectId = optionalString(value, "subjectId").value_or("");
            result.kind = optionalString(value, "kind").value_or("");
            result.lifecycle = optionalString(value, "lifecycle").value_or("");
            result.summary = optionalString(value, "summary");
            result.details = optionalString(value, "details");
            result.active = booleanOr(value, "active", false);
            if (const auto thread = optionalString(value, "threadId"); thread.has_value()) {
                result.threadId = ThreadIdentity::parse(*thread);
                if (!result.threadId.has_value()) {
                    fail(ModelErrorCode::InvalidIdentifier, path + "/threadId", "activity thread identifier is invalid");
                }
            }
            if (const auto turn = optionalString(value, "turnId"); turn.has_value()) {
                result.turnId = TurnIdentity::parse(*turn);
                if (!result.turnId.has_value()) {
                    fail(ModelErrorCode::InvalidIdentifier, path + "/turnId", "activity turn identifier is invalid");
                }
            }
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                result.stamp = decodeSourceMetadata(*stamp, path + "/stamp");
            }
            if (const auto details = value.find("safeDetails"); details != value.end()) {
                result.safeDetails = safeDetail(*details, path + "/safeDetails");
            }
            Json remaining = value;
            for (std::string_view key :
                 {"key", "subjectId", "kind", "lifecycle", "summary", "details", "active", "threadId", "turnId", "stamp", "safeDetails"}) {
                remaining.erase(key);
            }
            result.extensions = safeDetail(std::move(remaining), path + "/extensions");
            return result;
        }

        template <typename Entry, typename Encoder>
        std::optional<Json>
        encodeTypedCollection(const DomainState& base, const std::vector<Entry>& entries, const std::string& path, Encoder&& encoder) {
            if (!base.latestResults.empty()) {
                fail(ModelErrorCode::InvalidShape, path, "typed collection must not duplicate entries in generic results");
            }
            if (!base.valid()) {
                fail(ModelErrorCode::InvalidShape, path, "typed collection information state is invalid");
            }
            if (base.information == InformationState::Absent || base.information == InformationState::Omitted) {
                if (!entries.empty()) {
                    fail(ModelErrorCode::InvalidShape, path, "absent typed collection contains entries");
                }
                return std::nullopt;
            }
            if (base.information == InformationState::NullValue) {
                if (!entries.empty()) {
                    fail(ModelErrorCode::InvalidShape, path, "null typed collection contains entries");
                }
                return std::optional<Json>{std::in_place, nullptr};
            }
            Json encoded = objectDetail(base.extensions, path + "/extensions");
            if (base.information != InformationState::Present) {
                constexpr std::array<std::string_view, 9> states{
                    "present", "omitted", "redacted", "truncated", "unavailable", "stale", "unknown", "absent", "null"};
                encoded["informationState"] = states[static_cast<std::size_t>(base.information)];
            }
            if (!base.safeDetails.empty()) {
                encoded["details"] = base.safeDetails.json();
            }
            encoded["entries"] = Json::array();
            for (std::size_t index = 0; index < entries.size(); ++index) {
                encoded["entries"].push_back(encoder(entries[index], path + "/entries/" + std::to_string(index)));
            }
            encoded["truncation"] = encodeCompleteTruncation(base.truncation);
            return std::optional<Json>{std::in_place, std::move(encoded)};
        }

        template <typename Entry, typename Decoder>
        std::pair<DomainState, std::vector<Entry>>
        decodeTypedCollection(const std::optional<Json>& value, const std::string& path, Decoder&& decoder) {
            if (!value.has_value() || value->is_null()) {
                return {decodeDomain(value, path), {}};
            }
            if (!value->is_object()) {
                fail(ModelErrorCode::InvalidShape, path, "typed collection must be an object");
            }
            Json base = *value;
            std::vector<Entry> entries;
            const auto member = base.find("entries");
            if (member != base.end()) {
                if (!member->is_array()) {
                    fail(ModelErrorCode::InvalidShape, path + "/entries", "typed collection entries must be an array");
                }
                entries.reserve(member->size());
                for (std::size_t index = 0; index < member->size(); ++index) {
                    entries.push_back(decoder(member->at(index), path + "/entries/" + std::to_string(index)));
                }
                base.erase(member);
            }
            return {decodeDomain(std::optional<Json>{std::move(base)}, path), std::move(entries)};
        }

        BackendCursorMetadata decodeBackendCursor(const Json& value) {
            BackendCursorMetadata result;
            if (!value.is_object()) {
                return result;
            }
            result.backendRevision = optionalUnsigned(value, "backendRevision");
            const auto sequence = [&](std::string_view key) -> std::optional<FrontendSequence> {
                const auto decoded = optionalUnsigned(value, key);
                return decoded.has_value() ? std::optional<FrontendSequence>{FrontendSequence(*decoded)} : std::nullopt;
            };
            result.oldestReplayableAfter = sequence("oldestReplayableAfter");
            result.currentSequence = sequence("currentSequence");
            result.oldestRetainedSequence = sequence("oldestRetainedSequence");
            result.newestRetainedSequence = sequence("newestRetainedSequence");
            if (const auto exhausted = value.find("backendSequenceExhausted"); exhausted != value.end() && exhausted->is_boolean()) {
                result.backendSequenceExhausted = exhausted->get<bool>();
            }
            if (const auto exhausted = value.find("frontendSequenceExhausted"); exhausted != value.end() && exhausted->is_boolean()) {
                result.frontendSequenceExhausted = exhausted->get<bool>();
            }
            if (const auto stamp = optionalString(value, "sourceStamp"); stamp.has_value()) {
                result.sourceStamp = SourceStamp::parse(*stamp);
            }
            return result;
        }
    } // namespace

    SafeDetail::SafeDetail()
        : detail(Json::object())
        , bytes(2) {
    }

    SafeDetail::SafeDetail(Json value, std::size_t serializedBytes)
        : detail(std::move(value))
        , bytes(serializedBytes) {
    }

    std::optional<SafeDetail> SafeDetail::fromJson(Json value, SafeDetailError* error, SafeDetailLimits limits) noexcept {
        setSafeDetailError(error, SafeDetailError::None);
        if (limits.maximumBytes > HardMaximumBytes || limits.maximumDepth > HardMaximumDepth ||
            limits.maximumMembers > HardMaximumMembers) {
            setSafeDetailError(error, SafeDetailError::InvalidLimits);
            return std::nullopt;
        }
        try {
            std::size_t members = 0;
            SafeDetailError inspectionError = SafeDetailError::None;
            if (!inspectSafeDetail(value, 0, members, limits, inspectionError)) {
                setSafeDetailError(error, inspectionError);
                return std::nullopt;
            }
            const std::size_t serializedBytes = value.dump().size();
            if (serializedBytes > limits.maximumBytes) {
                setSafeDetailError(error, SafeDetailError::ByteLimit);
                return std::nullopt;
            }
            return SafeDetail(std::move(value), serializedBytes);
        } catch (...) {
            setSafeDetailError(error, SafeDetailError::UnsupportedValue);
            return std::nullopt;
        }
    }

    const Json& SafeDetail::json() const noexcept {
        return detail;
    }

    std::size_t SafeDetail::serializedBytes() const noexcept {
        return bytes;
    }

    bool SafeDetail::empty() const noexcept {
        return detail.empty();
    }

    bool SafeDetail::isSecretKey(std::string_view key) noexcept {
        try {
            const std::string normalized = normalizedKey(key);
            // This is the frozen projection vocabulary.  Suffix matching keeps
            // accounting names such as tokenUsage, tokenBudget, and tokensUsed
            // while rejecting accessToken, occurrenceToken, and future
            // credential-token spellings.
            if (normalized.ends_with("token") || normalized.ends_with("secret") || normalized.ends_with("credential") ||
                normalized.ends_with("credentials") || normalized.find("password") != std::string::npos ||
                normalized.find("passphrase") != std::string::npos || normalized.find("authorization") != std::string::npos ||
                normalized.find("privatekey") != std::string::npos || normalized.find("apikey") != std::string::npos ||
                normalized == "authentication" || normalized == "cookie" || normalized == "setcookie" || normalized == "answer" ||
                normalized == "answers") {
                return true;
            }
            return normalized == "raw" || normalized.starts_with("raw") || normalized.ends_with("raw") ||
                   normalized.find("rawprovider") != std::string::npos || normalized == "providerenvelope" ||
                   normalized == "occurrencetoken" || normalized == "requesttoken";
        } catch (...) {
            return true;
        }
    }

    std::string_view toString(ProviderLifecycle lifecycle) noexcept {
        switch (lifecycle) {
            case ProviderLifecycle::Stopped:
                return "stopped";
            case ProviderLifecycle::Starting:
                return "starting";
            case ProviderLifecycle::Initializing:
                return "initializing";
            case ProviderLifecycle::Ready:
                return "ready";
            case ProviderLifecycle::Stopping:
                return "stopping";
            case ProviderLifecycle::Failed:
                return "failed";
            case ProviderLifecycle::Recovering:
                return "recovering";
        }
        return {};
    }

    std::optional<ProviderLifecycle> providerLifecycleFromString(std::string_view value) noexcept {
        constexpr std::array<ProviderLifecycle, 7> values{ProviderLifecycle::Stopped,
                                                          ProviderLifecycle::Starting,
                                                          ProviderLifecycle::Initializing,
                                                          ProviderLifecycle::Ready,
                                                          ProviderLifecycle::Stopping,
                                                          ProviderLifecycle::Failed,
                                                          ProviderLifecycle::Recovering};
        const auto found = std::find_if(values.begin(), values.end(), [value](ProviderLifecycle candidate) {
            return value == toString(candidate);
        });
        return found == values.end() ? std::nullopt : std::optional<ProviderLifecycle>{*found};
    }

    std::string_view toString(ProviderRecoveryStatus status) noexcept {
        switch (status) {
            case ProviderRecoveryStatus::Idle:
                return "idle";
            case ProviderRecoveryStatus::Waiting:
                return "waiting";
            case ProviderRecoveryStatus::Exhausted:
                return "exhausted";
        }
        return {};
    }

    std::optional<ProviderRecoveryStatus> providerRecoveryStatusFromString(std::string_view value) noexcept {
        constexpr std::array<ProviderRecoveryStatus, 3> values{
            ProviderRecoveryStatus::Idle, ProviderRecoveryStatus::Waiting, ProviderRecoveryStatus::Exhausted};
        const auto found = std::find_if(values.begin(), values.end(), [value](ProviderRecoveryStatus candidate) {
            return value == toString(candidate);
        });
        return found == values.end() ? std::nullopt : std::optional<ProviderRecoveryStatus>{*found};
    }

    DomainState DomainState::present(Freshness freshnessValue) {
        DomainState result;
        result.information = InformationState::Present;
        result.stamp.freshness = freshnessValue;
        return result;
    }

    bool DomainState::valid() const noexcept {
        switch (information) {
            case InformationState::Present:
            case InformationState::Truncated:
            case InformationState::Stale:
            case InformationState::Omitted:
            case InformationState::Redacted:
            case InformationState::Unavailable:
            case InformationState::Unknown:
            case InformationState::Absent:
            case InformationState::NullValue:
                return true;
        }
        return false;
    }

    ThreadItemKind threadItemKind(const ThreadItem& item) noexcept {
        return static_cast<ThreadItemKind>(item.index());
    }

    const ItemData& itemData(const ThreadItem& item) noexcept {
        return std::visit(
            [](const auto& value) -> const ItemData& {
                return value.value;
            },
            item);
    }

    PendingRequestKind pendingRequestKind(const PendingRequest& request) noexcept {
        return static_cast<PendingRequestKind>(request.index());
    }

    const PendingRequestData& pendingRequestData(const PendingRequest& request) noexcept {
        return std::visit(
            [](const auto& value) -> const PendingRequestData& {
                return value.value;
            },
            request);
    }

    ModelResult<ExpandedSnapshot> encodeSnapshot(const CanonicalSnapshot& snapshot) noexcept {
        try {
            ExpandedBackendSnapshotState state;

            state.provider = objectDetail(snapshot.provider.extensions, "/state/provider/extensions");
            if (snapshot.provider.provider.has_value()) {
                state.provider["provider"] = *snapshot.provider.provider;
            }
            state.provider["lifecycle"] = toString(snapshot.provider.lifecycle);
            state.provider["generation"] = snapshot.provider.generation;
            state.provider["desiredRunning"] = snapshot.provider.desiredRunning;
            Json recovery = objectDetail(snapshot.provider.recovery.extensions, "/state/provider/recovery/extensions");
            recovery["status"] = toString(snapshot.provider.recovery.status);
            recovery["attempts"] = snapshot.provider.recovery.attempts;
            if (snapshot.provider.recovery.delayMs.has_value()) {
                recovery["delayMs"] = *snapshot.provider.recovery.delayMs;
            }
            state.provider["recovery"] = std::move(recovery);
            if (snapshot.provider.lastError.has_value()) {
                state.provider["lastError"] = snapshot.provider.lastError->json();
            }
            if (snapshot.provider.initialization.has_value()) {
                state.provider["initialization"] = snapshot.provider.initialization->json();
            }

            state.controller = objectDetail(snapshot.controller.safeDetails, "/state/controller");
            const auto explicitControllerPresence = state.controller.find("present");
            if (explicitControllerPresence == state.controller.end() || !explicitControllerPresence->is_boolean()) {
                state.controller["present"] = snapshot.controller.session.has_value() || snapshot.controller.controller.has_value();
            }
            if (snapshot.controller.controller.has_value()) {
                state.controller["controllerId"] = snapshot.controller.controller->value();
            }
            if (snapshot.controller.session.has_value()) {
                state.controller["controllerSessionId"] = snapshot.controller.session->value();
            }

            state.sessions.reserve(snapshot.sessions.size());
            for (const SessionState& session : snapshot.sessions) {
                Json encoded = objectDetail(session.safeDetails, "/state/sessions");
                encoded["sessionId"] = session.id.value();
                encoded["role"] = toString(session.role);
                state.sessions.push_back(std::move(encoded));
            }

            state.threadList = objectDetail(snapshot.threadList.safeDetails, "/state/threadList");
            state.threadList["hasLoadedPage"] = snapshot.threadList.hasLoadedPage;
            state.threadList["complete"] = snapshot.threadList.complete;
            state.threadList["pagesLoaded"] = snapshot.threadList.pagesLoaded;
            if (snapshot.threadList.nextCursor.has_value()) {
                state.threadList["nextCursor"] = *snapshot.threadList.nextCursor;
            }
            if (snapshot.threadList.backwardsCursor.has_value()) {
                state.threadList["backwardsCursor"] = *snapshot.threadList.backwardsCursor;
            }
            if (snapshot.threadList.stampKnown) {
                state.threadList["stamp"] = encodeSourceMetadata(snapshot.threadList.stamp);
            }

            std::vector<Json> threads;
            threads.reserve(snapshot.threads.size());
            for (const ThreadState& thread : snapshot.threads) {
                Json encoded = objectDetail(thread.safeDetails, "/state/threads");
                encoded["id"] = thread.id.value();
                if (thread.title.has_value()) {
                    encoded["title"] = *thread.title;
                }
                encoded["fullyLoaded"] = thread.fullyLoaded;
                if (thread.stampKnown) {
                    encoded["stamp"] = encodeSourceMetadata(thread.stamp);
                }
                threads.push_back(std::move(encoded));
            }
            state.threads = std::move(threads);

            std::vector<Json> turns;
            turns.reserve(snapshot.turns.size());
            for (const TurnState& turn : snapshot.turns) {
                Json encoded = objectDetail(turn.safeDetails, "/state/turns");
                encoded["id"] = turn.id.value();
                encoded["threadId"] = turn.threadId.value();
                encoded["status"] = turn.status.value_or("unknown");
                encoded["active"] = turn.active;
                encoded["terminal"] = turn.terminal;
                if (turn.stampKnown) {
                    encoded["stamp"] = encodeSourceMetadata(turn.stamp);
                }
                encoded["connectionInvalidated"] = turn.connectionInvalidated;
                turns.push_back(std::move(encoded));
            }
            state.turns = std::move(turns);

            std::vector<ExpandedThreadItem> items;
            items.reserve(snapshot.items.size());
            for (const ThreadItem& item : snapshot.items) {
                items.push_back(encodeItem(item));
            }
            state.items = std::move(items);

            std::vector<ExpandedPendingRequest> pendingRequests;
            pendingRequests.reserve(snapshot.pendingRequests.size());
            for (const PendingRequest& request : snapshot.pendingRequests) {
                pendingRequests.push_back(encodePendingRequest(request));
            }
            state.pendingRequests = std::move(pendingRequests);

#define AISUITE_ENCODE_DOMAIN(member, wire)                                                                                                \
    do {                                                                                                                                   \
        state.wire = encodeDomain(snapshot.member.state, "/state/" #wire);                                                                 \
    } while (false)

            AISUITE_ENCODE_DOMAIN(accounts, accounts);
            AISUITE_ENCODE_DOMAIN(models, models);
            AISUITE_ENCODE_DOMAIN(configuration, configuration);
            AISUITE_ENCODE_DOMAIN(permissionProfiles, permissionProfiles);
            AISUITE_ENCODE_DOMAIN(reviews, reviews);
            AISUITE_ENCODE_DOMAIN(apps, apps);
            AISUITE_ENCODE_DOMAIN(externalAgents, externalAgents);
            AISUITE_ENCODE_DOMAIN(hooks, hooks);
            AISUITE_ENCODE_DOMAIN(marketplace, marketplace);
            AISUITE_ENCODE_DOMAIN(plugins, plugins);
            AISUITE_ENCODE_DOMAIN(skills, skills);
            AISUITE_ENCODE_DOMAIN(mcp, mcp);
            AISUITE_ENCODE_DOMAIN(windowsSandbox, windowsSandbox);
            AISUITE_ENCODE_DOMAIN(remoteControl, remoteControl);

#undef AISUITE_ENCODE_DOMAIN

            state.filesystemWatches = encodeTypedCollection(snapshot.filesystemWatches.state,
                                                            snapshot.filesystemWatches.entries,
                                                            "/state/filesystemWatches",
                                                            [](const FilesystemWatchRecord& value, const std::string&) {
                                                                return encodeFilesystemWatch(value);
                                                            });
            state.fuzzySearches = encodeTypedCollection(snapshot.fuzzySearches.state,
                                                        snapshot.fuzzySearches.entries,
                                                        "/state/fuzzySearches",
                                                        [](const FuzzySearchRecord& value, const std::string&) {
                                                            return encodeFuzzySearch(value);
                                                        });
            state.notices = encodeTypedCollection(
                snapshot.notices.state, snapshot.notices.entries, "/state/notices", [](const NoticeRecord& value, const std::string&) {
                    return encodeNotice(value);
                });
            state.activities = encodeTypedCollection(snapshot.activities.state,
                                                     snapshot.activities.entries,
                                                     "/state/activities",
                                                     [](const ActivityRecord& value, const std::string&) {
                                                         return encodeActivity(value);
                                                     });

            const bool processesOmitted =
                std::find(snapshot.projection.omittedPaths.begin(), snapshot.projection.omittedPaths.end(), "/processes") !=
                snapshot.projection.omittedPaths.end();
            if (!processesOmitted) {
                state.processes = encodeTypedCollection(snapshot.processesState,
                                                        snapshot.processes,
                                                        "/state/processes",
                                                        [](const ProcessState& process, const std::string& path) {
                                                            Json encoded = objectDetail(process.extensions, path + "/extensions");
                                                            encoded["processHandle"] = process.handle.value();
                                                            if (process.lifecycle.has_value()) {
                                                                encoded["lifecycle"] = *process.lifecycle;
                                                            }
                                                            if (process.stdoutBytes.has_value()) {
                                                                encoded["stdoutBytes"] = *process.stdoutBytes;
                                                            }
                                                            if (process.stderrBytes.has_value()) {
                                                                encoded["stderrBytes"] = *process.stderrBytes;
                                                            }
                                                            encoded["stdoutTruncated"] = process.stdoutTruncated;
                                                            encoded["stderrTruncated"] = process.stderrTruncated;
                                                            if (process.droppedOutputBytes.has_value()) {
                                                                encoded["droppedOutputBytes"] = *process.droppedOutputBytes;
                                                            }
                                                            encoded["stamp"] = encodeSourceMetadata(process.stamp);
                                                            encoded["connectionInvalidated"] = process.connectionInvalidated;
                                                            if (process.exitCode.has_value()) {
                                                                encoded["exitCode"] = *process.exitCode;
                                                            }
                                                            return encoded;
                                                        });
            }

            state.capacity = encodeCapacity(snapshot.capacity);
            state.truncation = encodeCompleteTruncation(snapshot.truncation);
            state.extensions = objectDetail(snapshot.stateExtensions, "/state/extensions");
            state.extensions.erase("codexExtensions");
            state.extensions.erase("omittedCodexExtensions");
            if (snapshot.backendCursor.frontendSequenceExhausted.has_value()) {
                state.extensions["frontendSequenceExhausted"] = *snapshot.backendCursor.frontendSequenceExhausted;
            }

            return ExpandedSnapshot{snapshot.sequence.protocolValue(), std::move(state), snapshot.extensions.json()};
        } catch (const ModelFailure& failure) {
            return failure.error;
        } catch (const std::exception& error) {
            return ModelError{ModelErrorCode::InvalidShape, "/state", std::string("snapshot encoding failed: ") + error.what()};
        } catch (...) {
            return ModelError{ModelErrorCode::InvalidShape, "/state", "snapshot encoding failed"};
        }
    }

    ModelResult<CanonicalSnapshot> decodeSnapshot(const ExpandedSnapshot& snapshot) noexcept {
        try {
            CanonicalSnapshot decoded;
            decoded.sequence = FrontendSequence(snapshot.sequence);
            decoded.threadsPresent = snapshot.state.threads.has_value();
            decoded.turnsPresent = snapshot.state.turns.has_value();
            decoded.itemsPresent = snapshot.state.items.has_value();
            decoded.pendingRequestsPresent = snapshot.state.pendingRequests.has_value();

            decoded.provider.provider = optionalString(snapshot.state.provider, "provider");
            const auto lifecycle = optionalString(snapshot.state.provider, "lifecycle");
            const auto decodedLifecycle = lifecycle.has_value() ? providerLifecycleFromString(*lifecycle) : std::nullopt;
            if (!decodedLifecycle.has_value()) {
                fail(ModelErrorCode::InvalidShape, "/state/provider/lifecycle", "provider lifecycle is invalid");
            }
            decoded.provider.lifecycle = *decodedLifecycle;
            decoded.provider.generation = optionalUnsigned(snapshot.state.provider, "generation").value_or(0);
            decoded.provider.desiredRunning = booleanOr(snapshot.state.provider, "desiredRunning", false);
            if (const auto recovery = snapshot.state.provider.find("recovery"); recovery != snapshot.state.provider.end()) {
                const auto status = optionalString(*recovery, "status");
                const auto decodedStatus = status.has_value() ? providerRecoveryStatusFromString(*status) : std::nullopt;
                if (!decodedStatus.has_value()) {
                    fail(ModelErrorCode::InvalidShape, "/state/provider/recovery/status", "provider recovery status is invalid");
                }
                decoded.provider.recovery.status = *decodedStatus;
                decoded.provider.recovery.attempts = optionalUnsigned(*recovery, "attempts").value_or(0);
                decoded.provider.recovery.delayMs = optionalUnsigned(*recovery, "delayMs");
                Json recoveryExtensions = *recovery;
                recoveryExtensions.erase("status");
                recoveryExtensions.erase("attempts");
                recoveryExtensions.erase("delayMs");
                decoded.provider.recovery.extensions = safeDetail(std::move(recoveryExtensions), "/state/provider/recovery/extensions");
            }
            if (const auto error = snapshot.state.provider.find("lastError"); error != snapshot.state.provider.end()) {
                decoded.provider.lastError = safeDetail(*error, "/state/provider/lastError");
            }
            if (const auto initialization = snapshot.state.provider.find("initialization");
                initialization != snapshot.state.provider.end()) {
                decoded.provider.initialization = safeDetail(*initialization, "/state/provider/initialization");
            }
            Json providerExtensions = snapshot.state.provider;
            for (std::string_view key :
                 {"provider", "lifecycle", "generation", "desiredRunning", "recovery", "lastError", "initialization"}) {
                providerExtensions.erase(key);
            }
            decoded.provider.extensions = safeDetail(std::move(providerExtensions), "/state/provider/extensions");

            Json controllerDetails = snapshot.state.controller;
            for (std::string_view key : {"controllerId", "controllerSessionId"}) {
                controllerDetails.erase(key);
            }
            decoded.controller.safeDetails = safeDetail(std::move(controllerDetails), "/state/controller");
            if (const auto controller = optionalString(snapshot.state.controller, "controllerId"); controller.has_value()) {
                decoded.controller.controller = ControllerIdentity::parse(*controller);
                if (!decoded.controller.controller.has_value()) {
                    fail(ModelErrorCode::InvalidIdentifier, "/state/controller/controllerId", "controller identifier is invalid");
                }
            }
            if (const auto session = optionalString(snapshot.state.controller, "controllerSessionId"); session.has_value()) {
                decoded.controller.session = SessionIdentity::parse(*session);
                if (!decoded.controller.session.has_value()) {
                    fail(ModelErrorCode::InvalidIdentifier, "/state/controller/controllerSessionId", "session identifier is invalid");
                }
            }

            decoded.sessions.reserve(snapshot.state.sessions.size());
            for (std::size_t index = 0; index < snapshot.state.sessions.size(); ++index) {
                const Json& session = snapshot.state.sessions[index];
                const std::string path = "/state/sessions/" + std::to_string(index);
                if (!session.is_object()) {
                    fail(ModelErrorCode::InvalidShape, path, "session must be an object");
                }
                const std::string idKey = session.contains("sessionId") ? "sessionId" : "id";
                SessionState value{requiredIdentity<SessionIdentity>(session, idKey, path)};
                if (const auto role = optionalString(session, "role"); role.has_value()) {
                    const auto decodedRole = sessionRoleFromString(*role);
                    if (!decodedRole.has_value()) {
                        fail(ModelErrorCode::InvalidShape, path + "/role", "session role is invalid");
                    }
                    value.role = *decodedRole;
                }
                value.principalId = optionalString(session, "principalId");
                if (const auto freshness = optionalString(session, "freshness"); freshness.has_value()) {
                    value.freshness = modelFreshness(stateFreshnessFromString(*freshness));
                }
                Json sessionDetails = session;
                // `freshness` is an additive Frontend Protocol safe-detail
                // member. Retain its exact spelling for the frozen public
                // SessionState::extensions surface while also decoding the
                // canonical freshness value above.
                for (std::string_view key : {"sessionId", "id", "role", "principalId"}) {
                    sessionDetails.erase(key);
                }
                value.safeDetails = safeDetail(std::move(sessionDetails), path);
                decoded.sessions.push_back(std::move(value));
            }

            decoded.threadList.hasLoadedPage = booleanOr(snapshot.state.threadList, "hasLoadedPage", false);
            decoded.threadList.complete = booleanOr(snapshot.state.threadList, "complete", false);
            decoded.threadList.pagesLoaded = optionalUnsigned(snapshot.state.threadList, "pagesLoaded").value_or(0);
            decoded.threadList.nextCursor = optionalString(snapshot.state.threadList, "nextCursor");
            decoded.threadList.backwardsCursor = optionalString(snapshot.state.threadList, "backwardsCursor");
            decoded.threadList.stampKnown = snapshot.state.threadList.contains("stamp");
            if (const auto stamp = snapshot.state.threadList.find("stamp"); stamp != snapshot.state.threadList.end()) {
                decoded.threadList.stamp = decodeSourceMetadata(*stamp, "/state/threadList/stamp");
            }
            Json threadListDetails = snapshot.state.threadList;
            for (std::string_view key : {"hasLoadedPage", "complete", "pagesLoaded", "nextCursor", "backwardsCursor", "stamp"}) {
                threadListDetails.erase(key);
            }
            decoded.threadList.safeDetails = safeDetail(std::move(threadListDetails), "/state/threadList");

            if (snapshot.state.threads.has_value()) {
                decoded.threads.reserve(snapshot.state.threads->size());
                for (std::size_t index = 0; index < snapshot.state.threads->size(); ++index) {
                    const Json& thread = snapshot.state.threads->at(index);
                    const std::string path = "/state/threads/" + std::to_string(index);
                    ThreadState value{requiredIdentity<ThreadIdentity>(thread, "id", path)};
                    value.title = optionalString(thread, "title");
                    value.createdAtMs = optionalSigned(thread, "createdAt");
                    value.updatedAtMs = optionalSigned(thread, "updatedAt");
                    value.fullyLoaded = booleanOr(thread, "fullyLoaded", false);
                    value.stampKnown = thread.contains("stamp");
                    if (const auto stamp = thread.find("stamp"); stamp != thread.end()) {
                        value.stamp = decodeSourceMetadata(*stamp, path + "/stamp");
                        value.freshness = value.stamp.freshness;
                    }
                    Json threadDetails = thread;
                    for (std::string_view key : {"id", "title", "createdAt", "updatedAt", "fullyLoaded", "stamp"}) {
                        threadDetails.erase(key);
                    }
                    value.safeDetails = safeDetail(std::move(threadDetails), path);
                    decoded.threads.push_back(std::move(value));
                }
            }

            if (snapshot.state.turns.has_value()) {
                decoded.turns.reserve(snapshot.state.turns->size());
                for (std::size_t index = 0; index < snapshot.state.turns->size(); ++index) {
                    const Json& turn = snapshot.state.turns->at(index);
                    const std::string path = "/state/turns/" + std::to_string(index);
                    TurnState value{requiredIdentity<TurnIdentity>(turn, "id", path),
                                    requiredIdentity<ThreadIdentity>(turn, "threadId", path)};
                    value.status = optionalString(turn, "status");
                    value.active = booleanOr(turn, "active", false);
                    value.terminal = booleanOr(turn, "terminal", false);
                    value.stampKnown = turn.contains("stamp");
                    if (const auto stamp = turn.find("stamp"); stamp != turn.end()) {
                        value.stamp = decodeSourceMetadata(*stamp, path + "/stamp");
                        value.freshness = value.stamp.freshness;
                    }
                    value.connectionInvalidated = booleanOr(turn, "connectionInvalidated", false);
                    Json turnDetails = turn;
                    for (std::string_view key : {"id", "threadId", "status", "active", "terminal", "stamp", "connectionInvalidated"}) {
                        turnDetails.erase(key);
                    }
                    value.safeDetails = safeDetail(std::move(turnDetails), path);
                    decoded.turns.push_back(std::move(value));
                }
            }

            if (snapshot.state.items.has_value()) {
                decoded.items.reserve(snapshot.state.items->size());
                for (std::size_t index = 0; index < snapshot.state.items->size(); ++index) {
                    decoded.items.push_back(decodeItem(snapshot.state.items->at(index), index));
                }
            }
            if (snapshot.state.pendingRequests.has_value()) {
                decoded.pendingRequests.reserve(snapshot.state.pendingRequests->size());
                for (std::size_t index = 0; index < snapshot.state.pendingRequests->size(); ++index) {
                    decoded.pendingRequests.push_back(decodePendingRequest(snapshot.state.pendingRequests->at(index), index));
                }
            }

#define AISUITE_DECODE_DOMAIN(member, wire)                                                                                                \
    do {                                                                                                                                   \
        decoded.member.state = decodeDomain(snapshot.state.wire, "/state/" #wire);                                                         \
    } while (false)

            AISUITE_DECODE_DOMAIN(accounts, accounts);
            AISUITE_DECODE_DOMAIN(models, models);
            AISUITE_DECODE_DOMAIN(configuration, configuration);
            AISUITE_DECODE_DOMAIN(permissionProfiles, permissionProfiles);
            AISUITE_DECODE_DOMAIN(reviews, reviews);
            AISUITE_DECODE_DOMAIN(apps, apps);
            AISUITE_DECODE_DOMAIN(externalAgents, externalAgents);
            AISUITE_DECODE_DOMAIN(hooks, hooks);
            AISUITE_DECODE_DOMAIN(marketplace, marketplace);
            AISUITE_DECODE_DOMAIN(plugins, plugins);
            AISUITE_DECODE_DOMAIN(skills, skills);
            AISUITE_DECODE_DOMAIN(mcp, mcp);
            AISUITE_DECODE_DOMAIN(windowsSandbox, windowsSandbox);
            AISUITE_DECODE_DOMAIN(remoteControl, remoteControl);

#undef AISUITE_DECODE_DOMAIN

            {
                auto [state, entries] = decodeTypedCollection<FilesystemWatchRecord>(
                    snapshot.state.filesystemWatches, "/state/filesystemWatches", decodeFilesystemWatch);
                decoded.filesystemWatches.state = std::move(state);
                decoded.filesystemWatches.entries = std::move(entries);
            }
            {
                auto [state, entries] =
                    decodeTypedCollection<FuzzySearchRecord>(snapshot.state.fuzzySearches, "/state/fuzzySearches", decodeFuzzySearch);
                decoded.fuzzySearches.state = std::move(state);
                decoded.fuzzySearches.entries = std::move(entries);
            }
            {
                auto [state, entries] = decodeTypedCollection<NoticeRecord>(snapshot.state.notices, "/state/notices", decodeNotice);
                decoded.notices.state = std::move(state);
                decoded.notices.entries = std::move(entries);
            }
            {
                auto [state, entries] =
                    decodeTypedCollection<ActivityRecord>(snapshot.state.activities, "/state/activities", decodeActivity);
                decoded.activities.state = std::move(state);
                decoded.activities.entries = std::move(entries);
            }

            if (snapshot.state.processes.has_value()) {
                const Json* entries = &*snapshot.state.processes;
                if (snapshot.state.processes->is_object()) {
                    Json base = *snapshot.state.processes;
                    const auto member = snapshot.state.processes->find("entries");
                    if (member == snapshot.state.processes->end()) {
                        entries = nullptr;
                    } else {
                        entries = &*member;
                        base.erase("entries");
                    }
                    if (const auto nested = base.find("extensions"); nested != base.end()) {
                        if (!nested->is_object()) {
                            fail(ModelErrorCode::InvalidShape,
                                 "/state/processes/extensions",
                                 "process collection extensions must be an object");
                        }
                        Json merged = *nested;
                        base.erase("extensions");
                        for (auto extension = base.begin(); extension != base.end(); ++extension) {
                            merged[extension.key()] = extension.value();
                        }
                        base = std::move(merged);
                    }
                    decoded.processesState = decodeDomain(std::optional<Json>{std::move(base)}, "/state/processes");
                } else {
                    decoded.processesState = DomainState::present();
                }
                if (entries == nullptr || !entries->is_array()) {
                    fail(ModelErrorCode::InvalidShape, "/state/processes/entries", "process entries must be an array");
                }
                decoded.processes.reserve(entries->size());
                for (std::size_t index = 0; index < entries->size(); ++index) {
                    const Json& process = entries->at(index);
                    const std::string path = "/state/processes/" + std::to_string(index);
                    std::optional<std::string> handle = optionalString(process, "processHandle");
                    if (!handle.has_value()) {
                        handle = optionalString(process, "handle");
                    }
                    if (!handle.has_value()) {
                        handle = optionalString(process, "id");
                    }
                    if (!handle.has_value()) {
                        const auto processId = optionalSigned(process, "processId");
                        if (processId.has_value()) {
                            handle = std::to_string(*processId);
                        }
                    }
                    if (!handle.has_value()) {
                        fail(ModelErrorCode::InvalidIdentifier, path + "/handle", "process handle is missing");
                    }
                    auto parsed = ProcessHandle::parse(*handle);
                    if (!parsed.has_value()) {
                        fail(ModelErrorCode::InvalidIdentifier, path + "/handle", "process handle is invalid");
                    }
                    ProcessState value{std::move(*parsed)};
                    value.processId = optionalSigned(process, "processId");
                    value.status = optionalString(process, "status");
                    value.lifecycle = optionalString(process, "lifecycle");
                    value.stdoutBytes = optionalUnsigned(process, "stdoutBytes");
                    value.stderrBytes = optionalUnsigned(process, "stderrBytes");
                    value.stdoutTruncated = booleanOr(process, "stdoutTruncated", false);
                    value.stderrTruncated = booleanOr(process, "stderrTruncated", false);
                    value.droppedOutputBytes = optionalUnsigned(process, "droppedOutputBytes");
                    if (const auto stamp = process.find("stamp"); stamp != process.end()) {
                        value.stamp = decodeSourceMetadata(*stamp, path + "/stamp");
                    }
                    value.connectionInvalidated = booleanOr(process, "connectionInvalidated", false);
                    value.exitCode = optionalSigned(process, "exitCode");
                    value.terminal = booleanOr(process, "terminal", false);
                    if (const auto truncation = process.find("truncation"); truncation != process.end()) {
                        value.truncation = decodeTruncation(*truncation);
                    }
                    if (const auto details = process.find("details"); details != process.end()) {
                        value.safeDetails = safeDetail(*details, path + "/details");
                    }
                    Json remaining = process;
                    constexpr std::array<std::string_view, 16> known{"processHandle",
                                                                     "handle",
                                                                     "id",
                                                                     "processId",
                                                                     "status",
                                                                     "lifecycle",
                                                                     "stdoutBytes",
                                                                     "stderrBytes",
                                                                     "stdoutTruncated",
                                                                     "stderrTruncated",
                                                                     "droppedOutputBytes",
                                                                     "stamp",
                                                                     "connectionInvalidated",
                                                                     "exitCode",
                                                                     "terminal",
                                                                     "truncation"};
                    for (std::string_view key : known) {
                        remaining.erase(key);
                    }
                    remaining.erase("details");
                    value.extensions = safeDetail(std::move(remaining), path + "/extensions");
                    Json publicExtensions = process;
                    for (std::string_view key : {"processHandle",
                                                 "lifecycle",
                                                 "stdout",
                                                 "stderr",
                                                 "stdoutBytes",
                                                 "stderrBytes",
                                                 "stdoutTruncated",
                                                 "stderrTruncated",
                                                 "droppedOutputBytes",
                                                 "exitCode",
                                                 "stamp",
                                                 "connectionInvalidated",
                                                 "stateUnavailable"}) {
                        publicExtensions.erase(key);
                    }
                    value.publicExtensions = safeDetail(std::move(publicExtensions), path + "/publicExtensions");
                    value.publicExtensionsKnown = true;
                    decoded.processes.push_back(std::move(value));
                }
            } else {
                decoded.processesState = {};
            }

            decoded.capacity = decodeCapacity(snapshot.state.capacity);
            decoded.truncation = decodeTruncation(snapshot.state.truncation);

            Json stateExtensions = snapshot.state.extensions;
            const auto decodeExtensionDomain = [&](std::string_view key, DomainState& destination) {
                const auto member = stateExtensions.find(key);
                if (member != stateExtensions.end()) {
                    destination = decodeDomain(std::optional<Json>{*member}, "/state/extensions/" + std::string(key));
                    stateExtensions.erase(member);
                }
            };
            decodeExtensionDomain("platform", decoded.platform.state);
            decodeExtensionDomain("integrations", decoded.integrations.state);
            if (const auto diagnostics = stateExtensions.find("diagnostics"); diagnostics != stateExtensions.end()) {
                if (!diagnostics->is_object()) {
                    fail(ModelErrorCode::InvalidShape, "/state/extensions/diagnostics", "diagnostics must be an object");
                }
                Json base = *diagnostics;
                decoded.diagnostics.received = optionalUnsigned(base, "received");
                base.erase("received");
                const auto recent = base.find("recent");
                if (recent != base.end()) {
                    if (!recent->is_array()) {
                        fail(ModelErrorCode::InvalidShape, "/state/extensions/diagnostics/recent", "diagnostics recent must be an array");
                    }
                    decoded.diagnostics.entries.reserve(recent->size());
                    for (std::size_t index = 0; index < recent->size(); ++index) {
                        if (!recent->at(index).is_string()) {
                            fail(ModelErrorCode::InvalidShape,
                                 "/state/extensions/diagnostics/recent/" + std::to_string(index),
                                 "diagnostic message must be a string");
                        }
                        DiagnosticRecord diagnostic;
                        diagnostic.message = recent->at(index).get<std::string>();
                        diagnostic.detailsOmitted = true;
                        decoded.diagnostics.entries.push_back(std::move(diagnostic));
                    }
                    base.erase(recent);
                }
                decoded.diagnostics.state = DomainState::present();
                decoded.diagnostics.state.extensions = safeDetail(std::move(base), "/state/extensions/diagnostics");
                stateExtensions.erase(diagnostics);
            }
            if (const auto projection = stateExtensions.find("projectionMetadata"); projection != stateExtensions.end()) {
                decoded.projection = decodeProjection(*projection);
                stateExtensions.erase(projection);
            }
            const auto rememberAbsentProjection = [&decoded](bool present, std::string path) {
                if (!present && std::find(decoded.projection.absentPaths.begin(), decoded.projection.absentPaths.end(), path) ==
                                    decoded.projection.absentPaths.end()) {
                    decoded.projection.absentPaths.push_back(std::move(path));
                }
            };
            // Preserve optional collection presence from the typed wire
            // record. Empty and absent projections have different public SDK
            // semantics even though both normalize to empty canonical
            // vectors.
            rememberAbsentProjection(snapshot.state.threads.has_value(), "/threads");
            rememberAbsentProjection(snapshot.state.turns.has_value(), "/turns");
            rememberAbsentProjection(snapshot.state.items.has_value(), "/items");
            rememberAbsentProjection(snapshot.state.pendingRequests.has_value(), "/pendingRequests");
            rememberAbsentProjection(snapshot.state.processes.has_value(), "/processes");
            if (const auto cursor = stateExtensions.find("backendCursor"); cursor != stateExtensions.end()) {
                decoded.backendCursor = decodeBackendCursor(*cursor);
                stateExtensions.erase(cursor);
            }
            if (const auto exhausted = stateExtensions.find("frontendSequenceExhausted");
                exhausted != stateExtensions.end() && exhausted->is_boolean()) {
                decoded.backendCursor.frontendSequenceExhausted = exhausted->get<bool>();
                stateExtensions.erase(exhausted);
            }
            decoded.stateExtensions = safeDetail(std::move(stateExtensions), "/state/extensions");
            decoded.extensions = safeDetail(snapshot.extensions, "/extensions");
            return decoded;
        } catch (const ModelFailure& failure) {
            return failure.error;
        } catch (const std::exception& error) {
            return ModelError{ModelErrorCode::InvalidShape, "/state", std::string("snapshot decoding failed: ") + error.what()};
        } catch (...) {
            return ModelError{ModelErrorCode::InvalidShape, "/state", "snapshot decoding failed"};
        }
    }

    namespace {
        std::string legacyLifecycle(ProviderLifecycle lifecycle) {
            return lifecycle == ProviderLifecycle::Recovering ? std::string{"starting"} : std::string{toString(lifecycle)};
        }

        bool legacyMetadataOnlyItem(ThreadItemKind kind) noexcept {
            const std::string registryKey = "item_discriminator:ThreadItem:type:" + std::string(toString(kind));
            const auto metadata = std::find_if(generated::AllThreadItemProjections.begin(),
                                               generated::AllThreadItemProjections.end(),
                                               [&registryKey](const generated::ProjectionMetadata& candidate) {
                                                   return candidate.registryKey == registryKey;
                                               });
            return metadata != generated::AllThreadItemProjections.end() && metadata->legacyContract == "legacy_metadata_only";
        }

        Json legacyItem(const ThreadItem& item) {
            const ItemData& value = itemData(item);
            const std::string discriminator = value.legacyDiscriminator.value_or(std::string(toString(threadItemKind(item))));
            const std::string sourceStatus = value.status.value_or("unknown");
            const std::string status = sourceStatus == "started" || sourceStatus == "completed" || sourceStatus == "failed"
                                           ? sourceStatus
                                           : std::string{"unknown"};
            Json data = value.safeDetails.has_value() && value.safeDetails->json().is_object() ? value.safeDetails->json() : Json::object();
            if (legacyMetadataOnlyItem(threadItemKind(item))) {
                data = Json{{"codexType", discriminator}};
            }
            const Json& extensions = value.legacyDiscriminator.has_value() ? value.legacyExtensions.json() : value.extensions.json();
            Json encoded{{"id", value.id.value()},
                         {"type", discriminator},
                         {"status", status},
                         {"agentText", value.agentText.value_or("")},
                         {"reasoningText", value.reasoningText.value_or("")},
                         {"reasoningSummary", value.reasoningSummary.value_or("")},
                         {"commandOutput", value.commandOutput.value_or("")},
                         {"droppedContentBytes", value.droppedContentBytes.value_or(0)},
                         {"contentTruncated", value.contentTruncated || value.truncation.truncated},
                         {"data", std::move(data)},
                         {"extensions", extensions.is_object() ? extensions : Json::object()}};
            if (value.startedAtMs.has_value()) {
                encoded["startedAtMs"] = *value.startedAtMs;
            }
            if (value.completedAtMs.has_value()) {
                encoded["completedAtMs"] = *value.completedAtMs;
            }
            return encoded;
        }

        Json legacyItem(const LegacyItemCompatibility& item) {
            const ItemData& value = item.value;
            const std::string sourceStatus = value.status.value_or("unknown");
            const std::string status = sourceStatus == "started" || sourceStatus == "completed" || sourceStatus == "failed"
                                           ? sourceStatus
                                           : std::string{"unknown"};
            Json data = value.safeDetails.has_value() && value.safeDetails->json().is_object() ? value.safeDetails->json() : Json::object();
            Json encoded{{"id", value.id.value()},
                         {"type", item.discriminator.empty() ? std::string{"unknown"} : item.discriminator},
                         {"status", status},
                         {"agentText", value.agentText.value_or("")},
                         {"reasoningText", value.reasoningText.value_or("")},
                         {"reasoningSummary", value.reasoningSummary.value_or("")},
                         {"commandOutput", value.commandOutput.value_or("")},
                         {"droppedContentBytes", value.droppedContentBytes.value_or(0)},
                         {"contentTruncated", value.contentTruncated || value.truncation.truncated},
                         {"data", std::move(data)},
                         {"extensions", value.legacyExtensions.json().is_object() ? value.legacyExtensions.json() : Json::object()}};
            if (value.startedAtMs.has_value()) {
                encoded["startedAtMs"] = *value.startedAtMs;
            }
            if (value.completedAtMs.has_value()) {
                encoded["completedAtMs"] = *value.completedAtMs;
            }
            return encoded;
        }

        Json legacyPendingRequest(const PendingRequest& request) {
            const PendingRequestData& value = pendingRequestData(request);
            std::string kindName = "unknown";
            switch (pendingRequestKind(request)) {
                case PendingRequestKind::CommandExecutionApproval:
                    kindName = "command_approval";
                    break;
                case PendingRequestKind::FileChangeApproval:
                    kindName = "file_change_approval";
                    break;
                case PendingRequestKind::UserInput:
                    kindName = "user_input";
                    break;
                case PendingRequestKind::Authentication:
                    kindName = "authentication";
                    break;
                default:
                    break;
            }
            Json details =
                value.safeDetails.has_value() && value.safeDetails->json().is_object() ? value.safeDetails->json() : Json::object();
            if (pendingRequestKind(request) == PendingRequestKind::ApplyPatchApproval ||
                pendingRequestKind(request) == PendingRequestKind::ExecCommandApproval ||
                pendingRequestKind(request) == PendingRequestKind::PermissionsApproval) {
                Json compatible = Json::object();
                for (std::string_view key : {"method",
                                             "methodTruncated",
                                             "originalMethodBytes",
                                             "params",
                                             "sensitiveFieldsRedacted",
                                             "paramsTruncated",
                                             "originalParamsBytes",
                                             "decodingError",
                                             "decodingErrorTruncated",
                                             "originalDecodingErrorBytes"}) {
                    if (const auto member = details.find(key); member != details.end()) {
                        compatible[std::string(key)] = *member;
                    }
                }
                details = std::move(compatible);
            } else if (pendingRequestKind(request) == PendingRequestKind::UserInput) {
                if (value.questionsPresent || !value.questions.empty()) {
                    details["questions"] = Json::array();
                    for (const PendingRequestQuestion& question : value.questions) {
                        Json typedQuestion{{"id", question.id},
                                           {"header", question.header},
                                           {"prompt", question.prompt},
                                           {"allowsFreeText", question.allowsFreeText},
                                           {"secret", question.secretAnswer},
                                           {"options", Json::array()}};
                        for (const PendingRequestOption& option : question.options) {
                            typedQuestion["options"].push_back(Json{{"label", option.label}, {"description", option.description}});
                        }
                        details["questions"].push_back(std::move(typedQuestion));
                    }
                }
                if (value.autoResolutionMs.has_value()) {
                    details["autoResolutionMs"] = *value.autoResolutionMs;
                }
            } else if (pendingRequestKind(request) == PendingRequestKind::Attestation ||
                       pendingRequestKind(request) == PendingRequestKind::DynamicToolCall ||
                       pendingRequestKind(request) == PendingRequestKind::McpElicitation) {
                details = Json::object();
            }
            Json encoded{{"id", value.id.value()}, {"type", std::move(kindName)}, {"details", std::move(details)}};
            if (value.threadId.has_value()) {
                encoded["threadId"] = value.threadId->value();
            }
            if (value.turnId.has_value()) {
                encoded["turnId"] = value.turnId->value();
            }
            if (value.itemId.has_value()) {
                encoded["itemId"] = value.itemId->value();
            }
            return encoded;
        }

        Json legacyPendingRequest(const LegacyPendingRequestCompatibility& request) {
            const PendingRequestData& value = request.value;
            Json details =
                value.safeDetails.has_value() && value.safeDetails->json().is_object() ? value.safeDetails->json() : Json::object();
            Json encoded{{"id", value.id.value()}, {"type", "unknown"}, {"details", std::move(details)}};
            if (value.threadId.has_value()) {
                encoded["threadId"] = value.threadId->value();
            }
            if (value.turnId.has_value()) {
                encoded["turnId"] = value.turnId->value();
            }
            if (value.itemId.has_value()) {
                encoded["itemId"] = value.itemId->value();
            }
            return encoded;
        }

        Json metadataCompatibleItems(const Json& items) {
            Json result = Json::array();
            if (!items.is_array()) {
                return result;
            }
            for (const Json& item : items) {
                if (!item.is_object()) {
                    continue;
                }
                const auto id = item.find("id");
                const auto type = item.find("type");
                if (id == item.end() || !id->is_string() || id->get_ref<const std::string&>().empty() || type == item.end() ||
                    !type->is_string()) {
                    continue;
                }
                const std::string& typeName = type->get_ref<const std::string&>();
                const std::string registryKey = "item_discriminator:ThreadItem:type:" + typeName;
                const auto metadata = std::find_if(generated::AllThreadItemProjections.begin(),
                                                   generated::AllThreadItemProjections.end(),
                                                   [&registryKey](const generated::ProjectionMetadata& candidate) {
                                                       return candidate.registryKey == registryKey;
                                                   });
                if (metadata == generated::AllThreadItemProjections.end() || metadata->legacyContract != "legacy_metadata_only") {
                    result.push_back(item);
                    continue;
                }
                Json compatible{{"id", *id}, {"type", typeName}, {"codexType", typeName}};
                if (const auto decodingError = item.find("decodingError"); decodingError != item.end() && decodingError->is_string()) {
                    compatible["decodingError"] = *decodingError;
                }
                result.push_back(std::move(compatible));
            }
            return result;
        }

        Json metadataCompatibleItem(const LegacyItemCompatibility& item) {
            Json compatible{{"id", item.value.id.value()},
                            {"type", item.discriminator.empty() ? std::string{"unknown"} : item.discriminator},
                            {"codexType", item.discriminator.empty() ? std::string{"unknown"} : item.discriminator}};
            if (item.value.threadId.has_value()) {
                compatible["threadId"] = item.value.threadId->value();
            }
            if (item.value.turnId.has_value()) {
                compatible["turnId"] = item.value.turnId->value();
            }
            if (item.value.truncation.truncated) {
                compatible["truncated"] = true;
            }
            return compatible;
        }

        Json legacySnapshotState(const CanonicalSnapshot& snapshot) {
            Json state = snapshot.stateExtensions.json();
            if (!state.is_object()) {
                state = Json::object();
            }
            for (const LegacyRootExtension& extension : snapshot.legacyRootExtensions) {
                if (!SafeDetail::isSecretKey(extension.name) && !state.contains(extension.name)) {
                    state[extension.name] = extension.value.json();
                }
            }
            state.erase("projectionMetadata");
            state.erase("backendCursor");
            state.erase("diagnostics");
            const std::uint64_t backendRevision = snapshot.backendCursor.backendRevision.value_or(0);
            state["backendRevision"] = backendRevision;
            state["lifecycle"] = legacyLifecycle(snapshot.provider.lifecycle);
            if (snapshot.provider.lastError.has_value()) {
                state["lastLifecycleError"] = snapshot.provider.lastError->json();
            }
            if (snapshot.controller.session.has_value()) {
                state["controllerSessionId"] = snapshot.controller.session->value();
            }
            state["sessions"] = Json::array();
            for (const SessionState& session : snapshot.sessions) {
                Json encoded{{"sessionId", session.id.value()}, {"role", toString(session.role)}};
                if (session.principalId.has_value()) {
                    encoded["principalId"] = *session.principalId;
                }
                state["sessions"].push_back(std::move(encoded));
            }
            state["threadList"] = snapshot.threadList.safeDetails.json();
            state["threadList"]["hasLoadedPage"] = snapshot.threadList.hasLoadedPage;
            state["threadList"]["complete"] = snapshot.threadList.complete;
            state["threadList"]["pagesLoaded"] = snapshot.threadList.pagesLoaded;
            if (snapshot.threadList.nextCursor.has_value()) {
                state["threadList"]["nextCursor"] = *snapshot.threadList.nextCursor;
            }
            if (snapshot.threadList.backwardsCursor.has_value()) {
                state["threadList"]["backwardsCursor"] = *snapshot.threadList.backwardsCursor;
            }

            state["threads"] = Json::array();
            for (const ThreadState& thread : snapshot.threads) {
                Json encoded = thread.safeDetails.json();
                if (!encoded.is_object()) {
                    encoded = Json::object();
                }
                encoded.erase("realtime");
                encoded["id"] = thread.id.value();
                encoded["fullyLoaded"] = thread.fullyLoaded;
                encoded["turns"] = Json::array();
                encoded["extensions"] = thread.legacyExtensions.json().is_object() ? thread.legacyExtensions.json() : Json::object();
                if (thread.title.has_value()) {
                    encoded["title"] = *thread.title;
                }
                if (thread.createdAtMs.has_value()) {
                    encoded["createdAt"] = *thread.createdAtMs;
                }
                if (thread.updatedAtMs.has_value()) {
                    encoded["updatedAt"] = *thread.updatedAtMs;
                }
                for (const TurnState& turn : snapshot.turns) {
                    if (turn.threadId != thread.id) {
                        continue;
                    }
                    Json encodedTurn = turn.safeDetails.json();
                    if (!encodedTurn.is_object()) {
                        encodedTurn = Json::object();
                    }
                    encodedTurn["id"] = turn.id.value();
                    encodedTurn["threadId"] = turn.threadId.value();
                    encodedTurn["status"] = turn.status.value_or("unknown");
                    encodedTurn["active"] = turn.active;
                    encodedTurn["terminal"] = turn.terminal;
                    encodedTurn["items"] = Json::array();
                    encodedTurn["extensions"] = turn.legacyExtensions.json().is_object() ? turn.legacyExtensions.json() : Json::object();
                    std::vector<std::pair<std::size_t, Json>> orderedItems;
                    std::size_t fallbackIndex = 0;
                    for (const ThreadItem& item : snapshot.items) {
                        if (itemData(item).turnId == std::optional<TurnIdentity>{turn.id}) {
                            orderedItems.emplace_back(itemData(item).sourceIndex.value_or(fallbackIndex++), legacyItem(item));
                        }
                    }
                    for (const LegacyItemCompatibility& item : snapshot.legacyItems) {
                        if (item.value.turnId == std::optional<TurnIdentity>{turn.id}) {
                            orderedItems.emplace_back(item.sourceIndex, legacyItem(item));
                        }
                    }
                    std::stable_sort(orderedItems.begin(), orderedItems.end(), [](const auto& left, const auto& right) {
                        return left.first < right.first;
                    });
                    for (auto& [index, item] : orderedItems) {
                        (void) index;
                        encodedTurn["items"].push_back(std::move(item));
                    }
                    encoded["turns"].push_back(std::move(encodedTurn));
                }
                state["threads"].push_back(std::move(encoded));
            }

            state["pendingRequests"] = Json::array();
            std::vector<std::pair<std::size_t, Json>> orderedPending;
            std::size_t fallbackPendingIndex = 0;
            for (const PendingRequest& request : snapshot.pendingRequests) {
                orderedPending.emplace_back(pendingRequestData(request).sourceIndex.value_or(fallbackPendingIndex++),
                                            legacyPendingRequest(request));
            }
            for (const LegacyPendingRequestCompatibility& request : snapshot.legacyPendingRequests) {
                orderedPending.emplace_back(request.sourceIndex, legacyPendingRequest(request));
            }
            std::stable_sort(orderedPending.begin(), orderedPending.end(), [](const auto& left, const auto& right) {
                return left.first < right.first;
            });
            for (auto& [index, request] : orderedPending) {
                (void) index;
                state["pendingRequests"].push_back(std::move(request));
            }

            state["diagnostics"] = Json{{"received", snapshot.diagnostics.received.value_or(0)}, {"recent", Json::array()}};
            for (const DiagnosticRecord& diagnostic : snapshot.diagnostics.entries) {
                if (diagnostic.message.has_value()) {
                    state["diagnostics"]["recent"].push_back(*diagnostic.message);
                }
            }
            Json codexExtensions = Json::array();
            if (const auto retained = snapshot.stateExtensions.json().find("codexExtensions");
                retained != snapshot.stateExtensions.json().end() && retained->is_array()) {
                const std::size_t begin = retained->size() > 64 ? retained->size() - 64 : 0;
                for (std::size_t index = begin; index < retained->size(); ++index) {
                    codexExtensions.push_back((*retained)[index]);
                }
            }
            state["codexExtensions"] = std::move(codexExtensions);
            state["omittedCodexExtensions"] = snapshot.stateExtensions.json().value("omittedCodexExtensions", std::size_t{0});
            Json journal{{"oldestReplayableAfter", snapshot.backendCursor.oldestReplayableAfter.value_or(FrontendSequence{}).value()},
                         {"currentSequence", snapshot.backendCursor.currentSequence.value_or(snapshot.sequence).value()}};
            if (snapshot.backendCursor.oldestRetainedSequence.has_value()) {
                journal["oldestRetainedSequence"] = snapshot.backendCursor.oldestRetainedSequence->value();
            }
            if (snapshot.backendCursor.newestRetainedSequence.has_value()) {
                journal["newestRetainedSequence"] = snapshot.backendCursor.newestRetainedSequence->value();
            }
            state["journal"] = std::move(journal);
            state["sequenceExhausted"] = snapshot.backendCursor.backendSequenceExhausted.value_or(false);
            if (snapshot.backendCursor.frontendSequenceExhausted.has_value()) {
                state["frontendSequenceExhausted"] = *snapshot.backendCursor.frontendSequenceExhausted;
            }
            return state;
        }

        std::optional<std::string> expandedItemType(std::string_view value, const Json& data) {
            if (threadItemKindFromString(value).has_value()) {
                return std::string(value);
            }
            constexpr std::array<std::pair<std::string_view, ThreadItemKind>, 5> legacy{{
                {"agent_message", ThreadItemKind::AgentMessage},
                {"user_message", ThreadItemKind::UserMessage},
                {"command_execution", ThreadItemKind::CommandExecution},
                {"file_change", ThreadItemKind::FileChange},
                {"web_search", ThreadItemKind::WebSearch},
            }};
            for (const auto& [name, kindValue] : legacy) {
                if (value == name) {
                    return std::string(toString(kindValue));
                }
            }
            if (value == "tool_call") {
                return std::string(toString(data.contains("server") ? ThreadItemKind::McpToolCall : ThreadItemKind::DynamicToolCall));
            }
            if (const auto codexType = optionalString(data, "codexType");
                codexType.has_value() && threadItemKindFromString(*codexType).has_value()) {
                return codexType;
            }
            return std::nullopt;
        }

        Json normalizeLegacyItem(Json item, std::optional<std::string> threadId, std::optional<std::string> turnId) {
            const Json data = item.value("data", Json::object());
            const auto kindName = optionalString(item, "type");
            const auto type = kindName.has_value() ? expandedItemType(*kindName, data) : std::nullopt;
            if (!type.has_value()) {
                fail(ModelErrorCode::UnsupportedDiscriminator, "/state/threads/turns/items/type", "legacy item type is unsupported");
            }
            item["type"] = *type;
            if (!item.contains("threadId") && threadId.has_value()) {
                item["threadId"] = *threadId;
            }
            if (!item.contains("turnId") && turnId.has_value()) {
                item["turnId"] = *turnId;
            }
            if (const auto extensions = item.find("extensions"); extensions != item.end() && extensions->is_object()) {
                Json merged = *extensions;
                item.erase(extensions);
                for (auto member = merged.begin(); member != merged.end(); ++member) {
                    if (!item.contains(member.key())) {
                        item[member.key()] = member.value();
                    }
                }
            }
            return item;
        }

        Json normalizeLegacyPending(const Json& pending) {
            if (pending.contains("pendingRequestId")) {
                return pending;
            }
            const auto id = optionalString(pending, "id");
            if (!id.has_value()) {
                fail(ModelErrorCode::InvalidIdentifier, "/state/pendingRequests/id", "legacy pending request id is missing");
            }
            const std::string legacyKind = optionalString(pending, "type").value_or("unknown");
            std::optional<PendingRequestKind> kindValue = pendingRequestKindFromString(legacyKind);
            if (legacyKind == "command_approval") {
                kindValue = PendingRequestKind::CommandExecutionApproval;
            }
            if (legacyKind == "file_change_approval") {
                kindValue = PendingRequestKind::FileChangeApproval;
            } else if (legacyKind == "user_input") {
                kindValue = PendingRequestKind::UserInput;
            } else if (legacyKind == "authentication") {
                kindValue = PendingRequestKind::Authentication;
            }
            if (!kindValue.has_value()) {
                fail(ModelErrorCode::UnsupportedDiscriminator,
                     "/state/pendingRequests/type",
                     "legacy pending request type is not reducible to a typed v1 kind");
            }
            Json expanded{{"pendingRequestId", *id}, {"kind", toString(*kindValue)}};
            for (std::string_view key : {"threadId", "turnId", "itemId", "summary", "truncated"}) {
                if (const auto value = pending.find(key); value != pending.end()) {
                    expanded[std::string(key)] = *value;
                }
            }
            if (const auto details = pending.find("details"); details != pending.end()) {
                Json safeDetails = *details;
                if (*kindValue == PendingRequestKind::UserInput && safeDetails.is_object()) {
                    if (const auto questions = safeDetails.find("questions"); questions != safeDetails.end()) {
                        Json normalizedQuestions = *questions;
                        if (normalizedQuestions.is_array()) {
                            for (Json& question : normalizedQuestions) {
                                if (question.is_object() && !question.contains("isSecret") && question.contains("secret")) {
                                    question["isSecret"] = question.at("secret");
                                }
                                if (question.is_object()) {
                                    question.erase("secret");
                                }
                            }
                        }
                        expanded["questions"] = std::move(normalizedQuestions);
                        safeDetails.erase(questions);
                    }
                    if (const auto resolution = safeDetails.find("autoResolutionMs"); resolution != safeDetails.end()) {
                        expanded["autoResolutionMs"] = *resolution;
                        safeDetails.erase(resolution);
                    }
                }
                expanded["details"] = std::move(safeDetails);
            }
            if (pending.value("connectionInvalidated", false)) {
                expanded["connectionInvalidated"] = true;
            }
            return expanded;
        }

        void preserveLegacyPendingQuestionClassifications(Json& state) {
            if (!state.is_object()) {
                return;
            }
            const auto pendingRequests = state.find("pendingRequests");
            if (pendingRequests == state.end() || !pendingRequests->is_array()) {
                return;
            }
            for (Json& pending : *pendingRequests) {
                if (!pending.is_object() || optionalString(pending, "type") != std::optional<std::string>{"user_input"}) {
                    continue;
                }
                const auto details = pending.find("details");
                if (details == pending.end() || !details->is_object()) {
                    continue;
                }
                const auto questions = details->find("questions");
                if (questions == details->end() || !questions->is_array()) {
                    continue;
                }
                for (Json& question : *questions) {
                    if (!question.is_object() || question.contains("isSecret")) {
                        continue;
                    }
                    const auto classification = question.find("secret");
                    if (classification != question.end() && classification->is_boolean()) {
                        // `secret` is the frozen legacy boolean
                        // classification, not secret material. Promote it to
                        // the expanded spelling before the recursive legacy
                        // compatibility scrub removes secret-named members.
                        question["isSecret"] = *classification;
                    }
                }
            }
        }

        void scrubLegacyCompatibilityDetail(Json& value, std::size_t depth = 0) {
            if (depth > SafeDetail::HardMaximumDepth) {
                value = nullptr;
                return;
            }
            if (value.is_object()) {
                for (auto member = value.begin(); member != value.end();) {
                    const bool safeClassification = normalizedKey(member.key()) == "issecret" && member.value().is_boolean();
                    if (!safeClassification && SafeDetail::isSecretKey(member.key())) {
                        member = value.erase(member);
                    } else {
                        scrubLegacyCompatibilityDetail(member.value(), depth + 1);
                        ++member;
                    }
                }
            } else if (value.is_array()) {
                for (Json& member : value) {
                    scrubLegacyCompatibilityDetail(member, depth + 1);
                }
            }
        }

        SafeDetail safeLegacyCompatibilityDetail(Json value, const std::string& path) {
            scrubLegacyCompatibilityDetail(value);
            return safeDetail(std::move(value), path);
        }

        template <typename KnownNames>
        Json legacyUnknownMembers(const Json& value, const KnownNames& known) {
            Json result = Json::object();
            if (!value.is_object()) {
                return result;
            }
            for (auto member = value.begin(); member != value.end(); ++member) {
                const bool recognized = std::ranges::any_of(known, [&](std::string_view name) {
                    return member.key() == name;
                });
                const bool safeClassification = normalizedKey(member.key()) == "issecret" && member.value().is_boolean();
                if (!recognized && (safeClassification || !SafeDetail::isSecretKey(member.key()))) {
                    result[member.key()] = member.value();
                }
            }
            scrubLegacyCompatibilityDetail(result);
            return result;
        }

        Json legacyUnknownMembers(const Json& value, std::initializer_list<std::string_view> known) {
            return legacyUnknownMembers<std::initializer_list<std::string_view>>(value, known);
        }

        std::vector<LegacyRootExtension> legacyRootExtensions(const Json& legacy) {
            constexpr std::array<std::string_view, 46> known{
                "backendRevision",
                "provider",
                "lifecycle",
                "lastLifecycleError",
                "controller",
                "controllerSessionId",
                "sessions",
                "threadList",
                "threads",
                "items",
                "pendingRequests",
                "domains",
                "accounts",
                "models",
                "configuration",
                "permissionProfiles",
                "reviews",
                "apps",
                "externalAgents",
                "hooks",
                "marketplace",
                "plugins",
                "pluginsAndSkills",
                "skills",
                "mcp",
                "windowsSandbox",
                "platform",
                "remoteControl",
                "integrations",
                "processes",
                "filesystemWatches",
                "fuzzySearches",
                "fuzzySearchSessions",
                "notices",
                "activities",
                "capacity",
                "diagnostics",
                "codexExtensions",
                "omittedCodexExtensions",
                "journal",
                "sequenceExhausted",
                "frontendSequenceExhausted",
                "authorization",
                "authentication",
                "credential",
                "credentials",
            };
            Json unknown = legacyUnknownMembers(legacy, known);
            std::vector<LegacyRootExtension> result;
            result.reserve(unknown.size());
            for (auto member = unknown.begin(); member != unknown.end(); ++member) {
                result.push_back({member.key(), safeLegacyCompatibilityDetail(member.value(), "/state/" + member.key())});
            }
            return result;
        }

        LegacyItemCompatibility legacyItemCompatibility(const Json& item,
                                                         const std::optional<std::string>& parentThreadId,
                                                         const std::optional<std::string>& parentTurnId,
                                                         std::size_t sourceIndex,
                                                         std::string path) {
            const auto discriminator = optionalString(item, "type");
            ItemData data(requiredIdentity<ItemIdentity>(item, "id", path),
                          optionalIdentity<ThreadIdentity>(optionalString(item, "threadId").has_value()
                                                               ? optionalString(item, "threadId")
                                                               : parentThreadId,
                                                           path + "/threadId"),
                          optionalIdentity<TurnIdentity>(optionalString(item, "turnId").has_value()
                                                             ? optionalString(item, "turnId")
                                                             : parentTurnId,
                                                         path + "/turnId"));
            data.status = optionalString(item, "status");
            data.summary = optionalString(item, "summary");
            if (const auto location = item.find("location"); location != item.end()) {
                data.location = safeLegacyCompatibilityDetail(*location, path + "/location");
            }
            data.agentText = optionalString(item, "agentText");
            data.reasoningText = optionalString(item, "reasoningText");
            data.reasoningSummary = optionalString(item, "reasoningSummary");
            data.commandOutput = optionalString(item, "commandOutput");
            data.droppedContentBytes = optionalUnsigned(item, "droppedContentBytes");
            data.contentTruncated = item.value("contentTruncated", false);
            data.startedAtMs = optionalSigned(item, "startedAtMs");
            data.completedAtMs = optionalSigned(item, "completedAtMs");
            data.truncation.truncated = item.value("truncated", false);
            if (const auto omitted = item.find("omittedFields"); omitted != item.end()) {
                if (!omitted->is_array()) {
                    fail(ModelErrorCode::InvalidShape, path + "/omittedFields", "legacy item omitted fields must be an array");
                }
                for (std::size_t index = 0; index < omitted->size(); ++index) {
                    if (!omitted->at(index).is_string()) {
                        fail(ModelErrorCode::InvalidShape,
                             path + "/omittedFields/" + std::to_string(index),
                             "legacy item omitted field must be a string");
                    }
                    data.truncation.omittedPaths.push_back(omitted->at(index).get<std::string>());
                }
            }
            data.connectionInvalidated = item.value("connectionInvalidated", false);
            if (const auto stamp = item.find("stamp"); stamp != item.end()) {
                Json sanitizedStamp = *stamp;
                scrubLegacyCompatibilityDetail(sanitizedStamp);
                const SourceMetadata decoded = decodeSourceMetadata(sanitizedStamp, path + "/stamp");
                data.generation = decoded.generation;
                data.freshness = decoded.freshness;
                data.stampExtensions = decoded.extensions;
            } else if (const auto generation = optionalUnsigned(item, "generation"); generation.has_value()) {
                data.generation = *generation;
                if (const auto freshness = optionalString(item, "freshness"); freshness.has_value()) {
                    data.freshness = modelFreshness(stateFreshnessFromString(*freshness));
                }
            }
            data.sourceIndex = sourceIndex;
            if (const auto details = item.find("data"); details != item.end()) {
                data.safeDetails = safeLegacyCompatibilityDetail(*details, path + "/data");
            }
            if (const auto extensions = item.find("extensions"); extensions != item.end()) {
                data.legacyExtensions = safeLegacyCompatibilityDetail(*extensions, path + "/extensions");
            }
            data.extensions = safeLegacyCompatibilityDetail(legacyUnknownMembers(item,
                                                                                 {"id",
                                                                                  "type",
                                                                                  "kind",
                                                                                  "threadId",
                                                                                  "turnId",
                                                                                  "status",
                                                                                  "summary",
                                                                                  "location",
                                                                                  "agentText",
                                                                                  "reasoningText",
                                                                                  "reasoningSummary",
                                                                                  "commandOutput",
                                                                                  "droppedContentBytes",
                                                                                  "contentTruncated",
                                                                                  "startedAtMs",
                                                                                  "completedAtMs",
                                                                                  "data",
                                                                                  "truncated",
                                                                                  "omittedFields",
                                                                                  "connectionInvalidated",
                                                                                  "stamp",
                                                                                  "generation",
                                                                                  "freshness",
                                                                                  "extensions"}),
                                                            path + "/compatibilityExtensions");
            return {std::move(data), discriminator.value_or("unknown"), sourceIndex, std::move(path)};
        }

        LegacyPendingRequestCompatibility
        legacyPendingCompatibility(const Json& pending, std::size_t sourceIndex, std::string path) {
            PendingRequestData data(requiredIdentity<PendingRequestIdentity>(pending, "id", path));
            data.threadId = optionalIdentity<ThreadIdentity>(optionalString(pending, "threadId"), path + "/threadId");
            data.turnId = optionalIdentity<TurnIdentity>(optionalString(pending, "turnId"), path + "/turnId");
            data.itemId = optionalIdentity<ItemIdentity>(optionalString(pending, "itemId"), path + "/itemId");
            data.sourceIndex = sourceIndex;
            if (const auto details = pending.find("details"); details != pending.end()) {
                data.safeDetails = safeLegacyCompatibilityDetail(*details, path + "/details");
            }
            return {std::move(data), sourceIndex, std::move(path)};
        }

        Json expandedStateFromLegacy(const Json& legacy,
                                     std::vector<LegacyItemCompatibility>* legacyItems = nullptr,
                                     std::vector<LegacyPendingRequestCompatibility>* legacyPending = nullptr) {
            if (!legacy.is_object()) {
                fail(ModelErrorCode::InvalidShape, "/state", "legacy snapshot state must be an object");
            }
            if (legacy.size() > LegacySnapshotMaximumObjectMembers) {
                fail(ModelErrorCode::UnsafeDetail, "/state", "legacy snapshot state exceeds its object-member limit");
            }
            if (const auto controller = legacy.find("controller"); controller != legacy.end()) {
                if (!controller->is_string() && !controller->is_number_unsigned() && !controller->is_object()) {
                    fail(ModelErrorCode::InvalidShape,
                         "/state/controller",
                         "legacy snapshot controller must be a string, unsigned integer, or object");
                }
                if (controller->is_string() && controller->get_ref<const std::string&>().empty()) {
                    fail(ModelErrorCode::InvalidIdentifier, "/state/controller", "legacy snapshot controller must not be empty");
                }
                if (controller->is_object()) {
                    if (const auto present = controller->find("present"); present != controller->end() && !present->is_boolean()) {
                        fail(ModelErrorCode::InvalidShape, "/state/controller/present", "controller present must be a boolean");
                    }
                    if (const auto session = controller->find("controllerSessionId");
                        session != controller->end() && !session->is_string()) {
                        fail(ModelErrorCode::InvalidShape,
                             "/state/controller/controllerSessionId",
                             "controller session identity must be a string");
                    }
                }
            }
            constexpr std::array<std::string_view, 16> legacyDomainNames{"accounts",
                                                                         "models",
                                                                         "configuration",
                                                                         "permissionProfiles",
                                                                         "reviews",
                                                                         "apps",
                                                                         "externalAgents",
                                                                         "hooks",
                                                                         "marketplace",
                                                                         "plugins",
                                                                         "pluginsAndSkills",
                                                                         "skills",
                                                                         "mcp",
                                                                         "windowsSandbox",
                                                                         "platform",
                                                                         "integrations"};
            const auto validateLegacyDomains = [&](const Json& domains, std::string_view path) {
                for (std::string_view name : legacyDomainNames) {
                    const auto value = domains.find(std::string(name));
                    if (value != domains.end() && !value->is_object()) {
                        fail(ModelErrorCode::InvalidShape,
                             std::string(path) + "/" + std::string(name),
                             "legacy snapshot domain must be an object");
                    }
                }
            };
            validateLegacyDomains(legacy, "/state");
            const Json* nestedDomains = nullptr;
            if (const auto domains = legacy.find("domains"); domains != legacy.end()) {
                if (!domains->is_object()) {
                    fail(ModelErrorCode::InvalidShape, "/state/domains", "legacy snapshot domains must be an object");
                }
                validateLegacyDomains(*domains, "/state/domains");
                nestedDomains = &*domains;
            }
            const auto validateLegacyCollection = [&legacy](std::string_view name) {
                const auto value = legacy.find(std::string(name));
                if (value == legacy.end()) {
                    return;
                }
                if (value->is_array()) {
                    return;
                }
                if (!value->is_object()) {
                    fail(ModelErrorCode::InvalidShape,
                         "/state/" + std::string(name),
                         "legacy snapshot collection must be an array or object");
                }
                const auto entries = value->find("entries");
                if (entries == value->end() || !entries->is_array()) {
                    fail(ModelErrorCode::InvalidShape,
                         "/state/" + std::string(name) + "/entries",
                         "legacy snapshot collection must contain an entries array");
                }
                if (const auto truncation = value->find("truncation"); truncation != value->end() && !truncation->is_object()) {
                    fail(ModelErrorCode::InvalidShape,
                         "/state/" + std::string(name) + "/truncation",
                         "legacy snapshot collection truncation must be an object");
                }
            };
            for (std::string_view name :
                 {"processes", "filesystemWatches", "fuzzySearches", "fuzzySearchSessions", "notices", "activities"}) {
                validateLegacyCollection(name);
            }
            if (const auto sessions = legacy.find("sessions"); sessions == legacy.end() || !sessions->is_array()) {
                fail(ModelErrorCode::InvalidShape, "/state/sessions", "legacy snapshot sessions must be an array");
            }
            if (const auto threadList = legacy.find("threadList"); threadList == legacy.end() || !threadList->is_object()) {
                fail(ModelErrorCode::InvalidShape, "/state/threadList", "legacy snapshot thread list must be an object");
            }
            if (const auto threads = legacy.find("threads"); threads == legacy.end() || !threads->is_array()) {
                fail(ModelErrorCode::InvalidShape, "/state/threads", "legacy snapshot threads must be an array");
            }
            if (const auto items = legacy.find("items"); items != legacy.end() && !items->is_array()) {
                fail(ModelErrorCode::InvalidShape, "/state/items", "legacy complete items must be an array");
            }
            if (const auto requests = legacy.find("pendingRequests"); requests == legacy.end() || !requests->is_array()) {
                fail(ModelErrorCode::InvalidShape, "/state/pendingRequests", "legacy pending requests must be an array");
            }
            if (const auto capacity = legacy.find("capacity"); capacity != legacy.end()) {
                if (!capacity->is_object()) {
                    fail(ModelErrorCode::InvalidShape, "/state/capacity", "legacy capacity must be an object");
                }
                constexpr std::array<std::string_view, 20> knownCapacityMembers{
                    "sessions",
                    "observers",
                    "activeOperations",
                    "pendingRequests",
                    "retainedThreads",
                    "retainedTurns",
                    "retainedItems",
                    "accumulatedContentBytes",
                    "retainedNotices",
                    "retainedProcesses",
                    "accumulatedProcessOutputBytes",
                    "retainedFilesystemWatches",
                    "retainedFuzzySearchSessions",
                    "retainedActivityRecords",
                    "evictedNotices",
                    "evictedProcesses",
                    "droppedProcessOutputBytes",
                    "evictedFilesystemWatches",
                    "evictedFuzzySearchSessions",
                    "evictedActivityRecords",
                };
                for (std::string_view name : knownCapacityMembers) {
                    const auto member = capacity->find(std::string(name));
                    if (member != capacity->end() && !member->is_number_unsigned()) {
                        fail(ModelErrorCode::InvalidShape,
                             "/state/capacity/" + std::string(name),
                             "legacy capacity values must be unsigned integers");
                    }
                }
            }
            if (const auto diagnostics = legacy.find("diagnostics"); diagnostics != legacy.end()) {
                if (!diagnostics->is_object()) {
                    fail(ModelErrorCode::InvalidShape, "/state/diagnostics", "legacy diagnostics must be an object");
                }
                if (const auto recent = diagnostics->find("recent"); recent != diagnostics->end()) {
                    if (!recent->is_array() || std::ranges::any_of(*recent, [](const Json& value) {
                            return !value.is_string();
                        })) {
                        fail(ModelErrorCode::InvalidShape, "/state/diagnostics/recent", "legacy diagnostics recent must contain strings");
                    }
                }
            }
            const auto lifecycleName = optionalString(legacy, "lifecycle");
            if (!lifecycleName.has_value() || !providerLifecycleFromString(*lifecycleName).has_value()) {
                fail(ModelErrorCode::InvalidShape, "/state/lifecycle", "legacy lifecycle is invalid");
            }
            Json state = Json::object();
            state["provider"] = Json{{"lifecycle", *lifecycleName},
                                     {"generation", 0},
                                     {"desiredRunning", false},
                                     {"recovery", Json{{"status", "idle"}, {"attempts", 0}}}};
            if (const auto error = legacy.find("lastLifecycleError"); error != legacy.end()) {
                state["provider"]["lastError"] = *error;
            }
            state["controller"] = Json{{"present", false}};
            if (const auto controller = optionalString(legacy, "controllerSessionId"); controller.has_value()) {
                state["controller"] = Json{{"present", true}, {"controllerSessionId", *controller}};
            } else if (const auto controller = legacy.find("controller"); controller != legacy.end()) {
                if (controller->is_string()) {
                    state["controller"] = Json{{"present", true}, {"controllerSessionId", controller->get<std::string>()}};
                } else if (controller->is_number_unsigned()) {
                    state["controller"] =
                        Json{{"present", true}, {"controllerSessionId", std::to_string(controller->get<std::uint64_t>())}};
                } else if (controller->is_object()) {
                    state["controller"] = *controller;
                }
            }
            state["sessions"] = legacy.value("sessions", Json::array());
            state["threadList"] = legacy.value("threadList", Json{{"hasLoadedPage", false}, {"complete", false}, {"pagesLoaded", 0}});
            state["threads"] = Json::array();
            state["turns"] = Json::array();
            state["items"] = Json::array();
            std::size_t legacyItemSourceIndex = 0;
            if (const auto threads = legacy.find("threads"); threads != legacy.end() && threads->is_array()) {
                for (Json thread : *threads) {
                    const auto threadId = optionalString(thread, "id");
                    if (const auto turns = thread.find("turns"); turns != thread.end() && turns->is_array()) {
                        for (Json turn : *turns) {
                            const auto turnId = optionalString(turn, "id");
                            if (const auto items = turn.find("items"); items != turn.end() && items->is_array()) {
                                for (std::size_t itemIndex = 0; itemIndex < items->size(); ++itemIndex) {
                                    Json item = items->at(itemIndex);
                                    const Json data = item.value("data", Json::object());
                                    const auto discriminator = optionalString(item, "type");
                                    if (discriminator.has_value() && !expandedItemType(*discriminator, data).has_value() && legacyItems != nullptr) {
                                        legacyItems->push_back(legacyItemCompatibility(
                                            item,
                                            threadId,
                                            turnId,
                                            legacyItemSourceIndex,
                                            "/threads/" + std::to_string(state["threads"].size()) + "/turns/" +
                                                std::to_string(state["turns"].size()) + "/items/" + std::to_string(itemIndex)));
                                        ++legacyItemSourceIndex;
                                        continue;
                                    }
                                    state["items"].push_back(normalizeLegacyItem(item, threadId, turnId));
                                    ++legacyItemSourceIndex;
                                }
                            }
                            turn.erase("items");
                            turn.erase("extensions");
                            state["turns"].push_back(std::move(turn));
                        }
                    }
                    thread.erase("turns");
                    thread.erase("extensions");
                    state["threads"].push_back(std::move(thread));
                }
            }
            if (const auto completeItems = legacy.find("items"); completeItems != legacy.end() && completeItems->is_array()) {
                for (std::size_t itemIndex = 0; itemIndex < completeItems->size(); ++itemIndex) {
                    Json item = completeItems->at(itemIndex);
                    const Json data = item.value("data", Json::object());
                    const auto discriminator = optionalString(item, "type");
                    if (discriminator.has_value() && !expandedItemType(*discriminator, data).has_value() && legacyItems != nullptr) {
                        LegacyItemCompatibility decoded = legacyItemCompatibility(
                            item, std::nullopt, std::nullopt, legacyItemSourceIndex, "/items/" + std::to_string(itemIndex));
                        const auto prior =
                            std::find_if(legacyItems->begin(), legacyItems->end(), [&](const LegacyItemCompatibility& value) {
                                return value.value.id == decoded.value.id;
                            });
                        if (prior != legacyItems->end()) {
                            decoded.sourceIndex = prior->sourceIndex;
                            decoded.value.sourceIndex = prior->value.sourceIndex;
                            if (!decoded.value.threadId.has_value()) {
                                decoded.value.threadId = prior->value.threadId;
                            }
                            if (!decoded.value.turnId.has_value()) {
                                decoded.value.turnId = prior->value.turnId;
                            }
                            *prior = std::move(decoded);
                        } else {
                            legacyItems->push_back(std::move(decoded));
                            ++legacyItemSourceIndex;
                        }
                        continue;
                    }
                    Json normalized = normalizeLegacyItem(std::move(item), std::nullopt, std::nullopt);
                    const auto identity = optionalString(normalized, "id");
                    const auto prior = identity.has_value() ? std::find_if(state["items"].begin(),
                                                                           state["items"].end(),
                                                                           [&](const Json& value) {
                                                                               return optionalString(value, "id") == identity;
                                                                           })
                                                            : state["items"].end();
                    if (prior != state["items"].end()) {
                        if (!normalized.contains("threadId") && prior->contains("threadId")) {
                            normalized["threadId"] = prior->at("threadId");
                        }
                        if (!normalized.contains("turnId") && prior->contains("turnId")) {
                            normalized["turnId"] = prior->at("turnId");
                        }
                        *prior = std::move(normalized);
                    } else {
                        state["items"].push_back(std::move(normalized));
                        ++legacyItemSourceIndex;
                    }
                }
            }
            state["pendingRequests"] = Json::array();
            if (const auto pending = legacy.find("pendingRequests"); pending != legacy.end() && pending->is_array()) {
                for (std::size_t pendingIndex = 0; pendingIndex < pending->size(); ++pendingIndex) {
                    const Json& request = pending->at(pendingIndex);
                    if (request.contains("pendingRequestId")) {
                        state["pendingRequests"].push_back(normalizeLegacyPending(request));
                        continue;
                    }
                    const std::string type = optionalString(request, "type").value_or("unknown");
                    if (!pendingRequestKindFromString(type).has_value() && type != "command_approval" &&
                        type != "file_change_approval" && type != "user_input" && type != "authentication" &&
                        legacyPending != nullptr) {
                        legacyPending->push_back(legacyPendingCompatibility(
                            request, pendingIndex, "/pendingRequests/" + std::to_string(pendingIndex)));
                        continue;
                    }
                    state["pendingRequests"].push_back(normalizeLegacyPending(request));
                }
            }

            for (std::string_view name : legacyDomainNames) {
                const auto topLevel = legacy.find(name);
                if (topLevel != legacy.end()) {
                    state[std::string(name)] = *topLevel;
                } else if (nestedDomains != nullptr) {
                    const auto nested = nestedDomains->find(name);
                    if (nested != nestedDomains->end()) {
                        state[std::string(name)] = *nested;
                    }
                }
            }
            if (const auto domain = legacy.find("remoteControl"); domain != legacy.end()) {
                if (!domain->is_object()) {
                    fail(ModelErrorCode::InvalidShape, "/state/remoteControl", "legacy snapshot domain must be an object");
                }
                state["remoteControl"] = *domain;
            }
            if (!state.contains("plugins") && legacy.contains("pluginsAndSkills")) {
                state["plugins"] = legacy.at("pluginsAndSkills");
            }
            for (std::string_view name : {"processes", "filesystemWatches", "fuzzySearches", "notices", "activities", "capacity"}) {
                if (const auto value = legacy.find(name); value != legacy.end()) {
                    state[std::string(name)] = *value;
                }
            }
            if (!state.contains("fuzzySearches")) {
                if (const auto value = legacy.find("fuzzySearchSessions"); value != legacy.end()) {
                    state["fuzzySearches"] = *value;
                }
            }
            if (!state.contains("capacity")) {
                state["capacity"] = Json::object();
            }
            const std::uint64_t omitted = optionalUnsigned(legacy, "omittedCodexExtensions").value_or(0);
            state["truncation"] = Json{{"truncated", omitted != 0}, {"omittedEntries", omitted}};
            if (const auto diagnostics = legacy.find("diagnostics"); diagnostics != legacy.end() && diagnostics->is_object()) {
                state["diagnostics"] = *diagnostics;
            }
            Json cursor{{"backendRevision", optionalUnsigned(legacy, "backendRevision").value_or(0)}};
            if (const auto journal = legacy.find("journal"); journal != legacy.end() && journal->is_object()) {
                for (std::string_view key :
                     {"oldestReplayableAfter", "currentSequence", "oldestRetainedSequence", "newestRetainedSequence"}) {
                    if (const auto value = journal->find(key); value != journal->end()) {
                        cursor[std::string(key)] = *value;
                    }
                }
            }
            cursor["backendSequenceExhausted"] = legacy.value("sequenceExhausted", false);
            if (legacy.contains("frontendSequenceExhausted")) {
                cursor["frontendSequenceExhausted"] = legacy.at("frontendSequenceExhausted");
            }
            state["backendCursor"] = std::move(cursor);
            if (legacy.contains("codexExtensions")) {
                state["codexExtensions"] = legacy.at("codexExtensions");
            }
            return state;
        }

        ModelResult<CanonicalSnapshot> decodeExpandedState(const Snapshot& snapshot, Json state) noexcept {
            try {
                Json encoded{{"protocol", ProtocolIdentity},
                             {"version", ProtocolVersion},
                             {"kind", kind::Snapshot},
                             {"sequence", snapshot.sequence.value()},
                             {"state", std::move(state)}};
                if (!snapshot.extensions.is_object()) {
                    fail(ModelErrorCode::InvalidShape, "/extensions", "snapshot extensions must be an object");
                }
                for (auto extension = snapshot.extensions.begin(); extension != snapshot.extensions.end(); ++extension) {
                    if (!encoded.contains(extension.key())) {
                        encoded[extension.key()] = extension.value();
                    }
                }
                const auto expanded = Codec::decodeExpandedSnapshot(encoded);
                if (!expanded) {
                    return ModelError{ModelErrorCode::InvalidShape, "/state", expanded.error().message};
                }
                return decodeSnapshot(expanded.value());
            } catch (const ModelFailure& failure) {
                return failure.error;
            } catch (const std::exception& error) {
                return ModelError{ModelErrorCode::InvalidShape, "/state", error.what()};
            }
        }

        void restoreLegacyRepresentation(const Json& legacy, CanonicalSnapshot& snapshot) {
            const auto mutableItemData = [](ThreadItem& item) -> ItemData& {
                return std::visit(
                    [](auto& value) -> ItemData& {
                        return value.value;
                    },
                    item);
            };
            const auto restoreItem =
                [&](const Json& encoded,
                    const std::optional<std::string>& threadId,
                    const std::optional<std::string>& turnId,
                    std::optional<std::size_t> sourceIndex) {
                    const auto identity = optionalString(encoded, "id");
                    if (!identity.has_value()) {
                        return;
                    }
                    const auto found = std::find_if(snapshot.items.begin(), snapshot.items.end(), [&](const ThreadItem& item) {
                        const ItemData& data = itemData(item);
                        return data.id.value() == *identity &&
                               (!threadId.has_value() || (data.threadId.has_value() && data.threadId->value() == *threadId)) &&
                               (!turnId.has_value() || (data.turnId.has_value() && data.turnId->value() == *turnId));
                    });
                    if (found == snapshot.items.end()) {
                        return;
                    }
                    ItemData& data = mutableItemData(*found);
                    if (sourceIndex.has_value()) {
                        data.sourceIndex = *sourceIndex;
                    }
                    data.legacyDiscriminator = optionalString(encoded, "type");
                    if (const auto extensions = encoded.find("extensions"); extensions != encoded.end()) {
                        data.legacyExtensions = safeLegacyCompatibilityDetail(*extensions, "/state/items/extensions");
                    }
                };

            std::size_t nextSourceIndex = 0;
            std::vector<std::string> nestedItemIds;
            if (const auto threads = legacy.find("threads"); threads != legacy.end() && threads->is_array()) {
                for (const Json& encodedThread : *threads) {
                    const auto threadId = optionalString(encodedThread, "id");
                    if (!threadId.has_value()) {
                        continue;
                    }
                    const auto thread = std::find_if(snapshot.threads.begin(), snapshot.threads.end(), [&](const ThreadState& value) {
                        return value.id.value() == *threadId;
                    });
                    if (thread != snapshot.threads.end()) {
                        if (const auto extensions = encodedThread.find("extensions"); extensions != encodedThread.end()) {
                            thread->legacyExtensions = safeLegacyCompatibilityDetail(*extensions, "/state/threads/extensions");
                        }
                    }
                    const auto turns = encodedThread.find("turns");
                    if (turns == encodedThread.end() || !turns->is_array()) {
                        continue;
                    }
                    for (const Json& encodedTurn : *turns) {
                        const auto turnId = optionalString(encodedTurn, "id");
                        if (!turnId.has_value()) {
                            continue;
                        }
                        const auto turn = std::find_if(snapshot.turns.begin(), snapshot.turns.end(), [&](const TurnState& value) {
                            return value.id.value() == *turnId && value.threadId.value() == *threadId;
                        });
                        if (turn != snapshot.turns.end()) {
                            if (const auto extensions = encodedTurn.find("extensions"); extensions != encodedTurn.end()) {
                                turn->legacyExtensions = safeLegacyCompatibilityDetail(*extensions, "/state/turns/extensions");
                            }
                        }
                        if (const auto items = encodedTurn.find("items"); items != encodedTurn.end() && items->is_array()) {
                            for (const Json& encodedItem : *items) {
                                if (const auto identity = optionalString(encodedItem, "id"); identity.has_value()) {
                                    nestedItemIds.push_back(*identity);
                                }
                                restoreItem(encodedItem, threadId, turnId, nextSourceIndex);
                                ++nextSourceIndex;
                            }
                        }
                    }
                }
            }
            if (const auto items = legacy.find("items"); items != legacy.end() && items->is_array()) {
                for (const Json& encodedItem : *items) {
                    const auto identity = optionalString(encodedItem, "id");
                    const bool alreadyOrdered =
                        identity.has_value() && std::find(nestedItemIds.begin(), nestedItemIds.end(), *identity) != nestedItemIds.end();
                    restoreItem(encodedItem,
                                std::nullopt,
                                std::nullopt,
                                alreadyOrdered ? std::nullopt : std::optional<std::size_t>{nextSourceIndex});
                    if (!alreadyOrdered) {
                        ++nextSourceIndex;
                    }
                }
            }
        }
    } // namespace

    ModelResult<Snapshot> encodeLegacySnapshot(const CanonicalSnapshot& snapshot) noexcept {
        try {
            return Snapshot{snapshot.sequence.protocolValue(), legacySnapshotState(snapshot), snapshot.extensions.json()};
        } catch (const ModelFailure& failure) {
            return failure.error;
        } catch (const std::exception& error) {
            return ModelError{ModelErrorCode::InvalidShape, "/state", error.what()};
        } catch (...) {
            return ModelError{ModelErrorCode::InvalidShape, "/state", "legacy snapshot encoding failed"};
        }
    }

    ModelResult<CanonicalSnapshot> decodeLegacySnapshot(const Snapshot& snapshot) noexcept {
        try {
            Json sanitizedLegacy = snapshot.state;
            preserveLegacyPendingQuestionClassifications(sanitizedLegacy);
            scrubLegacyCompatibilityDetail(sanitizedLegacy);
            for (std::string_view requiredRoot : {"sessions", "threadList", "threads", "pendingRequests"}) {
                if (!sanitizedLegacy.contains(requiredRoot)) {
                    return ModelError{ModelErrorCode::InvalidShape,
                                      "/state/" + std::string(requiredRoot),
                                      "legacy snapshot is missing a required state root"};
                }
            }
            std::vector<LegacyItemCompatibility> legacyItems;
            std::vector<LegacyPendingRequestCompatibility> legacyPending;
            auto decoded = decodeExpandedState(snapshot, expandedStateFromLegacy(sanitizedLegacy, &legacyItems, &legacyPending));
            if (!decoded) {
                return decoded.error();
            }
            CanonicalSnapshot value = std::move(decoded).value();
            value.sessionsPresent = sanitizedLegacy.contains("sessions");
            value.threadListPresent = sanitizedLegacy.contains("threadList");
            value.threadsPresent = sanitizedLegacy.contains("threads");
            value.turnsPresent = value.threadsPresent;
            value.itemsPresent = value.threadsPresent || sanitizedLegacy.contains("items");
            value.pendingRequestsPresent = sanitizedLegacy.contains("pendingRequests");
            value.capacityPresent = sanitizedLegacy.contains("capacity");
            value.legacyItems = std::move(legacyItems);
            value.legacyPendingRequests = std::move(legacyPending);
            value.legacyRootExtensions = legacyRootExtensions(sanitizedLegacy);
            restoreLegacyRepresentation(sanitizedLegacy, value);
            return value;
        } catch (const ModelFailure& failure) {
            return failure.error;
        } catch (const std::exception& error) {
            return ModelError{ModelErrorCode::InvalidShape, "/state", error.what()};
        } catch (...) {
            return ModelError{ModelErrorCode::InvalidShape, "/state", "legacy snapshot decoding failed"};
        }
    }

    SnapshotRepresentationSelection snapshotRepresentationSelection(std::span<const FrontendCapability> selectedCapabilities) noexcept {
        const auto selected = [&](FrontendCapability capability) {
            return std::find(selectedCapabilities.begin(), selectedCapabilities.end(), capability) != selectedCapabilities.end();
        };
        return {selected(FrontendCapability::CompleteBackendDomains),
                selected(FrontendCapability::CompleteThreadItems),
                selected(FrontendCapability::DedicatedPendingRequests),
                selected(FrontendCapability::ScopeProjectedState)};
    }

    ModelResult<Snapshot> encodeProjectedSnapshot(const CanonicalSnapshot& snapshot, SnapshotRepresentationSelection selection) noexcept {
        try {
            auto legacy = encodeLegacySnapshot(snapshot);
            CanonicalSnapshot represented = snapshot;
            const auto accountOmission = [](TruncationMetadata& truncation, const std::string& path) {
                truncation.truncated = true;
                const std::size_t current = truncation.omittedEntries.value_or(0);
                truncation.omittedEntries = current == std::numeric_limits<std::size_t>::max() ? current : current + 1;
                truncation.omittedPaths.push_back(path);
            };
            if (selection.expandedItems) {
                for (const LegacyItemCompatibility& item : represented.legacyItems) {
                    accountOmission(represented.truncation, item.omissionPath);
                }
            }
            if (selection.expandedPendingRequests) {
                for (const LegacyPendingRequestCompatibility& request : represented.legacyPendingRequests) {
                    accountOmission(represented.truncation, request.omissionPath);
                }
            }
            auto expanded = encodeSnapshot(represented);
            if (!legacy) {
                return legacy.error();
            }
            if (!expanded) {
                return expanded.error();
            }
            const auto expandedWire = Codec::encodeExpandedSnapshot(expanded.value());
            if (!expandedWire) {
                return ModelError{
                    ModelErrorCode::InvalidShape, "/state", "expanded snapshot encoding failed: " + expandedWire.error().message};
            }
            if (!expandedWire.value().contains("state")) {
                return ModelError{ModelErrorCode::InvalidShape, "/state", "expanded snapshot encoding omitted state"};
            }
            const Json& expandedState = expandedWire.value().at("state");
            Json state = selection.expandedDomains ? expandedState : legacy.value().state;
            if (selection.expandedItems) {
                state["items"] = expandedState.value("items", Json::array());
            } else if (selection.expandedDomains) {
                std::vector<std::pair<std::size_t, Json>> orderedItems;
                Json knownItems = metadataCompatibleItems(expandedState.value("items", Json::array()));
                std::size_t fallbackIndex = 0;
                for (Json& item : knownItems) {
                    std::size_t index = fallbackIndex++;
                    const auto id = optionalString(item, "id");
                    if (id.has_value()) {
                        const auto known = std::find_if(snapshot.items.begin(), snapshot.items.end(), [&](const ThreadItem& value) {
                            return itemData(value).id.value() == *id;
                        });
                        if (known != snapshot.items.end()) {
                            index = itemData(*known).sourceIndex.value_or(index);
                        }
                    }
                    orderedItems.emplace_back(index, std::move(item));
                }
                for (const LegacyItemCompatibility& item : snapshot.legacyItems) {
                    orderedItems.emplace_back(item.sourceIndex, metadataCompatibleItem(item));
                }
                std::stable_sort(orderedItems.begin(), orderedItems.end(), [](const auto& left, const auto& right) {
                    return left.first < right.first;
                });
                state["items"] = Json::array();
                for (auto& [index, item] : orderedItems) {
                    (void) index;
                    state["items"].push_back(std::move(item));
                }
            }
            if (selection.expandedPendingRequests) {
                state["pendingRequests"] = expandedState.value("pendingRequests", Json::array());
            } else if (selection.expandedDomains) {
                Json pending = legacy.value().state.value("pendingRequests", Json::array());
                if (pending.empty()) {
                    state.erase("pendingRequests");
                } else {
                    state["pendingRequests"] = std::move(pending);
                }
            }
            Json extensions = snapshot.extensions.json();
            if (selection.includeProjectionMetadata) {
                extensions["scopeProjection"] =
                    Json{{"omittedFields", snapshot.projection.omittedPaths}, {"redactedFields", snapshot.projection.redactedPaths}};
            } else {
                extensions.erase("scopeProjection");
            }
            return Snapshot{snapshot.sequence.protocolValue(), std::move(state), std::move(extensions)};
        } catch (const std::exception& error) {
            return ModelError{ModelErrorCode::InvalidShape, "/state", error.what()};
        } catch (...) {
            return ModelError{ModelErrorCode::InvalidShape, "/state", "projected snapshot encoding failed"};
        }
    }

    ModelResult<Snapshot> encodeProjectedSnapshot(const CanonicalSnapshot& snapshot,
                                                  std::span<const FrontendCapability> selectedCapabilities) noexcept {
        return encodeProjectedSnapshot(snapshot, snapshotRepresentationSelection(selectedCapabilities));
    }

    ModelResult<CanonicalSnapshot> decodeProjectedSnapshot(const Snapshot& snapshot, SnapshotRepresentationSelection selection) noexcept {
        try {
            if (!selection.expandedDomains) {
                return decodeLegacySnapshot(snapshot);
            }
            Json normalized = snapshot.state;
            std::vector<LegacyItemCompatibility> legacyItems;
            std::vector<LegacyPendingRequestCompatibility> legacyPending;
            if (!selection.expandedItems) {
                Json items = Json::array();
                std::size_t sourceIndex = 0;
                for (const Json& item : normalized.value("items", Json::array())) {
                    const std::string type = optionalString(item, "type").value_or("unknown");
                    const Json data = item.value("data", Json::object());
                    if (!expandedItemType(type, data).has_value()) {
                        legacyItems.push_back(legacyItemCompatibility(
                            item, std::nullopt, std::nullopt, sourceIndex, "/items/" + std::to_string(sourceIndex)));
                    } else {
                        items.push_back(normalizeLegacyItem(item, std::nullopt, std::nullopt));
                    }
                    ++sourceIndex;
                }
                normalized["items"] = std::move(items);
            }
            if (!selection.expandedPendingRequests) {
                Json pending = Json::array();
                std::size_t sourceIndex = 0;
                for (const Json& request : normalized.value("pendingRequests", Json::array())) {
                    const std::string type = optionalString(request, "type").value_or("unknown");
                    if (!pendingRequestKindFromString(type).has_value() && type != "command_approval" &&
                        type != "file_change_approval" && type != "user_input" && type != "authentication") {
                        legacyPending.push_back(legacyPendingCompatibility(
                            request, sourceIndex, "/pendingRequests/" + std::to_string(sourceIndex)));
                    } else {
                        pending.push_back(normalizeLegacyPending(request));
                    }
                    ++sourceIndex;
                }
                normalized["pendingRequests"] = std::move(pending);
            }
            auto decoded = decodeExpandedState(snapshot, std::move(normalized));
            if (!decoded) {
                return decoded.error();
            }
            CanonicalSnapshot value = std::move(decoded).value();
            value.legacyItems = std::move(legacyItems);
            value.legacyPendingRequests = std::move(legacyPending);
            return value;
        } catch (const ModelFailure& failure) {
            return failure.error;
        } catch (const std::exception& error) {
            return ModelError{ModelErrorCode::InvalidShape, "/state", error.what()};
        }
    }

    ModelResult<CanonicalSnapshot> decodeProjectedSnapshot(const Snapshot& snapshot,
                                                           std::span<const FrontendCapability> selectedCapabilities) noexcept {
        return decodeProjectedSnapshot(snapshot, snapshotRepresentationSelection(selectedCapabilities));
    }

} // namespace ai::openai::codex::frontend::internal::model
