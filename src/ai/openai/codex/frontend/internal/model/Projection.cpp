/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/internal/model/Projection.h"

#include "ai/openai/codex/frontend/Messages.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <string_view>
#include <type_traits>

namespace ai::openai::codex::frontend::internal::model {
    namespace {
        constexpr std::string_view RedactedValue = "[redacted]";

        bool hasScope(const ProjectionContext& context, FrontendScope scope) noexcept {
            return std::find(context.principal.scopes.begin(), context.principal.scopes.end(), scope) != context.principal.scopes.end();
        }

        bool hasAllScopes(const ProjectionContext& context, std::span<const FrontendScope> scopes) noexcept {
            return std::all_of(scopes.begin(), scopes.end(), [&](FrontendScope scope) {
                return hasScope(context, scope);
            });
        }

        bool hasCapability(const ProjectionContext& context, FrontendCapability capability) noexcept {
            return std::find(context.selectedCapabilities.begin(), context.selectedCapabilities.end(), capability) !=
                   context.selectedCapabilities.end();
        }

        void normalize(std::vector<std::string>& paths) {
            std::sort(paths.begin(), paths.end());
            paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        }

        void normalize(ProjectionMetadata& metadata) {
            normalize(metadata.omittedPaths);
            normalize(metadata.redactedPaths);
            normalize(metadata.truncatedPaths);
            normalize(metadata.unavailablePaths);
            normalize(metadata.stalePaths);
            normalize(metadata.unknownPaths);
            normalize(metadata.absentPaths);
            normalize(metadata.nullPaths);
        }

        std::string escapePointer(std::string_view value) {
            std::string escaped;
            for (const char character : value) {
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

        std::optional<std::vector<std::string>> pointerTokens(std::string_view pointer) {
            if (pointer.empty()) {
                return std::vector<std::string>{};
            }
            if (!pointer.starts_with('/')) {
                return std::nullopt;
            }
            std::vector<std::string> tokens;
            std::size_t begin = 1;
            while (begin <= pointer.size()) {
                const std::size_t end = pointer.find('/', begin);
                const std::string_view encoded =
                    pointer.substr(begin, end == std::string_view::npos ? pointer.size() - begin : end - begin);
                std::string decoded;
                decoded.reserve(encoded.size());
                for (std::size_t index = 0; index < encoded.size(); ++index) {
                    if (encoded[index] != '~') {
                        decoded.push_back(encoded[index]);
                        continue;
                    }
                    if (index + 1 >= encoded.size() || (encoded[index + 1] != '0' && encoded[index + 1] != '1')) {
                        return std::nullopt;
                    }
                    decoded.push_back(encoded[++index] == '0' ? '~' : '/');
                }
                tokens.push_back(std::move(decoded));
                if (end == std::string_view::npos) {
                    break;
                }
                begin = end + 1;
            }
            return tokens;
        }

        void report(ProjectionMetadata& metadata, ProjectionAction action, const std::string& path) {
            switch (action) {
                case ProjectionAction::Omit:
                    metadata.omittedPaths.push_back(path);
                    break;
                case ProjectionAction::Redact:
                    metadata.redactedPaths.push_back(path);
                    break;
                case ProjectionAction::Truncate:
                    metadata.truncatedPaths.push_back(path);
                    break;
                case ProjectionAction::Unavailable:
                    metadata.unavailablePaths.push_back(path);
                    break;
            }
        }

        bool applyAt(
            Json& value, ProjectionAction action, std::size_t maximumStringBytes, ProjectionMetadata& metadata, const std::string& path) {
            switch (action) {
                case ProjectionAction::Omit:
                    report(metadata, action, path);
                    return true;
                case ProjectionAction::Redact:
                    value = RedactedValue;
                    report(metadata, action, path);
                    return false;
                case ProjectionAction::Unavailable:
                    value = nullptr;
                    report(metadata, action, path);
                    return false;
                case ProjectionAction::Truncate:
                    if (value.is_string() && value.get_ref<const std::string&>().size() > maximumStringBytes) {
                        value = value.get_ref<const std::string&>().substr(0, maximumStringBytes);
                        report(metadata, action, path);
                    }
                    return false;
            }
            return false;
        }

        bool parseArrayIndex(std::string_view token, std::size_t& value) noexcept {
            if (token.empty() || (token.size() > 1 && token.front() == '0')) {
                return false;
            }
            value = 0;
            const auto conversion = std::from_chars(token.data(), token.data() + token.size(), value);
            return conversion.ec == std::errc{} && conversion.ptr == token.data() + token.size();
        }

        bool applyRule(Json& value,
                       const std::vector<std::string>& tokens,
                       std::size_t index,
                       ProjectionAction action,
                       std::size_t maximumStringBytes,
                       ProjectionMetadata& metadata,
                       const std::string& path) {
            if (index == tokens.size()) {
                return applyAt(value, action, maximumStringBytes, metadata, path.empty() ? std::string{"/"} : path);
            }
            const std::string& token = tokens[index];
            if (token == "**") {
                if (applyRule(value, tokens, index + 1, action, maximumStringBytes, metadata, path)) {
                    return true;
                }
                if (value.is_object()) {
                    for (auto member = value.begin(); member != value.end();) {
                        const std::string childPath = path + "/" + escapePointer(member.key());
                        if (applyRule(member.value(), tokens, index, action, maximumStringBytes, metadata, childPath)) {
                            member = value.erase(member);
                        } else {
                            ++member;
                        }
                    }
                } else if (value.is_array()) {
                    for (std::size_t child = value.size(); child > 0; --child) {
                        const std::size_t actual = child - 1;
                        if (applyRule(
                                value[actual], tokens, index, action, maximumStringBytes, metadata, path + "/" + std::to_string(actual))) {
                            value.erase(value.begin() + static_cast<Json::difference_type>(actual));
                        }
                    }
                }
                return false;
            }
            if (value.is_object()) {
                if (token == "*") {
                    for (auto member = value.begin(); member != value.end();) {
                        const std::string childPath = path + "/" + escapePointer(member.key());
                        if (applyRule(member.value(), tokens, index + 1, action, maximumStringBytes, metadata, childPath)) {
                            member = value.erase(member);
                        } else {
                            ++member;
                        }
                    }
                } else if (auto member = value.find(token); member != value.end()) {
                    if (applyRule(
                            member.value(), tokens, index + 1, action, maximumStringBytes, metadata, path + "/" + escapePointer(token))) {
                        value.erase(member);
                    }
                }
            } else if (value.is_array()) {
                if (token == "*") {
                    for (std::size_t child = value.size(); child > 0; --child) {
                        const std::size_t actual = child - 1;
                        if (applyRule(value[actual],
                                      tokens,
                                      index + 1,
                                      action,
                                      maximumStringBytes,
                                      metadata,
                                      path + "/" + std::to_string(actual))) {
                            value.erase(value.begin() + static_cast<Json::difference_type>(actual));
                        }
                    }
                } else {
                    std::size_t child = 0;
                    if (parseArrayIndex(token, child) && child < value.size() &&
                        applyRule(
                            value[child], tokens, index + 1, action, maximumStringBytes, metadata, path + "/" + std::to_string(child))) {
                        value.erase(value.begin() + static_cast<Json::difference_type>(child));
                    }
                }
            }
            return false;
        }

        ProjectionRule omitNested(std::string path, FrontendScope scope) {
            return ProjectionRule{std::move(path), {scope}, std::nullopt, ProjectionAction::Omit, 0};
        }

        std::vector<ProjectionRule> boundedDomainRules() {
            return {
                omitNested("/**/aggregatedOutput", FrontendScope::CommandExecution),
                omitNested("/**/command", FrontendScope::CommandExecution),
                omitNested("/**/commandActions", FrontendScope::CommandExecution),
                omitNested("/**/processId", FrontendScope::CommandExecution),
                omitNested("/**/stderr", FrontendScope::CommandExecution),
                omitNested("/**/stdout", FrontendScope::CommandExecution),
                omitNested("/**/cwd", FrontendScope::FilesystemRead),
                omitNested("/**/path", FrontendScope::FilesystemRead),
                omitNested("/**/diff", FrontendScope::FilesystemWrite),
                omitNested("/**/patch", FrontendScope::FilesystemWrite),
                omitNested("/**/arguments", FrontendScope::McpInvoke),
                omitNested("/**/result", FrontendScope::McpInvoke),
                omitNested("/**/loginLifecycle", FrontendScope::AccountManagement),
                omitNested("/**/loginMethod", FrontendScope::AccountManagement),
                omitNested("/**/loginSucceeded", FrontendScope::AccountManagement),
                omitNested("/**/filePath", FrontendScope::FilesystemRead),
                omitNested("/**/writeStatus", FrontendScope::ConfigurationWrite),
                omitNested("/**/writeVersion", FrontendScope::ConfigurationWrite),
                omitNested("/**/writeOverridden", FrontendScope::ConfigurationWrite),
                omitNested("/**/marketplaceAddStatus", FrontendScope::ExtensionManagement),
                omitNested("/**/marketplaceRemoveStatus", FrontendScope::ExtensionManagement),
                omitNested("/**/marketplaceUpgradeStatus", FrontendScope::ExtensionManagement),
                omitNested("/**/lastPluginOperation", FrontendScope::ExtensionManagement),
                omitNested("/**/lastSkillsOperation", FrontendScope::ExtensionManagement),
                omitNested("/**/oauthStatus", FrontendScope::McpInvoke),
            };
        }

        void prefixPaths(std::vector<std::string>& destination, const std::vector<std::string>& source, std::string_view prefix) {
            for (const std::string& path : source) {
                destination.push_back(std::string(prefix) + (path == "/" ? std::string{} : path));
            }
        }

        void mergeMetadata(ProjectionMetadata& destination, const ProjectionMetadata& source, std::string_view prefix) {
            prefixPaths(destination.omittedPaths, source.omittedPaths, prefix);
            prefixPaths(destination.redactedPaths, source.redactedPaths, prefix);
            prefixPaths(destination.truncatedPaths, source.truncatedPaths, prefix);
            prefixPaths(destination.unavailablePaths, source.unavailablePaths, prefix);
        }

        void projectSafeDetail(SafeDetail& detail,
                               const ProjectionAuthority& authority,
                               const ProjectionContext& context,
                               ProjectionMetadata& metadata,
                               std::string_view prefix,
                               std::span<const ProjectionRule> rules) {
            auto projected = authority.projectDetail(detail, context, rules);
            if (!projected) {
                throw projected.error();
            }
            detail = std::move(projected).value().value;
            mergeMetadata(metadata, projected.value().metadata, prefix);
        }

        void addScopeProjection(SafeDetail& detail, const ProjectionMetadata& metadata) {
            Json extensions = detail.json();
            extensions["scopeProjection"] = Json{{"omittedFields", metadata.omittedPaths}, {"redactedFields", metadata.redactedPaths}};
            SafeDetailError error = SafeDetailError::None;
            auto bounded = SafeDetail::fromJson(std::move(extensions), &error);
            if (!bounded) {
                throw ProjectionError{
                    ProjectionErrorCode::UnsafeResult, "/event/scopeProjection", "projection metadata exceeded safe-detail bounds"};
            }
            detail = std::move(*bounded);
        }

        template <typename Domain>
        void omitDomain(Domain& domain, std::string path, ProjectionMetadata& metadata) {
            domain.state.information = InformationState::Omitted;
            domain.state.status.reset();
            domain.state.summary.reset();
            domain.state.nextCursor.reset();
            domain.state.itemCount.reset();
            domain.state.complete = false;
            domain.state.completeKnown = false;
            domain.state.latestResults.clear();
            domain.state.truncation = {};
            domain.state.safeDetails = {};
            domain.state.extensions = {};
            if constexpr (requires { domain.entries.clear(); }) {
                domain.entries.clear();
            }
            metadata.omittedPaths.push_back(std::move(path));
        }

        void filterDomain(DomainState& domain,
                          const ProjectionAuthority& authority,
                          const ProjectionContext& context,
                          ProjectionMetadata& metadata,
                          std::string_view prefix) {
            const std::vector<ProjectionRule> rules = boundedDomainRules();
            projectSafeDetail(domain.safeDetails, authority, context, metadata, prefix, rules);
            projectSafeDetail(domain.extensions, authority, context, metadata, prefix, rules);
            for (std::size_t index = 0; index < domain.latestResults.size(); ++index) {
                DomainResultSummary& result = domain.latestResults[index];
                projectSafeDetail(result.extensions,
                                  authority,
                                  context,
                                  metadata,
                                  std::string(prefix) + "/latestResults/" + std::to_string(index),
                                  rules);
            }
        }

        void omitResultIdentity(DomainState& domain,
                                const ProjectionContext& context,
                                FrontendScope scope,
                                bool omitCursor,
                                ProjectionMetadata& metadata,
                                std::string_view prefix) {
            if (hasScope(context, scope)) {
                return;
            }
            for (std::size_t index = 0; index < domain.latestResults.size(); ++index) {
                DomainResultSummary& result = domain.latestResults[index];
                const std::string resultPath = std::string(prefix) + "/latestResults/" + std::to_string(index);
                if (result.subjectId.has_value()) {
                    result.subjectId.reset();
                    metadata.omittedPaths.push_back(resultPath + "/subjectId");
                }
                if (omitCursor && result.nextCursor.has_value()) {
                    result.nextCursor.reset();
                    metadata.omittedPaths.push_back(resultPath + "/nextCursor");
                }
            }
        }

        void filterItem(ThreadItem& item,
                        const ProjectionAuthority& authority,
                        const ProjectionContext& context,
                        ProjectionMetadata& metadata,
                        std::string_view prefix) {
            std::visit(
                [&](auto& typed) {
                    ItemData& value = typed.value;
                    const ThreadItemKind kind = threadItemKind(item);
                    const bool commandOutputVisible =
                        kind == ThreadItemKind::CommandExecution
                            ? hasScope(context, FrontendScope::CommandExecution)
                            : (kind == ThreadItemKind::FileChange ? hasScope(context, FrontendScope::FilesystemWrite)
                                                                  : hasScope(context, FrontendScope::CommandExecution) &&
                                                                        hasScope(context, FrontendScope::FilesystemWrite));
                    if (!commandOutputVisible && value.commandOutput.has_value()) {
                        value.commandOutput.reset();
                        metadata.omittedPaths.push_back(std::string(prefix) + "/commandOutput");
                    }
                    if (!hasScope(context, FrontendScope::FilesystemRead) && value.location.has_value()) {
                        value.location.reset();
                        metadata.omittedPaths.push_back(std::string(prefix) + "/location");
                    }
                    const std::vector<ProjectionRule> rules = boundedDomainRules();
                    if (value.safeDetails.has_value()) {
                        projectSafeDetail(*value.safeDetails, authority, context, metadata, prefix, rules);
                    }
                    if (!hasScope(context, FrontendScope::SensitiveResponse) &&
                        (!value.extensions.empty() || !value.legacyExtensions.empty())) {
                        value.extensions = {};
                        value.legacyExtensions = {};
                        metadata.omittedPaths.push_back(std::string(prefix) + "/extensions");
                    } else {
                        projectSafeDetail(value.extensions, authority, context, metadata, prefix, rules);
                        projectSafeDetail(value.legacyExtensions, authority, context, metadata, std::string(prefix) + "/extensions", rules);
                    }
                },
                item);
        }

        void filterSnapshot(CanonicalSnapshot& projected, const ProjectionAuthority& authority, const ProjectionContext& context) {
            ProjectionMetadata& metadata = projected.projection;
            metadata.projectionStamp = context.continuityFingerprint;

            const std::vector<ProjectionRule> rules = boundedDomainRules();
            if (projected.provider.initialization.has_value()) {
                projectSafeDetail(*projected.provider.initialization, authority, context, metadata, "/provider/initialization", rules);
            }
            projectSafeDetail(projected.provider.extensions, authority, context, metadata, "/provider", rules);
            projectSafeDetail(projected.controller.safeDetails, authority, context, metadata, "/controller", rules);
            for (std::size_t index = 0; index < projected.sessions.size(); ++index) {
                projectSafeDetail(
                    projected.sessions[index].safeDetails, authority, context, metadata, "/sessions/" + std::to_string(index), rules);
            }
            projectSafeDetail(projected.threadList.safeDetails, authority, context, metadata, "/threadList", rules);
            for (std::size_t index = 0; index < projected.threads.size(); ++index) {
                projectSafeDetail(
                    projected.threads[index].safeDetails, authority, context, metadata, "/threads/" + std::to_string(index), rules);
                projectSafeDetail(projected.threads[index].legacyExtensions,
                                  authority,
                                  context,
                                  metadata,
                                  "/threads/" + std::to_string(index) + "/extensions",
                                  rules);
            }
            for (std::size_t index = 0; index < projected.turns.size(); ++index) {
                projectSafeDetail(
                    projected.turns[index].safeDetails, authority, context, metadata, "/turns/" + std::to_string(index), rules);
                projectSafeDetail(projected.turns[index].legacyExtensions,
                                  authority,
                                  context,
                                  metadata,
                                  "/turns/" + std::to_string(index) + "/extensions",
                                  rules);
            }
            for (std::size_t index = 0; index < projected.items.size(); ++index) {
                filterItem(projected.items[index], authority, context, metadata, "/items/" + std::to_string(index));
            }
            for (std::size_t index = 0; index < projected.pendingRequests.size(); ++index) {
                std::visit(
                    [&](auto& request) {
                        if (request.value.safeDetails.has_value()) {
                            projectSafeDetail(*request.value.safeDetails,
                                              authority,
                                              context,
                                              metadata,
                                              "/pendingRequests/" + std::to_string(index),
                                              rules);
                        }
                    },
                    projected.pendingRequests[index]);
            }

            filterDomain(projected.accounts.state, authority, context, metadata, "/accounts");
            omitResultIdentity(projected.accounts.state, context, FrontendScope::AccountManagement, false, metadata, "/accounts");
            filterDomain(projected.models.state, authority, context, metadata, "/models");
            filterDomain(projected.configuration.state, authority, context, metadata, "/configuration");
            omitResultIdentity(projected.configuration.state, context, FrontendScope::ConfigurationWrite, true, metadata, "/configuration");
            filterDomain(projected.permissionProfiles.state, authority, context, metadata, "/permissionProfiles");
            filterDomain(projected.reviews.state, authority, context, metadata, "/reviews");
            filterDomain(projected.apps.state, authority, context, metadata, "/apps");
            filterDomain(projected.externalAgents.state, authority, context, metadata, "/externalAgents");
            filterDomain(projected.hooks.state, authority, context, metadata, "/hooks");
            filterDomain(projected.marketplace.state, authority, context, metadata, "/marketplace");
            filterDomain(projected.plugins.state, authority, context, metadata, "/plugins");
            filterDomain(projected.skills.state, authority, context, metadata, "/skills");
            filterDomain(projected.mcp.state, authority, context, metadata, "/mcp");
            omitResultIdentity(projected.mcp.state, context, FrontendScope::McpInvoke, true, metadata, "/mcp");
            filterDomain(projected.windowsSandbox.state, authority, context, metadata, "/windowsSandbox");
            filterDomain(projected.platform.state, authority, context, metadata, "/platform");
            filterDomain(projected.remoteControl.state, authority, context, metadata, "/remoteControl");
            filterDomain(projected.integrations.state, authority, context, metadata, "/integrations");

            if (!hasScope(context, FrontendScope::CommandExecution)) {
                projected.processes.clear();
                metadata.omittedPaths.push_back("/processes");
            }
            if (!hasScope(context, FrontendScope::FilesystemRead)) {
                omitDomain(projected.filesystemWatches, "/filesystemWatches", metadata);
                omitDomain(projected.fuzzySearches, "/fuzzySearches", metadata);
            }
            normalize(metadata);
        }

        bool payloadVisible(ExpandedEventType family, const ProjectionContext& context) noexcept {
            switch (family) {
                case ExpandedEventType::ProcessUpdated:
                    return hasScope(context, FrontendScope::CommandExecution);
                case ExpandedEventType::FilesystemWatchUpdated:
                case ExpandedEventType::FuzzySearchUpdated:
                    return hasScope(context, FrontendScope::FilesystemRead);
                default:
                    return true;
            }
        }

        bool trustedTypedOccurrenceAuthority(const CanonicalOccurrence& occurrence) noexcept {
            if (occurrence.expandedPayloads().size() != 1) {
                return false;
            }
            const std::string_view source = occurrence.identity().sourceStamp.value();
            constexpr std::string_view BackendPrefix = "backend-event:";
            if (source.starts_with(BackendPrefix)) {
                const std::string_view sequence = source.substr(BackendPrefix.size());
                return !sequence.empty() && std::all_of(sequence.begin(), sequence.end(), [](char character) {
                    return character >= '0' && character <= '9';
                });
            }
            if (source != "server-core") {
                return false;
            }
            switch (occurrenceType(occurrence.expandedPayloads().front())) {
                case ExpandedEventType::ControllerUpdated:
                case ExpandedEventType::SessionsUpdated:
                    return true;
                default:
                    return false;
            }
        }
    } // namespace

    ProjectionOutcome<ProjectedDetail> ProjectionAuthority::projectDetail(const SafeDetail& detail,
                                                                          const ProjectionContext& context,
                                                                          std::span<const ProjectionRule> rules) const noexcept {
        try {
            Json projected = detail.json();
            ProjectionMetadata metadata;
            metadata.projectionStamp = context.continuityFingerprint;
            for (const ProjectionRule& rule : rules) {
                const auto tokens = pointerTokens(rule.jsonPointer);
                if (!tokens.has_value() || (rule.action == ProjectionAction::Truncate && rule.maximumStringBytes == 0)) {
                    return ProjectionError{ProjectionErrorCode::InvalidRule, rule.jsonPointer, "invalid projection rule"};
                }
                const bool authorized = hasAllScopes(context, rule.requiredScopes) &&
                                        (!rule.requiredCapability.has_value() || hasCapability(context, *rule.requiredCapability));
                if (authorized && (!rule.requiredScopes.empty() || rule.requiredCapability.has_value())) {
                    continue;
                }
                (void) applyRule(projected, *tokens, 0, rule.action, rule.maximumStringBytes, metadata, "");
            }
            SafeDetailError error = SafeDetailError::None;
            auto safe = SafeDetail::fromJson(std::move(projected), &error);
            if (!safe.has_value()) {
                return ProjectionError{
                    ProjectionErrorCode::UnsafeResult, "/", "projected detail violated the bounded safe-detail contract"};
            }
            normalize(metadata);
            return ProjectedDetail{std::move(*safe), std::move(metadata)};
        } catch (const std::exception& error) {
            return ProjectionError{ProjectionErrorCode::InvalidValue, "/", error.what()};
        } catch (...) {
            return ProjectionError{ProjectionErrorCode::InvalidValue, "/", "projection failed"};
        }
    }

    ProjectionOutcome<CanonicalSnapshot> ProjectionAuthority::projectSnapshot(const CanonicalSnapshot& snapshot,
                                                                              const ProjectionContext& context) const noexcept {
        try {
            if (!hasScope(context, FrontendScope::Observe)) {
                return ProjectionError{ProjectionErrorCode::InvalidValue, "/", "observe scope is required for state projection"};
            }
            CanonicalSnapshot projected = snapshot;
            filterSnapshot(projected, *this, context);
            if (!encodeSnapshot(projected)) {
                return ProjectionError{ProjectionErrorCode::UnsafeResult, "/", "typed projected snapshot is not encodable"};
            }
            return projected;
        } catch (const ProjectionError& error) {
            return error;
        } catch (const std::exception& error) {
            return ProjectionError{ProjectionErrorCode::InvalidValue, "/", error.what()};
        } catch (...) {
            return ProjectionError{ProjectionErrorCode::InvalidValue, "/", "snapshot projection failed"};
        }
    }

    ProjectionOutcome<std::optional<CanonicalOccurrence>>
    ProjectionAuthority::projectOccurrence(const CanonicalOccurrence& occurrence, const ProjectionContext& context) const noexcept {
        try {
            if (!hasScope(context, FrontendScope::Observe)) {
                return std::optional<CanonicalOccurrence>{};
            }
            if (!occurrence.expandedPayloads().empty() && !trustedTypedOccurrenceAuthority(occurrence)) {
                const auto authority =
                    std::find_if(generated::AllNotificationProjections.begin(),
                                 generated::AllNotificationProjections.end(),
                                 [&](const generated::ProjectionMetadata& metadata) {
                                     if (metadata.registryKey != occurrence.identity().sourceStamp.value() ||
                                         metadata.expandedMappings.size() != occurrence.expandedPayloads().size()) {
                                         return false;
                                     }
                                     for (std::size_t index = 0; index < metadata.expandedMappings.size(); ++index) {
                                         const auto family = expandedEventTypeFromString(metadata.expandedMappings[index]);
                                         if (!family.has_value() || *family != occurrenceType(occurrence.expandedPayloads()[index])) {
                                             return false;
                                         }
                                     }
                                     return true;
                                 });
                if (authority == generated::AllNotificationProjections.end()) {
                    return ProjectionError{ProjectionErrorCode::MissingGeneratedAuthority,
                                           "/event/sourceStamp",
                                           "typed occurrence source and expanded families lack generated authority"};
                }
                if (!hasAllScopes(context, authority->requiredScopes)) {
                    return std::optional<CanonicalOccurrence>{};
                }
            }
            std::vector<OccurrencePayload> payloads = occurrence.expandedPayloads();
            if (std::any_of(payloads.begin(), payloads.end(), [&](const OccurrencePayload& payload) {
                    return !payloadVisible(occurrenceType(payload), context);
                })) {
                // Equal-sequence groups are one semantic occurrence.  Suppress
                // the whole group when no complete authorized projection can
                // be emitted; never leak occurrence topology through legacy.
                return std::optional<CanonicalOccurrence>{};
            }
            const std::vector<ProjectionRule> rules = boundedDomainRules();
            std::vector<ProjectionMetadata> payloadMetadata(payloads.size());
            for (std::size_t payloadIndex = 0; payloadIndex < payloads.size(); ++payloadIndex) {
                OccurrencePayload& payload = payloads[payloadIndex];
                ProjectionMetadata& metadata = payloadMetadata[payloadIndex];
                std::visit(
                    [&](auto& update) {
                        using Update = std::decay_t<decltype(update)>;
                        projectSafeDetail(update.extensions, *this, context, metadata, "/event", rules);
                        if constexpr (std::is_same_v<Update, ProviderUpdatedOccurrence>) {
                            if (update.provider.initialization.has_value()) {
                                projectSafeDetail(
                                    *update.provider.initialization, *this, context, metadata, "/provider/initialization", rules);
                            }
                        } else if constexpr (std::is_same_v<Update, ControllerUpdatedOccurrence>) {
                            projectSafeDetail(update.controller.safeDetails, *this, context, metadata, "/controller", rules);
                        } else if constexpr (std::is_same_v<Update, ThreadUpsertedOccurrence>) {
                            projectSafeDetail(update.thread.safeDetails, *this, context, metadata, "/thread", rules);
                            projectSafeDetail(update.thread.legacyExtensions, *this, context, metadata, "/thread/extensions", rules);
                            for (std::size_t turnIndex = 0; turnIndex < update.turns.size(); ++turnIndex) {
                                TurnState& turn = update.turns[turnIndex];
                                const std::string path = "/thread/turns/" + std::to_string(turnIndex);
                                projectSafeDetail(turn.safeDetails, *this, context, metadata, path, rules);
                                projectSafeDetail(turn.legacyExtensions, *this, context, metadata, path + "/extensions", rules);
                            }
                            for (std::size_t itemIndex = 0; itemIndex < update.items.size(); ++itemIndex) {
                                filterItem(update.items[itemIndex], *this, context, metadata, "/thread/items/" + std::to_string(itemIndex));
                            }
                        } else if constexpr (std::is_same_v<Update, TurnUpsertedOccurrence>) {
                            projectSafeDetail(update.turn.safeDetails, *this, context, metadata, "/turn", rules);
                            projectSafeDetail(update.turn.legacyExtensions, *this, context, metadata, "/turn/extensions", rules);
                            for (std::size_t itemIndex = 0; itemIndex < update.items.size(); ++itemIndex) {
                                filterItem(update.items[itemIndex], *this, context, metadata, "/turn/items/" + std::to_string(itemIndex));
                            }
                        } else if constexpr (std::is_same_v<Update, ItemUpsertedOccurrence>) {
                            filterItem(update.item, *this, context, metadata, "/item");
                        } else if constexpr (std::is_same_v<Update, AccountUpdatedOccurrence>) {
                            filterDomain(update.account.state, *this, context, metadata, "/domain");
                            omitResultIdentity(update.account.state, context, FrontendScope::AccountManagement, false, metadata, "/domain");
                        } else if constexpr (std::is_same_v<Update, ModelsUpdatedOccurrence>) {
                            filterDomain(update.models.state, *this, context, metadata, "/domain");
                        } else if constexpr (std::is_same_v<Update, ConfigurationUpdatedOccurrence>) {
                            filterDomain(update.configuration.state, *this, context, metadata, "/domain");
                            omitResultIdentity(
                                update.configuration.state, context, FrontendScope::ConfigurationWrite, true, metadata, "/domain");
                        } else if constexpr (std::is_same_v<Update, ReviewsUpdatedOccurrence>) {
                            filterDomain(update.reviews.state, *this, context, metadata, "/domain");
                        } else if constexpr (std::is_same_v<Update, IntegrationsUpdatedOccurrence>) {
                            filterDomain(update.integrations.state, *this, context, metadata, "/domain");
                        } else if constexpr (std::is_same_v<Update, PluginsUpdatedOccurrence>) {
                            filterDomain(update.plugins.state, *this, context, metadata, "/domain");
                        } else if constexpr (std::is_same_v<Update, SkillsUpdatedOccurrence>) {
                            filterDomain(update.skills.state, *this, context, metadata, "/domain");
                        } else if constexpr (std::is_same_v<Update, McpUpdatedOccurrence>) {
                            filterDomain(update.mcp.state, *this, context, metadata, "/domain");
                            omitResultIdentity(update.mcp.state, context, FrontendScope::McpInvoke, true, metadata, "/domain");
                        } else if constexpr (std::is_same_v<Update, PlatformUpdatedOccurrence>) {
                            filterDomain(update.platform.state, *this, context, metadata, "/domain");
                        }
                    },
                    payload);
                normalize(metadata);
            }
            LegacyCompatibilityPayload legacy = occurrence.legacyCompatibility();
            ProjectionMetadata legacyMetadata;
            if (legacy.safeExtension.has_value()) {
                projectSafeDetail(legacy.safeExtension->params, *this, context, legacyMetadata, "/legacy/params", rules);
                projectSafeDetail(legacy.safeExtension->extensions, *this, context, legacyMetadata, "/legacy", rules);
            }
            normalize(legacyMetadata);
            if (hasCapability(context, FrontendCapability::ScopeProjectedState)) {
                const ProjectionMetadata& selectedLegacyMetadata =
                    legacy.kind == LegacyCompatibilityKind::CodexExtension || legacy.sourcePayloadIndex >= payloadMetadata.size()
                        ? legacyMetadata
                        : payloadMetadata[legacy.sourcePayloadIndex];
                addScopeProjection(legacy.extensions, selectedLegacyMetadata);
                for (std::size_t payloadIndex = 0; payloadIndex < payloads.size(); ++payloadIndex) {
                    std::visit(
                        [&](auto& update) {
                            addScopeProjection(update.extensions, payloadMetadata[payloadIndex]);
                        },
                        payloads[payloadIndex]);
                }
            }
            OccurrenceIdentity identity = occurrence.identity();
            identity.projectionStamp = context.continuityFingerprint;
            auto projected = makeOccurrenceGroup(std::move(identity), std::move(legacy), std::move(payloads));
            if (!projected) {
                return ProjectionError{ProjectionErrorCode::UnsafeResult, projected.error().path, projected.error().message};
            }
            return std::optional<CanonicalOccurrence>{std::move(projected).value()};
        } catch (const ProjectionError& error) {
            return error;
        } catch (const std::exception& error) {
            return ProjectionError{ProjectionErrorCode::InvalidValue, "/event", error.what()};
        } catch (...) {
            return ProjectionError{ProjectionErrorCode::InvalidValue, "/event", "occurrence projection failed"};
        }
    }

    bool
    ProjectionAuthority::methodAllowed(const ProjectionContext& context, generated::MethodId method, bool providerReady) const noexcept {
        const std::size_t index = static_cast<std::size_t>(method);
        if (index >= generated::AllMethods.size()) {
            return false;
        }
        const generated::MethodMetadata& metadata = generated::AllMethods[index];
        if (metadata.id != method || !metadata.currentlyImplemented || !hasAllScopes(context, metadata.requiredScopes) ||
            (metadata.controllerRequired && !context.controllerOwned) || (metadata.providerReadyRequired && !providerReady)) {
            return false;
        }
        return true;
    }

} // namespace ai::openai::codex::frontend::internal::model
