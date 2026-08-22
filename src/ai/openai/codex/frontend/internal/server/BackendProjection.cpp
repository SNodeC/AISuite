/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/internal/server/BackendProjection.h"

#include "ai/openai/codex/frontend/GeneratedProtocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ai::openai::codex::frontend::internal::server {

    namespace {

        class ProjectionFailure final : public std::runtime_error {
        public:
            ProjectionFailure(std::string path, std::string message)
                : std::runtime_error(std::move(message))
                , failurePath(std::move(path)) {
            }

            [[nodiscard]] const std::string& path() const noexcept {
                return failurePath;
            }

        private:
            std::string failurePath;
        };

        template <typename Strong>
        Strong requiredIdentifier(std::string value, std::string path) {
            std::optional<Strong> parsed = Strong::parse(std::move(value));
            if (!parsed) {
                throw ProjectionFailure(std::move(path), "backend identity is empty, oversized, or contains NUL");
            }
            return std::move(*parsed);
        }

        model::Freshness freshness(backend::Freshness value) noexcept {
            switch (value) {
                case backend::Freshness::Unknown:
                    return model::Freshness::Unknown;
                case backend::Freshness::Current:
                    return model::Freshness::Current;
                case backend::Freshness::Stale:
                    return model::Freshness::Stale;
            }
            return model::Freshness::Unknown;
        }

        model::SourceMetadata sourceMetadata(const backend::SourceStamp& stamp) {
            model::SourceMetadata result;
            result.generation = stamp.generation;
            result.freshness = freshness(stamp.freshness);
            return result;
        }

        std::size_t utf8PrefixLength(std::string_view value, std::size_t maximumBytes) noexcept {
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

        bool validUtf8(std::string_view value) noexcept {
            for (std::size_t offset = 0; offset < value.size();) {
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
                    return false;
                }
                if (offset + width > value.size()) {
                    return false;
                }
                for (std::size_t index = 1; index < width; ++index) {
                    if ((static_cast<unsigned char>(value[offset + index]) & 0xc0U) != 0x80U) {
                        return false;
                    }
                }
                if (width == 3) {
                    const unsigned char second = static_cast<unsigned char>(value[offset + 1]);
                    if ((lead == 0xe0U && second < 0xa0U) || (lead == 0xedU && second > 0x9fU)) {
                        return false;
                    }
                } else if (width == 4) {
                    const unsigned char second = static_cast<unsigned char>(value[offset + 1]);
                    if ((lead == 0xf0U && second < 0x90U) || (lead == 0xf4U && second > 0x8fU)) {
                        return false;
                    }
                }
                offset += width;
            }
            return true;
        }

        std::size_t utf8CharacterPrefixLength(std::string_view value, std::size_t maximumCharacters) noexcept {
            std::size_t offset = 0;
            std::size_t characters = 0;
            while (offset < value.size() && characters < maximumCharacters) {
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
                if (offset + width > value.size()) {
                    break;
                }
                bool continuationValid = true;
                for (std::size_t index = 1; index < width; ++index) {
                    continuationValid =
                        continuationValid && (static_cast<unsigned char>(value[offset + index]) & 0xc0U) == 0x80U;
                }
                if (!continuationValid) {
                    break;
                }
                if (width == 3) {
                    const unsigned char second = static_cast<unsigned char>(value[offset + 1]);
                    if ((lead == 0xe0U && second < 0xa0U) || (lead == 0xedU && second > 0x9fU)) {
                        break;
                    }
                } else if (width == 4) {
                    const unsigned char second = static_cast<unsigned char>(value[offset + 1]);
                    if ((lead == 0xf0U && second < 0x90U) || (lead == 0xf4U && second > 0x8fU)) {
                        break;
                    }
                }
                offset += width;
                ++characters;
            }
            return offset;
        }

        void recordOmission(model::TruncationMetadata& truncation, std::string path) {
            constexpr std::size_t MaximumOmittedPaths = 64;
            constexpr std::size_t MaximumPathCharacters = 256;
            const std::size_t retained = utf8CharacterPrefixLength(path, MaximumPathCharacters);
            path.resize(retained);
            truncation.truncated = true;
            if (path.empty() || std::find(truncation.omittedPaths.begin(), truncation.omittedPaths.end(), path) !=
                                    truncation.omittedPaths.end()) {
                return;
            }
            if (truncation.omittedPaths.size() < MaximumOmittedPaths) {
                truncation.omittedPaths.push_back(std::move(path));
                return;
            }
            std::size_t omitted = truncation.omittedEntries.value_or(0);
            if (omitted != std::numeric_limits<std::size_t>::max()) {
                ++omitted;
            }
            truncation.omittedEntries = omitted;
        }

        std::string boundedFrontendString(std::string_view value,
                                          std::size_t maximumCharacters,
                                          model::TruncationMetadata* truncation = nullptr,
                                          std::string path = {}) {
            const std::size_t retained = utf8CharacterPrefixLength(value, maximumCharacters);
            if (retained != value.size() && truncation != nullptr) {
                recordOmission(*truncation, std::move(path));
            }
            return std::string(value.substr(0, retained));
        }

        backend::CodexExtensionReceived projectionSafeExtension(const backend::CodexExtensionReceived& extension) {
            if (extension.safeProjection) {
                return extension;
            }
            const backend::ExtensionSnapshot safe = backend::makeExtensionSnapshot(backend::ExtensionRecord{
                extension.method, extension.payload, extension.decodingError, std::nullopt, std::nullopt, std::nullopt});
            backend::CodexExtensionReceived projected;
            projected.method = safe.method;
            projected.payload = safe.payload;
            projected.decodingError = safe.decodingError;
            projected.safeProjection = true;
            projected.methodTruncated = safe.methodTruncated;
            projected.payloadTruncated = safe.payloadTruncated;
            projected.decodingErrorTruncated = safe.decodingErrorTruncated;
            projected.sensitiveFieldsRedacted = safe.sensitiveFieldsRedacted;
            projected.originalMethodBytes = safe.originalMethodBytes;
            projected.originalPayloadBytes = safe.originalPayloadBytes;
            projected.originalDecodingErrorBytes = safe.originalDecodingErrorBytes;
            return projected;
        }

        model::ProviderLifecycle providerLifecycle(backend::ProviderLifecycle value) noexcept {
            switch (value) {
                case backend::ProviderLifecycle::Stopped:
                    return model::ProviderLifecycle::Stopped;
                case backend::ProviderLifecycle::Starting:
                    return model::ProviderLifecycle::Starting;
                case backend::ProviderLifecycle::Initializing:
                    return model::ProviderLifecycle::Initializing;
                case backend::ProviderLifecycle::Ready:
                    return model::ProviderLifecycle::Ready;
                case backend::ProviderLifecycle::Stopping:
                    return model::ProviderLifecycle::Stopping;
                case backend::ProviderLifecycle::Failed:
                    return model::ProviderLifecycle::Failed;
                case backend::ProviderLifecycle::Recovering:
                    return model::ProviderLifecycle::Recovering;
            }
            return model::ProviderLifecycle::Stopped;
        }

        model::ProviderRecoveryStatus recoveryStatus(backend::RecoveryStatus value) noexcept {
            switch (value) {
                case backend::RecoveryStatus::Idle:
                    return model::ProviderRecoveryStatus::Idle;
                case backend::RecoveryStatus::Waiting:
                    return model::ProviderRecoveryStatus::Waiting;
                case backend::RecoveryStatus::Exhausted:
                    return model::ProviderRecoveryStatus::Exhausted;
            }
            return model::ProviderRecoveryStatus::Idle;
        }

        SessionRole sessionRole(backend::SessionRole value) noexcept {
            return value == backend::SessionRole::Controller ? SessionRole::Controller : SessionRole::Observer;
        }

        model::SafeDetail boundedDetail(Json value, model::TruncationMetadata& truncation, std::string path) {
            model::SafeDetailError error = model::SafeDetailError::None;
            std::optional<model::SafeDetail> safe = model::SafeDetail::fromJson(std::move(value), &error);
            if (safe) {
                return std::move(*safe);
            }
            recordOmission(truncation, std::move(path));
            return {};
        }

        Json boundedFrontendDetailObject(const Json& value,
                                         model::TruncationMetadata* truncation = nullptr,
                                         std::string_view path = {}) {
            constexpr std::size_t MaximumMembers = 64;
            constexpr std::size_t MaximumArrayItems = 64;
            constexpr std::size_t MaximumKeyCharacters = 256;
            constexpr std::size_t MaximumStringCharacters = 16U * 1024U;
            const auto scalar = [](const Json& candidate) {
                return candidate.is_null() || candidate.is_boolean() || candidate.is_number() || candidate.is_string();
            };
            const auto boundedScalar = [&](const Json& candidate, std::string memberPath) -> Json {
                if (!candidate.is_string()) {
                    return candidate;
                }
                const std::string& text = candidate.get_ref<const std::string&>();
                return boundedFrontendString(text, MaximumStringCharacters, truncation, std::move(memberPath));
            };

            Json projected = Json::object();
            if (!value.is_object()) {
                if (truncation != nullptr) {
                    recordOmission(*truncation, std::string(path));
                }
                return projected;
            }
            for (auto member = value.begin(); member != value.end(); ++member) {
                if (projected.size() == MaximumMembers) {
                    if (truncation != nullptr) {
                        recordOmission(*truncation, std::string(path));
                    }
                    break;
                }
                if (model::SafeDetail::isSecretKey(member.key())) {
                    if (truncation != nullptr) {
                        recordOmission(*truncation, std::string(path) + "/" + member.key());
                    }
                    continue;
                }
                const std::size_t keyBytes = utf8CharacterPrefixLength(member.key(), MaximumKeyCharacters);
                if (keyBytes == 0 && !member.key().empty()) {
                    if (truncation != nullptr) {
                        recordOmission(*truncation, std::string(path));
                    }
                    continue;
                }
                const std::string key = member.key().substr(0, keyBytes);
                if (model::SafeDetail::isSecretKey(key)) {
                    if (truncation != nullptr) {
                        recordOmission(*truncation, std::string(path) + "/" + key);
                    }
                    continue;
                }
                const std::string memberPath = std::string(path) + "/" + key;
                if (scalar(member.value())) {
                    projected[key] = boundedScalar(member.value(), memberPath);
                    continue;
                }
                if (!member.value().is_array()) {
                    if (truncation != nullptr) {
                        recordOmission(*truncation, memberPath);
                    }
                    continue;
                }
                Json array = Json::array();
                const std::size_t count = std::min(member.value().size(), MaximumArrayItems);
                bool scalarOnly = true;
                for (std::size_t index = 0; index < count; ++index) {
                    if (!scalar(member.value()[index])) {
                        scalarOnly = false;
                        break;
                    }
                    array.push_back(boundedScalar(member.value()[index], memberPath + "/" + std::to_string(index)));
                }
                if (scalarOnly) {
                    projected[key] = std::move(array);
                    if (count != member.value().size() && truncation != nullptr) {
                        recordOmission(*truncation, memberPath);
                    }
                } else if (truncation != nullptr) {
                    recordOmission(*truncation, memberPath);
                }
            }
            return projected;
        }

        template <typename Value>
        void saturatingAdd(Value& destination, Value increment) noexcept {
            if (increment > std::numeric_limits<Value>::max() - destination) {
                destination = std::numeric_limits<Value>::max();
            } else {
                destination += increment;
            }
        }

        void recordOmittedEntries(model::TruncationMetadata& truncation, std::string path, std::size_t count) {
            if (count == 0) {
                return;
            }
            recordOmission(truncation, std::move(path));
            std::size_t omitted = truncation.omittedEntries.value_or(0);
            saturatingAdd(omitted, count);
            truncation.omittedEntries = omitted;
        }

        void removeSecretMembers(Json& value, bool& redacted) {
            if (value.is_object()) {
                for (auto member = value.begin(); member != value.end();) {
                    if (model::SafeDetail::isSecretKey(member.key())) {
                        member = value.erase(member);
                        redacted = true;
                    } else {
                        removeSecretMembers(member.value(), redacted);
                        ++member;
                    }
                }
            } else if (value.is_array()) {
                for (Json& member : value) {
                    removeSecretMembers(member, redacted);
                }
            }
        }

        void addTurnFailureSemanticDetails(Json& turnDetails,
                                           const Json& failure,
                                           model::TruncationMetadata& truncation) {
            Json compatibility = boundedFrontendDetailObject(failure, &truncation, "/turns/failure");
            if (!compatibility.empty()) {
                turnDetails["failure"] = std::move(compatibility);
            }
            if (!failure.is_object()) {
                turnDetails["failureDecodingOmitted"] = true;
                return;
            }
            if (const auto message = failure.find("message"); message != failure.end() && message->is_string()) {
                turnDetails["failureMessage"] =
                    boundedFrontendString(message->get_ref<const std::string&>(), 16'384, &truncation, "/turns/failure/message");
            }
            if (const auto additional = failure.find("additionalDetails"); additional != failure.end()) {
                if (additional->is_null()) {
                    turnDetails["failureAdditionalDetails"] = nullptr;
                    turnDetails["failureAdditionalDetailsPresent"] = true;
                } else if (additional->is_string()) {
                    turnDetails["failureAdditionalDetails"] = boundedFrontendString(additional->get_ref<const std::string&>(),
                                                                                      16'384,
                                                                                      &truncation,
                                                                                      "/turns/failure/additionalDetails");
                    turnDetails["failureAdditionalDetailsPresent"] = true;
                } else {
                    turnDetails["failureDecodingOmitted"] = true;
                }
            }
            if (const auto info = failure.find("codexErrorInfo"); info != failure.end()) {
                turnDetails["failureCodexErrorInfoPresent"] = true;
                if (info->is_null()) {
                    turnDetails["failureCodexErrorInfoNull"] = true;
                    return;
                }
                std::optional<std::string> discriminator;
                const Json* nested = nullptr;
                if (info->is_string()) {
                    discriminator = info->get<std::string>();
                } else if (info->is_object() && info->size() == 1) {
                    discriminator = info->begin().key();
                    nested = &info->begin().value();
                } else {
                    turnDetails["failureDecodingOmitted"] = true;
                }
                if (discriminator) {
                    turnDetails["failureCodexErrorDiscriminator"] =
                        boundedFrontendString(*discriminator, 16'384, &truncation, "/turns/failure/codexErrorInfo");
                }
                if (nested && nested->is_object()) {
                    if (const auto status = nested->find("httpStatusCode"); status != nested->end()) {
                        turnDetails["failureHttpStatusCodePresent"] = true;
                        if (status->is_null() || status->is_number_integer() || status->is_number_unsigned()) {
                            turnDetails["failureHttpStatusCode"] = *status;
                        } else {
                            turnDetails["failureDecodingOmitted"] = true;
                        }
                    }
                    if (const auto kind = nested->find("turnKind"); kind != nested->end() && kind->is_string()) {
                        turnDetails["failureNonSteerableTurnKind"] = boundedFrontendString(
                            kind->get_ref<const std::string&>(), 16'384, &truncation, "/turns/failure/codexErrorInfo/turnKind");
                    }
                }
            }
            bool redacted = false;
            Json sanitized = failure;
            removeSecretMembers(sanitized, redacted);
            if (redacted) {
                turnDetails["failureRedacted"] = true;
            }
        }

        void addTurnTokenUsageSemanticDetails(Json& turnDetails,
                                              const Json& usage,
                                              model::TruncationMetadata& truncation) {
            Json compatibility = boundedFrontendDetailObject(usage, &truncation, "/turns/tokenUsage");
            if (!compatibility.empty()) {
                turnDetails["tokenUsage"] = std::move(compatibility);
            }
            if (!usage.is_object()) {
                turnDetails["tokenUsageProjectionOmitted"] = true;
                return;
            }
            for (const char* name : {"last", "total"}) {
                const auto counts = usage.find(name);
                if (counts == usage.end()) {
                    continue;
                }
                if (!counts->is_object()) {
                    turnDetails["tokenUsageProjectionOmitted"] = true;
                    continue;
                }
                turnDetails[std::string{"tokenUsage"} + (name == std::string_view{"last"} ? "Last" : "Total")] =
                    boundedFrontendDetailObject(*counts, &truncation, std::string{"/turns/tokenUsage/"} + name);
            }
            if (const auto context = usage.find("modelContextWindow"); context != usage.end()) {
                if (context->is_null() || context->is_number_integer() || context->is_number_unsigned()) {
                    turnDetails["tokenUsageModelContextWindow"] = *context;
                    turnDetails["tokenUsageModelContextWindowPresent"] = true;
                } else {
                    turnDetails["tokenUsageProjectionOmitted"] = true;
                }
            }
        }

        std::string_view freshnessName(backend::Freshness value) noexcept {
            switch (value) {
                case backend::Freshness::Unknown:
                    return "unknown";
                case backend::Freshness::Current:
                    return "current";
                case backend::Freshness::Stale:
                    return "stale";
            }
            return "unknown";
        }

        Json semanticDomain(const backend::ProviderDomainSnapshot& domain) {
            constexpr std::size_t MaximumSemanticSummaries = 8;
            bool textTruncated = false;
            const auto boundedText = [&textTruncated](std::string_view value) {
                std::string bounded = boundedFrontendString(value, 16'384);
                textTruncated = textTruncated || bounded.size() != value.size();
                return bounded;
            };
            Json projected{
                {"resultMethods", Json::array()},
                {"resultAlternatives", Json::array()},
                {"resultStatuses", Json::array()},
                {"resultSubjectIds", Json::array()},
                {"resultSubjectIdPresent", Json::array()},
                {"resultNextCursors", Json::array()},
                {"resultNextCursorPresent", Json::array()},
                {"resultItemCounts", Json::array()},
                {"resultComplete", Json::array()},
                {"resultGenerations", Json::array()},
                {"resultFreshness", Json::array()},
                {"notificationMethods", Json::array()},
                {"notificationAlternatives", Json::array()},
                {"notificationGenerations", Json::array()},
                {"notificationFreshness", Json::array()},
                {"omittedResults",
                 domain.latestResults.size() > MaximumSemanticSummaries ? domain.latestResults.size() - MaximumSemanticSummaries : 0},
                {"omittedNotifications",
                 domain.latestNotifications.size() > MaximumSemanticSummaries ? domain.latestNotifications.size() - MaximumSemanticSummaries
                                                                              : 0}};
            const std::size_t resultStart =
                domain.latestResults.size() > MaximumSemanticSummaries ? domain.latestResults.size() - MaximumSemanticSummaries : 0;
            for (std::size_t index = resultStart; index < domain.latestResults.size(); ++index) {
                const backend::ProviderResultSummarySnapshot& result = domain.latestResults[index];
                projected["resultMethods"].push_back(boundedText(result.method));
                projected["resultAlternatives"].push_back(result.resultAlternative);
                projected["resultStatuses"].push_back(boundedText(result.status));
                projected["resultSubjectIds"].push_back(boundedText(result.subjectId.value_or("")));
                projected["resultSubjectIdPresent"].push_back(result.subjectId.has_value());
                projected["resultNextCursors"].push_back(boundedText(result.nextCursor.value_or("")));
                projected["resultNextCursorPresent"].push_back(result.nextCursor.has_value());
                projected["resultItemCounts"].push_back(result.itemCount);
                projected["resultComplete"].push_back(result.complete);
                projected["resultGenerations"].push_back(result.stamp.generation);
                projected["resultFreshness"].push_back(freshnessName(result.stamp.freshness));
            }
            const std::size_t notificationStart = domain.latestNotifications.size() > MaximumSemanticSummaries
                                                      ? domain.latestNotifications.size() - MaximumSemanticSummaries
                                                      : 0;
            for (std::size_t index = notificationStart; index < domain.latestNotifications.size(); ++index) {
                const backend::ProviderNotificationSnapshot& notification = domain.latestNotifications[index];
                projected["notificationMethods"].push_back(boundedText(notification.method));
                projected["notificationAlternatives"].push_back(notification.eventAlternative);
                projected["notificationGenerations"].push_back(notification.stamp.generation);
                projected["notificationFreshness"].push_back(freshnessName(notification.stamp.freshness));
            }
            projected["truncated"] =
                projected["omittedResults"] != 0 || projected["omittedNotifications"] != 0 || textTruncated;
            return projected;
        }

        bool addGoalMutation(Json& target,
                             std::string_view prefix,
                             const backend::ConversationDomainSnapshot::GoalMutation& mutation) {
            const std::string name{prefix};
            bool truncated = false;
            const auto boundedText = [&truncated](std::string_view value) {
                std::string bounded = boundedFrontendString(value, 16'384);
                truncated = truncated || bounded.size() != value.size();
                return bounded;
            };
            target[name + "Operation"] = boundedText(mutation.operation);
            target[name + "ThreadId"] = boundedText(mutation.threadId);
            target[name + "Objective"] = mutation.objective ? Json{boundedText(*mutation.objective)} : Json{nullptr};
            target[name + "Status"] = mutation.status ? Json{boundedText(*mutation.status)} : Json{nullptr};
            target[name + "Cleared"] = mutation.cleared ? Json{*mutation.cleared} : Json{nullptr};
            target[name + "Generation"] = mutation.stamp.generation;
            target[name + "Freshness"] = freshnessName(mutation.stamp.freshness);
            return truncated;
        }

        Json semanticProjection(const backend::Snapshot& snapshot) {
            constexpr std::size_t MaximumProviderOperations = 16;
            Json operations{{"methods", Json::array()},
                            {"resultAlternatives", Json::array()},
                            {"generations", Json::array()},
                            {"freshness", Json::array()}};
            const std::size_t operationStart = snapshot.providerOperations.size() > MaximumProviderOperations
                                                   ? snapshot.providerOperations.size() - MaximumProviderOperations
                                                   : 0;
            bool operationTextTruncated = false;
            for (std::size_t index = operationStart; index < snapshot.providerOperations.size(); ++index) {
                const backend::ProviderOperationSnapshot& operation = snapshot.providerOperations[index];
                std::string method = boundedFrontendString(operation.method, 16'384);
                operationTextTruncated = operationTextTruncated || method.size() != operation.method.size();
                operations["methods"].push_back(std::move(method));
                operations["resultAlternatives"].push_back(operation.resultAlternative);
                operations["generations"].push_back(operation.stamp.generation);
                operations["freshness"].push_back(freshnessName(operation.stamp.freshness));
            }
            operations["truncated"] = operationStart != 0 || operationTextTruncated;
            operations["omittedEntries"] = operationStart;
            Json conversations = semanticDomain(snapshot.conversations);
            bool conversationTextTruncated = false;
            if (snapshot.conversations.latestGoal) {
                conversationTextTruncated =
                    addGoalMutation(conversations, "latestGoal", *snapshot.conversations.latestGoal) || conversationTextTruncated;
            }
            if (snapshot.conversations.latestGoalClear) {
                conversationTextTruncated = addGoalMutation(
                                                conversations, "latestGoalClear", *snapshot.conversations.latestGoalClear) ||
                                            conversationTextTruncated;
            }
            if (snapshot.conversations.latestGoalSet) {
                conversationTextTruncated =
                    addGoalMutation(conversations, "latestGoalSet", *snapshot.conversations.latestGoalSet) || conversationTextTruncated;
            }
            if (snapshot.conversations.latestUnsubscribe) {
                conversationTextTruncated = addGoalMutation(
                                                conversations, "latestUnsubscribe", *snapshot.conversations.latestUnsubscribe) ||
                                            conversationTextTruncated;
            }
            if (conversationTextTruncated) {
                conversations["truncated"] = true;
            }
            return Json{{"providerOperationsSemantic", std::move(operations)},
                        {"conversationSemantic", std::move(conversations)},
                        {"filesystemProviderSemantic", semanticDomain(snapshot.filesystem)},
                        {"capacityProvenance",
                         {{"rejectedSessions", snapshot.capacity.state.rejectedSessions},
                          {"rejectedObservers", snapshot.capacity.state.rejectedObservers},
                          {"rejectedOperations", snapshot.capacity.state.rejectedOperations},
                          {"providerRequestOverflows", snapshot.capacity.state.providerRequestOverflows},
                          {"evictedThreads", snapshot.capacity.state.evictedThreads},
                          {"evictedTurns", snapshot.capacity.state.evictedTurns},
                          {"evictedItems", snapshot.capacity.state.evictedItems},
                          {"droppedContentBytes", snapshot.capacity.state.droppedContentBytes},
                          {"snapshotOmissions", snapshot.capacity.state.snapshotOmissions},
                          {"evictedNotices", snapshot.capacity.state.evictedNotices},
                          {"evictedProcesses", snapshot.capacity.state.evictedProcesses},
                          {"droppedProcessOutputBytes", snapshot.capacity.state.droppedProcessOutputBytes},
                          {"evictedFilesystemWatches", snapshot.capacity.state.evictedFilesystemWatches},
                          {"evictedFuzzySearchSessions", snapshot.capacity.state.evictedFuzzySearchSessions},
                          {"evictedActivityRecords", snapshot.capacity.state.evictedActivityRecords},
                          {"maxSessions", snapshot.capacity.state.limits.maxSessions},
                          {"maxObservers", snapshot.capacity.state.limits.maxObservers},
                          {"maxActiveOperations", snapshot.capacity.state.limits.maxActiveOperations},
                          {"maxPendingRequests", snapshot.capacity.state.limits.maxPendingRequests},
                          {"maxRetainedThreads", snapshot.capacity.state.limits.maxRetainedThreads},
                          {"maxRetainedTurns", snapshot.capacity.state.limits.maxRetainedTurns},
                          {"maxRetainedItems", snapshot.capacity.state.limits.maxRetainedItems},
                          {"maxAccumulatedContentBytes", snapshot.capacity.state.limits.maxAccumulatedContentBytes},
                          {"maxSnapshotBytes", snapshot.capacity.state.limits.maxSnapshotBytes},
                          {"maxRetainedNotices", snapshot.capacity.state.limits.maxRetainedNotices},
                          {"maxRetainedProcesses", snapshot.capacity.state.limits.maxRetainedProcesses},
                          {"maxProcessOutputBytesPerProcess", snapshot.capacity.state.limits.maxProcessOutputBytesPerProcess},
                          {"maxAccumulatedProcessOutputBytes", snapshot.capacity.state.limits.maxAccumulatedProcessOutputBytes},
                          {"maxRetainedFilesystemWatches", snapshot.capacity.state.limits.maxRetainedFilesystemWatches},
                          {"maxRetainedFuzzySearchSessions", snapshot.capacity.state.limits.maxRetainedFuzzySearchSessions},
                          {"maxRetainedActivityRecords", snapshot.capacity.state.limits.maxRetainedActivityRecords},
                          {"sourceSessionCount", snapshot.capacity.sourceSessionCount},
                          {"sourcePendingRequestCount", snapshot.capacity.sourcePendingRequestCount},
                          {"omittedThreads", snapshot.capacity.omittedThreads},
                          {"omittedTurns", snapshot.capacity.omittedTurns},
                          {"omittedItems", snapshot.capacity.omittedItems},
                          {"truncated", snapshot.capacity.truncated},
                          {"mandatoryCoreExceedsLimit", snapshot.capacity.mandatoryCoreExceedsLimit}}}};
        }

        std::optional<ThreadItemKind> backendItemKind(std::string_view type) noexcept {
            if (const auto direct = threadItemKindFromString(type)) {
                return direct;
            }
            if (type == "agent_message") {
                return ThreadItemKind::AgentMessage;
            }
            if (type == "user_message") {
                return ThreadItemKind::UserMessage;
            }
            if (type == "command_execution") {
                return ThreadItemKind::CommandExecution;
            }
            if (type == "file_change") {
                return ThreadItemKind::FileChange;
            }
            if (type == "web_search") {
                return ThreadItemKind::WebSearch;
            }
            return std::nullopt;
        }

        std::optional<ThreadItemKind> backendItemKind(const backend::ItemSnapshot& item) noexcept {
            if (const auto direct = backendItemKind(item.type)) {
                return direct;
            }
            if (item.type == "tool_call") {
                return item.data.is_object() && item.data.contains("server") ? ThreadItemKind::McpToolCall
                                                                             : ThreadItemKind::DynamicToolCall;
            }
            // The outer backend discriminator is authoritative. In
            // particular, an unknown future outer type must not be smuggled
            // into the frozen frontend authority by a familiar-looking
            // data.codexType field.
            return std::nullopt;
        }

        void projectItemContent(std::string_view source,
                                std::optional<std::string>& destination,
                                model::ItemData& data,
                                std::string path,
                                bool retainAgentTextOverflow = false,
                                bool retainCommandOutputOverflow = false) {
            if (source.empty()) {
                return;
            }
            constexpr std::size_t MaximumContentCharacters = 16U * 1024U;
            const std::uint64_t droppedBeforeProjection = data.droppedContentBytes.value_or(0);
            const bool contentTruncatedBeforeProjection = data.contentTruncated;
            const bool truncationBeforeProjection = data.truncation.truncated;
            std::string retained = boundedFrontendString(source, MaximumContentCharacters, &data.truncation, std::move(path));
            if (retained.size() != source.size()) {
                if (retainAgentTextOverflow && source.size() <= model::MaximumItemContentOverflowV1Bytes && validUtf8(source)) {
                    data.agentTextOverflowV1 = model::ItemContentOverflowV1{static_cast<std::uint64_t>(retained.size()),
                                                                           std::string(source.substr(retained.size())),
                                                                           droppedBeforeProjection,
                                                                           contentTruncatedBeforeProjection,
                                                                           truncationBeforeProjection};
                }
                if (retainCommandOutputOverflow && source.size() <= model::MaximumCommandOutputOverflowV2Bytes && validUtf8(source)) {
                    data.commandOutputOverflowV2 = model::ItemContentOverflowV1{static_cast<std::uint64_t>(retained.size()),
                                                                               std::string(source.substr(retained.size())),
                                                                               droppedBeforeProjection,
                                                                               contentTruncatedBeforeProjection,
                                                                               truncationBeforeProjection};
                }
                const std::uint64_t dropped = static_cast<std::uint64_t>(source.size() - retained.size());
                std::uint64_t droppedContent = data.droppedContentBytes.value_or(0);
                saturatingAdd(droppedContent, dropped);
                data.droppedContentBytes = droppedContent;
                saturatingAdd(data.truncation.droppedBytes, dropped);
                data.contentTruncated = true;
            }
            destination = std::move(retained);
        }

        model::ItemData projectItemData(const backend::ItemSnapshot& item,
                                        const model::ThreadIdentity& threadId,
                                        const model::TurnIdentity& turnId,
                                        std::optional<std::size_t> sourceIndex = std::nullopt) {
            model::ItemData data(requiredIdentifier<model::ItemIdentity>(item.id, "/items/id"), threadId, turnId);
            data.legacyDiscriminator = item.type;
            data.droppedContentBytes = item.droppedContentBytes;
            data.contentTruncated = item.contentTruncated;
            data.truncation.truncated = item.contentTruncated;
            data.truncation.droppedBytes = item.droppedContentBytes;
            if (!item.status.empty()) {
                data.status = boundedFrontendString(item.status, 256, &data.truncation, "/status");
            }
            projectItemContent(item.agentText, data.agentText, data, "/agentText", true);
            projectItemContent(item.reasoningText, data.reasoningText, data, "/reasoningText");
            projectItemContent(item.reasoningSummary, data.reasoningSummary, data, "/reasoningSummary");
            projectItemContent(item.commandOutput, data.commandOutput, data, "/commandOutput", false, true);
            const auto finalizeOverflow = [&data](std::optional<model::ItemContentOverflowV1>& projectedOverflow,
                                                  std::string_view restoredPath) {
                if (!projectedOverflow) {
                    return;
                }
                model::ItemContentOverflowV1& overflow = *projectedOverflow;
                const std::uint64_t suffixBytes = static_cast<std::uint64_t>(overflow.suffix.size());
                if (data.droppedContentBytes.has_value() && *data.droppedContentBytes >= suffixBytes) {
                    overflow.droppedContentBytesBeforeProjection = *data.droppedContentBytes - suffixBytes;
                }
                overflow.contentTruncatedBeforeProjection =
                    overflow.contentTruncatedBeforeProjection || overflow.droppedContentBytesBeforeProjection != 0;
                overflow.truncationBeforeProjection =
                    overflow.truncationBeforeProjection || overflow.contentTruncatedBeforeProjection ||
                    data.truncation.omittedEntries.value_or(0) != 0 ||
                    std::any_of(data.truncation.omittedPaths.begin(), data.truncation.omittedPaths.end(), [restoredPath](const std::string& path) {
                        return path != restoredPath;
                    });
            };
            finalizeOverflow(data.agentTextOverflowV1, "/agentText");
            finalizeOverflow(data.commandOutputOverflowV2, "/commandOutput");
            data.startedAtMs = item.startedAtMs;
            data.completedAtMs = item.completedAtMs;
            if (item.userMessage) {
                data.userMessage = model::UserMessageProjection{item.userMessage->clientId,
                                                                item.userMessage->text,
                                                                item.userMessage->textTruncated,
                                                                item.userMessage->contentTruncated,
                                                                item.userMessage->originalContentBytes,
                                                                item.userMessage->retainedContentBytes,
                                                                item.userMessage->originalContentItems,
                                                                item.userMessage->retainedContentItems,
                                                                item.userMessage->textParts};
            }
            data.connectionInvalidated = item.connectionInvalidated;
            data.generation = item.stamp.generation;
            data.freshness = freshness(item.stamp.freshness);
            data.sourceIndex = sourceIndex;
            return data;
        }

        std::optional<model::ThreadItem> projectItem(const backend::ItemSnapshot& item,
                                                     const model::ThreadIdentity& threadId,
                                                     const model::TurnIdentity& turnId,
                                                     model::TruncationMetadata& snapshotTruncation,
                                                     std::optional<std::size_t> sourceIndex = std::nullopt) {
            const std::optional<ThreadItemKind> kind = backendItemKind(item);
            if (!kind) {
                return std::nullopt;
            }
            model::ItemData data = projectItemData(item, threadId, turnId, sourceIndex);
            if (!item.data.empty()) {
                data.safeDetails = boundedDetail(item.data, snapshotTruncation, "/items/details");
            }
            data.legacyExtensions = boundedDetail(item.extensions, snapshotTruncation, "/items/extensions");

#define AISUITE_PROJECT_ITEM(kindName, typeName)                                                                                           \
    case ThreadItemKind::kindName:                                                                                                         \
        return model::typeName {                                                                                                           \
            std::move(data)                                                                                                                \
        }

            switch (*kind) {
                AISUITE_PROJECT_ITEM(AgentMessage, AgentMessageItem);
                AISUITE_PROJECT_ITEM(CollabAgentToolCall, CollabAgentToolCallItem);
                AISUITE_PROJECT_ITEM(CommandExecution, CommandExecutionItem);
                AISUITE_PROJECT_ITEM(ContextCompaction, ContextCompactionItem);
                AISUITE_PROJECT_ITEM(DynamicToolCall, DynamicToolCallItem);
                AISUITE_PROJECT_ITEM(EnteredReviewMode, EnteredReviewModeItem);
                AISUITE_PROJECT_ITEM(ExitedReviewMode, ExitedReviewModeItem);
                AISUITE_PROJECT_ITEM(FileChange, FileChangeItem);
                AISUITE_PROJECT_ITEM(HookPrompt, HookPromptItem);
                AISUITE_PROJECT_ITEM(ImageGeneration, ImageGenerationItem);
                AISUITE_PROJECT_ITEM(ImageView, ImageViewItem);
                AISUITE_PROJECT_ITEM(McpToolCall, McpToolCallItem);
                AISUITE_PROJECT_ITEM(Plan, PlanItem);
                AISUITE_PROJECT_ITEM(Reasoning, ReasoningItem);
                AISUITE_PROJECT_ITEM(Sleep, SleepItem);
                AISUITE_PROJECT_ITEM(SubAgentActivity, SubAgentActivityItem);
                AISUITE_PROJECT_ITEM(UserMessage, UserMessageItem);
                AISUITE_PROJECT_ITEM(WebSearch, WebSearchItem);
            }

#undef AISUITE_PROJECT_ITEM

            throw ProjectionFailure("/items/type", "backend item discriminator is not exhaustively mapped");
        }

        model::LegacyItemCompatibility projectLegacyItem(const backend::ItemSnapshot& item,
                                                          const model::ThreadIdentity& threadId,
                                                          const model::TurnIdentity& turnId,
                                                          std::size_t sourceIndex,
                                                          std::string omissionPath) {
            model::ItemData data(requiredIdentifier<model::ItemIdentity>(item.id, "/items/id"), threadId, turnId);
            data.droppedContentBytes = item.droppedContentBytes;
            data.contentTruncated = item.contentTruncated;
            data.truncation.truncated = item.contentTruncated;
            data.truncation.droppedBytes = item.droppedContentBytes;
            if (!item.status.empty()) {
                data.status = boundedFrontendString(item.status, 256, &data.truncation, omissionPath + "/status");
            }
            projectItemContent(item.agentText, data.agentText, data, omissionPath + "/agentText");
            projectItemContent(item.reasoningText, data.reasoningText, data, omissionPath + "/reasoningText");
            projectItemContent(item.reasoningSummary, data.reasoningSummary, data, omissionPath + "/reasoningSummary");
            projectItemContent(item.commandOutput, data.commandOutput, data, omissionPath + "/commandOutput");
            data.startedAtMs = item.startedAtMs;
            data.completedAtMs = item.completedAtMs;
            data.connectionInvalidated = item.connectionInvalidated;
            data.generation = item.stamp.generation;
            data.freshness = freshness(item.stamp.freshness);
            data.sourceIndex = sourceIndex;
            if (!item.data.empty()) {
                recordOmission(data.truncation, omissionPath + "/data");
            }
            if (!item.extensions.empty()) {
                recordOmission(data.truncation, omissionPath + "/extensions");
            }
            std::string discriminator;
            const bool validDiscriminator = !item.type.empty() && item.type.find('\0') == std::string::npos && validUtf8(item.type);
            if (!validDiscriminator) {
                discriminator = "unknown";
                data.truncation.truncated = true;
            } else if (item.type.size() > model::ItemIdentity::MaximumBytes) {
                const std::size_t retainedBytes = utf8PrefixLength(item.type, model::ItemIdentity::MaximumBytes);
                if (retainedBytes == 0) {
                    discriminator = "unknown";
                } else {
                    discriminator.assign(item.type.data(), retainedBytes);
                }
                data.truncation.truncated = true;
            } else {
                discriminator = item.type;
            }
            if (discriminator.empty()) {
                discriminator = "unknown";
                data.truncation.truncated = true;
            }
            return {std::move(data), std::move(discriminator), sourceIndex, std::move(omissionPath)};
        }

        std::optional<PendingRequestKind> backendPendingKind(std::string_view type) noexcept {
            if (const auto direct = pendingRequestKindFromString(type)) {
                return direct;
            }
            if (type == "command_approval") {
                return PendingRequestKind::CommandExecutionApproval;
            }
            if (type == "file_change_approval") {
                return PendingRequestKind::FileChangeApproval;
            }
            if (type == "user_input") {
                return PendingRequestKind::UserInput;
            }
            if (type == "authentication") {
                return PendingRequestKind::Authentication;
            }
            return std::nullopt;
        }

        model::PendingRequest projectPending(const backend::PendingRequestSnapshot& pending,
                                             model::TruncationMetadata& snapshotTruncation,
                                             std::optional<std::size_t> sourceIndex = std::nullopt) {
            const std::optional<PendingRequestKind> kind = backendPendingKind(pending.type);
            if (!kind) {
                throw ProjectionFailure("/pendingRequests/kind", "backend pending request is outside the frozen 10-kind authority");
            }
            model::PendingRequestData data(
                requiredIdentifier<model::PendingRequestIdentity>(std::to_string(pending.id.value()), "/pendingRequests/id"));
            data.sourceIndex = sourceIndex;
            if (pending.threadId) {
                data.threadId = requiredIdentifier<model::ThreadIdentity>(*pending.threadId, "/pendingRequests/threadId");
            }
            if (pending.turnId) {
                data.turnId = requiredIdentifier<model::TurnIdentity>(*pending.turnId, "/pendingRequests/turnId");
            }
            if (pending.itemId) {
                data.itemId = requiredIdentifier<model::ItemIdentity>(*pending.itemId, "/pendingRequests/itemId");
            }
            Json details = pending.details.is_object() ? pending.details : Json::object();
            if (const auto summary = details.find("summary"); summary != details.end()) {
                data.summary = "bounded request summary available";
            }
            if (*kind == PendingRequestKind::UserInput) {
                if (details.value("questionsTruncated", false)) {
                    recordOmission(data.truncation, "/pendingRequests/details/questions");
                }
                constexpr std::size_t MaximumPresentationEntries = 64;
                const auto boundedText = [&](const Json& value, std::size_t maximumCharacters, std::string_view path) {
                    if (!value.is_string()) {
                        throw ProjectionFailure(std::string(path), "pending-request presentation field must be a string");
                    }
                    std::string text = value.get<std::string>();
                    if (!validUtf8(text)) {
                        throw ProjectionFailure(std::string(path), "pending-request presentation field must contain valid UTF-8");
                    }
                    return boundedFrontendString(text, maximumCharacters, &data.truncation, std::string(path));
                };
                if (const auto questions = details.find("questions"); questions != details.end()) {
                    if (!questions->is_array()) {
                        throw ProjectionFailure("/pendingRequests/details/questions", "user-input questions must be an array");
                    }
                    data.questionsPresent = true;
                    const std::size_t count = std::min(questions->size(), MaximumPresentationEntries);
                    data.truncation.truncated = data.truncation.truncated || count != questions->size();
                    data.questions.reserve(count);
                    for (std::size_t questionIndex = 0; questionIndex < count; ++questionIndex) {
                        const Json& question = questions->at(questionIndex);
                        if (!question.is_object() || !question.contains("id") || !question.contains("header") ||
                            !question.contains("prompt") || !question.contains("allowsFreeText") ||
                            !question.at("allowsFreeText").is_boolean() || !question.contains("secret") ||
                            !question.at("secret").is_boolean() || !question.contains("options") || !question.at("options").is_array()) {
                            throw ProjectionFailure("/pendingRequests/details/questions/" + std::to_string(questionIndex),
                                                    "user-input question lacks required presentation fields");
                        }
                        const std::string path = "/pendingRequests/details/questions/" + std::to_string(questionIndex);
                        model::PendingRequestQuestion projectedQuestion;
                        projectedQuestion.id = boundedText(question.at("id"), 1'024, path + "/id");
                        projectedQuestion.header = boundedText(question.at("header"), 16'384, path + "/header");
                        projectedQuestion.prompt = boundedText(question.at("prompt"), 16'384, path + "/prompt");
                        projectedQuestion.allowsFreeText = question.at("allowsFreeText").get<bool>();
                        projectedQuestion.secretAnswer = question.at("secret").get<bool>();
                        const Json& options = question.at("options");
                        const std::size_t optionCount = std::min(options.size(), MaximumPresentationEntries);
                        data.truncation.truncated = data.truncation.truncated || optionCount != options.size();
                        projectedQuestion.options.reserve(optionCount);
                        for (std::size_t optionIndex = 0; optionIndex < optionCount; ++optionIndex) {
                            const Json& option = options.at(optionIndex);
                            if (!option.is_object() || !option.contains("label") || !option.contains("description")) {
                                throw ProjectionFailure(path + "/options/" + std::to_string(optionIndex),
                                                        "user-input option lacks required presentation fields");
                            }
                            const std::string optionPath = path + "/options/" + std::to_string(optionIndex);
                            projectedQuestion.options.push_back({boundedText(option.at("label"), 16'384, optionPath + "/label"),
                                                                 boundedText(option.at("description"), 16'384, optionPath + "/description"),
                                                                 {}});
                        }
                        data.questions.push_back(std::move(projectedQuestion));
                    }
                    details.erase(questions);
                }
                if (const auto resolution = details.find("autoResolutionMs"); resolution != details.end()) {
                    if (resolution->is_number_unsigned()) {
                        data.autoResolutionMs = resolution->get<std::uint64_t>();
                    } else if (resolution->is_number_integer() && resolution->get<std::int64_t>() >= 0) {
                        data.autoResolutionMs = static_cast<std::uint64_t>(resolution->get<std::int64_t>());
                    } else {
                        recordOmission(data.truncation, "/pendingRequests/autoResolutionMs");
                    }
                    details.erase(resolution);
                }
            }
            if (!details.empty()) {
                Json projectedDetails = boundedFrontendDetailObject(details, &data.truncation, "/pendingRequests/details");
                if (data.truncation.truncated) {
                    recordOmission(snapshotTruncation, "/pendingRequests/details");
                }
                data.safeDetails = boundedDetail(std::move(projectedDetails), snapshotTruncation, "/pendingRequests/details");
            }

#define AISUITE_PROJECT_PENDING(kindName, typeName)                                                                                        \
    case PendingRequestKind::kindName:                                                                                                     \
        return model::typeName {                                                                                                           \
            std::move(data)                                                                                                                \
        }

            switch (*kind) {
                AISUITE_PROJECT_PENDING(CommandExecutionApproval, CommandExecutionApprovalRequest);
                AISUITE_PROJECT_PENDING(FileChangeApproval, FileChangeApprovalRequest);
                AISUITE_PROJECT_PENDING(UserInput, UserInputRequest);
                AISUITE_PROJECT_PENDING(Authentication, AuthenticationRequest);
                AISUITE_PROJECT_PENDING(ApplyPatchApproval, ApplyPatchApprovalRequest);
                AISUITE_PROJECT_PENDING(ExecCommandApproval, ExecCommandApprovalRequest);
                AISUITE_PROJECT_PENDING(PermissionsApproval, PermissionsApprovalRequest);
                AISUITE_PROJECT_PENDING(Attestation, AttestationRequest);
                AISUITE_PROJECT_PENDING(DynamicToolCall, DynamicToolCallRequest);
                AISUITE_PROJECT_PENDING(McpElicitation, McpElicitationRequest);
            }

#undef AISUITE_PROJECT_PENDING

            throw ProjectionFailure("/pendingRequests/kind", "backend pending request kind is not exhaustively mapped");
        }

        model::LegacyPendingRequestCompatibility
        projectLegacyPending(const backend::PendingRequestSnapshot& pending,
                             std::size_t sourceIndex,
                             std::string omissionPath) {
            model::PendingRequestData data(
                requiredIdentifier<model::PendingRequestIdentity>(std::to_string(pending.id.value()), "/pendingRequests/id"));
            data.sourceIndex = sourceIndex;
            if (pending.threadId) {
                data.threadId = requiredIdentifier<model::ThreadIdentity>(*pending.threadId, "/pendingRequests/threadId");
            }
            if (pending.turnId) {
                data.turnId = requiredIdentifier<model::TurnIdentity>(*pending.turnId, "/pendingRequests/turnId");
            }
            if (pending.itemId) {
                data.itemId = requiredIdentifier<model::ItemIdentity>(*pending.itemId, "/pendingRequests/itemId");
            }
            Json details = pending.details.is_object() ? pending.details : Json::object();
            if (!details.empty()) {
                if (auto safe = model::SafeDetail::fromJson(std::move(details)); safe.has_value()) {
                    data.safeDetails = std::move(*safe);
                } else {
                    recordOmission(data.truncation, omissionPath + "/details");
                }
            }
            return {std::move(data), sourceIndex, std::move(omissionPath)};
        }

        constexpr std::size_t MaximumDomainResults = 128;
        constexpr std::size_t MaximumDetailArrayItems = 64;
        constexpr std::size_t MaximumFrontendSessions = 128;
        constexpr std::size_t MaximumFrontendThreads = 2'048;
        constexpr std::size_t MaximumFrontendTurns = 16'384;
        constexpr std::size_t MaximumFrontendItems = 65'536;
        constexpr std::size_t MaximumFrontendPendingRequests = 1'024;
        constexpr std::size_t MaximumFrontendProcesses = 256;
        constexpr std::size_t MaximumFrontendFilesystemWatches = 1'024;
        constexpr std::size_t MaximumFrontendFuzzySearches = 256;
        constexpr std::size_t MaximumFrontendNotices = 256;
        constexpr std::size_t MaximumFrontendActivities = 512;

        backend::SourceStamp domainStamp(const backend::ProviderDomainSnapshot& domain) noexcept {
            if (!domain.latestResults.empty()) {
                return domain.latestResults.back().stamp;
            }
            if (!domain.latestNotifications.empty()) {
                return domain.latestNotifications.back().stamp;
            }
            return {};
        }

        model::DomainState projectDomain(const backend::ProviderDomainSnapshot& domain, Json details = Json::object()) {
            model::DomainState projected = model::DomainState::present();
            projected.stamp = sourceMetadata(domainStamp(domain));
            if (!domain.latestResults.empty()) {
                const backend::ProviderResultSummarySnapshot& authoritative = domain.latestResults.back();
                projected.status = boundedFrontendString(authoritative.status, 256, &projected.truncation, "/domains/status");
                projected.complete = authoritative.complete;
                projected.completeKnown = true;
                projected.itemCount = static_cast<std::uint64_t>(authoritative.itemCount);
                if (authoritative.nextCursor) {
                    projected.nextCursor =
                        boundedFrontendString(*authoritative.nextCursor, 16'384, &projected.truncation, "/domains/nextCursor");
                }
            }
            const std::size_t start =
                domain.latestResults.size() > MaximumDomainResults ? domain.latestResults.size() - MaximumDomainResults : 0;
            projected.latestResults.reserve(domain.latestResults.size() - start);
            for (std::size_t index = start; index < domain.latestResults.size(); ++index) {
                const backend::ProviderResultSummarySnapshot& summary = domain.latestResults[index];
                if (summary.method.empty()) {
                    continue;
                }
                model::DomainResultSummary result;
                result.method = boundedFrontendString(summary.method, 1'024, &projected.truncation, "/domains/latestResults/method");
                if (result.method.empty()) {
                    recordOmission(projected.truncation, "/domains/latestResults");
                    continue;
                }
                result.status = boundedFrontendString(summary.status, 256, &projected.truncation, "/domains/latestResults/status");
                if (summary.subjectId) {
                    std::string subject =
                        boundedFrontendString(*summary.subjectId, 1'024, &projected.truncation, "/domains/latestResults/subjectId");
                    if (!subject.empty()) {
                        result.subjectId = std::move(subject);
                    }
                }
                if (summary.nextCursor) {
                    result.nextCursor = boundedFrontendString(
                        *summary.nextCursor, 16'384, &projected.truncation, "/domains/latestResults/nextCursor");
                }
                result.itemCount = static_cast<std::uint64_t>(summary.itemCount);
                result.complete = summary.complete;
                result.completeKnown = true;
                result.stamp = sourceMetadata(summary.stamp);
                projected.latestResults.push_back(std::move(result));
            }
            details["notificationCount"] = domain.latestNotifications.size();
            Json methods = Json::array();
            const std::size_t methodCount = std::min(domain.latestNotificationMethods.size(), MaximumDetailArrayItems);
            for (std::size_t index = 0; index < methodCount; ++index) {
                methods.push_back(domain.latestNotificationMethods[index]);
            }
            if (!methods.empty()) {
                details["latestNotificationMethods"] = std::move(methods);
            }
            Json projectedDetails = boundedFrontendDetailObject(details, &projected.truncation, "/domains/details");
            model::SafeDetailError detailError = model::SafeDetailError::None;
            auto safeDetails = model::SafeDetail::fromJson(std::move(projectedDetails), &detailError);
            if (!safeDetails) {
                throw ProjectionFailure("/domains/details", "backend domain details exceed the bounded safe-detail contract");
            }
            projected.safeDetails = std::move(*safeDetails);
            projected.truncation.truncated = projected.truncation.truncated || start != 0;
            std::size_t omittedEntries = projected.truncation.omittedEntries.value_or(0);
            saturatingAdd(omittedEntries, start);
            projected.truncation.omittedEntries = omittedEntries;
            return projected;
        }

        Json accountDetails(const backend::AccountDomainSnapshot& accounts) {
            Json details{{"loggedOut", accounts.loggedOut}};
            if (accounts.login) {
                details["loginLifecycle"] = accounts.login->lifecycle;
                details["loginMethod"] = accounts.login->method;
                if (accounts.login->success) {
                    details["loginSucceeded"] = *accounts.login->success;
                }
            }
            if (accounts.authentication) {
                details["authenticated"] = accounts.authentication->authenticated;
                if (accounts.authentication->accountType) {
                    details["accountType"] = *accounts.authentication->accountType;
                }
                if (accounts.authentication->authMode) {
                    details["authMode"] = *accounts.authentication->authMode;
                }
                if (accounts.authentication->planType) {
                    details["planType"] = *accounts.authentication->planType;
                }
            }
            if (accounts.rateLimits) {
                if (accounts.rateLimits->primaryUsedPercent) {
                    details["primaryUsedPercent"] = *accounts.rateLimits->primaryUsedPercent;
                }
                if (accounts.rateLimits->secondaryUsedPercent) {
                    details["secondaryUsedPercent"] = *accounts.rateLimits->secondaryUsedPercent;
                }
                if (accounts.rateLimits->hasCredits) {
                    details["hasCredits"] = *accounts.rateLimits->hasCredits;
                }
            }
            return details;
        }

        Json configurationDetails(const backend::ConfigurationDomainSnapshot& configuration) {
            Json details = Json::object();
            if (configuration.lastWrite) {
                details["filePath"] = configuration.lastWrite->filePath;
                details["writeStatus"] = configuration.lastWrite->status;
                details["writeVersion"] = configuration.lastWrite->version;
                details["writeOverridden"] = configuration.lastWrite->overridden;
            }
            if (configuration.experimentalFeatureEnablement) {
                details["featureCount"] = configuration.experimentalFeatureEnablement->totalEntries;
                details["featureListTruncated"] = configuration.experimentalFeatureEnablement->truncated;
            }
            return details;
        }

        Json integrationsDetails(const backend::IntegrationsDomainSnapshot& integrations) {
            Json details = Json::object();
            if (integrations.apps) {
                details["appCount"] = integrations.apps->totalEntries;
                details["appListTruncated"] = integrations.apps->truncated;
            }
            if (integrations.marketplaceAdd) {
                details["marketplaceAddStatus"] = integrations.marketplaceAdd->operation;
            }
            if (integrations.marketplaceRemove) {
                details["marketplaceRemoveStatus"] = integrations.marketplaceRemove->operation;
            }
            if (integrations.marketplaceUpgrade) {
                details["marketplaceUpgradeStatus"] = integrations.marketplaceUpgrade->operation;
            }
            return details;
        }

        Json pluginsDetails(const backend::PluginsAndSkillsDomainSnapshot& plugins) {
            Json details = Json::object();
            if (plugins.pluginInstall) {
                details["lastPluginOperation"] = plugins.pluginInstall->operation;
            }
            if (plugins.skillsConfigWrite) {
                details["lastSkillsOperation"] = plugins.skillsConfigWrite->operation;
            }
            if (plugins.extraRoots) {
                details["extraRootCount"] = plugins.extraRoots->roots.size();
                details["extraRootsTruncated"] = plugins.extraRoots->truncated;
            }
            return details;
        }

        Json mcpDetails(const backend::McpDomainSnapshot& mcp) {
            Json details = Json::object();
            if (mcp.oauth) {
                details["oauthStatus"] = mcp.oauth->lifecycle;
            }
            if (mcp.startup) {
                details["startupStatus"] = mcp.startup->status;
            }
            if (mcp.statusList) {
                details["serverCount"] = mcp.statusList->serverCount;
                details["statusListComplete"] = mcp.statusList->complete;
            }
            return details;
        }

        Json platformDetails(const backend::PlatformDomainSnapshot& platform) {
            Json details = Json::object();
            if (platform.remoteControl) {
                details["remoteControlStatus"] = platform.remoteControl->status;
            }
            if (platform.windowsSandbox) {
                details["windowsSandboxStatus"] = platform.windowsSandbox->lifecycle;
            }
            return details;
        }

        std::string noticeCategory(backend::NoticeCategory category) {
            switch (category) {
                case backend::NoticeCategory::Warning:
                    return "warning";
                case backend::NoticeCategory::Deprecation:
                    return "deprecation";
                case backend::NoticeCategory::Configuration:
                    return "configuration";
                case backend::NoticeCategory::Security:
                    return "security";
                case backend::NoticeCategory::WindowsWorldWritable:
                    return "windows_world_writable";
            }
            return "warning";
        }

        std::optional<model::NoticeRecord> noticeFromExtension(const backend::CodexExtensionReceived& extension,
                                                               const model::CanonicalSnapshot& snapshot) {
            if (!extension.safeProjection || !extension.payload.is_object()) {
                return std::nullopt;
            }
            const auto boundedString = [&](std::string_view member,
                                           std::size_t maximumCharacters = 16'384) -> std::optional<std::string> {
                const auto value = extension.payload.find(member);
                if (value == extension.payload.end() || !value->is_string() || value->get_ref<const std::string&>().empty()) {
                    return std::nullopt;
                }
                const std::string& text = value->get_ref<const std::string&>();
                return boundedFrontendString(text, maximumCharacters);
            };
            const auto exactString = [&](std::string_view member, std::size_t maximumBytes) -> std::optional<std::string> {
                const auto value = extension.payload.find(member);
                if (value == extension.payload.end() || !value->is_string() || value->get_ref<const std::string&>().empty()) {
                    return std::nullopt;
                }
                const std::string& text = value->get_ref<const std::string&>();
                return text.size() <= maximumBytes && validUtf8(text) ? std::optional<std::string>{text} : std::nullopt;
            };

            model::NoticeRecord notice;
            notice.occurrence = 0;
            notice.stamp.generation = snapshot.provider.generation;
            notice.stamp.freshness = model::Freshness::Current;
            if (extension.method == "deprecationNotice") {
                notice.category = "deprecation";
                const auto summary = boundedString("summary");
                if (!summary) {
                    return std::nullopt;
                }
                notice.summary = *summary;
                notice.details = boundedString("details");
            } else if (extension.method == "configWarning") {
                notice.category = "configuration";
                const auto summary = boundedString("summary");
                if (!summary) {
                    return std::nullopt;
                }
                notice.summary = *summary;
                notice.details = boundedString("details");
            } else if (extension.method == "guardianWarning" || extension.method == "warning") {
                notice.category = extension.method == "guardianWarning" ? "security" : "warning";
                const auto summary = boundedString("message");
                if (!summary) {
                    return std::nullopt;
                }
                notice.summary = *summary;
                if (const auto thread = exactString("threadId", model::ThreadIdentity::MaximumBytes); thread.has_value()) {
                    notice.threadId = model::ThreadIdentity::parse(*thread);
                }
            } else if (extension.method == "windows/worldWritableWarning") {
                const auto failedScan = extension.payload.find("failedScan");
                const auto samplePaths = extension.payload.find("samplePaths");
                const auto extraCount = extension.payload.find("extraCount");
                if (failedScan == extension.payload.end() || !failedScan->is_boolean() || samplePaths == extension.payload.end() ||
                    !samplePaths->is_array() || extraCount == extension.payload.end() ||
                    (!extraCount->is_number_unsigned() && !extraCount->is_number_integer())) {
                    return std::nullopt;
                }
                notice.category = "windows_world_writable";
                notice.summary = failedScan->get<bool>() ? "world-writable path scan failed" : "world-writable paths detected";
                notice.details = "sample paths: " + std::to_string(samplePaths->size()) + ", additional paths: " + extraCount->dump();
            } else {
                return std::nullopt;
            }
            return notice;
        }

        struct OccurrenceSelection {
            std::optional<model::SessionIdentity> sessionId;
            std::optional<model::ThreadIdentity> threadId;
            std::optional<model::TurnIdentity> turnId;
            std::optional<model::ItemIdentity> itemId;
            std::optional<model::PendingRequestIdentity> pendingRequestId;
            std::optional<model::ProcessHandle> processHandle;
            std::optional<std::string> filesystemWatchId;
            std::optional<std::string> fuzzySearchId;
            std::optional<std::string> activityKey;
            std::optional<std::string> channel;
            std::optional<model::NoticeRecord> notice;
        };

        const Json* exactValueAt(const Json& value, std::initializer_list<std::string_view> path) noexcept {
            try {
                const Json* current = &value;
                for (const std::string_view component : path) {
                    if (!current->is_object()) {
                        return nullptr;
                    }
                    const auto member = current->find(std::string(component));
                    if (member == current->end()) {
                        return nullptr;
                    }
                    current = &*member;
                }
                return current;
            } catch (...) {
                return nullptr;
            }
        }

        template <typename Strong>
        std::optional<Strong> identifierAt(const Json& value, std::initializer_list<std::string_view> path) noexcept {
            try {
                const Json* candidate = exactValueAt(value, path);
                return candidate != nullptr && candidate->is_string() ? Strong::parse(candidate->get<std::string>()) : std::nullopt;
            } catch (...) {
                return std::nullopt;
            }
        }

        bool validEntityKey(std::string_view key) noexcept {
            return model::ThreadIdentity::isValid(key);
        }

        std::optional<std::string> entityKeyAt(const Json& value, std::initializer_list<std::string_view> path) noexcept {
            try {
                const Json* candidate = exactValueAt(value, path);
                if (candidate == nullptr || !candidate->is_string()) {
                    return std::nullopt;
                }
                const std::string& key = candidate->get_ref<const std::string&>();
                return validEntityKey(key) ? std::optional<std::string>{key} : std::nullopt;
            } catch (...) {
                return std::nullopt;
            }
        }

        template <typename Strong>
        bool agreesWhenPresent(const Json& value, std::initializer_list<std::string_view> path, const Strong& expected) noexcept {
            const Json* candidate = exactValueAt(value, path);
            if (candidate == nullptr) {
                return true;
            }
            const std::optional<Strong> parsed = identifierAt<Strong>(value, path);
            return parsed && *parsed == expected;
        }

        template <typename Strong>
        bool nestedIdentityAgreesWhenPresent(const Json& value, std::string_view wrapperName, const Strong& expected) noexcept {
            try {
                if (!value.is_object()) {
                    return false;
                }
                const auto wrapper = value.find(std::string(wrapperName));
                if (wrapper == value.end()) {
                    return true;
                }
                if (!wrapper->is_object()) {
                    return false;
                }
                const std::optional<Strong> nested = identifierAt<Strong>(*wrapper, {"id"});
                return nested && *nested == expected;
            } catch (...) {
                return false;
            }
        }

        OccurrenceSelection selectionFromExtension(const backend::CodexExtensionReceived& extension,
                                                   const model::CanonicalSnapshot& snapshot) noexcept {
            OccurrenceSelection selection;
            try {
                const Json& payload = extension.payload;
                if (!payload.is_object()) {
                    selection.notice = noticeFromExtension(extension, snapshot);
                    return selection;
                }

                selection.pendingRequestId = identifierAt<model::PendingRequestIdentity>(payload, {"pendingRequestId"});

                if (extension.method == "thread/started") {
                    const std::optional<model::ThreadIdentity> nested = identifierAt<model::ThreadIdentity>(payload, {"thread", "id"});
                    if (nested && agreesWhenPresent(payload, {"threadId"}, *nested)) {
                        selection.threadId = nested;
                    }
                } else if (extension.method.starts_with("thread/") || extension.method.starts_with("turn/") ||
                           extension.method.starts_with("item/") || extension.method.starts_with("model/")) {
                    const std::optional<model::ThreadIdentity> threadId =
                        identifierAt<model::ThreadIdentity>(payload, {"threadId"});
                    if (threadId && nestedIdentityAgreesWhenPresent(payload, "thread", *threadId) &&
                        agreesWhenPresent(payload, {"turn", "threadId"}, *threadId) &&
                        agreesWhenPresent(payload, {"item", "threadId"}, *threadId)) {
                        selection.threadId = threadId;
                    }
                }

                if (extension.method == "turn/started" || extension.method == "turn/completed") {
                    const std::optional<model::TurnIdentity> nested = identifierAt<model::TurnIdentity>(payload, {"turn", "id"});
                    if (selection.threadId && nested && agreesWhenPresent(payload, {"turnId"}, *nested) &&
                        agreesWhenPresent(payload, {"turn", "threadId"}, *selection.threadId)) {
                        selection.turnId = nested;
                    }
                } else if (extension.method.starts_with("turn/") || extension.method == "thread/compacted" ||
                           extension.method == "thread/tokenUsage/updated" || extension.method.starts_with("model/")) {
                    const std::optional<model::TurnIdentity> turnId = identifierAt<model::TurnIdentity>(payload, {"turnId"});
                    if (selection.threadId && turnId && nestedIdentityAgreesWhenPresent(payload, "turn", *turnId) &&
                        agreesWhenPresent(payload, {"turn", "threadId"}, *selection.threadId)) {
                        selection.turnId = turnId;
                    }
                }

                if (extension.method == "item/started" || extension.method == "item/completed") {
                    const std::optional<model::TurnIdentity> turnId = identifierAt<model::TurnIdentity>(payload, {"turnId"});
                    const std::optional<model::ItemIdentity> nested = identifierAt<model::ItemIdentity>(payload, {"item", "id"});
                    if (selection.threadId && turnId && nested && agreesWhenPresent(payload, {"itemId"}, *nested) &&
                        agreesWhenPresent(payload, {"item", "threadId"}, *selection.threadId) &&
                        agreesWhenPresent(payload, {"item", "turnId"}, *turnId)) {
                        selection.turnId = turnId;
                        selection.itemId = nested;
                    }
                } else if (extension.method.starts_with("item/")) {
                    const std::optional<model::TurnIdentity> turnId = identifierAt<model::TurnIdentity>(payload, {"turnId"});
                    const std::optional<model::ItemIdentity> itemId = identifierAt<model::ItemIdentity>(payload, {"itemId"});
                    if (selection.threadId && turnId && itemId && nestedIdentityAgreesWhenPresent(payload, "item", *itemId) &&
                        agreesWhenPresent(payload, {"item", "threadId"}, *selection.threadId) &&
                        agreesWhenPresent(payload, {"item", "turnId"}, *turnId)) {
                        selection.turnId = turnId;
                        selection.itemId = itemId;
                    }
                }

                if (extension.method == "item/agentMessage/delta") {
                    selection.channel = "agentText";
                } else if (extension.method == "item/reasoning/textDelta") {
                    selection.channel = "reasoningText";
                } else if (extension.method == "item/reasoning/summaryTextDelta") {
                    selection.channel = "reasoningSummary";
                } else if (extension.method == "item/commandExecution/outputDelta" ||
                           extension.method == "item/fileChange/outputDelta") {
                    selection.channel = "commandOutput";
                }

                if (extension.method == "command/exec/outputDelta") {
                    selection.processHandle = identifierAt<model::ProcessHandle>(payload, {"processId"});
                } else if (extension.method == "process/outputDelta" || extension.method == "process/exited") {
                    selection.processHandle = identifierAt<model::ProcessHandle>(payload, {"processHandle"});
                }
                if (extension.method == "fs/changed") {
                    selection.filesystemWatchId = entityKeyAt(payload, {"watchId"});
                }
                if (extension.method == "fuzzyFileSearch/sessionUpdated" || extension.method == "fuzzyFileSearch/sessionCompleted") {
                    selection.fuzzySearchId = entityKeyAt(payload, {"sessionId"});
                }
                if (const std::optional<std::string> key = entityKeyAt(payload, {"activity", "key"}); key) {
                    selection.activityKey = key;
                }
                selection.notice = noticeFromExtension(extension, snapshot);
            } catch (...) {
            }
            return selection;
        }

        // BEGIN exact-entity-lookup-policy
        const model::ThreadState* findThread(const model::CanonicalSnapshot& snapshot, const model::ThreadIdentity& id) noexcept {
            const model::ThreadState* found = nullptr;
            for (const model::ThreadState& thread : snapshot.threads) {
                if (thread.id != id) {
                    continue;
                }
                if (found != nullptr) {
                    return nullptr;
                }
                found = &thread;
            }
            return found;
        }

        const model::TurnState* findTurn(const model::CanonicalSnapshot& snapshot,
                                         const model::ThreadIdentity& threadId,
                                         const model::TurnIdentity& turnId) noexcept {
            const model::TurnState* found = nullptr;
            for (const model::TurnState& turn : snapshot.turns) {
                if (turn.threadId != threadId || turn.id != turnId) {
                    continue;
                }
                if (found != nullptr) {
                    return nullptr;
                }
                found = &turn;
            }
            return found;
        }

        const model::ThreadItem* findItem(const model::CanonicalSnapshot& snapshot,
                                          const model::ThreadIdentity& threadId,
                                          const model::TurnIdentity& turnId,
                                          const model::ItemIdentity& itemId) noexcept {
            const model::ThreadItem* found = nullptr;
            for (const model::ThreadItem& item : snapshot.items) {
                const model::ItemData& data = model::itemData(item);
                if (!data.threadId || *data.threadId != threadId || !data.turnId || *data.turnId != turnId || data.id != itemId) {
                    continue;
                }
                if (found != nullptr) {
                    return nullptr;
                }
                found = &item;
            }
            return found;
        }

        const backend::ItemSnapshot* findBackendItem(const backend::Snapshot& snapshot,
                                                      const typed::ThreadId& threadId,
                                                      const typed::TurnId& turnId,
                                                      const typed::ItemId& itemId) noexcept {
            const backend::ItemSnapshot* found = nullptr;
            for (const backend::ThreadSnapshot& thread : snapshot.threads) {
                if (thread.id != threadId.value) {
                    continue;
                }
                for (const backend::TurnSnapshot& turn : thread.turns) {
                    if (turn.id != turnId.value) {
                        continue;
                    }
                    for (const backend::ItemSnapshot& item : turn.items) {
                        if (item.id != itemId.value) {
                            continue;
                        }
                        if (found != nullptr) {
                            return nullptr;
                        }
                        found = &item;
                    }
                }
            }
            return found;
        }

        const model::ProcessState* findProcess(const model::CanonicalSnapshot& snapshot, const model::ProcessHandle& handle) noexcept {
            const model::ProcessState* found = nullptr;
            for (const model::ProcessState& process : snapshot.processes) {
                if (process.handle != handle) {
                    continue;
                }
                if (found != nullptr) {
                    return nullptr;
                }
                found = &process;
            }
            return found;
        }

        const model::FilesystemWatchRecord*
        findFilesystemWatch(const model::CanonicalSnapshot& snapshot, std::string_view watchId) noexcept {
            const model::FilesystemWatchRecord* found = nullptr;
            for (const model::FilesystemWatchRecord& watch : snapshot.filesystemWatches.entries) {
                if (watch.watchId != watchId) {
                    continue;
                }
                if (found != nullptr) {
                    return nullptr;
                }
                found = &watch;
            }
            return found;
        }

        const model::FuzzySearchRecord*
        findFuzzySearch(const model::CanonicalSnapshot& snapshot, std::string_view sessionId) noexcept {
            const model::FuzzySearchRecord* found = nullptr;
            for (const model::FuzzySearchRecord& search : snapshot.fuzzySearches.entries) {
                if (search.sessionId != sessionId) {
                    continue;
                }
                if (found != nullptr) {
                    return nullptr;
                }
                found = &search;
            }
            return found;
        }

        const model::ActivityRecord* findActivity(const model::CanonicalSnapshot& snapshot, std::string_view key) noexcept {
            const model::ActivityRecord* found = nullptr;
            for (const model::ActivityRecord& activity : snapshot.activities.entries) {
                if (activity.key != key) {
                    continue;
                }
                if (found != nullptr) {
                    return nullptr;
                }
                found = &activity;
            }
            return found;
        }
        // END exact-entity-lookup-policy

        std::optional<model::OccurrencePayload>
        payloadForFamily(ExpandedEventType family, const model::CanonicalSnapshot& snapshot, const OccurrenceSelection& selection) {
            switch (family) {
                case ExpandedEventType::ProviderUpdated:
                    return model::ProviderUpdatedOccurrence{snapshot.provider};
                case ExpandedEventType::ControllerUpdated:
                    return model::ControllerUpdatedOccurrence{snapshot.controller};
                case ExpandedEventType::SessionsUpdated:
                    return model::SessionsUpdatedOccurrence{snapshot.sessions};
                case ExpandedEventType::ThreadListUpdated:
                    return model::ThreadListUpdatedOccurrence{snapshot.threadList};
                case ExpandedEventType::ThreadUpserted: {
                    if (!selection.threadId) {
                        return std::nullopt;
                    }
                    const model::ThreadState* thread = findThread(snapshot, *selection.threadId);
                    if (thread == nullptr) {
                        return std::nullopt;
                    }
                    model::ThreadUpsertedOccurrence update{*thread};
                    // Global thread families publish bounded headers. They
                    // carry no descendant replacement authority; recipients
                    // retain their existing turns and may hydrate explicitly.
                    update.thread.fullyLoaded = false;
                    update.authority = model::ThreadUpsertAuthority::Header;
                    return model::OccurrencePayload{std::move(update)};
                }
                case ExpandedEventType::ThreadRemoved:
                    return selection.threadId ? std::optional<model::OccurrencePayload>{model::ThreadRemovedOccurrence{*selection.threadId}}
                                              : std::nullopt;
                case ExpandedEventType::TurnUpserted: {
                    if (!selection.threadId || !selection.turnId) {
                        return std::nullopt;
                    }
                    const model::TurnState* turn = findTurn(snapshot, *selection.threadId, *selection.turnId);
                    if (turn == nullptr) {
                        return std::nullopt;
                    }
                    model::TurnUpsertedOccurrence update{*turn};
                    // Turn lifecycle/status notifications carry no item-list
                    // authority. Keep the global occurrence bounded to the
                    // turn header represented by the canonical snapshot.
                    update.replaceItems = false;
                    return model::OccurrencePayload{std::move(update)};
                }
                case ExpandedEventType::ItemUpserted: {
                    if (!selection.threadId || !selection.turnId || !selection.itemId) {
                        return std::nullopt;
                    }
                    const model::ThreadItem* item = findItem(snapshot, *selection.threadId, *selection.turnId, *selection.itemId);
                    return item ? std::optional<model::OccurrencePayload>{model::ItemUpsertedOccurrence{*item}} : std::nullopt;
                }
                case ExpandedEventType::ItemContentUpdated: {
                    if (!selection.threadId || !selection.turnId || !selection.itemId || !selection.channel) {
                        return std::nullopt;
                    }
                    const model::ThreadItem* item = findItem(snapshot, *selection.threadId, *selection.turnId, *selection.itemId);
                    if (item == nullptr) {
                        return std::nullopt;
                    }
                    const model::ItemData& data = model::itemData(*item);
                    model::ItemContentUpdatedOccurrence update{data.id};
                    update.threadId = data.threadId;
                    update.turnId = data.turnId;
                    update.channel = *selection.channel;
                    update.itemKind = model::threadItemKind(*item);
                    if (*update.channel == "agentText") {
                        update.content = data.agentText;
                        update.overflowV1 = data.agentTextOverflowV1;
                    } else if (*update.channel == "reasoningText") {
                        update.content = data.reasoningText;
                    } else if (*update.channel == "reasoningSummary") {
                        update.content = data.reasoningSummary;
                    } else if (*update.channel == "commandOutput") {
                        update.content = data.commandOutput;
                        update.overflowV1 = data.commandOutputOverflowV2;
                    } else {
                        return std::nullopt;
                    }
                    update.truncation = data.truncation;
                    return update.content ? std::optional<model::OccurrencePayload>{std::move(update)} : std::nullopt;
                }
                case ExpandedEventType::PendingRequestsUpdated:
                    return model::PendingRequestsUpdatedOccurrence{snapshot.pendingRequests};
                case ExpandedEventType::AccountUpdated:
                    return model::AccountUpdatedOccurrence{snapshot.accounts};
                case ExpandedEventType::ModelsUpdated:
                    return model::ModelsUpdatedOccurrence{snapshot.models};
                case ExpandedEventType::ConfigurationUpdated:
                    return model::ConfigurationUpdatedOccurrence{snapshot.configuration};
                case ExpandedEventType::ProcessUpdated: {
                    if (!selection.processHandle) {
                        return std::nullopt;
                    }
                    const model::ProcessState* process = findProcess(snapshot, *selection.processHandle);
                    return process ? std::optional<model::OccurrencePayload>{model::ProcessUpdatedOccurrence{*process}} : std::nullopt;
                }
                case ExpandedEventType::FilesystemWatchUpdated: {
                    if (!selection.filesystemWatchId) {
                        return std::nullopt;
                    }
                    const model::FilesystemWatchRecord* watch = findFilesystemWatch(snapshot, *selection.filesystemWatchId);
                    return watch ? std::optional<model::OccurrencePayload>{model::FilesystemWatchUpdatedOccurrence{*watch}} : std::nullopt;
                }
                case ExpandedEventType::FuzzySearchUpdated: {
                    if (!selection.fuzzySearchId) {
                        return std::nullopt;
                    }
                    const model::FuzzySearchRecord* search = findFuzzySearch(snapshot, *selection.fuzzySearchId);
                    return search ? std::optional<model::OccurrencePayload>{model::FuzzySearchUpdatedOccurrence{*search}} : std::nullopt;
                }
                case ExpandedEventType::ReviewsUpdated:
                    return model::ReviewsUpdatedOccurrence{snapshot.reviews};
                case ExpandedEventType::IntegrationsUpdated:
                    return model::IntegrationsUpdatedOccurrence{snapshot.integrations};
                case ExpandedEventType::PluginsUpdated:
                    return model::PluginsUpdatedOccurrence{snapshot.plugins};
                case ExpandedEventType::SkillsUpdated:
                    return model::SkillsUpdatedOccurrence{snapshot.skills};
                case ExpandedEventType::McpUpdated:
                    return model::McpUpdatedOccurrence{snapshot.mcp};
                case ExpandedEventType::PlatformUpdated:
                    return model::PlatformUpdatedOccurrence{snapshot.platform};
                case ExpandedEventType::NoticeAdded:
                    return selection.notice.has_value()
                               ? std::optional<model::OccurrencePayload>{model::NoticeAddedOccurrence{*selection.notice}}
                               : std::nullopt;
                case ExpandedEventType::ActivityUpdated: {
                    if (!selection.activityKey) {
                        return std::nullopt;
                    }
                    const model::ActivityRecord* activity = findActivity(snapshot, *selection.activityKey);
                    return activity ? std::optional<model::OccurrencePayload>{model::ActivityUpdatedOccurrence{*activity}} : std::nullopt;
                }
                case ExpandedEventType::CapacityUpdated:
                    return model::CapacityUpdatedOccurrence{snapshot.capacity};
                case ExpandedEventType::DiagnosticsUpdated: {
                    model::DiagnosticRecord diagnostic;
                    diagnostic.received = snapshot.diagnostics.received;
                    diagnostic.detailsOmitted = true;
                    model::DiagnosticsUpdatedOccurrence update{std::move(diagnostic)};
                    update.aggregateEntries = snapshot.diagnostics.entries;
                    return model::OccurrencePayload{std::move(update)};
                }
            }
            return std::nullopt;
        }

        const generated::ProjectionMetadata* notificationProjection(std::string_view method) noexcept {
            constexpr std::string_view Prefix = "server_notification:ServerNotification:method:";
            for (const generated::ProjectionMetadata& metadata : generated::AllNotificationProjections) {
                if (metadata.registryKey.starts_with(Prefix) && metadata.registryKey.substr(Prefix.size()) == method) {
                    return &metadata;
                }
            }
            return nullptr;
        }

        const generated::MethodMetadata* generatedProviderMethod(std::string_view providerMethod) noexcept {
            constexpr std::string_view Marker = ":method:";
            for (const generated::MethodMetadata& metadata : generated::AllMethods) {
                if (metadata.category != generated::MethodCategory::ProviderOperation &&
                    metadata.category != generated::MethodCategory::ReverseResponse) {
                    continue;
                }
                for (const std::string_view registryKey : metadata.registryKeys) {
                    const std::size_t marker = registryKey.find(Marker);
                    if (marker != std::string_view::npos && registryKey.substr(marker + Marker.size()) == providerMethod) {
                        return &metadata;
                    }
                }
            }
            return nullptr;
        }

        std::optional<ExpandedEventType> providerDomainFamily(std::string_view providerMethod) noexcept {
            if (generatedProviderMethod(providerMethod) == nullptr) {
                return std::nullopt;
            }
            if (providerMethod.starts_with("account/")) {
                return ExpandedEventType::AccountUpdated;
            }
            if (providerMethod.starts_with("model/")) {
                return ExpandedEventType::ModelsUpdated;
            }
            if (providerMethod.starts_with("config/") || providerMethod.starts_with("configRequirements/") ||
                providerMethod.starts_with("experimentalFeature/")) {
                return ExpandedEventType::ConfigurationUpdated;
            }
            if (providerMethod.starts_with("command/exec")) {
                return ExpandedEventType::ProcessUpdated;
            }
            if (providerMethod.starts_with("fs/")) {
                return ExpandedEventType::FilesystemWatchUpdated;
            }
            if (providerMethod.starts_with("fuzzyFileSearch/")) {
                return ExpandedEventType::FuzzySearchUpdated;
            }
            if (providerMethod.starts_with("review/") || providerMethod.starts_with("guardian/")) {
                return ExpandedEventType::ReviewsUpdated;
            }
            if (providerMethod.starts_with("app/") || providerMethod.starts_with("externalAgentConfig/") ||
                providerMethod.starts_with("hook/") || providerMethod.starts_with("marketplace/")) {
                return ExpandedEventType::IntegrationsUpdated;
            }
            if (providerMethod.starts_with("plugin/")) {
                return ExpandedEventType::PluginsUpdated;
            }
            if (providerMethod.starts_with("skills/")) {
                return ExpandedEventType::SkillsUpdated;
            }
            if (providerMethod.starts_with("mcp")) {
                return ExpandedEventType::McpUpdated;
            }
            if (providerMethod.starts_with("windowsSandbox/") || providerMethod.starts_with("remoteControl/")) {
                return ExpandedEventType::PlatformUpdated;
            }
            return std::nullopt;
        }

        model::LegacyCompatibilityPayload extensionCompatibility(const backend::CodexExtensionReceived& extension) {
            model::LegacyCompatibilityPayload legacy;
            legacy.kind = model::LegacyCompatibilityKind::CodexExtension;
            model::LegacySafeExtension safe;
            if (extension.method.empty()) {
                throw ProjectionFailure("/events/method", "Codex notification method is empty");
            }
            safe.method = extension.method.substr(0, backend::MaxSnapshotExtensionMethodBytes);
            safe.sensitiveFieldsRedacted = extension.sensitiveFieldsRedacted || !extension.safeProjection;
            if (extension.safeProjection) {
                Json safePayload = extension.payload;
                removeSecretMembers(safePayload, safe.sensitiveFieldsRedacted);
                model::SafeDetailError error = model::SafeDetailError::None;
                std::optional<model::SafeDetail> detail = model::SafeDetail::fromJson(std::move(safePayload), &error);
                if (detail) {
                    safe.params = std::move(*detail);
                } else {
                    safe.sensitiveFieldsRedacted = true;
                    safe.truncation.truncated = true;
                    safe.truncation.omittedPaths.push_back("/params");
                }
            }
            if (extension.decodingError) {
                safe.decodingError = extension.decodingError->substr(0, backend::MaxSnapshotExtensionDecodingErrorBytes);
            }
            if (extension.methodTruncated || extension.method.size() > safe.method.size()) {
                model::LegacySafeExtension::FieldTruncation field;
                field.originalBytes = extension.originalMethodBytes != 0 ? extension.originalMethodBytes
                                                                         : static_cast<std::uint64_t>(extension.method.size());
                field.retainedBytes = static_cast<std::uint64_t>(safe.method.size());
                safe.wireTruncation.method = std::move(field);
            }
            if (extension.payloadTruncated || !extension.safeProjection ||
                std::find(safe.truncation.omittedPaths.begin(), safe.truncation.omittedPaths.end(), "/params") !=
                    safe.truncation.omittedPaths.end()) {
                model::LegacySafeExtension::FieldTruncation field;
                field.originalBytes = extension.originalPayloadBytes;
                safe.wireTruncation.params = std::move(field);
            }
            if (extension.decodingErrorTruncated || extension.originalDecodingErrorBytes > safe.decodingError.value_or("").size()) {
                model::LegacySafeExtension::FieldTruncation field;
                field.originalBytes = extension.originalDecodingErrorBytes;
                field.retainedBytes = static_cast<std::uint64_t>(safe.decodingError.value_or("").size());
                safe.wireTruncation.decodingError = std::move(field);
            }
            safe.truncation.truncated = safe.truncation.truncated || extension.methodTruncated || extension.payloadTruncated ||
                                        extension.decodingErrorTruncated || extension.method.size() > safe.method.size();
            const auto addDropped = [&](std::uint64_t amount) {
                safe.truncation.droppedBytes = amount > std::numeric_limits<std::uint64_t>::max() - safe.truncation.droppedBytes
                                                   ? std::numeric_limits<std::uint64_t>::max()
                                                   : safe.truncation.droppedBytes + amount;
            };
            if (extension.originalMethodBytes > safe.method.size()) {
                addDropped(extension.originalMethodBytes - safe.method.size());
            }
            if (extension.originalPayloadBytes && (extension.payloadTruncated || !extension.safeProjection)) {
                addDropped(*extension.originalPayloadBytes);
            }
            if (extension.originalDecodingErrorBytes > safe.decodingError.value_or("").size()) {
                addDropped(extension.originalDecodingErrorBytes - safe.decodingError.value_or("").size());
            }
            legacy.safeExtension = std::move(safe);
            return legacy;
        }

        OccurrenceCoalescingKey
        occurrenceKey(const model::OccurrencePayload& payload, const OccurrenceSelection& selection, std::string fallback = {}) {
            OccurrenceCoalescingKey key;
            switch (model::occurrenceType(payload)) {
                case ExpandedEventType::ProviderUpdated:
                case ExpandedEventType::AccountUpdated:
                case ExpandedEventType::ModelsUpdated:
                case ExpandedEventType::ConfigurationUpdated:
                case ExpandedEventType::ReviewsUpdated:
                case ExpandedEventType::IntegrationsUpdated:
                case ExpandedEventType::PluginsUpdated:
                case ExpandedEventType::SkillsUpdated:
                case ExpandedEventType::McpUpdated:
                case ExpandedEventType::PlatformUpdated:
                case ExpandedEventType::CapacityUpdated:
                    key.kind = OccurrenceEntityKind::BackendLifecycle;
                    key.entityId = fallback.empty() ? std::string(toString(model::occurrenceType(payload))) : std::move(fallback);
                    break;
                case ExpandedEventType::ControllerUpdated:
                    key.kind = OccurrenceEntityKind::Controller;
                    key.entityId = "controller";
                    break;
                case ExpandedEventType::SessionsUpdated:
                    key.kind = OccurrenceEntityKind::Session;
                    key.entityId = selection.sessionId ? selection.sessionId->value() : "sessions";
                    break;
                case ExpandedEventType::ThreadListUpdated:
                    key.kind = OccurrenceEntityKind::Thread;
                    key.entityId = "thread-list";
                    break;
                case ExpandedEventType::ThreadUpserted:
                case ExpandedEventType::ThreadRemoved:
                    key.kind = OccurrenceEntityKind::Thread;
                    key.threadId = selection.threadId;
                    key.entityId = selection.threadId ? selection.threadId->value() : std::move(fallback);
                    break;
                case ExpandedEventType::TurnUpserted:
                    key.kind = OccurrenceEntityKind::Turn;
                    key.threadId = selection.threadId;
                    key.turnId = selection.turnId;
                    key.entityId = selection.turnId ? selection.turnId->value() : std::move(fallback);
                    break;
                case ExpandedEventType::ItemUpserted:
                    key.kind = OccurrenceEntityKind::Item;
                    key.threadId = selection.threadId;
                    key.turnId = selection.turnId;
                    key.itemId = selection.itemId;
                    key.entityId = selection.itemId ? selection.itemId->value() : std::move(fallback);
                    break;
                case ExpandedEventType::ItemContentUpdated:
                    key.kind = OccurrenceEntityKind::ItemContent;
                    key.threadId = selection.threadId;
                    key.turnId = selection.turnId;
                    key.itemId = selection.itemId;
                    key.entityId = selection.itemId ? selection.itemId->value() : std::move(fallback);
                    key.channel = selection.channel.value_or("");
                    break;
                case ExpandedEventType::PendingRequestsUpdated:
                    key.kind = OccurrenceEntityKind::PendingRequest;
                    key.pendingRequestId = selection.pendingRequestId;
                    key.entityId = selection.pendingRequestId ? selection.pendingRequestId->value() : "pending-requests";
                    break;
                case ExpandedEventType::ProcessUpdated:
                    key.kind = OccurrenceEntityKind::BackendLifecycle;
                    key.entityId = selection.processHandle ? "process:" + selection.processHandle->value() : std::move(fallback);
                    break;
                case ExpandedEventType::FilesystemWatchUpdated:
                    key.kind = OccurrenceEntityKind::BackendLifecycle;
                    key.entityId = selection.filesystemWatchId ? "watch:" + *selection.filesystemWatchId : std::move(fallback);
                    break;
                case ExpandedEventType::FuzzySearchUpdated:
                    key.kind = OccurrenceEntityKind::BackendLifecycle;
                    key.entityId = selection.fuzzySearchId ? "search:" + *selection.fuzzySearchId : std::move(fallback);
                    break;
                case ExpandedEventType::NoticeAdded:
                case ExpandedEventType::ActivityUpdated:
                case ExpandedEventType::DiagnosticsUpdated:
                    key.kind = OccurrenceEntityKind::Diagnostic;
                    key.entityId = fallback.empty() ? std::string(toString(model::occurrenceType(payload))) : std::move(fallback);
                    break;
            }
            return key;
        }

        void attachOccurrenceIdentities(model::OccurrenceDraft& occurrence, const OccurrenceSelection& selection) {
            occurrence.sessionId = selection.sessionId;
            occurrence.threadId = selection.threadId;
            occurrence.turnId = selection.turnId;
            occurrence.itemId = selection.itemId;
            occurrence.pendingRequestId = selection.pendingRequestId;
            occurrence.processHandle = selection.processHandle;
        }

        std::string itemContentChannel(backend::ItemContentChanged::Kind kind) {
            switch (kind) {
                case backend::ItemContentChanged::Kind::AgentText:
                    return "agentText";
                case backend::ItemContentChanged::Kind::ReasoningText:
                    return "reasoningText";
                case backend::ItemContentChanged::Kind::ReasoningSummary:
                    return "reasoningSummary";
                case backend::ItemContentChanged::Kind::CommandOutput:
                    return "commandOutput";
            }
            return "agentText";
        }

        backend::ItemContentSnapshotChannel itemContentSnapshotChannel(backend::ItemContentChanged::Kind kind) noexcept {
            switch (kind) {
                case backend::ItemContentChanged::Kind::AgentText:
                    return backend::ItemContentSnapshotChannel::AgentText;
                case backend::ItemContentChanged::Kind::ReasoningText:
                    return backend::ItemContentSnapshotChannel::ReasoningText;
                case backend::ItemContentChanged::Kind::ReasoningSummary:
                    return backend::ItemContentSnapshotChannel::ReasoningSummary;
                case backend::ItemContentChanged::Kind::CommandOutput:
                    return backend::ItemContentSnapshotChannel::CommandOutput;
            }
            return backend::ItemContentSnapshotChannel::AgentText;
        }

        void projectSelectedItemContent(model::ItemContentUpdatedOccurrence& update,
                                        std::string_view source,
                                        std::uint64_t droppedContentBytes,
                                        bool contentTruncated,
                                        backend::ItemContentSnapshotChannel selectedChannel,
                                        std::uint8_t frontendOmittedContentChannels = 0) {
            if (source.empty()) {
                return;
            }
            constexpr std::size_t MaximumContentCharacters = 16U * 1024U;
            update.truncation.truncated = contentTruncated;
            update.truncation.droppedBytes = droppedContentBytes;
            std::string retained = boundedFrontendString(source, MaximumContentCharacters);
            if (retained.size() != source.size()) {
                if (selectedChannel == backend::ItemContentSnapshotChannel::AgentText &&
                    source.size() <= model::MaximumItemContentOverflowV1Bytes && validUtf8(source)) {
                    update.overflowV1 = model::ItemContentOverflowV1{static_cast<std::uint64_t>(retained.size()),
                                                                    std::string(source.substr(retained.size())),
                                                                    droppedContentBytes,
                                                                    contentTruncated,
                                                                    contentTruncated};
                }
                if (selectedChannel == backend::ItemContentSnapshotChannel::CommandOutput &&
                    source.size() <= model::MaximumCommandOutputOverflowV2Bytes && validUtf8(source)) {
                    update.overflowV1 = model::ItemContentOverflowV1{static_cast<std::uint64_t>(retained.size()),
                                                                    std::string(source.substr(retained.size())),
                                                                    droppedContentBytes,
                                                                    contentTruncated,
                                                                    contentTruncated};
                }
                saturatingAdd(update.truncation.droppedBytes,
                              static_cast<std::uint64_t>(source.size() - retained.size()));
                update.truncation.truncated = true;
                frontendOmittedContentChannels |=
                    static_cast<std::uint8_t>(1U << static_cast<unsigned int>(selectedChannel));
            }
            constexpr std::array<std::pair<backend::ItemContentSnapshotChannel, std::string_view>, 4> ContentPaths{{
                {backend::ItemContentSnapshotChannel::AgentText, "/agentText"},
                {backend::ItemContentSnapshotChannel::ReasoningText, "/reasoningText"},
                {backend::ItemContentSnapshotChannel::ReasoningSummary, "/reasoningSummary"},
                {backend::ItemContentSnapshotChannel::CommandOutput, "/commandOutput"},
            }};
            for (const auto& [channel, path] : ContentPaths) {
                if ((frontendOmittedContentChannels &
                     static_cast<std::uint8_t>(1U << static_cast<unsigned int>(channel))) != 0) {
                    recordOmission(update.truncation, std::string(path));
                }
            }
            update.content = std::move(retained);
        }

        void retainItemContentAppendHint(model::ItemContentUpdatedOccurrence& update,
                                         const backend::ItemContentChanged& event,
                                         std::uint64_t currentBackendDroppedBytes,
                                         std::string_view fullContent) {
            if (!update.content.has_value() || !event.channelBytesBefore.has_value() ||
                !event.droppedContentBytesBefore.has_value() ||
                currentBackendDroppedBytes < *event.droppedContentBytesBefore || !validUtf8(fullContent)) {
                return;
            }

            const std::size_t base = *event.channelBytesBefore;
            const std::uint64_t newlyDropped = currentBackendDroppedBytes - *event.droppedContentBytesBefore;
            const std::size_t discard = static_cast<std::size_t>(std::min<std::uint64_t>(newlyDropped, base));
            const std::size_t retainedOldBytes = base - discard;
            if (retainedOldBytes > fullContent.size()) {
                return;
            }
            const std::size_t retainedDeltaBytes = fullContent.size() - retainedOldBytes;
            if (event.delta.size() < retainedDeltaBytes) {
                return;
            }
            const std::size_t retainedDeltaOffset = event.delta.size() - retainedDeltaBytes;
            if (fullContent.compare(retainedOldBytes, retainedDeltaBytes, event.delta, retainedDeltaOffset, retainedDeltaBytes) != 0) {
                return;
            }
            update.appendHint = model::ItemContentAppendHint{
                static_cast<std::uint64_t>(base),
                std::string(fullContent.substr(retainedOldBytes, retainedDeltaBytes)),
                static_cast<std::uint64_t>(discard),
                true};
        }

        template <typename>
        inline constexpr bool AlwaysFalse = false;

    } // namespace

    model::ModelResult<model::CanonicalSnapshot> BackendProjection::projectSnapshot(const backend::Snapshot& snapshot) const noexcept {
        try {
            model::CanonicalSnapshot projected;
            projected.backendCursor.backendRevision = snapshot.sequence.value();
            projected.backendCursor.backendSequenceExhausted = snapshot.sequenceExhausted;
            projected.backendCursor.sourceStamp = model::SourceStamp{"backend-snapshot:" + std::to_string(snapshot.sequence.value())};

            projected.provider.lifecycle = providerLifecycle(snapshot.provider.lifecycle);
            projected.provider.generation = snapshot.provider.generation;
            projected.provider.desiredRunning = snapshot.provider.desiredRunning;
            projected.provider.recovery.status = recoveryStatus(snapshot.provider.recovery.status);
            projected.provider.recovery.attempts = snapshot.provider.recovery.attempts;
            projected.provider.recovery.delayMs = snapshot.provider.recovery.delayMs;
            if (snapshot.provider.lastError) {
                const bool messageAvailable = !snapshot.provider.lastError->message.empty();
                Json error{{"category",
                            boundedFrontendString(snapshot.provider.lastError->category,
                                                  16'384,
                                                  &projected.truncation,
                                                  "/provider/lastError/category")},
                           {"code", snapshot.provider.lastError->code},
                           {"message",
                            messageAvailable
                                ? boundedFrontendString(snapshot.provider.lastError->message,
                                                        16'384,
                                                        &projected.truncation,
                                                        "/provider/lastError/message")
                                : "Codex App Server reported an error"},
                           {"detailsOmitted", !messageAvailable}};
                projected.provider.lastError = boundedDetail(
                    boundedFrontendDetailObject(error, &projected.truncation, "/provider/lastError"),
                    projected.truncation,
                    "/provider/lastError");
            }
            if (snapshot.provider.initialization) {
                const Json initialization{{"codexHome", snapshot.provider.initialization->codexHome},
                                          {"platformFamily", snapshot.provider.initialization->platformFamily},
                                          {"platformOs", snapshot.provider.initialization->platformOs},
                                          {"userAgent", snapshot.provider.initialization->userAgent}};
                projected.provider.initialization = boundedDetail(
                    boundedFrontendDetailObject(initialization, &projected.truncation, "/provider/initialization"),
                    projected.truncation,
                    "/provider/initialization");
            }

            if (snapshot.controller) {
                const std::string id = std::to_string(snapshot.controller->value());
                projected.controller.session = requiredIdentifier<model::SessionIdentity>(id, "/controller/sessionId");
            }
            const std::size_t sessionCount = std::min(snapshot.sessions.size(), MaximumFrontendSessions);
            projected.sessions.reserve(sessionCount);
            for (std::size_t sessionIndex = 0; sessionIndex < sessionCount; ++sessionIndex) {
                const backend::SessionSnapshot& session = snapshot.sessions[sessionIndex];
                model::SessionState value(
                    requiredIdentifier<model::SessionIdentity>(std::to_string(session.id.value()), "/sessions/sessionId"));
                value.role = sessionRole(session.role);
                projected.sessions.push_back(std::move(value));
            }
            recordOmittedEntries(projected.truncation, "/sessions", snapshot.sessions.size() - sessionCount);

            projected.threadList.hasLoadedPage = snapshot.threadList.hasLoadedPage;
            projected.threadList.complete = snapshot.threadList.complete;
            if (snapshot.threadList.nextCursor) {
                projected.threadList.nextCursor = boundedFrontendString(
                    *snapshot.threadList.nextCursor, 16'384, &projected.truncation, "/threadList/nextCursor");
            }
            if (snapshot.threadList.backwardsCursor) {
                projected.threadList.backwardsCursor = boundedFrontendString(
                    *snapshot.threadList.backwardsCursor, 16'384, &projected.truncation, "/threadList/backwardsCursor");
            }
            projected.threadList.pagesLoaded = snapshot.threadList.pagesLoaded;
            projected.threadList.stamp = sourceMetadata(snapshot.threadList.stamp);

            const std::size_t threadCount = std::min(snapshot.threads.size(), MaximumFrontendThreads);
            projected.threads.reserve(threadCount);
            for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
                const backend::ThreadSnapshot& thread = snapshot.threads[threadIndex];
                const model::ThreadIdentity threadId = requiredIdentifier<model::ThreadIdentity>(thread.id, "/threads/id");
                model::ThreadState threadState(threadId);
                if (thread.title) {
                    threadState.title =
                        boundedFrontendString(*thread.title, 16'384, &projected.truncation, "/threads/title");
                }
                threadState.createdAtMs = thread.createdAt;
                threadState.updatedAtMs = thread.updatedAt;
                threadState.fullyLoaded = thread.fullyLoaded;
                threadState.freshness = freshness(thread.stamp.freshness);
                threadState.stamp = sourceMetadata(thread.stamp);
                Json threadDetails = Json::object();
                if (thread.cwd) {
                    threadDetails["cwd"] =
                        boundedFrontendString(*thread.cwd, 16'384, &projected.truncation, "/threads/cwd");
                }
                if (thread.model) {
                    threadDetails["model"] =
                        boundedFrontendString(*thread.model, 1'024, &projected.truncation, "/threads/model");
                }
                if (thread.modelProvider) {
                    threadDetails["modelProvider"] = boundedFrontendString(
                        *thread.modelProvider, 1'024, &projected.truncation, "/threads/modelProvider");
                }
                if (thread.preview) {
                    threadDetails["preview"] =
                        boundedFrontendString(*thread.preview, 16'384, &projected.truncation, "/threads/preview");
                }
                if (thread.status) {
                    threadDetails["status"] =
                        boundedFrontendString(*thread.status, 256, &projected.truncation, "/threads/status");
                }
                if (thread.ephemeral) {
                    threadDetails["ephemeral"] = *thread.ephemeral;
                }
                if (thread.archived) {
                    threadDetails["archived"] = *thread.archived;
                }
                if (thread.executionConfiguration) {
                    threadDetails["executionConfiguration"] = *thread.executionConfiguration;
                }
                Json realtime{{"lifecycle",
                               boundedFrontendString(thread.realtime.lifecycle,
                                                     16'384,
                                                     &projected.truncation,
                                                     "/threads/realtime/lifecycle")},
                              {"transcript",
                               boundedFrontendString(thread.realtime.transcript,
                                                     16'384,
                                                     &projected.truncation,
                                                     "/threads/realtime/transcript")},
                              {"itemCount", thread.realtime.itemCount},
                              {"receivedAudioBytes", thread.realtime.receivedAudioBytes},
                              {"droppedAudioBytes", thread.realtime.droppedAudioBytes},
                              {"transcriptTruncated", thread.realtime.transcriptTruncated},
                              {"sourceGeneration", thread.realtime.stamp.generation},
                              {"sourceFreshness", freshnessName(thread.realtime.stamp.freshness)}};
                if (thread.realtime.lastError.has_value()) {
                    realtime["lastError"] = boundedFrontendString(
                        *thread.realtime.lastError, 16'384, &projected.truncation, "/threads/realtime/lastError");
                    realtime["errorDetailsOmitted"] = false;
                }
                if (thread.realtime.sessionId.has_value()) {
                    realtime["sessionId"] = boundedFrontendString(
                        *thread.realtime.sessionId, 16'384, &projected.truncation, "/threads/realtime/sessionId");
                }
                if (thread.realtime.version.has_value()) {
                    realtime["version"] = boundedFrontendString(
                        *thread.realtime.version, 16'384, &projected.truncation, "/threads/realtime/version");
                }
                if (thread.realtime.lastSdpBytes.has_value()) {
                    realtime["lastSdpBytes"] = *thread.realtime.lastSdpBytes;
                }
                threadDetails["realtime"] = std::move(realtime);
                threadState.safeDetails = boundedDetail(std::move(threadDetails), projected.truncation, "/threads/details");
                threadState.legacyExtensions = boundedDetail(thread.extensions, projected.truncation, "/threads/extensions");
                projected.threads.push_back(std::move(threadState));

                for (std::size_t turnIndex = 0; turnIndex < thread.turns.size(); ++turnIndex) {
                    if (projected.turns.size() == MaximumFrontendTurns) {
                        recordOmittedEntries(projected.truncation, "/turns", thread.turns.size() - turnIndex);
                        break;
                    }
                    const backend::TurnSnapshot& turn = thread.turns[turnIndex];
                    const model::TurnIdentity turnId = requiredIdentifier<model::TurnIdentity>(turn.id, "/turns/id");
                    const model::ThreadIdentity turnThreadId =
                        requiredIdentifier<model::ThreadIdentity>(turn.threadId, "/turns/threadId");
                    model::TurnState turnState(turnId, turnThreadId);
                    if (!turn.status.empty()) {
                        turnState.status =
                            boundedFrontendString(turn.status, 256, &projected.truncation, "/turns/status");
                    }
                    turnState.active = turn.active;
                    turnState.terminal = turn.terminal;
                    turnState.stamp = sourceMetadata(turn.stamp);
                    turnState.connectionInvalidated = turn.connectionInvalidated;
                    Json turnDetails = Json::object();
                    if (turn.failure) {
                        addTurnFailureSemanticDetails(turnDetails, *turn.failure, projected.truncation);
                    }
                    if (turn.tokenUsage) {
                        addTurnTokenUsageSemanticDetails(turnDetails, *turn.tokenUsage, projected.truncation);
                    }
                    if (turn.plan) {
                        model::TurnPlanState plan;
                        plan.explanation = turn.plan->explanation;
                        plan.totalSteps = turn.plan->totalSteps;
                        plan.truncated = turn.plan->truncated;
                        plan.steps.reserve(turn.plan->steps.size());
                        for (const backend::TurnPlanStepState& step : turn.plan->steps) {
                            plan.steps.push_back({step.step, step.status.value});
                        }
                        turnState.plan = std::move(plan);
                    }
                    if (turn.effectiveExecutionConfiguration) {
                        turnDetails["effectiveExecutionConfiguration"] = *turn.effectiveExecutionConfiguration;
                    }
                    if (turn.effectiveExecutionConfigurationProvenance) {
                        turnDetails["effectiveExecutionConfigurationProvenance"] =
                            *turn.effectiveExecutionConfigurationProvenance;
                    }
                    turnState.safeDetails = boundedDetail(std::move(turnDetails), projected.truncation, "/turns/details");
                    turnState.legacyExtensions = boundedDetail(turn.extensions, projected.truncation, "/turns/extensions");
                    projected.turns.push_back(std::move(turnState));
                    for (std::size_t itemIndex = 0; itemIndex < turn.items.size(); ++itemIndex) {
                        const backend::ItemSnapshot& item = turn.items[itemIndex];
                        std::optional<model::ThreadItem> projectedItem =
                            projectItem(item, turnThreadId, turnId, projected.truncation, itemIndex);
                        if (!projectedItem) {
                            const std::string path = "/threads/" + std::to_string(threadIndex) + "/turns/" +
                                                     std::to_string(turnIndex) + "/items/" + std::to_string(itemIndex);
                            projected.legacyItems.push_back(
                                projectLegacyItem(item, turnThreadId, turnId, itemIndex, path));
                            continue;
                        }
                        if (projected.items.size() < MaximumFrontendItems) {
                            projected.items.push_back(std::move(*projectedItem));
                        } else {
                            recordOmittedEntries(projected.truncation, "/items", 1);
                        }
                    }
                }
            }
            recordOmittedEntries(projected.truncation, "/threads", snapshot.threads.size() - threadCount);

            const std::size_t pendingCount = std::min(snapshot.pendingRequests.size(), MaximumFrontendPendingRequests);
            projected.pendingRequests.reserve(pendingCount);
            for (std::size_t pendingIndex = 0; pendingIndex < pendingCount; ++pendingIndex) {
                const backend::PendingRequestSnapshot& pending = snapshot.pendingRequests[pendingIndex];
                if (backendPendingKind(pending.type).has_value()) {
                    projected.pendingRequests.push_back(projectPending(pending, projected.truncation, pendingIndex));
                } else {
                    projected.legacyPendingRequests.push_back(projectLegacyPending(pending,
                                                                                   pendingIndex,
                                                                                   "/pendingRequests/" +
                                                                                       std::to_string(pendingIndex)));
                }
            }
            recordOmittedEntries(projected.truncation,
                                 "/pendingRequests",
                                 snapshot.pendingRequests.size() - pendingCount);

            projected.accounts.state = projectDomain(snapshot.accounts, accountDetails(snapshot.accounts));
            projected.models.state = projectDomain(snapshot.models);
            projected.configuration.state = projectDomain(snapshot.configuration, configurationDetails(snapshot.configuration));
            projected.permissionProfiles.state = projectDomain(snapshot.reviews);
            projected.reviews.state = projectDomain(snapshot.reviews);
            const Json integrationDetails = integrationsDetails(snapshot.integrations);
            projected.apps.state = projectDomain(snapshot.integrations, integrationDetails);
            projected.externalAgents.state = projectDomain(snapshot.integrations, integrationDetails);
            projected.hooks.state = projectDomain(snapshot.integrations, integrationDetails);
            projected.marketplace.state = projectDomain(snapshot.integrations, integrationDetails);
            projected.integrations.state = projectDomain(snapshot.integrations, integrationDetails);
            const Json pluginDetails = pluginsDetails(snapshot.pluginsAndSkills);
            projected.plugins.state = projectDomain(snapshot.pluginsAndSkills, pluginDetails);
            projected.skills.state = projectDomain(snapshot.pluginsAndSkills, pluginDetails);
            projected.mcp.state = projectDomain(snapshot.mcp, mcpDetails(snapshot.mcp));
            const Json projectedPlatformDetails = platformDetails(snapshot.platform);
            projected.windowsSandbox.state = projectDomain(snapshot.platform, projectedPlatformDetails);
            projected.platform.state = projectDomain(snapshot.platform, projectedPlatformDetails);
            projected.remoteControl.state = projectDomain(snapshot.platform, projectedPlatformDetails);

            const std::size_t processCount = std::min(snapshot.processes.size(), MaximumFrontendProcesses);
            projected.processes.reserve(processCount);
            for (std::size_t processIndex = 0; processIndex < processCount; ++processIndex) {
                const backend::ProcessSnapshot& process = snapshot.processes[processIndex];
                model::ProcessState value(requiredIdentifier<model::ProcessHandle>(process.processHandle, "/processes/processHandle"));
                value.lifecycle = process.lifecycle;
                value.status = process.lifecycle;
                value.stdoutBytes = process.stdoutBytes;
                value.stderrBytes = process.stderrBytes;
                value.stdoutTruncated = process.stdoutTruncated;
                value.stderrTruncated = process.stderrTruncated;
                value.droppedOutputBytes = process.droppedOutputBytes;
                value.exitCode = process.exitCode;
                value.terminal = process.exitCode.has_value();
                value.stamp = sourceMetadata(process.stamp);
                value.connectionInvalidated = process.connectionInvalidated;
                value.truncation.truncated = process.stdoutTruncated || process.stderrTruncated;
                value.truncation.droppedBytes = process.droppedOutputBytes;
                projected.processes.push_back(std::move(value));
            }
            recordOmittedEntries(projected.truncation, "/processes", snapshot.processes.size() - processCount);

            projected.filesystemWatches.state = model::DomainState::present();
            projected.filesystemWatches.state.complete = true;
            const std::size_t watchCount = std::min(snapshot.filesystemWatches.size(), MaximumFrontendFilesystemWatches);
            projected.filesystemWatches.entries.reserve(watchCount);
            for (std::size_t watchIndex = 0; watchIndex < watchCount; ++watchIndex) {
                const backend::FilesystemWatchSnapshot& watch = snapshot.filesystemWatches[watchIndex];
                model::FilesystemWatchRecord value;
                value.watchId = watch.watchId;
                value.root = watch.root;
                value.changedPathCount = watch.changedPathCount;
                value.stamp = sourceMetadata(watch.stamp);
                value.connectionInvalidated = watch.connectionInvalidated;
                projected.filesystemWatches.entries.push_back(std::move(value));
            }
            recordOmittedEntries(
                projected.truncation, "/filesystemWatches", snapshot.filesystemWatches.size() - watchCount);

            projected.fuzzySearches.state = model::DomainState::present();
            projected.fuzzySearches.state.complete = true;
            const std::size_t searchCount = std::min(snapshot.fuzzySearchSessions.size(), MaximumFrontendFuzzySearches);
            projected.fuzzySearches.entries.reserve(searchCount);
            for (std::size_t searchIndex = 0; searchIndex < searchCount; ++searchIndex) {
                const backend::FuzzySearchSnapshot& search = snapshot.fuzzySearchSessions[searchIndex];
                model::FuzzySearchRecord value;
                value.sessionId = search.sessionId;
                value.resultCount = search.resultCount;
                value.complete = search.complete;
                value.stamp = sourceMetadata(search.stamp);
                value.connectionInvalidated = search.connectionInvalidated;
                projected.fuzzySearches.entries.push_back(std::move(value));
            }
            recordOmittedEntries(
                projected.truncation, "/fuzzySearches", snapshot.fuzzySearchSessions.size() - searchCount);

            projected.notices.state = model::DomainState::present();
            projected.notices.state.complete = true;
            const std::size_t noticeCount = std::min(snapshot.notices.size(), MaximumFrontendNotices);
            projected.notices.entries.reserve(noticeCount);
            for (std::size_t noticeIndex = 0; noticeIndex < noticeCount; ++noticeIndex) {
                const backend::NoticeSnapshot& notice = snapshot.notices[noticeIndex];
                model::NoticeRecord value;
                value.occurrence = notice.occurrence;
                value.category = noticeCategory(notice.category);
                value.summary =
                    boundedFrontendString(notice.summary, 16'384, &projected.truncation, "/notices/summary");
                if (notice.details) {
                    value.details =
                        boundedFrontendString(*notice.details, 16'384, &projected.truncation, "/notices/details");
                }
                if (notice.threadId) {
                    value.threadId = requiredIdentifier<model::ThreadIdentity>(*notice.threadId, "/notices/threadId");
                }
                value.stamp = sourceMetadata(notice.stamp);
                projected.notices.entries.push_back(std::move(value));
            }
            recordOmittedEntries(projected.truncation, "/notices", snapshot.notices.size() - noticeCount);

            projected.activities.state = model::DomainState::present();
            projected.activities.state.complete = true;
            const std::size_t activityCount = std::min(snapshot.activities.size(), MaximumFrontendActivities);
            projected.activities.entries.reserve(activityCount);
            for (std::size_t activityIndex = 0; activityIndex < activityCount; ++activityIndex) {
                const backend::ActivitySnapshot& activity = snapshot.activities[activityIndex];
                model::ActivityRecord value;
                value.key = activity.key;
                value.subjectId = activity.subjectId;
                value.kind = boundedFrontendString(activity.kind, 256, &projected.truncation, "/activities/kind");
                value.lifecycle =
                    boundedFrontendString(activity.lifecycle, 256, &projected.truncation, "/activities/lifecycle");
                if (activity.summary) {
                    value.summary = boundedFrontendString(
                        *activity.summary, 16'384, &projected.truncation, "/activities/summary");
                }
                if (activity.details) {
                    value.details = boundedFrontendString(
                        *activity.details, 16'384, &projected.truncation, "/activities/details");
                }
                value.active = activity.active;
                if (activity.threadId) {
                    value.threadId = requiredIdentifier<model::ThreadIdentity>(*activity.threadId, "/activities/threadId");
                }
                if (activity.turnId) {
                    value.turnId = requiredIdentifier<model::TurnIdentity>(*activity.turnId, "/activities/turnId");
                }
                value.stamp = sourceMetadata(activity.stamp);
                projected.activities.entries.push_back(std::move(value));
            }
            recordOmittedEntries(projected.truncation, "/activities", snapshot.activities.size() - activityCount);

            projected.diagnostics.state = model::DomainState::present();
            projected.diagnostics.state.complete = true;
            projected.diagnostics.received = snapshot.diagnostics.received;
            projected.diagnostics.entries.reserve(snapshot.diagnostics.recent.size());
            for (const std::string& message : snapshot.diagnostics.recent) {
                model::DiagnosticRecord value;
                value.received = snapshot.diagnostics.received;
                value.message =
                    boundedFrontendString(message, 16'384, &projected.truncation, "/diagnostics/recent");
                projected.diagnostics.entries.push_back(std::move(value));
            }

            projected.capacity.sessions = snapshot.sessions.size();
            projected.capacity.observers = static_cast<std::size_t>(
                std::count_if(snapshot.sessions.begin(), snapshot.sessions.end(), [](const backend::SessionSnapshot& session) {
                    return session.role == backend::SessionRole::Observer;
                }));
            projected.capacity.pendingRequests = snapshot.pendingRequests.size();
            projected.capacity.retainedThreads = snapshot.capacity.retainedThreads;
            projected.capacity.retainedTurns = snapshot.capacity.retainedTurns;
            projected.capacity.retainedItems = snapshot.capacity.retainedItems;
            projected.capacity.accumulatedContentBytes = snapshot.capacity.accumulatedContentBytes;
            projected.capacity.retainedNotices = snapshot.capacity.retainedNotices;
            projected.capacity.retainedProcesses = snapshot.capacity.retainedProcesses;
            projected.capacity.accumulatedProcessOutputBytes = snapshot.capacity.accumulatedProcessOutputBytes;
            projected.capacity.retainedFilesystemWatches = snapshot.capacity.retainedFilesystemWatches;
            projected.capacity.retainedFuzzySearchSessions = snapshot.capacity.retainedFuzzySearchSessions;
            projected.capacity.retainedActivityRecords = snapshot.capacity.retainedActivityRecords;
            projected.capacity.evictedNotices = snapshot.capacity.state.evictedNotices;
            projected.capacity.evictedProcesses = snapshot.capacity.state.evictedProcesses;
            projected.capacity.droppedProcessOutputBytes = snapshot.capacity.state.droppedProcessOutputBytes;
            projected.capacity.evictedFilesystemWatches = snapshot.capacity.state.evictedFilesystemWatches;
            projected.capacity.evictedFuzzySearchSessions = snapshot.capacity.state.evictedFuzzySearchSessions;
            projected.capacity.evictedActivityRecords = snapshot.capacity.state.evictedActivityRecords;
            std::size_t omittedEntries = projected.truncation.omittedEntries.value_or(0);
            saturatingAdd(omittedEntries, snapshot.capacity.omittedThreads);
            saturatingAdd(omittedEntries, snapshot.capacity.omittedTurns);
            saturatingAdd(omittedEntries, snapshot.capacity.omittedItems);
            saturatingAdd(omittedEntries, snapshot.omittedRecentExtensions);
            std::uint64_t droppedBytes = snapshot.capacity.state.droppedContentBytes;
            saturatingAdd(droppedBytes, snapshot.capacity.state.droppedProcessOutputBytes);
            projected.truncation.truncated =
                projected.truncation.truncated || snapshot.capacity.truncated || omittedEntries != 0 || droppedBytes != 0;
            projected.truncation.omittedEntries = omittedEntries;
            projected.truncation.droppedBytes = droppedBytes;
            projected.processesState.truncation = projected.truncation;
            projected.filesystemWatches.state.truncation = projected.truncation;
            projected.fuzzySearches.state.truncation = projected.truncation;
            projected.notices.state.truncation = projected.truncation;
            projected.activities.state.truncation = projected.truncation;

            Json extensions{{"omittedCodexExtensions", snapshot.omittedRecentExtensions}};
            Json encodedExtensions = Json::array();
            for (const backend::ExtensionSnapshot& extension : snapshot.recentExtensions) {
                Json encoded{{"method", extension.method}, {"params", extension.payload}};
                if (extension.decodingError) {
                    encoded["decodingError"] = *extension.decodingError;
                }
                if (extension.sensitiveFieldsRedacted) {
                    encoded["sensitiveFieldsRedacted"] = true;
                }
                Json truncation = Json::object();
                if (extension.methodTruncated) {
                    truncation["method"] =
                        Json{{"originalBytes", extension.originalMethodBytes}, {"retainedBytes", extension.method.size()}};
                }
                if (extension.payloadTruncated) {
                    truncation["params"] = Json::object();
                    if (extension.originalPayloadBytes) {
                        truncation["params"]["originalBytes"] = *extension.originalPayloadBytes;
                    }
                }
                if (extension.decodingErrorTruncated) {
                    truncation["decodingError"] = Json{{"originalBytes", extension.originalDecodingErrorBytes},
                                                       {"retainedBytes", extension.decodingError ? extension.decodingError->size() : 0}};
                }
                if (!truncation.empty()) {
                    encoded["truncation"] = std::move(truncation);
                }
                encodedExtensions.push_back(std::move(encoded));
            }
            extensions["codexExtensions"] = std::move(encodedExtensions);
            projected.stateExtensions = boundedDetail(std::move(extensions), projected.truncation, "/stateExtensions");
            projected.extensions = boundedDetail(semanticProjection(snapshot), projected.truncation, "/extensions/semanticProjection");
            projected.projection.sourceStamp = projected.backendCursor.sourceStamp;
            return projected;
        } catch (const ProjectionFailure& failure) {
            return model::ModelError{model::ModelErrorCode::InvalidIdentifier, failure.path(), failure.what()};
        } catch (const std::exception& error) {
            return model::ModelError{model::ModelErrorCode::InvalidShape, "/", error.what()};
        } catch (...) {
            return model::ModelError{model::ModelErrorCode::InvalidShape, "/", "backend snapshot projection failed"};
        }
    }

    model::ModelResult<ProjectedBackendBatch>
    BackendProjection::projectItemContentOccurrences(std::span<const backend::SequencedBackendEvent> events,
                                                     std::span<const backend::ItemContentSnapshot> items,
                                                     bool allowTerminalSnapshots) const noexcept {
        try {
            if (events.size() != items.size()) {
                return model::ModelError{
                    model::ModelErrorCode::InvalidShape, "/events", "content event and item snapshot counts differ"};
            }

            ProjectedBackendBatch projected;
            projected.occurrences.reserve(events.size());
            std::optional<backend::SequenceNumber> previousSequence;
            for (std::size_t index = 0; index < events.size(); ++index) {
                if (previousSequence && events[index].sequence < *previousSequence) {
                    return model::ModelError{
                        model::ModelErrorCode::InvalidShape, "/events/sequence", "backend events are not in deterministic sequence order"};
                }
                previousSequence = events[index].sequence;
                const auto* event = std::get_if<backend::ItemContentChanged>(&events[index].event);
                if (event == nullptr) {
                    return model::ModelError{
                        model::ModelErrorCode::InvalidShape, "/events", "content fast path received a non-content event"};
                }
                const backend::ItemContentSnapshot& item = items[index];
                const bool activeItem = item.connectionInvalidated || item.status == "started" || item.status == "unknown";
                const backend::ItemContentSnapshotKey expectedKey{
                    event->threadId, event->turnId, event->itemId, itemContentSnapshotChannel(event->kind)};
                // An ahead exact-item read may already include the queued
                // item/completed event. The bridge opts into that terminal
                // replacement only while it also records covered sequences.
                if (item.key != expectedKey || !item.knownType || (!activeItem && !allowTerminalSnapshots)) {
                    return model::ModelError{
                        model::ModelErrorCode::InvalidShape,
                        "/events/item",
                        "content event does not resolve to one active known exact item"};
                }
                const std::optional<model::ThreadIdentity> threadId = model::ThreadIdentity::parse(event->threadId.value);
                const std::optional<model::TurnIdentity> turnId = model::TurnIdentity::parse(event->turnId.value);
                const std::optional<model::ItemIdentity> itemId = model::ItemIdentity::parse(event->itemId.value);
                if (!threadId || !turnId || !itemId) {
                    return model::ModelError{
                        model::ModelErrorCode::InvalidIdentifier, "/events/item", "content event has an invalid composite identity"};
                }

                model::ItemContentUpdatedOccurrence update{*itemId};
                update.threadId = threadId;
                update.turnId = turnId;
                update.channel = itemContentChannel(event->kind);
                update.itemKind = backendItemKind(item.type);
                projectSelectedItemContent(update,
                                           item.content,
                                           item.droppedContentBytes,
                                           item.contentTruncated,
                                           item.key.channel,
                                           item.frontendOmittedContentChannels);
                if (!update.content) {
                    return model::ModelError{
                        model::ModelErrorCode::InvalidShape, "/events/item/content", "content channel is absent from the exact item"};
                }
                retainItemContentAppendHint(update, *event, item.backendDroppedContentBytes, item.content);

                // Once an append-only backend channel was already longer than
                // the final projected UTF-8 prefix, another append cannot
                // change that prefix. Equality is deliberately not suppressed:
                // it is the first overflow transition which makes truncation
                // observable to the client. Any missing hint or backend-side
                // rolling retention remains on the conservative emit path.
                const std::size_t projectedContentBytes =
                    update.content->size() + (update.overflowV1.has_value() ? update.overflowV1->suffix.size() : 0);
                const bool unchangedFrozenContent =
                    event->channelBytesBefore.has_value() && event->droppedContentBytesBefore.has_value() &&
                    *event->droppedContentBytesBefore == 0 && item.backendDroppedContentBytes == 0 &&
                    *event->channelBytesBefore > projectedContentBytes;
                if (unchangedFrozenContent) {
                    continue;
                }

                OccurrenceSelection selection;
                selection.threadId = threadId;
                selection.turnId = turnId;
                selection.itemId = itemId;
                selection.channel = update.channel;
                model::OccurrencePayload payload{std::move(update)};
                OccurrenceCoalescingKey key = occurrenceKey(payload, selection, {});
                model::OccurrenceDraft occurrence{
                    model::SourceStamp{"backend-event:" + std::to_string(events[index].sequence.value())}, std::move(payload)};
                attachOccurrenceIdentities(occurrence, selection);
                model::OccurrenceError validation;
                if (!model::validateOccurrenceDraft(occurrence, &validation)) {
                    return model::ModelError{model::ModelErrorCode::InvalidShape, validation.path, validation.message};
                }
                projected.occurrences.push_back(
                    {std::move(key), std::move(occurrence), OccurrenceFlushUrgency::Deferred});
            }
            return projected;
        } catch (const ProjectionFailure& failure) {
            return model::ModelError{model::ModelErrorCode::InvalidIdentifier, failure.path(), failure.what()};
        } catch (const std::exception& error) {
            return model::ModelError{model::ModelErrorCode::InvalidShape, "/events", error.what()};
        } catch (...) {
            return model::ModelError{model::ModelErrorCode::InvalidShape, "/events", "content occurrence projection failed"};
        }
    }

    model::ModelResult<ProjectedBackendBatch> BackendProjection::projectOccurrences(std::span<const backend::SequencedBackendEvent> events,
                                                                                    const backend::Snapshot& snapshot) const noexcept {
        static_assert(std::variant_size_v<backend::BackendEvent> == 26);
        try {
            model::ModelResult<model::CanonicalSnapshot> projectedSnapshot = projectSnapshot(snapshot);
            if (!projectedSnapshot) {
                return projectedSnapshot.error();
            }

            ProjectedBackendBatch projected;
            projected.snapshot = std::move(projectedSnapshot).value();

            const auto sourceStamp = [](backend::SequenceNumber sequence) {
                return model::SourceStamp{"backend-event:" + std::to_string(sequence.value())};
            };
            const auto append = [&](backend::SequenceNumber sequence,
                                    model::OccurrencePayload payload,
                                    OccurrenceSelection selection,
                                    OccurrenceFlushUrgency urgency,
                                    std::optional<model::LegacyCompatibilityPayload> legacy = std::nullopt,
                                    std::string keyFallback = {}) {
                model::OccurrenceDraft occurrence =
                    legacy
                        ? model::OccurrenceDraft{sourceStamp(sequence), std::move(*legacy), std::vector<model::OccurrencePayload>{payload}}
                        : model::OccurrenceDraft{sourceStamp(sequence), payload};
                attachOccurrenceIdentities(occurrence, selection);
                model::OccurrenceError validation;
                if (!model::validateOccurrenceDraft(occurrence, &validation)) {
                    throw ProjectionFailure(validation.path, validation.message);
                }
                projected.occurrences.push_back(
                    ProjectedBackendOccurrence{occurrenceKey(payload, selection, std::move(keyFallback)), std::move(occurrence), urgency});
            };
            const auto appendFamily = [&](backend::SequenceNumber sequence,
                                          ExpandedEventType family,
                                          OccurrenceSelection selection,
                                          OccurrenceFlushUrgency urgency,
                                          std::optional<model::LegacyCompatibilityPayload> legacy = std::nullopt,
                                          std::string keyFallback = {}) {
                std::optional<model::OccurrencePayload> payload = payloadForFamily(family, projected.snapshot, selection);
                if (!payload) {
                    projected.snapshotRequired = true;
                    return;
                }
                append(sequence, std::move(*payload), std::move(selection), urgency, std::move(legacy), std::move(keyFallback));
            };
            const auto appendLegacyOnly = [&](backend::SequenceNumber sequence,
                                              model::LegacyCompatibilityPayload legacy,
                                              OccurrenceSelection selection,
                                              OccurrenceEntityKind kind,
                                              OccurrenceFlushUrgency urgency) {
                model::OccurrenceDraft occurrence{sourceStamp(sequence), std::move(legacy), {}};
                attachOccurrenceIdentities(occurrence, selection);
                model::OccurrenceError validation;
                if (!model::validateOccurrenceDraft(occurrence, &validation)) {
                    throw ProjectionFailure(validation.path, validation.message);
                }
                OccurrenceCoalescingKey key;
                key.kind = kind;
                key.threadId = selection.threadId;
                key.turnId = selection.turnId;
                key.itemId = selection.itemId;
                key.pendingRequestId = selection.pendingRequestId;
                projected.occurrences.push_back({std::move(key), std::move(occurrence), urgency});
            };

            std::optional<backend::SequenceNumber> previousSequence;
            for (const backend::SequencedBackendEvent& sequenced : events) {
                if (previousSequence && sequenced.sequence < *previousSequence) {
                    return model::ModelError{
                        model::ModelErrorCode::InvalidShape, "/events/sequence", "backend events are not in deterministic sequence order"};
                }
                previousSequence = sequenced.sequence;

                std::visit(
                    [&](const auto& event) {
                        using Event = std::decay_t<decltype(event)>;
                        if constexpr (std::is_same_v<Event, backend::ProviderLifecycleChanged> ||
                                      std::is_same_v<Event, backend::ProviderConnectionInvalidated>) {
                            appendFamily(sequenced.sequence,
                                         ExpandedEventType::ProviderUpdated,
                                         {},
                                         OccurrenceFlushUrgency::Immediate,
                                         std::nullopt,
                                         "provider");
                        } else if constexpr (std::is_same_v<Event, backend::CapacityConfigured>) {
                            appendFamily(sequenced.sequence,
                                         ExpandedEventType::CapacityUpdated,
                                         {},
                                         OccurrenceFlushUrgency::Immediate,
                                         std::nullopt,
                                         "capacity");
                        } else if constexpr (std::is_same_v<Event, backend::CapacityChanged>) {
                            if (event.canonicalStateRewritten) {
                                // Retention enforcement changed entities outside
                                // the nominal event. Rebase all clients from the
                                // authoritative post-reduction snapshot.
                                projected.snapshotRequired = true;
                            } else {
                                appendFamily(sequenced.sequence,
                                             ExpandedEventType::CapacityUpdated,
                                             {},
                                             OccurrenceFlushUrgency::Immediate,
                                             std::nullopt,
                                             "capacity");
                            }
                        } else if constexpr (std::is_same_v<Event, backend::DiagnosticReceived>) {
                            appendFamily(sequenced.sequence,
                                         ExpandedEventType::DiagnosticsUpdated,
                                         {},
                                         OccurrenceFlushUrgency::Immediate,
                                         std::nullopt,
                                         "diagnostics");
                        } else if constexpr (std::is_same_v<Event, backend::ProviderOperationCompleted>) {
                            // The reducer publishes ProviderOperationStateChanged
                            // after applying this potentially large typed input.
                            // Retaining both would duplicate one semantic change.
                        } else if constexpr (std::is_same_v<Event, backend::ProviderOperationStateChanged>) {
                            // thread/read has a dedicated ThreadUpserted marker
                            // for summary reads; full reads are requester-local
                            // and emit neither global marker.
                            if (event.method != "thread/read") {
                                projected.snapshotRequired = true;
                                if (const auto family = providerDomainFamily(event.method)) {
                                    appendFamily(
                                        sequenced.sequence, *family, {}, OccurrenceFlushUrgency::Deferred, std::nullopt, event.method);
                                }
                            }
                        } else if constexpr (std::is_same_v<Event, backend::ProviderResourceAdmissionRequested>) {
                            OccurrenceSelection selection;
                            ExpandedEventType family = ExpandedEventType::ProcessUpdated;
                            if (event.kind == backend::ProviderResourceKind::Process) {
                                selection.processHandle = model::ProcessHandle::parse(event.resourceId);
                            } else if (event.kind == backend::ProviderResourceKind::FilesystemWatch) {
                                family = ExpandedEventType::FilesystemWatchUpdated;
                                if (validEntityKey(event.resourceId)) {
                                    selection.filesystemWatchId = event.resourceId;
                                }
                            } else {
                                family = ExpandedEventType::FuzzySearchUpdated;
                                if (validEntityKey(event.resourceId)) {
                                    selection.fuzzySearchId = event.resourceId;
                                }
                            }
                            appendFamily(sequenced.sequence,
                                         family,
                                         std::move(selection),
                                         OccurrenceFlushUrgency::Deferred,
                                         std::nullopt,
                                         event.key);
                        } else if constexpr (std::is_same_v<Event, backend::ProviderResourceAdmissionReleased>) {
                            // A removed entity has no typed upsert payload. A
                            // canonical snapshot is the frozen containment path.
                            projected.snapshotRequired = true;
                        } else if constexpr (std::is_same_v<Event, backend::ThreadUpserted>) {
                            if (event.load == backend::EntityLoad::Full) {
                                // Full thread reads are delivered only to the
                                // requester through the negotiated state
                                // effect. Publishing even a bounded header
                                // here would leak requester-local hydration to
                                // every synchronized observer.
                                return;
                            }
                            OccurrenceSelection selection;
                            selection.threadId = model::ThreadIdentity::parse(event.thread.id.value);
                            appendFamily(sequenced.sequence,
                                         ExpandedEventType::ThreadUpserted,
                                         std::move(selection),
                                         OccurrenceFlushUrgency::Deferred);
                        } else if constexpr (std::is_same_v<Event, backend::ThreadStatusUpdated>) {
                            OccurrenceSelection selection;
                            selection.threadId = model::ThreadIdentity::parse(event.threadId.value);
                            appendFamily(sequenced.sequence,
                                         ExpandedEventType::ThreadUpserted,
                                         std::move(selection),
                                         OccurrenceFlushUrgency::Deferred);
                        } else if constexpr (std::is_same_v<Event, backend::ThreadListUpdated>) {
                            for (const typed::Thread& thread : event.page.data) {
                                OccurrenceSelection selection;
                                selection.threadId = model::ThreadIdentity::parse(thread.id.value);
                                appendFamily(sequenced.sequence,
                                             ExpandedEventType::ThreadUpserted,
                                             std::move(selection),
                                             OccurrenceFlushUrgency::Deferred);
                            }
                            appendFamily(sequenced.sequence,
                                         ExpandedEventType::ThreadListUpdated,
                                         {},
                                         OccurrenceFlushUrgency::Deferred,
                                         std::nullopt,
                                         "thread-list");
                        } else if constexpr (std::is_same_v<Event, backend::TurnUpserted> ||
                                             std::is_same_v<Event, backend::TurnCompleted> || std::is_same_v<Event, backend::TurnFailed>) {
                            OccurrenceSelection selection;
                            selection.threadId = model::ThreadIdentity::parse(event.turn.threadId.value);
                            selection.turnId = model::TurnIdentity::parse(event.turn.id.value);
                            constexpr bool Terminal =
                                std::is_same_v<Event, backend::TurnCompleted> || std::is_same_v<Event, backend::TurnFailed>;
                            appendFamily(sequenced.sequence,
                                         ExpandedEventType::TurnUpserted,
                                         std::move(selection),
                                         Terminal ? OccurrenceFlushUrgency::Immediate : OccurrenceFlushUrgency::Deferred);
                        } else if constexpr (std::is_same_v<Event, backend::TurnErrorUpdated>) {
                            OccurrenceSelection selection;
                            selection.threadId = model::ThreadIdentity::parse(event.threadId.value);
                            selection.turnId = model::TurnIdentity::parse(event.turnId.value);
                            appendFamily(sequenced.sequence,
                                         ExpandedEventType::TurnUpserted,
                                         std::move(selection),
                                         event.willRetry ? OccurrenceFlushUrgency::Deferred : OccurrenceFlushUrgency::Immediate);
                        } else if constexpr (std::is_same_v<Event, backend::ItemUpserted>) {
                            OccurrenceSelection selection;
                            selection.threadId = model::ThreadIdentity::parse(event.threadId.value);
                            selection.turnId = model::TurnIdentity::parse(event.turnId.value);
                            if (const std::optional<typed::ItemId> id = backend::itemId(event.item)) {
                                selection.itemId = model::ItemIdentity::parse(id->value);
                            }
                            const bool terminal =
                                event.lifecycle == backend::ItemLifecycle::Completed || event.lifecycle == backend::ItemLifecycle::Failed;
                            const auto legacyItem = selection.itemId.has_value()
                                                        ? std::find_if(projected.snapshot.legacyItems.begin(),
                                                                       projected.snapshot.legacyItems.end(),
                                                                       [&](const model::LegacyItemCompatibility& item) {
                                                                           return item.value.id == *selection.itemId;
                                                                       })
                                                        : projected.snapshot.legacyItems.end();
                            if (legacyItem != projected.snapshot.legacyItems.end()) {
                                model::LegacyCompatibilityPayload legacy;
                                legacy.kind = model::LegacyCompatibilityKind::LegacyItem;
                                legacy.legacyItem = *legacyItem;
                                appendLegacyOnly(sequenced.sequence,
                                                 std::move(legacy),
                                                 std::move(selection),
                                                 OccurrenceEntityKind::Item,
                                                 terminal ? OccurrenceFlushUrgency::Immediate : OccurrenceFlushUrgency::Deferred);
                            } else {
                                appendFamily(sequenced.sequence,
                                             ExpandedEventType::ItemUpserted,
                                             std::move(selection),
                                             terminal ? OccurrenceFlushUrgency::Immediate : OccurrenceFlushUrgency::Deferred);
                            }
                        } else if constexpr (std::is_same_v<Event, backend::ItemContentChanged>) {
                            OccurrenceSelection selection;
                            selection.threadId = model::ThreadIdentity::parse(event.threadId.value);
                            selection.turnId = model::TurnIdentity::parse(event.turnId.value);
                            selection.itemId = model::ItemIdentity::parse(event.itemId.value);
                            selection.channel = itemContentChannel(event.kind);
                            if (!selection.threadId || !selection.turnId || !selection.itemId) {
                                projected.snapshotRequired = true;
                                return;
                            }
                            std::optional<model::OccurrencePayload> payload =
                                payloadForFamily(ExpandedEventType::ItemContentUpdated, projected.snapshot, selection);
                            const backend::ItemSnapshot* backendItem =
                                findBackendItem(snapshot, event.threadId, event.turnId, event.itemId);
                            if (!payload || backendItem == nullptr) {
                                projected.snapshotRequired = true;
                            } else {
                                const std::string_view fullContent = [&]() -> std::string_view {
                                    switch (event.kind) {
                                        case backend::ItemContentChanged::Kind::AgentText:
                                            return backendItem->agentText;
                                        case backend::ItemContentChanged::Kind::ReasoningText:
                                            return backendItem->reasoningText;
                                        case backend::ItemContentChanged::Kind::ReasoningSummary:
                                            return backendItem->reasoningSummary;
                                        case backend::ItemContentChanged::Kind::CommandOutput:
                                            return backendItem->commandOutput;
                                    }
                                    return {};
                                }();
                                const std::uint64_t channelDroppedContentBytes = [&]() {
                                    switch (event.kind) {
                                        case backend::ItemContentChanged::Kind::AgentText:
                                            return backendItem->agentTextDroppedContentBytes;
                                        case backend::ItemContentChanged::Kind::ReasoningText:
                                            return backendItem->reasoningTextDroppedContentBytes;
                                        case backend::ItemContentChanged::Kind::ReasoningSummary:
                                            return backendItem->reasoningSummaryDroppedContentBytes;
                                        case backend::ItemContentChanged::Kind::CommandOutput:
                                            return backendItem->commandOutputDroppedContentBytes;
                                    }
                                    return std::uint64_t{0};
                                }();
                                retainItemContentAppendHint(std::get<model::ItemContentUpdatedOccurrence>(*payload),
                                                            event,
                                                            channelDroppedContentBytes,
                                                            fullContent);
                                append(sequenced.sequence,
                                       std::move(*payload),
                                       std::move(selection),
                                       OccurrenceFlushUrgency::Deferred);
                            }
                        } else if constexpr (std::is_same_v<Event, backend::FileChangeUpdated>) {
                            OccurrenceSelection selection;
                            selection.threadId = model::ThreadIdentity::parse(event.threadId.value);
                            selection.turnId = model::TurnIdentity::parse(event.turnId.value);
                            selection.itemId = model::ItemIdentity::parse(event.itemId.value);
                            appendFamily(sequenced.sequence,
                                         ExpandedEventType::ItemUpserted,
                                         std::move(selection),
                                         OccurrenceFlushUrgency::Deferred);
                        } else if constexpr (std::is_same_v<Event, backend::TokenUsageUpdated> ||
                                             std::is_same_v<Event, backend::ModelRerouted>) {
                            OccurrenceSelection selection;
                            selection.threadId = model::ThreadIdentity::parse(event.threadId.value);
                            selection.turnId = model::TurnIdentity::parse(event.turnId.value);
                            appendFamily(sequenced.sequence,
                                         ExpandedEventType::TurnUpserted,
                                         std::move(selection),
                                         OccurrenceFlushUrgency::Deferred);
                        } else if constexpr (std::is_same_v<Event, backend::PendingRequestAdded>) {
                            OccurrenceSelection selection;
                            selection.pendingRequestId = model::PendingRequestIdentity::parse(std::to_string(event.pending.id.value()));
                            const auto legacyPending = selection.pendingRequestId.has_value()
                                                           ? std::find_if(projected.snapshot.legacyPendingRequests.begin(),
                                                                          projected.snapshot.legacyPendingRequests.end(),
                                                                          [&](const model::LegacyPendingRequestCompatibility& request) {
                                                                              return request.value.id == *selection.pendingRequestId;
                                                                          })
                                                           : projected.snapshot.legacyPendingRequests.end();
                            if (legacyPending != projected.snapshot.legacyPendingRequests.end()) {
                                model::LegacyCompatibilityPayload legacy;
                                legacy.kind = model::LegacyCompatibilityKind::LegacyPendingRequest;
                                legacy.legacyPendingRequest = *legacyPending;
                                appendLegacyOnly(sequenced.sequence,
                                                 std::move(legacy),
                                                 std::move(selection),
                                                 OccurrenceEntityKind::PendingRequest,
                                                 OccurrenceFlushUrgency::Immediate);
                            } else {
                                appendFamily(sequenced.sequence,
                                             ExpandedEventType::PendingRequestsUpdated,
                                             std::move(selection),
                                             OccurrenceFlushUrgency::Immediate);
                            }
                        } else if constexpr (std::is_same_v<Event, backend::PendingRequestRemoved>) {
                            OccurrenceSelection selection;
                            selection.pendingRequestId = model::PendingRequestIdentity::parse(std::to_string(event.id.value()));
                            model::LegacyCompatibilityPayload legacy;
                            legacy.kind = model::LegacyCompatibilityKind::PendingRequestResolved;
                            legacy.resolvedRequestId = selection.pendingRequestId;
                            legacy.resolutionReason = event.reason;
                            std::optional<model::OccurrencePayload> payload =
                                payloadForFamily(ExpandedEventType::PendingRequestsUpdated, projected.snapshot, selection);
                            if (!payload) {
                                projected.snapshotRequired = true;
                            } else {
                                model::PendingRequestsUpdatedOccurrence& update =
                                    std::get<model::PendingRequestsUpdatedOccurrence>(*payload);
                                update.removedRequestId = selection.pendingRequestId;
                                update.resolutionReason = event.reason;
                                append(sequenced.sequence,
                                       std::move(*payload),
                                       std::move(selection),
                                       OccurrenceFlushUrgency::Immediate,
                                       std::move(legacy));
                            }
                        } else if constexpr (std::is_same_v<Event, backend::ControllerChanged>) {
                            OccurrenceSelection selection;
                            if (event.controller) {
                                selection.sessionId = model::SessionIdentity::parse(std::to_string(event.controller->value()));
                            }
                            appendFamily(sequenced.sequence,
                                         ExpandedEventType::ControllerUpdated,
                                         std::move(selection),
                                         OccurrenceFlushUrgency::Immediate);
                        } else if constexpr (std::is_same_v<Event, backend::SessionChanged>) {
                            OccurrenceSelection selection;
                            selection.sessionId = model::SessionIdentity::parse(std::to_string(event.id.value()));
                            model::SessionsUpdatedOccurrence update{projected.snapshot.sessions};
                            const auto changed =
                                std::find_if(update.sessions.begin(), update.sessions.end(), [&](const model::SessionState& value) {
                                    return selection.sessionId && value.id == *selection.sessionId;
                                });
                            if (changed != update.sessions.end()) {
                                update.changedSession = *changed;
                            } else if (selection.sessionId) {
                                model::SessionState removed{*selection.sessionId};
                                removed.role = sessionRole(event.role);
                                update.changedSession = std::move(removed);
                            }
                            update.connected = event.connected;
                            model::LegacyCompatibilityPayload legacy;
                            legacy.kind = model::LegacyCompatibilityKind::SessionChanged;
                            legacy.changedSessionId = selection.sessionId;
                            legacy.connected = event.connected;
                            append(sequenced.sequence,
                                   std::move(update),
                                   std::move(selection),
                                   OccurrenceFlushUrgency::Immediate,
                                   std::move(legacy));
                        } else if constexpr (std::is_same_v<Event, backend::CodexExtensionReceived>) {
                            const backend::CodexExtensionReceived safeExtension = projectionSafeExtension(event);
                            const OccurrenceSelection selection = selectionFromExtension(safeExtension, projected.snapshot);
                            const generated::ProjectionMetadata* metadata = notificationProjection(safeExtension.method);
                            const model::SourceStamp occurrenceSource =
                                metadata != nullptr ? model::SourceStamp{std::string(metadata->registryKey)}
                                                    : model::SourceStamp{"server_notification:unknown:" + safeExtension.method};
                            model::LegacyCompatibilityPayload legacy = extensionCompatibility(safeExtension);
                            std::vector<model::OccurrencePayload> expanded;
                            bool mappingFailed = false;
                            bool legacyOnly = false;
                            if (metadata != nullptr) {
                                expanded.reserve(metadata->expandedMappings.size());
                                for (const std::string_view mapping : metadata->expandedMappings) {
                                    const std::optional<ExpandedEventType> family = expandedEventTypeFromString(mapping);
                                    std::optional<model::OccurrencePayload> payload =
                                        family ? payloadForFamily(*family, projected.snapshot, selection) : std::nullopt;
                                    if (!payload) {
                                        if (family == ExpandedEventType::ItemUpserted && selection.itemId.has_value()) {
                                            const auto item = std::find_if(projected.snapshot.legacyItems.begin(),
                                                                           projected.snapshot.legacyItems.end(),
                                                                           [&](const model::LegacyItemCompatibility& value) {
                                                                               return value.value.id == *selection.itemId;
                                                                           });
                                            if (item != projected.snapshot.legacyItems.end()) {
                                                legacy.kind = model::LegacyCompatibilityKind::LegacyItem;
                                                legacy.legacyItem = *item;
                                                legacyOnly = true;
                                                expanded.clear();
                                                break;
                                            }
                                        }
                                        if (family == ExpandedEventType::PendingRequestsUpdated &&
                                            selection.pendingRequestId.has_value()) {
                                            const auto request =
                                                std::find_if(projected.snapshot.legacyPendingRequests.begin(),
                                                             projected.snapshot.legacyPendingRequests.end(),
                                                             [&](const model::LegacyPendingRequestCompatibility& value) {
                                                                 return value.value.id == *selection.pendingRequestId;
                                                             });
                                            if (request != projected.snapshot.legacyPendingRequests.end()) {
                                                legacy.kind = model::LegacyCompatibilityKind::LegacyPendingRequest;
                                                legacy.legacyPendingRequest = *request;
                                                legacyOnly = true;
                                                expanded.clear();
                                                break;
                                            }
                                        }
                                        projected.snapshotRequired = true;
                                        mappingFailed = true;
                                        expanded.clear();
                                        break;
                                    }
                                    if (*family == ExpandedEventType::NoticeAdded) {
                                        std::get<model::NoticeAddedOccurrence>(*payload).notice.occurrence = 0;
                                    }
                                    expanded.push_back(std::move(*payload));
                                }
                                if (!legacyOnly && !expanded.empty() && metadata->legacyContract == "legacy_normalized") {
                                    model::OccurrenceDraft normalized{occurrenceSource, expanded.front()};
                                    legacy = normalized.legacyCompatibility;
                                }
                            }
                            if (!mappingFailed) {
                                model::OccurrenceDraft occurrence{occurrenceSource, std::move(legacy), std::move(expanded)};
                                attachOccurrenceIdentities(occurrence, selection);
                                model::OccurrenceError validation;
                                if (!model::validateOccurrenceDraft(occurrence, &validation)) {
                                    throw ProjectionFailure(validation.path, validation.message);
                                }
                                OccurrenceCoalescingKey key;
                                key.kind = OccurrenceEntityKind::CodexExtension;
                                key.threadId = selection.threadId;
                                key.turnId = selection.turnId;
                                key.itemId = selection.itemId;
                                key.pendingRequestId = selection.pendingRequestId;
                                key.entityId = event.method.substr(0, model::SourceStamp::MaximumBytes);
                                const bool urgent = event.method == "error" || event.method == "warning" ||
                                                    event.method == "configWarning" || event.method == "guardianWarning";
                                projected.occurrences.push_back(ProjectedBackendOccurrence{std::move(key),
                                                                                           std::move(occurrence),
                                                                                           urgent ? OccurrenceFlushUrgency::Immediate
                                                                                                  : OccurrenceFlushUrgency::Deferred});
                            }
                        } else {
                            static_assert(AlwaysFalse<Event>, "BackendProjection must exhaustively handle BackendEvent");
                        }
                    },
                    sequenced.event);
            }
            return projected;
        } catch (const ProjectionFailure& failure) {
            return model::ModelError{model::ModelErrorCode::InvalidShape, failure.path(), failure.what()};
        } catch (const std::exception& error) {
            return model::ModelError{model::ModelErrorCode::InvalidShape, "/events", error.what()};
        } catch (...) {
            return model::ModelError{model::ModelErrorCode::InvalidShape, "/events", "backend occurrence projection failed"};
        }
    }

    model::ModelResult<ProjectedBackendBatch>
    BackendProjection::projectActivityForTesting(const backend::Snapshot& snapshot,
                                                 std::optional<std::string_view> key) const noexcept {
        try {
            model::ModelResult<model::CanonicalSnapshot> projectedSnapshot = projectSnapshot(snapshot);
            if (!projectedSnapshot) {
                return projectedSnapshot.error();
            }

            ProjectedBackendBatch projected;
            projected.snapshot = std::move(projectedSnapshot).value();
            OccurrenceSelection selection;
            if (key && validEntityKey(*key)) {
                selection.activityKey = std::string(*key);
            }
            std::optional<model::OccurrencePayload> payload =
                payloadForFamily(ExpandedEventType::ActivityUpdated, projected.snapshot, selection);
            if (!payload) {
                projected.snapshotRequired = true;
                return projected;
            }

            model::OccurrenceDraft occurrence{
                model::SourceStamp{"backend-test:activity.updated"}, *payload};
            model::OccurrenceError validation;
            if (!model::validateOccurrenceDraft(occurrence, &validation)) {
                throw ProjectionFailure(validation.path, validation.message);
            }
            projected.occurrences.push_back(ProjectedBackendOccurrence{
                occurrenceKey(*payload, selection, *selection.activityKey), std::move(occurrence), OccurrenceFlushUrgency::Deferred});
            return projected;
        } catch (const ProjectionFailure& failure) {
            return model::ModelError{model::ModelErrorCode::InvalidShape, failure.path(), failure.what()};
        } catch (const std::exception& error) {
            return model::ModelError{model::ModelErrorCode::InvalidShape, "/events/activity", error.what()};
        } catch (...) {
            return model::ModelError{model::ModelErrorCode::InvalidShape,
                                     "/events/activity",
                                     "backend activity occurrence projection failed"};
        }
    }

} // namespace ai::openai::codex::frontend::internal::server
