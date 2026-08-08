/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/detail/FrontendProjection.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace ai::openai::codex::frontend::detail {

    namespace {

        constexpr std::size_t DefaultMaximumReportedPaths = 64;
        constexpr std::string_view ThreadItemRegistryKeyPrefix = "item_discriminator:ThreadItem:type:";
        constexpr std::array<ThreadItemKind, 18> StableThreadItemKinds{
            ThreadItemKind::AgentMessage,
            ThreadItemKind::CollabAgentToolCall,
            ThreadItemKind::CommandExecution,
            ThreadItemKind::ContextCompaction,
            ThreadItemKind::DynamicToolCall,
            ThreadItemKind::EnteredReviewMode,
            ThreadItemKind::ExitedReviewMode,
            ThreadItemKind::FileChange,
            ThreadItemKind::HookPrompt,
            ThreadItemKind::ImageGeneration,
            ThreadItemKind::ImageView,
            ThreadItemKind::McpToolCall,
            ThreadItemKind::Plan,
            ThreadItemKind::Reasoning,
            ThreadItemKind::Sleep,
            ThreadItemKind::SubAgentActivity,
            ThreadItemKind::UserMessage,
            ThreadItemKind::WebSearch,
        };

        template <typename Value>
        void saturatingIncrement(Value& value) noexcept {
            if (value != std::numeric_limits<Value>::max()) {
                ++value;
            }
        }

        template <typename Value>
        void saturatingAdd(Value& value, Value increment) noexcept {
            if (increment > std::numeric_limits<Value>::max() - value) {
                value = std::numeric_limits<Value>::max();
            } else {
                value += increment;
            }
        }

        void mergeStatistics(CanonicalSanitizationStatistics& target, const CanonicalSanitizationStatistics& source) noexcept {
            saturatingAdd(target.visits, source.visits);
            target.maximumDepthObserved = std::max(target.maximumDepthObserved, source.maximumDepthObserved);
            saturatingAdd(target.knownStructuredSecretFieldsRemoved, source.knownStructuredSecretFieldsRemoved);
            saturatingAdd(target.unsafeRawFieldsRemoved, source.unsafeRawFieldsRemoved);
            saturatingAdd(target.valuesOmittedByBounds, source.valuesOmittedByBounds);
            saturatingAdd(target.stringsTruncated, source.stringsTruncated);
            target.truncated = target.truncated || source.truncated;
            target.failed = target.failed || source.failed;
        }

        std::string normalizedName(std::string_view value) {
            std::string normalized;
            normalized.reserve(value.size());
            for (const unsigned char character : value) {
                if (std::isalnum(character) != 0) {
                    normalized.push_back(static_cast<char>(std::tolower(character)));
                }
            }
            return normalized;
        }

        bool isKnownStructuredSecretFieldName(std::string_view value) {
            const std::string normalized = normalizedName(value);
            // A token count or budget is useful canonical state; a value whose
            // field is itself a token is secret.  Suffix matching deliberately
            // preserves tokenUsage, tokenBudget, tokensUsed, and similar
            // accounting fields while removing accessToken, refreshToken,
            // occurrenceToken, and future credential-token spellings.
            return normalized.ends_with("token") || normalized.ends_with("secret") || normalized.ends_with("credential") ||
                   normalized.ends_with("credentials") || normalized.find("password") != std::string::npos ||
                   normalized.find("passphrase") != std::string::npos || normalized.find("authorization") != std::string::npos ||
                   normalized.find("privatekey") != std::string::npos || normalized.find("apikey") != std::string::npos ||
                   normalized == "authentication" || normalized == "cookie" || normalized == "setcookie" || normalized == "answer" ||
                   normalized == "answers";
        }

        bool isUnsafeRawFieldName(std::string_view value) {
            const std::string normalized = normalizedName(value);
            return normalized == "raw" || normalized.starts_with("raw") || normalized.ends_with("raw") ||
                   normalized.find("rawprovider") != std::string::npos || normalized == "providerenvelope" ||
                   normalized == "occurrencetoken" || normalized == "requesttoken";
        }

        bool isSafeSecretClassification(std::string_view key, const Json& value) {
            return normalizedName(key) == "issecret" && value.is_boolean();
        }

        std::string escapedPathComponent(std::string_view component) {
            std::string escaped;
            escaped.reserve(component.size());
            for (const char character : component) {
                if (character == '~') {
                    escaped += "~0";
                } else if (character == '/') {
                    escaped += "~1";
                } else {
                    escaped.push_back(character);
                }
            }
            return escaped;
        }

        std::string childPath(std::string_view parent, std::string_view component) {
            std::string path(parent);
            path.push_back('/');
            path += escapedPathComponent(component);
            return path;
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

        struct Sanitizer {
            const FrontendProjectionLimits& limits;
            CanonicalSanitizationStatistics statistics;
            bool terminal = false;

            Json sanitize(const Json& value, std::size_t depth) {
                if (terminal || statistics.visits >= limits.maximumVisits || depth > limits.maximumDepth) {
                    statistics.truncated = true;
                    saturatingIncrement(statistics.valuesOmittedByBounds);
                    terminal = statistics.visits >= limits.maximumVisits;
                    return nullptr;
                }

                saturatingIncrement(statistics.visits);
                statistics.maximumDepthObserved = std::max(statistics.maximumDepthObserved, depth);

                if (value.is_object()) {
                    Json result = Json::object();
                    std::size_t inspected = 0;
                    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
                        if (terminal) {
                            statistics.truncated = true;
                            saturatingAdd(statistics.valuesOmittedByBounds, value.size() - inspected);
                            break;
                        }
                        if (inspected >= limits.maximumObjectMembers) {
                            statistics.truncated = true;
                            saturatingAdd(statistics.valuesOmittedByBounds, value.size() - inspected);
                            break;
                        }
                        ++inspected;
                        if (isKnownStructuredSecretFieldName(iterator.key()) &&
                            !isSafeSecretClassification(iterator.key(), iterator.value())) {
                            saturatingIncrement(statistics.knownStructuredSecretFieldsRemoved);
                            continue;
                        }
                        if (isUnsafeRawFieldName(iterator.key())) {
                            saturatingIncrement(statistics.unsafeRawFieldsRemoved);
                            continue;
                        }
                        if (iterator.key().size() > limits.maximumPropertyNameBytes) {
                            statistics.truncated = true;
                            saturatingIncrement(statistics.stringsTruncated);
                            saturatingIncrement(statistics.valuesOmittedByBounds);
                            continue;
                        }
                        result[iterator.key()] = sanitize(iterator.value(), depth + 1);
                    }
                    return result;
                }

                if (value.is_array()) {
                    Json result = Json::array();
                    const std::size_t retained = std::min(value.size(), limits.maximumArrayItems);
                    if (retained != value.size()) {
                        statistics.truncated = true;
                        saturatingAdd(statistics.valuesOmittedByBounds, value.size() - retained);
                    }
                    for (std::size_t index = 0; index < retained; ++index) {
                        if (terminal) {
                            statistics.truncated = true;
                            saturatingAdd(statistics.valuesOmittedByBounds, retained - index);
                            break;
                        }
                        result.push_back(sanitize(value[index], depth + 1));
                    }
                    return result;
                }

                if (value.is_string()) {
                    const std::string& string = value.get_ref<const std::string&>();
                    if (string.size() > limits.maximumStringBytes) {
                        statistics.truncated = true;
                        saturatingIncrement(statistics.stringsTruncated);
                        return string.substr(0, utf8PrefixLength(string, limits.maximumStringBytes));
                    }
                }
                return value;
            }
        };

        ScopedProjectionValue sanitizeScopedValue(ScopedProjectionValue value,
                                                  const FrontendProjectionLimits& limits,
                                                  std::size_t& remainingVisits,
                                                  CanonicalSanitizationStatistics& aggregate) {
            FrontendProjectionLimits remainingLimits = limits;
            remainingLimits.maximumVisits = remainingVisits;
            Sanitizer sanitizer{remainingLimits, {}, false};
            value.value = sanitizer.sanitize(value.value, 0);
            remainingVisits -= std::min(remainingVisits, sanitizer.statistics.visits);
            mergeStatistics(aggregate, sanitizer.statistics);

            if (value.rules.size() > limits.maximumProjectionRules) {
                saturatingAdd(aggregate.valuesOmittedByBounds, value.rules.size() - limits.maximumProjectionRules);
                aggregate.truncated = true;
                value.rules.resize(limits.maximumProjectionRules);
            }
            for (ScopeProjectionRule& rule : value.rules) {
                if (rule.path.size() > limits.maximumStringBytes) {
                    rule.path.resize(utf8PrefixLength(rule.path, limits.maximumStringBytes));
                    saturatingIncrement(aggregate.stringsTruncated);
                    aggregate.truncated = true;
                }
                if (rule.requiredScopes.size() > LocalTrustedScopes.size()) {
                    rule.requiredScopes.resize(LocalTrustedScopes.size());
                    aggregate.truncated = true;
                    saturatingIncrement(aggregate.valuesOmittedByBounds);
                }
                std::sort(rule.requiredScopes.begin(), rule.requiredScopes.end());
                rule.requiredScopes.erase(std::unique(rule.requiredScopes.begin(), rule.requiredScopes.end()), rule.requiredScopes.end());
            }
            return value;
        }

        std::vector<std::string_view> pathComponents(std::string_view path) {
            std::vector<std::string_view> components;
            std::size_t start = !path.empty() && path.front() == '/' ? 1 : 0;
            while (start <= path.size()) {
                const std::size_t end = path.find('/', start);
                components.emplace_back(path.substr(start, end == std::string_view::npos ? path.size() - start : end - start));
                if (end == std::string_view::npos) {
                    break;
                }
                start = end + 1;
            }
            if (components.size() == 1 && components.front().empty()) {
                components.clear();
            }
            return components;
        }

        bool pathMatches(std::string_view pattern, std::string_view path) {
            const std::vector<std::string_view> patternParts = pathComponents(pattern);
            const std::vector<std::string_view> pathParts = pathComponents(path);
            std::vector<bool> current(pathParts.size() + 1, false);
            current.front() = true;
            for (const std::string_view part : patternParts) {
                std::vector<bool> next(pathParts.size() + 1, false);
                if (part == "**") {
                    for (std::size_t index = 0; index <= pathParts.size(); ++index) {
                        next[index] = current[index] || (index != 0 && next[index - 1]);
                    }
                } else {
                    for (std::size_t index = 1; index <= pathParts.size(); ++index) {
                        next[index] = current[index - 1] && (part == "*" || part == pathParts[index - 1]);
                    }
                }
                current = std::move(next);
            }
            return current.back();
        }

        struct ProjectionState {
            const FrontendProjectionContext& context;
            std::vector<std::string> omittedFields;
            std::vector<std::string> redactedFields;
            std::size_t maximumReportedPaths = DefaultMaximumReportedPaths;

            void report(std::vector<std::string>& paths, const std::string& path) {
                if (paths.size() < maximumReportedPaths) {
                    paths.push_back(path);
                }
            }
        };

        bool missingScope(const FrontendProjectionContext& context, std::span<const FrontendScope> required) noexcept {
            return !context.hasAllScopes(required);
        }

        struct RuleDecision {
            bool restricted = false;
            ScopeProjectionAction action = ScopeProjectionAction::Redact;
        };

        RuleDecision
        ruleDecision(const std::vector<ScopeProjectionRule>& rules, std::string_view path, const FrontendProjectionContext& context) {
            RuleDecision decision;
            for (const ScopeProjectionRule& rule : rules) {
                if (!pathMatches(rule.path, path) || !missingScope(context, rule.requiredScopes)) {
                    continue;
                }
                decision.restricted = true;
                if (rule.action == ScopeProjectionAction::Omit) {
                    decision.action = ScopeProjectionAction::Omit;
                }
            }
            return decision;
        }

        Json
        projectValue(const Json& value, const std::vector<ScopeProjectionRule>& rules, ProjectionState& state, std::string_view path = {}) {
            if (value.is_object()) {
                Json result = Json::object();
                for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
                    const std::string memberPath = childPath(path, iterator.key());
                    const RuleDecision decision = ruleDecision(rules, memberPath, state.context);
                    if (!decision.restricted) {
                        result[iterator.key()] = projectValue(iterator.value(), rules, state, memberPath);
                    } else if (decision.action == ScopeProjectionAction::Redact) {
                        result[iterator.key()] = "[redacted]";
                        state.report(state.redactedFields, memberPath);
                    } else {
                        state.report(state.omittedFields, memberPath);
                    }
                }
                return result;
            }

            if (value.is_array()) {
                Json result = Json::array();
                for (std::size_t index = 0; index < value.size(); ++index) {
                    const std::string elementPath = childPath(path, std::to_string(index));
                    const RuleDecision decision = ruleDecision(rules, elementPath, state.context);
                    if (!decision.restricted) {
                        result.push_back(projectValue(value[index], rules, state, elementPath));
                    } else if (decision.action == ScopeProjectionAction::Redact) {
                        result.push_back("[redacted]");
                        state.report(state.redactedFields, elementPath);
                    } else {
                        state.report(state.omittedFields, elementPath);
                    }
                }
                return result;
            }
            return value;
        }

        enum class ItemCommandOutputCeiling { CommandExecution, FilesystemWrite, Conservative };

        ItemCommandOutputCeiling itemCommandOutputCeiling(const Json& item) {
            if (!item.is_object()) {
                return ItemCommandOutputCeiling::Conservative;
            }
            const auto type = item.find("type");
            if (type == item.end() || !type->is_string()) {
                return ItemCommandOutputCeiling::Conservative;
            }
            const std::string normalized = normalizedName(type->get_ref<const std::string&>());
            const ItemCommandOutputCeiling ceiling = normalized == "commandexecution" ? ItemCommandOutputCeiling::CommandExecution
                                                     : normalized == "filechange"     ? ItemCommandOutputCeiling::FilesystemWrite
                                                                                      : ItemCommandOutputCeiling::Conservative;
            if (ceiling == ItemCommandOutputCeiling::Conservative) {
                return ceiling;
            }

            // A compatibility discriminator, when present, must agree with
            // the stable type. Treat malformed or conflicting data as an
            // unknown ceiling instead of trusting whichever field is most
            // permissive for the current principal.
            const auto agrees = [&normalized](const Json& owner, std::string_view member) {
                const auto candidate = owner.find(std::string(member));
                return candidate == owner.end() ||
                       (candidate->is_string() && normalizedName(candidate->get_ref<const std::string&>()) == normalized);
            };
            if (!agrees(item, "codexType")) {
                return ItemCommandOutputCeiling::Conservative;
            }
            const auto data = item.find("data");
            if (data != item.end() && (!data->is_object() || !agrees(*data, "codexType"))) {
                return ItemCommandOutputCeiling::Conservative;
            }
            return ceiling;
        }

        bool itemCommandOutputVisible(const Json& item, const FrontendProjectionContext& context) {
            switch (itemCommandOutputCeiling(item)) {
                case ItemCommandOutputCeiling::CommandExecution:
                    return context.hasScope(FrontendScope::CommandExecution);
                case ItemCommandOutputCeiling::FilesystemWrite:
                    return context.hasScope(FrontendScope::FilesystemWrite);
                case ItemCommandOutputCeiling::Conservative:
                    break;
            }

            // commandOutput is shared by two item contracts with different
            // information ceilings. An absent or unknown discriminator cannot
            // select either ceiling, so retain the value only for a principal
            // satisfying both. This semantic pass is deliberately O(items):
            // per-item rules would consume the bounded generic-rule budget and
            // could expose tail entries after that budget was exhausted.
            return context.hasScope(FrontendScope::CommandExecution) && context.hasScope(FrontendScope::FilesystemWrite);
        }

        void projectItemCommandOutput(Json& item, std::string_view path, ProjectionState& state) {
            if (!item.is_object()) {
                return;
            }
            const auto output = item.find("commandOutput");
            if (output == item.end() || itemCommandOutputVisible(item, state.context)) {
                return;
            }
            item.erase(output);
            state.report(state.omittedFields, childPath(path, "commandOutput"));
        }

        void projectItemArrayCommandOutput(Json& items, std::string_view path, ProjectionState& state) {
            if (!items.is_array()) {
                return;
            }
            for (std::size_t index = 0; index < items.size(); ++index) {
                projectItemCommandOutput(items[index], childPath(path, std::to_string(index)), state);
            }
        }

        void projectTurnArrayItemCommandOutput(Json& turns, std::string_view path, ProjectionState& state) {
            if (!turns.is_array()) {
                return;
            }
            for (std::size_t index = 0; index < turns.size(); ++index) {
                Json& turn = turns[index];
                if (!turn.is_object()) {
                    continue;
                }
                const auto items = turn.find("items");
                if (items != turn.end()) {
                    projectItemArrayCommandOutput(*items, childPath(childPath(path, std::to_string(index)), "items"), state);
                }
            }
        }

        void projectLegacySnapshotItemCommandOutput(Json& value, ProjectionState& state) {
            if (!value.is_object()) {
                return;
            }
            const auto threads = value.find("threads");
            if (threads == value.end() || !threads->is_array()) {
                return;
            }
            for (std::size_t index = 0; index < threads->size(); ++index) {
                Json& thread = (*threads)[index];
                if (!thread.is_object()) {
                    continue;
                }
                const auto turns = thread.find("turns");
                if (turns != thread.end()) {
                    projectTurnArrayItemCommandOutput(*turns, childPath(childPath("/threads", std::to_string(index)), "turns"), state);
                }
            }
        }

        void projectExpandedSnapshotItemCommandOutput(Json& value, ProjectionState& state) {
            if (!value.is_object()) {
                return;
            }
            const auto items = value.find("items");
            if (items != value.end()) {
                projectItemArrayCommandOutput(*items, "/items", state);
            }
        }

        void projectLegacyEventItemCommandOutput(std::string_view type, Json& data, ProjectionState& state) {
            if (!data.is_object()) {
                return;
            }
            if (type == "thread.updated") {
                const auto thread = data.find("thread");
                if (thread != data.end() && thread->is_object()) {
                    const auto turns = thread->find("turns");
                    if (turns != thread->end()) {
                        projectTurnArrayItemCommandOutput(*turns, "/thread/turns", state);
                    }
                }
            } else if (type == "turn.updated") {
                const auto turn = data.find("turn");
                if (turn != data.end() && turn->is_object()) {
                    const auto items = turn->find("items");
                    if (items != turn->end()) {
                        projectItemArrayCommandOutput(*items, "/turn/items", state);
                    }
                }
            } else if (type == "item.updated") {
                const auto item = data.find("item");
                if (item != data.end()) {
                    projectItemCommandOutput(*item, "/item", state);
                }
            }
        }

        void projectExpandedEventItemCommandOutput(ExpandedEventType type, Json& data, ProjectionState& state) {
            if (type != ExpandedEventType::ItemUpserted || !data.is_object()) {
                return;
            }
            const auto item = data.find("item");
            if (item != data.end()) {
                projectItemCommandOutput(*item, "/item", state);
            }
        }

        const ExpandedEventProjectionMetadata* eventMetadata(ExpandedEventType type) noexcept {
            const auto iterator = std::find_if(AllExpandedEventProjections.begin(),
                                               AllExpandedEventProjections.end(),
                                               [type](const ExpandedEventProjectionMetadata& metadata) {
                                                   return metadata.type == type;
                                               });
            return iterator == AllExpandedEventProjections.end() ? nullptr : &*iterator;
        }

        Json metadataCompatibleItems(const Json& items, ProjectionState& state) {
            if (!items.is_array()) {
                return Json::array();
            }
            Json result = Json::array();
            for (std::size_t index = 0; index < items.size(); ++index) {
                const Json& item = items[index];
                const std::string itemPath = childPath("/items", std::to_string(index));
                if (!item.is_object()) {
                    state.report(state.omittedFields, itemPath);
                    continue;
                }
                const auto type = item.find("type");
                if (type == item.end() || !type->is_string()) {
                    state.report(state.omittedFields, itemPath);
                    continue;
                }
                const auto id = item.find("id");
                if (id == item.end() || !id->is_string() || id->get_ref<const std::string&>().empty()) {
                    state.report(state.omittedFields, itemPath);
                    continue;
                }
                const std::string& typeName = type->get_ref<const std::string&>();
                const std::string registryKey = "item_discriminator:ThreadItem:type:" + typeName;
                const generated::ProjectionMetadata* metadata = threadItemProjection(registryKey);
                if (metadata == nullptr || metadata->legacyContract != "legacy_metadata_only") {
                    result.push_back(item);
                    continue;
                }
                Json compatible{{"id", *id}, {"type", typeName}, {"codexType", typeName}};
                if (const auto error = item.find("decodingError"); error != item.end() && error->is_string()) {
                    compatible["decodingError"] = *error;
                }
                result.push_back(std::move(compatible));
            }
            return result;
        }

        FrontendCapability effectiveExpansionCapability(const CanonicalEventRecord& record) noexcept {
            if (record.legacyType == "item.updated" || record.legacyType == "item.content.updated") {
                return FrontendCapability::CompleteThreadItems;
            }
            if (record.legacyType == "request.pending" || record.legacyType == "request.resolved") {
                return FrontendCapability::DedicatedPendingRequests;
            }
            return record.expansionCapability;
        }

        void normalizePaths(std::vector<std::string>& paths) {
            std::sort(paths.begin(), paths.end());
            paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        }

        void addProjectionMetadata(Json& extensions, std::vector<std::string> omittedFields, std::vector<std::string> redactedFields) {
            normalizePaths(omittedFields);
            normalizePaths(redactedFields);
            extensions["scopeProjection"] =
                Json{{"omittedFields", std::move(omittedFields)}, {"redactedFields", std::move(redactedFields)}};
        }

        bool containsNoKnownStructuredSecrets(const Json& value, std::size_t depth, std::size_t& remainingVisits) {
            if (depth > 128 || remainingVisits == 0) {
                return false;
            }
            --remainingVisits;
            if (value.is_object()) {
                for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
                    if ((isKnownStructuredSecretFieldName(iterator.key()) &&
                         !isSafeSecretClassification(iterator.key(), iterator.value())) ||
                        isUnsafeRawFieldName(iterator.key()) ||
                        !containsNoKnownStructuredSecrets(iterator.value(), depth + 1, remainingVisits)) {
                        return false;
                    }
                }
            } else if (value.is_array()) {
                for (const Json& item : value) {
                    if (!containsNoKnownStructuredSecrets(item, depth + 1, remainingVisits)) {
                        return false;
                    }
                }
            }
            return true;
        }

        bool uniqueNotificationAndItemKeys() noexcept {
            for (std::size_t left = 0; left < generated::AllNotificationProjections.size(); ++left) {
                const auto& metadata = generated::AllNotificationProjections[left];
                if (metadata.family != generated::ProjectionFamily::ServerNotification || metadata.expandedMappings.empty() ||
                    metadata.requiredScopes.size() != 1 || metadata.requiredScopes.front() != FrontendScope::Observe) {
                    return false;
                }
                for (const std::string_view mapping : metadata.expandedMappings) {
                    if (!expandedEventTypeFromString(mapping).has_value()) {
                        return false;
                    }
                }
                for (std::size_t right = left + 1; right < generated::AllNotificationProjections.size(); ++right) {
                    if (metadata.registryKey == generated::AllNotificationProjections[right].registryKey) {
                        return false;
                    }
                }
            }
            for (std::size_t left = 0; left < generated::AllThreadItemProjections.size(); ++left) {
                const auto& metadata = generated::AllThreadItemProjections[left];
                if (metadata.family != generated::ProjectionFamily::ThreadItem || metadata.expandedMappings.size() != 1 ||
                    metadata.expandedMappings.front() != "item.upserted" || metadata.requiredScopes.size() != 1 ||
                    metadata.requiredScopes.front() != FrontendScope::Observe) {
                    return false;
                }
                for (std::size_t right = left + 1; right < generated::AllThreadItemProjections.size(); ++right) {
                    if (metadata.registryKey == generated::AllThreadItemProjections[right].registryKey) {
                        return false;
                    }
                }
            }
            return true;
        }

        std::optional<std::string_view> generatedThreadItemDiscriminator(std::string_view registryKey) noexcept {
            if (!registryKey.starts_with(ThreadItemRegistryKeyPrefix)) {
                return std::nullopt;
            }
            const std::string_view discriminator = registryKey.substr(ThreadItemRegistryKeyPrefix.size());
            return discriminator.empty() ? std::nullopt : std::optional<std::string_view>{discriminator};
        }

        bool threadItemVocabularyIsComplete() noexcept {
            if (generated::AllThreadItemProjections.size() != StableThreadItemKinds.size()) {
                return false;
            }

            for (const ThreadItemKind kind : StableThreadItemKinds) {
                const std::string_view spelling = toString(kind);
                if (spelling.empty() || threadItemKindFromString(spelling) != kind) {
                    return false;
                }
                const std::size_t matches =
                    static_cast<std::size_t>(std::count_if(generated::AllThreadItemProjections.begin(),
                                                           generated::AllThreadItemProjections.end(),
                                                           [spelling](const generated::ProjectionMetadata& metadata) {
                                                               return generatedThreadItemDiscriminator(metadata.registryKey) == spelling;
                                                           }));
                if (matches != 1) {
                    return false;
                }
            }

            for (std::size_t left = 0; left < generated::AllThreadItemProjections.size(); ++left) {
                const auto leftDiscriminator = generatedThreadItemDiscriminator(generated::AllThreadItemProjections[left].registryKey);
                if (!leftDiscriminator.has_value()) {
                    return false;
                }
                const std::optional<ThreadItemKind> leftKind = threadItemKindFromString(*leftDiscriminator);
                if (!leftKind.has_value() || toString(*leftKind) != *leftDiscriminator) {
                    return false;
                }
                for (std::size_t right = left + 1; right < generated::AllThreadItemProjections.size(); ++right) {
                    const auto rightDiscriminator =
                        generatedThreadItemDiscriminator(generated::AllThreadItemProjections[right].registryKey);
                    if (!rightDiscriminator.has_value() || *leftDiscriminator == *rightDiscriminator ||
                        threadItemKindFromString(*rightDiscriminator) == leftKind) {
                        return false;
                    }
                }
            }
            return true;
        }

    } // namespace

    bool FrontendProjectionContext::hasScope(FrontendScope scope) const noexcept {
        return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
    }

    bool FrontendProjectionContext::hasCapability(FrontendCapability capability) const noexcept {
        return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
    }

    bool FrontendProjectionContext::hasAllScopes(std::span<const FrontendScope> required) const noexcept {
        return std::all_of(required.begin(), required.end(), [this](FrontendScope scope) {
            return hasScope(scope);
        });
    }

    FrontendProjectionContext makeProjectionContext(const FrontendPrincipal& principal,
                                                    std::span<const FrontendCapability> negotiatedCapabilities) noexcept {
        try {
            FrontendProjectionContext context{
                principal.scopes, std::vector<FrontendCapability>{negotiatedCapabilities.begin(), negotiatedCapabilities.end()}};
            if (context.scopes.size() > LocalTrustedScopes.size()) {
                context.scopes.resize(LocalTrustedScopes.size());
            }
            if (context.capabilities.size() > 18) {
                context.capabilities.resize(18);
            }
            std::sort(context.scopes.begin(), context.scopes.end());
            context.scopes.erase(std::unique(context.scopes.begin(), context.scopes.end()), context.scopes.end());
            std::sort(context.capabilities.begin(), context.capabilities.end());
            context.capabilities.erase(std::unique(context.capabilities.begin(), context.capabilities.end()), context.capabilities.end());
            return context;
        } catch (...) {
            return {};
        }
    }

    std::vector<FrontendScope> itemCommandOutputRequiredScopes(const Json& item) noexcept {
        try {
            switch (itemCommandOutputCeiling(item)) {
                case ItemCommandOutputCeiling::CommandExecution:
                    return {FrontendScope::CommandExecution};
                case ItemCommandOutputCeiling::FilesystemWrite:
                    return {FrontendScope::FilesystemWrite};
                case ItemCommandOutputCeiling::Conservative:
                    return {FrontendScope::CommandExecution, FrontendScope::FilesystemWrite};
            }
        } catch (...) {
        }
        return {FrontendScope::CommandExecution, FrontendScope::FilesystemWrite};
    }

    CanonicalSnapshotRecord canonicalizeSnapshot(CanonicalSnapshotRecord input, FrontendProjectionLimits limits) noexcept {
        try {
            input.sanitization = {};
            input.maximumReportedPaths = limits.maximumReportedPaths;
            input.extensions.erase("scopeProjection");
            std::size_t remainingVisits = limits.maximumVisits;
            input.legacyState = sanitizeScopedValue(std::move(input.legacyState), limits, remainingVisits, input.sanitization);
            input.expandedState = sanitizeScopedValue(std::move(input.expandedState), limits, remainingVisits, input.sanitization);
            FrontendProjectionLimits remainingLimits = limits;
            remainingLimits.maximumVisits = remainingVisits;
            Sanitizer extensionSanitizer{remainingLimits, {}, false};
            input.extensions = extensionSanitizer.sanitize(input.extensions, 0);
            mergeStatistics(input.sanitization, extensionSanitizer.statistics);
            return input;
        } catch (...) {
            input.legacyState = {};
            input.expandedState = {};
            input.extensions = Json::object();
            input.sanitization.failed = true;
            input.sanitization.truncated = true;
            return input;
        }
    }

    CanonicalEventRecord canonicalizeEvent(CanonicalEventRecord input, FrontendProjectionLimits limits) noexcept {
        try {
            input.sanitization = {};
            input.maximumReportedPaths = limits.maximumReportedPaths;
            input.extensions.erase("scopeProjection");
            std::size_t remainingVisits = limits.maximumVisits;
            if (input.legacyType.size() > limits.maximumStringBytes) {
                input.legacyType.resize(utf8PrefixLength(input.legacyType, limits.maximumStringBytes));
                saturatingIncrement(input.sanitization.stringsTruncated);
                input.sanitization.truncated = true;
            }
            input.legacyData = sanitizeScopedValue(std::move(input.legacyData), limits, remainingVisits, input.sanitization);
            if (input.expandedEvents.size() > limits.maximumArrayItems) {
                saturatingAdd(input.sanitization.valuesOmittedByBounds, input.expandedEvents.size() - limits.maximumArrayItems);
                input.expandedEvents.resize(limits.maximumArrayItems);
                input.sanitization.truncated = true;
            }
            for (CanonicalExpandedEvent& event : input.expandedEvents) {
                event.data = sanitizeScopedValue(std::move(event.data), limits, remainingVisits, input.sanitization);
                if (event.requiredScopes.size() > LocalTrustedScopes.size()) {
                    event.requiredScopes.resize(LocalTrustedScopes.size());
                    saturatingIncrement(input.sanitization.valuesOmittedByBounds);
                    input.sanitization.truncated = true;
                }
                std::sort(event.requiredScopes.begin(), event.requiredScopes.end());
                event.requiredScopes.erase(std::unique(event.requiredScopes.begin(), event.requiredScopes.end()),
                                           event.requiredScopes.end());
            }
            if (input.registryKey.has_value() && input.registryKey->size() > limits.maximumStringBytes) {
                input.registryKey->resize(utf8PrefixLength(*input.registryKey, limits.maximumStringBytes));
                saturatingIncrement(input.sanitization.stringsTruncated);
                input.sanitization.truncated = true;
            }
            if (input.requiredScopes.size() > LocalTrustedScopes.size()) {
                input.requiredScopes.resize(LocalTrustedScopes.size());
                saturatingIncrement(input.sanitization.valuesOmittedByBounds);
                input.sanitization.truncated = true;
            }
            std::sort(input.requiredScopes.begin(), input.requiredScopes.end());
            input.requiredScopes.erase(std::unique(input.requiredScopes.begin(), input.requiredScopes.end()), input.requiredScopes.end());
            FrontendProjectionLimits remainingLimits = limits;
            remainingLimits.maximumVisits = remainingVisits;
            Sanitizer extensionSanitizer{remainingLimits, {}, false};
            input.extensions = extensionSanitizer.sanitize(input.extensions, 0);
            mergeStatistics(input.sanitization, extensionSanitizer.statistics);
            return input;
        } catch (...) {
            input.legacyType.clear();
            input.legacyData = {};
            input.expandedEvents.clear();
            input.extensions = Json::object();
            input.sanitization.failed = true;
            input.sanitization.truncated = true;
            return input;
        }
    }

    std::optional<SnapshotProjection> projectSnapshot(const CanonicalSnapshotRecord& record,
                                                      const FrontendProjectionContext& context) noexcept {
        try {
            if (record.sanitization.failed || !context.hasScope(FrontendScope::Observe)) {
                return std::nullopt;
            }
            const bool expanded = context.hasCapability(FrontendCapability::CompleteBackendDomains);
            const bool completeItems = context.hasCapability(FrontendCapability::CompleteThreadItems);
            const bool dedicatedPending = context.hasCapability(FrontendCapability::DedicatedPendingRequests);

            // Mandatory security projection precedes representation choice.
            // This prevents capability selection from ever bypassing a rule
            // attached to either canonical compatibility view.
            ProjectionState legacyProjection{context, {}, {}, record.maximumReportedPaths};
            ProjectionState expandedProjection{context, {}, {}, record.maximumReportedPaths};
            Json projectedLegacy = projectValue(record.legacyState.value, record.legacyState.rules, legacyProjection);
            Json projectedExpanded = projectValue(record.expandedState.value, record.expandedState.rules, expandedProjection);
            projectLegacySnapshotItemCommandOutput(projectedLegacy, legacyProjection);
            projectExpandedSnapshotItemCommandOutput(projectedExpanded, expandedProjection);
            if (expanded && !completeItems && projectedExpanded.is_object()) {
                if (const auto items = projectedExpanded.find("items"); items != projectedExpanded.end()) {
                    projectedExpanded["items"] = metadataCompatibleItems(*items, expandedProjection);
                }
            }

            Json selected = expanded ? std::move(projectedExpanded) : std::move(projectedLegacy);
            std::vector<std::string> omittedFields =
                expanded ? std::move(expandedProjection.omittedFields) : std::move(legacyProjection.omittedFields);
            std::vector<std::string> redactedFields =
                expanded ? std::move(expandedProjection.redactedFields) : std::move(legacyProjection.redactedFields);
            if (expanded) {
                if (!dedicatedPending && selected.is_object()) {
                    selected.erase("pendingRequests");
                }
            } else if (selected.is_object() && projectedExpanded.is_object()) {
                if (completeItems) {
                    if (const auto items = projectedExpanded.find("items"); items != projectedExpanded.end()) {
                        selected["items"] = *items;
                    }
                }
                if (dedicatedPending) {
                    if (const auto pending = projectedExpanded.find("pendingRequests"); pending != projectedExpanded.end()) {
                        selected["pendingRequests"] = *pending;
                    }
                }
                if (completeItems || dedicatedPending) {
                    omittedFields.insert(
                        omittedFields.end(), expandedProjection.omittedFields.begin(), expandedProjection.omittedFields.end());
                    redactedFields.insert(
                        redactedFields.end(), expandedProjection.redactedFields.begin(), expandedProjection.redactedFields.end());
                }
            }
            Json extensions = record.extensions;
            if (context.hasCapability(FrontendCapability::ScopeProjectedState)) {
                addProjectionMetadata(extensions, omittedFields, redactedFields);
            }
            normalizePaths(omittedFields);
            normalizePaths(redactedFields);
            return SnapshotProjection{Snapshot{record.sequence, std::move(selected), std::move(extensions)},
                                      expanded,
                                      std::move(omittedFields),
                                      std::move(redactedFields)};
        } catch (...) {
            return std::nullopt;
        }
    }

    EventProjection projectEvent(const CanonicalEventRecord& record,
                                 const FrontendProjectionContext& context,
                                 std::optional<SequenceNumber> replayAfter) noexcept {
        EventProjection projection;
        try {
            if (record.snapshotRequired || record.sanitization.failed || !context.hasAllScopes(record.requiredScopes)) {
                return projection;
            }
            if (replayAfter.has_value() && record.sequence <= *replayAfter) {
                return projection;
            }

            ProjectionState legacyState{context, {}, {}, record.maximumReportedPaths};
            Json legacyData = projectValue(record.legacyData.value, record.legacyData.rules, legacyState);
            projectLegacyEventItemCommandOutput(record.legacyType, legacyData, legacyState);

            struct FilteredExpandedEvent {
                ExpandedEventType type;
                Json data;
                ProjectionState state;
            };
            std::vector<FilteredExpandedEvent> filteredExpanded;
            filteredExpanded.reserve(record.expandedEvents.size());
            std::vector<std::string> omittedEventFamilies;
            for (const CanonicalExpandedEvent& expanded : record.expandedEvents) {
                const ExpandedEventProjectionMetadata* metadata = eventMetadata(expanded.type);
                if (metadata == nullptr || (metadata->privilegedScope.has_value() && !context.hasScope(*metadata->privilegedScope)) ||
                    !context.hasAllScopes(expanded.requiredScopes)) {
                    omittedEventFamilies.push_back("/event/" + std::string(toString(expanded.type)));
                    continue;
                }
                ProjectionState eventState{context, {}, {}, record.maximumReportedPaths};
                Json data = projectValue(expanded.data.value, expanded.data.rules, eventState);
                projectExpandedEventItemCommandOutput(expanded.type, data, eventState);
                filteredExpanded.push_back(FilteredExpandedEvent{expanded.type, std::move(data), std::move(eventState)});
            }

            // Negotiating fewer representation capabilities cannot reveal
            // that an occurrence happened. If every dedicated family is
            // outside the principal's scope ceiling, suppress its legacy
            // compatibility representation as well.
            if (!record.expandedEvents.empty() && filteredExpanded.empty()) {
                return projection;
            }

            // Select the wire representation only after both canonical views
            // have passed mandatory scope filtering.
            projection.expanded = !record.expandedEvents.empty() && context.hasCapability(effectiveExpansionCapability(record));
            ProjectionState aggregate{context, {}, {}, record.maximumReportedPaths};
            if (!projection.expanded) {
                aggregate.omittedFields = std::move(legacyState.omittedFields);
                aggregate.redactedFields = std::move(legacyState.redactedFields);
                Json extensions = record.extensions;
                if (context.hasCapability(FrontendCapability::ScopeProjectedState)) {
                    addProjectionMetadata(extensions, aggregate.omittedFields, aggregate.redactedFields);
                }
                projection.events.push_back(
                    FrontendEvent{record.sequence, record.legacyType, std::move(legacyData), std::move(extensions)});
            } else {
                for (FilteredExpandedEvent& expanded : filteredExpanded) {
                    ProjectionState& eventState = expanded.state;
                    eventState.omittedFields.insert(
                        eventState.omittedFields.end(), omittedEventFamilies.begin(), omittedEventFamilies.end());
                    Json extensions = record.extensions;
                    if (context.hasCapability(FrontendCapability::ScopeProjectedState)) {
                        addProjectionMetadata(extensions, eventState.omittedFields, eventState.redactedFields);
                    }
                    aggregate.omittedFields.insert(
                        aggregate.omittedFields.end(), eventState.omittedFields.begin(), eventState.omittedFields.end());
                    aggregate.redactedFields.insert(
                        aggregate.redactedFields.end(), eventState.redactedFields.begin(), eventState.redactedFields.end());
                    projection.events.push_back(FrontendEvent{
                        record.sequence, std::string(toString(expanded.type)), std::move(expanded.data), std::move(extensions)});
                }
            }

            normalizePaths(aggregate.omittedFields);
            normalizePaths(aggregate.redactedFields);
            projection.omittedFields = std::move(aggregate.omittedFields);
            projection.redactedFields = std::move(aggregate.redactedFields);
            return projection;
        } catch (...) {
            return {};
        }
    }

    std::optional<std::size_t> canonicalEventRetainedBytes(const CanonicalEventRecord& record) noexcept {
        try {
            if (record.snapshotRequired || record.sanitization.failed || record.legacyType.empty() ||
                !canonicalValueContainsNoKnownStructuredSecrets(record.legacyData.value) ||
                !canonicalValueContainsNoKnownStructuredSecrets(record.extensions)) {
                return std::nullopt;
            }

            std::size_t retainedBytes = 1'024;
            const auto add = [&retainedBytes](std::size_t bytes) {
                retainedBytes = bytes > std::numeric_limits<std::size_t>::max() - retainedBytes ? std::numeric_limits<std::size_t>::max()
                                                                                                : retainedBytes + bytes;
            };
            const auto addRules = [&add](const std::vector<ScopeProjectionRule>& rules) {
                for (const ScopeProjectionRule& rule : rules) {
                    add(rule.path.size());
                    add(rule.requiredScopes.size() * sizeof(FrontendScope));
                    add(sizeof(ScopeProjectionAction));
                }
            };

            add(record.legacyType.size());
            add(record.legacyData.value.dump().size());
            add(record.extensions.dump().size());
            addRules(record.legacyData.rules);
            add(record.requiredScopes.size() * sizeof(FrontendScope));
            if (record.registryKey.has_value()) {
                add(record.registryKey->size());
            }
            for (const CanonicalExpandedEvent& expanded : record.expandedEvents) {
                if (!canonicalValueContainsNoKnownStructuredSecrets(expanded.data.value)) {
                    return std::nullopt;
                }
                add(toString(expanded.type).size());
                add(expanded.data.value.dump().size());
                addRules(expanded.data.rules);
                add(expanded.requiredScopes.size() * sizeof(FrontendScope));
            }
            return retainedBytes;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool canonicalValueContainsNoKnownStructuredSecrets(const Json& value) noexcept {
        try {
            std::size_t remainingVisits = FrontendProjectionLimits{}.maximumVisits;
            return containsNoKnownStructuredSecrets(value, 0, remainingVisits);
        } catch (...) {
            return false;
        }
    }

    const generated::ProjectionMetadata* notificationProjection(std::string_view registryKey) noexcept {
        const auto iterator = std::find_if(generated::AllNotificationProjections.begin(),
                                           generated::AllNotificationProjections.end(),
                                           [registryKey](const generated::ProjectionMetadata& metadata) {
                                               return metadata.registryKey == registryKey;
                                           });
        return iterator == generated::AllNotificationProjections.end() ? nullptr : &*iterator;
    }

    const generated::ProjectionMetadata* threadItemProjection(std::string_view registryKey) noexcept {
        const auto iterator = std::find_if(generated::AllThreadItemProjections.begin(),
                                           generated::AllThreadItemProjections.end(),
                                           [registryKey](const generated::ProjectionMetadata& metadata) {
                                               return metadata.registryKey == registryKey;
                                           });
        return iterator == generated::AllThreadItemProjections.end() ? nullptr : &*iterator;
    }

    const generated::PendingRequestProjectionMetadata* pendingRequestProjection(PendingRequestKind kind) noexcept {
        return generated::pendingRequestProjectionFromKind(toString(kind));
    }

    bool projectionMetadataIsComplete() noexcept {
        try {
            if (!uniqueNotificationAndItemKeys() || !threadItemVocabularyIsComplete()) {
                return false;
            }

            for (const generated::PendingRequestProjectionMetadata& metadata : generated::AllPendingRequestProjections) {
                const std::optional<PendingRequestKind> kind = pendingRequestKindFromString(metadata.kind);
                if (!kind.has_value() || pendingRequestProjection(*kind) != &metadata || metadata.registryKey.empty() ||
                    metadata.providerMethod.empty() || metadata.legacyContract.empty() || metadata.responseMethods.empty() ||
                    metadata.exposure != "DedicatedPendingRequestContract" ||
                    metadata.securityDecision != "ScopeProjectedStateEventApproved" || metadata.redactionClass != "safe_pending_request" ||
                    metadata.presentationRequiredScopes.size() != 1 ||
                    metadata.presentationRequiredScopes.front() != FrontendScope::Observe || metadata.controllerRequiredForPresentation ||
                    metadata.responseRequiredScopes.size() != 2 ||
                    std::find(metadata.responseRequiredScopes.begin(), metadata.responseRequiredScopes.end(), FrontendScope::Control) ==
                        metadata.responseRequiredScopes.end() ||
                    std::find(metadata.responseRequiredScopes.begin(),
                              metadata.responseRequiredScopes.end(),
                              FrontendScope::SensitiveResponse) == metadata.responseRequiredScopes.end() ||
                    !metadata.controllerRequiredForResponse || metadata.expandedEvent != "pendingRequests.updated" ||
                    metadata.duplicateSuppression != "exactly_one_compatibility_representation_per_connection" ||
                    metadata.expansionCapability != generated::Capability::DedicatedPendingRequests) {
                    return false;
                }
                for (const std::string_view responseMethod : metadata.responseMethods) {
                    const std::optional<generated::MethodId> id = generated::definedMethodFromString(responseMethod);
                    if (!id.has_value() ||
                        generated::AllMethods[static_cast<std::size_t>(*id)].category != generated::MethodCategory::ReverseResponse ||
                        !generated::AllMethods[static_cast<std::size_t>(*id)].currentlyImplemented) {
                        return false;
                    }
                }
            }

            for (std::size_t left = 0; left < AllExpandedEventProjections.size(); ++left) {
                const ExpandedEventProjectionMetadata& metadata = AllExpandedEventProjections[left];
                if (expandedEventTypeFromString(toString(metadata.type)) != metadata.type) {
                    return false;
                }
                for (std::size_t right = left + 1; right < AllExpandedEventProjections.size(); ++right) {
                    if (metadata.type == AllExpandedEventProjections[right].type) {
                        return false;
                    }
                }
            }
            return true;
        } catch (...) {
            return false;
        }
    }

} // namespace ai::openai::codex::frontend::detail
