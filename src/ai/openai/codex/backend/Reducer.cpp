/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/backend/Reducer.h"

#include "ai/openai/codex/backend/detail/PreserveUnmodeledTypedEvent.h"
#include "ai/openai/codex/backend/internal/RetentionCapacityInstrumentation.h"
#include "ai/openai/codex/detail/ConversationCodec.h"
#include "ai/openai/codex/detail/DecodeDiagnostic.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "log/LogScopeOwner.h"
#include "log/Logger.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ai::openai::codex::backend {

    namespace detail {
        namespace {
            thread_local RetentionCapacityInstrumentation retentionInstrumentation;
        }

        void resetRetentionCapacityInstrumentation() noexcept {
            retentionInstrumentation = {};
        }

        RetentionCapacityInstrumentation retentionCapacityInstrumentation() noexcept {
            return retentionInstrumentation;
        }

        void recordRetentionCapacitySlowPath() noexcept {
            if (retentionInstrumentation.slowPathEntries != std::numeric_limits<std::size_t>::max()) {
                ++retentionInstrumentation.slowPathEntries;
            }
        }

        void recordPendingReferenceBuild() noexcept {
            if (retentionInstrumentation.pendingReferenceBuilds != std::numeric_limits<std::size_t>::max()) {
                ++retentionInstrumentation.pendingReferenceBuilds;
            }
        }
    } // namespace detail

    namespace {
        template <typename... Visitors>
        struct Overloaded : Visitors... {
            using Visitors::operator()...;
        };

        template <typename... Visitors>
        Overloaded(Visitors...) -> Overloaded<Visitors...>;

        template <typename Unsigned>
        std::uint64_t saturatingUint64(Unsigned value) noexcept {
            static_assert(std::is_unsigned_v<Unsigned>);
            if constexpr (std::numeric_limits<Unsigned>::digits > std::numeric_limits<std::uint64_t>::digits) {
                if (value > static_cast<Unsigned>(std::numeric_limits<std::uint64_t>::max())) {
                    return std::numeric_limits<std::uint64_t>::max();
                }
            }
            return static_cast<std::uint64_t>(value);
        }

        void saturatingAdd(std::uint64_t& value, std::uint64_t amount = 1) noexcept {
            value = amount > std::numeric_limits<std::uint64_t>::max() - value ? std::numeric_limits<std::uint64_t>::max() : value + amount;
        }

        void saturatingAddSize(std::size_t& value, std::size_t amount) noexcept {
            value = amount > std::numeric_limits<std::size_t>::max() - value ? std::numeric_limits<std::size_t>::max() : value + amount;
        }

        void guardedSubtractSize(std::size_t& value, std::size_t amount) noexcept {
            value = amount > value ? 0 : value - amount;
        }

        void replaceAccumulatedContent(CapacityState& capacity, std::size_t before, std::size_t after) noexcept {
            if (after >= before) {
                saturatingAddSize(capacity.accumulatedContentBytes, after - before);
            } else {
                guardedSubtractSize(capacity.accumulatedContentBytes, before - after);
            }
        }

        bool sameError(const std::optional<Error>& left, const std::optional<Error>& right) noexcept {
            return left.has_value() == right.has_value() &&
                   (!left || (left->category == right->category && left->code == right->code && left->message == right->message));
        }

        bool sameInitialization(const std::optional<typed::InitializeResponse>& left,
                                const std::optional<typed::InitializeResponse>& right) noexcept {
            if (left.has_value() != right.has_value()) {
                return false;
            }
            if (!left) {
                return true;
            }
            try {
                return left->codexHome.value == right->codexHome.value && left->platformFamily == right->platformFamily &&
                       left->platformOs == right->platformOs && left->userAgent == right->userAgent && left->raw == right->raw;
            } catch (...) {
                return false;
            }
        }

        bool sameProvider(const ProviderState& left, const ProviderState& right) noexcept {
            return left.lifecycle == right.lifecycle && left.generation == right.generation &&
                   left.desiredRunning == right.desiredRunning && sameError(left.lastError, right.lastError) &&
                   left.recovery == right.recovery && sameInitialization(left.initialization, right.initialization);
        }

        SourceStamp currentStamp(const BackendState& state) noexcept {
            return {state.provider.generation, Freshness::Current};
        }

        template <typename T>
        void applyTurnOverride(T& destination, const typed::OptionalNullable<T>& overrideValue) {
            if (overrideValue.hasValue()) {
                destination = *overrideValue.value;
            }
        }

        template <typename T>
        void applyTurnOverride(typed::OptionalNullable<T>& destination, const typed::OptionalNullable<T>& overrideValue) {
            if (overrideValue.hasValue()) {
                destination = overrideValue;
            }
        }

        typed::ThreadSettings retainedExecutionConfiguration(typed::ThreadSettings value) {
            value.raw = Json::object();
            value.diagnostics.clear();
            if (value.activePermissionProfile.hasValue()) {
                value.activePermissionProfile->raw = Json::object();
                value.activePermissionProfile->diagnostics.clear();
            }
            value.collaborationMode.raw = Json::object();
            value.collaborationMode.diagnostics.clear();
            value.collaborationMode.settings.raw = Json::object();
            value.collaborationMode.settings.diagnostics.clear();
            return value;
        }

        std::optional<typed::ThreadSettings> effectiveExecutionConfiguration(const ThreadState& thread,
                                                                              const typed::TurnStartParams& params) {
            if (!thread.executionConfiguration) {
                return std::nullopt;
            }
            // An explicit null asks the app-server to resolve a default/reset.
            // The prior ThreadSettings value is not authoritative for that
            // result, so wait for thread/settings/updated instead of guessing.
            if (params.approvalPolicy.isNull() || params.approvalsReviewer.isNull() || params.cwd.isNull() ||
                params.effort.isNull() || params.model.isNull() || params.personality.isNull() ||
                params.sandboxPolicy.isNull() || params.serviceTier.isNull() || params.summary.isNull() ||
                params.collaborationMode.isNull()) {
                return std::nullopt;
            }
            typed::ThreadSettings result = *thread.executionConfiguration;
            applyTurnOverride(result.approvalPolicy, params.approvalPolicy);
            applyTurnOverride(result.approvalsReviewer, params.approvalsReviewer);
            if (params.cwd.hasValue()) {
                result.cwd = typed::AbsolutePath{*params.cwd.value};
            }
            applyTurnOverride(result.effort, params.effort);
            applyTurnOverride(result.model, params.model);
            applyTurnOverride(result.personality, params.personality);
            applyTurnOverride(result.sandboxPolicy, params.sandboxPolicy);
            applyTurnOverride(result.serviceTier, params.serviceTier);
            applyTurnOverride(result.summary, params.summary);
            if (params.collaborationMode.hasValue()) {
                result.collaborationMode = *params.collaborationMode.value;
                result.model = result.collaborationMode.settings.model;
                if (!result.collaborationMode.settings.reasoningEffort.isOmitted()) {
                    result.effort = result.collaborationMode.settings.reasoningEffort;
                }
            }
            return retainedExecutionConfiguration(std::move(result));
        }

        constexpr std::size_t MaxRetainedFilesystemChangePaths = 256;
        constexpr std::size_t MaxRetainedFuzzyResultsPerSession = 512;
        constexpr std::size_t MaxRetainedFuzzyIndicesPerResult = 256;
        constexpr std::size_t MaxRetainedAppCatalogEntries = 256;
        constexpr std::size_t MaxRetainedModelVerifications = 256;
        constexpr std::size_t MaxCanonicalIdentifierBytes = 256;

        ProviderDomainState& notificationDomain(BackendState& state, std::string_view method) noexcept {
            if (method.starts_with("account/")) {
                return state.accounts;
            }
            if (method.starts_with("model/") || method.starts_with("modelProvider/")) {
                return state.models;
            }
            if (method.starts_with("config") || method.starts_with("experimentalFeature/")) {
                return state.configuration;
            }
            if (method.starts_with("thread/") || method.starts_with("turn/") || method.starts_with("item/") ||
                method.starts_with("command/") || method.starts_with("process/") || method == "error") {
                return state.conversations;
            }
            if (method.starts_with("fs/") || method.starts_with("fuzzyFileSearch/")) {
                return state.filesystem;
            }
            if (method.starts_with("guardian") || method.starts_with("review/") || method.starts_with("permissionProfile/")) {
                return state.reviews;
            }
            if (method.starts_with("skills/") || method.starts_with("plugin/")) {
                return state.pluginsAndSkills;
            }
            if (method.starts_with("mcpServer/") || method.starts_with("mcpServerStatus/")) {
                return state.mcp;
            }
            if (method.starts_with("remoteControl/") || method.starts_with("windows")) {
                return state.platform;
            }
            return state.integrations;
        }

        void markOperationStale(BackendState& state, std::string_view method) noexcept {
            const auto operation = state.providerOperations.find(std::string{method});
            if (operation != state.providerOperations.end()) {
                operation->second.stamp.freshness = Freshness::Stale;
            }
            ProviderDomainState& domain = notificationDomain(state, method);
            const auto summary = domain.latestResults.find(std::string{method});
            if (summary != domain.latestResults.end()) {
                summary->second.stamp.freshness = Freshness::Stale;
            }
            const auto staleState = [](auto& value) {
                if (value) {
                    value->stamp.freshness = Freshness::Stale;
                }
            };
            if (method == "thread/list") {
                state.threadList.stamp.freshness = Freshness::Stale;
            } else if (method == "thread/goal/get") {
                staleState(state.conversations.latestGoal);
            } else if (method == "account/rateLimits/read") {
                staleState(state.accounts.rateLimits);
            } else if (method == "account/read") {
                staleState(state.accounts.authentication);
            } else if (method == "app/list") {
                staleState(state.integrations.apps);
            } else if (method == "mcpServerStatus/list") {
                staleState(state.mcp.statusList);
            } else if (method == "windowsSandbox/readiness") {
                staleState(state.platform.windowsSandbox);
            }
        }

        void markDomainStale(ProviderDomainState& domain) noexcept {
            for (auto& [method, notification] : domain.latestNotifications) {
                (void) method;
                notification.stamp.freshness = Freshness::Stale;
            }
            for (auto& [method, result] : domain.latestResults) {
                (void) method;
                result.stamp.freshness = Freshness::Stale;
            }
        }

        void markThreadStale(BackendState& state, const typed::ThreadId& threadId) noexcept {
            const auto thread = state.threads.find(threadId.value);
            if (thread != state.threads.end()) {
                thread->second.stamp.freshness = Freshness::Stale;
            }
        }

        void markLatestGoalStale(BackendState& state, const typed::ThreadId& threadId) noexcept {
            if (state.conversations.latestGoal && state.conversations.latestGoal->threadId == threadId.value) {
                markOperationStale(state, "thread/goal/get");
            }
        }

        void markTurnStale(BackendState& state, const typed::ThreadId& threadId, const typed::TurnId& turnId) noexcept {
            const auto thread = state.threads.find(threadId.value);
            if (thread == state.threads.end()) {
                return;
            }
            const auto turn = thread->second.turns.find(turnId.value);
            if (turn != thread->second.turns.end()) {
                turn->second.stamp.freshness = Freshness::Stale;
            }
        }

        void markProcessCurrent(BackendState& state, const typed::CommandExecProcessId& processId, std::string lifecycle) {
            const auto process = state.processes.find(processId.value);
            if (process == state.processes.end()) {
                return;
            }
            process->second.lifecycle = std::move(lifecycle);
            process->second.stamp = currentStamp(state);
            process->second.connectionInvalidated = false;
        }

        void retainDomainNotification(BackendState& state, const std::string& method, const typed::Event& event) {
            ProviderDomainState& domain = notificationDomain(state, method);
            const SourceStamp stamp = currentStamp(state);
            domain.latestNotifications.insert_or_assign(method, ProviderNotificationState{method, event.index(), stamp});
        }

        std::string boundedText(std::string value, std::size_t limit) {
            if (value.size() > limit) {
                value.resize(limit);
            }
            return value;
        }

        std::size_t boundedUtf8PrefixLength(std::string_view value, std::size_t byteLimit) noexcept {
            std::size_t offset = 0;
            while (offset < value.size() && offset < byteLimit) {
                const unsigned char first = static_cast<unsigned char>(value[offset]);
                std::size_t width = 0;
                if (first <= 0x7fU) {
                    width = 1;
                } else if (first >= 0xc2U && first <= 0xdfU) {
                    width = 2;
                } else if (first >= 0xe0U && first <= 0xefU) {
                    width = 3;
                } else if (first >= 0xf0U && first <= 0xf4U) {
                    width = 4;
                } else {
                    break;
                }
                if (offset + width > value.size() || offset + width > byteLimit) {
                    break;
                }
                bool valid = true;
                for (std::size_t index = 1; index < width; ++index) {
                    valid = valid &&
                            (static_cast<unsigned char>(value[offset + index]) & 0xc0U) == 0x80U;
                }
                if (valid && width == 3) {
                    const unsigned char second = static_cast<unsigned char>(value[offset + 1]);
                    valid = (first != 0xe0U || second >= 0xa0U) && (first != 0xedU || second <= 0x9fU);
                } else if (valid && width == 4) {
                    const unsigned char second = static_cast<unsigned char>(value[offset + 1]);
                    valid = (first != 0xf0U || second >= 0x90U) && (first != 0xf4U || second <= 0x8fU);
                }
                if (!valid) {
                    break;
                }
                offset += width;
            }
            return offset;
        }

        std::string boundedUtf8Text(std::string_view value, std::size_t limit) {
            return std::string(value.substr(0, boundedUtf8PrefixLength(value, limit)));
        }

        std::size_t utf8PrefixRemovalLength(std::string_view value, std::size_t minimumBytes) noexcept {
            std::size_t removed = std::min(minimumBytes, value.size());
            while (removed < value.size() &&
                   (static_cast<unsigned char>(value[removed]) & 0xc0U) == 0x80U) {
                ++removed;
            }
            return removed;
        }

        std::string boundedIdentity(std::string_view kind, std::string_view value) {
            constexpr std::uint64_t FnvOffset = 1469598103934665603ULL;
            constexpr std::uint64_t FnvPrime = 1099511628211ULL;
            std::uint64_t hash = FnvOffset;
            for (const unsigned char byte : value) {
                hash ^= byte;
                hash *= FnvPrime;
            }
            const std::string prefix = std::string{kind} + ":";
            const std::string suffix = "#" + std::to_string(hash);
            if (prefix.size() + value.size() <= MaxCanonicalIdentifierBytes) {
                return prefix + std::string{value};
            }
            const std::size_t retained = MaxCanonicalIdentifierBytes > prefix.size() + suffix.size()
                                             ? MaxCanonicalIdentifierBytes - prefix.size() - suffix.size()
                                             : 0;
            return prefix + std::string{value.substr(0, retained)} + suffix;
        }

        std::string boundedIdentifier(std::string_view value) {
            if (value.size() <= MaxCanonicalIdentifierBytes) {
                return std::string{value};
            }
            constexpr std::uint64_t FnvOffset = 1469598103934665603ULL;
            constexpr std::uint64_t FnvPrime = 1099511628211ULL;
            std::uint64_t hash = FnvOffset;
            for (const unsigned char byte : value) {
                hash ^= byte;
                hash *= FnvPrime;
            }
            const std::string suffix = "#" + std::to_string(hash);
            return std::string{value.substr(0, MaxCanonicalIdentifierBytes - suffix.size())} + suffix;
        }

        ProviderResultSummaryState summarizeProviderResult(const std::string& method,
                                                           const ProviderOperationValue& value,
                                                           const BackendState& state,
                                                           const ReducerOptions& options) {
            ProviderResultSummaryState summary;
            summary.method = method;
            summary.resultAlternative = value.index();
            summary.stamp = currentStamp(state);
            std::visit(
                [&summary, &options](const auto& result) {
                    if constexpr (requires { result.data.size(); }) {
                        summary.itemCount = result.data.size();
                    } else if constexpr (requires { result.files.size(); }) {
                        summary.itemCount = result.files.size();
                    }
                    if constexpr (requires { result.nextCursor.hasValue(); }) {
                        if (result.nextCursor.hasValue()) {
                            summary.nextCursor = boundedText(*result.nextCursor.value, options.maxNoticeDetailsBytes);
                            summary.complete = false;
                        }
                    }
                    if constexpr (requires { result.started; }) {
                        summary.status = result.started ? "started" : "not_started";
                    }
                    if constexpr (requires { result.status.value; }) {
                        summary.status = boundedText(result.status.value, MaxCanonicalIdentifierBytes);
                    }
                    if constexpr (requires { result.cleared; }) {
                        summary.status = result.cleared ? "cleared" : "not_cleared";
                    }
                    if constexpr (requires { result.effectiveEnabled; }) {
                        summary.status = result.effectiveEnabled ? "enabled" : "disabled";
                    }
                    if constexpr (requires { result.alreadyAdded; }) {
                        summary.status = result.alreadyAdded ? "already_added" : "added";
                    }
                    if constexpr (requires { result.enablement.size(); }) {
                        summary.itemCount = result.enablement.size();
                    }
                    if constexpr (requires { result.principals.size(); }) {
                        summary.itemCount = result.principals.size();
                    }
                    if constexpr (requires { result.appsNeedingAuth.size(); }) {
                        summary.itemCount = result.appsNeedingAuth.size();
                    }
                    if constexpr (requires { result.marketplaceName; }) {
                        summary.subjectId = boundedText(result.marketplaceName, MaxCanonicalIdentifierBytes);
                    }
                    if constexpr (requires { result.remotePluginId; }) {
                        summary.subjectId = boundedText(result.remotePluginId, MaxCanonicalIdentifierBytes);
                    }
                    if constexpr (requires { result.pluginId; }) {
                        summary.subjectId = boundedText(result.pluginId, MaxCanonicalIdentifierBytes);
                    }
                    if constexpr (requires { result.importId; }) {
                        summary.subjectId = boundedText(result.importId, MaxCanonicalIdentifierBytes);
                    }
                    if constexpr (requires { result.reviewThreadId.value; }) {
                        summary.subjectId = boundedText(result.reviewThreadId.value, MaxCanonicalIdentifierBytes);
                    }
                },
                value);
            return summary;
        }

        template <typename Value>
        void discardRetainedWireArtifacts(Value& value) {
            if constexpr (requires { value.raw = Json::object(); }) {
                value.raw = Json::object();
            }
            if constexpr (requires { value.diagnostics.clear(); }) {
                value.diagnostics.clear();
            }
        }

        void updateAccountRateLimits(BackendState& state, const typed::RateLimitSnapshot& source, const ReducerOptions& options) {
            AccountRateLimitState retained;
            if (source.planType.hasValue()) {
                retained.planType = boundedText(source.planType.value->value, MaxCanonicalIdentifierBytes);
            }
            if (source.rateLimitReachedType.hasValue()) {
                retained.reachedType = boundedText(source.rateLimitReachedType.value->value, MaxCanonicalIdentifierBytes);
            }
            if (source.primary.hasValue()) {
                retained.primaryUsedPercent = source.primary.value->usedPercent;
                if (source.primary.value->resetsAt.hasValue()) {
                    retained.primaryResetsAt = *source.primary.value->resetsAt.value;
                }
            }
            if (source.secondary.hasValue()) {
                retained.secondaryUsedPercent = source.secondary.value->usedPercent;
                if (source.secondary.value->resetsAt.hasValue()) {
                    retained.secondaryResetsAt = *source.secondary.value->resetsAt.value;
                }
            }
            if (source.credits.hasValue()) {
                retained.hasCredits = source.credits.value->hasCredits;
                retained.unlimitedCredits = source.credits.value->unlimited;
                if (source.credits.value->balance.hasValue()) {
                    retained.creditBalance = boundedText(*source.credits.value->balance.value, options.maxNoticeDetailsBytes);
                }
            }
            retained.stamp = currentStamp(state);
            state.accounts.rateLimits = std::move(retained);
        }

        void updateAppCatalog(BackendState& state, const std::vector<typed::AppInfo>& source, const ReducerOptions& options) {
            AppCatalogState catalog;
            catalog.totalEntries = source.size();
            catalog.truncated = source.size() > MaxRetainedAppCatalogEntries;
            const std::size_t retained = std::min(source.size(), MaxRetainedAppCatalogEntries);
            catalog.entries.reserve(retained);
            for (std::size_t index = 0; index < retained; ++index) {
                const typed::AppInfo& app = source[index];
                catalog.entries.push_back({boundedText(app.id, MaxCanonicalIdentifierBytes),
                                           boundedText(app.name, options.maxNoticeSummaryBytes),
                                           app.isAccessible,
                                           app.isEnabled});
            }
            catalog.stamp = currentStamp(state);
            state.integrations.apps = std::move(catalog);
        }

        ConfigurationDomainState::WriteState configurationWriteState(const typed::ConfigWriteResponse& result,
                                                                      const BackendState& state,
                                                                      const ReducerOptions& options) {
            return {boundedText(result.filePath.value, options.maxNoticeDetailsBytes),
                    boundedText(result.status.value, options.maxNoticeDetailsBytes),
                    boundedText(result.version, options.maxNoticeDetailsBytes),
                    result.overriddenMetadata.hasValue(),
                    result.overriddenMetadata.hasValue(),
                    currentStamp(state)};
        }

        ConfigurationDomainState::FeatureEnablementState featureEnablementState(
            const typed::ExperimentalFeatureEnablementSetResponse& result,
            const BackendState& state) {
            ConfigurationDomainState::FeatureEnablementState retained;
            retained.totalEntries = result.enablement.size();
            retained.truncated = result.enablement.size() > MaxRetainedAppCatalogEntries;
            retained.entries.reserve(std::min(result.enablement.size(), MaxRetainedAppCatalogEntries));
            for (const auto& [feature, enabled] : result.enablement) {
                if (retained.entries.size() == MaxRetainedAppCatalogEntries) {
                    break;
                }
                retained.entries.emplace_back(boundedIdentifier(feature.value), enabled);
            }
            retained.stamp = currentStamp(state);
            return retained;
        }

        ConversationDomainState::GoalMutationState goalMutationState(std::string operation,
                                                                     std::optional<std::string> objective,
                                                                     std::optional<std::string> status,
                                                                     std::optional<bool> cleared,
                                                                     bool hasGoal,
                                                                     const BackendState& state,
                                                                     const ReducerOptions& options) {
            if (objective) {
                *objective = boundedText(*objective, options.maxNoticeDetailsBytes);
            }
            if (status) {
                *status = boundedText(*status, options.maxNoticeDetailsBytes);
            }
            return {std::move(operation), {}, std::move(objective), std::move(status), cleared, hasGoal, currentStamp(state)};
        }

        IntegrationsDomainState::MarketplaceMutationState marketplaceAddState(const typed::MarketplaceAddResponse& result,
                                                                               const BackendState& state,
                                                                               const ReducerOptions& options) {
            return {"add",
                    boundedText(result.marketplaceName, options.maxNoticeDetailsBytes),
                    boundedText(result.installedRoot.value, options.maxNoticeDetailsBytes),
                    0,
                    0,
                    0,
                    result.alreadyAdded,
                    false,
                    currentStamp(state)};
        }

        IntegrationsDomainState::MarketplaceMutationState marketplaceRemoveState(const typed::MarketplaceRemoveResponse& result,
                                                                                  const BackendState& state,
                                                                                  const ReducerOptions& options) {
            std::optional<std::string> installedRoot;
            if (result.installedRoot.hasValue()) {
                installedRoot = boundedText(result.installedRoot.value->value, options.maxNoticeDetailsBytes);
            }
            return {"remove",
                    boundedText(result.marketplaceName, options.maxNoticeDetailsBytes),
                    std::move(installedRoot),
                    0,
                    0,
                    0,
                    false,
                    false,
                    currentStamp(state)};
        }

        IntegrationsDomainState::MarketplaceMutationState marketplaceUpgradeState(const typed::MarketplaceUpgradeResponse& result,
                                                                                   const BackendState& state) {
            const auto retainedCount = [](std::size_t count) {
                return std::min(count, MaxRetainedAppCatalogEntries);
            };
            return {"upgrade",
                    std::nullopt,
                    std::nullopt,
                    retainedCount(result.selectedMarketplaces.size()),
                    retainedCount(result.upgradedRoots.size()),
                    retainedCount(result.errors.size()),
                    false,
                    result.selectedMarketplaces.size() > MaxRetainedAppCatalogEntries ||
                        result.upgradedRoots.size() > MaxRetainedAppCatalogEntries ||
                        result.errors.size() > MaxRetainedAppCatalogEntries,
                    currentStamp(state)};
        }

        PluginsAndSkillsDomainState::MutationState pluginMutationState(std::string operation,
                                                                       std::optional<std::string> subjectId,
                                                                       std::optional<std::string> status,
                                                                       std::size_t itemCount,
                                                                       bool truncated,
                                                                       const BackendState& state,
                                                                       const ReducerOptions& options) {
            if (subjectId) {
                *subjectId = boundedText(*subjectId, options.maxNoticeDetailsBytes);
            }
            if (status) {
                *status = boundedText(*status, options.maxNoticeDetailsBytes);
            }
            return {std::move(operation), std::move(subjectId), std::move(status), itemCount, truncated, currentStamp(state)};
        }

        void boundFuzzyResult(typed::FuzzyFileSearchResult& result, std::size_t stringLimit) {
            if (result.fileName.size() > stringLimit) {
                result.fileName.resize(stringLimit);
            }
            if (result.path.size() > stringLimit) {
                result.path.resize(stringLimit);
            }
            if (result.root.size() > stringLimit) {
                result.root.resize(stringLimit);
            }
            if (result.indices.hasValue() && result.indices.value->size() > MaxRetainedFuzzyIndicesPerResult) {
                result.indices.value->resize(MaxRetainedFuzzyIndicesPerResult);
            }
            discardRetainedWireArtifacts(result);
        }

        using TurnKey = std::tuple<std::string, std::string>;
        using ItemKey = std::tuple<std::string, std::string, std::string>;

        struct RetentionInsertions {
            std::vector<std::string> threads;
            std::vector<TurnKey> turns;
            std::vector<ItemKey> items;
        };

        using ServerNotificationTarget = ::ai::openai::codex::detail::ServerNotificationTarget;

        template <typename Notification>
        std::vector<BackendEvent> preserveTypedNotification(const Notification& value, ServerNotificationTarget target) {
            const ::ai::openai::codex::detail::ProtocolSurfaceEntry& registryEntry = ::ai::openai::codex::detail::entryFor(target);
            const std::optional<typed::DecodeDiagnostic> diagnostic =
                value.diagnostics.empty() ? std::nullopt : std::optional<typed::DecodeDiagnostic>{value.diagnostics.front()};
            CodexExtensionReceived preserved = detail::preserveUnmodeledTypedEvent(
                {std::string(registryEntry.key.name), value.raw.at("params"), std::nullopt, diagnostic});
            if constexpr (std::is_same_v<Notification, typed::ServerRequestResolvedNotification>) {
                // The provider request id is an occurrence-ownership detail.
                // Matching notifications are represented by PendingRequestRemoved;
                // the bounded marker is retained only for a conflicting thread.
                preserved.payload = Json::object({{"resolved", true}});
            }
            preserved.typedEvent = typed::Event{value};
            return {std::move(preserved)};
        }

        logger::BoundaryLogger lifecycleLog() {
            static const logger::LogScopeOwner scope(
                logger::LogOrigin::Framework, logger::LogBoundary::Connection, "ai.openai.codex.backend");
            return scope.logger(logger::Logger::semanticSink());
        }

        typed::Thread placeholderThread(const typed::ThreadId& id) {
            typed::Thread thread;
            thread.id = id;
            thread.raw = Json::object({{"backendPlaceholder", true}});
            return thread;
        }

        typed::Turn placeholderTurn(const typed::ThreadId& threadId, const typed::TurnId& turnId) {
            typed::Turn turn;
            turn.id = turnId;
            turn.threadId = threadId;
            turn.status = typed::TurnStatus::inProgress();
            turn.itemsView = typed::TurnItemsView::notLoaded();
            turn.raw = Json::object({{"backendPlaceholder", true}});
            return turn;
        }

        ThreadState& ensureThread(BackendState& state, const typed::ThreadId& id, RetentionInsertions* insertions = nullptr) {
            ThreadState initial;
            initial.thread = placeholderThread(id);
            initial.stamp = currentStamp(state);
            auto [iterator, inserted] = state.threads.try_emplace(id.value, std::move(initial));
            if (inserted) {
                state.threadOrder.push_back(id);
                saturatingAddSize(state.capacity.retainedThreads, 1);
                if (insertions != nullptr) {
                    insertions->threads.push_back(id.value);
                }
            }
            return iterator->second;
        }

        TurnState& ensureTurn(BackendState& state,
                              const typed::ThreadId& threadId,
                              const typed::TurnId& turnId,
                              RetentionInsertions* insertions = nullptr) {
            ThreadState& thread = ensureThread(state, threadId, insertions);
            TurnState initial;
            initial.turn = placeholderTurn(threadId, turnId);
            initial.stamp = currentStamp(state);
            auto [iterator, inserted] = thread.turns.try_emplace(turnId.value, std::move(initial));
            if (inserted) {
                thread.turnOrder.push_back(turnId);
                saturatingAddSize(state.capacity.retainedTurns, 1);
                if (insertions != nullptr) {
                    insertions->turns.emplace_back(threadId.value, turnId.value);
                }
            }
            return iterator->second;
        }

        void assignBounded(std::string& target,
                           const std::string& value,
                           std::size_t limit,
                           std::uint64_t& dropped,
                           std::uint64_t* channelDropped = nullptr) {
            const auto recordDropped = [&dropped, channelDropped](std::size_t bytes) {
                saturatingAdd(dropped, saturatingUint64(bytes));
                if (channelDropped != nullptr) {
                    saturatingAdd(*channelDropped, saturatingUint64(bytes));
                }
            };
            const std::size_t validBytes = boundedUtf8PrefixLength(value, value.size());
            if (validBytes != value.size()) {
                recordDropped(value.size() - validBytes);
            }
            const std::string_view validValue{value.data(), validBytes};
            if (limit == 0) {
                recordDropped(validValue.size());
                target.clear();
            } else if (validValue.size() > limit) {
                const std::size_t removed = utf8PrefixRemovalLength(validValue, validValue.size() - limit);
                recordDropped(removed);
                target.assign(validValue.substr(removed));
            } else {
                target = validValue;
            }
        }

        void appendBounded(std::string& target,
                           const std::string& value,
                           std::size_t limit,
                           std::uint64_t& dropped,
                           std::uint64_t* channelDropped = nullptr) {
            const auto recordDropped = [&dropped, channelDropped](std::size_t bytes) {
                saturatingAdd(dropped, saturatingUint64(bytes));
                if (channelDropped != nullptr) {
                    saturatingAdd(*channelDropped, saturatingUint64(bytes));
                }
            };
            const std::size_t validBytes = boundedUtf8PrefixLength(value, value.size());
            if (validBytes != value.size()) {
                recordDropped(value.size() - validBytes);
            }
            const std::string_view validValue{value.data(), validBytes};
            if (validValue.empty()) {
                return;
            }
            if (limit == 0) {
                recordDropped(target.size());
                recordDropped(validValue.size());
                target.clear();
                return;
            }
            if (validValue.size() >= limit) {
                recordDropped(target.size());
                const std::size_t removed = utf8PrefixRemovalLength(validValue, validValue.size() - limit);
                recordDropped(removed);
                target.assign(validValue.substr(removed));
                return;
            }
            if (target.size() > limit - validValue.size()) {
                const std::size_t remove =
                    utf8PrefixRemovalLength(target, target.size() - (limit - validValue.size()));
                recordDropped(remove);
                target.erase(0, remove);
            }
            target.append(validValue.data(), validValue.size());
        }

        std::size_t processOutputBytes(const ProcessState& process) noexcept {
            std::size_t bytes = process.stdoutData.size();
            saturatingAddSize(bytes, process.stderrData.size());
            return bytes;
        }

        bool claimResourceReservation(BackendState& state, ProviderResourceKind kind, const std::string& resourceId);
        std::size_t unclaimedResourceReservations(BackendState& state, ProviderResourceKind kind) noexcept;
        void forgetResourceReservationClaim(BackendState& state, ProviderResourceKind kind, const std::string& resourceId) noexcept;

        void eraseProcess(BackendState& state, const std::string& handle, bool evicted) {
            const auto iterator = state.processes.find(handle);
            if (iterator == state.processes.end()) {
                return;
            }
            guardedSubtractSize(state.capacity.accumulatedProcessOutputBytes, processOutputBytes(iterator->second));
            guardedSubtractSize(state.capacity.retainedProcesses, 1);
            forgetResourceReservationClaim(state, ProviderResourceKind::Process, handle);
            state.processes.erase(iterator);
            std::erase(state.processOrder, handle);
            if (evicted) {
                saturatingAdd(state.capacity.evictedProcesses);
            }
        }

        ProcessState* admitProcess(BackendState& state, const std::string& handle) {
            if (handle.size() > MaxCanonicalIdentifierBytes) {
                return nullptr;
            }
            if (const auto existing = state.processes.find(handle); existing != state.processes.end()) {
                return &existing->second;
            }
            claimResourceReservation(state, ProviderResourceKind::Process, handle);
            const std::size_t limit = state.capacity.limits.maxRetainedProcesses;
            std::size_t occupied = state.capacity.retainedProcesses;
            saturatingAddSize(occupied, unclaimedResourceReservations(state, ProviderResourceKind::Process));
            if (occupied >= limit) {
                const auto candidate = std::find_if(state.processOrder.begin(), state.processOrder.end(), [&state](const std::string& id) {
                    const auto process = state.processes.find(id);
                    return process != state.processes.end() && process->second.lifecycle == "exited" &&
                           !state.processReservationClaims.contains(id);
                });
                if (candidate == state.processOrder.end()) {
                    return nullptr;
                }
                eraseProcess(state, *candidate, true);
            }
            occupied = state.capacity.retainedProcesses;
            saturatingAddSize(occupied, unclaimedResourceReservations(state, ProviderResourceKind::Process));
            if (occupied >= limit) {
                return nullptr;
            }
            ProcessState process;
            process.processHandle = handle;
            process.stamp = currentStamp(state);
            const auto [iterator, inserted] = state.processes.emplace(handle, std::move(process));
            if (inserted) {
                state.processOrder.push_back(handle);
                saturatingAddSize(state.capacity.retainedProcesses, 1);
            }
            return &iterator->second;
        }

        void trimProcessOutput(ProcessState& process, std::string& output, bool stderrOutput, std::size_t amount) {
            const std::size_t removed = std::min(output.size(), amount);
            if (removed == 0) {
                return;
            }
            output.erase(0, removed);
            saturatingAdd(process.droppedOutputBytes, saturatingUint64(removed));
            if (stderrOutput) {
                process.stderrTruncated = true;
            } else {
                process.stdoutTruncated = true;
            }
        }

        void enforcePerProcessOutputLimit(ProcessState& process, bool updatedStderr, std::size_t limit) {
            const std::size_t total = processOutputBytes(process);
            if (total <= limit) {
                return;
            }
            std::size_t excess = total - limit;
            std::string& olderStream = updatedStderr ? process.stdoutData : process.stderrData;
            const bool olderIsStderr = !updatedStderr;
            const std::size_t olderRemoval = std::min(olderStream.size(), excess);
            trimProcessOutput(process, olderStream, olderIsStderr, olderRemoval);
            excess -= olderRemoval;
            std::string& updatedStream = updatedStderr ? process.stderrData : process.stdoutData;
            trimProcessOutput(process, updatedStream, updatedStderr, excess);
        }

        void updateProcessOutputAccounting(BackendState& state, ProcessState& process, std::size_t before, std::uint64_t droppedBefore) {
            const std::size_t after = processOutputBytes(process);
            if (after >= before) {
                saturatingAddSize(state.capacity.accumulatedProcessOutputBytes, after - before);
            } else {
                guardedSubtractSize(state.capacity.accumulatedProcessOutputBytes, before - after);
            }
            if (process.droppedOutputBytes > droppedBefore) {
                saturatingAdd(state.capacity.droppedProcessOutputBytes, process.droppedOutputBytes - droppedBefore);
            }
        }

        void
        replaceProcessOutput(BackendState& state, ProcessState& process, std::string& target, const std::string& value, bool stderrOutput) {
            const std::size_t before = processOutputBytes(process);
            const std::uint64_t droppedBefore = process.droppedOutputBytes;
            assignBounded(target, value, state.capacity.limits.maxProcessOutputBytesPerProcess, process.droppedOutputBytes);
            if (process.droppedOutputBytes != droppedBefore) {
                if (stderrOutput) {
                    process.stderrTruncated = true;
                } else {
                    process.stdoutTruncated = true;
                }
            }
            enforcePerProcessOutputLimit(process, stderrOutput, state.capacity.limits.maxProcessOutputBytesPerProcess);
            updateProcessOutputAccounting(state, process, before, droppedBefore);
        }

        void
        appendProcessOutput(BackendState& state, ProcessState& process, std::string& target, const std::string& value, bool stderrOutput) {
            const std::size_t before = processOutputBytes(process);
            const std::uint64_t droppedBefore = process.droppedOutputBytes;
            appendBounded(target, value, state.capacity.limits.maxProcessOutputBytesPerProcess, process.droppedOutputBytes);
            if (process.droppedOutputBytes != droppedBefore) {
                if (stderrOutput) {
                    process.stderrTruncated = true;
                } else {
                    process.stdoutTruncated = true;
                }
            }
            enforcePerProcessOutputLimit(process, stderrOutput, state.capacity.limits.maxProcessOutputBytesPerProcess);
            updateProcessOutputAccounting(state, process, before, droppedBefore);
        }

        void retainNotice(BackendState& state, NoticeState notice) {
            const std::size_t limit = state.capacity.limits.maxRetainedNotices;
            if (limit == 0) {
                saturatingAdd(state.capacity.evictedNotices);
                return;
            }
            state.notices.push_back(std::move(notice));
            saturatingAddSize(state.capacity.retainedNotices, 1);
            if (state.notices.size() > limit) {
                state.notices.erase(state.notices.begin());
                guardedSubtractSize(state.capacity.retainedNotices, 1);
                saturatingAdd(state.capacity.evictedNotices);
            }
        }

        void boundNotice(NoticeState& notice, std::size_t summaryLimit, std::size_t detailsLimit) {
            if (notice.summary.size() > summaryLimit) {
                notice.summary.resize(summaryLimit);
            }
            if (notice.details && notice.details->size() > detailsLimit) {
                notice.details->resize(detailsLimit);
            }
        }

        void retainActivityRecord(BackendState& state,
                                  std::string subjectId,
                                  std::string kind,
                                  std::string lifecycle,
                                  bool active,
                                  const std::string& method,
                                  std::size_t eventAlternative,
                                  const ReducerOptions& options,
                                  std::optional<std::string> summary = std::nullopt,
                                  std::optional<std::string> details = std::nullopt,
                                  std::optional<typed::ThreadId> threadId = std::nullopt,
                                  std::optional<typed::TurnId> turnId = std::nullopt) {
            const std::string key = boundedIdentity(kind, subjectId);
            subjectId = boundedText(std::move(subjectId), MaxCanonicalIdentifierBytes);
            if (summary) {
                *summary = boundedText(std::move(*summary), options.maxNoticeSummaryBytes);
            }
            if (details) {
                *details = boundedText(std::move(*details), options.maxNoticeDetailsBytes);
            }
            const auto existing = state.activities.find(key);
            if (existing != state.activities.end()) {
                existing->second.lifecycle = std::move(lifecycle);
                existing->second.active = active;
                existing->second.summary = std::move(summary);
                existing->second.details = std::move(details);
                existing->second.threadId = std::move(threadId);
                existing->second.turnId = std::move(turnId);
                existing->second.notification = {method, eventAlternative, currentStamp(state)};
                return;
            }
            const std::size_t limit = state.capacity.limits.maxRetainedActivityRecords;
            if (state.capacity.retainedActivityRecords >= limit) {
                const auto candidate =
                    std::find_if(state.activityOrder.begin(), state.activityOrder.end(), [&state](const std::string& id) {
                        const auto activity = state.activities.find(id);
                        return activity != state.activities.end() && !activity->second.active;
                    });
                if (candidate == state.activityOrder.end()) {
                    saturatingAdd(state.capacity.evictedActivityRecords);
                    return;
                }
                state.activities.erase(*candidate);
                state.activityOrder.erase(candidate);
                guardedSubtractSize(state.capacity.retainedActivityRecords, 1);
                saturatingAdd(state.capacity.evictedActivityRecords);
            }
            state.activityOrder.push_back(key);
            ActivityRecordState activity;
            activity.key = key;
            activity.subjectId = std::move(subjectId);
            activity.kind = std::move(kind);
            activity.lifecycle = std::move(lifecycle);
            activity.summary = std::move(summary);
            activity.details = std::move(details);
            activity.threadId = std::move(threadId);
            activity.turnId = std::move(turnId);
            activity.notification = {method, eventAlternative, currentStamp(state)};
            activity.active = active;
            state.activities.emplace(key, std::move(activity));
            saturatingAddSize(state.capacity.retainedActivityRecords, 1);
        }

        void retainActivity(BackendState& state,
                            std::string subjectId,
                            std::string kind,
                            std::string lifecycle,
                            bool active,
                            const std::string& method,
                            const typed::Event& event,
                            const ReducerOptions& options,
                            std::optional<std::string> summary = std::nullopt,
                            std::optional<std::string> details = std::nullopt,
                            std::optional<typed::ThreadId> threadId = std::nullopt,
                            std::optional<typed::TurnId> turnId = std::nullopt) {
            retainActivityRecord(state,
                                 std::move(subjectId),
                                 std::move(kind),
                                 std::move(lifecycle),
                                 active,
                                 method,
                                 event.index(),
                                 options,
                                 std::move(summary),
                                 std::move(details),
                                 std::move(threadId),
                                 std::move(turnId));
        }

        FilesystemWatchState* admitFilesystemWatch(BackendState& state, const typed::FsWatchId& watchId) {
            if (watchId.value.size() > MaxCanonicalIdentifierBytes) {
                return nullptr;
            }
            if (const auto existing = state.filesystemWatches.find(watchId.value); existing != state.filesystemWatches.end()) {
                return &existing->second;
            }
            claimResourceReservation(state, ProviderResourceKind::FilesystemWatch, watchId.value);
            std::size_t occupied = state.capacity.retainedFilesystemWatches;
            saturatingAddSize(occupied, unclaimedResourceReservations(state, ProviderResourceKind::FilesystemWatch));
            if (occupied >= state.capacity.limits.maxRetainedFilesystemWatches) {
                return nullptr;
            }
            FilesystemWatchState watch;
            watch.watchId = watchId;
            watch.stamp = currentStamp(state);
            const auto [iterator, inserted] = state.filesystemWatches.emplace(watchId.value, std::move(watch));
            if (inserted) {
                state.filesystemWatchOrder.push_back(watchId.value);
                saturatingAddSize(state.capacity.retainedFilesystemWatches, 1);
            }
            return &iterator->second;
        }

        FuzzySearchState* admitFuzzySearch(BackendState& state, const std::string& sessionId) {
            if (sessionId.size() > MaxCanonicalIdentifierBytes) {
                return nullptr;
            }
            if (const auto existing = state.fuzzySearchSessions.find(sessionId); existing != state.fuzzySearchSessions.end()) {
                return &existing->second;
            }
            claimResourceReservation(state, ProviderResourceKind::FuzzySearch, sessionId);
            const std::size_t limit = state.capacity.limits.maxRetainedFuzzySearchSessions;
            std::size_t occupied = state.capacity.retainedFuzzySearchSessions;
            saturatingAddSize(occupied, unclaimedResourceReservations(state, ProviderResourceKind::FuzzySearch));
            if (occupied >= limit) {
                const auto candidate =
                    std::find_if(state.fuzzySearchOrder.begin(), state.fuzzySearchOrder.end(), [&state](const std::string& id) {
                        const auto search = state.fuzzySearchSessions.find(id);
                        return search != state.fuzzySearchSessions.end() && search->second.complete &&
                               !state.fuzzySearchReservationClaims.contains(id);
                    });
                if (candidate == state.fuzzySearchOrder.end()) {
                    return nullptr;
                }
                forgetResourceReservationClaim(state, ProviderResourceKind::FuzzySearch, *candidate);
                state.fuzzySearchSessions.erase(*candidate);
                state.fuzzySearchOrder.erase(candidate);
                guardedSubtractSize(state.capacity.retainedFuzzySearchSessions, 1);
                saturatingAdd(state.capacity.evictedFuzzySearchSessions);
            }
            occupied = state.capacity.retainedFuzzySearchSessions;
            saturatingAddSize(occupied, unclaimedResourceReservations(state, ProviderResourceKind::FuzzySearch));
            if (occupied >= limit) {
                return nullptr;
            }
            FuzzySearchState search;
            search.sessionId = sessionId;
            search.stamp = currentStamp(state);
            const auto [iterator, inserted] = state.fuzzySearchSessions.emplace(sessionId, std::move(search));
            if (inserted) {
                state.fuzzySearchOrder.push_back(sessionId);
                saturatingAddSize(state.capacity.retainedFuzzySearchSessions, 1);
            }
            return &iterator->second;
        }

        std::set<std::string>& resourceReservations(BackendState& state, ProviderResourceKind kind) noexcept {
            switch (kind) {
                case ProviderResourceKind::Process:
                    return state.processReservations;
                case ProviderResourceKind::FilesystemWatch:
                    return state.filesystemWatchReservations;
                case ProviderResourceKind::FuzzySearch:
                    return state.fuzzySearchReservations;
            }
            return state.processReservations;
        }

        std::map<std::string, std::string>& resourceReservationClaims(BackendState& state, ProviderResourceKind kind) noexcept {
            switch (kind) {
                case ProviderResourceKind::Process:
                    return state.processReservationClaims;
                case ProviderResourceKind::FilesystemWatch:
                    return state.filesystemWatchReservationClaims;
                case ProviderResourceKind::FuzzySearch:
                    return state.fuzzySearchReservationClaims;
            }
            return state.processReservationClaims;
        }

        std::map<std::string, std::string>& resourceReservationTargets(BackendState& state, ProviderResourceKind kind) noexcept {
            switch (kind) {
                case ProviderResourceKind::Process:
                    return state.processReservationTargets;
                case ProviderResourceKind::FilesystemWatch:
                    return state.filesystemWatchReservationTargets;
                case ProviderResourceKind::FuzzySearch:
                    return state.fuzzySearchReservationTargets;
            }
            return state.processReservationTargets;
        }

        bool retainedResourceExists(const BackendState& state, ProviderResourceKind kind, const std::string& resourceId) noexcept {
            switch (kind) {
                case ProviderResourceKind::Process:
                    return state.processes.contains(resourceId);
                case ProviderResourceKind::FilesystemWatch:
                    return state.filesystemWatches.contains(resourceId);
                case ProviderResourceKind::FuzzySearch:
                    return state.fuzzySearchSessions.contains(resourceId);
            }
            return false;
        }

        std::size_t& resourceRetainedCount(CapacityState& capacity, ProviderResourceKind kind) noexcept {
            switch (kind) {
                case ProviderResourceKind::Process:
                    return capacity.retainedProcesses;
                case ProviderResourceKind::FilesystemWatch:
                    return capacity.retainedFilesystemWatches;
                case ProviderResourceKind::FuzzySearch:
                    return capacity.retainedFuzzySearchSessions;
            }
            return capacity.retainedProcesses;
        }

        std::size_t resourceLimit(const BackendCapacityOptions& limits, ProviderResourceKind kind) noexcept {
            switch (kind) {
                case ProviderResourceKind::Process:
                    return limits.maxRetainedProcesses;
                case ProviderResourceKind::FilesystemWatch:
                    return limits.maxRetainedFilesystemWatches;
                case ProviderResourceKind::FuzzySearch:
                    return limits.maxRetainedFuzzySearchSessions;
            }
            return 0;
        }

        std::size_t unclaimedResourceReservations(BackendState& state, ProviderResourceKind kind) noexcept {
            const std::set<std::string>& reservations = resourceReservations(state, kind);
            const std::map<std::string, std::string>& targets = resourceReservationTargets(state, kind);
            const std::map<std::string, std::string>& claims = resourceReservationClaims(state, kind);
            std::size_t unclaimed = 0;
            for (const std::string& reservation : reservations) {
                const auto target = targets.find(reservation);
                if (target == targets.end() || retainedResourceExists(state, kind, target->second)) {
                    continue;
                }
                const bool isClaimed = std::any_of(claims.begin(), claims.end(), [&reservation](const auto& claim) {
                    return claim.second == reservation;
                });
                if (!isClaimed) {
                    saturatingAddSize(unclaimed, 1);
                }
            }
            return unclaimed;
        }

        bool claimResourceReservation(BackendState& state, ProviderResourceKind kind, const std::string& resourceId) {
            std::set<std::string>& reservations = resourceReservations(state, kind);
            std::map<std::string, std::string>& targets = resourceReservationTargets(state, kind);
            std::map<std::string, std::string>& claims = resourceReservationClaims(state, kind);
            if (claims.contains(resourceId)) {
                return true;
            }
            const auto unclaimed =
                std::find_if(reservations.begin(), reservations.end(), [&claims, &targets, &resourceId](const std::string& reservation) {
                    const auto target = targets.find(reservation);
                    return target != targets.end() && target->second == resourceId &&
                           std::none_of(claims.begin(), claims.end(), [&reservation](const auto& claim) {
                               return claim.second == reservation;
                           });
                });
            if (unclaimed == reservations.end()) {
                return false;
            }
            claims.emplace(resourceId, *unclaimed);
            return true;
        }

        void forgetResourceReservationClaim(BackendState& state, ProviderResourceKind kind, const std::string& resourceId) noexcept {
            std::map<std::string, std::string>& claims = resourceReservationClaims(state, kind);
            const auto claim = claims.find(resourceId);
            if (claim == claims.end()) {
                return;
            }
            resourceReservations(state, kind).erase(claim->second);
            resourceReservationTargets(state, kind).erase(claim->second);
            claims.erase(claim);
        }

        bool reserveResource(BackendState& state, ProviderResourceKind kind, const std::string& key, const std::string& resourceId) {
            if (key.size() > MaxCanonicalIdentifierBytes || resourceId.size() > MaxCanonicalIdentifierBytes) {
                return false;
            }
            std::set<std::string>& reservations = resourceReservations(state, kind);
            std::map<std::string, std::string>& targets = resourceReservationTargets(state, kind);
            if (reservations.contains(key)) {
                const auto target = targets.find(key);
                return target != targets.end() && target->second == resourceId;
            }
            if (retainedResourceExists(state, kind, resourceId)) {
                reservations.insert(key);
                targets.emplace(key, resourceId);
                return true;
            }
            if (!retainedResourceExists(state, kind, resourceId) &&
                std::any_of(targets.begin(), targets.end(), [&resourceId](const auto& target) {
                    return target.second == resourceId;
                })) {
                return false;
            }
            std::size_t& retained = resourceRetainedCount(state.capacity, kind);
            const std::size_t limit = resourceLimit(state.capacity.limits, kind);
            std::size_t occupied = retained;
            saturatingAddSize(occupied, unclaimedResourceReservations(state, kind));
            if (occupied >= limit) {
                if (kind == ProviderResourceKind::Process) {
                    const auto candidate =
                        std::find_if(state.processOrder.begin(), state.processOrder.end(), [&state](const std::string& id) {
                            const auto process = state.processes.find(id);
                            return process != state.processes.end() && process->second.lifecycle == "exited" &&
                                   !state.processReservationClaims.contains(id);
                        });
                    if (candidate != state.processOrder.end()) {
                        eraseProcess(state, *candidate, true);
                    }
                } else if (kind == ProviderResourceKind::FuzzySearch) {
                    const auto candidate =
                        std::find_if(state.fuzzySearchOrder.begin(), state.fuzzySearchOrder.end(), [&state](const std::string& id) {
                            const auto search = state.fuzzySearchSessions.find(id);
                            return search != state.fuzzySearchSessions.end() && search->second.complete &&
                                   !state.fuzzySearchReservationClaims.contains(id);
                        });
                    if (candidate != state.fuzzySearchOrder.end()) {
                        forgetResourceReservationClaim(state, ProviderResourceKind::FuzzySearch, *candidate);
                        state.fuzzySearchSessions.erase(*candidate);
                        state.fuzzySearchOrder.erase(candidate);
                        guardedSubtractSize(retained, 1);
                        saturatingAdd(state.capacity.evictedFuzzySearchSessions);
                    }
                }
            }
            occupied = retained;
            saturatingAddSize(occupied, unclaimedResourceReservations(state, kind));
            if (occupied >= limit) {
                return false;
            }
            reservations.insert(key);
            targets.emplace(key, resourceId);
            return true;
        }

        bool releaseResource(BackendState& state, ProviderResourceKind kind, const std::string& key) noexcept {
            std::set<std::string>& reservations = resourceReservations(state, kind);
            if (reservations.erase(key) == 0) {
                return false;
            }
            resourceReservationTargets(state, kind).erase(key);
            std::map<std::string, std::string>& claims = resourceReservationClaims(state, kind);
            std::erase_if(claims, [&key](const auto& claim) {
                return claim.second == key;
            });
            return true;
        }

        std::optional<std::vector<typed::FileUpdateChange>> decodeFileUpdateChanges(const Json& changes) {
            if (!changes.is_array()) {
                return std::nullopt;
            }

            std::vector<typed::FileUpdateChange> decoded;
            decoded.reserve(changes.size());
            for (const Json& change : changes) {
                if (!change.is_object()) {
                    return std::nullopt;
                }
                const auto diff = change.find("diff");
                const auto kind = change.find("kind");
                const auto path = change.find("path");
                if (diff == change.end() || !diff->is_string() || kind == change.end() || path == change.end() || !path->is_string()) {
                    return std::nullopt;
                }
                ai::openai::codex::detail::ConversationDecodeResult<typed::PatchChangeKind> decodedKind =
                    ai::openai::codex::detail::decodePatchChangeKind(*kind);
                if (!decodedKind.value) {
                    return std::nullopt;
                }
                decoded.push_back({diff->get<std::string>(), std::move(*decodedKind.value), path->get<std::string>()});
            }
            return decoded;
        }

        void initializeVisibleContent(ItemState& state, bool authoritative, std::size_t limit) {
            const auto resetChannelDrop = [&state](std::uint64_t& channelDropped) {
                state.droppedContentBytes = channelDropped > state.droppedContentBytes
                                                ? 0
                                                : state.droppedContentBytes - channelDropped;
                channelDropped = 0;
            };
            std::visit(Overloaded{[&state, &resetChannelDrop, authoritative, limit](const typed::AgentMessageThreadItem& item) {
                                      if ((!item.text.empty() && authoritative) || state.agentText.empty()) {
                                          if (authoritative) {
                                              resetChannelDrop(state.agentTextDroppedContentBytes);
                                          }
                                          assignBounded(state.agentText,
                                                        item.text,
                                                        limit,
                                                        state.droppedContentBytes,
                                                        &state.agentTextDroppedContentBytes);
                                      }
                                  },
                                  [&state, &resetChannelDrop, authoritative, limit](const typed::ReasoningThreadItem& item) {
                                      const std::vector<std::string>& content = item.contentOrDefault();
                                      const bool hasContent = std::any_of(content.begin(), content.end(), [](const std::string& value) {
                                          return !value.empty();
                                      });
                                      if ((authoritative && hasContent) || state.reasoningText.empty()) {
                                          if (authoritative) {
                                              resetChannelDrop(state.reasoningTextDroppedContentBytes);
                                          }
                                          state.reasoningText.clear();
                                          for (const std::string& value : content) {
                                              appendBounded(state.reasoningText,
                                                            value,
                                                            limit,
                                                            state.droppedContentBytes,
                                                            &state.reasoningTextDroppedContentBytes);
                                          }
                                      }
                                      const std::vector<std::string>& summary = item.summaryOrDefault();
                                      const bool hasSummary = std::any_of(summary.begin(), summary.end(), [](const std::string& value) {
                                          return !value.empty();
                                      });
                                      if ((authoritative && hasSummary) || state.reasoningSummary.empty()) {
                                          if (authoritative) {
                                              resetChannelDrop(state.reasoningSummaryDroppedContentBytes);
                                          }
                                          state.reasoningSummary.clear();
                                          for (const std::string& value : summary) {
                                              appendBounded(state.reasoningSummary,
                                                            value,
                                                            limit,
                                                            state.droppedContentBytes,
                                                            &state.reasoningSummaryDroppedContentBytes);
                                          }
                                      }
                                  },
                                  [&state, &resetChannelDrop, authoritative, limit](const typed::CommandExecutionThreadItem& item) {
                                      if (item.aggregatedOutput &&
                                          ((!item.aggregatedOutput->empty() && authoritative) || state.commandOutput.empty())) {
                                          if (authoritative) {
                                              resetChannelDrop(state.commandOutputDroppedContentBytes);
                                          }
                                          assignBounded(state.commandOutput,
                                                        *item.aggregatedOutput,
                                                        limit,
                                                        state.droppedContentBytes,
                                                        &state.commandOutputDroppedContentBytes);
                                      }
                                  },
                                  [](const auto&) {
                                  }},
                       state.item);
        }

        std::size_t itemContentBytes(const ItemState& item) noexcept {
            std::size_t bytes = 0;
            saturatingAddSize(bytes, item.agentText.size());
            saturatingAddSize(bytes, item.reasoningText.size());
            saturatingAddSize(bytes, item.reasoningSummary.size());
            saturatingAddSize(bytes, item.commandOutput.size());
            return bytes;
        }

        std::size_t turnPlanContentBytes(const std::optional<TurnPlanState>& plan) noexcept {
            if (!plan) {
                return 0;
            }
            std::size_t bytes = plan->explanation ? plan->explanation->size() : 0;
            for (const TurnPlanStepState& step : plan->steps) {
                saturatingAddSize(bytes, step.step.size());
                saturatingAddSize(bytes, step.status.value.size());
            }
            return bytes;
        }

        std::optional<std::pair<typed::ThreadId, typed::TurnId>> itemLocation(const typed::ThreadItem& item) {
            return std::visit(
                [](const auto& value) -> std::optional<std::pair<typed::ThreadId, typed::TurnId>> {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, typed::UnknownItem>) {
                        if (value.metadata.threadId && !value.metadata.threadId->value.empty() && value.metadata.turnId &&
                            !value.metadata.turnId->value.empty()) {
                            return std::pair{*value.metadata.threadId, *value.metadata.turnId};
                        }
                    } else {
                        if (value.metadata.threadId && !value.metadata.threadId->value.empty() && value.metadata.turnId &&
                            !value.metadata.turnId->value.empty()) {
                            return std::pair{*value.metadata.threadId, *value.metadata.turnId};
                        }
                    }
                    return std::nullopt;
                },
                item);
        }

        bool upsertItem(BackendState& state,
                        const typed::ThreadId& threadId,
                        const typed::TurnId& turnId,
                        const typed::ThreadItem& item,
                        ItemLifecycle lifecycle,
                        std::optional<std::int64_t> occurredAtMs,
                        std::size_t contentLimit,
                        RetentionInsertions* insertions = nullptr) {
            const std::optional<typed::ItemId> id = itemId(item);
            if (!id) {
                return false;
            }

            TurnState& turn = ensureTurn(state, threadId, turnId, insertions);
            auto iterator = turn.items.find(id->value);
            if (iterator == turn.items.end()) {
                ItemState itemState;
                itemState.item = item;
                itemState.lifecycle = lifecycle;
                itemState.stamp = currentStamp(state);
                itemState.connectionInvalidated = false;
                if (lifecycle == ItemLifecycle::Started) {
                    itemState.startedAtMs = occurredAtMs;
                } else if (lifecycle == ItemLifecycle::Completed) {
                    itemState.completedAtMs = occurredAtMs;
                }
                initializeVisibleContent(itemState, true, contentLimit);
                const std::size_t contentBytes = itemContentBytes(itemState);
                turn.items.emplace(id->value, std::move(itemState));
                turn.itemOrder.push_back(*id);
                saturatingAddSize(state.capacity.retainedItems, 1);
                saturatingAddSize(state.capacity.accumulatedContentBytes, contentBytes);
                if (insertions != nullptr) {
                    insertions->items.emplace_back(threadId.value, turnId.value, id->value);
                }
            } else {
                ItemState& itemState = iterator->second;
                const std::size_t previousContentBytes = itemContentBytes(itemState);
                itemState.item = item;
                itemState.stamp = currentStamp(state);
                itemState.connectionInvalidated = false;
                if (lifecycle != ItemLifecycle::Unknown || itemState.lifecycle == ItemLifecycle::Unknown) {
                    itemState.lifecycle = lifecycle;
                }
                if (lifecycle == ItemLifecycle::Started && occurredAtMs) {
                    itemState.startedAtMs = occurredAtMs;
                } else if (lifecycle == ItemLifecycle::Completed && occurredAtMs) {
                    itemState.completedAtMs = occurredAtMs;
                }
                initializeVisibleContent(itemState, lifecycle == ItemLifecycle::Completed, contentLimit);
                replaceAccumulatedContent(state.capacity, previousContentBytes, itemContentBytes(itemState));
            }
            return true;
        }

        void eraseItem(BackendState& state, TurnState& turn, const std::string& id);
        void eraseTurn(BackendState& state, ThreadState& thread, const std::string& id);

        bool
        upsertTurn(BackendState& state, const typed::Turn& value, std::size_t contentLimit, RetentionInsertions* insertions = nullptr) {
            typed::Turn normalized = value;
            normalized.items.clear();
            if (normalized.raw.is_object()) {
                normalized.raw.erase("items");
            }
            ThreadState& thread = ensureThread(state, value.threadId, insertions);
            auto iterator = thread.turns.find(value.id.value);
            if (iterator == thread.turns.end()) {
                TurnState turn;
                turn.turn = std::move(normalized);
                turn.stamp = currentStamp(state);
                turn.connectionInvalidated = false;
                turn.active = !isTerminal(value.status);
                turn.terminal = isTerminal(value.status);
                iterator = thread.turns.emplace(value.id.value, std::move(turn)).first;
                thread.turnOrder.push_back(value.id);
                saturatingAddSize(state.capacity.retainedTurns, 1);
                if (insertions != nullptr) {
                    insertions->turns.emplace_back(value.threadId.value, value.id.value);
                }
            } else {
                iterator->second.turn = std::move(normalized);
                iterator->second.stamp = currentStamp(state);
                iterator->second.connectionInvalidated = false;
                iterator->second.active = !isTerminal(value.status);
                iterator->second.terminal = isTerminal(value.status);
                if (value.error.hasValue()) {
                    iterator->second.failure = value.error->raw;
                }
            }

            for (const typed::ThreadItem& item : value.items) {
                upsertItem(state,
                           value.threadId,
                           value.id,
                           item,
                           iterator->second.terminal ? ItemLifecycle::Completed : ItemLifecycle::Unknown,
                           std::nullopt,
                           contentLimit,
                           insertions);
            }
            return true;
        }

        bool upsertThread(BackendState& state,
                          const typed::Thread& value,
                          EntityLoad load,
                          std::size_t contentLimit,
                          RetentionInsertions* insertions = nullptr) {
            typed::Thread normalized = value;
            normalized.turns.clear();
            if (normalized.raw.is_object()) {
                normalized.raw.erase("turns");
            }
            auto iterator = state.threads.find(value.id.value);
            if (iterator == state.threads.end()) {
                ThreadState thread;
                thread.thread = std::move(normalized);
                thread.fullyLoaded = load == EntityLoad::Full;
                thread.stamp = currentStamp(state);
                iterator = state.threads.emplace(value.id.value, std::move(thread)).first;
                state.threadOrder.push_back(value.id);
                saturatingAddSize(state.capacity.retainedThreads, 1);
                if (insertions != nullptr) {
                    insertions->threads.push_back(value.id.value);
                }
            } else {
                const bool currentGeneration = iterator->second.stamp.generation == state.provider.generation &&
                                               iterator->second.stamp.freshness == Freshness::Current;
                iterator->second.thread = std::move(normalized);
                iterator->second.fullyLoaded =
                    currentGeneration ? iterator->second.fullyLoaded || load == EntityLoad::Full : load == EntityLoad::Full;
                iterator->second.stamp = currentStamp(state);
            }

            if (load == EntityLoad::Full) {
                for (const typed::Turn& turn : value.turns) {
                    upsertTurn(state, turn, contentLimit, insertions);
                }
                ThreadState& retainedThread = state.threads.at(value.id.value);
                std::set<std::string> authoritativeTurnIds;
                std::vector<typed::TurnId> authoritativeTurnOrder;
                authoritativeTurnOrder.reserve(value.turns.size());
                for (const typed::Turn& sourceTurn : value.turns) {
                    if (authoritativeTurnIds.insert(sourceTurn.id.value).second) {
                        authoritativeTurnOrder.push_back(sourceTurn.id);
                    }
                    const auto retainedTurn = retainedThread.turns.find(sourceTurn.id.value);
                    if (retainedTurn == retainedThread.turns.end()) {
                        continue;
                    }
                    std::set<std::string> authoritativeItemIds;
                    std::vector<typed::ItemId> authoritativeItemOrder;
                    authoritativeItemOrder.reserve(sourceTurn.items.size());
                    for (const typed::ThreadItem& sourceItem : sourceTurn.items) {
                        if (const std::optional<typed::ItemId> id = itemId(sourceItem)) {
                            if (authoritativeItemIds.insert(id->value).second && retainedTurn->second.items.contains(id->value)) {
                                authoritativeItemOrder.push_back(*id);
                            }
                        }
                    }
                    std::vector<std::string> removedItems;
                    removedItems.reserve(retainedTurn->second.items.size());
                    for (const auto& [id, item] : retainedTurn->second.items) {
                        (void) item;
                        if (!authoritativeItemIds.contains(id)) {
                            removedItems.push_back(id);
                        }
                    }
                    for (const std::string& id : removedItems) {
                        eraseItem(state, retainedTurn->second, id);
                    }
                    retainedTurn->second.itemOrder = std::move(authoritativeItemOrder);
                }
                std::vector<std::string> removedTurns;
                removedTurns.reserve(retainedThread.turns.size());
                for (const auto& [id, turn] : retainedThread.turns) {
                    (void) turn;
                    if (!authoritativeTurnIds.contains(id)) {
                        removedTurns.push_back(id);
                    }
                }
                for (const std::string& id : removedTurns) {
                    eraseTurn(state, retainedThread, id);
                }
                retainedThread.turnOrder = std::move(authoritativeTurnOrder);
            }
            return true;
        }

        struct PendingReferences {
            std::set<std::string> threads;
            std::set<TurnKey> turns;
            std::set<ItemKey> items;
        };

        PendingReferences pendingReferences(const BackendState& state) {
            PendingReferences references;
            for (const auto& [id, pending] : state.pendingRequests) {
                (void) id;
                std::visit(
                    [&references](const auto& request) {
                        using Request = std::remove_cvref_t<decltype(request)>;
                        if constexpr (std::is_same_v<Request, typed::ApplyPatchApprovalRequest> ||
                                      std::is_same_v<Request, typed::ExecCommandApprovalRequest>) {
                            references.threads.insert(request.params.conversationId.value);
                        } else if constexpr (std::is_same_v<Request, typed::PermissionsApprovalRequest>) {
                            references.threads.insert(request.params.threadId.value);
                            references.turns.emplace(request.params.threadId.value, request.params.turnId.value);
                            references.items.emplace(
                                request.params.threadId.value, request.params.turnId.value, request.params.itemId.value);
                        } else if constexpr (std::is_same_v<Request, typed::DynamicToolCallRequest>) {
                            references.threads.insert(request.params.threadId.value);
                            references.turns.emplace(request.params.threadId.value, request.params.turnId.value);
                        } else if constexpr (std::is_same_v<Request, typed::McpServerElicitationRequest>) {
                            references.threads.insert(request.params.threadId.value);
                            if (request.params.turnId.hasValue()) {
                                references.turns.emplace(request.params.threadId.value, request.params.turnId.value->value);
                            }
                        } else if constexpr (requires { request.threadId.value; }) {
                            references.threads.insert(request.threadId.value);
                            if constexpr (requires { request.turnId.value; }) {
                                references.turns.emplace(request.threadId.value, request.turnId.value);
                                if constexpr (requires { request.itemId.value; }) {
                                    references.items.emplace(request.threadId.value, request.turnId.value, request.itemId.value);
                                }
                            }
                        }
                    },
                    pending.request);
            }
            return references;
        }

        bool turnHasActiveItem(const TurnState& turn) noexcept {
            return std::any_of(turn.items.begin(), turn.items.end(), [](const auto& entry) {
                return entry.second.lifecycle == ItemLifecycle::Started || entry.second.lifecycle == ItemLifecycle::Unknown;
            });
        }

        bool threadHasActiveTurn(const ThreadState& thread) noexcept {
            return std::any_of(thread.turns.begin(), thread.turns.end(), [](const auto& entry) {
                return entry.second.active || !entry.second.terminal || turnHasActiveItem(entry.second);
            });
        }

        bool threadIsReferenced(const ThreadState& thread, const std::string& threadId, const PendingReferences& referenced) {
            if (referenced.threads.contains(threadId)) {
                return true;
            }
            for (const auto& [turnId, turn] : thread.turns) {
                if (referenced.turns.contains(TurnKey{threadId, turnId})) {
                    return true;
                }
                for (const auto& [itemIdValue, item] : turn.items) {
                    (void) item;
                    if (referenced.items.contains(ItemKey{threadId, turnId, itemIdValue})) {
                        return true;
                    }
                }
            }
            return false;
        }

        bool turnIsReferenced(const TurnState& turn,
                              const std::string& threadId,
                              const std::string& turnId,
                              const PendingReferences& referenced) {
            if (referenced.turns.contains(TurnKey{threadId, turnId})) {
                return true;
            }
            return std::any_of(turn.items.begin(), turn.items.end(), [&](const auto& entry) {
                return referenced.items.contains(ItemKey{threadId, turnId, entry.first});
            });
        }

        void accountTurnRemoval(CapacityState& capacity, const TurnState& turn, bool eviction) noexcept {
            if (eviction) {
                saturatingAdd(capacity.evictedTurns);
                saturatingAdd(capacity.evictedItems, saturatingUint64(turn.items.size()));
            } else {
                saturatingAdd(capacity.snapshotOmissions);
            }
        }

        void accountThreadRemoval(CapacityState& capacity, const ThreadState& thread, bool eviction) noexcept {
            if (!eviction) {
                saturatingAdd(capacity.snapshotOmissions);
                return;
            }
            saturatingAdd(capacity.evictedThreads);
            saturatingAdd(capacity.evictedTurns, saturatingUint64(thread.turns.size()));
            std::size_t itemCount = 0;
            for (const auto& [turnId, turn] : thread.turns) {
                (void) turnId;
                saturatingAddSize(itemCount, turn.items.size());
            }
            saturatingAdd(capacity.evictedItems, saturatingUint64(itemCount));
        }

        void eraseItem(BackendState& state, TurnState& turn, const std::string& id) {
            const auto item = turn.items.find(id);
            if (item == turn.items.end()) {
                return;
            }
            guardedSubtractSize(state.capacity.retainedItems, 1);
            guardedSubtractSize(state.capacity.accumulatedContentBytes, itemContentBytes(item->second));
            turn.items.erase(item);
            std::erase_if(turn.itemOrder, [&id](const typed::ItemId& value) {
                return value.value == id;
            });
        }

        void eraseTurn(BackendState& state, ThreadState& thread, const std::string& id) {
            const auto turn = thread.turns.find(id);
            if (turn == thread.turns.end()) {
                return;
            }
            guardedSubtractSize(state.capacity.retainedTurns, 1);
            guardedSubtractSize(state.capacity.retainedItems, turn->second.items.size());
            guardedSubtractSize(state.capacity.accumulatedContentBytes, turnPlanContentBytes(turn->second.plan));
            for (const auto& [itemIdValue, item] : turn->second.items) {
                (void) itemIdValue;
                guardedSubtractSize(state.capacity.accumulatedContentBytes, itemContentBytes(item));
            }
            thread.turns.erase(turn);
            std::erase_if(thread.turnOrder, [&id](const typed::TurnId& value) {
                return value.value == id;
            });
        }

        void eraseThread(BackendState& state, const std::string& id) {
            const auto thread = state.threads.find(id);
            if (thread == state.threads.end()) {
                return;
            }
            guardedSubtractSize(state.capacity.retainedThreads, 1);
            guardedSubtractSize(state.capacity.retainedTurns, thread->second.turns.size());
            for (const auto& [turnId, turn] : thread->second.turns) {
                (void) turnId;
                guardedSubtractSize(state.capacity.retainedItems, turn.items.size());
                guardedSubtractSize(state.capacity.accumulatedContentBytes, turnPlanContentBytes(turn.plan));
                for (const auto& [itemIdValue, item] : turn.items) {
                    (void) itemIdValue;
                    guardedSubtractSize(state.capacity.accumulatedContentBytes, itemContentBytes(item));
                }
            }
            state.threads.erase(thread);
            std::erase_if(state.threadOrder, [&id](const typed::ThreadId& value) {
                return value.value == id;
            });
        }

        bool enforceRetentionCapacity(BackendState& state, const RetentionInsertions& insertions) {
            const BackendCapacityOptions& limits = state.capacity.limits;
            if (state.capacity.retainedThreads <= limits.maxRetainedThreads && state.capacity.retainedTurns <= limits.maxRetainedTurns &&
                state.capacity.retainedItems <= limits.maxRetainedItems &&
                state.capacity.accumulatedContentBytes <= limits.maxAccumulatedContentBytes &&
                state.capacity.retainedNotices <= limits.maxRetainedNotices &&
                state.capacity.retainedProcesses <= limits.maxRetainedProcesses &&
                state.capacity.accumulatedProcessOutputBytes <= limits.maxAccumulatedProcessOutputBytes &&
                state.capacity.retainedFilesystemWatches <= limits.maxRetainedFilesystemWatches &&
                state.capacity.retainedFuzzySearchSessions <= limits.maxRetainedFuzzySearchSessions &&
                state.capacity.retainedActivityRecords <= limits.maxRetainedActivityRecords) {
                return false;
            }
            bool canonicalStateRewritten = false;
            detail::recordRetentionCapacitySlowPath();
            const bool structuralCapacityExceeded = state.capacity.retainedThreads > limits.maxRetainedThreads ||
                                                    state.capacity.retainedTurns > limits.maxRetainedTurns ||
                                                    state.capacity.retainedItems > limits.maxRetainedItems;
            if (structuralCapacityExceeded) {
                detail::recordPendingReferenceBuild();
            }
            const PendingReferences referenced = structuralCapacityExceeded ? pendingReferences(state) : PendingReferences{};
            const std::set<std::string> insertedThreads(insertions.threads.begin(), insertions.threads.end());
            const std::set<TurnKey> insertedTurns(insertions.turns.begin(), insertions.turns.end());
            const std::set<ItemKey> insertedItems(insertions.items.begin(), insertions.items.end());

            while (state.capacity.retainedThreads > limits.maxRetainedThreads) {
                auto candidate = state.threadOrder.end();
                for (auto iterator = state.threadOrder.begin(); iterator != state.threadOrder.end(); ++iterator) {
                    const auto thread = state.threads.find(iterator->value);
                    if (!insertedThreads.contains(iterator->value) && thread != state.threads.end() &&
                        !threadHasActiveTurn(thread->second) && !threadIsReferenced(thread->second, iterator->value, referenced)) {
                        candidate = iterator;
                        break;
                    }
                }
                if (candidate == state.threadOrder.end()) {
                    const auto inserted =
                        std::find_if(insertions.threads.rbegin(), insertions.threads.rend(), [&](const std::string& value) {
                            return state.threads.contains(value);
                        });
                    if (inserted != insertions.threads.rend()) {
                        const std::string id = *inserted;
                        const auto thread = state.threads.find(id);
                        accountThreadRemoval(state.capacity, thread->second, false);
                        eraseThread(state, id);
                        canonicalStateRewritten = true;
                        continue;
                    }
                    saturatingAdd(state.capacity.snapshotOmissions);
                    break;
                }
                const std::string id = candidate->value;
                const auto thread = state.threads.find(id);
                accountThreadRemoval(state.capacity, thread->second, true);
                eraseThread(state, id);
                canonicalStateRewritten = true;
            }

            while (state.capacity.retainedTurns > limits.maxRetainedTurns) {
                bool evicted = false;
                for (const typed::ThreadId& threadId : state.threadOrder) {
                    auto thread = state.threads.find(threadId.value);
                    if (thread == state.threads.end()) {
                        continue;
                    }
                    for (const typed::TurnId& turnId : thread->second.turnOrder) {
                        const auto turn = thread->second.turns.find(turnId.value);
                        if (!insertedTurns.contains(TurnKey{threadId.value, turnId.value}) && turn != thread->second.turns.end() &&
                            turn->second.terminal && !turn->second.active && !turnHasActiveItem(turn->second) &&
                            !turnIsReferenced(turn->second, threadId.value, turnId.value, referenced)) {
                            accountTurnRemoval(state.capacity, turn->second, true);
                            const std::string id = turnId.value;
                            eraseTurn(state, thread->second, id);
                            thread->second.fullyLoaded = false;
                            canonicalStateRewritten = true;
                            evicted = true;
                            break;
                        }
                    }
                    if (evicted) {
                        break;
                    }
                }
                if (!evicted) {
                    for (auto inserted = insertions.turns.begin(); inserted != insertions.turns.end() && !evicted; ++inserted) {
                        const auto& [threadId, turnId] = *inserted;
                        auto thread = state.threads.find(threadId);
                        if (thread == state.threads.end()) {
                            continue;
                        }
                        const auto turn = thread->second.turns.find(turnId);
                        if (turn != thread->second.turns.end()) {
                            accountTurnRemoval(state.capacity, turn->second, false);
                            eraseTurn(state, thread->second, turnId);
                            thread->second.fullyLoaded = false;
                            canonicalStateRewritten = true;
                            evicted = true;
                        }
                    }
                }
                if (!evicted) {
                    saturatingAdd(state.capacity.snapshotOmissions);
                    break;
                }
            }

            while (state.capacity.retainedItems > limits.maxRetainedItems) {
                bool evicted = false;
                for (const typed::ThreadId& threadId : state.threadOrder) {
                    auto thread = state.threads.find(threadId.value);
                    if (thread == state.threads.end()) {
                        continue;
                    }
                    for (const typed::TurnId& turnId : thread->second.turnOrder) {
                        auto turn = thread->second.turns.find(turnId.value);
                        if (turn == thread->second.turns.end()) {
                            continue;
                        }
                        for (const typed::ItemId& itemIdValue : turn->second.itemOrder) {
                            const auto item = turn->second.items.find(itemIdValue.value);
                            if (!insertedItems.contains(ItemKey{threadId.value, turnId.value, itemIdValue.value}) &&
                                item != turn->second.items.end() &&
                                (item->second.lifecycle == ItemLifecycle::Completed || item->second.lifecycle == ItemLifecycle::Failed) &&
                                !item->second.connectionInvalidated &&
                                !referenced.items.contains(ItemKey{threadId.value, turnId.value, itemIdValue.value})) {
                                const std::string id = itemIdValue.value;
                                eraseItem(state, turn->second, id);
                                thread->second.fullyLoaded = false;
                                saturatingAdd(state.capacity.evictedItems);
                                canonicalStateRewritten = true;
                                evicted = true;
                                break;
                            }
                        }
                        if (evicted) {
                            break;
                        }
                    }
                    if (evicted) {
                        break;
                    }
                }
                if (!evicted) {
                    for (auto inserted = insertions.items.begin(); inserted != insertions.items.end() && !evicted; ++inserted) {
                        const auto& [threadId, turnId, itemIdValue] = *inserted;
                        auto thread = state.threads.find(threadId);
                        if (thread == state.threads.end()) {
                            continue;
                        }
                        auto turn = thread->second.turns.find(turnId);
                        if (turn != thread->second.turns.end() && turn->second.items.contains(itemIdValue)) {
                            eraseItem(state, turn->second, itemIdValue);
                            thread->second.fullyLoaded = false;
                            saturatingAdd(state.capacity.snapshotOmissions);
                            canonicalStateRewritten = true;
                            evicted = true;
                        }
                    }
                }
                if (!evicted) {
                    saturatingAdd(state.capacity.snapshotOmissions);
                    break;
                }
            }

            std::size_t excess = state.capacity.accumulatedContentBytes > limits.maxAccumulatedContentBytes
                                     ? state.capacity.accumulatedContentBytes - limits.maxAccumulatedContentBytes
                                     : 0;
            const auto trimPass = [&state, &excess, &canonicalStateRewritten](bool inactiveTerminalOnly) {
                for (const typed::ThreadId& threadId : state.threadOrder) {
                    auto thread = state.threads.find(threadId.value);
                    if (thread == state.threads.end()) {
                        continue;
                    }
                    for (const typed::TurnId& turnId : thread->second.turnOrder) {
                        auto turn = thread->second.turns.find(turnId.value);
                        if (turn == thread->second.turns.end() ||
                            (inactiveTerminalOnly && (turn->second.active || !turn->second.terminal))) {
                            continue;
                        }
                        if (turn->second.plan && excess != 0) {
                            TurnPlanState& plan = *turn->second.plan;
                            const auto accountRemoved =
                                [&state, &excess, &plan, &thread, &canonicalStateRewritten](std::size_t removed) {
                                if (removed == 0) {
                                    return;
                                }
                                excess = removed > excess ? 0 : excess - removed;
                                guardedSubtractSize(state.capacity.accumulatedContentBytes, removed);
                                saturatingAdd(state.capacity.droppedContentBytes, saturatingUint64(removed));
                                plan.truncated = true;
                                thread->second.fullyLoaded = false;
                                canonicalStateRewritten = true;
                            };
                            if (plan.explanation && !plan.explanation->empty()) {
                                const std::size_t targetBytes =
                                    plan.explanation->size() > excess ? plan.explanation->size() - excess : 0;
                                const std::size_t retainedBytes = boundedUtf8PrefixLength(*plan.explanation, targetBytes);
                                const std::size_t removed = plan.explanation->size() - retainedBytes;
                                plan.explanation->resize(retainedBytes);
                                if (plan.explanation->empty()) {
                                    plan.explanation.reset();
                                }
                                accountRemoved(removed);
                            }
                            while (excess != 0 && !plan.steps.empty()) {
                                TurnPlanStepState& step = plan.steps.back();
                                const std::size_t stepBytes = step.step.size() + step.status.value.size();
                                if (stepBytes <= excess || step.step.empty()) {
                                    plan.steps.pop_back();
                                    accountRemoved(stepBytes);
                                    continue;
                                }
                                const std::size_t targetBytes = step.step.size() > excess ? step.step.size() - excess : 0;
                                const std::size_t retainedBytes = boundedUtf8PrefixLength(step.step, targetBytes);
                                const std::size_t removed = step.step.size() - retainedBytes;
                                step.step.resize(retainedBytes);
                                accountRemoved(removed);
                            }
                            if (excess == 0) {
                                return;
                            }
                        }
                        for (const typed::ItemId& itemIdValue : turn->second.itemOrder) {
                            auto item = turn->second.items.find(itemIdValue.value);
                            if (item == turn->second.items.end()) {
                                continue;
                            }
                            auto trim = [&excess, &item, &thread, &state, &canonicalStateRewritten](
                                            std::string& content, std::uint64_t& channelDropped) {
                                const std::size_t removed =
                                    utf8PrefixRemovalLength(content, std::min(excess, content.size()));
                                if (removed == 0) {
                                    return;
                                }
                                content.erase(0, removed);
                                excess = removed >= excess ? 0 : excess - removed;
                                guardedSubtractSize(state.capacity.accumulatedContentBytes, removed);
                                saturatingAdd(item->second.droppedContentBytes, saturatingUint64(removed));
                                saturatingAdd(channelDropped, saturatingUint64(removed));
                                saturatingAdd(state.capacity.droppedContentBytes, saturatingUint64(removed));
                                thread->second.fullyLoaded = false;
                                canonicalStateRewritten = true;
                            };
                            trim(item->second.agentText, item->second.agentTextDroppedContentBytes);
                            trim(item->second.reasoningText, item->second.reasoningTextDroppedContentBytes);
                            trim(item->second.reasoningSummary, item->second.reasoningSummaryDroppedContentBytes);
                            trim(item->second.commandOutput, item->second.commandOutputDroppedContentBytes);
                            if (excess == 0) {
                                return;
                            }
                        }
                    }
                }
            };
            trimPass(true);
            if (excess != 0) {
                trimPass(false);
            }

            while (state.capacity.retainedNotices > limits.maxRetainedNotices && !state.notices.empty()) {
                state.notices.erase(state.notices.begin());
                guardedSubtractSize(state.capacity.retainedNotices, 1);
                saturatingAdd(state.capacity.evictedNotices);
                canonicalStateRewritten = true;
            }

            while (state.capacity.retainedProcesses > limits.maxRetainedProcesses) {
                const auto candidate = std::find_if(state.processOrder.begin(), state.processOrder.end(), [&state](const std::string& id) {
                    const auto process = state.processes.find(id);
                    return process != state.processes.end() && process->second.lifecycle == "exited" &&
                           !state.processReservationClaims.contains(id);
                });
                if (candidate == state.processOrder.end()) {
                    saturatingAdd(state.capacity.snapshotOmissions);
                    break;
                }
                eraseProcess(state, *candidate, true);
                canonicalStateRewritten = true;
            }

            std::size_t processOutputExcess = state.capacity.accumulatedProcessOutputBytes > limits.maxAccumulatedProcessOutputBytes
                                                  ? state.capacity.accumulatedProcessOutputBytes - limits.maxAccumulatedProcessOutputBytes
                                                  : 0;
            const auto trimProcessPass = [&state, &processOutputExcess, &canonicalStateRewritten](bool terminalOnly) {
                for (const std::string& processId : state.processOrder) {
                    auto process = state.processes.find(processId);
                    if (process == state.processes.end() || (terminalOnly && process->second.lifecycle != "exited")) {
                        continue;
                    }
                    const auto trim = [&state, &processOutputExcess, &process, &canonicalStateRewritten](std::string& output,
                                                                                                        bool stderrOutput) {
                        const std::size_t removed = std::min(output.size(), processOutputExcess);
                        if (removed == 0) {
                            return;
                        }
                        output.erase(0, removed);
                        if (stderrOutput) {
                            process->second.stderrTruncated = true;
                        } else {
                            process->second.stdoutTruncated = true;
                        }
                        processOutputExcess -= removed;
                        guardedSubtractSize(state.capacity.accumulatedProcessOutputBytes, removed);
                        saturatingAdd(process->second.droppedOutputBytes, saturatingUint64(removed));
                        saturatingAdd(state.capacity.droppedProcessOutputBytes, saturatingUint64(removed));
                        canonicalStateRewritten = true;
                    };
                    trim(process->second.stdoutData, false);
                    trim(process->second.stderrData, true);
                    if (processOutputExcess == 0) {
                        return;
                    }
                }
            };
            trimProcessPass(true);
            if (processOutputExcess != 0) {
                trimProcessPass(false);
            }

            while (state.capacity.retainedFuzzySearchSessions > limits.maxRetainedFuzzySearchSessions) {
                const auto candidate =
                    std::find_if(state.fuzzySearchOrder.begin(), state.fuzzySearchOrder.end(), [&state](const std::string& id) {
                        const auto search = state.fuzzySearchSessions.find(id);
                        return search != state.fuzzySearchSessions.end() && search->second.complete &&
                               !state.fuzzySearchReservationClaims.contains(id);
                    });
                if (candidate == state.fuzzySearchOrder.end()) {
                    saturatingAdd(state.capacity.snapshotOmissions);
                    break;
                }
                forgetResourceReservationClaim(state, ProviderResourceKind::FuzzySearch, *candidate);
                state.fuzzySearchSessions.erase(*candidate);
                state.fuzzySearchOrder.erase(candidate);
                guardedSubtractSize(state.capacity.retainedFuzzySearchSessions, 1);
                saturatingAdd(state.capacity.evictedFuzzySearchSessions);
                canonicalStateRewritten = true;
            }

            while (state.capacity.retainedActivityRecords > limits.maxRetainedActivityRecords) {
                const auto candidate =
                    std::find_if(state.activityOrder.begin(), state.activityOrder.end(), [&state](const std::string& id) {
                        const auto activity = state.activities.find(id);
                        return activity != state.activities.end() && !activity->second.active;
                    });
                if (candidate == state.activityOrder.end()) {
                    saturatingAdd(state.capacity.snapshotOmissions);
                    break;
                }
                state.activities.erase(*candidate);
                state.activityOrder.erase(candidate);
                guardedSubtractSize(state.capacity.retainedActivityRecords, 1);
                saturatingAdd(state.capacity.evictedActivityRecords);
                canonicalStateRewritten = true;
            }
            return canonicalStateRewritten;
        }

        const ServerRequestId& pendingProviderRequestId(const typed::TypedServerRequest& request) {
            return std::visit(
                [](const auto& value) -> const ServerRequestId& {
                    return value.requestId;
                },
                request);
        }

        std::uint64_t boundedFingerprint(std::string_view value) noexcept {
            std::uint64_t hash = 1469598103934665603ULL;
            for (const unsigned char character : value) {
                hash ^= character;
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        std::string pendingRequestMethod(const typed::TypedServerRequest& request) {
            return std::visit(
                [](const auto& value) {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, typed::CommandApprovalRequest>) {
                        return std::string{"command-approval"};
                    } else if constexpr (std::is_same_v<Value, typed::FileChangeApprovalRequest>) {
                        return std::string{"file-change-approval"};
                    } else if constexpr (std::is_same_v<Value, typed::UserInputRequest>) {
                        return std::string{"user-input"};
                    } else if constexpr (std::is_same_v<Value, typed::ApplyPatchApprovalRequest>) {
                        return std::string{"apply-patch-approval"};
                    } else if constexpr (std::is_same_v<Value, typed::ExecCommandApprovalRequest>) {
                        return std::string{"exec-command-approval"};
                    } else if constexpr (std::is_same_v<Value, typed::PermissionsApprovalRequest>) {
                        return std::string{"permissions-approval"};
                    } else if constexpr (std::is_same_v<Value, typed::AttestationGenerateRequest>) {
                        return std::string{"attestation-generate"};
                    } else if constexpr (std::is_same_v<Value, typed::DynamicToolCallRequest>) {
                        return std::string{"dynamic-tool-call"};
                    } else if constexpr (std::is_same_v<Value, typed::McpServerElicitationRequest>) {
                        return std::string{"mcp-server-elicitation"};
                    } else {
                        return std::string{"unknown-server-request"};
                    }
                },
                request);
        }

        std::string pendingProviderRequestIdDiagnostic(const typed::TypedServerRequest& request) {
            return std::visit(
                [](const auto& value) {
                    return std::visit(
                        [](const auto& idValue) {
                            using Value = std::decay_t<decltype(idValue)>;
                            if constexpr (std::is_same_v<Value, std::int64_t>) {
                                return std::string{"request-id-type=int64 request-id-value="} + std::to_string(idValue);
                            } else {
                                return std::string{"request-id-type=string request-id-fingerprint="} +
                                       std::to_string(boundedFingerprint(idValue));
                            }
                        },
                        value.requestId.value());
                },
                request);
        }

        void logResolvedMatch(std::uint64_t providerGeneration,
                              const typed::ThreadId& threadId,
                              bool matched,
                              std::string_view classification,
                              std::optional<PendingRequestId> retiredId = std::nullopt) noexcept {
            try {
                std::clog << "codex-backend: pending request resolved: provider-generation=" << providerGeneration
                          << " thread-id=" << threadId.value
                          << " matched=" << (matched ? "true" : "false")
                          << " classification=" << classification;
                if (retiredId) {
                    std::clog << " retired-backend-pending-request-id=" << retiredId->value();
                }
                std::clog << '\n';
            } catch (...) {
            }
        }

        std::optional<typed::ThreadId> pendingRequestThreadId(const typed::TypedServerRequest& request) {
            return std::visit(
                [](const auto& value) -> std::optional<typed::ThreadId> {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, typed::CommandApprovalRequest> ||
                                  std::is_same_v<Value, typed::FileChangeApprovalRequest> ||
                                  std::is_same_v<Value, typed::UserInputRequest>) {
                        return value.threadId;
                    } else if constexpr (std::is_same_v<Value, typed::ApplyPatchApprovalRequest> ||
                                         std::is_same_v<Value, typed::ExecCommandApprovalRequest>) {
                        return value.params.conversationId;
                    } else if constexpr (std::is_same_v<Value, typed::PermissionsApprovalRequest> ||
                                         std::is_same_v<Value, typed::DynamicToolCallRequest>) {
                        return value.params.threadId;
                    } else if constexpr (std::is_same_v<Value, typed::McpServerElicitationRequest>) {
                        return value.params.threadId;
                    }
                    return std::nullopt;
                },
                request);
        }

        void logPendingRequestRemoval(PendingRequestId id,
                                      std::uint64_t providerGeneration,
                                      const typed::TypedServerRequest& request,
                                      std::string_view reason) noexcept {
            try {
                std::clog << "codex-backend: pending request lifecycle: action=removed"
                          << " backend-pending-request-id=" << id.value()
                          << " provider-generation=" << providerGeneration
                          << " method=" << pendingRequestMethod(request)
                          << ' ' << pendingProviderRequestIdDiagnostic(request);
                if (const std::optional<typed::ThreadId> threadId = pendingRequestThreadId(request); threadId) {
                    std::clog << " thread-id=" << threadId->value;
                }
                std::clog << " removal-reason=" << reason << '\n';
            } catch (...) {
            }
        }

        Reduction applyTypedNotificationState(BackendState& state,
                                              const std::string& method,
                                              const typed::Event& event,
                                              const ReducerOptions& options,
                                              RetentionInsertions& insertions) {
            const bool requestResolved = std::holds_alternative<typed::ServerRequestResolvedNotification>(event);
            if (!requestResolved) {
                retainDomainNotification(state, method, event);
            }
            Reduction reduction{!requestResolved, false};
            const auto boundedText = [&options](std::string value) {
                if (value.size() > options.maxNoticeDetailsBytes) {
                    value.resize(options.maxNoticeDetailsBytes);
                }
                return value;
            };
            std::visit(
                [&](const auto& value) {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, typed::TerminalInteractionNotification>) {
                        if (ItemState* item = findItem(state, value.threadId, value.turnId, value.itemId)) {
                            item->extensions["terminalInteraction"] =
                                Json::object({{"processId", boundedText(value.processId)}, {"stdinBytes", value.stdin.size()}});
                            item->stamp = currentStamp(state);
                            item->connectionInvalidated = false;
                        }
                    } else if constexpr (std::is_same_v<Value, typed::FileChangeOutputDeltaNotification>) {
                        if (ItemState* item = findItem(state, value.threadId, value.turnId, value.itemId)) {
                            const std::size_t before = itemContentBytes(*item);
                            appendBounded(item->commandOutput,
                                          value.delta,
                                          options.maxAccumulatedItemBytes,
                                          item->droppedContentBytes,
                                          &item->commandOutputDroppedContentBytes);
                            replaceAccumulatedContent(state.capacity, before, itemContentBytes(*item));
                            item->stamp = currentStamp(state);
                            item->connectionInvalidated = false;
                        }
                    } else if constexpr (std::is_same_v<Value, typed::McpToolCallProgressNotification>) {
                        if (ItemState* item = findItem(state, value.threadId, value.turnId, value.itemId)) {
                            item->extensions["mcpProgress"] = boundedText(value.message);
                            item->stamp = currentStamp(state);
                            item->connectionInvalidated = false;
                        }
                    } else if constexpr (std::is_same_v<Value, typed::PlanDeltaNotification>) {
                        if (ItemState* item = findItem(state, value.threadId, value.turnId, value.itemId)) {
                            item->extensions["planDelta"] = boundedText(value.delta);
                            item->stamp = currentStamp(state);
                            item->connectionInvalidated = false;
                        }
                    } else if constexpr (std::is_same_v<Value, typed::ReasoningSummaryPartAddedNotification>) {
                        if (ItemState* item = findItem(state, value.threadId, value.turnId, value.itemId)) {
                            item->extensions["reasoningSummaryPart"] = value.summaryIndex;
                            item->stamp = currentStamp(state);
                            item->connectionInvalidated = false;
                        }
                    } else if constexpr (std::is_same_v<Value, typed::ThreadArchivedNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        thread.archived = true;
                        thread.stamp = currentStamp(state);
                        markOperationStale(state, "thread/list");
                    } else if constexpr (std::is_same_v<Value, typed::ThreadClosedNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        thread.extensions["closed"] = true;
                        thread.stamp = currentStamp(state);
                    } else if constexpr (std::is_same_v<Value, typed::ContextCompactedNotification>) {
                        TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                        turn.extensions["contextCompacted"] = true;
                        turn.stamp = currentStamp(state);
                        turn.connectionInvalidated = false;
                    } else if constexpr (std::is_same_v<Value, typed::ThreadDeletedNotification>) {
                        eraseThread(state, value.threadId.value);
                        markOperationStale(state, "thread/list");
                    } else if constexpr (std::is_same_v<Value, typed::ThreadGoalClearedNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        thread.extensions["goal"] = nullptr;
                        thread.stamp = currentStamp(state);
                        markLatestGoalStale(state, value.threadId);
                    } else if constexpr (std::is_same_v<Value, typed::ThreadGoalUpdatedNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        thread.extensions["goal"] = Json::object({{"objective", boundedText(value.goal.objective)},
                                                                  {"status", value.goal.status.value},
                                                                  {"updatedAt", value.goal.updatedAt}});
                        thread.stamp = currentStamp(state);
                        markLatestGoalStale(state, value.threadId);
                    } else if constexpr (std::is_same_v<Value, typed::ThreadNameUpdatedNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        thread.thread.name = value.threadName;
                        thread.thread.title = value.threadName.hasValue() ? value.threadName.value : std::nullopt;
                        thread.stamp = currentStamp(state);
                    } else if constexpr (std::is_same_v<Value, typed::ThreadSettingsUpdatedNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        thread.thread.cwd = value.threadSettings.cwd;
                        thread.thread.model = value.threadSettings.model;
                        thread.thread.modelProvider = value.threadSettings.modelProvider;
                        const typed::ThreadSettings retained = retainedExecutionConfiguration(value.threadSettings);
                        thread.executionConfiguration = retained;
                        for (auto turnId = thread.turnOrder.rbegin(); turnId != thread.turnOrder.rend(); ++turnId) {
                            const auto turn = thread.turns.find(turnId->value);
                            if (turn != thread.turns.end() && turn->second.active) {
                                turn->second.effectiveExecutionConfiguration = retained;
                                turn->second.effectiveExecutionConfigurationProvenance = "thread_settings_updated";
                                break;
                            }
                        }
                        thread.extensions.erase("settings");
                        thread.stamp = currentStamp(state);
                    } else if constexpr (std::is_same_v<Value, typed::ThreadUnarchivedNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        thread.archived = false;
                        thread.stamp = currentStamp(state);
                        markOperationStale(state, "thread/list");
                    } else if constexpr (std::is_same_v<Value, typed::TurnDiffUpdatedNotification>) {
                        TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                        turn.extensions["diff"] = boundedText(value.diff);
                        turn.stamp = currentStamp(state);
                        turn.connectionInvalidated = false;
                    } else if constexpr (std::is_same_v<Value, typed::TurnModerationMetadataNotification>) {
                        TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                        std::size_t metadataBytes = 0;
                        if (value.metadata) {
                            try {
                                metadataBytes = value.metadata->dump().size();
                            } catch (...) {
                                metadataBytes = std::numeric_limits<std::size_t>::max();
                            }
                        }
                        turn.extensions["moderation"] = Json::object({{"present", value.metadata.has_value()}, {"bytes", metadataBytes}});
                        turn.stamp = currentStamp(state);
                        turn.connectionInvalidated = false;
                    } else if constexpr (std::is_same_v<Value, typed::TurnPlanUpdatedNotification>) {
                        TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                        const std::size_t previousPlanBytes = turnPlanContentBytes(turn.plan);
                        TurnPlanState plan;
                        plan.totalSteps = value.plan.size();
                        const std::size_t retainedSteps = std::min(value.plan.size(), MaxRetainedTurnPlanSteps);
                        plan.steps.reserve(retainedSteps);
                        for (std::size_t index = 0; index < retainedSteps; ++index) {
                            std::string step = boundedUtf8Text(value.plan[index].step, MaxRetainedTurnPlanStepBytes);
                            if (step.size() != value.plan[index].step.size()) {
                                plan.truncated = true;
                            }
                            typed::TurnPlanStepStatus status = value.plan[index].status;
                            std::string retainedStatus = boundedUtf8Text(status.value, MaxRetainedTurnPlanStatusBytes);
                            if (retainedStatus.size() != status.value.size()) {
                                plan.truncated = true;
                            }
                            status.value = std::move(retainedStatus);
                            plan.steps.push_back({std::move(step), std::move(status)});
                        }
                        plan.truncated = plan.truncated || retainedSteps < value.plan.size();
                        if (value.explanation.hasValue()) {
                            std::string explanation =
                                boundedUtf8Text(*value.explanation.value, MaxRetainedTurnPlanExplanationBytes);
                            if (explanation.size() != value.explanation.value->size()) {
                                plan.truncated = true;
                            }
                            plan.explanation = std::move(explanation);
                        }
                        turn.plan = std::move(plan);
                        replaceAccumulatedContent(state.capacity, previousPlanBytes, turnPlanContentBytes(turn.plan));
                        turn.extensions.erase("plan");
                        turn.stamp = currentStamp(state);
                        turn.connectionInvalidated = false;
                    } else if constexpr (std::is_same_v<Value, typed::AccountLoginCompletedNotification>) {
                        AccountLoginFlowState login = state.accounts.login.value_or(AccountLoginFlowState{});
                        login.lifecycle = value.success ? "completed" : "failed";
                        login.success = value.success;
                        login.loginId = value.loginId.hasValue() ? value.loginId.value : login.loginId;
                        login.error = value.error.hasValue() ? std::optional<std::string>{boundedText(*value.error.value)} : std::nullopt;
                        login.stamp = currentStamp(state);
                        state.accounts.login = std::move(login);
                        markOperationStale(state, "account/read");
                    } else if constexpr (std::is_same_v<Value, typed::AccountRateLimitsUpdatedNotification>) {
                        updateAccountRateLimits(state, value.rateLimits, options);
                        markOperationStale(state, "account/rateLimits/read");
                    } else if constexpr (std::is_same_v<Value, typed::AccountUpdatedNotification>) {
                        AccountAuthenticationState authentication = state.accounts.authentication.value_or(AccountAuthenticationState{});
                        if (value.authMode.hasValue()) {
                            authentication.authMode = boundedText(value.authMode.value->value);
                        }
                        if (value.planType.hasValue()) {
                            authentication.planType = boundedText(value.planType.value->value);
                        }
                        authentication.stamp = currentStamp(state);
                        state.accounts.authentication = std::move(authentication);
                        markOperationStale(state, "account/read");
                    } else if constexpr (std::is_same_v<Value, typed::ModelSafetyBufferingUpdatedNotification>) {
                        TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                        turn.extensions["modelSafetyBuffering"] = Json::object({{"model", value.model.value},
                                                                                {"showBufferingUi", value.showBufferingUi},
                                                                                {"reasonCount", value.reasons.size()},
                                                                                {"useCaseCount", value.useCases.size()}});
                        turn.stamp = currentStamp(state);
                        turn.connectionInvalidated = false;
                    } else if constexpr (std::is_same_v<Value, typed::ModelVerificationNotification>) {
                        TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                        Json verifications = Json::array();
                        const std::size_t count = std::min(value.verifications.size(), MaxRetainedModelVerifications);
                        for (std::size_t index = 0; index < count; ++index) {
                            verifications.push_back(boundedText(value.verifications[index].value));
                        }
                        turn.extensions["modelVerifications"] = Json::object({{"entries", std::move(verifications)},
                                                                              {"total", value.verifications.size()},
                                                                              {"truncated", count < value.verifications.size()}});
                        turn.stamp = currentStamp(state);
                        turn.connectionInvalidated = false;
                    } else if constexpr (std::is_same_v<Value, typed::AppListUpdatedNotification>) {
                        updateAppCatalog(state, value.data, options);
                        markOperationStale(state, "app/list");
                    } else if constexpr (std::is_same_v<Value, typed::SkillsChangedNotification>) {
                        markOperationStale(state, "skills/list");
                    } else if constexpr (std::is_same_v<Value, typed::McpServerStatusUpdatedNotification>) {
                        McpStartupState startup;
                        startup.serverName = boundedText(value.name);
                        startup.status = boundedText(value.status.value);
                        startup.error = value.error.hasValue() ? std::optional<std::string>{boundedText(*value.error.value)} : std::nullopt;
                        startup.failureReason = value.failureReason.hasValue()
                                                    ? std::optional<std::string>{boundedText(value.failureReason.value->value)}
                                                    : std::nullopt;
                        startup.threadId = value.threadId.hasValue() ? value.threadId.value : std::nullopt;
                        startup.stamp = currentStamp(state);
                        state.mcp.startup = std::move(startup);
                        markOperationStale(state, "mcpServerStatus/list");
                    } else if constexpr (std::is_same_v<Value, typed::RemoteControlStatusChangedNotification>) {
                        RemoteControlState remote;
                        remote.status = boundedText(value.status.value);
                        remote.environmentId = value.environmentId.hasValue()
                                                   ? std::optional<std::string>{boundedText(*value.environmentId.value)}
                                                   : std::nullopt;
                        remote.installationId = boundedText(value.installationId);
                        remote.serverName = boundedText(value.serverName);
                        remote.stamp = currentStamp(state);
                        state.platform.remoteControl = std::move(remote);
                    } else if constexpr (std::is_same_v<Value, typed::ThreadRealtimeStartedNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        thread.realtime.lifecycle = "started";
                        thread.realtime.sessionId = value.realtimeSessionId.hasValue() ? value.realtimeSessionId.value : std::nullopt;
                        thread.realtime.version = value.version.value;
                        thread.realtime.lastError.reset();
                        thread.realtime.stamp = currentStamp(state);
                    } else if constexpr (std::is_same_v<Value, typed::ThreadRealtimeClosedNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        thread.realtime.lifecycle = "closed";
                        thread.realtime.stamp = currentStamp(state);
                    } else if constexpr (std::is_same_v<Value, typed::ThreadRealtimeErrorNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        thread.realtime.lifecycle = "error";
                        thread.realtime.lastError = value.message;
                        if (thread.realtime.lastError->size() > options.maxNoticeDetailsBytes) {
                            thread.realtime.lastError->resize(options.maxNoticeDetailsBytes);
                        }
                        thread.realtime.stamp = currentStamp(state);
                    } else if constexpr (std::is_same_v<Value, typed::ThreadRealtimeItemAddedNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        saturatingAddSize(thread.realtime.itemCount, 1);
                        thread.realtime.stamp = currentStamp(state);
                    } else if constexpr (std::is_same_v<Value, typed::ThreadRealtimeOutputAudioDeltaNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        saturatingAdd(thread.realtime.receivedAudioBytes, saturatingUint64(value.audio.data.size()));
                        saturatingAdd(thread.realtime.droppedAudioBytes, saturatingUint64(value.audio.data.size()));
                        thread.realtime.stamp = currentStamp(state);
                    } else if constexpr (std::is_same_v<Value, typed::ThreadRealtimeSdpNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        thread.realtime.lastSdpBytes = value.sdp.size();
                        thread.realtime.stamp = currentStamp(state);
                    } else if constexpr (std::is_same_v<Value, typed::ThreadRealtimeTranscriptDeltaNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        std::uint64_t dropped = 0;
                        appendBounded(thread.realtime.transcript, value.delta, options.maxRealtimeTranscriptBytes, dropped);
                        thread.realtime.transcriptTruncated = thread.realtime.transcriptTruncated || dropped != 0;
                        thread.realtime.stamp = currentStamp(state);
                    } else if constexpr (std::is_same_v<Value, typed::ThreadRealtimeTranscriptDoneNotification>) {
                        ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                        std::uint64_t dropped = 0;
                        assignBounded(thread.realtime.transcript, value.text, options.maxRealtimeTranscriptBytes, dropped);
                        thread.realtime.transcriptTruncated = thread.realtime.transcriptTruncated || dropped != 0;
                        thread.realtime.stamp = currentStamp(state);
                    } else if constexpr (std::is_same_v<Value, typed::CommandExecOutputDeltaNotification>) {
                        ProcessState* process = admitProcess(state, value.processId.value);
                        if (!process) {
                            reduction.providerCapacityFailure = true;
                            reduction.providerCapacityFailureMessage = "unsolicited command process exceeds backend process capacity";
                        } else {
                            process->lifecycle = "running";
                            process->stamp = currentStamp(state);
                            process->connectionInvalidated = false;
                            if (value.stream.value == "stderr") {
                                appendProcessOutput(state, *process, process->stderrData, value.deltaBase64, true);
                                process->stderrTruncated = process->stderrTruncated || value.capReached;
                            } else {
                                appendProcessOutput(state, *process, process->stdoutData, value.deltaBase64, false);
                                process->stdoutTruncated = process->stdoutTruncated || value.capReached;
                            }
                        }
                    } else if constexpr (std::is_same_v<Value, typed::ProcessOutputDeltaNotification>) {
                        ProcessState* process = admitProcess(state, value.processHandle);
                        if (!process) {
                            reduction.providerCapacityFailure = true;
                            reduction.providerCapacityFailureMessage = "unsolicited process exceeds backend process capacity";
                        } else {
                            process->lifecycle = "running";
                            process->stamp = currentStamp(state);
                            process->connectionInvalidated = false;
                            if (value.stream.value == "stderr") {
                                appendProcessOutput(state, *process, process->stderrData, value.deltaBase64, true);
                                process->stderrTruncated = process->stderrTruncated || value.capReached;
                            } else {
                                appendProcessOutput(state, *process, process->stdoutData, value.deltaBase64, false);
                                process->stdoutTruncated = process->stdoutTruncated || value.capReached;
                            }
                        }
                    } else if constexpr (std::is_same_v<Value, typed::ProcessExitedNotification>) {
                        if (ProcessState* process = admitProcess(state, value.processHandle)) {
                            process->lifecycle = "exited";
                            process->exitCode = value.exitCode;
                            process->stamp = currentStamp(state);
                            process->connectionInvalidated = false;
                            replaceProcessOutput(state, *process, process->stdoutData, value.stdout, false);
                            replaceProcessOutput(state, *process, process->stderrData, value.stderr, true);
                            process->stdoutTruncated = process->stdoutTruncated || value.stdoutCapReached;
                            process->stderrTruncated = process->stderrTruncated || value.stderrCapReached;
                        }
                    } else if constexpr (std::is_same_v<Value, typed::FsChangedNotification>) {
                        FilesystemWatchState* watch = admitFilesystemWatch(state, value.watchId);
                        if (!watch) {
                            reduction.providerCapacityFailure = true;
                            reduction.providerCapacityFailureMessage = "unsolicited filesystem watch exceeds backend watch capacity";
                        } else {
                            const std::size_t first = value.changedPaths.size() > MaxRetainedFilesystemChangePaths
                                                          ? value.changedPaths.size() - MaxRetainedFilesystemChangePaths
                                                          : 0;
                            watch->changedPaths.assign(value.changedPaths.begin() + static_cast<std::ptrdiff_t>(first),
                                                       value.changedPaths.end());
                            for (typed::AbsolutePath& path : watch->changedPaths) {
                                if (path.value.size() > options.maxNoticeDetailsBytes) {
                                    path.value.resize(options.maxNoticeDetailsBytes);
                                }
                            }
                            watch->stamp = currentStamp(state);
                            watch->connectionInvalidated = false;
                        }
                    } else if constexpr (std::is_same_v<Value, typed::FuzzyFileSearchSessionUpdatedNotification>) {
                        FuzzySearchState* search = admitFuzzySearch(state, value.sessionId);
                        if (!search) {
                            reduction.providerCapacityFailure = true;
                            reduction.providerCapacityFailureMessage = "unsolicited fuzzy search exceeds backend search capacity";
                        } else {
                            search->query = boundedText(value.query);
                            search->files.assign(value.files.begin(),
                                                 value.files.begin() + static_cast<std::ptrdiff_t>(std::min(
                                                                           value.files.size(), MaxRetainedFuzzyResultsPerSession)));
                            for (typed::FuzzyFileSearchResult& result : search->files) {
                                boundFuzzyResult(result, options.maxNoticeDetailsBytes);
                            }
                            search->complete = false;
                            search->stamp = currentStamp(state);
                            search->connectionInvalidated = false;
                        }
                    } else if constexpr (std::is_same_v<Value, typed::FuzzyFileSearchSessionCompletedNotification>) {
                        if (FuzzySearchState* search = admitFuzzySearch(state, value.sessionId)) {
                            search->complete = true;
                            search->stamp = currentStamp(state);
                            search->connectionInvalidated = false;
                        }
                    } else if constexpr (std::is_same_v<Value, typed::DeprecationNoticeNotification>) {
                        NoticeState notice;
                        notice.occurrence = state.nextNoticeOccurrence;
                        saturatingAdd(state.nextNoticeOccurrence);
                        notice.category = NoticeCategory::Deprecation;
                        notice.summary = value.summary;
                        if (value.details.hasValue()) {
                            notice.details = *value.details.value;
                        }
                        notice.stamp = currentStamp(state);
                        boundNotice(notice, options.maxNoticeSummaryBytes, options.maxNoticeDetailsBytes);
                        retainNotice(state, std::move(notice));
                    } else if constexpr (std::is_same_v<Value, typed::WarningNotification>) {
                        NoticeState notice;
                        notice.occurrence = state.nextNoticeOccurrence;
                        saturatingAdd(state.nextNoticeOccurrence);
                        notice.category = NoticeCategory::Warning;
                        notice.summary = value.message;
                        notice.threadId = value.threadId.hasValue() ? value.threadId.value : std::nullopt;
                        notice.stamp = currentStamp(state);
                        boundNotice(notice, options.maxNoticeSummaryBytes, options.maxNoticeDetailsBytes);
                        retainNotice(state, std::move(notice));
                    } else if constexpr (std::is_same_v<Value, typed::ConfigWarningNotification>) {
                        NoticeState notice;
                        notice.occurrence = state.nextNoticeOccurrence;
                        saturatingAdd(state.nextNoticeOccurrence);
                        notice.category = NoticeCategory::Configuration;
                        notice.summary = value.summary;
                        notice.details = value.details.hasValue() ? value.details.value : std::nullopt;
                        notice.stamp = currentStamp(state);
                        boundNotice(notice, options.maxNoticeSummaryBytes, options.maxNoticeDetailsBytes);
                        retainNotice(state, std::move(notice));
                    } else if constexpr (std::is_same_v<Value, typed::GuardianWarningNotification>) {
                        NoticeState notice;
                        notice.occurrence = state.nextNoticeOccurrence;
                        saturatingAdd(state.nextNoticeOccurrence);
                        notice.category = NoticeCategory::Security;
                        notice.summary = value.message;
                        notice.threadId = value.threadId;
                        notice.stamp = currentStamp(state);
                        boundNotice(notice, options.maxNoticeSummaryBytes, options.maxNoticeDetailsBytes);
                        retainNotice(state, std::move(notice));
                    } else if constexpr (std::is_same_v<Value, typed::WindowsWorldWritableWarningNotification>) {
                        NoticeState notice;
                        notice.occurrence = state.nextNoticeOccurrence;
                        saturatingAdd(state.nextNoticeOccurrence);
                        notice.category = NoticeCategory::WindowsWorldWritable;
                        notice.summary = value.failedScan ? "world-writable path scan failed" : "world-writable paths detected";
                        notice.details = "sample paths: " + std::to_string(value.samplePaths.size()) +
                                         ", additional paths: " + std::to_string(value.extraCount);
                        notice.stamp = currentStamp(state);
                        boundNotice(notice, options.maxNoticeSummaryBytes, options.maxNoticeDetailsBytes);
                        retainNotice(state, std::move(notice));
                    } else if constexpr (std::is_same_v<Value, typed::ItemGuardianApprovalReviewStartedNotification>) {
                        retainActivity(state,
                                       value.reviewId,
                                       "guardian_review",
                                       "started",
                                       true,
                                       method,
                                       event,
                                       options,
                                       "action/" + std::to_string(value.action.index()),
                                       std::nullopt,
                                       value.threadId,
                                       value.turnId);
                    } else if constexpr (std::is_same_v<Value, typed::ItemGuardianApprovalReviewCompletedNotification>) {
                        retainActivity(state,
                                       value.reviewId,
                                       "guardian_review",
                                       "completed",
                                       false,
                                       method,
                                       event,
                                       options,
                                       "action/" + std::to_string(value.action.index()),
                                       value.decisionSource.value,
                                       value.threadId,
                                       value.turnId);
                    } else if constexpr (std::is_same_v<Value, typed::ExternalAgentConfigImportProgressNotification>) {
                        retainActivity(state,
                                       value.importId,
                                       "external_agent_import",
                                       "running",
                                       true,
                                       method,
                                       event,
                                       options,
                                       "external agent import",
                                       std::to_string(value.itemTypeResults.size()) + " item-type results");
                    } else if constexpr (std::is_same_v<Value, typed::ExternalAgentConfigImportCompletedNotification>) {
                        markOperationStale(state, "externalAgentConfig/import/readHistories");
                        retainActivity(state,
                                       value.importId,
                                       "external_agent_import",
                                       "completed",
                                       false,
                                       method,
                                       event,
                                       options,
                                       "external agent import",
                                       std::to_string(value.itemTypeResults.size()) + " item-type results");
                    } else if constexpr (std::is_same_v<Value, typed::HookStartedNotification>) {
                        retainActivity(state,
                                       value.run.id,
                                       "hook",
                                       "started",
                                       true,
                                       method,
                                       event,
                                       options,
                                       value.run.eventName.value,
                                       value.run.status.value,
                                       value.threadId,
                                       value.turnId.hasValue() ? value.turnId.value : std::nullopt);
                    } else if constexpr (std::is_same_v<Value, typed::HookCompletedNotification>) {
                        retainActivity(state,
                                       value.run.id,
                                       "hook",
                                       "completed",
                                       false,
                                       method,
                                       event,
                                       options,
                                       value.run.eventName.value,
                                       value.run.status.value,
                                       value.threadId,
                                       value.turnId.hasValue() ? value.turnId.value : std::nullopt);
                    } else if constexpr (std::is_same_v<Value, typed::McpServerOauthLoginCompletedNotification>) {
                        McpOauthState oauth = state.mcp.oauth.value_or(McpOauthState{});
                        oauth.serverName = boundedText(value.name);
                        oauth.lifecycle = value.success ? "completed" : "failed";
                        oauth.success = value.success;
                        oauth.error = value.error.hasValue() ? std::optional<std::string>{boundedText(*value.error.value)} : std::nullopt;
                        oauth.threadId = value.threadId.hasValue() ? value.threadId.value : std::nullopt;
                        oauth.stamp = currentStamp(state);
                        state.mcp.oauth = oauth;
                        retainActivity(state,
                                       value.name,
                                       "mcp_oauth",
                                       oauth.lifecycle,
                                       false,
                                       method,
                                       event,
                                       options,
                                       "MCP OAuth",
                                       oauth.error,
                                       oauth.threadId);
                    } else if constexpr (std::is_same_v<Value, typed::WindowsSandboxSetupCompletedNotification>) {
                        WindowsSandboxState windows = state.platform.windowsSandbox.value_or(WindowsSandboxState{});
                        windows.lifecycle = value.success ? "completed" : "failed";
                        windows.mode = boundedText(value.mode.value);
                        windows.success = value.success;
                        windows.error = value.error.hasValue() ? std::optional<std::string>{boundedText(*value.error.value)} : std::nullopt;
                        windows.stamp = currentStamp(state);
                        state.platform.windowsSandbox = windows;
                        markOperationStale(state, "windowsSandbox/readiness");
                        retainActivity(state,
                                       "windows_sandbox",
                                       "windows_sandbox_setup",
                                       windows.lifecycle,
                                       false,
                                       method,
                                       event,
                                       options,
                                       windows.mode,
                                       windows.error);
                    } else if constexpr (std::is_same_v<Value, typed::ServerRequestResolvedNotification>) {
                        bool requestIdMatched = false;
                        bool generationMismatched = false;
                        bool threadMismatched = false;
                        for (auto iterator = state.pendingRequests.begin(); iterator != state.pendingRequests.end(); ++iterator) {
                            PendingRequestState& pending = iterator->second;
                            if (pendingProviderRequestId(pending.request) != value.requestId) {
                                continue;
                            }
                            requestIdMatched = true;
                            if (pending.connectionGeneration != state.provider.generation) {
                                generationMismatched = true;
                                continue;
                            }
                            const std::optional<typed::ThreadId> pendingThread = pendingRequestThreadId(pending.request);
                            if (pendingThread && pendingThread->value != value.threadId.value) {
                                threadMismatched = true;
                                retainDomainNotification(state, method, event);
                                reduction.changed = true;
                                reduction.flushImmediately = true;
                                logResolvedMatch(state.provider.generation, value.threadId, false, "thread-mismatch");
                                break;
                            }
                            const PendingRequestId id = iterator->first;
                            const PendingRequestState removed = pending;
                            state.pendingRequests.erase(iterator);
                            reduction.changed = true;
                            reduction.pendingRequestRemovals.push_back({id, "externally_resolved"});
                            reduction.flushImmediately = true;
                            logPendingRequestRemoval(id, removed.connectionGeneration, removed.request, "externally_resolved");
                            logResolvedMatch(state.provider.generation, value.threadId, true, "matched", id);
                            break;
                        }
                        if (!reduction.changed) {
                            if (!requestIdMatched) {
                                logResolvedMatch(state.provider.generation, value.threadId, false, "no-request-id-match");
                            } else if (generationMismatched) {
                                logResolvedMatch(state.provider.generation, value.threadId, false, "generation-mismatch");
                            } else if (threadMismatched) {
                                logResolvedMatch(state.provider.generation, value.threadId, false, "thread-mismatch");
                            }
                        }
                    }
                },
                event);
            return reduction;
        }

        void retainExtension(BackendState& state,
                             ExtensionRecord extension,
                             std::size_t limit,
                             std::size_t methodByteLimit,
                             std::size_t payloadByteLimit,
                             std::size_t decodingErrorByteLimit) {
            if (limit == 0) {
                return;
            }
            if (extension.method.size() > methodByteLimit) {
                extension.originalMethodBytes = static_cast<std::uint64_t>(extension.method.size());
                extension.method.resize(methodByteLimit);
            }
            if (extension.decodingError && extension.decodingError->size() > decodingErrorByteLimit) {
                extension.originalDecodingErrorBytes = static_cast<std::uint64_t>(extension.decodingError->size());
                extension.decodingError->resize(decodingErrorByteLimit);
            }
            if (extension.diagnostic) {
                std::uint64_t originalDiagnosticBytes = 0;
                const auto account = [&originalDiagnosticBytes](std::size_t bytes) {
                    const std::uint64_t value = saturatingUint64(bytes);
                    originalDiagnosticBytes = value > std::numeric_limits<std::uint64_t>::max() - originalDiagnosticBytes
                                                  ? std::numeric_limits<std::uint64_t>::max()
                                                  : originalDiagnosticBytes + value;
                };
                account(extension.diagnostic->surface.size());
                account(extension.diagnostic->fieldPath.size());
                account(extension.diagnostic->message.size());
                bool diagnosticTruncated = false;
                if (extension.diagnostic->surface.size() > methodByteLimit) {
                    extension.diagnostic->surface.resize(methodByteLimit);
                    diagnosticTruncated = true;
                }
                if (extension.diagnostic->fieldPath.size() > decodingErrorByteLimit) {
                    extension.diagnostic->fieldPath.resize(decodingErrorByteLimit);
                    diagnosticTruncated = true;
                }
                if (extension.diagnostic->message.size() > decodingErrorByteLimit) {
                    extension.diagnostic->message.resize(decodingErrorByteLimit);
                    diagnosticTruncated = true;
                }
                if (diagnosticTruncated) {
                    extension.originalDiagnosticBytes = originalDiagnosticBytes;
                }
            }
            try {
                const std::string encoded = extension.payload.dump();
                if (encoded.size() > payloadByteLimit) {
                    extension.originalPayloadBytes = static_cast<std::uint64_t>(encoded.size());
                    extension.payload = Json::object({{"truncated", true},
                                                      {"originalBytes", encoded.size()},
                                                      {"omitted", true},
                                                      {"reason", "extension payload exceeds canonical reducer bound"}});
                }
            } catch (...) {
                extension.payload = Json::object({{"omitted", true}, {"reason", "extension serialization failed"}});
            }
            state.recentExtensions.push_back(std::move(extension));
            if (state.recentExtensions.size() > limit) {
                state.recentExtensions.erase(state.recentExtensions.begin(),
                                             state.recentExtensions.begin() +
                                                 static_cast<std::ptrdiff_t>(state.recentExtensions.size() - limit));
            }
        }
    } // namespace

    CodexExtensionReceived detail::preserveUnmodeledTypedEvent(UnmodeledTypedEvent event) {
        return {.method = std::move(event.surface),
                .payload = std::move(event.rawPayload),
                .decodingError = std::move(event.decodingError),
                .diagnostic = std::move(event.diagnostic)};
    }

    Reducer::Reducer(ReducerOptions options)
        : options(std::move(options)) {
    }

    Reduction Reducer::apply(BackendState& state, const BackendEvent& event) const {
        RetentionInsertions insertions;
        const CapacityState capacityBefore = state.capacity;
        Reduction reduction = std::visit(
            Overloaded{
                [&state](const ProviderLifecycleChanged& value) {
                    const bool changed = !sameProvider(state.provider, value.provider);
                    state.provider = value.provider;
                    return Reduction{changed,
                                     value.provider.lifecycle == ProviderLifecycle::Failed ||
                                         value.provider.lifecycle == ProviderLifecycle::Stopping ||
                                         value.provider.lifecycle == ProviderLifecycle::Recovering};
                },
                [&state](const ProviderConnectionInvalidated& value) {
                    if (value.generation != state.provider.generation) {
                        return Reduction{};
                    }
                    Reduction reduction{true, true};
                    reduction.pendingRequestRemovals.reserve(state.pendingRequests.size());
                    for (const auto& [id, pending] : state.pendingRequests) {
                        logPendingRequestRemoval(id, pending.connectionGeneration, pending.request, value.reason);
                        reduction.pendingRequestRemovals.push_back({id, value.reason});
                    }
                    for (auto& [threadId, thread] : state.threads) {
                        (void) threadId;
                        thread.stamp.freshness = Freshness::Stale;
                        thread.realtime.stamp.freshness = Freshness::Stale;
                        for (auto& [turnId, turn] : thread.turns) {
                            (void) turnId;
                            turn.stamp.freshness = Freshness::Stale;
                            turn.connectionInvalidated = turn.active || !turn.terminal;
                            for (auto& [itemIdValue, item] : turn.items) {
                                (void) itemIdValue;
                                item.stamp.freshness = Freshness::Stale;
                                item.connectionInvalidated =
                                    item.lifecycle == ItemLifecycle::Started || item.lifecycle == ItemLifecycle::Unknown;
                            }
                        }
                    }
                    state.threadList.stamp.freshness = Freshness::Stale;
                    for (auto& [method, operation] : state.providerOperations) {
                        (void) method;
                        operation.stamp.freshness = Freshness::Stale;
                    }
                    markDomainStale(state.accounts);
                    markDomainStale(state.models);
                    markDomainStale(state.configuration);
                    markDomainStale(state.conversations);
                    markDomainStale(state.filesystem);
                    markDomainStale(state.reviews);
                    markDomainStale(state.integrations);
                    markDomainStale(state.pluginsAndSkills);
                    markDomainStale(state.mcp);
                    markDomainStale(state.platform);
                    const auto markStampStale = [](auto& value) {
                        if (value) {
                            value->stamp.freshness = Freshness::Stale;
                        }
                    };
                    markStampStale(state.accounts.login);
                    markStampStale(state.accounts.authentication);
                    markStampStale(state.accounts.rateLimits);
                    markStampStale(state.configuration.lastWrite);
                    markStampStale(state.configuration.experimentalFeatureEnablement);
                    markStampStale(state.conversations.latestGoal);
                    markStampStale(state.conversations.latestGoalClear);
                    markStampStale(state.conversations.latestGoalSet);
                    markStampStale(state.conversations.latestUnsubscribe);
                    markStampStale(state.integrations.marketplaceAdd);
                    markStampStale(state.integrations.marketplaceRemove);
                    markStampStale(state.integrations.marketplaceUpgrade);
                    markStampStale(state.pluginsAndSkills.pluginInstall);
                    markStampStale(state.pluginsAndSkills.pluginShareCheckout);
                    markStampStale(state.pluginsAndSkills.pluginShareSave);
                    markStampStale(state.pluginsAndSkills.pluginShareUpdateTargets);
                    markStampStale(state.pluginsAndSkills.skillsConfigWrite);
                    if (state.accounts.resetCreditOutcome) {
                        state.accounts.resetCreditStamp.freshness = Freshness::Stale;
                    }
                    if (state.accounts.loggedOut) {
                        state.accounts.logoutStamp.freshness = Freshness::Stale;
                    }
                    markStampStale(state.integrations.apps);
                    markStampStale(state.pluginsAndSkills.extraRoots);
                    markStampStale(state.mcp.oauth);
                    markStampStale(state.mcp.startup);
                    markStampStale(state.mcp.statusList);
                    markStampStale(state.platform.remoteControl);
                    markStampStale(state.platform.windowsSandbox);
                    for (auto& [id, process] : state.processes) {
                        (void) id;
                        process.stamp.freshness = Freshness::Stale;
                        process.connectionInvalidated = process.lifecycle != "exited";
                    }
                    for (auto& [id, watch] : state.filesystemWatches) {
                        (void) id;
                        watch.stamp.freshness = Freshness::Stale;
                        watch.connectionInvalidated = true;
                    }
                    for (auto& [id, search] : state.fuzzySearchSessions) {
                        (void) id;
                        search.stamp.freshness = Freshness::Stale;
                        search.connectionInvalidated = !search.complete;
                    }
                    for (NoticeState& notice : state.notices) {
                        notice.stamp.freshness = Freshness::Stale;
                    }
                    for (auto& [key, activity] : state.activities) {
                        (void) key;
                        activity.notification.stamp.freshness = Freshness::Stale;
                    }
                    state.processReservations.clear();
                    state.processReservationTargets.clear();
                    state.processReservationClaims.clear();
                    state.filesystemWatchReservations.clear();
                    state.filesystemWatchReservationTargets.clear();
                    state.filesystemWatchReservationClaims.clear();
                    state.fuzzySearchReservations.clear();
                    state.fuzzySearchReservationTargets.clear();
                    state.fuzzySearchReservationClaims.clear();
                    state.pendingRequests.clear();
                    return reduction;
                },
                [&state](const CapacityConfigured& value) {
                    const bool changed = state.capacity.limits != value.limits;
                    state.capacity.limits = value.limits;
                    return Reduction{changed, false};
                },
                [&state](const CapacityChanged& value) {
                    std::uint64_t* counter = nullptr;
                    switch (value.metric) {
                        case CapacityMetric::RejectedSessions:
                            counter = &state.capacity.rejectedSessions;
                            break;
                        case CapacityMetric::RejectedObservers:
                            counter = &state.capacity.rejectedObservers;
                            break;
                        case CapacityMetric::RejectedOperations:
                            counter = &state.capacity.rejectedOperations;
                            break;
                        case CapacityMetric::ProviderRequestOverflows:
                            counter = &state.capacity.providerRequestOverflows;
                            break;
                        case CapacityMetric::EvictedThreads:
                            counter = &state.capacity.evictedThreads;
                            break;
                        case CapacityMetric::EvictedTurns:
                            counter = &state.capacity.evictedTurns;
                            break;
                        case CapacityMetric::EvictedItems:
                            counter = &state.capacity.evictedItems;
                            break;
                        case CapacityMetric::DroppedContentBytes:
                            counter = &state.capacity.droppedContentBytes;
                            break;
                        case CapacityMetric::SnapshotOmissions:
                            counter = &state.capacity.snapshotOmissions;
                            break;
                        case CapacityMetric::EvictedNotices:
                            counter = &state.capacity.evictedNotices;
                            break;
                        case CapacityMetric::EvictedProcesses:
                            counter = &state.capacity.evictedProcesses;
                            break;
                        case CapacityMetric::DroppedProcessOutputBytes:
                            counter = &state.capacity.droppedProcessOutputBytes;
                            break;
                        case CapacityMetric::EvictedFilesystemWatches:
                            counter = &state.capacity.evictedFilesystemWatches;
                            break;
                        case CapacityMetric::EvictedFuzzySearchSessions:
                            counter = &state.capacity.evictedFuzzySearchSessions;
                            break;
                        case CapacityMetric::EvictedActivityRecords:
                            counter = &state.capacity.evictedActivityRecords;
                            break;
                    }
                    saturatingAdd(*counter, value.amount);
                    return Reduction{true, true};
                },
                [this, &state](const DiagnosticReceived& value) {
                    ++state.diagnostics.received;
                    if (options.retainedDiagnostics != 0) {
                        std::string message = value.message;
                        if (message.size() > options.maxDiagnosticBytes) {
                            message.erase(0, message.size() - options.maxDiagnosticBytes);
                        }
                        state.diagnostics.recent.push_back(std::move(message));
                        if (state.diagnostics.recent.size() > options.retainedDiagnostics) {
                            state.diagnostics.recent.erase(
                                state.diagnostics.recent.begin(),
                                state.diagnostics.recent.begin() +
                                    static_cast<std::ptrdiff_t>(state.diagnostics.recent.size() - options.retainedDiagnostics));
                        }
                    }
                    return Reduction{true, false};
                },
                [this, &state, &insertions](const ProviderOperationCompleted& value) {
                    Reduction reduction{true, false};

                    state.providerOperations.insert_or_assign(
                        value.method, ProviderOperationState{value.method, value.value.index(), currentStamp(state)});
                    notificationDomain(state, value.method)
                        .latestResults.insert_or_assign(value.method, summarizeProviderResult(value.method, value.value, state, options));

                    std::visit(
                        Overloaded{[this, &state](const typed::CancelLoginAccountResponse& result) {
                                       AccountLoginFlowState login = state.accounts.login.value_or(AccountLoginFlowState{});
                                       login.lifecycle = "cancelled";
                                       login.cancellationStatus = result.status.value;
                                       login.stamp = currentStamp(state);
                                       state.accounts.login = std::move(login);
                                   },
                                   [this, &state](const typed::LoginAccountResponse& result) {
                                       AccountLoginFlowState login;
                                       login.lifecycle = "started";
                                       login.method =
                                           boundedText(typed::loginAccountResponseDiscriminator(result), MaxCanonicalIdentifierBytes);
                                       std::visit(
                                           [&login](const auto& response) {
                                               if constexpr (requires { response.loginId; }) {
                                                   login.loginId = response.loginId;
                                               }
                                           },
                                           result);
                                       login.stamp = currentStamp(state);
                                       state.accounts.login = std::move(login);
                                   },
                                   [this, &state](const typed::ConsumeAccountRateLimitResetCreditResponse& result) {
                                       state.accounts.resetCreditOutcome = boundedText(result.outcome.value, MaxCanonicalIdentifierBytes);
                                       state.accounts.resetCreditStamp = currentStamp(state);
                                   },
                                   [this, &state](const typed::GetAccountRateLimitsResponse& result) {
                                       updateAccountRateLimits(state, result.rateLimits, options);
                                   },
                                   [this, &state](const typed::GetAccountResponse& result) {
                                       AccountAuthenticationState authentication;
                                       authentication.authenticated = result.account.hasValue();
                                       if (result.account.hasValue()) {
                                           authentication.accountType =
                                               boundedText(typed::accountDiscriminator(*result.account.value), MaxCanonicalIdentifierBytes);
                                           std::visit(
                                               [&authentication](const auto& account) {
                                                   using Account = std::decay_t<decltype(account)>;
                                                   if constexpr (std::is_same_v<Account, typed::ChatgptAccount>) {
                                                       authentication.planType = account.planType.value;
                                                   }
                                               },
                                               *result.account.value);
                                       }
                                       authentication.stamp = currentStamp(state);
                                       state.accounts.authentication = std::move(authentication);
                                       state.accounts.loggedOut = false;
                                   },
                                   [this, &state](const typed::ConfigWriteResponse& result) {
                                       state.configuration.lastWrite = configurationWriteState(result, state, options);
                                   },
                                   [&state](const typed::ExperimentalFeatureEnablementSetResponse& result) {
                                       state.configuration.experimentalFeatureEnablement = featureEnablementState(result, state);
                                   },
                                   [this, &state](const typed::ThreadGoalGetResponse& result) {
                                       state.conversations.latestGoal = result.goal.hasValue()
                                                                                 ? goalMutationState("get",
                                                                                                     result.goal.value->objective,
                                                                                                     result.goal.value->status.value,
                                                                                                     std::nullopt,
                                                                                                     true,
                                                                                                     state,
                                                                                                     options)
                                                                                 : goalMutationState("get",
                                                                                                     std::nullopt,
                                                                                                     std::nullopt,
                                                                                                     std::nullopt,
                                                                                                     false,
                                                                                                     state,
                                                                                                     options);
                                   },
                                   [this, &state](const typed::ThreadGoalClearResponse& result) {
                                       state.conversations.latestGoalClear =
                                           goalMutationState("clear", std::nullopt, std::nullopt, result.cleared, false, state, options);
                                   },
                                   [this, &state](const typed::ThreadGoalSetResponse& result) {
                                       state.conversations.latestGoalSet = goalMutationState("set",
                                                                                            result.goal.objective,
                                                                                            result.goal.status.value,
                                                                                            std::nullopt,
                                                                                            true,
                                                                                            state,
                                                                                            options);
                                   },
                                   [this, &state](const typed::ThreadUnsubscribeResponse& result) {
                                       state.conversations.latestUnsubscribe = goalMutationState(
                                           "unsubscribe", std::nullopt, result.status.value, std::nullopt, false, state, options);
                                   },
                                   [this, &state, &insertions](const typed::ReviewStartResponse& result) {
                                       upsertTurn(state, result.turn, options.maxAccumulatedItemBytes, &insertions);
                                   },
                                   [this, &state](const typed::AppsListResponse& result) {
                                       updateAppCatalog(state, result.data, options);
                                   },
                                   [this, &state](const typed::MarketplaceAddResponse& result) {
                                       state.integrations.marketplaceAdd = marketplaceAddState(result, state, options);
                                   },
                                   [this, &state](const typed::MarketplaceRemoveResponse& result) {
                                       state.integrations.marketplaceRemove = marketplaceRemoveState(result, state, options);
                                   },
                                   [&state](const typed::MarketplaceUpgradeResponse& result) {
                                       state.integrations.marketplaceUpgrade = marketplaceUpgradeState(result, state);
                                   },
                                   [this, &state](const typed::PluginInstallResponse& result) {
                                       state.pluginsAndSkills.pluginInstall = pluginMutationState(
                                           "install",
                                           std::nullopt,
                                           result.authPolicy.value,
                                           std::min(result.appsNeedingAuth.size(), MaxRetainedAppCatalogEntries),
                                           result.appsNeedingAuth.size() > MaxRetainedAppCatalogEntries,
                                           state,
                                           options);
                                   },
                                   [this, &state](const typed::PluginShareCheckoutResponse& result) {
                                       state.pluginsAndSkills.pluginShareCheckout = pluginMutationState(
                                           "share_checkout", result.remotePluginId, result.pluginName, 0, false, state, options);
                                   },
                                   [this, &state](const typed::PluginShareSaveResponse& result) {
                                       state.pluginsAndSkills.pluginShareSave =
                                           pluginMutationState("share_save", result.remotePluginId, "saved", 0, false, state, options);
                                   },
                                   [this, &state](const typed::PluginShareUpdateTargetsResponse& result) {
                                       state.pluginsAndSkills.pluginShareUpdateTargets = pluginMutationState(
                                           "share_update_targets",
                                           std::nullopt,
                                           result.discoverability.value,
                                           std::min(result.principals.size(), MaxRetainedAppCatalogEntries),
                                           result.principals.size() > MaxRetainedAppCatalogEntries,
                                           state,
                                           options);
                                   },
                                   [this, &state](const typed::SkillsConfigWriteResponse& result) {
                                       state.pluginsAndSkills.skillsConfigWrite = pluginMutationState("skills_config_write",
                                                                                                      std::nullopt,
                                                                                                      result.effectiveEnabled
                                                                                                          ? "enabled"
                                                                                                          : "disabled",
                                                                                                      0,
                                                                                                      false,
                                                                                                      state,
                                                                                                      options);
                                   },
                                   [this, &state](const typed::ListMcpServerStatusResponse& result) {
                                       McpStatusListState status;
                                       status.serverCount = result.data.size();
                                       status.nextCursor = result.nextCursor.hasValue()
                                                               ? std::optional<std::string>{boundedText(*result.nextCursor.value,
                                                                                                        options.maxNoticeDetailsBytes)}
                                                               : std::nullopt;
                                       status.complete = !result.nextCursor.hasValue();
                                       status.stamp = currentStamp(state);
                                       state.mcp.statusList = std::move(status);
                                   },
                                   [this, &state](const typed::WindowsSandboxReadinessResponse& result) {
                                       WindowsSandboxState windows = state.platform.windowsSandbox.value_or(WindowsSandboxState{});
                                       windows.lifecycle = "ready";
                                       windows.readiness = boundedText(result.status.value, MaxCanonicalIdentifierBytes);
                                       windows.stamp = currentStamp(state);
                                       state.platform.windowsSandbox = std::move(windows);
                                   },
                                   [this, &state, &insertions](const typed::ThreadForkResponse& result) {
                                       upsertThread(state, result.thread, EntityLoad::Full, options.maxAccumulatedItemBytes, &insertions);
                                   },
                                   [this, &state, &insertions](const typed::ThreadMetadataUpdateResponse& result) {
                                       upsertThread(state, result.thread, EntityLoad::Summary, options.maxAccumulatedItemBytes, &insertions);
                                   },
                                   [this, &state, &insertions](const typed::ThreadRollbackResponse& result) {
                                       upsertThread(state, result.thread, EntityLoad::Full, options.maxAccumulatedItemBytes, &insertions);
                                   },
                                   [this, &state, &insertions](const typed::ThreadUnarchiveResponse& result) {
                                       upsertThread(state, result.thread, EntityLoad::Summary, options.maxAccumulatedItemBytes, &insertions);
                                   },
                                   [this, &state, &insertions](const typed::ThreadListResponse& result) {
                                       for (const typed::Thread& thread : result.data) {
                                           upsertThread(state, thread, EntityLoad::Summary, options.maxAccumulatedItemBytes, &insertions);
                                       }
                                       state.threadList.hasLoadedPage = true;
                                       state.threadList.nextCursor = result.nextCursor.hasValue() ? result.nextCursor.value : std::nullopt;
                                       state.threadList.backwardsCursor =
                                           result.backwardsCursor.hasValue() ? result.backwardsCursor.value : std::nullopt;
                                       state.threadList.complete = !result.nextCursor.hasValue();
                                       state.threadList.stamp = currentStamp(state);
                                   },
                                   [this, &state, &insertions](const typed::TurnStartResponse& result) {
                                       upsertTurn(state, result.turn, options.maxAccumulatedItemBytes, &insertions);
                                   },
                                   [](const auto&) {
                                   }},
                        value.value);

                    std::visit(
                        Overloaded{[&state](const AccountLoginCancel& command) {
                                       if (state.accounts.login) {
                                           state.accounts.login->loginId = command.params.loginId;
                                       }
                                   },
                                   [&state](const AccountLoginStart& command) {
                                       if (state.accounts.login) {
                                           state.accounts.login->method = boundedText(
                                               typed::loginAccountParamsDiscriminator(command.params), MaxCanonicalIdentifierBytes);
                                       }
                                       state.accounts.loggedOut = false;
                                   },
                                   [&state](const AccountLogout&) {
                                       markOperationStale(state, "account/read");
                                       markOperationStale(state, "account/rateLimits/read");
                                       markOperationStale(state, "account/usage/read");
                                       markOperationStale(state, "account/workspaceMessages/read");
                                       AccountAuthenticationState authentication;
                                       authentication.stamp = currentStamp(state);
                                       state.accounts.authentication = std::move(authentication);
                                       AccountLoginFlowState login;
                                       login.stamp = currentStamp(state);
                                       state.accounts.login = std::move(login);
                                       state.accounts.loggedOut = true;
                                       state.accounts.logoutStamp = currentStamp(state);
                                   },
                                   [&state](const AccountRateLimitResetCreditConsume&) {
                                       markOperationStale(state, "account/rateLimits/read");
                                   },
                                   [&state](const ConfigBatchWrite&) {
                                       markOperationStale(state, "config/read");
                                   },
                                   [&state](const ConfigValueWrite&) {
                                       markOperationStale(state, "config/read");
                                   },
                                   [&state](const ConfigMcpServerReload&) {
                                       markOperationStale(state, "mcpServerStatus/list");
                                       markDomainStale(state.mcp);
                                       if (state.mcp.statusList) {
                                           state.mcp.statusList->stamp.freshness = Freshness::Stale;
                                       }
                                   },
                                   [&state](const ExperimentalFeatureEnablementSet&) {
                                       markOperationStale(state, "experimentalFeature/list");
                                   },
                                   [&state](const ThreadArchive& command) {
                                       markThreadStale(state, command.params.threadId);
                                       markOperationStale(state, "thread/list");
                                   },
                                   [&state, &value](const ThreadList& command) {
                                       if (const auto* result = std::get_if<typed::ThreadListResponse>(&value.value)) {
                                           const bool archived = command.params.archived.hasValue() && *command.params.archived.value;
                                           for (const typed::Thread& listed : result->data) {
                                               const auto thread = state.threads.find(listed.id.value);
                                               if (thread != state.threads.end()) {
                                                   thread->second.archived = archived;
                                               }
                                           }
                                       }
                                   },
                                   [&state](const ThreadCompactStart& command) {
                                       markThreadStale(state, command.params.threadId);
                                   },
                                   [&state](const ThreadDelete& command) {
                                       eraseThread(state, command.params.threadId.value);
                                       markOperationStale(state, "thread/list");
                                   },
                                   [&state, &value](const ThreadGoalClear& command) {
                                       if (value.method == "thread/goal/clear" &&
                                           std::holds_alternative<typed::ThreadGoalClearResponse>(value.value) &&
                                           state.conversations.latestGoalClear) {
                                           state.conversations.latestGoalClear->threadId = command.params.threadId.value;
                                       }
                                       markLatestGoalStale(state, command.params.threadId);
                                       markThreadStale(state, command.params.threadId);
                                   },
                                   [&state, &value](const ThreadGoalGet& command) {
                                       if (value.method == "thread/goal/get" &&
                                           std::holds_alternative<typed::ThreadGoalGetResponse>(value.value) &&
                                           state.conversations.latestGoal) {
                                           state.conversations.latestGoal->threadId = command.params.threadId.value;
                                       }
                                   },
                                   [&state, &value](const ThreadGoalSet& command) {
                                       if (value.method == "thread/goal/set" &&
                                           std::holds_alternative<typed::ThreadGoalSetResponse>(value.value) &&
                                           state.conversations.latestGoalSet) {
                                           state.conversations.latestGoalSet->threadId = command.params.threadId.value;
                                       }
                                       markLatestGoalStale(state, command.params.threadId);
                                       markThreadStale(state, command.params.threadId);
                                   },
                                   [&state](const ThreadInjectItems& command) {
                                       markThreadStale(state, command.params.threadId);
                                   },
                                   [&state, &insertions](const ThreadSetName& command) {
                                       ThreadState& thread = ensureThread(state, command.params.threadId, &insertions);
                                       thread.thread.name = typed::OptionalNullable<std::string>{command.params.name};
                                       thread.thread.title = command.params.name;
                                       thread.stamp = currentStamp(state);
                                   },
                                   [&state, &value](const ThreadUnsubscribe& command) {
                                       if (value.method == "thread/unsubscribe" &&
                                           std::holds_alternative<typed::ThreadUnsubscribeResponse>(value.value) &&
                                           state.conversations.latestUnsubscribe) {
                                           state.conversations.latestUnsubscribe->threadId = command.params.threadId.value;
                                       }
                                       markThreadStale(state, command.params.threadId);
                                   },
                                   [&state](const ThreadApproveGuardianDeniedAction& command) {
                                       markThreadStale(state, command.params.threadId);
                                   },
                                   [&state](const TurnSteer& command) {
                                       markTurnStale(state, command.params.threadId, command.params.expectedTurnId);
                                   },
                                   [&state, &value](const TurnStart& command) {
                                       const auto thread = state.threads.find(command.params.threadId.value);
                                       if (thread == state.threads.end()) {
                                           return;
                                       }
                                       const auto* result = std::get_if<typed::TurnStartResponse>(&value.value);
                                       if (result == nullptr) {
                                           return;
                                       }
                                       const auto turn = thread->second.turns.find(result->turn.id.value);
                                       if (turn == thread->second.turns.end()) {
                                           return;
                                       }
                                       turn->second.effectiveExecutionConfiguration =
                                           effectiveExecutionConfiguration(thread->second, command.params);
                                       if (turn->second.effectiveExecutionConfiguration) {
                                           turn->second.effectiveExecutionConfigurationProvenance = "turn_start_accepted";
                                       }
                                   },
                                   [&state, &value, this](const ReviewStart& command) {
                                       if (const auto* result = std::get_if<typed::ReviewStartResponse>(&value.value)) {
                                           retainActivityRecord(state,
                                                                result->reviewThreadId.value,
                                                                "review",
                                                                "started",
                                                                true,
                                                                value.method,
                                                                value.value.index(),
                                                                options,
                                                                "review started",
                                                                std::nullopt,
                                                                command.params.threadId,
                                                                result->turn.id);
                                       }
                                   },
                                   [&state, &value, this](const ExternalAgentConfigImport&) {
                                       markOperationStale(state, "externalAgentConfig/import/readHistories");
                                       if (const auto* result = std::get_if<typed::ExternalAgentConfigImportResponse>(&value.value)) {
                                           retainActivityRecord(state,
                                                                result->importId,
                                                                "external_agent_import",
                                                                "running",
                                                                true,
                                                                value.method,
                                                                value.value.index(),
                                                                options,
                                                                "external agent import");
                                       }
                                   },
                                   [&state, &value, this](const McpServerOauthLogin& command) {
                                       McpOauthState oauth;
                                       oauth.serverName = boundedText(command.params.name, MaxCanonicalIdentifierBytes);
                                       oauth.lifecycle = "waiting";
                                       oauth.threadId = command.params.threadId.hasValue() ? command.params.threadId.value : std::nullopt;
                                       oauth.stamp = currentStamp(state);
                                       state.mcp.oauth = oauth;
                                       retainActivityRecord(state,
                                                            command.params.name,
                                                            "mcp_oauth",
                                                            "waiting",
                                                            true,
                                                            value.method,
                                                            value.value.index(),
                                                            options,
                                                            "MCP OAuth",
                                                            std::nullopt,
                                                            oauth.threadId);
                                   },
                                   [&state](const CommandExecResize& command) {
                                       markProcessCurrent(state, command.params.processId, "running");
                                   },
                                   [&state](const CommandExecWrite& command) {
                                       markProcessCurrent(state,
                                                          command.params.processId,
                                                          command.params.closeStdin.value_or(false) ? "stdin_closed" : "running");
                                   },
                                   [&state](const CommandExecTerminate& command) {
                                       markProcessCurrent(state, command.params.processId, "terminate_requested");
                                   },
                                   [&state, &value, &reduction](const CommandExec& command) {
                                       std::string processId = value.resourceReservationKey.value_or(std::string{"command/exec"});
                                       if (command.params.processId.hasValue()) {
                                           processId = command.params.processId.value->value;
                                       }
                                       if (const auto* result = std::get_if<typed::CommandExecResponse>(&value.value)) {
                                           if (ProcessState* process = admitProcess(state, processId)) {
                                               process->lifecycle = "exited";
                                               process->exitCode = result->exitCode;
                                               process->stamp = currentStamp(state);
                                               process->connectionInvalidated = false;
                                               replaceProcessOutput(state, *process, process->stdoutData, result->stdoutData, false);
                                               replaceProcessOutput(state, *process, process->stderrData, result->stderrData, true);
                                           } else {
                                               reduction.providerCapacityFailure = true;
                                               reduction.providerCapacityFailureMessage =
                                                   "command process result exceeds backend process capacity";
                                           }
                                       }
                                   },
                                   [&state, &reduction, this](const FsWatch& command) {
                                       if (FilesystemWatchState* watch = admitFilesystemWatch(state, command.params.watchId)) {
                                           typed::AbsolutePath root = command.params.path;
                                           if (root.value.size() > options.maxNoticeDetailsBytes) {
                                               root.value.resize(options.maxNoticeDetailsBytes);
                                           }
                                           watch->root = std::move(root);
                                           watch->stamp = currentStamp(state);
                                           watch->connectionInvalidated = false;
                                       } else {
                                           reduction.providerCapacityFailure = true;
                                           reduction.providerCapacityFailureMessage =
                                               "filesystem watch result exceeds backend watch capacity";
                                       }
                                   },
                                   [&state](const FsUnwatch& command) {
                                       if (state.filesystemWatches.erase(command.params.watchId.value) != 0) {
                                           forgetResourceReservationClaim(
                                               state, ProviderResourceKind::FilesystemWatch, command.params.watchId.value);
                                           std::erase(state.filesystemWatchOrder, command.params.watchId.value);
                                           guardedSubtractSize(state.capacity.retainedFilesystemWatches, 1);
                                       }
                                   },
                                   [&state, &value, &reduction, this](const FuzzyFileSearch& command) {
                                       std::string sessionId;
                                       if (command.params.cancellationToken.hasValue()) {
                                           sessionId = *command.params.cancellationToken.value;
                                       } else {
                                           sessionId = value.resourceReservationKey.value_or(std::string{"fuzzyFileSearch"});
                                       }
                                       if (FuzzySearchState* search = admitFuzzySearch(state, sessionId)) {
                                           search->query = command.params.query;
                                           if (search->query.size() > options.maxNoticeDetailsBytes) {
                                               search->query.resize(options.maxNoticeDetailsBytes);
                                           }
                                           if (const auto* result = std::get_if<typed::FuzzyFileSearchResponse>(&value.value)) {
                                               search->files.assign(result->files.begin(),
                                                                    result->files.begin() +
                                                                        static_cast<std::ptrdiff_t>(std::min(
                                                                            result->files.size(), MaxRetainedFuzzyResultsPerSession)));
                                               for (typed::FuzzyFileSearchResult& file : search->files) {
                                                   boundFuzzyResult(file, options.maxNoticeDetailsBytes);
                                               }
                                           }
                                           search->complete = true;
                                           search->stamp = currentStamp(state);
                                           search->connectionInvalidated = false;
                                       } else {
                                           reduction.providerCapacityFailure = true;
                                           reduction.providerCapacityFailureMessage = "fuzzy-search result exceeds backend search capacity";
                                       }
                                   },
                                   [&state](const MarketplaceAdd&) {
                                       markOperationStale(state, "plugin/installed");
                                       markOperationStale(state, "plugin/list");
                                   },
                                   [&state](const MarketplaceRemove&) {
                                       markOperationStale(state, "plugin/installed");
                                       markOperationStale(state, "plugin/list");
                                   },
                                   [&state](const MarketplaceUpgrade&) {
                                       markOperationStale(state, "plugin/installed");
                                       markOperationStale(state, "plugin/list");
                                   },
                                   [&state](const PluginInstall&) {
                                       markOperationStale(state, "plugin/installed");
                                       markOperationStale(state, "plugin/list");
                                   },
                                   [&state](const PluginShareCheckout&) {
                                       markOperationStale(state, "plugin/installed");
                                       markOperationStale(state, "plugin/list");
                                   },
                                   [&state](const PluginShareDelete&) {
                                       markOperationStale(state, "plugin/share/list");
                                   },
                                   [&state](const PluginShareSave&) {
                                       markOperationStale(state, "plugin/share/list");
                                   },
                                   [&state](const PluginShareUpdateTargets&) {
                                       markOperationStale(state, "plugin/share/list");
                                   },
                                   [&state](const PluginUninstall&) {
                                       markOperationStale(state, "plugin/installed");
                                       markOperationStale(state, "plugin/list");
                                       markOperationStale(state, "plugin/read");
                                   },
                                   [&state](const SkillsConfigWrite&) {
                                       markOperationStale(state, "skills/list");
                                   },
                                   [&state, this](const SkillsExtraRootsSet& command) {
                                       markOperationStale(state, "skills/list");
                                       SkillsExtraRootsState roots;
                                       roots.totalRoots = command.params.extraRoots.size();
                                       roots.truncated = command.params.extraRoots.size() > MaxRetainedAppCatalogEntries;
                                       roots.roots.reserve(std::min(command.params.extraRoots.size(), MaxRetainedAppCatalogEntries));
                                       for (const typed::AbsolutePath& source : command.params.extraRoots) {
                                           if (roots.roots.size() == MaxRetainedAppCatalogEntries) {
                                               break;
                                           }
                                           typed::AbsolutePath retained = source;
                                           retained.value = boundedText(retained.value, options.maxNoticeDetailsBytes);
                                           roots.roots.push_back(std::move(retained));
                                       }
                                       roots.stamp = currentStamp(state);
                                       state.pluginsAndSkills.extraRoots = std::move(roots);
                                   },
                                   [&state, &value, this](const WindowsSandboxSetupStart& command) {
                                       markOperationStale(state, "windowsSandbox/readiness");
                                       WindowsSandboxState windows = state.platform.windowsSandbox.value_or(WindowsSandboxState{});
                                       windows.lifecycle = "started";
                                       windows.mode = boundedText(command.params.mode.value, MaxCanonicalIdentifierBytes);
                                       if (const auto* result = std::get_if<typed::WindowsSandboxSetupStartResponse>(&value.value)) {
                                           windows.success = result->started;
                                           if (!result->started) {
                                               windows.lifecycle = "not_started";
                                           }
                                       }
                                       windows.stamp = currentStamp(state);
                                       state.platform.windowsSandbox = windows;
                                       retainActivityRecord(state,
                                                            "windows_sandbox",
                                                            "windows_sandbox_setup",
                                                            windows.lifecycle,
                                                            windows.lifecycle == "started",
                                                            value.method,
                                                            value.value.index(),
                                                            options,
                                                            windows.mode);
                                   },
                                   [](const auto&) {
                                   }},
                        value.command);
                    if (value.resourceReservationKey) {
                        if (std::holds_alternative<CommandExec>(value.command)) {
                            releaseResource(state, ProviderResourceKind::Process, *value.resourceReservationKey);
                        } else if (std::holds_alternative<FsWatch>(value.command)) {
                            releaseResource(state, ProviderResourceKind::FilesystemWatch, *value.resourceReservationKey);
                        } else if (std::holds_alternative<FuzzyFileSearch>(value.command)) {
                            releaseResource(state, ProviderResourceKind::FuzzySearch, *value.resourceReservationKey);
                        }
                    }
                    return reduction;
                },
                [](const ProviderOperationStateChanged&) {
                    // BackendCore emits this only after the exact operation
                    // event has already passed through this reducer.
                    return Reduction{};
                },
                [&state](const ProviderResourceAdmissionRequested& value) {
                    Reduction reduction{false, false};
                    reduction.resourceAdmission = reserveResource(state, value.kind, value.key, value.resourceId);
                    if (!*reduction.resourceAdmission) {
                        saturatingAdd(state.capacity.rejectedOperations);
                    }
                    reduction.changed = true;
                    return reduction;
                },
                [&state](const ProviderResourceAdmissionReleased& value) {
                    return Reduction{releaseResource(state, value.kind, value.key), false};
                },
                [this, &state, &insertions](const ThreadUpserted& value) {
                    return Reduction{upsertThread(state, value.thread, value.load, options.maxAccumulatedItemBytes, &insertions), false};
                },
                [this, &state, &insertions](const ThreadListUpdated& value) {
                    for (const typed::Thread& thread : value.page.data) {
                        upsertThread(state, thread, EntityLoad::Summary, options.maxAccumulatedItemBytes, &insertions);
                    }
                    const bool firstPageForGeneration = state.threadList.stamp.generation != state.provider.generation ||
                                                        state.threadList.stamp.freshness != Freshness::Current;
                    if (value.initialRefresh || firstPageForGeneration) {
                        state.threadList = {};
                    }
                    state.threadList.hasLoadedPage = true;
                    saturatingAddSize(state.threadList.pagesLoaded, 1);
                    state.threadList.nextCursor = value.page.nextCursor.hasValue() ? value.page.nextCursor.value : std::nullopt;
                    state.threadList.backwardsCursor =
                        value.page.backwardsCursor.hasValue() ? value.page.backwardsCursor.value : std::nullopt;
                    state.threadList.complete = !value.page.nextCursor.hasValue();
                    state.threadList.stamp = currentStamp(state);
                    return Reduction{true, false};
                },
                [&state, &insertions](const ThreadStatusUpdated& value) {
                    ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                    thread.thread.status = value.status;
                    thread.stamp = currentStamp(state);
                    if (thread.thread.raw.is_object() && thread.thread.raw.size() == 1 &&
                        thread.thread.raw.value("backendPlaceholder", false)) {
                        thread.thread.raw["backendPlaceholderStatusKnown"] = true;
                    }
                    return Reduction{true, false};
                },
                [this, &state, &insertions](const TurnUpserted& value) {
                    return Reduction{upsertTurn(state, value.turn, options.maxAccumulatedItemBytes, &insertions), false};
                },
                [this, &state, &insertions](const TurnCompleted& value) {
                    const TurnState* prior = findTurn(state, value.turn.threadId, value.turn.id);
                    const bool emitTerminal = prior != nullptr && !prior->terminal;
                    upsertTurn(state, value.turn, options.maxAccumulatedItemBytes, &insertions);
                    TurnState& turn = ensureTurn(state, value.turn.threadId, value.turn.id, &insertions);
                    turn.active = false;
                    turn.terminal = true;
                    if (emitTerminal) {
                        const bool cancelled = value.turn.status.value == "cancelled" || value.turn.status.value == "interrupted";
                        const char* outcome = value.turn.status.value == "failed" ? "failed" : (cancelled ? "cancelled" : "completed");
                        lifecycleLog().debug("turn {}: thread={} turn={}", outcome, value.turn.threadId.value, value.turn.id.value);
                    }
                    return Reduction{true, true};
                },
                [this, &state, &insertions](const TurnFailed& value) {
                    const TurnState* prior = findTurn(state, value.turn.threadId, value.turn.id);
                    const bool emitTerminal = prior != nullptr && !prior->terminal;
                    upsertTurn(state, value.turn, options.maxAccumulatedItemBytes, &insertions);
                    TurnState& turn = ensureTurn(state, value.turn.threadId, value.turn.id, &insertions);
                    turn.active = false;
                    turn.terminal = true;
                    turn.failure = value.error;
                    if (emitTerminal) {
                        lifecycleLog().debug("turn failed: thread={} turn={}", value.turn.threadId.value, value.turn.id.value);
                    }
                    return Reduction{true, true};
                },
                [&state, &insertions](const TurnErrorUpdated& value) {
                    TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                    turn.failure = value.error;
                    turn.stamp = currentStamp(state);
                    turn.connectionInvalidated = false;
                    if (!value.willRetry) {
                        turn.active = false;
                    }
                    return Reduction{true, !value.willRetry};
                },
                [this, &state, &insertions](const ItemUpserted& value) {
                    if (upsertItem(state,
                                   value.threadId,
                                   value.turnId,
                                   value.item,
                                   value.lifecycle,
                                   value.occurredAtMs,
                                   options.maxAccumulatedItemBytes,
                                   &insertions)) {
                        return Reduction{true, value.lifecycle == ItemLifecycle::Completed || value.lifecycle == ItemLifecycle::Failed};
                    }
                    retainExtension(state,
                                    {.method = "codex/item-without-id",
                                     .payload = std::visit(
                                         [](const auto& item) {
                                             using Item = std::decay_t<decltype(item)>;
                                             if constexpr (std::is_same_v<Item, typed::UnknownItem>) {
                                                 return item.raw;
                                             } else {
                                                 return item.metadata.raw;
                                             }
                                         },
                                         value.item),
                                     .decodingError = "typed item has no stable id",
                                     .originalMethodBytes = std::nullopt,
                                     .originalPayloadBytes = std::nullopt,
                                     .originalDecodingErrorBytes = std::nullopt,
                                     .diagnostic = std::nullopt,
                                     .originalDiagnosticBytes = std::nullopt},
                                    options.retainedExtensions,
                                    options.maxExtensionMethodBytes,
                                    options.maxExtensionBytes,
                                    options.maxExtensionDecodingErrorBytes);
                    return Reduction{true, value.lifecycle == ItemLifecycle::Completed};
                },
                [this, &state, &insertions](const ItemContentChanged& value) {
                    TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                    auto iterator = turn.items.find(value.itemId.value);
                    if (iterator == turn.items.end()) {
                        typed::UnknownItem placeholder;
                        placeholder.type = "backend.delta-placeholder";
                        placeholder.raw = Json::object({{"id", value.itemId.value}, {"backendPlaceholder", true}});
                        placeholder.metadata = {value.itemId, value.threadId, value.turnId};
                        upsertItem(state,
                                   value.threadId,
                                   value.turnId,
                                   typed::ThreadItem{std::move(placeholder)},
                                   ItemLifecycle::Started,
                                   std::nullopt,
                                   options.maxAccumulatedItemBytes,
                                   &insertions);
                        iterator = turn.items.find(value.itemId.value);
                    }
                    ItemState& item = iterator->second;
                    const std::size_t previousContentBytes = itemContentBytes(item);
                    item.stamp = currentStamp(state);
                    item.connectionInvalidated = false;
                    switch (value.kind) {
                        case ItemContentChanged::Kind::AgentText:
                            appendBounded(item.agentText,
                                          value.delta,
                                          options.maxAccumulatedItemBytes,
                                          item.droppedContentBytes,
                                          &item.agentTextDroppedContentBytes);
                            break;
                        case ItemContentChanged::Kind::ReasoningText:
                            appendBounded(item.reasoningText,
                                          value.delta,
                                          options.maxAccumulatedItemBytes,
                                          item.droppedContentBytes,
                                          &item.reasoningTextDroppedContentBytes);
                            break;
                        case ItemContentChanged::Kind::ReasoningSummary:
                            appendBounded(item.reasoningSummary,
                                          value.delta,
                                          options.maxAccumulatedItemBytes,
                                          item.droppedContentBytes,
                                          &item.reasoningSummaryDroppedContentBytes);
                            break;
                        case ItemContentChanged::Kind::CommandOutput:
                            appendBounded(item.commandOutput,
                                          value.delta,
                                          options.maxAccumulatedItemBytes,
                                          item.droppedContentBytes,
                                          &item.commandOutputDroppedContentBytes);
                            break;
                    }
                    replaceAccumulatedContent(state.capacity, previousContentBytes, itemContentBytes(item));
                    return Reduction{true, false};
                },
                [this, &state, &insertions](const FileChangeUpdated& value) {
                    ItemState* item = findItem(state, value.threadId, value.turnId, value.itemId);
                    if (!item) {
                        typed::UnknownItem placeholder;
                        placeholder.type = "fileChange";
                        placeholder.raw = Json::object({{"id", value.itemId.value}, {"changes", value.changes}});
                        placeholder.metadata = {value.itemId, value.threadId, value.turnId};
                        upsertItem(state,
                                   value.threadId,
                                   value.turnId,
                                   typed::ThreadItem{std::move(placeholder)},
                                   ItemLifecycle::Started,
                                   std::nullopt,
                                   options.maxAccumulatedItemBytes,
                                   &insertions);
                        item = findItem(state, value.threadId, value.turnId, value.itemId);
                    }
                    if (item) {
                        item->stamp = currentStamp(state);
                        item->connectionInvalidated = false;
                        if (auto* fileChange = std::get_if<typed::FileChangeThreadItem>(&item->item)) {
                            fileChange->metadata.raw["changes"] = value.changes;
                            if (std::optional<std::vector<typed::FileUpdateChange>> changes = decodeFileUpdateChanges(value.changes)) {
                                fileChange->changes = std::move(*changes);
                            }
                        }
                        item->extensions["fileChanges"] = value.changes;
                    }
                    return Reduction{true, false};
                },
                [&state, &insertions](const TokenUsageUpdated& value) {
                    TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                    turn.tokenUsage = value.usage;
                    turn.stamp = currentStamp(state);
                    turn.connectionInvalidated = false;
                    return Reduction{true, false};
                },
                [this, &state, &insertions](const ModelRerouted& value) {
                    TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                    turn.stamp = currentStamp(state);
                    turn.connectionInvalidated = false;
                    if (options.maxModelReroutesPerTurn != 0) {
                        turn.modelReroutes.push_back({value.from, value.to, value.reason});
                        if (turn.modelReroutes.size() > options.maxModelReroutesPerTurn) {
                            turn.modelReroutes.erase(
                                turn.modelReroutes.begin(),
                                turn.modelReroutes.begin() +
                                    static_cast<std::ptrdiff_t>(turn.modelReroutes.size() - options.maxModelReroutesPerTurn));
                        }
                    }
                    return Reduction{true, false};
                },
                [&state](const PendingRequestAdded& value) {
                    state.pendingRequests.insert_or_assign(value.pending.id, value.pending);
                    return Reduction{true, true};
                },
                [&state](const PendingRequestRemoved& value) {
                    return Reduction{state.pendingRequests.erase(value.id) != 0, true};
                },
                [&state](const ControllerChanged& value) {
                    const bool changed = state.controller != value.controller;
                    if (state.controller) {
                        const auto previous = state.sessions.find(*state.controller);
                        if (previous != state.sessions.end()) {
                            previous->second.role = SessionRole::Observer;
                        }
                    }
                    state.controller = value.controller;
                    if (state.controller) {
                        const auto current = state.sessions.find(*state.controller);
                        if (current != state.sessions.end()) {
                            current->second.role = SessionRole::Controller;
                        }
                    }
                    return Reduction{changed, true};
                },
                [&state](const SessionChanged& value) {
                    if (value.connected) {
                        state.sessions.insert_or_assign(value.id, ConnectedSessionState{value.id, value.role});
                    } else {
                        state.sessions.erase(value.id);
                        if (state.controller == value.id) {
                            state.controller.reset();
                        }
                    }
                    return Reduction{true, true};
                },
                [this, &state, &insertions](const CodexExtensionReceived& value) {
                    Reduction reduction;
                    if (value.typedEvent) {
                        reduction = applyTypedNotificationState(state, value.method, *value.typedEvent, options, insertions);
                    }
                    const bool resolved =
                        value.typedEvent && std::holds_alternative<typed::ServerRequestResolvedNotification>(*value.typedEvent);
                    const bool ignoredResolved = resolved && (!reduction.changed || !reduction.pendingRequestRemovals.empty());
                    if (!ignoredResolved) {
                        retainExtension(state,
                                        {.method = value.method,
                                         .payload = value.payload,
                                         .decodingError = value.decodingError,
                                         .originalMethodBytes = std::nullopt,
                                         .originalPayloadBytes = std::nullopt,
                                         .originalDecodingErrorBytes = std::nullopt,
                                         .diagnostic = value.diagnostic,
                                         .originalDiagnosticBytes = std::nullopt},
                                        options.retainedExtensions,
                                        options.maxExtensionMethodBytes,
                                        options.maxExtensionBytes,
                                        options.maxExtensionDecodingErrorBytes);
                        reduction.changed = true;
                    }
                    return reduction;
                }},
            event);
        reduction.canonicalStateRewritten = enforceRetentionCapacity(state, insertions);
        reduction.changed = reduction.changed || state.capacity != capacityBefore;
        const auto appendCapacityChange = [&reduction](CapacityMetric metric, std::uint64_t before, std::uint64_t after) {
            if (after > before) {
                reduction.capacityChanges.push_back({metric, after - before, reduction.canonicalStateRewritten});
            }
        };
        if (!std::holds_alternative<CapacityChanged>(event)) {
            appendCapacityChange(CapacityMetric::RejectedSessions, capacityBefore.rejectedSessions, state.capacity.rejectedSessions);
            appendCapacityChange(CapacityMetric::RejectedObservers, capacityBefore.rejectedObservers, state.capacity.rejectedObservers);
            appendCapacityChange(CapacityMetric::RejectedOperations, capacityBefore.rejectedOperations, state.capacity.rejectedOperations);
            appendCapacityChange(
                CapacityMetric::ProviderRequestOverflows, capacityBefore.providerRequestOverflows, state.capacity.providerRequestOverflows);
            appendCapacityChange(CapacityMetric::EvictedThreads, capacityBefore.evictedThreads, state.capacity.evictedThreads);
            appendCapacityChange(CapacityMetric::EvictedTurns, capacityBefore.evictedTurns, state.capacity.evictedTurns);
            appendCapacityChange(CapacityMetric::EvictedItems, capacityBefore.evictedItems, state.capacity.evictedItems);
            appendCapacityChange(
                CapacityMetric::DroppedContentBytes, capacityBefore.droppedContentBytes, state.capacity.droppedContentBytes);
            appendCapacityChange(CapacityMetric::SnapshotOmissions, capacityBefore.snapshotOmissions, state.capacity.snapshotOmissions);
            appendCapacityChange(CapacityMetric::EvictedNotices, capacityBefore.evictedNotices, state.capacity.evictedNotices);
            appendCapacityChange(CapacityMetric::EvictedProcesses, capacityBefore.evictedProcesses, state.capacity.evictedProcesses);
            appendCapacityChange(CapacityMetric::DroppedProcessOutputBytes,
                                 capacityBefore.droppedProcessOutputBytes,
                                 state.capacity.droppedProcessOutputBytes);
            appendCapacityChange(
                CapacityMetric::EvictedFilesystemWatches, capacityBefore.evictedFilesystemWatches, state.capacity.evictedFilesystemWatches);
            appendCapacityChange(CapacityMetric::EvictedFuzzySearchSessions,
                                 capacityBefore.evictedFuzzySearchSessions,
                                 state.capacity.evictedFuzzySearchSessions);
            appendCapacityChange(
                CapacityMetric::EvictedActivityRecords, capacityBefore.evictedActivityRecords, state.capacity.evictedActivityRecords);
        }
        return reduction;
    }

    std::vector<BackendEvent> Reducer::translate(const typed::Event& event) const {
        return std::visit(
            Overloaded{
                [](const typed::ThreadStarted& value) -> std::vector<BackendEvent> {
                    lifecycleLog().info("thread created: thread={}", value.thread.id.value);
                    return {ThreadUpserted{value.thread, EntityLoad::Summary}};
                },
                [](const typed::ThreadStatusChanged& value) -> std::vector<BackendEvent> {
                    return {ThreadStatusUpdated{value.threadId, value.status}};
                },
                [](const typed::TurnStarted& value) -> std::vector<BackendEvent> {
                    lifecycleLog().debug("turn started: thread={} turn={}", value.turn.threadId.value, value.turn.id.value);
                    return {TurnUpserted{value.turn}};
                },
                [](const typed::TurnCompleted& value) -> std::vector<BackendEvent> {
                    return {TurnCompleted{value.turn}};
                },
                [](const typed::TurnFailed& value) -> std::vector<BackendEvent> {
                    return {TurnFailed{value.turn, value.error}};
                },
                [](const typed::ItemStarted& value) -> std::vector<BackendEvent> {
                    const auto location = itemLocation(value.item);
                    if (!location) {
                        return {CodexExtensionReceived{"item/started", value.raw, "item event omitted threadId or turnId", std::nullopt}};
                    }
                    return {ItemUpserted{location->first, location->second, value.item, ItemLifecycle::Started, value.startedAtMs}};
                },
                [](const typed::ItemCompleted& value) -> std::vector<BackendEvent> {
                    const auto location = itemLocation(value.item);
                    if (!location) {
                        return {CodexExtensionReceived{"item/completed", value.raw, "item event omitted threadId or turnId", std::nullopt}};
                    }
                    return {ItemUpserted{location->first, location->second, value.item, ItemLifecycle::Completed, value.completedAtMs}};
                },
                [](const typed::AgentMessageDelta& value) -> std::vector<BackendEvent> {
                    return {ItemContentChanged{
                        value.threadId, value.turnId, value.itemId, ItemContentChanged::Kind::AgentText, value.text, std::nullopt}};
                },
                [](const typed::ReasoningDelta& value) -> std::vector<BackendEvent> {
                    return {ItemContentChanged{value.threadId,
                                               value.turnId,
                                               value.itemId,
                                               value.kind == typed::ReasoningDelta::Kind::Summary
                                                   ? ItemContentChanged::Kind::ReasoningSummary
                                                   : ItemContentChanged::Kind::ReasoningText,
                                               value.text,
                                               value.index}};
                },
                [](const typed::CommandOutputDelta& value) -> std::vector<BackendEvent> {
                    return {ItemContentChanged{
                        value.threadId, value.turnId, value.itemId, ItemContentChanged::Kind::CommandOutput, value.output, std::nullopt}};
                },
                [](const typed::FileChangeUpdated& value) -> std::vector<BackendEvent> {
                    return {FileChangeUpdated{value.threadId, value.turnId, value.itemId, value.changes}};
                },
                [](const typed::TokenUsageUpdated& value) -> std::vector<BackendEvent> {
                    return {TokenUsageUpdated{value.threadId, value.turnId, value.usage}};
                },
                [](const typed::TerminalInteractionNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::TerminalInteraction);
                },
                [](const typed::FileChangeOutputDeltaNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::FileChangeOutputDelta);
                },
                [](const typed::McpToolCallProgressNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::McpToolCallProgress);
                },
                [](const typed::PlanDeltaNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::PlanDelta);
                },
                [](const typed::ReasoningSummaryPartAddedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ReasoningSummaryPartAdded);
                },
                [](const typed::ThreadArchivedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadArchived);
                },
                [](const typed::ThreadClosedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadClosed);
                },
                [](const typed::ContextCompactedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ContextCompacted);
                },
                [](const typed::ThreadDeletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadDeleted);
                },
                [](const typed::ThreadGoalClearedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadGoalCleared);
                },
                [](const typed::ThreadGoalUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadGoalUpdated);
                },
                [](const typed::ThreadNameUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadNameUpdated);
                },
                [](const typed::ThreadRealtimeClosedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeClosed);
                },
                [](const typed::ThreadRealtimeErrorNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeError);
                },
                [](const typed::ThreadRealtimeItemAddedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeItemAdded);
                },
                [](const typed::ThreadRealtimeOutputAudioDeltaNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeOutputAudioDelta);
                },
                [](const typed::ThreadRealtimeSdpNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeSdp);
                },
                [](const typed::ThreadRealtimeStartedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeStarted);
                },
                [](const typed::ThreadRealtimeTranscriptDeltaNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeTranscriptDelta);
                },
                [](const typed::ThreadRealtimeTranscriptDoneNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeTranscriptDone);
                },
                [](const typed::ThreadSettingsUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadSettingsUpdated);
                },
                [](const typed::ThreadUnarchivedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadUnarchived);
                },
                [](const typed::TurnDiffUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::TurnDiffUpdated);
                },
                [](const typed::TurnModerationMetadataNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::TurnModerationMetadata);
                },
                [](const typed::TurnPlanUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::TurnPlanUpdated);
                },
                [](const typed::AccountLoginCompletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::AccountLoginCompleted);
                },
                [](const typed::AccountRateLimitsUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::AccountRateLimitsUpdated);
                },
                [](const typed::AccountUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::AccountUpdated);
                },
                [](const typed::ConfigWarningNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ConfigWarning);
                },
                [](const typed::ModelRerouted& value) -> std::vector<BackendEvent> {
                    return {ModelRerouted{value.threadId, value.turnId, value.from, value.to, value.reason}};
                },
                [](const typed::ModelSafetyBufferingUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ModelSafetyBufferingUpdated);
                },
                [](const typed::ModelVerificationNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ModelVerification);
                },
                [](const typed::TurnErrorEvent& value) -> std::vector<BackendEvent> {
                    return {TurnErrorUpdated{value.threadId, value.turnId, value.error, value.willRetry}};
                },
                [](const typed::UnknownEvent& value) -> std::vector<BackendEvent> {
                    return {detail::preserveUnmodeledTypedEvent({value.method,
                                                                 value.params,
                                                                 ::ai::openai::codex::detail::safeDecodeDiagnosticText(value.diagnostic),
                                                                 value.diagnostic})};
                },
                [](const typed::CommandExecOutputDeltaNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::CommandExecOutputDelta);
                },
                [](const typed::FsChangedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::FsChanged);
                },
                [](const typed::FuzzyFileSearchSessionCompletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::FuzzyFileSearchSessionCompleted);
                },
                [](const typed::FuzzyFileSearchSessionUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::FuzzyFileSearchSessionUpdated);
                },
                [](const typed::GuardianWarningNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::GuardianWarning);
                },
                [](const typed::ItemGuardianApprovalReviewCompletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ItemGuardianApprovalReviewCompleted);
                },
                [](const typed::ItemGuardianApprovalReviewStartedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ItemGuardianApprovalReviewStarted);
                },
                [](const typed::AppListUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::AppListUpdated);
                },
                [](const typed::ExternalAgentConfigImportCompletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ExternalAgentConfigImportCompleted);
                },
                [](const typed::ExternalAgentConfigImportProgressNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ExternalAgentConfigImportProgress);
                },
                [](const typed::HookCompletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::HookCompleted);
                },
                [](const typed::HookStartedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::HookStarted);
                },
                [](const typed::SkillsChangedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::SkillsChanged);
                },
                [](const typed::McpServerOauthLoginCompletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::McpServerOauthLoginCompleted);
                },
                [](const typed::McpServerStatusUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::McpServerStartupStatusUpdated);
                },
                [](const typed::DeprecationNoticeNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::DeprecationNotice);
                },
                [](const typed::ProcessExitedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ProcessExited);
                },
                [](const typed::ProcessOutputDeltaNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ProcessOutputDelta);
                },
                [](const typed::RemoteControlStatusChangedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::RemoteControlStatusChanged);
                },
                [](const typed::ServerRequestResolvedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ServerRequestResolved);
                },
                [](const typed::WarningNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::Warning);
                },
                [](const typed::WindowsWorldWritableWarningNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::WindowsWorldWritableWarning);
                },
                [](const typed::WindowsSandboxSetupCompletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::WindowsSandboxSetupCompleted);
                }},
            event);
    }

} // namespace ai::openai::codex::backend
