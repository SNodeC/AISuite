/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/backend/Snapshot.h"

#include "ai/openai/codex/detail/DecodeDiagnostic.h"
#include "ai/openai/codex/typed/Items.h"
#include "ai/openai/codex/typed/Types.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <nlohmann/detail/iterators/iter_impl.hpp>
#include <nlohmann/detail/iterators/iteration_proxy.hpp>
#include <nlohmann/detail/json_ref.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace ai::openai::codex::backend {

    namespace {
        constexpr std::size_t MaxExtensionBytes = 64U * 1024U;
        constexpr std::size_t MaxExtensionNestingDepth = 32;
        constexpr std::size_t MaxExtensionJsonNodes = 4096;
        template <typename... Visitors>
        struct Overloaded : Visitors... {
            using Visitors::operator()...;
        };

        template <typename... Visitors>
        Overloaded(Visitors...) -> Overloaded<Visitors...>;

        std::string safeUtf8Prefix(std::string_view value, std::size_t byteLimit);

        struct UserMessageTextProjection {
            std::string text;
            std::uint64_t originalBytes = 0;
            std::size_t textFragments = 0;
            std::size_t nonTextItems = 0;
            bool truncated = false;
        };

        void saturatingAddUserMessageBytes(std::uint64_t& destination, std::size_t increment) noexcept {
            const auto boundedIncrement = static_cast<std::uint64_t>(increment);
            if (boundedIncrement > std::numeric_limits<std::uint64_t>::max() - destination) {
                destination = std::numeric_limits<std::uint64_t>::max();
            } else {
                destination += boundedIncrement;
            }
        }

        UserMessageTextProjection userMessageTextProjection(const typed::UserMessageThreadItem& item) {
            constexpr std::string_view FragmentSeparator = "\n\n";
            UserMessageTextProjection projection;
            projection.text.reserve(MaxProjectedUserMessageTextBytes);

            const auto append = [&projection](std::string_view value) {
                if (projection.truncated || value.empty()) {
                    return;
                }
                const std::size_t available = MaxProjectedUserMessageTextBytes - projection.text.size();
                const std::string retained = safeUtf8Prefix(value, available);
                projection.text.append(retained);
                projection.truncated = retained.size() != value.size();
            };

            for (const typed::TurnInput& input : item.content) {
                const auto* text = std::get_if<typed::TextInput>(&input);
                if (text == nullptr) {
                    ++projection.nonTextItems;
                    continue;
                }
                if (projection.textFragments != 0) {
                    saturatingAddUserMessageBytes(projection.originalBytes, FragmentSeparator.size());
                    append(FragmentSeparator);
                }
                ++projection.textFragments;
                saturatingAddUserMessageBytes(projection.originalBytes, text->text.size());
                append(text->text);
            }
            return projection;
        }

        Json boundedJson(const Json& value) {
            try {
                const std::string encoded = value.dump();
                if (encoded.size() <= MaxExtensionBytes) {
                    return value;
                }
                return Json::object(
                    {{"truncated", true}, {"originalBytes", encoded.size()}, {"preview", encoded.substr(0, MaxExtensionBytes)}});
            } catch (...) {
                return Json::object({{"omitted", true}, {"reason", "value could not be serialized safely"}});
            }
        }

        Json userMessageData(const typed::UserMessageThreadItem& item) {
            try {
                const UserMessageTextProjection textProjection = userMessageTextProjection(item);
                const auto rawContent = item.metadata.raw.find("content");
                const Json content = rawContent != item.metadata.raw.end() && rawContent->is_array() ? *rawContent : Json::array();
                const std::size_t originalContentBytes = content.dump().size();
                const std::size_t originalContentItems = content.size();
                const Json clientId = item.clientId ? Json(item.clientId->value) : Json(nullptr);
                Json retainedContent = Json::array();
                std::size_t retainedContentBytes = retainedContent.dump().size();
                std::size_t retainedContentItems = 0;

                const auto makeData = [&](const Json& retained, bool contentTruncated, std::size_t contentBytes, std::size_t contentItems) {
                    return Json::object({{"clientId", clientId},
                                         {"content", retained},
                                         {"contentTruncated", contentTruncated},
                                         {"text", textProjection.text},
                                         {"textTruncated", textProjection.truncated},
                                         {"originalTextBytes", textProjection.originalBytes},
                                         {"retainedTextBytes", static_cast<std::uint64_t>(textProjection.text.size())},
                                         {"textFragments", static_cast<std::uint64_t>(textProjection.textFragments)},
                                         {"nonTextItems", static_cast<std::uint64_t>(textProjection.nonTextItems)},
                                         {"originalContentBytes", static_cast<std::uint64_t>(originalContentBytes)},
                                         {"retainedContentBytes", static_cast<std::uint64_t>(contentBytes)},
                                         {"originalContentItems", static_cast<std::uint64_t>(originalContentItems)},
                                         {"retainedContentItems", static_cast<std::uint64_t>(contentItems)}});
                };

                for (const Json& contentEntry : content) {
                    const std::size_t separatorBytes = retainedContentItems == 0 ? 0 : 1;
                    const std::size_t contentEntryBytes = contentEntry.dump().size();
                    if (retainedContentBytes > MaxSerializedUserMessageDataBytes - separatorBytes ||
                        contentEntryBytes > MaxSerializedUserMessageDataBytes - separatorBytes - retainedContentBytes) {
                        break;
                    }

                    const std::size_t candidateContentBytes = retainedContentBytes + separatorBytes + contentEntryBytes;
                    const std::size_t candidateContentItems = retainedContentItems + 1;
                    const bool candidateTruncated = candidateContentItems < originalContentItems;
                    const Json candidateSkeleton =
                        makeData(Json::array(), candidateTruncated, candidateContentBytes, candidateContentItems);
                    const std::size_t candidateDataBytes =
                        candidateSkeleton.dump().size() - Json::array().dump().size() + candidateContentBytes;
                    if (candidateDataBytes > MaxSerializedUserMessageDataBytes) {
                        break;
                    }

                    retainedContent.push_back(contentEntry);
                    retainedContentBytes = candidateContentBytes;
                    retainedContentItems = candidateContentItems;
                }

                return makeData(retainedContent, retainedContentItems < originalContentItems, retainedContentBytes, retainedContentItems);
            } catch (...) {
                return Json::object({{"omitted", true}, {"reason", "user message content could not be serialized safely"}});
            }
        }

        std::string safeUtf8Prefix(std::string_view value, std::size_t byteLimit) {
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
                    const unsigned char continuation = static_cast<unsigned char>(value[offset + index]);
                    valid = valid && (continuation & 0xc0U) == 0x80U;
                }
                if (!valid) {
                    break;
                }
                offset += width;
            }
            return std::string(value.substr(0, offset));
        }

        std::string normalizedKey(std::string_view key) {
            std::string result;
            result.reserve(key.size());
            for (const char rawCharacter : key) {
                const unsigned char character = static_cast<unsigned char>(rawCharacter);
                if (std::isalnum(character) != 0) {
                    result.push_back(static_cast<char>(std::tolower(character)));
                }
            }
            return result;
        }

        bool isSensitiveKey(std::string_view key) {
            const std::string normalized = normalizedKey(key);
            return normalized.ends_with("token") || normalized.ends_with("secret") || normalized.ends_with("answer") ||
                   normalized.ends_with("answers") || normalized.ends_with("credential") || normalized.ends_with("credentials") ||
                   normalized.find("password") != std::string::npos || normalized.find("passphrase") != std::string::npos ||
                   normalized.find("authorization") != std::string::npos || normalized.find("apikey") != std::string::npos ||
                   normalized == "command" || normalized == "cwd" || normalized == "reason" || normalized.ends_with("path");
        }

        bool isSecretValueKey(std::string_view key) {
            const std::string normalized = normalizedKey(key);
            return normalized == "value" || normalized == "values" || normalized == "text" || normalized == "response" ||
                   normalized == "responses";
        }

        struct JsonSanitizerState {
            std::size_t remainingNodes = MaxExtensionJsonNodes;
            bool truncated = false;
            bool redacted = false;
        };

        Json sanitizeExtensionJson(const Json& value, JsonSanitizerState& state, std::size_t depth = 0) {
            if (depth >= MaxExtensionNestingDepth || state.remainingNodes == 0) {
                state.truncated = true;
                return Json::object({{"omitted", true}, {"reason", "extension structure limit exceeded"}});
            }
            --state.remainingNodes;

            if (value.is_object()) {
                Json result = Json::object();
                bool secretObject = false;
                if (const auto secret = value.find("secret"); secret != value.end() && secret->is_boolean()) {
                    secretObject = secret->get<bool>();
                }
                for (const auto& [key, member] : value.items()) {
                    if (isSensitiveKey(key) || (secretObject && isSecretValueKey(key))) {
                        result[key] = "[redacted]";
                        state.redacted = true;
                    } else if (normalizedKey(key) == "secret" && !member.is_boolean()) {
                        result[key] = "[redacted]";
                        state.redacted = true;
                    } else {
                        result[key] = sanitizeExtensionJson(member, state, depth + 1);
                    }
                }
                return result;
            }
            if (value.is_array()) {
                Json result = Json::array();
                for (const Json& member : value) {
                    if (state.remainingNodes == 0) {
                        state.truncated = true;
                        result.push_back(Json::object({{"omitted", true}, {"reason", "extension structure limit exceeded"}}));
                        break;
                    }
                    result.push_back(sanitizeExtensionJson(member, state, depth + 1));
                }
                return result;
            }
            return value;
        }

        Json sanitizeExtensionJsonForMethod(std::string_view method, const Json& value, JsonSanitizerState& state) {
            if (method == "app/list/updated" && value.is_object()) {
                Json methodSanitized = value;
                const auto data = methodSanitized.find("data");
                if (data != methodSanitized.end()) {
                    *data = "[redacted]";
                    state.redacted = true;
                }
                return sanitizeExtensionJson(methodSanitized, state);
            }
            if ((method == "externalAgentConfig/import/completed" || method == "externalAgentConfig/import/progress") &&
                value.is_object()) {
                Json methodSanitized = value;
                for (const char* field : {"importId", "itemTypeResults"}) {
                    const auto sensitive = methodSanitized.find(field);
                    if (sensitive != methodSanitized.end()) {
                        *sensitive = "[redacted]";
                        state.redacted = true;
                    }
                }
                return sanitizeExtensionJson(methodSanitized, state);
            }
            if ((method == "hook/completed" || method == "hook/started") && value.is_object()) {
                Json methodSanitized = value;
                for (const char* field : {"run", "threadId", "turnId"}) {
                    const auto sensitive = methodSanitized.find(field);
                    if (sensitive != methodSanitized.end()) {
                        *sensitive = "[redacted]";
                        state.redacted = true;
                    }
                }
                return sanitizeExtensionJson(methodSanitized, state);
            }
            if (method == "skills/changed") {
                state.redacted = true;
                return "[redacted]";
            }
            if (method == "command/exec/outputDelta" && value.is_object()) {
                Json methodSanitized = value;
                for (const char* field : {"deltaBase64", "processId"}) {
                    const auto sensitive = methodSanitized.find(field);
                    if (sensitive != methodSanitized.end()) {
                        *sensitive = "[redacted]";
                        state.redacted = true;
                    }
                }
                return sanitizeExtensionJson(methodSanitized, state);
            }
            if (method == "fs/changed" && value.is_object()) {
                Json methodSanitized = value;
                for (const char* field : {"changedPaths", "watchId"}) {
                    const auto sensitive = methodSanitized.find(field);
                    if (sensitive != methodSanitized.end()) {
                        *sensitive = "[redacted]";
                        state.redacted = true;
                    }
                }
                return sanitizeExtensionJson(methodSanitized, state);
            }
            if ((method == "fuzzyFileSearch/sessionCompleted" || method == "fuzzyFileSearch/sessionUpdated") && value.is_object()) {
                Json methodSanitized = value;
                for (const char* field : {"files", "query", "sessionId"}) {
                    const auto sensitive = methodSanitized.find(field);
                    if (sensitive != methodSanitized.end()) {
                        *sensitive = "[redacted]";
                        state.redacted = true;
                    }
                }
                return sanitizeExtensionJson(methodSanitized, state);
            }
            if (method == "guardianWarning" && value.is_object()) {
                Json methodSanitized = value;
                for (const char* field : {"message", "threadId"}) {
                    const auto sensitive = methodSanitized.find(field);
                    if (sensitive != methodSanitized.end()) {
                        *sensitive = "[redacted]";
                        state.redacted = true;
                    }
                }
                return sanitizeExtensionJson(methodSanitized, state);
            }
            if ((method == "item/autoApprovalReview/started" || method == "item/autoApprovalReview/completed") && value.is_object()) {
                Json methodSanitized = value;
                for (const char* field : {"action", "review", "reviewId", "targetItemId", "threadId", "turnId"}) {
                    const auto sensitive = methodSanitized.find(field);
                    if (sensitive != methodSanitized.end()) {
                        *sensitive = "[redacted]";
                        state.redacted = true;
                    }
                }
                return sanitizeExtensionJson(methodSanitized, state);
            }
            if (method == "applyPatchApproval" && value.is_object()) {
                Json methodSanitized = value;
                for (const char* field : {"callId", "conversationId", "fileChanges", "grantRoot", "reason"}) {
                    const auto sensitive = methodSanitized.find(field);
                    if (sensitive != methodSanitized.end()) {
                        *sensitive = "[redacted]";
                        state.redacted = true;
                    }
                }
                return sanitizeExtensionJson(methodSanitized, state);
            }
            if (method == "execCommandApproval" && value.is_object()) {
                Json methodSanitized = value;
                for (const char* field : {"approvalId", "callId", "command", "conversationId", "cwd", "parsedCmd", "reason"}) {
                    const auto sensitive = methodSanitized.find(field);
                    if (sensitive != methodSanitized.end()) {
                        *sensitive = "[redacted]";
                        state.redacted = true;
                    }
                }
                return sanitizeExtensionJson(methodSanitized, state);
            }
            if (method == "item/permissions/requestApproval" && value.is_object()) {
                Json methodSanitized = value;
                for (const char* field : {"cwd", "environmentId", "itemId", "permissions", "reason", "threadId", "turnId"}) {
                    const auto sensitive = methodSanitized.find(field);
                    if (sensitive != methodSanitized.end()) {
                        *sensitive = "[redacted]";
                        state.redacted = true;
                    }
                }
                return sanitizeExtensionJson(methodSanitized, state);
            }
            if (method == "configWarning" && value.is_object()) {
                Json methodSanitized = value;
                const auto details = methodSanitized.find("details");
                if (details != methodSanitized.end() && !details->is_null()) {
                    *details = "[redacted]";
                    state.redacted = true;
                }
                return sanitizeExtensionJson(methodSanitized, state);
            }
            return sanitizeExtensionJson(value, state);
        }

        Json safeSnapshotJson(const Json& value) {
            JsonSanitizerState sanitizer;
            return boundedJson(sanitizeExtensionJson(value, sanitizer));
        }

        std::string lifecycleName(ItemLifecycle lifecycle) {
            switch (lifecycle) {
                case ItemLifecycle::Unknown:
                    return "unknown";
                case ItemLifecycle::Started:
                    return "started";
                case ItemLifecycle::Completed:
                    return "completed";
                case ItemLifecycle::Failed:
                    return "failed";
            }
            return "unknown";
        }

        std::string errorCategoryName(Error::Category category) {
            switch (category) {
                case Error::Category::Launch:
                    return "launch";
                case Error::Category::Transport:
                    return "transport";
                case Error::Category::Protocol:
                    return "protocol";
                case Error::Category::Initialization:
                    return "initialization";
                case Error::Category::Process:
                    return "process";
                case Error::Category::InvalidState:
                    return "invalid_state";
                case Error::Category::Capacity:
                    return "capacity";
                case Error::Category::Cancelled:
                    return "cancelled";
                case Error::Category::Enqueue:
                    return "enqueue";
            }
            return "unknown";
        }

        ItemSnapshot snapshotItem(const typed::ItemId& id, const ItemState& state) {
            ItemSnapshot snapshot;
            snapshot.id = safeUtf8Prefix(id.value, MaxSnapshotExtensionMethodBytes);
            snapshot.type = safeUtf8Prefix(itemType(state.item), MaxSnapshotExtensionMethodBytes);
            snapshot.status = safeUtf8Prefix(lifecycleName(state.lifecycle), MaxSnapshotExtensionMethodBytes);
            snapshot.agentText = safeUtf8Prefix(state.agentText, MaxSnapshotExtensionPayloadBytes);
            snapshot.reasoningText = safeUtf8Prefix(state.reasoningText, MaxSnapshotExtensionPayloadBytes);
            snapshot.reasoningSummary = safeUtf8Prefix(state.reasoningSummary, MaxSnapshotExtensionPayloadBytes);
            snapshot.commandOutput = safeUtf8Prefix(state.commandOutput, MaxSnapshotExtensionPayloadBytes);
            snapshot.droppedContentBytes = state.droppedContentBytes;
            snapshot.contentTruncated = state.droppedContentBytes != 0;
            snapshot.startedAtMs = state.startedAtMs;
            snapshot.completedAtMs = state.completedAtMs;
            snapshot.extensions = safeSnapshotJson(state.extensions);
            snapshot.stamp = state.stamp;
            snapshot.connectionInvalidated = state.connectionInvalidated;

            std::visit(
                Overloaded{
                    [&snapshot](const typed::AgentMessageThreadItem& value) {
                        if (value.phase) {
                            snapshot.data["phase"] = safeUtf8Prefix(value.phase->value, MaxSnapshotExtensionMethodBytes);
                        }
                    },
                    [&snapshot](const typed::UserMessageThreadItem& value) {
                        snapshot.data = userMessageData(value);
                    },
                    [&snapshot](const typed::ReasoningThreadItem&) {
                        snapshot.data["hasSummary"] = !snapshot.reasoningSummary.empty();
                    },
                    [&snapshot](const typed::CommandExecutionThreadItem& value) {
                        snapshot.data = Json::object({{"command", safeUtf8Prefix(value.command, MaxSnapshotExtensionPayloadBytes)},
                                                      {"cwd", safeUtf8Prefix(value.cwd.value, MaxSnapshotExtensionMethodBytes)},
                                                      {"status", safeUtf8Prefix(value.status.value, MaxSnapshotExtensionMethodBytes)}});
                        if (value.processId) {
                            snapshot.data["processId"] = safeUtf8Prefix(*value.processId, MaxSnapshotExtensionMethodBytes);
                        }
                        if (value.exitCode) {
                            snapshot.data["exitCode"] = *value.exitCode;
                        }
                        if (value.durationMs) {
                            snapshot.data["durationMs"] = *value.durationMs;
                        }
                    },
                    [&snapshot](const typed::FileChangeThreadItem& value) {
                        constexpr std::size_t MaxRetainedFileChangeSummaries = 256;
                        snapshot.data = Json::object({{"status", safeUtf8Prefix(value.status.value, MaxSnapshotExtensionMethodBytes)},
                                                      {"changeCount", value.changes.size()},
                                                      {"changesTruncated", value.changes.size() > MaxRetainedFileChangeSummaries},
                                                      {"changes", Json::array()}});
                        const std::size_t retainedCount = std::min(value.changes.size(), MaxRetainedFileChangeSummaries);
                        for (std::size_t index = 0; index < retainedCount; ++index) {
                            const typed::FileUpdateChange& change = value.changes[index];
                            snapshot.data["changes"].push_back({{"kindAlternative", change.kind.index()},
                                                                {"pathBytes", change.path.size()},
                                                                {"pathRedacted", true},
                                                                {"diffBytes", change.diff.size()},
                                                                {"diffOmitted", true}});
                        }
                    },
                    [&snapshot](const typed::McpToolCallThreadItem& value) {
                        snapshot.data = Json::object({{"tool", safeUtf8Prefix(value.tool, MaxSnapshotExtensionMethodBytes)},
                                                      {"status", safeUtf8Prefix(value.status.value, MaxSnapshotExtensionMethodBytes)},
                                                      {"hasResult", value.result.hasValue()}});
                        snapshot.data["server"] = safeUtf8Prefix(value.server, MaxSnapshotExtensionMethodBytes);
                    },
                    [&snapshot](const typed::DynamicToolCallThreadItem& value) {
                        snapshot.data = Json::object({{"tool", safeUtf8Prefix(value.tool, MaxSnapshotExtensionMethodBytes)},
                                                      {"status", safeUtf8Prefix(value.status.value, MaxSnapshotExtensionMethodBytes)},
                                                      {"hasResult", value.contentItems.hasValue()}});
                        if (value.nameSpace) {
                            snapshot.data["namespace"] = safeUtf8Prefix(*value.nameSpace, MaxSnapshotExtensionMethodBytes);
                        }
                    },
                    [&snapshot](const typed::WebSearchThreadItem& value) {
                        snapshot.data = Json::object({{"query", safeUtf8Prefix(value.query, MaxSnapshotExtensionPayloadBytes)}});
                    },
                    [&snapshot](const typed::CollabAgentToolCallThreadItem& value) {
                        snapshot.data =
                            Json::object({{"tool", safeUtf8Prefix(value.tool.value, MaxSnapshotExtensionMethodBytes)},
                                          {"status", safeUtf8Prefix(value.status.value, MaxSnapshotExtensionMethodBytes)},
                                          {"senderThreadId", safeUtf8Prefix(value.senderThreadId.value, MaxSnapshotExtensionMethodBytes)},
                                          {"receiverCount", value.receiverThreadIds.size()},
                                          {"agentStateCount", value.agentsStates.size()},
                                          {"hasPrompt", value.prompt.hasValue()}});
                        if (value.prompt.hasValue()) {
                            snapshot.data["promptBytes"] = value.prompt.value->size();
                        }
                    },
                    [&snapshot](const typed::ContextCompactionThreadItem&) {
                        snapshot.data = Json::object({{"compacted", true}});
                    },
                    [&snapshot](const typed::EnteredReviewModeThreadItem& value) {
                        snapshot.data =
                            Json::object({{"mode", "entered"}, {"review", safeUtf8Prefix(value.review, MaxSnapshotExtensionMethodBytes)}});
                    },
                    [&snapshot](const typed::ExitedReviewModeThreadItem& value) {
                        snapshot.data =
                            Json::object({{"mode", "exited"}, {"review", safeUtf8Prefix(value.review, MaxSnapshotExtensionMethodBytes)}});
                    },
                    [&snapshot](const typed::HookPromptThreadItem& value) {
                        snapshot.data = Json::object({{"fragmentCount", value.fragments.size()}});
                        if (!value.fragments.empty()) {
                            snapshot.data["firstHookRunId"] =
                                safeUtf8Prefix(value.fragments.front().hookRunId, MaxSnapshotExtensionMethodBytes);
                        }
                    },
                    [&snapshot](const typed::ImageGenerationThreadItem& value) {
                        snapshot.data = Json::object({{"status", safeUtf8Prefix(value.status, MaxSnapshotExtensionMethodBytes)},
                                                      {"resultBytes", value.result.size()},
                                                      {"hasRevisedPrompt", value.revisedPrompt.hasValue()},
                                                      {"hasSavedPath", value.savedPath.hasValue()}});
                    },
                    [&snapshot](const typed::ImageViewThreadItem& value) {
                        snapshot.data = Json::object({{"path", safeUtf8Prefix(value.path.value, MaxSnapshotExtensionMethodBytes)}});
                    },
                    [&snapshot](const typed::PlanThreadItem& value) {
                        snapshot.data = Json::object({{"text", safeUtf8Prefix(value.text, MaxSnapshotExtensionPayloadBytes)},
                                                      {"textTruncated", value.text.size() > MaxSnapshotExtensionPayloadBytes}});
                    },
                    [&snapshot](const typed::SleepThreadItem& value) {
                        snapshot.data = Json::object({{"durationMs", value.durationMs}});
                    },
                    [&snapshot](const typed::SubAgentActivityThreadItem& value) {
                        snapshot.data =
                            Json::object({{"agentPath", safeUtf8Prefix(value.agentPath, MaxSnapshotExtensionMethodBytes)},
                                          {"agentThreadId", safeUtf8Prefix(value.agentThreadId.value, MaxSnapshotExtensionMethodBytes)},
                                          {"kind", safeUtf8Prefix(value.kind.value, MaxSnapshotExtensionMethodBytes)}});
                    },
                    [&snapshot](const typed::UnknownItem& value) {
                        snapshot.data = Json::object();
                        if (value.type) {
                            snapshot.data["codexType"] = safeUtf8Prefix(*value.type, MaxSnapshotExtensionMethodBytes);
                        }
                        if (const std::optional<std::string> decodingError =
                                ::ai::openai::codex::detail::safeDecodeDiagnosticText(value.diagnostic)) {
                            snapshot.data["decodingError"] = safeUtf8Prefix(*decodingError, MaxSnapshotExtensionDecodingErrorBytes);
                        }
                    },
                    [&snapshot](const auto&) {
                        snapshot.data = Json::object({{"codexType", snapshot.type}});
                    }},
                state.item);
            return snapshot;
        }

        TurnSnapshot snapshotTurn(const typed::TurnId& id, const TurnState& state) {
            TurnSnapshot snapshot;
            snapshot.id = safeUtf8Prefix(id.value, MaxSnapshotExtensionMethodBytes);
            snapshot.threadId = safeUtf8Prefix(state.turn.threadId.value, MaxSnapshotExtensionMethodBytes);
            snapshot.status = safeUtf8Prefix(state.turn.status.value, MaxSnapshotExtensionMethodBytes);
            snapshot.active = state.active;
            snapshot.terminal = state.terminal;
            if (state.failure) {
                snapshot.failure = safeSnapshotJson(*state.failure);
            }
            if (state.tokenUsage) {
                snapshot.tokenUsage = safeSnapshotJson(*state.tokenUsage);
            }
            snapshot.extensions = safeSnapshotJson(state.extensions);
            snapshot.stamp = state.stamp;
            snapshot.connectionInvalidated = state.connectionInvalidated;

            std::set<std::string> visited;
            for (const typed::ItemId& itemIdValue : state.itemOrder) {
                const auto iterator = state.items.find(itemIdValue.value);
                if (iterator != state.items.end() && visited.insert(iterator->first).second) {
                    snapshot.items.push_back(snapshotItem(itemIdValue, iterator->second));
                }
            }
            for (const auto& [itemIdValue, item] : state.items) {
                if (visited.insert(itemIdValue).second) {
                    snapshot.items.push_back(snapshotItem(typed::ItemId{itemIdValue}, item));
                }
            }
            return snapshot;
        }

        ThreadSnapshot snapshotThread(const typed::ThreadId& id, const ThreadState& state) {
            ThreadSnapshot snapshot;
            snapshot.id = safeUtf8Prefix(id.value, MaxSnapshotExtensionMethodBytes);
            if (state.thread.title) {
                snapshot.title = safeUtf8Prefix(*state.thread.title, MaxSnapshotExtensionPayloadBytes);
            }
            const bool backendPlaceholder =
                state.thread.raw.is_object() && (state.thread.raw.size() == 1 || state.thread.raw.size() == 2) &&
                state.thread.raw.value("backendPlaceholder", false) &&
                (state.thread.raw.size() == 1 ||
                 (state.thread.raw.size() == 2 && state.thread.raw.value("backendPlaceholderStatusKnown", false)));
            const bool backendPlaceholderStatusKnown = backendPlaceholder && state.thread.raw.size() == 2;
            if (!backendPlaceholder && !state.thread.cwd.value.empty()) {
                snapshot.cwd = "[redacted]";
            }
            if (state.thread.model) {
                snapshot.model = safeUtf8Prefix(state.thread.model->value, MaxSnapshotExtensionMethodBytes);
            }
            if (!backendPlaceholder) {
                snapshot.modelProvider = safeUtf8Prefix(state.thread.modelProvider, MaxSnapshotExtensionMethodBytes);
                snapshot.preview = safeUtf8Prefix(state.thread.preview, MaxSnapshotExtensionPayloadBytes);
                snapshot.createdAt = state.thread.createdAt;
                snapshot.updatedAt = state.thread.updatedAt;
            }
            if (!backendPlaceholder || backendPlaceholderStatusKnown) {
                snapshot.status = safeUtf8Prefix(typed::threadStatusDiscriminator(state.thread.status), MaxSnapshotExtensionMethodBytes);
            }
            snapshot.fullyLoaded = state.fullyLoaded;
            snapshot.extensions = safeSnapshotJson(state.extensions);
            snapshot.realtime = {
                safeUtf8Prefix(state.realtime.lifecycle, MaxSnapshotExtensionMethodBytes),
                safeUtf8Prefix(state.realtime.transcript, MaxSnapshotExtensionPayloadBytes),
                state.realtime.lastError
                    ? std::optional<std::string>{safeUtf8Prefix(*state.realtime.lastError, MaxSnapshotExtensionDecodingErrorBytes)}
                    : std::nullopt,
                state.realtime.sessionId
                    ? std::optional<std::string>{safeUtf8Prefix(*state.realtime.sessionId, MaxSnapshotExtensionMethodBytes)}
                    : std::nullopt,
                state.realtime.version
                    ? std::optional<std::string>{safeUtf8Prefix(*state.realtime.version, MaxSnapshotExtensionMethodBytes)}
                    : std::nullopt,
                state.realtime.lastSdpBytes,
                state.realtime.itemCount,
                state.realtime.receivedAudioBytes,
                state.realtime.droppedAudioBytes,
                state.realtime.transcriptTruncated,
                state.realtime.stamp};
            snapshot.stamp = state.stamp;

            std::set<std::string> visited;
            for (const typed::TurnId& turnId : state.turnOrder) {
                const auto iterator = state.turns.find(turnId.value);
                if (iterator != state.turns.end() && visited.insert(iterator->first).second) {
                    snapshot.turns.push_back(snapshotTurn(turnId, iterator->second));
                }
            }
            for (const auto& [turnId, turn] : state.turns) {
                if (visited.insert(turnId).second) {
                    snapshot.turns.push_back(snapshotTurn(typed::TurnId{turnId}, turn));
                }
            }
            return snapshot;
        }

        void snapshotGenericRequest(PendingRequestSnapshot& snapshot,
                                    std::string method,
                                    const Json& params,
                                    std::optional<std::string> decodingError = std::nullopt) {
            snapshot.type = "unknown";
            const ExtensionSnapshot safeParams = makeExtensionSnapshot(
                ExtensionRecord{std::move(method), params, std::move(decodingError), std::nullopt, std::nullopt, std::nullopt});
            snapshot.details["method"] = safeParams.method;
            if (safeParams.methodTruncated) {
                snapshot.details["methodTruncated"] = true;
                snapshot.details["originalMethodBytes"] = safeParams.originalMethodBytes;
            }
            snapshot.details["params"] = safeParams.payload;
            if (safeParams.sensitiveFieldsRedacted) {
                snapshot.details["sensitiveFieldsRedacted"] = true;
            }
            if (safeParams.payloadTruncated) {
                snapshot.details["paramsTruncated"] = true;
                if (safeParams.originalPayloadBytes.has_value()) {
                    snapshot.details["originalParamsBytes"] = *safeParams.originalPayloadBytes;
                }
            }
            if (safeParams.decodingError) {
                snapshot.details["decodingError"] = *safeParams.decodingError;
            }
            if (safeParams.decodingErrorTruncated) {
                snapshot.details["decodingErrorTruncated"] = true;
                snapshot.details["originalDecodingErrorBytes"] = safeParams.originalDecodingErrorBytes;
            }
        }

        PendingRequestSnapshot snapshotPendingRequest(const PendingRequestState& state) {
            PendingRequestSnapshot snapshot;
            snapshot.id = state.id;
            std::visit(Overloaded{[&snapshot](const typed::CommandApprovalRequest& value) {
                                      snapshot.type = "command_approval";
                                      snapshot.threadId = value.threadId.value;
                                      snapshot.turnId = value.turnId.value;
                                      snapshot.itemId = value.itemId.value;
                                      if (value.command) {
                                          snapshot.details["commandBytes"] = value.command->size();
                                          snapshot.details["commandRedacted"] = true;
                                      }
                                      if (value.cwd) {
                                          snapshot.details["cwdBytes"] = value.cwd->size();
                                          snapshot.details["cwdRedacted"] = true;
                                      }
                                      if (value.reason) {
                                          snapshot.details["reasonBytes"] = value.reason->size();
                                          snapshot.details["reasonRedacted"] = true;
                                      }
                                  },
                                  [&snapshot](const typed::FileChangeApprovalRequest& value) {
                                      snapshot.type = "file_change_approval";
                                      snapshot.threadId = value.threadId.value;
                                      snapshot.turnId = value.turnId.value;
                                      snapshot.itemId = value.itemId.value;
                                      if (value.reason) {
                                          snapshot.details["reasonBytes"] = value.reason->size();
                                          snapshot.details["reasonRedacted"] = true;
                                      }
                                      if (value.grantRoot) {
                                          snapshot.details["grantRootBytes"] = value.grantRoot->size();
                                          snapshot.details["grantRootRedacted"] = true;
                                      }
                                  },
                                  [&snapshot](const typed::UserInputRequest& value) {
                                      snapshot.type = "user_input";
                                      snapshot.threadId = value.threadId.value;
                                      snapshot.turnId = value.turnId.value;
                                      snapshot.itemId = value.itemId.value;
                                      snapshot.details["questions"] = Json::array();
                                      for (const typed::UserInputQuestion& question : value.questions) {
                                          Json encoded = Json::object({{"id", question.id},
                                                                       {"header", question.header},
                                                                       {"prompt", question.prompt},
                                                                       {"allowsFreeText", question.allowsFreeText},
                                                                       {"secret", question.secret},
                                                                       {"options", Json::array()}});
                                          for (const typed::UserInputOption& option : question.options) {
                                              encoded["options"].push_back({{"label", option.label}, {"description", option.description}});
                                          }
                                          snapshot.details["questions"].push_back(std::move(encoded));
                                      }
                                      if (value.autoResolutionMs) {
                                          snapshot.details["autoResolutionMs"] = *value.autoResolutionMs;
                                      }
                                  },
                                  [&snapshot](const typed::AuthenticationRequest& value) {
                                      snapshot.type = "authentication";
                                      snapshot.details["reason"] = value.reason;
                                      if (value.previousAccountId) {
                                          snapshot.details["previousAccountId"] = *value.previousAccountId;
                                      }
                                  },
                                  [&snapshot](const typed::UnknownServerRequest& value) {
                                      snapshotGenericRequest(snapshot,
                                                             value.method,
                                                             value.params,
                                                             ::ai::openai::codex::detail::safeDecodeDiagnosticText(value.diagnostic));
                                  },
                                  [&snapshot](const typed::ApplyPatchApprovalRequest& value) {
                                      snapshotGenericRequest(snapshot, "applyPatchApproval", value.params.raw);
                                      snapshot.type = "apply_patch_approval";
                                      snapshot.threadId = value.params.conversationId.value;
                                      snapshot.details["summary"] = Json::object({{"fileChangeCount", value.params.fileChanges.size()},
                                                                                  {"hasReason", value.params.reason.hasValue()},
                                                                                  {"hasGrantRoot", value.params.grantRoot.hasValue()}});
                                  },
                                  [&snapshot](const typed::ExecCommandApprovalRequest& value) {
                                      snapshotGenericRequest(snapshot, "execCommandApproval", value.params.raw);
                                      snapshot.type = "exec_command_approval";
                                      snapshot.threadId = value.params.conversationId.value;
                                      snapshot.details["summary"] = Json::object({{"commandArgumentCount", value.params.command.size()},
                                                                                  {"parsedCommandCount", value.params.parsedCommand.size()},
                                                                                  {"hasReason", value.params.reason.hasValue()},
                                                                                  {"hasApprovalId", value.params.approvalId.hasValue()}});
                                  },
                                  [&snapshot](const typed::PermissionsApprovalRequest& value) {
                                      snapshotGenericRequest(snapshot, "item/permissions/requestApproval", value.params.raw);
                                      snapshot.type = "permissions_approval";
                                      snapshot.threadId = value.params.threadId.value;
                                      snapshot.turnId = value.params.turnId.value;
                                      snapshot.itemId = value.params.itemId.value;
                                      snapshot.details["summary"] =
                                          Json::object({{"hasReason", value.params.reason.hasValue()},
                                                        {"hasEnvironmentId", value.params.environmentId.hasValue()}});
                                  },
                                  [&snapshot](const typed::AttestationGenerateRequest&) {
                                      snapshot.type = "attestation";
                                  },
                                  [&snapshot](const typed::DynamicToolCallRequest& value) {
                                      snapshot.type = "dynamic_tool_call";
                                      snapshot.threadId = value.params.threadId.value;
                                      snapshot.turnId = value.params.turnId.value;
                                      snapshot.details["tool"] = safeUtf8Prefix(value.params.tool, MaxSnapshotExtensionMethodBytes);
                                      snapshot.details["hasNamespace"] = value.params.nameSpace.hasValue();
                                      snapshot.details["argumentsOmitted"] = true;
                                  },
                                  [&snapshot](const typed::McpServerElicitationRequest& value) {
                                      snapshot.type = "mcp_elicitation";
                                      snapshot.threadId = value.params.threadId.value;
                                      if (value.params.turnId.hasValue()) {
                                          snapshot.turnId = value.params.turnId.value->value;
                                      }
                                      snapshot.details["serverName"] =
                                          safeUtf8Prefix(value.params.serverName, MaxSnapshotExtensionMethodBytes);
                                      snapshot.details["elicitationAlternative"] = value.params.elicitation.index();
                                      snapshot.details["elicitationContentOmitted"] = true;
                                  }},
                       state.request);
            snapshot.details = boundedJson(snapshot.details);
            return snapshot;
        }

        void saturatingAdd(std::uint64_t& target, std::uint64_t amount = 1) noexcept {
            target =
                amount > std::numeric_limits<std::uint64_t>::max() - target ? std::numeric_limits<std::uint64_t>::max() : target + amount;
        }

        void saturatingAddSize(std::size_t& target, std::size_t amount = 1) noexcept {
            target = amount > std::numeric_limits<std::size_t>::max() - target ? std::numeric_limits<std::size_t>::max() : target + amount;
        }

        Json sourceStampJson(const SourceStamp& stamp) {
            return Json{{"generation", stamp.generation}, {"freshness", static_cast<unsigned>(stamp.freshness)}};
        }

        template <typename T>
        Json optionalSnapshotJson(const std::optional<T>& value) {
            return value ? Json(*value) : Json(nullptr);
        }

        Json itemSnapshotJson(const ItemSnapshot& item) {
            Json encoded{{"id", item.id},
                         {"type", item.type},
                         {"status", item.status},
                         {"agentText", item.agentText},
                         {"reasoningText", item.reasoningText},
                         {"reasoningSummary", item.reasoningSummary},
                         {"commandOutput", item.commandOutput},
                         {"droppedContentBytes", item.droppedContentBytes},
                         {"contentTruncated", item.contentTruncated},
                         {"data", item.data},
                         {"extensions", item.extensions},
                         {"stamp", sourceStampJson(item.stamp)},
                         {"connectionInvalidated", item.connectionInvalidated}};
            if (item.startedAtMs) {
                encoded["startedAtMs"] = *item.startedAtMs;
            }
            if (item.completedAtMs) {
                encoded["completedAtMs"] = *item.completedAtMs;
            }
            return encoded;
        }

        Json turnSnapshotJson(const TurnSnapshot& turn) {
            Json encoded{{"id", turn.id},
                         {"threadId", turn.threadId},
                         {"status", turn.status},
                         {"active", turn.active},
                         {"terminal", turn.terminal},
                         {"items", Json::array()},
                         {"extensions", turn.extensions},
                         {"stamp", sourceStampJson(turn.stamp)},
                         {"connectionInvalidated", turn.connectionInvalidated}};
            if (turn.failure) {
                encoded["failure"] = *turn.failure;
            }
            if (turn.tokenUsage) {
                encoded["tokenUsage"] = *turn.tokenUsage;
            }
            for (const ItemSnapshot& item : turn.items) {
                encoded["items"].push_back(itemSnapshotJson(item));
            }
            return encoded;
        }

        Json threadSnapshotJson(const ThreadSnapshot& thread) {
            Json encoded{{"id", thread.id},
                         {"fullyLoaded", thread.fullyLoaded},
                         {"turns", Json::array()},
                         {"extensions", thread.extensions},
                         {"stamp", sourceStampJson(thread.stamp)},
                         {"realtime",
                          {{"lifecycle", thread.realtime.lifecycle},
                           {"transcript", thread.realtime.transcript},
                           {"itemCount", thread.realtime.itemCount},
                           {"receivedAudioBytes", thread.realtime.receivedAudioBytes},
                           {"droppedAudioBytes", thread.realtime.droppedAudioBytes},
                           {"transcriptTruncated", thread.realtime.transcriptTruncated},
                           {"stamp", sourceStampJson(thread.realtime.stamp)}}}};
            if (thread.realtime.lastError) {
                encoded["realtime"]["lastError"] = *thread.realtime.lastError;
            }
            if (thread.realtime.sessionId) {
                encoded["realtime"]["sessionId"] = *thread.realtime.sessionId;
            }
            if (thread.realtime.version) {
                encoded["realtime"]["version"] = *thread.realtime.version;
            }
            if (thread.realtime.lastSdpBytes) {
                encoded["realtime"]["lastSdpBytes"] = *thread.realtime.lastSdpBytes;
            }
            const auto assign = [&encoded](const char* name, const auto& value) {
                if (value) {
                    encoded[name] = *value;
                }
            };
            assign("title", thread.title);
            assign("cwd", thread.cwd);
            assign("model", thread.model);
            assign("modelProvider", thread.modelProvider);
            assign("preview", thread.preview);
            assign("status", thread.status);
            assign("createdAt", thread.createdAt);
            assign("updatedAt", thread.updatedAt);
            for (const TurnSnapshot& turn : thread.turns) {
                encoded["turns"].push_back(turnSnapshotJson(turn));
            }
            return encoded;
        }

        Json pendingSnapshotJson(const PendingRequestSnapshot& pending) {
            Json encoded{{"id", pending.id.value()}, {"type", pending.type}, {"details", pending.details}};
            const auto assign = [&encoded](const char* name, const auto& value) {
                if (value) {
                    encoded[name] = *value;
                }
            };
            assign("threadId", pending.threadId);
            assign("turnId", pending.turnId);
            assign("itemId", pending.itemId);
            return encoded;
        }

        Json capacitySnapshotJson(const CapacitySnapshot& capacity) {
            const BackendCapacityOptions& limits = capacity.state.limits;
            return Json{{"state",
                         {{"rejectedSessions", capacity.state.rejectedSessions},
                          {"rejectedObservers", capacity.state.rejectedObservers},
                          {"rejectedOperations", capacity.state.rejectedOperations},
                          {"providerRequestOverflows", capacity.state.providerRequestOverflows},
                          {"evictedThreads", capacity.state.evictedThreads},
                          {"evictedTurns", capacity.state.evictedTurns},
                          {"evictedItems", capacity.state.evictedItems},
                          {"droppedContentBytes", capacity.state.droppedContentBytes},
                          {"snapshotOmissions", capacity.state.snapshotOmissions},
                          {"evictedNotices", capacity.state.evictedNotices},
                          {"evictedProcesses", capacity.state.evictedProcesses},
                          {"droppedProcessOutputBytes", capacity.state.droppedProcessOutputBytes},
                          {"evictedFilesystemWatches", capacity.state.evictedFilesystemWatches},
                          {"evictedFuzzySearchSessions", capacity.state.evictedFuzzySearchSessions},
                          {"evictedActivityRecords", capacity.state.evictedActivityRecords},
                          {"limits",
                           {{"maxSessions", limits.maxSessions},
                            {"maxObservers", limits.maxObservers},
                            {"maxActiveOperations", limits.maxActiveOperations},
                            {"maxPendingRequests", limits.maxPendingRequests},
                            {"maxRetainedThreads", limits.maxRetainedThreads},
                            {"maxRetainedTurns", limits.maxRetainedTurns},
                            {"maxRetainedItems", limits.maxRetainedItems},
                            {"maxAccumulatedContentBytes", limits.maxAccumulatedContentBytes},
                            {"maxSnapshotBytes", limits.maxSnapshotBytes},
                            {"maxRetainedNotices", limits.maxRetainedNotices},
                            {"maxRetainedProcesses", limits.maxRetainedProcesses},
                            {"maxProcessOutputBytesPerProcess", limits.maxProcessOutputBytesPerProcess},
                            {"maxAccumulatedProcessOutputBytes", limits.maxAccumulatedProcessOutputBytes},
                            {"maxRetainedFilesystemWatches", limits.maxRetainedFilesystemWatches},
                            {"maxRetainedFuzzySearchSessions", limits.maxRetainedFuzzySearchSessions},
                            {"maxRetainedActivityRecords", limits.maxRetainedActivityRecords}}}}},
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
                        {"omittedThreads", capacity.omittedThreads},
                        {"omittedTurns", capacity.omittedTurns},
                        {"omittedItems", capacity.omittedItems},
                        {"sourceSessionCount", capacity.sourceSessionCount},
                        {"sourcePendingRequestCount", capacity.sourcePendingRequestCount},
                        {"truncated", capacity.truncated},
                        {"mandatoryCoreExceedsLimit", capacity.mandatoryCoreExceedsLimit}};
        }

        std::size_t accountedSnapshotBytes(const Snapshot& snapshot) noexcept {
            try {
                Json provider{{"lifecycle", static_cast<unsigned>(snapshot.provider.lifecycle)},
                              {"generation", snapshot.provider.generation},
                              {"desiredRunning", snapshot.provider.desiredRunning},
                              {"recovery",
                               {{"status", static_cast<unsigned>(snapshot.provider.recovery.status)},
                                {"attempts", snapshot.provider.recovery.attempts}}}};
                if (snapshot.provider.recovery.delayMs) {
                    provider["recovery"]["delayMs"] = *snapshot.provider.recovery.delayMs;
                }
                if (snapshot.provider.lastError) {
                    provider["lastError"] = {{"category", snapshot.provider.lastError->category},
                                             {"code", snapshot.provider.lastError->code},
                                             {"message", snapshot.provider.lastError->message}};
                }
                if (snapshot.provider.initialization) {
                    provider["initialization"] = {{"codexHome", snapshot.provider.initialization->codexHome},
                                                  {"platformFamily", snapshot.provider.initialization->platformFamily},
                                                  {"platformOs", snapshot.provider.initialization->platformOs},
                                                  {"userAgent", snapshot.provider.initialization->userAgent}};
                }

                Json encoded{{"sequence", snapshot.sequence.value()},
                             {"provider", std::move(provider)},
                             {"capacity", capacitySnapshotJson(snapshot.capacity)},
                             {"diagnostics", {{"received", snapshot.diagnostics.received}, {"recent", snapshot.diagnostics.recent}}},
                             {"threads", Json::array()},
                             {"pendingRequests", Json::array()},
                             {"sessions", Json::array()},
                             {"threadList",
                              {{"hasLoadedPage", snapshot.threadList.hasLoadedPage},
                               {"complete", snapshot.threadList.complete},
                               {"pagesLoaded", snapshot.threadList.pagesLoaded},
                               {"stamp", sourceStampJson(snapshot.threadList.stamp)}}},
                             {"providerOperations", Json::array()},
                             {"domains", Json::object()},
                             {"notices", Json::array()},
                             {"processes", Json::array()},
                             {"filesystemWatches", Json::array()},
                             {"fuzzySearchSessions", Json::array()},
                             {"activities", Json::array()},
                             {"recentExtensions", Json::array()},
                             {"omittedRecentExtensions", snapshot.omittedRecentExtensions},
                             {"sequenceExhausted", snapshot.sequenceExhausted}};
                if (snapshot.controller) {
                    encoded["controller"] = snapshot.controller->value();
                }
                if (snapshot.threadList.nextCursor) {
                    encoded["threadList"]["nextCursor"] = *snapshot.threadList.nextCursor;
                }
                if (snapshot.threadList.backwardsCursor) {
                    encoded["threadList"]["backwardsCursor"] = *snapshot.threadList.backwardsCursor;
                }
                for (const ThreadSnapshot& thread : snapshot.threads) {
                    encoded["threads"].push_back(threadSnapshotJson(thread));
                }
                for (const PendingRequestSnapshot& pending : snapshot.pendingRequests) {
                    encoded["pendingRequests"].push_back(pendingSnapshotJson(pending));
                }
                for (const SessionSnapshot& session : snapshot.sessions) {
                    encoded["sessions"].push_back({{"id", session.id.value()}, {"role", static_cast<unsigned>(session.role)}});
                }
                for (const ProviderOperationSnapshot& operation : snapshot.providerOperations) {
                    encoded["providerOperations"].push_back({{"method", operation.method},
                                                             {"resultAlternative", operation.resultAlternative},
                                                             {"stamp", sourceStampJson(operation.stamp)}});
                }
                const auto encodeDomain = [&encoded](const char* name, const ProviderDomainSnapshot& domain) {
                    Json projected{{"latestNotificationMethods", domain.latestNotificationMethods},
                                   {"latestNotifications", Json::array()},
                                   {"latestResults", Json::array()}};
                    for (const ProviderNotificationSnapshot& notification : domain.latestNotifications) {
                        projected["latestNotifications"].push_back({{"method", notification.method},
                                                                    {"eventAlternative", notification.eventAlternative},
                                                                    {"stamp", sourceStampJson(notification.stamp)}});
                    }
                    for (const ProviderResultSummarySnapshot& result : domain.latestResults) {
                        Json encodedResult{{"method", result.method},
                                           {"resultAlternative", result.resultAlternative},
                                           {"status", result.status},
                                           {"itemCount", result.itemCount},
                                           {"complete", result.complete},
                                           {"stamp", sourceStampJson(result.stamp)}};
                        if (result.subjectId) {
                            encodedResult["subjectId"] = *result.subjectId;
                        }
                        if (result.nextCursor) {
                            encodedResult["nextCursor"] = *result.nextCursor;
                        }
                        projected["latestResults"].push_back(std::move(encodedResult));
                    }
                    encoded["domains"][name] = std::move(projected);
                };
                encodeDomain("accounts", snapshot.accounts);
                encodeDomain("models", snapshot.models);
                encodeDomain("configuration", snapshot.configuration);
                encodeDomain("conversations", snapshot.conversations);
                encodeDomain("filesystem", snapshot.filesystem);
                encodeDomain("reviews", snapshot.reviews);
                encodeDomain("integrations", snapshot.integrations);
                encodeDomain("pluginsAndSkills", snapshot.pluginsAndSkills);
                encodeDomain("mcp", snapshot.mcp);
                encodeDomain("platform", snapshot.platform);
                if (snapshot.accounts.login) {
                    Json login{{"lifecycle", snapshot.accounts.login->lifecycle},
                               {"method", snapshot.accounts.login->method},
                               {"stamp", sourceStampJson(snapshot.accounts.login->stamp)}};
                    if (snapshot.accounts.login->loginId) {
                        login["loginId"] = snapshot.accounts.login->loginId->value;
                    }
                    if (snapshot.accounts.login->cancellationStatus) {
                        login["cancellationStatus"] = *snapshot.accounts.login->cancellationStatus;
                    }
                    if (snapshot.accounts.login->success) {
                        login["success"] = *snapshot.accounts.login->success;
                    }
                    if (snapshot.accounts.login->error) {
                        login["error"] = *snapshot.accounts.login->error;
                    }
                    encoded["domains"]["accounts"]["login"] = std::move(login);
                }
                if (snapshot.accounts.authentication) {
                    Json authentication{{"authenticated", snapshot.accounts.authentication->authenticated},
                                        {"stamp", sourceStampJson(snapshot.accounts.authentication->stamp)}};
                    if (snapshot.accounts.authentication->accountType) {
                        authentication["accountType"] = *snapshot.accounts.authentication->accountType;
                    }
                    if (snapshot.accounts.authentication->authMode) {
                        authentication["authMode"] = *snapshot.accounts.authentication->authMode;
                    }
                    if (snapshot.accounts.authentication->planType) {
                        authentication["planType"] = *snapshot.accounts.authentication->planType;
                    }
                    encoded["domains"]["accounts"]["authentication"] = std::move(authentication);
                }
                if (snapshot.accounts.rateLimits) {
                    encoded["domains"]["accounts"]["rateLimits"] = {
                        {"primaryUsedPercent", optionalSnapshotJson(snapshot.accounts.rateLimits->primaryUsedPercent)},
                        {"primaryResetsAt", optionalSnapshotJson(snapshot.accounts.rateLimits->primaryResetsAt)},
                        {"secondaryUsedPercent", optionalSnapshotJson(snapshot.accounts.rateLimits->secondaryUsedPercent)},
                        {"secondaryResetsAt", optionalSnapshotJson(snapshot.accounts.rateLimits->secondaryResetsAt)},
                        {"hasCredits", optionalSnapshotJson(snapshot.accounts.rateLimits->hasCredits)},
                        {"unlimitedCredits", optionalSnapshotJson(snapshot.accounts.rateLimits->unlimitedCredits)},
                        {"stamp", sourceStampJson(snapshot.accounts.rateLimits->stamp)}};
                }
                encoded["domains"]["accounts"]["loggedOut"] = snapshot.accounts.loggedOut;
                if (snapshot.configuration.lastWrite) {
                    encoded["domains"]["configuration"]["lastWrite"] = {
                        {"filePath", snapshot.configuration.lastWrite->filePath},
                        {"status", snapshot.configuration.lastWrite->status},
                        {"version", snapshot.configuration.lastWrite->version},
                        {"overridden", snapshot.configuration.lastWrite->overridden},
                        {"truncated", snapshot.configuration.lastWrite->truncated},
                        {"stamp", sourceStampJson(snapshot.configuration.lastWrite->stamp)}};
                }
                if (snapshot.configuration.experimentalFeatureEnablement) {
                    Json enablement{{"totalEntries", snapshot.configuration.experimentalFeatureEnablement->totalEntries},
                                    {"truncated", snapshot.configuration.experimentalFeatureEnablement->truncated},
                                    {"stamp", sourceStampJson(snapshot.configuration.experimentalFeatureEnablement->stamp)},
                                    {"entries", Json::array()}};
                    for (const auto& [feature, enabled] : snapshot.configuration.experimentalFeatureEnablement->entries) {
                        enablement["entries"].push_back({{"feature", feature}, {"enabled", enabled}});
                    }
                    encoded["domains"]["configuration"]["experimentalFeatureEnablement"] = std::move(enablement);
                }
                const auto encodeGoalMutation = [&encoded](const char* name, const ConversationDomainSnapshot::GoalMutation& mutation) {
                    Json value{
                        {"operation", mutation.operation}, {"threadId", mutation.threadId}, {"stamp", sourceStampJson(mutation.stamp)}};
                    if (mutation.objective) {
                        value["objective"] = *mutation.objective;
                    }
                    if (mutation.status) {
                        value["status"] = *mutation.status;
                    }
                    if (mutation.cleared) {
                        value["cleared"] = *mutation.cleared;
                    }
                    encoded["domains"]["conversations"][name] = std::move(value);
                };
                if (snapshot.conversations.latestGoal) {
                    encodeGoalMutation("latestGoal", *snapshot.conversations.latestGoal);
                }
                if (snapshot.conversations.latestGoalClear) {
                    encodeGoalMutation("latestGoalClear", *snapshot.conversations.latestGoalClear);
                }
                if (snapshot.conversations.latestGoalSet) {
                    encodeGoalMutation("latestGoalSet", *snapshot.conversations.latestGoalSet);
                }
                if (snapshot.conversations.latestUnsubscribe) {
                    encodeGoalMutation("latestUnsubscribe", *snapshot.conversations.latestUnsubscribe);
                }
                if (snapshot.integrations.apps) {
                    Json apps{{"totalEntries", snapshot.integrations.apps->totalEntries},
                              {"truncated", snapshot.integrations.apps->truncated},
                              {"stamp", sourceStampJson(snapshot.integrations.apps->stamp)},
                              {"entries", Json::array()}};
                    for (const AppCatalogEntryState& app : snapshot.integrations.apps->entries) {
                        apps["entries"].push_back({{"id", app.id},
                                                   {"name", app.name},
                                                   {"accessible", optionalSnapshotJson(app.accessible)},
                                                   {"enabled", optionalSnapshotJson(app.enabled)}});
                    }
                    encoded["domains"]["integrations"]["apps"] = std::move(apps);
                }
                const auto encodeMarketplaceMutation = [&encoded](const char* name,
                                                                  const IntegrationsDomainSnapshot::MarketplaceMutation& mutation) {
                    Json value{{"operation", mutation.operation},
                               {"selectedCount", mutation.selectedCount},
                               {"upgradedRootCount", mutation.upgradedRootCount},
                               {"errorCount", mutation.errorCount},
                               {"alreadyAdded", mutation.alreadyAdded},
                               {"truncated", mutation.truncated},
                               {"stamp", sourceStampJson(mutation.stamp)}};
                    if (mutation.marketplaceName) {
                        value["marketplaceName"] = *mutation.marketplaceName;
                    }
                    if (mutation.installedRoot) {
                        value["installedRoot"] = *mutation.installedRoot;
                    }
                    encoded["domains"]["integrations"][name] = std::move(value);
                };
                if (snapshot.integrations.marketplaceAdd) {
                    encodeMarketplaceMutation("marketplaceAdd", *snapshot.integrations.marketplaceAdd);
                }
                if (snapshot.integrations.marketplaceRemove) {
                    encodeMarketplaceMutation("marketplaceRemove", *snapshot.integrations.marketplaceRemove);
                }
                if (snapshot.integrations.marketplaceUpgrade) {
                    encodeMarketplaceMutation("marketplaceUpgrade", *snapshot.integrations.marketplaceUpgrade);
                }
                const auto encodePluginMutation = [&encoded](const char* name, const PluginsAndSkillsDomainSnapshot::Mutation& mutation) {
                    Json value{{"operation", mutation.operation},
                               {"itemCount", mutation.itemCount},
                               {"truncated", mutation.truncated},
                               {"stamp", sourceStampJson(mutation.stamp)}};
                    if (mutation.subjectId) {
                        value["subjectId"] = *mutation.subjectId;
                    }
                    if (mutation.status) {
                        value["status"] = *mutation.status;
                    }
                    encoded["domains"]["pluginsAndSkills"][name] = std::move(value);
                };
                if (snapshot.pluginsAndSkills.pluginInstall) {
                    encodePluginMutation("pluginInstall", *snapshot.pluginsAndSkills.pluginInstall);
                }
                if (snapshot.pluginsAndSkills.pluginShareCheckout) {
                    encodePluginMutation("pluginShareCheckout", *snapshot.pluginsAndSkills.pluginShareCheckout);
                }
                if (snapshot.pluginsAndSkills.pluginShareSave) {
                    encodePluginMutation("pluginShareSave", *snapshot.pluginsAndSkills.pluginShareSave);
                }
                if (snapshot.pluginsAndSkills.pluginShareUpdateTargets) {
                    encodePluginMutation("pluginShareUpdateTargets", *snapshot.pluginsAndSkills.pluginShareUpdateTargets);
                }
                if (snapshot.pluginsAndSkills.skillsConfigWrite) {
                    encodePluginMutation("skillsConfigWrite", *snapshot.pluginsAndSkills.skillsConfigWrite);
                }
                if (snapshot.pluginsAndSkills.extraRoots) {
                    Json roots{{"totalRoots", snapshot.pluginsAndSkills.extraRoots->totalRoots},
                               {"truncated", snapshot.pluginsAndSkills.extraRoots->truncated},
                               {"stamp", sourceStampJson(snapshot.pluginsAndSkills.extraRoots->stamp)},
                               {"roots", Json::array()}};
                    for (const typed::AbsolutePath& root : snapshot.pluginsAndSkills.extraRoots->roots) {
                        roots["roots"].push_back(root.value);
                    }
                    encoded["domains"]["pluginsAndSkills"]["extraRoots"] = std::move(roots);
                }
                if (snapshot.mcp.oauth) {
                    encoded["domains"]["mcp"]["oauth"] = {{"serverName", snapshot.mcp.oauth->serverName},
                                                          {"lifecycle", snapshot.mcp.oauth->lifecycle},
                                                          {"success", optionalSnapshotJson(snapshot.mcp.oauth->success)},
                                                          {"error", optionalSnapshotJson(snapshot.mcp.oauth->error)},
                                                          {"stamp", sourceStampJson(snapshot.mcp.oauth->stamp)}};
                }
                if (snapshot.mcp.startup) {
                    encoded["domains"]["mcp"]["startup"] = {{"serverName", snapshot.mcp.startup->serverName},
                                                            {"status", snapshot.mcp.startup->status},
                                                            {"error", optionalSnapshotJson(snapshot.mcp.startup->error)},
                                                            {"failureReason", optionalSnapshotJson(snapshot.mcp.startup->failureReason)},
                                                            {"stamp", sourceStampJson(snapshot.mcp.startup->stamp)}};
                }
                if (snapshot.mcp.statusList) {
                    encoded["domains"]["mcp"]["statusList"] = {{"serverCount", snapshot.mcp.statusList->serverCount},
                                                               {"nextCursor", optionalSnapshotJson(snapshot.mcp.statusList->nextCursor)},
                                                               {"complete", snapshot.mcp.statusList->complete},
                                                               {"stamp", sourceStampJson(snapshot.mcp.statusList->stamp)}};
                }
                if (snapshot.platform.remoteControl) {
                    encoded["domains"]["platform"]["remoteControl"] = {
                        {"status", snapshot.platform.remoteControl->status},
                        {"environmentId", optionalSnapshotJson(snapshot.platform.remoteControl->environmentId)},
                        {"installationId", snapshot.platform.remoteControl->installationId},
                        {"serverName", snapshot.platform.remoteControl->serverName},
                        {"stamp", sourceStampJson(snapshot.platform.remoteControl->stamp)}};
                }
                if (snapshot.platform.windowsSandbox) {
                    encoded["domains"]["platform"]["windowsSandbox"] = {
                        {"lifecycle", snapshot.platform.windowsSandbox->lifecycle},
                        {"readiness", optionalSnapshotJson(snapshot.platform.windowsSandbox->readiness)},
                        {"mode", optionalSnapshotJson(snapshot.platform.windowsSandbox->mode)},
                        {"success", optionalSnapshotJson(snapshot.platform.windowsSandbox->success)},
                        {"error", optionalSnapshotJson(snapshot.platform.windowsSandbox->error)},
                        {"stamp", sourceStampJson(snapshot.platform.windowsSandbox->stamp)}};
                }
                for (const NoticeSnapshot& notice : snapshot.notices) {
                    Json encodedNotice{{"occurrence", notice.occurrence},
                                       {"category", static_cast<unsigned>(notice.category)},
                                       {"summary", notice.summary},
                                       {"stamp", sourceStampJson(notice.stamp)}};
                    if (notice.details) {
                        encodedNotice["details"] = *notice.details;
                    }
                    if (notice.threadId) {
                        encodedNotice["threadId"] = *notice.threadId;
                    }
                    encoded["notices"].push_back(std::move(encodedNotice));
                }
                for (const ProcessSnapshot& process : snapshot.processes) {
                    Json encodedProcess{{"processHandle", process.processHandle},
                                        {"lifecycle", process.lifecycle},
                                        {"stdoutBytes", process.stdoutBytes},
                                        {"stderrBytes", process.stderrBytes},
                                        {"stdoutTruncated", process.stdoutTruncated},
                                        {"stderrTruncated", process.stderrTruncated},
                                        {"droppedOutputBytes", process.droppedOutputBytes},
                                        {"stamp", sourceStampJson(process.stamp)},
                                        {"connectionInvalidated", process.connectionInvalidated}};
                    if (process.exitCode) {
                        encodedProcess["exitCode"] = *process.exitCode;
                    }
                    encoded["processes"].push_back(std::move(encodedProcess));
                }
                for (const FilesystemWatchSnapshot& watch : snapshot.filesystemWatches) {
                    Json encodedWatch{{"watchId", watch.watchId},
                                      {"changedPathCount", watch.changedPathCount},
                                      {"stamp", sourceStampJson(watch.stamp)},
                                      {"connectionInvalidated", watch.connectionInvalidated}};
                    if (watch.root) {
                        encodedWatch["root"] = *watch.root;
                    }
                    encoded["filesystemWatches"].push_back(std::move(encodedWatch));
                }
                for (const FuzzySearchSnapshot& search : snapshot.fuzzySearchSessions) {
                    encoded["fuzzySearchSessions"].push_back({{"sessionId", search.sessionId},
                                                              {"resultCount", search.resultCount},
                                                              {"complete", search.complete},
                                                              {"stamp", sourceStampJson(search.stamp)},
                                                              {"connectionInvalidated", search.connectionInvalidated}});
                }
                for (const ActivitySnapshot& activity : snapshot.activities) {
                    Json encodedActivity{{"key", activity.key},
                                         {"subjectId", activity.subjectId},
                                         {"kind", activity.kind},
                                         {"lifecycle", activity.lifecycle},
                                         {"stamp", sourceStampJson(activity.stamp)},
                                         {"active", activity.active}};
                    if (activity.summary) {
                        encodedActivity["summary"] = *activity.summary;
                    }
                    if (activity.details) {
                        encodedActivity["details"] = *activity.details;
                    }
                    if (activity.threadId) {
                        encodedActivity["threadId"] = *activity.threadId;
                    }
                    if (activity.turnId) {
                        encodedActivity["turnId"] = *activity.turnId;
                    }
                    encoded["activities"].push_back(std::move(encodedActivity));
                }
                for (const ExtensionSnapshot& extension : snapshot.recentExtensions) {
                    Json value{{"method", extension.method},
                               {"payload", extension.payload},
                               {"methodTruncated", extension.methodTruncated},
                               {"payloadTruncated", extension.payloadTruncated},
                               {"decodingErrorTruncated", extension.decodingErrorTruncated},
                               {"sensitiveFieldsRedacted", extension.sensitiveFieldsRedacted},
                               {"originalMethodBytes", extension.originalMethodBytes},
                               {"originalDecodingErrorBytes", extension.originalDecodingErrorBytes}};
                    if (extension.decodingError) {
                        value["decodingError"] = *extension.decodingError;
                    }
                    if (extension.originalPayloadBytes) {
                        value["originalPayloadBytes"] = *extension.originalPayloadBytes;
                    }
                    encoded["recentExtensions"].push_back(std::move(value));
                }
                return encoded.dump().size();
            } catch (...) {
                return std::numeric_limits<std::size_t>::max();
            }
        }

        struct SnapshotPendingReferences {
            std::set<std::string> threads;
            std::set<std::pair<std::string, std::string>> turns;
            std::set<std::tuple<std::string, std::string, std::string>> items;
        };

        SnapshotPendingReferences pendingReferences(const Snapshot& snapshot) {
            SnapshotPendingReferences references;
            for (const PendingRequestSnapshot& pending : snapshot.pendingRequests) {
                if (!pending.threadId) {
                    continue;
                }
                references.threads.insert(*pending.threadId);
                if (!pending.turnId) {
                    continue;
                }
                references.turns.emplace(*pending.threadId, *pending.turnId);
                if (pending.itemId) {
                    references.items.emplace(*pending.threadId, *pending.turnId, *pending.itemId);
                }
            }
            return references;
        }

        void accountOmittedThread(CapacitySnapshot& capacity, const ThreadSnapshot& thread) noexcept {
            saturatingAddSize(capacity.omittedThreads);
            saturatingAddSize(capacity.omittedTurns, thread.turns.size());
            for (const TurnSnapshot& turn : thread.turns) {
                saturatingAddSize(capacity.omittedItems, turn.items.size());
            }
        }

        bool snapshotItemIsActive(const ItemSnapshot& item) noexcept {
            return item.connectionInvalidated || item.status == "started" || item.status == "unknown";
        }

        bool snapshotTurnIsActive(const TurnSnapshot& turn) noexcept {
            return turn.active || !turn.terminal || turn.connectionInvalidated ||
                   std::any_of(turn.items.begin(), turn.items.end(), snapshotItemIsActive);
        }

        std::size_t conservativeRemovalCredit(const Json& value) noexcept {
            try {
                const std::size_t bytes = value.dump().size();
                // Array punctuation and the decimal growth of omission
                // counters remain in the enclosing snapshot. Reserve ample
                // space for that metadata while still avoiding a full
                // snapshot serialization after every removed entry.
                return bytes > 64 ? bytes - 64 : 1;
            } catch (...) {
                return 1;
            }
        }

        void applyRemovalCredit(std::size_t& estimatedBytes, std::size_t credit) noexcept {
            estimatedBytes = credit >= estimatedBytes ? 0 : estimatedBytes - credit;
        }

        Json extensionSnapshotAccountingJson(const ExtensionSnapshot& extension) {
            Json value{{"method", extension.method},
                       {"payload", extension.payload},
                       {"methodTruncated", extension.methodTruncated},
                       {"payloadTruncated", extension.payloadTruncated},
                       {"decodingErrorTruncated", extension.decodingErrorTruncated},
                       {"sensitiveFieldsRedacted", extension.sensitiveFieldsRedacted},
                       {"originalMethodBytes", extension.originalMethodBytes},
                       {"originalDecodingErrorBytes", extension.originalDecodingErrorBytes}};
            if (extension.decodingError) {
                value["decodingError"] = *extension.decodingError;
            }
            if (extension.originalPayloadBytes) {
                value["originalPayloadBytes"] = *extension.originalPayloadBytes;
            }
            return value;
        }

        std::size_t itemRemovalCredit(const ItemSnapshot& item) noexcept {
            try {
                return conservativeRemovalCredit(itemSnapshotJson(item));
            } catch (...) {
                return 1;
            }
        }

        std::size_t turnRemovalCredit(const TurnSnapshot& turn) noexcept {
            try {
                return conservativeRemovalCredit(turnSnapshotJson(turn));
            } catch (...) {
                return 1;
            }
        }

        std::size_t threadRemovalCredit(const ThreadSnapshot& thread) noexcept {
            try {
                return conservativeRemovalCredit(threadSnapshotJson(thread));
            } catch (...) {
                return 1;
            }
        }

        std::size_t extensionRemovalCredit(const ExtensionSnapshot& extension) noexcept {
            try {
                return conservativeRemovalCredit(extensionSnapshotAccountingJson(extension));
            } catch (...) {
                return 1;
            }
        }

        std::size_t diagnosticRemovalCredit(const std::string& diagnostic) noexcept {
            try {
                return conservativeRemovalCredit(Json(diagnostic));
            } catch (...) {
                return 1;
            }
        }

        std::size_t providerOperationRemovalCredit(const ProviderOperationSnapshot& operation) noexcept {
            try {
                return conservativeRemovalCredit(Json{{"method", operation.method},
                                                      {"resultAlternative", operation.resultAlternative},
                                                      {"stamp", sourceStampJson(operation.stamp)}});
            } catch (...) {
                return 1;
            }
        }

        std::size_t noticeRemovalCredit(const NoticeSnapshot& notice) noexcept {
            try {
                Json encoded{{"occurrence", notice.occurrence},
                             {"category", static_cast<unsigned>(notice.category)},
                             {"summary", notice.summary},
                             {"stamp", sourceStampJson(notice.stamp)}};
                if (notice.details) {
                    encoded["details"] = *notice.details;
                }
                if (notice.threadId) {
                    encoded["threadId"] = *notice.threadId;
                }
                return conservativeRemovalCredit(encoded);
            } catch (...) {
                return 1;
            }
        }

        std::size_t processRemovalCredit(const ProcessSnapshot& process) noexcept {
            try {
                Json encoded{{"processHandle", process.processHandle},
                             {"lifecycle", process.lifecycle},
                             {"stdoutBytes", process.stdoutBytes},
                             {"stderrBytes", process.stderrBytes},
                             {"stdoutTruncated", process.stdoutTruncated},
                             {"stderrTruncated", process.stderrTruncated},
                             {"droppedOutputBytes", process.droppedOutputBytes},
                             {"stamp", sourceStampJson(process.stamp)},
                             {"connectionInvalidated", process.connectionInvalidated}};
                if (process.exitCode) {
                    encoded["exitCode"] = *process.exitCode;
                }
                return conservativeRemovalCredit(encoded);
            } catch (...) {
                return 1;
            }
        }

        std::size_t filesystemWatchRemovalCredit(const FilesystemWatchSnapshot& watch) noexcept {
            try {
                Json encoded{{"watchId", watch.watchId},
                             {"changedPathCount", watch.changedPathCount},
                             {"stamp", sourceStampJson(watch.stamp)},
                             {"connectionInvalidated", watch.connectionInvalidated}};
                if (watch.root) {
                    encoded["root"] = *watch.root;
                }
                return conservativeRemovalCredit(encoded);
            } catch (...) {
                return 1;
            }
        }

        std::size_t fuzzySearchRemovalCredit(const FuzzySearchSnapshot& search) noexcept {
            try {
                return conservativeRemovalCredit(Json{{"sessionId", search.sessionId},
                                                      {"resultCount", search.resultCount},
                                                      {"complete", search.complete},
                                                      {"stamp", sourceStampJson(search.stamp)},
                                                      {"connectionInvalidated", search.connectionInvalidated}});
            } catch (...) {
                return 1;
            }
        }

        std::size_t activityRemovalCredit(const ActivitySnapshot& activity) noexcept {
            try {
                Json encoded{{"key", activity.key},
                             {"subjectId", activity.subjectId},
                             {"kind", activity.kind},
                             {"lifecycle", activity.lifecycle},
                             {"stamp", sourceStampJson(activity.stamp)},
                             {"active", activity.active}};
                if (activity.summary) {
                    encoded["summary"] = *activity.summary;
                }
                if (activity.details) {
                    encoded["details"] = *activity.details;
                }
                if (activity.threadId) {
                    encoded["threadId"] = *activity.threadId;
                }
                if (activity.turnId) {
                    encoded["turnId"] = *activity.turnId;
                }
                return conservativeRemovalCredit(encoded);
            } catch (...) {
                return 1;
            }
        }

        std::size_t domainMethodRemovalCredit(const std::string& method) noexcept {
            try {
                return conservativeRemovalCredit(Json(method));
            } catch (...) {
                return 1;
            }
        }

        std::size_t domainNotificationRemovalCredit(const ProviderNotificationSnapshot& notification) noexcept {
            try {
                return conservativeRemovalCredit(Json{{"method", notification.method},
                                                      {"eventAlternative", notification.eventAlternative},
                                                      {"stamp", sourceStampJson(notification.stamp)}});
            } catch (...) {
                return 1;
            }
        }

        std::size_t domainResultRemovalCredit(const ProviderResultSummarySnapshot& result) noexcept {
            try {
                Json encoded{{"method", result.method},
                             {"resultAlternative", result.resultAlternative},
                             {"status", result.status},
                             {"itemCount", result.itemCount},
                             {"complete", result.complete},
                             {"stamp", sourceStampJson(result.stamp)}};
                if (result.subjectId) {
                    encoded["subjectId"] = *result.subjectId;
                }
                if (result.nextCursor) {
                    encoded["nextCursor"] = *result.nextCursor;
                }
                return conservativeRemovalCredit(encoded);
            } catch (...) {
                return 1;
            }
        }

        void minimizePendingRequest(PendingRequestSnapshot& pending) {
            pending.type = safeUtf8Prefix(pending.type, MaxSnapshotExtensionMethodBytes);
            const auto boundAssociation = [](std::optional<std::string>& value) {
                if (value) {
                    *value = safeUtf8Prefix(*value, MaxSnapshotExtensionMethodBytes);
                }
            };
            boundAssociation(pending.threadId);
            boundAssociation(pending.turnId);
            boundAssociation(pending.itemId);
            pending.details = Json::object({{"omitted", true}, {"reason", "minimal snapshot"}});
        }

        std::uint64_t saturatedSize(std::size_t value) noexcept {
            if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
                if (value > std::numeric_limits<std::uint64_t>::max()) {
                    return std::numeric_limits<std::uint64_t>::max();
                }
            }
            return static_cast<std::uint64_t>(value);
        }

        void collapseToMinimalSnapshot(Snapshot& snapshot) noexcept {
            try {
                for (const ThreadSnapshot& thread : snapshot.threads) {
                    accountOmittedThread(snapshot.capacity, thread);
                }
                snapshot.threads.clear();
                for (PendingRequestSnapshot& pending : snapshot.pendingRequests) {
                    minimizePendingRequest(pending);
                }
                snapshot.diagnostics.recent.clear();
                std::size_t optionalOmissions = 0;
                const auto accountOptionalEntries = [&optionalOmissions](std::size_t count) {
                    const std::size_t available = std::numeric_limits<std::size_t>::max() - optionalOmissions;
                    optionalOmissions += std::min(available, count);
                };
                accountOptionalEntries(snapshot.providerOperations.size());
                accountOptionalEntries(snapshot.notices.size());
                accountOptionalEntries(snapshot.processes.size());
                accountOptionalEntries(snapshot.filesystemWatches.size());
                accountOptionalEntries(snapshot.fuzzySearchSessions.size());
                accountOptionalEntries(snapshot.activities.size());
                const auto accountDomainMethods = [&accountOptionalEntries](const ProviderDomainSnapshot& domain) {
                    accountOptionalEntries(domain.latestNotificationMethods.size());
                    accountOptionalEntries(domain.latestNotifications.size());
                    accountOptionalEntries(domain.latestResults.size());
                };
                accountDomainMethods(snapshot.accounts);
                accountDomainMethods(snapshot.models);
                accountDomainMethods(snapshot.configuration);
                accountDomainMethods(snapshot.conversations);
                accountDomainMethods(snapshot.filesystem);
                accountDomainMethods(snapshot.reviews);
                accountDomainMethods(snapshot.integrations);
                accountDomainMethods(snapshot.pluginsAndSkills);
                accountDomainMethods(snapshot.mcp);
                accountDomainMethods(snapshot.platform);
                snapshot.providerOperations.clear();
                snapshot.accounts = {};
                snapshot.models = {};
                snapshot.configuration = {};
                snapshot.conversations = {};
                snapshot.filesystem = {};
                snapshot.reviews = {};
                snapshot.integrations = {};
                snapshot.pluginsAndSkills = {};
                snapshot.mcp = {};
                snapshot.platform = {};
                snapshot.notices.clear();
                snapshot.processes.clear();
                snapshot.filesystemWatches.clear();
                snapshot.fuzzySearchSessions.clear();
                snapshot.activities.clear();
                saturatingAddSize(snapshot.omittedRecentExtensions, snapshot.recentExtensions.size());
                snapshot.recentExtensions.clear();
                snapshot.threadList = {};
                snapshot.provider.initialization.reset();
                snapshot.capacity.truncated = true;
                saturatingAdd(snapshot.capacity.state.snapshotOmissions, saturatedSize(optionalOmissions));
                saturatingAdd(snapshot.capacity.state.snapshotOmissions);
            } catch (...) {
                snapshot.capacity.truncated = true;
                saturatingAdd(snapshot.capacity.state.snapshotOmissions);
            }
        }

        void boundSnapshot(Snapshot& snapshot, std::size_t limit) noexcept {
            try {
                std::size_t estimatedBytes = accountedSnapshotBytes(snapshot);
                bool collapsedToMinimal = false;
                if (estimatedBytes == std::numeric_limits<std::size_t>::max()) {
                    collapseToMinimalSnapshot(snapshot);
                    collapsedToMinimal = true;
                    estimatedBytes = accountedSnapshotBytes(snapshot);
                }

                const auto recordOmission = [&snapshot]() {
                    snapshot.capacity.truncated = true;
                    saturatingAdd(snapshot.capacity.state.snapshotOmissions);
                };
                const SnapshotPendingReferences referenced = pendingReferences(snapshot);
                const auto omitOptionalEntries = [&snapshot, &estimatedBytes, limit, &recordOmission]<typename Value>(
                                                     std::vector<Value>& values, const auto& mayOmit, const auto& removalCredit) {
                    if (estimatedBytes <= limit) {
                        return;
                    }
                    std::vector<Value> retained;
                    retained.reserve(values.size());
                    for (Value& value : values) {
                        if (estimatedBytes > limit && mayOmit(value)) {
                            applyRemovalCredit(estimatedBytes, removalCredit(value));
                            recordOmission();
                        } else {
                            retained.push_back(std::move(value));
                        }
                    }
                    values = std::move(retained);
                    estimatedBytes = accountedSnapshotBytes(snapshot);
                };

                // Each phase walks its vectors once and applies conservative
                // per-entry credits. This keeps bounding linear in the number of
                // projected entities instead of repeatedly serializing the whole
                // snapshot after every omission.
                if (estimatedBytes > limit) {
                    for (ThreadSnapshot& thread : snapshot.threads) {
                        for (TurnSnapshot& turn : thread.turns) {
                            std::vector<ItemSnapshot> retained;
                            retained.reserve(turn.items.size());
                            for (ItemSnapshot& item : turn.items) {
                                if (estimatedBytes > limit && !snapshotItemIsActive(item) &&
                                    !referenced.items.contains({thread.id, turn.id, item.id})) {
                                    applyRemovalCredit(estimatedBytes, itemRemovalCredit(item));
                                    saturatingAddSize(snapshot.capacity.omittedItems);
                                    recordOmission();
                                } else {
                                    retained.push_back(std::move(item));
                                }
                            }
                            turn.items = std::move(retained);
                        }
                    }
                    estimatedBytes = accountedSnapshotBytes(snapshot);
                }

                if (estimatedBytes > limit) {
                    for (ThreadSnapshot& thread : snapshot.threads) {
                        std::vector<TurnSnapshot> retained;
                        retained.reserve(thread.turns.size());
                        for (TurnSnapshot& turn : thread.turns) {
                            if (estimatedBytes > limit && !snapshotTurnIsActive(turn) && !referenced.turns.contains({thread.id, turn.id})) {
                                applyRemovalCredit(estimatedBytes, turnRemovalCredit(turn));
                                saturatingAddSize(snapshot.capacity.omittedTurns);
                                saturatingAddSize(snapshot.capacity.omittedItems, turn.items.size());
                                recordOmission();
                            } else {
                                retained.push_back(std::move(turn));
                            }
                        }
                        thread.turns = std::move(retained);
                    }
                    estimatedBytes = accountedSnapshotBytes(snapshot);
                }

                if (estimatedBytes > limit) {
                    std::vector<ThreadSnapshot> retained;
                    retained.reserve(snapshot.threads.size());
                    for (ThreadSnapshot& thread : snapshot.threads) {
                        if (estimatedBytes > limit && std::none_of(thread.turns.begin(), thread.turns.end(), snapshotTurnIsActive) &&
                            !referenced.threads.contains(thread.id)) {
                            applyRemovalCredit(estimatedBytes, threadRemovalCredit(thread));
                            accountOmittedThread(snapshot.capacity, thread);
                            recordOmission();
                        } else {
                            retained.push_back(std::move(thread));
                        }
                    }
                    snapshot.threads = std::move(retained);
                    estimatedBytes = accountedSnapshotBytes(snapshot);
                }

                // A1.6b domain projections are optional snapshot material.
                // Omit them deterministically before falling back to the
                // mandatory provider/controller/capacity/request core. Active
                // provider-scoped resources remain protected until that final
                // minimal fallback is unavoidable.
                omitOptionalEntries(
                    snapshot.notices,
                    [](const NoticeSnapshot&) {
                        return true;
                    },
                    noticeRemovalCredit);
                omitOptionalEntries(
                    snapshot.processes,
                    [](const ProcessSnapshot& process) {
                        return process.lifecycle == "exited" || process.connectionInvalidated;
                    },
                    processRemovalCredit);
                omitOptionalEntries(
                    snapshot.filesystemWatches,
                    [](const FilesystemWatchSnapshot& watch) {
                        return watch.connectionInvalidated;
                    },
                    filesystemWatchRemovalCredit);
                omitOptionalEntries(
                    snapshot.fuzzySearchSessions,
                    [](const FuzzySearchSnapshot& search) {
                        return search.complete || search.connectionInvalidated;
                    },
                    fuzzySearchRemovalCredit);
                omitOptionalEntries(
                    snapshot.activities,
                    [](const ActivitySnapshot& activity) {
                        return !activity.active;
                    },
                    activityRemovalCredit);
                omitOptionalEntries(
                    snapshot.providerOperations,
                    [](const ProviderOperationSnapshot&) {
                        return true;
                    },
                    providerOperationRemovalCredit);

                const auto omitDomainMethods = [&omitOptionalEntries](ProviderDomainSnapshot& domain) {
                    omitOptionalEntries(
                        domain.latestNotificationMethods,
                        [](const std::string&) {
                            return true;
                        },
                        domainMethodRemovalCredit);
                    omitOptionalEntries(
                        domain.latestNotifications,
                        [](const ProviderNotificationSnapshot&) {
                            return true;
                        },
                        domainNotificationRemovalCredit);
                    omitOptionalEntries(
                        domain.latestResults,
                        [](const ProviderResultSummarySnapshot&) {
                            return true;
                        },
                        domainResultRemovalCredit);
                };
                omitDomainMethods(snapshot.accounts);
                omitDomainMethods(snapshot.models);
                omitDomainMethods(snapshot.configuration);
                omitDomainMethods(snapshot.conversations);
                omitDomainMethods(snapshot.filesystem);
                omitDomainMethods(snapshot.reviews);
                omitDomainMethods(snapshot.integrations);
                omitDomainMethods(snapshot.pluginsAndSkills);
                omitDomainMethods(snapshot.mcp);
                omitDomainMethods(snapshot.platform);

                const auto omitOptionalProjection = [&snapshot, &estimatedBytes, limit, &recordOmission](auto& value) {
                    if (estimatedBytes > limit && value) {
                        value.reset();
                        recordOmission();
                        estimatedBytes = accountedSnapshotBytes(snapshot);
                    }
                };
                omitOptionalProjection(snapshot.accounts.login);
                omitOptionalProjection(snapshot.accounts.authentication);
                omitOptionalProjection(snapshot.accounts.rateLimits);
                omitOptionalProjection(snapshot.accounts.resetCreditOutcome);
                omitOptionalProjection(snapshot.configuration.lastWrite);
                omitOptionalProjection(snapshot.configuration.experimentalFeatureEnablement);
                omitOptionalProjection(snapshot.conversations.latestGoal);
                omitOptionalProjection(snapshot.conversations.latestGoalClear);
                omitOptionalProjection(snapshot.conversations.latestGoalSet);
                omitOptionalProjection(snapshot.conversations.latestUnsubscribe);
                omitOptionalProjection(snapshot.integrations.apps);
                omitOptionalProjection(snapshot.integrations.marketplaceAdd);
                omitOptionalProjection(snapshot.integrations.marketplaceRemove);
                omitOptionalProjection(snapshot.integrations.marketplaceUpgrade);
                omitOptionalProjection(snapshot.pluginsAndSkills.pluginInstall);
                omitOptionalProjection(snapshot.pluginsAndSkills.pluginShareCheckout);
                omitOptionalProjection(snapshot.pluginsAndSkills.pluginShareSave);
                omitOptionalProjection(snapshot.pluginsAndSkills.pluginShareUpdateTargets);
                omitOptionalProjection(snapshot.pluginsAndSkills.skillsConfigWrite);
                omitOptionalProjection(snapshot.pluginsAndSkills.extraRoots);
                omitOptionalProjection(snapshot.mcp.oauth);
                omitOptionalProjection(snapshot.mcp.startup);
                omitOptionalProjection(snapshot.mcp.statusList);
                omitOptionalProjection(snapshot.platform.remoteControl);
                omitOptionalProjection(snapshot.platform.windowsSandbox);

                if (estimatedBytes > limit) {
                    std::vector<ExtensionSnapshot> retained;
                    retained.reserve(snapshot.recentExtensions.size());
                    for (ExtensionSnapshot& extension : snapshot.recentExtensions) {
                        if (estimatedBytes > limit) {
                            applyRemovalCredit(estimatedBytes, extensionRemovalCredit(extension));
                            saturatingAddSize(snapshot.omittedRecentExtensions);
                            recordOmission();
                        } else {
                            retained.push_back(std::move(extension));
                        }
                    }
                    snapshot.recentExtensions = std::move(retained);
                    estimatedBytes = accountedSnapshotBytes(snapshot);
                }

                if (estimatedBytes > limit) {
                    std::vector<std::string> retained;
                    retained.reserve(snapshot.diagnostics.recent.size());
                    for (std::string& diagnostic : snapshot.diagnostics.recent) {
                        if (estimatedBytes > limit) {
                            applyRemovalCredit(estimatedBytes, diagnosticRemovalCredit(diagnostic));
                            recordOmission();
                        } else {
                            retained.push_back(std::move(diagnostic));
                        }
                    }
                    snapshot.diagnostics.recent = std::move(retained);
                    estimatedBytes = accountedSnapshotBytes(snapshot);
                }

                if (estimatedBytes > limit && !collapsedToMinimal) {
                    collapseToMinimalSnapshot(snapshot);
                    collapsedToMinimal = true;
                    estimatedBytes = accountedSnapshotBytes(snapshot);
                }
                if (estimatedBytes > limit && snapshot.provider.lastError && !snapshot.provider.lastError->message.empty()) {
                    snapshot.provider.lastError->message.clear();
                    snapshot.capacity.truncated = true;
                    saturatingAdd(snapshot.capacity.state.snapshotOmissions);
                    estimatedBytes = accountedSnapshotBytes(snapshot);
                }
                if (estimatedBytes > limit) {
                    snapshot.capacity.mandatoryCoreExceedsLimit = true;
                    snapshot.capacity.truncated = true;
                }
            } catch (...) {
                collapseToMinimalSnapshot(snapshot);
                if (accountedSnapshotBytes(snapshot) > limit) {
                    snapshot.capacity.mandatoryCoreExceedsLimit = true;
                    snapshot.capacity.truncated = true;
                }
            }
        }
    } // namespace

    ExtensionSnapshot makeExtensionSnapshot(const ExtensionRecord& extension) {
        ExtensionSnapshot snapshot;
        snapshot.originalMethodBytes = extension.originalMethodBytes.value_or(static_cast<std::uint64_t>(extension.method.size()));
        snapshot.method = safeUtf8Prefix(extension.method, MaxSnapshotExtensionMethodBytes);
        snapshot.methodTruncated = extension.originalMethodBytes.has_value() || snapshot.method.size() != extension.method.size();
        if (snapshot.method.empty()) {
            snapshot.method = "codex/unknown";
            snapshot.methodTruncated = true;
        }

        if (extension.decodingError) {
            snapshot.originalDecodingErrorBytes =
                extension.originalDecodingErrorBytes.value_or(static_cast<std::uint64_t>(extension.decodingError->size()));
            snapshot.decodingError = safeUtf8Prefix(*extension.decodingError, MaxSnapshotExtensionDecodingErrorBytes);
            snapshot.decodingErrorTruncated =
                extension.originalDecodingErrorBytes.has_value() || snapshot.decodingError->size() != extension.decodingError->size();
        }

        try {
            const std::string originalPayload = extension.payload.dump();
            snapshot.originalPayloadBytes = extension.originalPayloadBytes.value_or(static_cast<std::uint64_t>(originalPayload.size()));
            if (extension.payload.is_object() && extension.payload.value("truncated", false)) {
                snapshot.payloadTruncated = true;
                const auto originalBytes = extension.payload.find("originalBytes");
                if (originalBytes != extension.payload.end() && originalBytes->is_number_unsigned()) {
                    snapshot.originalPayloadBytes = originalBytes->get<std::uint64_t>();
                }
            }
            if (originalPayload.size() > MaxSnapshotExtensionPayloadBytes) {
                snapshot.payload = Json::object({{"omitted", true},
                                                 {"reason", "extension payload exceeds frontend snapshot bound"},
                                                 {"originalBytes", originalPayload.size()}});
                snapshot.payloadTruncated = true;
                return snapshot;
            }

            JsonSanitizerState sanitizer;
            snapshot.payload = sanitizeExtensionJsonForMethod(extension.method, extension.payload, sanitizer);
            snapshot.sensitiveFieldsRedacted = sanitizer.redacted;
            snapshot.payloadTruncated = snapshot.payloadTruncated || sanitizer.truncated;
            if (snapshot.payload.dump().size() > MaxSnapshotExtensionPayloadBytes) {
                snapshot.payload = Json::object({{"omitted", true},
                                                 {"reason", "sanitized extension payload exceeds frontend snapshot bound"},
                                                 {"originalBytes", originalPayload.size()}});
                snapshot.payloadTruncated = true;
            }
        } catch (...) {
            snapshot.payload = Json::object({{"omitted", true}, {"reason", "extension payload could not be serialized safely"}});
            snapshot.payloadTruncated = true;
        }
        return snapshot;
    }

    bool Snapshot::operator==(const Snapshot& other) const {
        return sequence == other.sequence && provider == other.provider && capacity == other.capacity &&
               diagnostics.received == other.diagnostics.received && diagnostics.recent == other.diagnostics.recent &&
               threads == other.threads && pendingRequests == other.pendingRequests && controller == other.controller &&
               sessions == other.sessions && threadList == other.threadList && providerOperations == other.providerOperations &&
               accounts == other.accounts && models == other.models && configuration == other.configuration &&
               conversations == other.conversations && filesystem == other.filesystem && reviews == other.reviews &&
               integrations == other.integrations && pluginsAndSkills == other.pluginsAndSkills && mcp == other.mcp &&
               platform == other.platform && notices == other.notices && processes == other.processes &&
               filesystemWatches == other.filesystemWatches && fuzzySearchSessions == other.fuzzySearchSessions &&
               activities == other.activities && recentExtensions == other.recentExtensions &&
               omittedRecentExtensions == other.omittedRecentExtensions && sequenceExhausted == other.sequenceExhausted;
    }

    bool Snapshot::operator!=(const Snapshot& other) const {
        return !(*this == other);
    }

    Snapshot makeSnapshot(const BackendState& state) {
        Snapshot snapshot;
        snapshot.sequence = state.sequence;
        snapshot.provider.lifecycle = state.provider.lifecycle;
        snapshot.provider.generation = state.provider.generation;
        snapshot.provider.desiredRunning = state.provider.desiredRunning;
        snapshot.provider.recovery = state.provider.recovery;
        if (state.provider.lastError) {
            snapshot.provider.lastError = ErrorSnapshot{
                errorCategoryName(state.provider.lastError->category), state.provider.lastError->code, state.provider.lastError->message};
        }
        if (state.provider.initialization) {
            snapshot.provider.initialization = InitializeResponseSnapshot{state.provider.initialization->codexHome.value,
                                                                          state.provider.initialization->platformFamily,
                                                                          state.provider.initialization->platformOs,
                                                                          state.provider.initialization->userAgent};
        }
        snapshot.capacity.state = state.capacity;
        snapshot.capacity.retainedThreads = state.capacity.retainedThreads;
        snapshot.capacity.retainedTurns = state.capacity.retainedTurns;
        snapshot.capacity.retainedItems = state.capacity.retainedItems;
        snapshot.capacity.accumulatedContentBytes = state.capacity.accumulatedContentBytes;
        snapshot.capacity.retainedNotices = state.capacity.retainedNotices;
        snapshot.capacity.retainedProcesses = state.capacity.retainedProcesses;
        snapshot.capacity.accumulatedProcessOutputBytes = state.capacity.accumulatedProcessOutputBytes;
        snapshot.capacity.retainedFilesystemWatches = state.capacity.retainedFilesystemWatches;
        snapshot.capacity.retainedFuzzySearchSessions = state.capacity.retainedFuzzySearchSessions;
        snapshot.capacity.retainedActivityRecords = state.capacity.retainedActivityRecords;
        snapshot.capacity.sourcePendingRequestCount = state.pendingRequests.size();
        snapshot.capacity.sourceSessionCount = state.sessions.size();
        snapshot.diagnostics = state.diagnostics;
        snapshot.controller = state.controller;
        snapshot.threadList = ThreadListSnapshot{state.threadList.hasLoadedPage,
                                                 state.threadList.complete,
                                                 state.threadList.nextCursor,
                                                 state.threadList.backwardsCursor,
                                                 state.threadList.pagesLoaded,
                                                 state.threadList.stamp};
        snapshot.sequenceExhausted = state.sequenceExhausted;

        for (const auto& [method, operation] : state.providerOperations) {
            (void) method;
            snapshot.providerOperations.push_back({operation.method, operation.resultAlternative, operation.stamp});
        }
        const auto snapshotDomain = [](const ProviderDomainState& domain) {
            ProviderDomainSnapshot projected;
            projected.latestNotificationMethods.reserve(domain.latestNotifications.size());
            for (const auto& [method, notification] : domain.latestNotifications) {
                projected.latestNotificationMethods.push_back(method);
                projected.latestNotifications.push_back({notification.method, notification.eventAlternative, notification.stamp});
            }
            projected.latestResults.reserve(domain.latestResults.size());
            for (const auto& [method, result] : domain.latestResults) {
                (void) method;
                projected.latestResults.push_back({result.method,
                                                   result.resultAlternative,
                                                   result.status,
                                                   result.subjectId,
                                                   result.nextCursor,
                                                   result.itemCount,
                                                   result.complete,
                                                   result.stamp});
            }
            return projected;
        };
        static_cast<ProviderDomainSnapshot&>(snapshot.accounts) = snapshotDomain(state.accounts);
        snapshot.accounts.login = state.accounts.login;
        snapshot.accounts.authentication = state.accounts.authentication;
        snapshot.accounts.rateLimits = state.accounts.rateLimits;
        snapshot.accounts.resetCreditOutcome = state.accounts.resetCreditOutcome;
        snapshot.accounts.resetCreditStamp = state.accounts.resetCreditStamp;
        snapshot.accounts.loggedOut = state.accounts.loggedOut;
        snapshot.accounts.logoutStamp = state.accounts.logoutStamp;
        snapshot.models = snapshotDomain(state.models);
        static_cast<ProviderDomainSnapshot&>(snapshot.configuration) = snapshotDomain(state.configuration);
        if (state.configuration.lastWrite) {
            const auto& cache = *state.configuration.lastWrite;
            snapshot.configuration.lastWrite =
                ConfigurationDomainSnapshot::Write{safeUtf8Prefix(cache.value.filePath.value, MaxSnapshotExtensionMethodBytes),
                                                   safeUtf8Prefix(cache.value.status.value, MaxSnapshotExtensionMethodBytes),
                                                   safeUtf8Prefix(cache.value.version, MaxSnapshotExtensionMethodBytes),
                                                   cache.value.overriddenMetadata.hasValue(),
                                                   cache.truncated,
                                                   cache.stamp};
        }
        if (state.configuration.experimentalFeatureEnablement) {
            const auto& cache = *state.configuration.experimentalFeatureEnablement;
            ConfigurationDomainSnapshot::FeatureEnablement enablement;
            enablement.totalEntries = cache.originalEntries;
            enablement.truncated = cache.truncated;
            enablement.stamp = cache.stamp;
            enablement.entries.reserve(cache.value.enablement.size());
            for (const auto& [feature, enabled] : cache.value.enablement) {
                enablement.entries.emplace_back(safeUtf8Prefix(feature.value, MaxSnapshotExtensionMethodBytes), enabled);
            }
            snapshot.configuration.experimentalFeatureEnablement = std::move(enablement);
        }
        static_cast<ProviderDomainSnapshot&>(snapshot.conversations) = snapshotDomain(state.conversations);
        const auto goalSnapshot =
            [](std::string operation, const typed::ThreadId& threadId, const typed::ThreadGoal& goal, const SourceStamp& stamp) {
                return ConversationDomainSnapshot::GoalMutation{std::move(operation),
                                                                safeUtf8Prefix(threadId.value, MaxSnapshotExtensionMethodBytes),
                                                                safeUtf8Prefix(goal.objective, MaxSnapshotExtensionPayloadBytes),
                                                                safeUtf8Prefix(goal.status.value, MaxSnapshotExtensionMethodBytes),
                                                                std::nullopt,
                                                                stamp};
            };
        if (state.conversations.latestGoal && state.conversations.latestGoalThreadId &&
            state.conversations.latestGoal->value.goal.hasValue()) {
            snapshot.conversations.latestGoal = goalSnapshot("get",
                                                             *state.conversations.latestGoalThreadId,
                                                             *state.conversations.latestGoal->value.goal.value,
                                                             state.conversations.latestGoal->stamp);
        }
        if (state.conversations.latestGoalClear && state.conversations.latestGoalClearThreadId) {
            snapshot.conversations.latestGoalClear = ConversationDomainSnapshot::GoalMutation{
                "clear",
                safeUtf8Prefix(state.conversations.latestGoalClearThreadId->value, MaxSnapshotExtensionMethodBytes),
                std::nullopt,
                std::nullopt,
                state.conversations.latestGoalClear->value.cleared,
                state.conversations.latestGoalClear->stamp};
        }
        if (state.conversations.latestGoalSet && state.conversations.latestGoalSetThreadId) {
            snapshot.conversations.latestGoalSet = goalSnapshot("set",
                                                                *state.conversations.latestGoalSetThreadId,
                                                                state.conversations.latestGoalSet->value.goal,
                                                                state.conversations.latestGoalSet->stamp);
        }
        if (state.conversations.latestUnsubscribe && state.conversations.latestUnsubscribeThreadId) {
            snapshot.conversations.latestUnsubscribe = ConversationDomainSnapshot::GoalMutation{
                "unsubscribe",
                safeUtf8Prefix(state.conversations.latestUnsubscribeThreadId->value, MaxSnapshotExtensionMethodBytes),
                std::nullopt,
                safeUtf8Prefix(state.conversations.latestUnsubscribe->value.status.value, MaxSnapshotExtensionMethodBytes),
                std::nullopt,
                state.conversations.latestUnsubscribe->stamp};
        }
        snapshot.filesystem = snapshotDomain(state.filesystem);
        snapshot.reviews = snapshotDomain(state.reviews);
        static_cast<ProviderDomainSnapshot&>(snapshot.integrations) = snapshotDomain(state.integrations);
        snapshot.integrations.apps = state.integrations.apps;
        const auto marketplaceSnapshot = [](std::string operation, const auto& cache) {
            IntegrationsDomainSnapshot::MarketplaceMutation mutation;
            mutation.operation = std::move(operation);
            mutation.truncated = cache.truncated;
            mutation.stamp = cache.stamp;
            if constexpr (requires { cache.value.marketplaceName; }) {
                mutation.marketplaceName = safeUtf8Prefix(cache.value.marketplaceName, MaxSnapshotExtensionMethodBytes);
            }
            if constexpr (requires { cache.value.alreadyAdded; }) {
                mutation.alreadyAdded = cache.value.alreadyAdded;
            }
            if constexpr (requires { cache.value.installedRoot.value; }) {
                using InstalledRoot = std::remove_cvref_t<decltype(cache.value.installedRoot.value)>;
                if constexpr (std::is_same_v<InstalledRoot, std::string>) {
                    mutation.installedRoot = safeUtf8Prefix(cache.value.installedRoot.value, MaxSnapshotExtensionMethodBytes);
                } else if constexpr (std::is_same_v<InstalledRoot, std::optional<typed::AbsolutePath>>) {
                    if (cache.value.installedRoot.value) {
                        mutation.installedRoot = safeUtf8Prefix(cache.value.installedRoot.value->value, MaxSnapshotExtensionMethodBytes);
                    }
                }
            }
            if constexpr (requires { cache.value.selectedMarketplaces.size(); }) {
                mutation.selectedCount = cache.value.selectedMarketplaces.size();
            }
            if constexpr (requires { cache.value.upgradedRoots.size(); }) {
                mutation.upgradedRootCount = cache.value.upgradedRoots.size();
            }
            if constexpr (requires { cache.value.errors.size(); }) {
                mutation.errorCount = cache.value.errors.size();
            }
            return mutation;
        };
        if (state.integrations.marketplaceAdd) {
            snapshot.integrations.marketplaceAdd = marketplaceSnapshot("add", *state.integrations.marketplaceAdd);
        }
        if (state.integrations.marketplaceRemove) {
            snapshot.integrations.marketplaceRemove = marketplaceSnapshot("remove", *state.integrations.marketplaceRemove);
        }
        if (state.integrations.marketplaceUpgrade) {
            snapshot.integrations.marketplaceUpgrade = marketplaceSnapshot("upgrade", *state.integrations.marketplaceUpgrade);
        }
        static_cast<ProviderDomainSnapshot&>(snapshot.pluginsAndSkills) = snapshotDomain(state.pluginsAndSkills);
        const auto pluginMutation = [](std::string operation,
                                       std::optional<std::string> subjectId,
                                       std::optional<std::string> status,
                                       std::size_t itemCount,
                                       bool truncated,
                                       const SourceStamp& stamp) {
            if (subjectId) {
                *subjectId = safeUtf8Prefix(*subjectId, MaxSnapshotExtensionMethodBytes);
            }
            if (status) {
                *status = safeUtf8Prefix(*status, MaxSnapshotExtensionMethodBytes);
            }
            return PluginsAndSkillsDomainSnapshot::Mutation{
                std::move(operation), std::move(subjectId), std::move(status), itemCount, truncated, stamp};
        };
        if (state.pluginsAndSkills.pluginInstall) {
            const auto& cache = *state.pluginsAndSkills.pluginInstall;
            snapshot.pluginsAndSkills.pluginInstall = pluginMutation(
                "install", std::nullopt, cache.value.authPolicy.value, cache.value.appsNeedingAuth.size(), cache.truncated, cache.stamp);
        }
        if (state.pluginsAndSkills.pluginShareCheckout) {
            const auto& cache = *state.pluginsAndSkills.pluginShareCheckout;
            snapshot.pluginsAndSkills.pluginShareCheckout =
                pluginMutation("share_checkout", cache.value.remotePluginId, cache.value.pluginName, 0, cache.truncated, cache.stamp);
        }
        if (state.pluginsAndSkills.pluginShareSave) {
            const auto& cache = *state.pluginsAndSkills.pluginShareSave;
            snapshot.pluginsAndSkills.pluginShareSave =
                pluginMutation("share_save", cache.value.remotePluginId, "saved", 0, cache.truncated, cache.stamp);
        }
        if (state.pluginsAndSkills.pluginShareUpdateTargets) {
            const auto& cache = *state.pluginsAndSkills.pluginShareUpdateTargets;
            snapshot.pluginsAndSkills.pluginShareUpdateTargets = pluginMutation("share_update_targets",
                                                                                std::nullopt,
                                                                                cache.value.discoverability.value,
                                                                                cache.value.principals.size(),
                                                                                cache.truncated,
                                                                                cache.stamp);
        }
        if (state.pluginsAndSkills.skillsConfigWrite) {
            const auto& cache = *state.pluginsAndSkills.skillsConfigWrite;
            snapshot.pluginsAndSkills.skillsConfigWrite = pluginMutation("skills_config_write",
                                                                         std::nullopt,
                                                                         cache.value.effectiveEnabled ? "enabled" : "disabled",
                                                                         0,
                                                                         cache.truncated,
                                                                         cache.stamp);
        }
        if (state.pluginsAndSkills.extraRoots) {
            SkillsExtraRootsState roots = *state.pluginsAndSkills.extraRoots;
            for (typed::AbsolutePath& path : roots.roots) {
                path.value = safeUtf8Prefix(path.value, MaxSnapshotExtensionMethodBytes);
            }
            snapshot.pluginsAndSkills.extraRoots = std::move(roots);
        }
        static_cast<ProviderDomainSnapshot&>(snapshot.mcp) = snapshotDomain(state.mcp);
        snapshot.mcp.oauth = state.mcp.oauth;
        snapshot.mcp.startup = state.mcp.startup;
        snapshot.mcp.statusList = state.mcp.statusList;
        static_cast<ProviderDomainSnapshot&>(snapshot.platform) = snapshotDomain(state.platform);
        snapshot.platform.remoteControl = state.platform.remoteControl;
        snapshot.platform.windowsSandbox = state.platform.windowsSandbox;
        for (const NoticeState& notice : state.notices) {
            std::optional<std::string> details = notice.details;
            if (notice.category == NoticeCategory::Configuration && details) {
                details = "[redacted]";
            }
            snapshot.notices.push_back({notice.occurrence,
                                        notice.category,
                                        notice.summary,
                                        std::move(details),
                                        notice.threadId ? std::optional<std::string>{notice.threadId->value} : std::nullopt,
                                        notice.stamp});
        }
        for (const std::string& processId : state.processOrder) {
            const auto process = state.processes.find(processId);
            if (process != state.processes.end()) {
                snapshot.processes.push_back({process->second.processHandle,
                                              process->second.lifecycle,
                                              process->second.stdoutData.size(),
                                              process->second.stderrData.size(),
                                              process->second.stdoutTruncated,
                                              process->second.stderrTruncated,
                                              process->second.droppedOutputBytes,
                                              process->second.exitCode,
                                              process->second.stamp,
                                              process->second.connectionInvalidated});
            }
        }
        for (const std::string& watchId : state.filesystemWatchOrder) {
            const auto watch = state.filesystemWatches.find(watchId);
            if (watch != state.filesystemWatches.end()) {
                snapshot.filesystemWatches.push_back({watch->second.watchId.value,
                                                      watch->second.root ? std::optional<std::string>{safeUtf8Prefix(
                                                                               watch->second.root->value, MaxSnapshotExtensionMethodBytes)}
                                                                         : std::nullopt,
                                                      watch->second.changedPaths.size(),
                                                      watch->second.stamp,
                                                      watch->second.connectionInvalidated});
            }
        }
        for (const std::string& sessionId : state.fuzzySearchOrder) {
            const auto search = state.fuzzySearchSessions.find(sessionId);
            if (search != state.fuzzySearchSessions.end()) {
                snapshot.fuzzySearchSessions.push_back({search->second.sessionId,
                                                        search->second.files.size(),
                                                        search->second.complete,
                                                        search->second.stamp,
                                                        search->second.connectionInvalidated});
            }
        }
        for (const std::string& activityId : state.activityOrder) {
            const auto activity = state.activities.find(activityId);
            if (activity != state.activities.end()) {
                snapshot.activities.push_back(
                    {activity->second.key,
                     activity->second.subjectId,
                     activity->second.kind,
                     activity->second.lifecycle,
                     activity->second.summary,
                     activity->second.details,
                     activity->second.threadId ? std::optional<std::string>{activity->second.threadId->value} : std::nullopt,
                     activity->second.turnId ? std::optional<std::string>{activity->second.turnId->value} : std::nullopt,
                     activity->second.notification.stamp,
                     activity->second.active});
            }
        }

        std::set<std::string> visited;
        for (const typed::ThreadId& threadId : state.threadOrder) {
            const auto iterator = state.threads.find(threadId.value);
            if (iterator != state.threads.end() && visited.insert(iterator->first).second) {
                snapshot.threads.push_back(snapshotThread(threadId, iterator->second));
            }
        }
        for (const auto& [threadId, thread] : state.threads) {
            if (visited.insert(threadId).second) {
                snapshot.threads.push_back(snapshotThread(typed::ThreadId{threadId}, thread));
            }
        }
        for (const auto& [id, pending] : state.pendingRequests) {
            (void) id;
            snapshot.pendingRequests.push_back(snapshotPendingRequest(pending));
        }
        for (const auto& [id, session] : state.sessions) {
            (void) id;
            snapshot.sessions.push_back({session.id, session.role});
        }
        const std::size_t firstExtension =
            state.recentExtensions.size() > MaxSnapshotCodexExtensions ? state.recentExtensions.size() - MaxSnapshotCodexExtensions : 0;
        snapshot.omittedRecentExtensions = firstExtension;
        snapshot.recentExtensions.reserve(state.recentExtensions.size() - firstExtension);
        for (std::size_t index = firstExtension; index < state.recentExtensions.size(); ++index) {
            snapshot.recentExtensions.push_back(makeExtensionSnapshot(state.recentExtensions[index]));
        }
        boundSnapshot(snapshot, state.capacity.limits.maxSnapshotBytes);
        return snapshot;
    }

    std::size_t snapshotSizeBytes(const Snapshot& snapshot) noexcept {
        return accountedSnapshotBytes(snapshot);
    }

} // namespace ai::openai::codex::backend
