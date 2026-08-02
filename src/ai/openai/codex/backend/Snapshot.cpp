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
#include <set>
#include <string>
#include <string_view>
#include <tuple>
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
                   normalized.find("authorization") != std::string::npos || normalized.find("apikey") != std::string::npos;
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
            snapshot.id = id.value;
            snapshot.type = itemType(state.item);
            snapshot.status = lifecycleName(state.lifecycle);
            snapshot.agentText = state.agentText;
            snapshot.reasoningText = state.reasoningText;
            snapshot.reasoningSummary = state.reasoningSummary;
            snapshot.commandOutput = state.commandOutput;
            snapshot.droppedContentBytes = state.droppedContentBytes;
            snapshot.contentTruncated = state.droppedContentBytes != 0;
            snapshot.startedAtMs = state.startedAtMs;
            snapshot.completedAtMs = state.completedAtMs;
            snapshot.extensions = boundedJson(state.extensions);
            snapshot.stamp = state.stamp;
            snapshot.connectionInvalidated = state.connectionInvalidated;

            std::visit(
                Overloaded{[&snapshot](const typed::AgentMessageThreadItem& value) {
                               if (value.phase) {
                                   snapshot.data["phase"] = value.phase->value;
                               }
                           },
                           [&snapshot](const typed::UserMessageThreadItem& value) {
                               snapshot.data = userMessageData(value);
                           },
                           [&snapshot](const typed::ReasoningThreadItem&) {
                               snapshot.data["hasSummary"] = !snapshot.reasoningSummary.empty();
                           },
                           [&snapshot](const typed::CommandExecutionThreadItem& value) {
                               snapshot.data =
                                   Json::object({{"command", value.command}, {"cwd", value.cwd.value}, {"status", value.status.value}});
                               if (value.processId) {
                                   snapshot.data["processId"] = *value.processId;
                               }
                               if (value.exitCode) {
                                   snapshot.data["exitCode"] = *value.exitCode;
                               }
                               if (value.durationMs) {
                                   snapshot.data["durationMs"] = *value.durationMs;
                               }
                           },
                           [&snapshot](const typed::FileChangeThreadItem& value) {
                               const auto changes = value.metadata.raw.find("changes");
                               snapshot.data = Json::object(
                                   {{"status", value.status.value},
                                    {"changes",
                                     changes != value.metadata.raw.end() && changes->is_array() ? boundedJson(*changes) : Json::array()}});
                           },
                           [&snapshot](const typed::McpToolCallThreadItem& value) {
                               snapshot.data = Json::object(
                                   {{"tool", value.tool}, {"status", value.status.value}, {"hasResult", value.result.hasValue()}});
                               snapshot.data["server"] = value.server;
                           },
                           [&snapshot](const typed::DynamicToolCallThreadItem& value) {
                               snapshot.data = Json::object(
                                   {{"tool", value.tool}, {"status", value.status.value}, {"hasResult", value.contentItems.hasValue()}});
                               if (value.nameSpace) {
                                   snapshot.data["namespace"] = *value.nameSpace;
                               }
                           },
                           [&snapshot](const typed::WebSearchThreadItem& value) {
                               snapshot.data = Json::object({{"query", value.query}});
                           },
                           [&snapshot](const typed::UnknownItem& value) {
                               snapshot.data = Json::object();
                               if (value.type) {
                                   snapshot.data["codexType"] = *value.type;
                               }
                               if (const std::optional<std::string> decodingError =
                                       ::ai::openai::codex::detail::safeDecodeDiagnosticText(value.diagnostic)) {
                                   snapshot.data["decodingError"] = *decodingError;
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
            snapshot.id = id.value;
            snapshot.threadId = state.turn.threadId.value;
            snapshot.status = state.turn.status.value;
            snapshot.active = state.active;
            snapshot.terminal = state.terminal;
            if (state.failure) {
                snapshot.failure = boundedJson(*state.failure);
            }
            if (state.tokenUsage) {
                snapshot.tokenUsage = boundedJson(*state.tokenUsage);
            }
            snapshot.extensions = boundedJson(state.extensions);
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
            snapshot.id = id.value;
            snapshot.title = state.thread.title;
            const bool backendPlaceholder =
                state.thread.raw.is_object() && (state.thread.raw.size() == 1 || state.thread.raw.size() == 2) &&
                state.thread.raw.value("backendPlaceholder", false) &&
                (state.thread.raw.size() == 1 ||
                 (state.thread.raw.size() == 2 && state.thread.raw.value("backendPlaceholderStatusKnown", false)));
            const bool backendPlaceholderStatusKnown = backendPlaceholder && state.thread.raw.size() == 2;
            if (!backendPlaceholder) {
                snapshot.cwd = state.thread.cwd.value;
            }
            if (state.thread.model) {
                snapshot.model = state.thread.model->value;
            }
            if (!backendPlaceholder) {
                snapshot.modelProvider = state.thread.modelProvider;
                snapshot.preview = state.thread.preview;
                snapshot.createdAt = state.thread.createdAt;
                snapshot.updatedAt = state.thread.updatedAt;
            }
            if (!backendPlaceholder || backendPlaceholderStatusKnown) {
                snapshot.status = typed::threadStatusDiscriminator(state.thread.status);
            }
            snapshot.fullyLoaded = state.fullyLoaded;
            snapshot.extensions = boundedJson(state.extensions);
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
                                          snapshot.details["command"] = *value.command;
                                      }
                                      if (value.cwd) {
                                          snapshot.details["cwd"] = *value.cwd;
                                      }
                                      if (value.reason) {
                                          snapshot.details["reason"] = *value.reason;
                                      }
                                  },
                                  [&snapshot](const typed::FileChangeApprovalRequest& value) {
                                      snapshot.type = "file_change_approval";
                                      snapshot.threadId = value.threadId.value;
                                      snapshot.turnId = value.turnId.value;
                                      snapshot.itemId = value.itemId.value;
                                      if (value.reason) {
                                          snapshot.details["reason"] = *value.reason;
                                      }
                                      if (value.grantRoot) {
                                          snapshot.details["grantRoot"] = *value.grantRoot;
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
                                      snapshot.threadId = value.params.conversationId.value;
                                      snapshotGenericRequest(snapshot, "applyPatchApproval", value.params.raw);
                                  },
                                  [&snapshot](const typed::ExecCommandApprovalRequest& value) {
                                      snapshot.threadId = value.params.conversationId.value;
                                      snapshotGenericRequest(snapshot, "execCommandApproval", value.params.raw);
                                  },
                                  [&snapshot](const typed::PermissionsApprovalRequest& value) {
                                      snapshot.threadId = value.params.threadId.value;
                                      snapshot.turnId = value.params.turnId.value;
                                      snapshot.itemId = value.params.itemId.value;
                                      snapshotGenericRequest(snapshot, "item/permissions/requestApproval", value.params.raw);
                                  },
                                  [&snapshot](const typed::AttestationGenerateRequest&) {
                                      snapshot.type = "unknown";
                                  },
                                  [&snapshot](const typed::DynamicToolCallRequest& value) {
                                      snapshot.type = "unknown";
                                      snapshot.threadId = value.params.threadId.value;
                                      snapshot.turnId = value.params.turnId.value;
                                  },
                                  [&snapshot](const typed::McpServerElicitationRequest& value) {
                                      snapshot.type = "unknown";
                                      snapshot.threadId = value.params.threadId.value;
                                      if (value.params.turnId.hasValue()) {
                                          snapshot.turnId = value.params.turnId.value->value;
                                      }
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
                         {"stamp", sourceStampJson(thread.stamp)}};
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
                          {"limits",
                           {{"maxSessions", limits.maxSessions},
                            {"maxObservers", limits.maxObservers},
                            {"maxActiveOperations", limits.maxActiveOperations},
                            {"maxPendingRequests", limits.maxPendingRequests},
                            {"maxRetainedThreads", limits.maxRetainedThreads},
                            {"maxRetainedTurns", limits.maxRetainedTurns},
                            {"maxRetainedItems", limits.maxRetainedItems},
                            {"maxAccumulatedContentBytes", limits.maxAccumulatedContentBytes},
                            {"maxSnapshotBytes", limits.maxSnapshotBytes}}}}},
                        {"retainedThreads", capacity.retainedThreads},
                        {"retainedTurns", capacity.retainedTurns},
                        {"retainedItems", capacity.retainedItems},
                        {"accumulatedContentBytes", capacity.accumulatedContentBytes},
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

        void collapseToMinimalSnapshot(Snapshot& snapshot) noexcept {
            try {
                for (const ThreadSnapshot& thread : snapshot.threads) {
                    accountOmittedThread(snapshot.capacity, thread);
                }
                snapshot.threads.clear();
                snapshot.pendingRequests.clear();
                snapshot.sessions.clear();
                snapshot.diagnostics.recent.clear();
                saturatingAddSize(snapshot.omittedRecentExtensions, snapshot.recentExtensions.size());
                snapshot.recentExtensions.clear();
                snapshot.threadList = {};
                snapshot.provider.initialization.reset();
                snapshot.capacity.truncated = true;
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
               sessions == other.sessions && threadList == other.threadList && recentExtensions == other.recentExtensions &&
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

        std::set<std::string> visited;
        for (const typed::ThreadId& threadId : state.threadOrder) {
            const auto iterator = state.threads.find(threadId.value);
            if (iterator != state.threads.end() && visited.insert(iterator->first).second) {
                snapshot.threads.push_back(snapshotThread(threadId, iterator->second));
                saturatingAddSize(snapshot.capacity.retainedThreads);
                saturatingAddSize(snapshot.capacity.retainedTurns, iterator->second.turns.size());
                for (const auto& [turnId, turn] : iterator->second.turns) {
                    (void) turnId;
                    saturatingAddSize(snapshot.capacity.retainedItems, turn.items.size());
                    for (const auto& [itemIdValue, item] : turn.items) {
                        (void) itemIdValue;
                        saturatingAddSize(snapshot.capacity.accumulatedContentBytes, item.agentText.size());
                        saturatingAddSize(snapshot.capacity.accumulatedContentBytes, item.reasoningText.size());
                        saturatingAddSize(snapshot.capacity.accumulatedContentBytes, item.reasoningSummary.size());
                        saturatingAddSize(snapshot.capacity.accumulatedContentBytes, item.commandOutput.size());
                    }
                }
            }
        }
        for (const auto& [threadId, thread] : state.threads) {
            if (visited.insert(threadId).second) {
                snapshot.threads.push_back(snapshotThread(typed::ThreadId{threadId}, thread));
                saturatingAddSize(snapshot.capacity.retainedThreads);
                saturatingAddSize(snapshot.capacity.retainedTurns, thread.turns.size());
                for (const auto& [turnId, turn] : thread.turns) {
                    (void) turnId;
                    saturatingAddSize(snapshot.capacity.retainedItems, turn.items.size());
                    for (const auto& [itemIdValue, item] : turn.items) {
                        (void) itemIdValue;
                        saturatingAddSize(snapshot.capacity.accumulatedContentBytes, item.agentText.size());
                        saturatingAddSize(snapshot.capacity.accumulatedContentBytes, item.reasoningText.size());
                        saturatingAddSize(snapshot.capacity.accumulatedContentBytes, item.reasoningSummary.size());
                        saturatingAddSize(snapshot.capacity.accumulatedContentBytes, item.commandOutput.size());
                    }
                }
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
