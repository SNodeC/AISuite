/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/detail/BackendProjectionBuilder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ai::openai::codex::frontend::detail {

    namespace {

        constexpr std::size_t MaximumIdentifierBytes = 1'024;
        constexpr std::size_t MaximumTextBytes = 16'384;
        constexpr std::size_t MaximumDetailMembers = 64;
        constexpr std::size_t MaximumDetailArrayItems = 64;
        constexpr std::size_t MaximumDomainResults = 128;

        template <typename Value>
        void saturatingAdd(Value& target, Value value) noexcept {
            target = value > std::numeric_limits<Value>::max() - target ? std::numeric_limits<Value>::max() : target + value;
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

        std::string boundedText(std::string_view value, std::size_t maximumBytes = MaximumTextBytes) {
            return std::string(value.substr(0, utf8PrefixLength(value, maximumBytes)));
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

        bool unsafeDetailName(std::string_view value) {
            const std::string normalized = normalizedName(value);
            return normalized.ends_with("token") || normalized.ends_with("secret") || normalized.ends_with("credential") ||
                   normalized.ends_with("credentials") || normalized.find("password") != std::string::npos ||
                   normalized.find("passphrase") != std::string::npos || normalized.find("authorization") != std::string::npos ||
                   normalized.find("privatekey") != std::string::npos || normalized.find("apikey") != std::string::npos ||
                   normalized == "authentication" || normalized == "cookie" || normalized == "setcookie" || normalized == "answer" ||
                   normalized == "answers" || normalized == "raw" || normalized == "rawjson" || normalized == "rawpayload" ||
                   normalized == "rawproviderjson" || normalized == "rawproviderenvelope" || normalized == "providerenvelope" ||
                   normalized == "occurrencetoken" || normalized == "requesttoken";
        }

        bool safeDetailScalar(const Json& value) noexcept {
            return value.is_null() || value.is_boolean() || value.is_number() || value.is_string();
        }

        Json boundedDetailScalar(const Json& value) {
            if (value.is_string()) {
                return boundedText(value.get_ref<const std::string&>());
            }
            return value;
        }

        Json safeDetailObject(const Json& value) {
            Json result = Json::object();
            if (!value.is_object()) {
                return result;
            }
            std::size_t retained = 0;
            for (auto iterator = value.begin(); iterator != value.end() && retained < MaximumDetailMembers; ++iterator) {
                if (unsafeDetailName(iterator.key())) {
                    continue;
                }
                if (safeDetailScalar(iterator.value())) {
                    result[boundedText(iterator.key(), 256)] = boundedDetailScalar(iterator.value());
                    ++retained;
                    continue;
                }
                if (!iterator.value().is_array()) {
                    continue;
                }
                Json array = Json::array();
                const std::size_t count = std::min(iterator.value().size(), MaximumDetailArrayItems);
                bool valid = true;
                for (std::size_t index = 0; index < count; ++index) {
                    if (!safeDetailScalar(iterator.value()[index])) {
                        valid = false;
                        break;
                    }
                    array.push_back(boundedDetailScalar(iterator.value()[index]));
                }
                if (valid) {
                    result[boundedText(iterator.key(), 256)] = std::move(array);
                    ++retained;
                }
            }
            return result;
        }

        std::string_view providerLifecycleName(backend::ProviderLifecycle lifecycle) noexcept {
            switch (lifecycle) {
                case backend::ProviderLifecycle::Stopped:
                    return "stopped";
                case backend::ProviderLifecycle::Starting:
                    return "starting";
                case backend::ProviderLifecycle::Initializing:
                    return "initializing";
                case backend::ProviderLifecycle::Ready:
                    return "ready";
                case backend::ProviderLifecycle::Stopping:
                    return "stopping";
                case backend::ProviderLifecycle::Failed:
                    return "failed";
                case backend::ProviderLifecycle::Recovering:
                    return "recovering";
            }
            return "failed";
        }

        std::string_view recoveryStatusName(backend::RecoveryStatus status) noexcept {
            switch (status) {
                case backend::RecoveryStatus::Idle:
                    return "idle";
                case backend::RecoveryStatus::Waiting:
                    return "waiting";
                case backend::RecoveryStatus::Exhausted:
                    return "exhausted";
            }
            return "exhausted";
        }

        std::string_view freshnessName(backend::Freshness freshness) noexcept {
            switch (freshness) {
                case backend::Freshness::Unknown:
                    return "unknown";
                case backend::Freshness::Current:
                    return "current";
                case backend::Freshness::Stale:
                    return "stale";
            }
            return "unknown";
        }

        std::string_view roleName(backend::SessionRole role) noexcept {
            return role == backend::SessionRole::Controller ? "controller" : "observer";
        }

        std::string_view noticeCategoryName(backend::NoticeCategory category) noexcept {
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

        Json stampJson(const backend::SourceStamp& stamp) {
            return Json{{"generation", stamp.generation}, {"freshness", freshnessName(stamp.freshness)}};
        }

        Json providerJson(const backend::ProviderSnapshot& provider) {
            Json result{{"lifecycle", providerLifecycleName(provider.lifecycle)},
                        {"generation", provider.generation},
                        {"desiredRunning", provider.desiredRunning},
                        {"recovery", {{"status", recoveryStatusName(provider.recovery.status)}, {"attempts", provider.recovery.attempts}}}};
            if (provider.recovery.delayMs.has_value()) {
                result["recovery"]["delayMs"] = *provider.recovery.delayMs;
            }
            if (provider.lastError.has_value()) {
                // Provider error text is intentionally not canonical journal
                // authority. Its stable classification remains useful.
                result["lastError"] = Json{{"category", boundedText(provider.lastError->category, 256)},
                                           {"code", provider.lastError->code},
                                           {"detailsOmitted", true}};
            }
            if (provider.initialization.has_value()) {
                result["initialization"] = Json{{"codexHome", boundedText(provider.initialization->codexHome)},
                                                {"platformFamily", boundedText(provider.initialization->platformFamily, 1'024)},
                                                {"platformOs", boundedText(provider.initialization->platformOs, 1'024)},
                                                {"userAgent", boundedText(provider.initialization->userAgent, 1'024)}};
            }
            return result;
        }

        Json controllerJson(const backend::Snapshot& snapshot) {
            Json result{{"present", snapshot.controller.has_value()}};
            if (snapshot.controller.has_value() && snapshot.controller->value() != 0) {
                result["controllerSessionId"] = std::to_string(snapshot.controller->value());
            }
            return result;
        }

        Json sessionsJson(const backend::Snapshot& snapshot) {
            Json result = Json::array();
            for (const backend::SessionSnapshot& session : snapshot.sessions) {
                if (session.id.value() != 0) {
                    result.push_back({{"sessionId", std::to_string(session.id.value())}, {"role", roleName(session.role)}});
                }
            }
            return result;
        }

        std::optional<ThreadItemKind> itemKind(const backend::ItemSnapshot& item) noexcept {
            const auto direct = threadItemKindFromString(item.type);
            if (direct.has_value()) {
                return direct;
            }
            if (item.type == "agent_message") {
                return ThreadItemKind::AgentMessage;
            }
            if (item.type == "user_message") {
                return ThreadItemKind::UserMessage;
            }
            if (item.type == "command_execution") {
                return ThreadItemKind::CommandExecution;
            }
            if (item.type == "file_change") {
                return ThreadItemKind::FileChange;
            }
            if (item.type == "web_search") {
                return ThreadItemKind::WebSearch;
            }
            if (item.type == "tool_call") {
                return item.data.is_object() && item.data.contains("server") ? ThreadItemKind::McpToolCall
                                                                             : ThreadItemKind::DynamicToolCall;
            }
            try {
                if (item.data.is_object()) {
                    const auto codexType = item.data.find("codexType");
                    if (codexType != item.data.end() && codexType->is_string()) {
                        return threadItemKindFromString(codexType->get_ref<const std::string&>());
                    }
                }
            } catch (...) {
            }
            return std::nullopt;
        }

        std::optional<Json>
        itemJson(const backend::ItemSnapshot& item, std::optional<std::string_view> threadId, std::optional<std::string_view> turnId) {
            const std::optional<ThreadItemKind> kind = itemKind(item);
            if (!kind.has_value() || item.id.empty()) {
                return std::nullopt;
            }
            Json result{{"id", boundedText(item.id, MaximumIdentifierBytes)},
                        {"type", toString(*kind)},
                        {"status", boundedText(item.status, 256)},
                        {"truncated", item.contentTruncated},
                        {"connectionInvalidated", item.connectionInvalidated},
                        {"generation", item.stamp.generation},
                        {"freshness", freshnessName(item.stamp.freshness)}};
            if (threadId.has_value() && !threadId->empty()) {
                result["threadId"] = boundedText(*threadId, MaximumIdentifierBytes);
            }
            if (turnId.has_value() && !turnId->empty()) {
                result["turnId"] = boundedText(*turnId, MaximumIdentifierBytes);
            }
            if (!item.agentText.empty()) {
                result["agentText"] = boundedText(item.agentText);
            }
            if (!item.reasoningText.empty()) {
                result["reasoningText"] = boundedText(item.reasoningText);
            }
            if (!item.reasoningSummary.empty()) {
                result["reasoningSummary"] = boundedText(item.reasoningSummary);
            }
            if (!item.commandOutput.empty()) {
                result["commandOutput"] = boundedText(item.commandOutput);
            }
            result["droppedContentBytes"] = item.droppedContentBytes;
            result["contentTruncated"] = item.contentTruncated;
            if (item.startedAtMs.has_value()) {
                result["startedAtMs"] = *item.startedAtMs;
            }
            if (item.completedAtMs.has_value()) {
                result["completedAtMs"] = *item.completedAtMs;
            }
            const Json details = safeDetailObject(item.data);
            if (!details.empty()) {
                result["data"] = details;
            }
            return std::optional<Json>{std::move(result)};
        }

        std::optional<Json> turnJson(const backend::TurnSnapshot& turn, bool includeItems) {
            if (turn.id.empty() || turn.threadId.empty()) {
                return std::nullopt;
            }
            Json result{{"id", boundedText(turn.id, MaximumIdentifierBytes)},
                        {"threadId", boundedText(turn.threadId, MaximumIdentifierBytes)},
                        {"status", boundedText(turn.status, 256)},
                        {"active", turn.active},
                        {"terminal", turn.terminal},
                        {"stamp", stampJson(turn.stamp)},
                        {"connectionInvalidated", turn.connectionInvalidated}};
            if (turn.failure.has_value()) {
                const Json failure = safeDetailObject(*turn.failure);
                if (!failure.empty()) {
                    result["failure"] = failure;
                }
            }
            if (turn.tokenUsage.has_value()) {
                const Json usage = safeDetailObject(*turn.tokenUsage);
                if (!usage.empty()) {
                    result["tokenUsage"] = usage;
                }
            }
            if (includeItems) {
                result["items"] = Json::array();
                for (const backend::ItemSnapshot& item : turn.items) {
                    if (const auto encoded = itemJson(item, turn.threadId, turn.id); encoded.has_value()) {
                        result["items"].push_back(*encoded);
                    }
                }
            }
            return std::optional<Json>{std::move(result)};
        }

        std::optional<Json> threadJson(const backend::ThreadSnapshot& thread, bool includeTurns) {
            if (thread.id.empty()) {
                return std::nullopt;
            }
            Json result{{"id", boundedText(thread.id, MaximumIdentifierBytes)},
                        {"fullyLoaded", thread.fullyLoaded},
                        {"stamp", stampJson(thread.stamp)}};
            const auto addString = [&result](const char* name, const std::optional<std::string>& value, std::size_t maximumBytes) {
                if (value.has_value()) {
                    result[name] = boundedText(*value, maximumBytes);
                }
            };
            addString("title", thread.title, MaximumTextBytes);
            addString("cwd", thread.cwd, MaximumTextBytes);
            addString("model", thread.model, MaximumIdentifierBytes);
            addString("modelProvider", thread.modelProvider, MaximumIdentifierBytes);
            addString("preview", thread.preview, MaximumTextBytes);
            addString("status", thread.status, 256);
            Json realtime{{"lifecycle", boundedText(thread.realtime.lifecycle, 256)},
                          {"transcript", boundedText(thread.realtime.transcript)},
                          {"itemCount", thread.realtime.itemCount},
                          {"receivedAudioBytes", thread.realtime.receivedAudioBytes},
                          {"droppedAudioBytes", thread.realtime.droppedAudioBytes},
                          {"transcriptTruncated", thread.realtime.transcriptTruncated}};
            if (thread.realtime.lastError.has_value()) {
                realtime["errorDetailsOmitted"] = true;
            }
            if (thread.realtime.sessionId.has_value()) {
                realtime["sessionId"] = boundedText(*thread.realtime.sessionId, MaximumIdentifierBytes);
            }
            if (thread.realtime.version.has_value()) {
                realtime["version"] = boundedText(*thread.realtime.version, MaximumIdentifierBytes);
            }
            if (thread.realtime.lastSdpBytes.has_value()) {
                realtime["lastSdpBytes"] = *thread.realtime.lastSdpBytes;
            }
            result["realtime"] = std::move(realtime);
            if (includeTurns) {
                result["turns"] = Json::array();
                for (const backend::TurnSnapshot& turn : thread.turns) {
                    if (const auto encoded = turnJson(turn, false); encoded.has_value()) {
                        result["turns"].push_back(*encoded);
                    }
                }
            }
            return std::optional<Json>{std::move(result)};
        }

        std::optional<PendingRequestKind> pendingKind(std::string_view type) noexcept {
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
            if (type == "apply_patch_approval") {
                return PendingRequestKind::ApplyPatchApproval;
            }
            if (type == "exec_command_approval") {
                return PendingRequestKind::ExecCommandApproval;
            }
            if (type == "permissions_approval") {
                return PendingRequestKind::PermissionsApproval;
            }
            if (type == "attestation") {
                return PendingRequestKind::Attestation;
            }
            if (type == "dynamic_tool_call") {
                return PendingRequestKind::DynamicToolCall;
            }
            if (type == "mcp_elicitation") {
                return PendingRequestKind::McpElicitation;
            }
            return std::nullopt;
        }

        std::optional<Json> pendingJson(const backend::PendingRequestSnapshot& pending) {
            const auto kind = pendingKind(pending.type);
            if (!kind.has_value() || pending.id.value() == 0) {
                return std::nullopt;
            }
            Json result{{"pendingRequestId", std::to_string(pending.id.value())}, {"kind", toString(*kind)}, {"truncated", false}};
            const auto addIdentifier = [&result](const char* name, const std::optional<std::string>& value) {
                if (value.has_value() && !value->empty()) {
                    result[name] = boundedText(*value, MaximumIdentifierBytes);
                }
            };
            addIdentifier("threadId", pending.threadId);
            addIdentifier("turnId", pending.turnId);
            addIdentifier("itemId", pending.itemId);
            Json details = safeDetailObject(pending.details);
            if (*kind == PendingRequestKind::UserInput && pending.details.is_object()) {
                bool truncated = false;
                if (const auto questions = pending.details.find("questions"); questions != pending.details.end() && questions->is_array()) {
                    Json encodedQuestions = Json::array();
                    const std::size_t questionCount = std::min(questions->size(), MaximumDetailArrayItems);
                    truncated = questionCount != questions->size();
                    for (std::size_t questionIndex = 0; questionIndex < questionCount; ++questionIndex) {
                        const Json& question = (*questions)[questionIndex];
                        if (!question.is_object()) {
                            truncated = true;
                            continue;
                        }
                        const auto id = question.find("id");
                        const auto header = question.find("header");
                        const auto prompt = question.find("prompt");
                        const auto allowsFreeText = question.find("allowsFreeText");
                        const auto secret = question.find("secret");
                        const auto options = question.find("options");
                        if (id == question.end() || !id->is_string() || header == question.end() || !header->is_string() ||
                            prompt == question.end() || !prompt->is_string() || allowsFreeText == question.end() ||
                            !allowsFreeText->is_boolean() || secret == question.end() || !secret->is_boolean() ||
                            options == question.end() || !options->is_array()) {
                            truncated = true;
                            continue;
                        }
                        Json encoded{{"id", boundedText(id->get_ref<const std::string&>(), MaximumIdentifierBytes)},
                                     {"header", boundedText(header->get_ref<const std::string&>(), MaximumTextBytes)},
                                     {"prompt", boundedText(prompt->get_ref<const std::string&>(), MaximumTextBytes)},
                                     {"allowsFreeText", allowsFreeText->get<bool>()},
                                     {"isSecret", secret->get<bool>()},
                                     {"options", Json::array()}};
                        const std::size_t optionCount = std::min(options->size(), MaximumDetailArrayItems);
                        truncated = truncated || optionCount != options->size();
                        for (std::size_t optionIndex = 0; optionIndex < optionCount; ++optionIndex) {
                            const Json& option = (*options)[optionIndex];
                            if (!option.is_object()) {
                                truncated = true;
                                continue;
                            }
                            const auto label = option.find("label");
                            const auto description = option.find("description");
                            if (label == option.end() || !label->is_string() || description == option.end() || !description->is_string()) {
                                truncated = true;
                                continue;
                            }
                            encoded["options"].push_back(
                                {{"label", boundedText(label->get_ref<const std::string&>(), MaximumTextBytes)},
                                 {"description", boundedText(description->get_ref<const std::string&>(), MaximumTextBytes)}});
                        }
                        encodedQuestions.push_back(std::move(encoded));
                    }
                    result["questions"] = std::move(encodedQuestions);
                }
                if (const auto resolution = pending.details.find("autoResolutionMs"); resolution != pending.details.end()) {
                    if (resolution->is_number_unsigned()) {
                        result["autoResolutionMs"] = resolution->get<std::uint64_t>();
                    } else if (resolution->is_number_integer() && resolution->get<std::int64_t>() >= 0) {
                        result["autoResolutionMs"] = static_cast<std::uint64_t>(resolution->get<std::int64_t>());
                    } else {
                        truncated = true;
                    }
                }
                details.erase("questions");
                details.erase("autoResolutionMs");
                result["truncated"] = truncated;
            }
            if (!details.empty()) {
                result["details"] = details;
            }
            if (pending.details.is_object() && pending.details.contains("summary")) {
                result["summary"] = "bounded request summary available";
            }
            return std::optional<Json>{std::move(result)};
        }

        backend::SourceStamp domainStamp(const backend::ProviderDomainSnapshot& domain) noexcept {
            if (!domain.latestResults.empty()) {
                return domain.latestResults.back().stamp;
            }
            if (!domain.latestNotifications.empty()) {
                return domain.latestNotifications.back().stamp;
            }
            return {};
        }

        Json domainJson(const backend::ProviderDomainSnapshot& domain, Json details = Json::object()) {
            Json result{{"stamp", stampJson(domainStamp(domain))}, {"latestResults", Json::array()}};
            if (!domain.latestResults.empty()) {
                const backend::ProviderResultSummarySnapshot& authoritative = domain.latestResults.back();
                result["status"] = boundedText(authoritative.status, 256);
                result["complete"] = authoritative.complete;
                result["itemCount"] = authoritative.itemCount;
                if (authoritative.nextCursor.has_value()) {
                    result["nextCursor"] = boundedText(*authoritative.nextCursor);
                }
            }
            const std::size_t start =
                domain.latestResults.size() > MaximumDomainResults ? domain.latestResults.size() - MaximumDomainResults : 0;
            for (std::size_t index = start; index < domain.latestResults.size(); ++index) {
                const backend::ProviderResultSummarySnapshot& summary = domain.latestResults[index];
                if (summary.method.empty()) {
                    continue;
                }
                Json encoded{{"method", boundedText(summary.method, MaximumIdentifierBytes)},
                             {"status", boundedText(summary.status, 256)},
                             {"itemCount", summary.itemCount},
                             {"complete", summary.complete},
                             {"stamp", stampJson(summary.stamp)}};
                if (summary.subjectId.has_value() && !summary.subjectId->empty()) {
                    encoded["subjectId"] = boundedText(*summary.subjectId, MaximumIdentifierBytes);
                }
                if (summary.nextCursor.has_value()) {
                    encoded["nextCursor"] = boundedText(*summary.nextCursor);
                }
                result["latestResults"].push_back(std::move(encoded));
            }
            details = safeDetailObject(details);
            details["notificationCount"] = domain.latestNotifications.size();
            Json methods = Json::array();
            const std::size_t count = std::min(domain.latestNotificationMethods.size(), MaximumDetailArrayItems);
            for (std::size_t index = 0; index < count; ++index) {
                methods.push_back(boundedText(domain.latestNotificationMethods[index], MaximumIdentifierBytes));
            }
            if (!methods.empty()) {
                details["latestNotificationMethods"] = std::move(methods);
            }
            result["details"] = std::move(details);
            result["truncation"] = Json{{"truncated", start != 0}, {"omittedEntries", start}, {"droppedBytes", 0}};
            return result;
        }

        Json accountDetails(const backend::AccountDomainSnapshot& accounts) {
            Json details{{"loggedOut", accounts.loggedOut}};
            if (accounts.login.has_value()) {
                details["loginLifecycle"] = boundedText(accounts.login->lifecycle, 256);
                details["loginMethod"] = boundedText(accounts.login->method, 256);
                if (accounts.login->success.has_value()) {
                    details["loginSucceeded"] = *accounts.login->success;
                }
            }
            if (accounts.authentication.has_value()) {
                details["authenticated"] = accounts.authentication->authenticated;
                if (accounts.authentication->accountType.has_value()) {
                    details["accountType"] = boundedText(*accounts.authentication->accountType, 256);
                }
                if (accounts.authentication->authMode.has_value()) {
                    details["authMode"] = boundedText(*accounts.authentication->authMode, 256);
                }
                if (accounts.authentication->planType.has_value()) {
                    details["planType"] = boundedText(*accounts.authentication->planType, 256);
                }
            }
            if (accounts.rateLimits.has_value()) {
                if (accounts.rateLimits->primaryUsedPercent.has_value()) {
                    details["primaryUsedPercent"] = *accounts.rateLimits->primaryUsedPercent;
                }
                if (accounts.rateLimits->secondaryUsedPercent.has_value()) {
                    details["secondaryUsedPercent"] = *accounts.rateLimits->secondaryUsedPercent;
                }
                if (accounts.rateLimits->hasCredits.has_value()) {
                    details["hasCredits"] = *accounts.rateLimits->hasCredits;
                }
            }
            return details;
        }

        Json configurationDetails(const backend::ConfigurationDomainSnapshot& configuration) {
            Json details = Json::object();
            if (configuration.lastWrite.has_value()) {
                details["filePath"] = boundedText(configuration.lastWrite->filePath);
                details["writeStatus"] = boundedText(configuration.lastWrite->status, 256);
                details["writeVersion"] = boundedText(configuration.lastWrite->version, 256);
                details["writeOverridden"] = configuration.lastWrite->overridden;
            }
            if (configuration.experimentalFeatureEnablement.has_value()) {
                details["featureCount"] = configuration.experimentalFeatureEnablement->totalEntries;
                details["featureListTruncated"] = configuration.experimentalFeatureEnablement->truncated;
            }
            return details;
        }

        Json integrationsDetails(const backend::IntegrationsDomainSnapshot& integrations) {
            Json details = Json::object();
            if (integrations.apps.has_value()) {
                details["appCount"] = integrations.apps->totalEntries;
                details["appListTruncated"] = integrations.apps->truncated;
            }
            if (integrations.marketplaceAdd.has_value()) {
                details["marketplaceAddStatus"] = boundedText(integrations.marketplaceAdd->operation, 256);
            }
            if (integrations.marketplaceRemove.has_value()) {
                details["marketplaceRemoveStatus"] = boundedText(integrations.marketplaceRemove->operation, 256);
            }
            if (integrations.marketplaceUpgrade.has_value()) {
                details["marketplaceUpgradeStatus"] = boundedText(integrations.marketplaceUpgrade->operation, 256);
            }
            return details;
        }

        Json pluginsDetails(const backend::PluginsAndSkillsDomainSnapshot& plugins) {
            Json details = Json::object();
            if (plugins.pluginInstall.has_value()) {
                details["lastPluginOperation"] = boundedText(plugins.pluginInstall->operation, 256);
            }
            if (plugins.skillsConfigWrite.has_value()) {
                details["lastSkillsOperation"] = boundedText(plugins.skillsConfigWrite->operation, 256);
            }
            if (plugins.extraRoots.has_value()) {
                details["extraRootCount"] = plugins.extraRoots->roots.size();
                details["extraRootsTruncated"] = plugins.extraRoots->truncated;
            }
            return details;
        }

        Json mcpDetails(const backend::McpDomainSnapshot& mcp) {
            Json details = Json::object();
            if (mcp.oauth.has_value()) {
                details["oauthStatus"] = boundedText(mcp.oauth->lifecycle, 256);
            }
            if (mcp.startup.has_value()) {
                details["startupStatus"] = boundedText(mcp.startup->status, 256);
            }
            if (mcp.statusList.has_value()) {
                details["serverCount"] = mcp.statusList->serverCount;
                details["statusListComplete"] = mcp.statusList->complete;
            }
            return details;
        }

        Json platformDetails(const backend::PlatformDomainSnapshot& platform) {
            Json details = Json::object();
            if (platform.remoteControl.has_value()) {
                details["remoteControlStatus"] = boundedText(platform.remoteControl->status, 256);
            }
            if (platform.windowsSandbox.has_value()) {
                details["windowsSandboxStatus"] = boundedText(platform.windowsSandbox->lifecycle, 256);
            }
            return details;
        }

        Json capacityJson(const backend::Snapshot& snapshot) {
            const backend::CapacitySnapshot& capacity = snapshot.capacity;
            const std::size_t observers = static_cast<std::size_t>(
                std::count_if(snapshot.sessions.begin(), snapshot.sessions.end(), [](const backend::SessionSnapshot& session) {
                    return session.role == backend::SessionRole::Observer;
                }));
            return Json{{"sessions", snapshot.sessions.size()},
                        {"observers", observers},
                        {"pendingRequests", snapshot.pendingRequests.size()},
                        {"retainedThreads", capacity.retainedThreads},
                        {"retainedTurns", capacity.retainedTurns},
                        {"retainedItems", capacity.retainedItems},
                        {"accumulatedContentBytes", capacity.accumulatedContentBytes},
                        {"retainedNotices", capacity.retainedNotices},
                        {"retainedProcesses", capacity.retainedProcesses},
                        {"accumulatedProcessOutputBytes", capacity.accumulatedProcessOutputBytes},
                        {"retainedFilesystemWatches", capacity.retainedFilesystemWatches},
                        {"retainedFuzzySearchSessions", capacity.retainedFuzzySearchSessions},
                        {"retainedActivityRecords", capacity.retainedActivityRecords},
                        {"evictedNotices", capacity.state.evictedNotices},
                        {"evictedProcesses", capacity.state.evictedProcesses},
                        {"droppedProcessOutputBytes", capacity.state.droppedProcessOutputBytes},
                        {"evictedFilesystemWatches", capacity.state.evictedFilesystemWatches},
                        {"evictedFuzzySearchSessions", capacity.state.evictedFuzzySearchSessions},
                        {"evictedActivityRecords", capacity.state.evictedActivityRecords}};
        }

        Json truncationJson(const backend::Snapshot& snapshot, std::size_t locallyOmitted = 0) {
            std::size_t omitted = locallyOmitted;
            saturatingAdd(omitted, snapshot.capacity.omittedThreads);
            saturatingAdd(omitted, snapshot.capacity.omittedTurns);
            saturatingAdd(omitted, snapshot.capacity.omittedItems);
            saturatingAdd(omitted, snapshot.omittedRecentExtensions);
            std::uint64_t dropped = snapshot.capacity.state.droppedContentBytes;
            saturatingAdd(dropped, snapshot.capacity.state.droppedProcessOutputBytes);
            return Json{{"truncated", snapshot.capacity.truncated || omitted != 0 || dropped != 0},
                        {"omittedEntries", omitted},
                        {"droppedBytes", dropped}};
        }

        Json processJson(const backend::ProcessSnapshot& process) {
            Json result{{"processHandle", boundedText(process.processHandle, MaximumIdentifierBytes)},
                        {"lifecycle", boundedText(process.lifecycle, 256)},
                        {"stdoutBytes", process.stdoutBytes},
                        {"stderrBytes", process.stderrBytes},
                        {"stdoutTruncated", process.stdoutTruncated},
                        {"stderrTruncated", process.stderrTruncated},
                        {"droppedOutputBytes", process.droppedOutputBytes},
                        {"stamp", stampJson(process.stamp)},
                        {"connectionInvalidated", process.connectionInvalidated}};
            if (process.exitCode.has_value()) {
                result["exitCode"] = *process.exitCode;
            }
            return result;
        }

        Json filesystemWatchJson(const backend::FilesystemWatchSnapshot& watch) {
            Json result{{"watchId", boundedText(watch.watchId, MaximumIdentifierBytes)},
                        {"changedPathCount", watch.changedPathCount},
                        {"stamp", stampJson(watch.stamp)},
                        {"connectionInvalidated", watch.connectionInvalidated}};
            if (watch.root.has_value()) {
                result["root"] = boundedText(*watch.root);
            }
            return result;
        }

        Json fuzzySearchJson(const backend::FuzzySearchSnapshot& search) {
            return Json{{"sessionId", boundedText(search.sessionId, MaximumIdentifierBytes)},
                        {"resultCount", search.resultCount},
                        {"complete", search.complete},
                        {"stamp", stampJson(search.stamp)},
                        {"connectionInvalidated", search.connectionInvalidated}};
        }

        Json noticeJson(const backend::NoticeSnapshot& notice) {
            Json result{{"occurrence", notice.occurrence},
                        {"category", noticeCategoryName(notice.category)},
                        {"summary", boundedText(notice.summary)},
                        {"stamp", stampJson(notice.stamp)}};
            if (notice.details.has_value()) {
                result["details"] = boundedText(*notice.details);
            }
            if (notice.threadId.has_value() && !notice.threadId->empty()) {
                result["threadId"] = boundedText(*notice.threadId, MaximumIdentifierBytes);
            }
            return result;
        }

        Json activityJson(const backend::ActivitySnapshot& activity) {
            Json result{{"key", boundedText(activity.key, MaximumIdentifierBytes)},
                        {"subjectId", boundedText(activity.subjectId, MaximumIdentifierBytes)},
                        {"kind", boundedText(activity.kind, 256)},
                        {"lifecycle", boundedText(activity.lifecycle, 256)},
                        {"active", activity.active},
                        {"stamp", stampJson(activity.stamp)}};
            if (activity.summary.has_value()) {
                result["summary"] = boundedText(*activity.summary);
            }
            if (activity.details.has_value()) {
                result["details"] = boundedText(*activity.details);
            }
            if (activity.threadId.has_value() && !activity.threadId->empty()) {
                result["threadId"] = boundedText(*activity.threadId, MaximumIdentifierBytes);
            }
            if (activity.turnId.has_value() && !activity.turnId->empty()) {
                result["turnId"] = boundedText(*activity.turnId, MaximumIdentifierBytes);
            }
            return result;
        }

        const generated::ProjectionMetadata* notificationForMethod(std::string_view method) noexcept;

        std::optional<FrontendScope> expandedFamilyScope(std::string_view family) noexcept {
            if (family == "account.updated") {
                return FrontendScope::AccountManagement;
            }
            if (family == "configuration.updated") {
                return FrontendScope::ConfigurationWrite;
            }
            if (family == "process.updated") {
                return FrontendScope::CommandExecution;
            }
            if (family == "filesystemWatch.updated" || family == "fuzzySearch.updated") {
                return FrontendScope::FilesystemRead;
            }
            if (family == "integrations.updated" || family == "plugins.updated" || family == "skills.updated") {
                return FrontendScope::ExtensionManagement;
            }
            if (family == "mcp.updated") {
                return FrontendScope::McpInvoke;
            }
            return std::nullopt;
        }

        std::vector<FrontendScope> legacyPayloadScopes(const generated::ProjectionMetadata& metadata) {
            std::vector<FrontendScope> scopes;
            const auto addScope = [&scopes](FrontendScope scope) {
                if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end()) {
                    scopes.push_back(scope);
                }
            };
            if (metadata.registryKey.find("item/commandExecution/") != std::string_view::npos ||
                metadata.registryKey.find("command/exec/") != std::string_view::npos ||
                metadata.registryKey.find("process/") != std::string_view::npos) {
                addScope(FrontendScope::CommandExecution);
            }
            if (metadata.registryKey.find("item/fileChange/") != std::string_view::npos) {
                addScope(FrontendScope::FilesystemWrite);
            }
            for (const std::string_view family : metadata.expandedMappings) {
                if (const auto scope = expandedFamilyScope(family); scope.has_value()) {
                    addScope(*scope);
                }
            }
            return scopes;
        }

        std::vector<ScopeProjectionRule> expandedSnapshotRules() {
            return {
                {"/provider/initialization/codexHome", {FrontendScope::FilesystemRead}, ScopeProjectionAction::Omit},
                // Pending-request presentation is the generated Observe
                // contract. Response submission remains independently gated
                // by Control + SensitiveResponse and controller ownership.
                {"/pendingRequests", {FrontendScope::Observe}, ScopeProjectionAction::Omit},
                {"/threads/*/cwd", {FrontendScope::FilesystemRead}, ScopeProjectionAction::Omit},
                {"/items/*/commandOutput", {FrontendScope::CommandExecution}, ScopeProjectionAction::Omit},
                {"/items/*/data/**/aggregatedOutput", {FrontendScope::CommandExecution}, ScopeProjectionAction::Omit},
                {"/items/*/data/**/command", {FrontendScope::CommandExecution}, ScopeProjectionAction::Omit},
                {"/items/*/data/**/commandActions", {FrontendScope::CommandExecution}, ScopeProjectionAction::Omit},
                {"/items/*/data/**/processId", {FrontendScope::CommandExecution}, ScopeProjectionAction::Omit},
                {"/items/*/data/**/stderr", {FrontendScope::CommandExecution}, ScopeProjectionAction::Omit},
                {"/items/*/data/**/stdout", {FrontendScope::CommandExecution}, ScopeProjectionAction::Omit},
                {"/items/*/data/**/cwd", {FrontendScope::FilesystemRead}, ScopeProjectionAction::Omit},
                {"/items/*/data/**/path", {FrontendScope::FilesystemRead}, ScopeProjectionAction::Omit},
                {"/items/*/data/**/patch", {FrontendScope::FilesystemWrite}, ScopeProjectionAction::Omit},
                {"/items/*/data/**/diff", {FrontendScope::FilesystemWrite}, ScopeProjectionAction::Omit},
                {"/items/*/data/**/arguments", {FrontendScope::McpInvoke}, ScopeProjectionAction::Omit},
                {"/items/*/data/**/result", {FrontendScope::McpInvoke}, ScopeProjectionAction::Omit},
                {"/accounts/details/loginLifecycle", {FrontendScope::AccountManagement}, ScopeProjectionAction::Omit},
                {"/accounts/details/loginMethod", {FrontendScope::AccountManagement}, ScopeProjectionAction::Omit},
                {"/accounts/details/loginSucceeded", {FrontendScope::AccountManagement}, ScopeProjectionAction::Omit},
                {"/accounts/latestResults/*/subjectId", {FrontendScope::AccountManagement}, ScopeProjectionAction::Omit},
                {"/configuration/details/filePath", {FrontendScope::FilesystemRead}, ScopeProjectionAction::Omit},
                {"/configuration/details/writeStatus", {FrontendScope::ConfigurationWrite}, ScopeProjectionAction::Omit},
                {"/configuration/details/writeVersion", {FrontendScope::ConfigurationWrite}, ScopeProjectionAction::Omit},
                {"/configuration/details/writeOverridden", {FrontendScope::ConfigurationWrite}, ScopeProjectionAction::Omit},
                {"/configuration/latestResults/*/subjectId", {FrontendScope::ConfigurationWrite}, ScopeProjectionAction::Omit},
                {"/configuration/latestResults/*/nextCursor", {FrontendScope::ConfigurationWrite}, ScopeProjectionAction::Omit},
                {"/processes", {FrontendScope::CommandExecution}, ScopeProjectionAction::Omit},
                {"/filesystemWatches", {FrontendScope::FilesystemRead}, ScopeProjectionAction::Omit},
                {"/fuzzySearches", {FrontendScope::FilesystemRead}, ScopeProjectionAction::Omit},
                {"/apps/details/marketplaceAddStatus", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                {"/apps/details/marketplaceRemoveStatus", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                {"/apps/details/marketplaceUpgradeStatus", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                {"/marketplace/details/marketplaceAddStatus", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                {"/marketplace/details/marketplaceRemoveStatus", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                {"/marketplace/details/marketplaceUpgradeStatus", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                {"/plugins/details/lastPluginOperation", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                {"/plugins/details/lastSkillsOperation", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                {"/skills/details/lastPluginOperation", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                {"/skills/details/lastSkillsOperation", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                {"/mcp/details/oauthStatus", {FrontendScope::McpInvoke}, ScopeProjectionAction::Omit},
                {"/mcp/latestResults/*/subjectId", {FrontendScope::McpInvoke}, ScopeProjectionAction::Omit},
                {"/mcp/latestResults/*/nextCursor", {FrontendScope::McpInvoke}, ScopeProjectionAction::Omit},
            };
        }

        void appendItemScopeRules(std::vector<ScopeProjectionRule>& rules, std::string_view prefix) {
            const auto append = [&rules, prefix](std::string_view suffix, FrontendScope scope) {
                rules.push_back({std::string(prefix) + std::string(suffix), {scope}, ScopeProjectionAction::Omit});
            };
            append("/commandOutput", FrontendScope::CommandExecution);
            append("/data/**/aggregatedOutput", FrontendScope::CommandExecution);
            append("/data/**/command", FrontendScope::CommandExecution);
            append("/data/**/commandActions", FrontendScope::CommandExecution);
            append("/data/**/processId", FrontendScope::CommandExecution);
            append("/data/**/stderr", FrontendScope::CommandExecution);
            append("/data/**/stdout", FrontendScope::CommandExecution);
            append("/data/**/cwd", FrontendScope::FilesystemRead);
            append("/data/**/path", FrontendScope::FilesystemRead);
            append("/data/**/diff", FrontendScope::FilesystemWrite);
            append("/data/**/patch", FrontendScope::FilesystemWrite);
            append("/data/**/arguments", FrontendScope::McpInvoke);
            append("/data/**/result", FrontendScope::McpInvoke);
            append("/extensions", FrontendScope::SensitiveResponse);
        }

        void appendTurnScopeRules(std::vector<ScopeProjectionRule>& rules, std::string_view prefix) {
            rules.push_back({std::string(prefix) + "/extensions", {FrontendScope::SensitiveResponse}, ScopeProjectionAction::Omit});
            appendItemScopeRules(rules, std::string(prefix) + "/items/*");
        }

        void appendThreadScopeRules(std::vector<ScopeProjectionRule>& rules, std::string_view prefix) {
            rules.push_back({std::string(prefix) + "/cwd", {FrontendScope::FilesystemRead}, ScopeProjectionAction::Omit});
            rules.push_back({std::string(prefix) + "/extensions", {FrontendScope::SensitiveResponse}, ScopeProjectionAction::Omit});
            appendTurnScopeRules(rules, std::string(prefix) + "/turns/*");
        }

        std::vector<ScopeProjectionRule> legacyRules(const Json& legacyState) {
            std::vector<ScopeProjectionRule> rules{
                {"/pendingRequests", {FrontendScope::Observe}, ScopeProjectionAction::Omit},
                {"/codexExtensions", {FrontendScope::Observe}, ScopeProjectionAction::Omit},
            };
            appendThreadScopeRules(rules, "/threads/*");
            if (!legacyState.is_object()) {
                return rules;
            }
            const auto extensions = legacyState.find("codexExtensions");
            if (extensions == legacyState.end() || !extensions->is_array()) {
                return rules;
            }
            for (std::size_t index = 0; index < extensions->size(); ++index) {
                const Json& extension = (*extensions)[index];
                if (!extension.is_object()) {
                    continue;
                }
                const auto method = extension.find("method");
                if (method == extension.end() || !method->is_string()) {
                    continue;
                }
                const generated::ProjectionMetadata* metadata = notificationForMethod(method->get_ref<const std::string&>());
                const std::vector<FrontendScope> scopes =
                    metadata == nullptr ? std::vector{FrontendScope::UnknownRequestResponse} : legacyPayloadScopes(*metadata);
                if (!scopes.empty()) {
                    const bool hasUnprivilegedFamily =
                        metadata != nullptr &&
                        std::any_of(metadata->expandedMappings.begin(), metadata->expandedMappings.end(), [](std::string_view family) {
                            return !expandedFamilyScope(family).has_value();
                        });
                    const std::string path = "/codexExtensions/" + std::to_string(index) + (hasUnprivilegedFamily ? "/params" : "");
                    rules.push_back({path, scopes, ScopeProjectionAction::Omit});
                }
            }
            return rules;
        }

        std::vector<ScopeProjectionRule> legacyEventRules(std::string_view type) {
            std::vector<ScopeProjectionRule> rules;
            if (type == "thread.updated") {
                appendThreadScopeRules(rules, "/thread");
            } else if (type == "turn.updated") {
                appendTurnScopeRules(rules, "/turn");
            } else if (type == "item.updated") {
                appendItemScopeRules(rules, "/item");
            }
            return rules;
        }

        std::vector<ScopeProjectionRule> expandedEventRules(ExpandedEventType type) {
            switch (type) {
                case ExpandedEventType::ThreadUpserted:
                    return {{"/thread/cwd", {FrontendScope::FilesystemRead}, ScopeProjectionAction::Omit}};
                case ExpandedEventType::ItemUpserted: {
                    std::vector<ScopeProjectionRule> rules;
                    appendItemScopeRules(rules, "/item");
                    return rules;
                }
                case ExpandedEventType::AccountUpdated:
                    return {{"/domain/details/loginLifecycle", {FrontendScope::AccountManagement}, ScopeProjectionAction::Omit},
                            {"/domain/details/loginMethod", {FrontendScope::AccountManagement}, ScopeProjectionAction::Omit},
                            {"/domain/details/loginSucceeded", {FrontendScope::AccountManagement}, ScopeProjectionAction::Omit},
                            {"/domain/latestResults/*/subjectId", {FrontendScope::AccountManagement}, ScopeProjectionAction::Omit}};
                case ExpandedEventType::ConfigurationUpdated:
                    return {{"/domain/details/filePath", {FrontendScope::ConfigurationWrite}, ScopeProjectionAction::Omit},
                            {"/domain/details/writeStatus", {FrontendScope::ConfigurationWrite}, ScopeProjectionAction::Omit},
                            {"/domain/details/writeVersion", {FrontendScope::ConfigurationWrite}, ScopeProjectionAction::Omit},
                            {"/domain/details/writeOverridden", {FrontendScope::ConfigurationWrite}, ScopeProjectionAction::Omit},
                            {"/domain/latestResults/*/subjectId", {FrontendScope::ConfigurationWrite}, ScopeProjectionAction::Omit},
                            {"/domain/latestResults/*/nextCursor", {FrontendScope::ConfigurationWrite}, ScopeProjectionAction::Omit}};
                case ExpandedEventType::ProcessUpdated:
                    return {{"/process/processHandle", {FrontendScope::CommandExecution}, ScopeProjectionAction::Omit},
                            {"/process/stdout", {FrontendScope::CommandExecution}, ScopeProjectionAction::Omit},
                            {"/process/stderr", {FrontendScope::CommandExecution}, ScopeProjectionAction::Omit}};
                case ExpandedEventType::FilesystemWatchUpdated:
                    return {{"/filesystemWatch/watchId", {FrontendScope::FilesystemRead}, ScopeProjectionAction::Omit},
                            {"/filesystemWatch/root", {FrontendScope::FilesystemRead}, ScopeProjectionAction::Omit}};
                case ExpandedEventType::FuzzySearchUpdated:
                    return {{"/fuzzySearch/sessionId", {FrontendScope::FilesystemRead}, ScopeProjectionAction::Omit}};
                case ExpandedEventType::IntegrationsUpdated:
                    return {
                        {"/domain/details/marketplaceAddStatus", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                        {"/domain/details/marketplaceRemoveStatus", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                        {"/domain/details/marketplaceUpgradeStatus", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit}};
                case ExpandedEventType::PluginsUpdated:
                case ExpandedEventType::SkillsUpdated:
                    return {{"/domain/details/lastPluginOperation", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit},
                            {"/domain/details/lastSkillsOperation", {FrontendScope::ExtensionManagement}, ScopeProjectionAction::Omit}};
                case ExpandedEventType::McpUpdated:
                    return {{"/domain/details/oauthStatus", {FrontendScope::McpInvoke}, ScopeProjectionAction::Omit},
                            {"/domain/latestResults/*/subjectId", {FrontendScope::McpInvoke}, ScopeProjectionAction::Omit},
                            {"/domain/latestResults/*/nextCursor", {FrontendScope::McpInvoke}, ScopeProjectionAction::Omit}};
                default:
                    return {};
            }
        }

        void scrubLegacyErrorAndDiagnostics(Json& value) {
            try {
                if (!value.is_object()) {
                    return;
                }
                if (auto error = value.find("lastLifecycleError");
                    error != value.end() && error->is_object() && error->contains("message")) {
                    (*error)["message"] = "provider error details omitted";
                }
                if (auto error = value.find("error"); error != value.end() && error->is_object() && error->contains("message")) {
                    (*error)["message"] = "provider error details omitted";
                }
                auto diagnostics = value.find("diagnostics");
                if (diagnostics != value.end() && diagnostics->is_object()) {
                    diagnostics->erase("recent");
                    (*diagnostics)["recent"] = Json::array();
                }
                if (value.contains("received") && value.contains("recent") && value["recent"].is_array()) {
                    value["recent"] = Json::array();
                }
            } catch (...) {
            }
        }

        const backend::ThreadSnapshot* findThread(const backend::Snapshot& snapshot, std::string_view id) noexcept {
            const auto iterator =
                std::find_if(snapshot.threads.begin(), snapshot.threads.end(), [id](const backend::ThreadSnapshot& thread) {
                    return thread.id == id;
                });
            return iterator == snapshot.threads.end() ? nullptr : &*iterator;
        }

        const backend::TurnSnapshot* findTurn(const backend::Snapshot& snapshot, std::string_view id) noexcept {
            for (const backend::ThreadSnapshot& thread : snapshot.threads) {
                const auto iterator = std::find_if(thread.turns.begin(), thread.turns.end(), [id](const backend::TurnSnapshot& turn) {
                    return turn.id == id;
                });
                if (iterator != thread.turns.end()) {
                    return &*iterator;
                }
            }
            return nullptr;
        }

        struct ItemLocation {
            const backend::ItemSnapshot* item = nullptr;
            const backend::TurnSnapshot* turn = nullptr;
        };

        ItemLocation findItem(const backend::Snapshot& snapshot, std::string_view id) noexcept {
            for (const backend::ThreadSnapshot& thread : snapshot.threads) {
                for (const backend::TurnSnapshot& turn : thread.turns) {
                    const auto iterator = std::find_if(turn.items.begin(), turn.items.end(), [id](const backend::ItemSnapshot& item) {
                        return item.id == id;
                    });
                    if (iterator != turn.items.end()) {
                        return {&*iterator, &turn};
                    }
                }
            }
            return {};
        }

        std::optional<std::string> findNamedString(const Json& value, std::span<const std::string_view> names, std::size_t depth = 0) {
            if (depth > 8) {
                return std::nullopt;
            }
            if (value.is_object()) {
                for (std::string_view name : names) {
                    const auto iterator = value.find(std::string(name));
                    if (iterator != value.end() && iterator->is_string() && !iterator->get_ref<const std::string&>().empty()) {
                        return boundedText(iterator->get_ref<const std::string&>(), MaximumIdentifierBytes);
                    }
                }
                for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
                    if (const auto found = findNamedString(iterator.value(), names, depth + 1); found.has_value()) {
                        return found;
                    }
                }
            } else if (value.is_array()) {
                for (const Json& entry : value) {
                    if (const auto found = findNamedString(entry, names, depth + 1); found.has_value()) {
                        return found;
                    }
                }
            }
            return std::nullopt;
        }

        std::optional<std::string> eventThreadId(const Json& data) {
            static constexpr std::array names{std::string_view{"threadId"}, std::string_view{"conversationId"}};
            return findNamedString(data, names);
        }

        std::optional<std::string> eventTurnId(const Json& data) {
            static constexpr std::array names{std::string_view{"turnId"}};
            return findNamedString(data, names);
        }

        std::optional<std::string> eventItemId(const Json& data) {
            static constexpr std::array names{std::string_view{"itemId"}};
            return findNamedString(data, names);
        }

        Json minimumThread(std::optional<std::string> id) {
            return Json{{"id", id.value_or("unavailable")}, {"stateUnavailable", true}};
        }

        Json minimumTurn(std::optional<std::string> id, std::optional<std::string> threadId) {
            return Json{{"id", id.value_or("unavailable")},
                        {"threadId", threadId.value_or("unavailable")},
                        {"status", "unknown"},
                        {"active", false},
                        {"terminal", false},
                        {"stateUnavailable", true}};
        }

        Json minimumItem(std::optional<std::string> id, std::optional<std::string> threadId, std::optional<std::string> turnId) {
            Json item{{"id", id.value_or("unavailable")},
                      {"type", toString(ThreadItemKind::AgentMessage)},
                      {"truncated", true},
                      {"stateUnavailable", true}};
            if (threadId.has_value()) {
                item["threadId"] = *threadId;
            }
            if (turnId.has_value()) {
                item["turnId"] = *turnId;
            }
            return item;
        }

        Json eventData(ExpandedEventType type, const Json& legacyData, const backend::Snapshot& snapshot) {
            const std::optional<std::string> threadId = eventThreadId(legacyData);
            const std::optional<std::string> turnId = eventTurnId(legacyData);
            const std::optional<std::string> itemId = eventItemId(legacyData);
            switch (type) {
                case ExpandedEventType::ProviderUpdated:
                    return Json{{"provider", providerJson(snapshot.provider)}};
                case ExpandedEventType::ControllerUpdated:
                    return Json{{"controller", controllerJson(snapshot)}};
                case ExpandedEventType::SessionsUpdated:
                    return Json{{"sessions", sessionsJson(snapshot)}};
                case ExpandedEventType::ThreadUpserted: {
                    const backend::ThreadSnapshot* thread = threadId.has_value() ? findThread(snapshot, *threadId) : nullptr;
                    if (thread == nullptr && !snapshot.threads.empty()) {
                        thread = &snapshot.threads.back();
                    }
                    return Json{
                        {"thread",
                         thread != nullptr ? threadJson(*thread, false).value_or(minimumThread(threadId)) : minimumThread(threadId)}};
                }
                case ExpandedEventType::ThreadRemoved:
                    return Json{{"threadId", threadId.value_or("unavailable")}};
                case ExpandedEventType::TurnUpserted: {
                    const backend::TurnSnapshot* turn = turnId.has_value() ? findTurn(snapshot, *turnId) : nullptr;
                    if (turn == nullptr && !snapshot.threads.empty() && !snapshot.threads.back().turns.empty()) {
                        turn = &snapshot.threads.back().turns.back();
                    }
                    return Json{
                        {"turn",
                         turn != nullptr ? turnJson(*turn, false).value_or(minimumTurn(turnId, threadId)) : minimumTurn(turnId, threadId)}};
                }
                case ExpandedEventType::ItemUpserted: {
                    ItemLocation location = itemId.has_value() ? findItem(snapshot, *itemId) : ItemLocation{};
                    if (location.item == nullptr && !snapshot.threads.empty() && !snapshot.threads.back().turns.empty() &&
                        !snapshot.threads.back().turns.back().items.empty()) {
                        location = {&snapshot.threads.back().turns.back().items.back(), &snapshot.threads.back().turns.back()};
                    }
                    Json item = minimumItem(itemId, threadId, turnId);
                    if (location.item != nullptr && location.turn != nullptr) {
                        item = itemJson(*location.item, location.turn->threadId, location.turn->id).value_or(std::move(item));
                    }
                    return Json{{"item", std::move(item)}};
                }
                case ExpandedEventType::ItemContentUpdated: {
                    Json result{{"threadId", threadId.value_or("unavailable")},
                                {"turnId", turnId.value_or("unavailable")},
                                {"itemId", itemId.value_or("unavailable")},
                                {"content", boundedText(legacyData.value("content", std::string()))}};
                    if (const auto channel = legacyData.find("channel"); channel != legacyData.end() && channel->is_string()) {
                        result["channel"] = boundedText(channel->get_ref<const std::string&>(), 256);
                    }
                    return result;
                }
                case ExpandedEventType::PendingRequestsUpdated: {
                    Json requests = Json::array();
                    for (const backend::PendingRequestSnapshot& pending : snapshot.pendingRequests) {
                        if (const auto encoded = pendingJson(pending); encoded.has_value()) {
                            requests.push_back(*encoded);
                        }
                    }
                    return Json{{"pendingRequests", std::move(requests)}};
                }
                case ExpandedEventType::AccountUpdated:
                    return Json{{"domain", domainJson(snapshot.accounts, accountDetails(snapshot.accounts))}};
                case ExpandedEventType::ModelsUpdated:
                    return Json{{"domain", domainJson(snapshot.models)}};
                case ExpandedEventType::ConfigurationUpdated:
                    return Json{{"domain", domainJson(snapshot.configuration, configurationDetails(snapshot.configuration))}};
                case ExpandedEventType::ProcessUpdated:
                    return Json{{"process",
                                 snapshot.processes.empty() ? Json{{"processHandle", "unavailable"},
                                                                   {"lifecycle", "unknown"},
                                                                   {"stamp", stampJson({})},
                                                                   {"stateUnavailable", true}}
                                                            : processJson(snapshot.processes.back())}};
                case ExpandedEventType::FilesystemWatchUpdated:
                    return Json{{"filesystemWatch",
                                 snapshot.filesystemWatches.empty()
                                     ? Json{{"watchId", "unavailable"}, {"stamp", stampJson({})}, {"stateUnavailable", true}}
                                     : filesystemWatchJson(snapshot.filesystemWatches.back())}};
                case ExpandedEventType::FuzzySearchUpdated:
                    return Json{
                        {"fuzzySearch",
                         snapshot.fuzzySearchSessions.empty()
                             ? Json{{"sessionId", "unavailable"}, {"complete", false}, {"stamp", stampJson({})}, {"stateUnavailable", true}}
                             : fuzzySearchJson(snapshot.fuzzySearchSessions.back())}};
                case ExpandedEventType::ReviewsUpdated:
                    return Json{{"domain", domainJson(snapshot.reviews)}};
                case ExpandedEventType::IntegrationsUpdated:
                    return Json{{"domain", domainJson(snapshot.integrations, integrationsDetails(snapshot.integrations))}};
                case ExpandedEventType::PluginsUpdated:
                case ExpandedEventType::SkillsUpdated:
                    return Json{{"domain", domainJson(snapshot.pluginsAndSkills, pluginsDetails(snapshot.pluginsAndSkills))}};
                case ExpandedEventType::McpUpdated:
                    return Json{{"domain", domainJson(snapshot.mcp, mcpDetails(snapshot.mcp))}};
                case ExpandedEventType::PlatformUpdated:
                    return Json{{"domain", domainJson(snapshot.platform, platformDetails(snapshot.platform))}};
                case ExpandedEventType::NoticeAdded:
                    return Json{{"notice",
                                 snapshot.notices.empty() ? Json{{"category", "warning"},
                                                                 {"summary", "provider notice"},
                                                                 {"stamp", stampJson({})},
                                                                 {"stateUnavailable", true}}
                                                          : noticeJson(snapshot.notices.back())}};
                case ExpandedEventType::ActivityUpdated:
                    return Json{{"activity",
                                 snapshot.activities.empty() ? Json{{"kind", "unknown"},
                                                                    {"lifecycle", "unknown"},
                                                                    {"active", false},
                                                                    {"stamp", stampJson({})},
                                                                    {"stateUnavailable", true}}
                                                             : activityJson(snapshot.activities.back())}};
                case ExpandedEventType::CapacityUpdated:
                    return Json{{"capacity", capacityJson(snapshot)}};
                case ExpandedEventType::DiagnosticsUpdated:
                    return Json{{"diagnostic", Json{{"received", snapshot.diagnostics.received}, {"detailsOmitted", true}}}};
            }
            return Json{{"diagnostic", Json{{"detailsOmitted", true}}}};
        }

        std::optional<std::string_view> notificationMethod(std::string_view registryKey) noexcept {
            constexpr std::string_view marker = ":method:";
            const std::size_t offset = registryKey.rfind(marker);
            return offset == std::string_view::npos ? std::nullopt
                                                    : std::optional<std::string_view>{registryKey.substr(offset + marker.size())};
        }

        const generated::ProjectionMetadata* notificationForMethod(std::string_view method) noexcept {
            const auto iterator = std::find_if(generated::AllNotificationProjections.begin(),
                                               generated::AllNotificationProjections.end(),
                                               [method](const generated::ProjectionMetadata& metadata) {
                                                   return notificationMethod(metadata.registryKey) == method;
                                               });
            return iterator == generated::AllNotificationProjections.end() ? nullptr : &*iterator;
        }

        std::optional<FrontendCapability> projectionCapability(generated::Capability capability) noexcept {
            const auto metadata = std::find_if(generated::AllCapabilities.begin(),
                                               generated::AllCapabilities.end(),
                                               [capability](const generated::CapabilityMetadata& candidate) {
                                                   return candidate.id == capability;
                                               });
            return metadata == generated::AllCapabilities.end() ? std::nullopt : frontendCapabilityFromString(metadata->key);
        }

        std::vector<CanonicalExpandedEvent>
        normalizedExpandedEvents(std::string_view legacyType, const Json& legacyData, const backend::Snapshot& snapshot) {
            std::vector<CanonicalExpandedEvent> events;
            const auto append = [&](ExpandedEventType type) {
                events.push_back(
                    CanonicalExpandedEvent{type, ScopedProjectionValue{eventData(type, legacyData, snapshot), expandedEventRules(type)}});
            };
            if (legacyType == "backend.lifecycle.changed") {
                append(ExpandedEventType::ProviderUpdated);
            } else if (legacyType == "controller.changed") {
                append(ExpandedEventType::ControllerUpdated);
            } else if (legacyType == "session.changed") {
                append(ExpandedEventType::SessionsUpdated);
            } else if (legacyType == "thread.updated") {
                append(ExpandedEventType::ThreadUpserted);
            } else if (legacyType == "thread.list.updated") {
                if (snapshot.threads.empty()) {
                    append(ExpandedEventType::ThreadUpserted);
                } else {
                    for (const backend::ThreadSnapshot& thread : snapshot.threads) {
                        const Json data{{"thread", threadJson(thread, false).value_or(minimumThread(thread.id))}};
                        events.push_back(CanonicalExpandedEvent{ExpandedEventType::ThreadUpserted, ScopedProjectionValue{data, {}}});
                    }
                }
            } else if (legacyType == "thread.removed") {
                append(ExpandedEventType::ThreadRemoved);
            } else if (legacyType == "turn.updated") {
                append(ExpandedEventType::TurnUpserted);
            } else if (legacyType == "item.updated") {
                append(ExpandedEventType::ItemUpserted);
            } else if (legacyType == "item.content.updated") {
                append(ExpandedEventType::ItemContentUpdated);
            } else if (legacyType == "request.pending" || legacyType == "request.resolved") {
                append(ExpandedEventType::PendingRequestsUpdated);
            } else if (legacyType == "diagnostics.updated") {
                append(ExpandedEventType::DiagnosticsUpdated);
            } else if (const auto direct = expandedEventTypeFromString(legacyType); direct.has_value()) {
                append(*direct);
            }
            return events;
        }

        CanonicalSnapshotRecord minimumCanonicalSnapshot(Json legacyState, SequenceNumber sequence) noexcept {
            try {
                Json expanded{{"provider", providerJson({})},
                              {"controller", Json{{"present", false}}},
                              {"sessions", Json::array()},
                              {"capacity", Json::object()},
                              {"truncation", Json{{"truncated", true}, {"omittedEntries", 1}, {"droppedBytes", 0}}}};
                return canonicalizeSnapshot(
                    CanonicalSnapshotRecord{sequence, {std::move(legacyState), {}}, {std::move(expanded), {}}, Json::object(), {}, 64});
            } catch (...) {
                return {};
            }
        }

    } // namespace

    Json expandedSnapshotState(const backend::Snapshot& snapshot) noexcept {
        try {
            Json state{{"provider", providerJson(snapshot.provider)},
                       {"controller", controllerJson(snapshot)},
                       {"sessions", sessionsJson(snapshot)},
                       {"threads", Json::array()},
                       {"turns", Json::array()},
                       {"items", Json::array()},
                       {"pendingRequests", Json::array()},
                       {"accounts", domainJson(snapshot.accounts, accountDetails(snapshot.accounts))},
                       {"models", domainJson(snapshot.models)},
                       {"configuration", domainJson(snapshot.configuration, configurationDetails(snapshot.configuration))},
                       {"permissionProfiles", domainJson(snapshot.reviews)},
                       {"reviews", domainJson(snapshot.reviews)},
                       {"apps", domainJson(snapshot.integrations, integrationsDetails(snapshot.integrations))},
                       {"externalAgents", domainJson(snapshot.integrations, integrationsDetails(snapshot.integrations))},
                       {"hooks", domainJson(snapshot.integrations, integrationsDetails(snapshot.integrations))},
                       {"marketplace", domainJson(snapshot.integrations, integrationsDetails(snapshot.integrations))},
                       {"plugins", domainJson(snapshot.pluginsAndSkills, pluginsDetails(snapshot.pluginsAndSkills))},
                       {"skills", domainJson(snapshot.pluginsAndSkills, pluginsDetails(snapshot.pluginsAndSkills))},
                       {"mcp", domainJson(snapshot.mcp, mcpDetails(snapshot.mcp))},
                       {"windowsSandbox", domainJson(snapshot.platform, platformDetails(snapshot.platform))},
                       {"remoteControl", domainJson(snapshot.platform, platformDetails(snapshot.platform))},
                       {"processes", {{"entries", Json::array()}, {"truncation", Json{{"truncated", false}}}}},
                       {"filesystemWatches", {{"entries", Json::array()}, {"truncation", Json{{"truncated", false}}}}},
                       {"fuzzySearches", {{"entries", Json::array()}, {"truncation", Json{{"truncated", false}}}}},
                       {"notices", {{"entries", Json::array()}, {"truncation", Json{{"truncated", false}}}}},
                       {"activities", {{"entries", Json::array()}, {"truncation", Json{{"truncated", false}}}}},
                       {"capacity", capacityJson(snapshot)}};
            std::size_t omitted = 0;
            for (const backend::ThreadSnapshot& thread : snapshot.threads) {
                if (const auto encodedThread = threadJson(thread, false); encodedThread.has_value()) {
                    state["threads"].push_back(*encodedThread);
                } else {
                    saturatingAdd(omitted, std::size_t{1});
                }
                for (const backend::TurnSnapshot& turn : thread.turns) {
                    if (const auto encodedTurn = turnJson(turn, false); encodedTurn.has_value()) {
                        state["turns"].push_back(*encodedTurn);
                    } else {
                        saturatingAdd(omitted, std::size_t{1});
                    }
                    for (const backend::ItemSnapshot& item : turn.items) {
                        if (const auto encodedItem = itemJson(item, turn.threadId, turn.id); encodedItem.has_value()) {
                            state["items"].push_back(*encodedItem);
                        } else {
                            saturatingAdd(omitted, std::size_t{1});
                        }
                    }
                }
            }
            for (const backend::PendingRequestSnapshot& pending : snapshot.pendingRequests) {
                if (const auto encoded = pendingJson(pending); encoded.has_value()) {
                    state["pendingRequests"].push_back(*encoded);
                } else {
                    saturatingAdd(omitted, std::size_t{1});
                }
            }
            for (const backend::ProcessSnapshot& process : snapshot.processes) {
                if (!process.processHandle.empty()) {
                    state["processes"]["entries"].push_back(processJson(process));
                }
            }
            for (const backend::FilesystemWatchSnapshot& watch : snapshot.filesystemWatches) {
                if (!watch.watchId.empty()) {
                    state["filesystemWatches"]["entries"].push_back(filesystemWatchJson(watch));
                }
            }
            for (const backend::FuzzySearchSnapshot& search : snapshot.fuzzySearchSessions) {
                if (!search.sessionId.empty()) {
                    state["fuzzySearches"]["entries"].push_back(fuzzySearchJson(search));
                }
            }
            for (const backend::NoticeSnapshot& notice : snapshot.notices) {
                state["notices"]["entries"].push_back(noticeJson(notice));
            }
            for (const backend::ActivitySnapshot& activity : snapshot.activities) {
                state["activities"]["entries"].push_back(activityJson(activity));
            }
            state["processes"]["truncation"] = truncationJson(snapshot);
            state["filesystemWatches"]["truncation"] = truncationJson(snapshot);
            state["fuzzySearches"]["truncation"] = truncationJson(snapshot);
            state["notices"]["truncation"] = truncationJson(snapshot);
            state["activities"]["truncation"] = truncationJson(snapshot);
            state["truncation"] = truncationJson(snapshot, omitted);
            return state;
        } catch (...) {
            return Json{{"provider", providerJson({})},
                        {"controller", Json{{"present", false}}},
                        {"sessions", Json::array()},
                        {"capacity", Json::object()},
                        {"truncation", Json{{"truncated", true}, {"omittedEntries", 1}, {"droppedBytes", 0}}}};
        }
    }

    CanonicalSnapshotRecord
    makeCanonicalSnapshotRecord(Json legacyState, const backend::Snapshot& snapshot, SequenceNumber sequence) noexcept {
        try {
            scrubLegacyErrorAndDiagnostics(legacyState);
            Json expanded = expandedSnapshotState(snapshot);
            CanonicalSnapshotRecord record{sequence,
                                           ScopedProjectionValue{std::move(legacyState), {}},
                                           ScopedProjectionValue{std::move(expanded), {}},
                                           Json::object(),
                                           {},
                                           64};
            record.legacyState.rules = legacyRules(record.legacyState.value);
            record.expandedState.rules = expandedSnapshotRules();
            return canonicalizeSnapshot(std::move(record));
        } catch (...) {
            return minimumCanonicalSnapshot(std::move(legacyState), sequence);
        }
    }

    CanonicalEventRecord
    makeCanonicalEventRecord(std::string legacyType, Json legacyData, const backend::Snapshot& snapshot, SequenceNumber sequence) noexcept {
        try {
            scrubLegacyErrorAndDiagnostics(legacyData);
            CanonicalEventRecord record;
            record.sequence = sequence;
            record.legacyType = std::move(legacyType);
            record.legacyData.value = std::move(legacyData);
            record.legacyData.rules = legacyEventRules(record.legacyType);
            record.expandedEvents = normalizedExpandedEvents(record.legacyType, record.legacyData.value, snapshot);
            if (record.legacyType == "request.pending" || record.legacyType == "request.resolved") {
                record.expansionCapability = FrontendCapability::DedicatedPendingRequests;
            } else if (record.legacyType == "item.updated" || record.legacyType == "item.content.updated") {
                record.expansionCapability = FrontendCapability::CompleteThreadItems;
            }
            if (record.legacyType == "item.content.updated") {
                const auto channel = record.legacyData.value.find("channel");
                if (channel != record.legacyData.value.end() && channel->is_string()) {
                    const std::string normalized = normalizedName(channel->get_ref<const std::string&>());
                    std::optional<FrontendScope> contentScope;
                    if (normalized == "commandoutput" || normalized == "processoutput" || normalized == "stdout" ||
                        normalized == "stderr" || normalized == "shelloutput") {
                        contentScope = FrontendScope::CommandExecution;
                    } else if (normalized == "patch" || normalized == "diff" || normalized == "filechange") {
                        contentScope = FrontendScope::FilesystemWrite;
                    } else if (normalized == "path" || normalized == "filesystem") {
                        contentScope = FrontendScope::FilesystemRead;
                    }
                    if (contentScope.has_value()) {
                        const ScopeProjectionRule outputRule{"/content", {*contentScope}, ScopeProjectionAction::Omit};
                        record.legacyData.rules.push_back(outputRule);
                        for (CanonicalExpandedEvent& event : record.expandedEvents) {
                            event.data.rules.push_back(outputRule);
                        }
                    }
                }
            }
            if (record.legacyType == "codex.extension") {
                const auto method = record.legacyData.value.find("method");
                if (method != record.legacyData.value.end() && method->is_string()) {
                    if (const generated::ProjectionMetadata* metadata = notificationForMethod(method->get_ref<const std::string&>());
                        metadata != nullptr) {
                        record.registryKey = std::string(metadata->registryKey);
                        const auto capability = projectionCapability(metadata->expansionCapability);
                        if (!capability.has_value()) {
                            throw std::logic_error("generated projection capability is inconsistent");
                        }
                        record.expansionCapability = *capability;
                        record.requiredScopes.assign(metadata->requiredScopes.begin(), metadata->requiredScopes.end());
                        const std::vector<FrontendScope> scopes = legacyPayloadScopes(*metadata);
                        if (!scopes.empty()) {
                            record.legacyData.rules.push_back({"/params", scopes, ScopeProjectionAction::Omit});
                        }
                        record.expandedEvents.clear();
                        for (const std::string_view mapping : metadata->expandedMappings) {
                            if (const auto type = expandedEventTypeFromString(mapping); type.has_value()) {
                                record.expandedEvents.push_back(CanonicalExpandedEvent{
                                    *type,
                                    ScopedProjectionValue{eventData(*type, record.legacyData.value, snapshot), expandedEventRules(*type)}});
                            }
                        }
                    }
                }
            }
            return canonicalizeEvent(std::move(record));
        } catch (...) {
            CanonicalEventRecord failed;
            failed.sequence = sequence;
            failed.legacyType = "diagnostics.updated";
            failed.legacyData.value = Json{{"received", snapshot.diagnostics.received}, {"recent", Json::array()}};
            failed.expandedEvents = {CanonicalExpandedEvent{
                ExpandedEventType::DiagnosticsUpdated, ScopedProjectionValue{Json{{"diagnostic", Json{{"detailsOmitted", true}}}}, {}}}};
            return canonicalizeEvent(std::move(failed));
        }
    }

    void assignCanonicalSequence(CanonicalSnapshotRecord& record, SequenceNumber sequence) noexcept {
        record.sequence = sequence;
    }

    void assignCanonicalSequence(CanonicalEventRecord& record, SequenceNumber sequence) noexcept {
        record.sequence = sequence;
    }

} // namespace ai::openai::codex::frontend::detail
