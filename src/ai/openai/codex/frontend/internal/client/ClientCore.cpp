/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/internal/client/ClientCore.h"

#include "ai/openai/codex/frontend/detail/EventRepresentation.h"

#include <algorithm>
#include <array>
#include <deque>
#include <exception>
#include <initializer_list>
#include <limits>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ai::openai::codex::frontend::internal::client {
    namespace {
        constexpr std::array<FrontendCapability, 5> ExpandedRepresentationCapabilities{
            FrontendCapability::CompleteBackendDomains,
            FrontendCapability::DedicatedPendingRequests,
            FrontendCapability::DedicatedNotificationEvents,
            FrontendCapability::CompleteThreadItems,
            FrontendCapability::ScopeProjectedState,
        };
        constexpr std::size_t MaximumContinuityKeyBytes = 256;

        template <typename Value>
        bool contains(const std::vector<Value>& values, Value value) noexcept {
            return std::find(values.begin(), values.end(), value) != values.end();
        }

        bool validCapability(FrontendCapability capability) noexcept {
            return std::ranges::any_of(generated::AllCapabilities, [capability](const generated::CapabilityMetadata& metadata) {
                return metadata.defined && metadata.id == static_cast<generated::Capability>(capability);
            });
        }

        bool isRepresentationCapability(FrontendCapability capability) noexcept {
            return std::ranges::find(ExpandedRepresentationCapabilities, capability) != ExpandedRepresentationCapabilities.end();
        }

        const generated::MethodMetadata* methodMetadata(generated::MethodId id) noexcept {
            const auto found = std::ranges::find_if(generated::AllMethods, [id](const generated::MethodMetadata& metadata) {
                return metadata.id == id;
            });
            return found == generated::AllMethods.end() ? nullptr : &*found;
        }

        ClientError localError(ClientErrorCode code, std::string message, bool retryable = false) {
            return {ErrorOrigin::Client, code, std::nullopt, std::move(message), std::nullopt, retryable};
        }

        ClientError transportError(ClientErrorCode code, std::string message, bool retryable) {
            return {ErrorOrigin::Transport, code, std::nullopt, std::move(message), std::nullopt, retryable};
        }

        ClientError protocolError(ClientErrorCode code, std::string message, std::optional<ErrorCode> protocolCode = std::nullopt) {
            return {ErrorOrigin::Protocol, code, protocolCode, std::move(message), std::nullopt, false};
        }

        ClientError commandError(const CommandError& error) {
            return {ErrorOrigin::Command, std::nullopt, error.code, error.message, error.details, false};
        }

        bool bindingIsSensitive(generated::MethodId method) noexcept {
            const generated::MethodMetadata* metadata = methodMetadata(method);
            return (metadata != nullptr && metadata->category == generated::MethodCategory::ReverseResponse) ||
                   method == generated::MethodId::AccountLoginStart;
        }

        void securelyErase(std::string& value) noexcept {
            try {
                value.resize(value.capacity(), '\0');
                volatile char* bytes = value.empty() ? nullptr : value.data();
                for (std::size_t index = 0; index < value.size(); ++index) {
                    bytes[index] = '\0';
                }
                value.clear();
            } catch (...) {
                volatile char* bytes = value.empty() ? nullptr : value.data();
                for (std::size_t index = 0; index < value.size(); ++index) {
                    bytes[index] = '\0';
                }
                value.clear();
            }
        }

        void securelyErase(Json& value) noexcept {
            try {
                if (value.is_string()) {
                    securelyErase(value.get_ref<std::string&>());
                } else if (value.is_array() || value.is_object()) {
                    for (Json& member : value) {
                        securelyErase(member);
                    }
                }
                value = nullptr;
            } catch (...) {
                value = nullptr;
            }
        }

        void securelyErase(generated::DefinedCommand& command) noexcept {
            std::visit(
                [](auto& parameters) {
                    securelyErase(parameters.value);
                },
                command.parameters);
            securelyErase(command.extensions);
            securelyErase(command.parameterExtensions);
        }

        void securelyErase(OutboundMessage& message) noexcept {
            if (auto* hello = std::get_if<Hello>(&message.value)) {
                securelyErase(hello->extensions);
                if (hello->authentication.has_value()) {
                    if (auto* bearer = std::get_if<BearerCredential>(&*hello->authentication)) {
                        securelyErase(bearer->token);
                    }
                    hello->authentication = NoCredential{};
                }
            } else if (auto* command = std::get_if<generated::DefinedCommand>(&message.value)) {
                securelyErase(*command);
            }
        }

        bool validUniqueCapabilities(const std::vector<FrontendCapability>& capabilities) noexcept {
            std::set<FrontendCapability> unique;
            return std::ranges::all_of(capabilities, [&unique](FrontendCapability capability) {
                return validCapability(capability) && unique.insert(capability).second;
            });
        }

        bool completeExpandedSelection(const std::vector<FrontendCapability>& selected) noexcept {
            return std::ranges::all_of(ExpandedRepresentationCapabilities, [&selected](FrontendCapability capability) {
                return contains(selected, capability);
            });
        }

        template <typename Value, typename Identity>
        Json canonicalSet(const std::vector<Value>& values, Identity identity) {
            std::vector<std::string> strings;
            strings.reserve(values.size());
            for (Value value : values) {
                strings.emplace_back(identity(value));
            }
            std::sort(strings.begin(), strings.end());
            strings.erase(std::unique(strings.begin(), strings.end()), strings.end());
            Json result = Json::array();
            for (std::string& value : strings) {
                result.push_back(std::move(value));
            }
            return result;
        }

        template <typename Value, typename Identity>
        Json canonicalOptionalSet(const std::optional<std::vector<Value>>& values, Identity identity) {
            Json result = Json::object();
            result["present"] = values.has_value();
            if (values.has_value()) {
                result["values"] = canonicalSet(*values, identity);
            }
            return result;
        }

        template <typename Enumeration>
        std::string enumIdentity(Enumeration value, std::string_view known) {
            return known.empty() ? "invalid:" + std::to_string(static_cast<std::underlying_type_t<Enumeration>>(value))
                                 : std::string(known);
        }

        std::string projectionFingerprint(const std::vector<FrontendCapability>& requested,
                                          const std::vector<FrontendCapability>& selected,
                                          const std::optional<std::string>& continuityKey,
                                          const std::optional<std::vector<FrontendScope>>& scopes,
                                          const std::optional<std::vector<generated::MethodId>>& permittedMethods,
                                          const std::optional<std::vector<generated::MethodId>>& availableMethods,
                                          const std::optional<Json>& projectionMetadata) {
            Json continuity = Json::object();
            continuity["present"] = continuityKey.has_value();
            if (continuityKey.has_value()) {
                continuity["value"] = *continuityKey;
            }
            Json projection = Json::object();
            projection["present"] = projectionMetadata.has_value();
            if (projectionMetadata.has_value()) {
                projection["value"] = *projectionMetadata;
            }
            Json canonical = Json::object();
            canonical["format"] = "snodec.codex-frontend.projection-fingerprint.v1";
            canonical["requestedRepresentationCapabilities"] = canonicalSet(requested, [](FrontendCapability value) {
                return enumIdentity(value, toString(value));
            });
            canonical["selectedRepresentationCapabilities"] = canonicalSet(selected, [](FrontendCapability value) {
                return enumIdentity(value, toString(value));
            });
            canonical["continuityKey"] = std::move(continuity);
            canonical["permittedScopes"] = canonicalOptionalSet(scopes, [](FrontendScope value) {
                return enumIdentity(value, toString(value));
            });
            canonical["permittedMethods"] = canonicalOptionalSet(permittedMethods, [](generated::MethodId value) {
                return enumIdentity(value, generated::methodString(value));
            });
            canonical["availableMethods"] = canonicalOptionalSet(availableMethods, [](generated::MethodId value) {
                return enumIdentity(value, generated::methodString(value));
            });
            canonical["explicitProjectionMetadata"] = std::move(projection);
            return canonical.dump();
        }

        std::optional<std::vector<generated::MethodId>> decodeMethodSet(const std::optional<std::vector<FrontendMethod>>& encoded,
                                                                        bool& valid) {
            valid = true;
            if (!encoded.has_value()) {
                return std::nullopt;
            }
            std::vector<generated::MethodId> result;
            result.reserve(encoded->size());
            std::set<generated::MethodId> unique;
            for (const std::string& method : *encoded) {
                const auto decoded = generated::definedMethodFromString(method);
                if (!decoded.has_value() || !unique.insert(*decoded).second) {
                    valid = false;
                    return std::nullopt;
                }
                result.push_back(*decoded);
            }
            return result;
        }

        std::optional<std::vector<FrontendScope>> decodeScopes(const Json& extensions, bool& valid) {
            valid = true;
            const auto encoded = extensions.find("permittedScopes");
            if (encoded == extensions.end()) {
                return std::nullopt;
            }
            if (!encoded->is_array()) {
                valid = false;
                return std::nullopt;
            }
            std::vector<FrontendScope> result;
            result.reserve(encoded->size());
            std::set<FrontendScope> unique;
            for (const Json& value : *encoded) {
                if (!value.is_string()) {
                    valid = false;
                    return std::nullopt;
                }
                const auto scope = frontendScopeFromString(value.get<std::string>());
                if (!scope.has_value() || !unique.insert(*scope).second) {
                    valid = false;
                    return std::nullopt;
                }
                result.push_back(*scope);
            }
            return result;
        }

        std::optional<std::string> reverseResponseIdentity(const generated::CompleteCommandParameters& parameters) {
            const generated::MethodId method = generated::commandMethod(parameters);
            const generated::MethodMetadata* metadata = methodMetadata(method);
            if (metadata == nullptr || metadata->category != generated::MethodCategory::ReverseResponse) {
                return std::nullopt;
            }
            return std::visit(
                [](const auto& value) -> std::optional<std::string> {
                    if (!value.value.is_object()) {
                        return std::nullopt;
                    }
                    const auto id = value.value.find("pendingRequestId");
                    if (id == value.value.end() || !id->is_string() || id->template get_ref<const std::string&>().empty()) {
                        return std::nullopt;
                    }
                    return id->template get<std::string>();
                },
                parameters);
        }

        template <typename Value>
        void addOptional(Json& object, std::string_view key, const std::optional<Value>& value) {
            if (value.has_value()) {
                object[std::string(key)] = *value;
            }
        }

        Json detailObject(const model::SafeDetail& detail, std::initializer_list<std::string_view> known = {}) {
            Json result = detail.json();
            if (!result.is_object()) {
                result = Json::object();
            }
            for (std::string_view member : known) {
                result.erase(member);
            }
            return result;
        }

        Json flattenedDetailObject(const model::SafeDetail& detail, std::initializer_list<std::string_view> known = {}) {
            Json result = detailObject(detail, known);
            const auto extensions = result.find("extensions");
            if (extensions != result.end() && extensions->is_object()) {
                Json nested = *extensions;
                result.erase(extensions);
                for (auto member = nested.begin(); member != nested.end(); ++member) {
                    if (!result.contains(member.key())) {
                        result[member.key()] = member.value();
                    }
                }
            }
            return result;
        }

        std::string_view freshnessIdentity(model::Freshness value) noexcept {
            switch (value) {
                case model::Freshness::Unknown:
                    return "unknown";
                case model::Freshness::Current:
                    return "current";
                case model::Freshness::Stale:
                    return "stale";
            }
            return "unknown";
        }

        Json encodeSourceMetadata(const model::SourceMetadata& value) {
            Json result = detailObject(value.extensions);
            result["generation"] = value.generation;
            result["freshness"] = freshnessIdentity(value.freshness);
            return result;
        }

        Json encodeTruncation(const model::TruncationMetadata& value) {
            Json result{{"truncated", value.truncated}, {"omittedFields", value.omittedPaths}};
            addOptional(result, "omittedEntries", value.omittedEntries);
            if (value.droppedBytes != 0) {
                result["droppedBytes"] = value.droppedBytes;
            }
            return result;
        }

        bool informationRetainsValue(model::InformationState state) noexcept {
            return state == model::InformationState::Present || state == model::InformationState::Truncated ||
                   state == model::InformationState::Stale;
        }

        Json projected(std::optional<Json> value, const model::TruncationMetadata& truncation = {}) {
            Json result{{"present", value.has_value()}, {"truncated", truncation.truncated}, {"omittedFields", truncation.omittedPaths}};
            if (value.has_value()) {
                result["value"] = std::move(*value);
            }
            return result;
        }

        Json encodeDomain(const model::DomainState& value) {
            Json result = detailObject(value.extensions);
            if (value.stampKnown) {
                result["stamp"] = encodeSourceMetadata(value.stamp);
            }
            addOptional(result, "status", value.status);
            addOptional(result, "summary", value.summary);
            addOptional(result, "nextCursor", value.nextCursor);
            if (value.completeKnown) {
                result["complete"] = value.complete;
            }
            addOptional(result, "itemCount", value.itemCount);
            result["latestResults"] = Json::array();
            for (const model::DomainResultSummary& entry : value.latestResults) {
                Json encoded = detailObject(entry.extensions);
                encoded["method"] = entry.method;
                encoded["status"] = entry.status;
                addOptional(encoded, "subjectId", entry.subjectId);
                addOptional(encoded, "nextCursor", entry.nextCursor);
                addOptional(encoded, "itemCount", entry.itemCount);
                if (entry.completeKnown) {
                    encoded["complete"] = entry.complete;
                }
                encoded["stamp"] = encodeSourceMetadata(entry.stamp);
                result["latestResults"].push_back(std::move(encoded));
            }
            if (!value.safeDetails.empty()) {
                result["details"] = value.safeDetails.json();
            }
            if (value.truncationKnown) {
                result["truncation"] = encodeTruncation(value.truncation);
            }
            return result;
        }

        Json projectedDomain(const model::DomainState& value) {
            return projected(informationRetainsValue(value.information) ? std::optional<Json>{encodeDomain(value)} : std::nullopt,
                             value.truncation);
        }

        Json encodeProvider(const model::ProviderState& value) {
            Json result = detailObject(value.extensions);
            if (value.provider.has_value()) {
                result["provider"] = *value.provider;
            }
            result["lifecycle"] = model::toString(value.lifecycle);
            result["generation"] = value.generation;
            result["desiredRunning"] = value.desiredRunning;
            result["ready"] = value.ready();
            result["recovery"] = detailObject(value.recovery.extensions);
            result["recovery"]["status"] = model::toString(value.recovery.status);
            result["recovery"]["attempts"] = value.recovery.attempts;
            addOptional(result["recovery"], "delayMs", value.recovery.delayMs);
            if (value.lastError.has_value()) {
                result["lastError"] = value.lastError->json();
            }
            if (value.initialization.has_value()) {
                result["initialization"] = value.initialization->json();
            }
            return result;
        }

        Json encodeController(const model::ControllerState& value, const SessionInfo* session) {
            Json result = detailObject(value.safeDetails, {"controller", "controllerSessionId", "present"});
            if (value.session.has_value()) {
                result["sessionId"] = value.session->value();
            }
            result["present"] = value.controller.has_value() || value.session.has_value();
            result["ownedByThisClient"] = session != nullptr && value.session.has_value() && value.session->value() == session->id.value();
            return result;
        }

        Json encodeSession(const model::SessionState& value) {
            Json result = detailObject(value.safeDetails, {"sessionId", "id", "role"});
            result["sessionId"] = value.id.value();
            result["role"] = toString(value.role);
            return result;
        }

        Json encodeThreadList(const model::ThreadListState& value) {
            Json result =
                detailObject(value.safeDetails, {"hasLoadedPage", "complete", "pagesLoaded", "nextCursor", "backwardsCursor", "stamp"});
            result["hasLoadedPage"] = value.hasLoadedPage;
            result["complete"] = value.complete;
            result["pagesLoaded"] = value.pagesLoaded;
            addOptional(result, "nextCursor", value.nextCursor);
            addOptional(result, "backwardsCursor", value.backwardsCursor);
            if (value.stampKnown) {
                result["stamp"] = encodeSourceMetadata(value.stamp);
            }
            return result;
        }

        Json encodeThread(const model::ThreadState& value, const model::CanonicalSnapshot& snapshot) {
            Json result = flattenedDetailObject(
                value.safeDetails,
                {"id", "title", "name", "createdAt", "createdAtMs", "updatedAt", "updatedAtMs", "fullyLoaded", "stamp", "turns"});
            for (auto member = value.legacyExtensions.json().begin(); member != value.legacyExtensions.json().end(); ++member) {
                if (!result.contains(member.key())) {
                    result[member.key()] = member.value();
                }
            }
            result["id"] = value.id.value();
            addOptional(result, "title", value.title);
            result["fullyLoaded"] = value.fullyLoaded;
            if (value.stampKnown) {
                result["stamp"] = encodeSourceMetadata(value.stamp);
            }
            addOptional(result, "createdAtMs", value.createdAtMs);
            addOptional(result, "updatedAtMs", value.updatedAtMs);
            result["orderedTurns"] = Json::array();
            for (const model::TurnState& turn : snapshot.turns) {
                if (turn.threadId == value.id) {
                    result["orderedTurns"].push_back(turn.id.value());
                }
            }
            return result;
        }

        Json encodeTurn(const model::TurnState& value, const model::CanonicalSnapshot& snapshot) {
            Json result = flattenedDetailObject(
                value.safeDetails, {"id", "threadId", "status", "active", "terminal", "stamp", "connectionInvalidated", "items"});
            for (auto member = value.legacyExtensions.json().begin(); member != value.legacyExtensions.json().end(); ++member) {
                if (!result.contains(member.key())) {
                    result[member.key()] = member.value();
                }
            }
            result["id"] = value.id.value();
            result["threadId"] = value.threadId.value();
            result["status"] = value.status.value_or("unknown");
            result["active"] = value.active;
            result["terminal"] = value.terminal;
            result["connectionInvalidated"] = value.connectionInvalidated;
            if (value.stampKnown) {
                result["stamp"] = encodeSourceMetadata(value.stamp);
            }
            result["orderedItems"] = Json::array();
            std::vector<std::pair<std::size_t, std::string>> orderedItems;
            std::size_t fallbackIndex = 0;
            for (const model::ThreadItem& item : snapshot.items) {
                const model::ItemData& data = model::itemData(item);
                if (data.turnId.has_value() && *data.turnId == value.id) {
                    orderedItems.emplace_back(data.sourceIndex.value_or(fallbackIndex++), data.id.value());
                }
            }
            for (const model::LegacyItemCompatibility& item : snapshot.legacyItems) {
                if (item.value.turnId.has_value() && *item.value.turnId == value.id) {
                    orderedItems.emplace_back(item.sourceIndex, item.value.id.value());
                }
            }
            std::stable_sort(orderedItems.begin(), orderedItems.end(), [](const auto& left, const auto& right) {
                return left.first < right.first;
            });
            for (const auto& [index, id] : orderedItems) {
                (void) index;
                result["orderedItems"].push_back(id);
            }
            return result;
        }

        Json encodeItem(const model::ThreadItem& item) {
            const model::ItemData& value = model::itemData(item);
            Json result = flattenedDetailObject(value.extensions);
            result["id"] = value.id.value();
            if (value.threadId.has_value()) {
                result["threadId"] = value.threadId->value();
            }
            if (value.turnId.has_value()) {
                result["turnId"] = value.turnId->value();
            }
            result["kind"] = toString(model::threadItemKind(item));
            addOptional(result, "status", value.status);
            addOptional(result, "summary", value.summary);
            if (value.location.has_value()) {
                result["location"] = value.location->json();
            }
            addOptional(result, "agentText", value.agentText);
            addOptional(result, "reasoningText", value.reasoningText);
            addOptional(result, "reasoningSummary", value.reasoningSummary);
            addOptional(result, "commandOutput", value.commandOutput);
            addOptional(result, "droppedContentBytes", value.droppedContentBytes);
            result["contentTruncated"] = value.contentTruncated;
            addOptional(result, "startedAtMs", value.startedAtMs);
            addOptional(result, "completedAtMs", value.completedAtMs);
            if (value.safeDetails.has_value()) {
                result["data"] = value.safeDetails->json();
            }
            result["truncated"] = value.truncation.truncated;
            result["omittedFields"] = value.truncation.omittedPaths;
            result["connectionInvalidated"] = value.connectionInvalidated;
            if (value.generation.has_value() || value.freshness != model::Freshness::Unknown) {
                result["stamp"] = Json{{"generation", value.generation.value_or(0)}, {"freshness", freshnessIdentity(value.freshness)}};
            }
            return result;
        }

        Json encodeItem(const model::LegacyItemCompatibility& item) {
            const model::ItemData& value = item.value;
            Json result = value.safeDetails.has_value() ? flattenedDetailObject(*value.safeDetails) : Json::object();
            result["id"] = value.id.value();
            addOptional(result, "threadId", value.threadId ? std::optional<std::string>{value.threadId->value()} : std::nullopt);
            addOptional(result, "turnId", value.turnId ? std::optional<std::string>{value.turnId->value()} : std::nullopt);
            result["kind"] = item.discriminator.empty() ? std::string{"unknown"} : item.discriminator;
            result["kindKnown"] = false;
            addOptional(result, "status", value.status);
            addOptional(result, "agentText", value.agentText);
            addOptional(result, "reasoningText", value.reasoningText);
            addOptional(result, "reasoningSummary", value.reasoningSummary);
            addOptional(result, "commandOutput", value.commandOutput);
            result["contentTruncated"] = value.contentTruncated || value.truncation.truncated;
            return result;
        }

        Json encodePendingRequest(const model::PendingRequest& request, RepresentationMode representation) {
            const model::PendingRequestData& value = model::pendingRequestData(request);
            Json result = detailObject(value.extensions);
            result["id"] = value.id.value();
            result["kind"] = toString(model::pendingRequestKind(request));
            if (value.threadId.has_value()) {
                result["threadId"] = value.threadId->value();
            }
            if (value.turnId.has_value()) {
                result["turnId"] = value.turnId->value();
            }
            if (value.itemId.has_value()) {
                result["itemId"] = value.itemId->value();
            }
            addOptional(result, "summary", value.summary);
            const bool legacyUserInput =
                representation == RepresentationMode::LegacyV1 && model::pendingRequestKind(request) == PendingRequestKind::UserInput;
            Json* questions = nullptr;
            if (legacyUserInput) {
                result["details"] = value.safeDetails.has_value() ? value.safeDetails->json() : Json::object();
                result["details"]["questions"] = Json::array();
                questions = &result["details"]["questions"];
            } else {
                if (value.safeDetails.has_value()) {
                    result["details"] = value.safeDetails->json();
                }
                if (!value.questions.empty()) {
                    result["questions"] = Json::array();
                    questions = &result["questions"];
                }
            }
            if (questions != nullptr) {
                for (const model::PendingRequestQuestion& question : value.questions) {
                    Json encoded = detailObject(question.extensions);
                    encoded["id"] = question.id;
                    encoded["header"] = question.header;
                    encoded["prompt"] = question.prompt;
                    encoded["allowsFreeText"] = question.allowsFreeText;
                    if (!legacyUserInput) {
                        encoded["isSecret"] = question.secretAnswer;
                    }
                    encoded["options"] = Json::array();
                    for (const model::PendingRequestOption& option : question.options) {
                        Json optionJson = detailObject(option.extensions);
                        optionJson["label"] = option.label;
                        optionJson["description"] = option.description;
                        encoded["options"].push_back(std::move(optionJson));
                    }
                    questions->push_back(std::move(encoded));
                }
            }
            if (legacyUserInput) {
                addOptional(result["details"], "autoResolutionMs", value.autoResolutionMs);
            } else {
                addOptional(result, "autoResolutionMs", value.autoResolutionMs);
            }
            result["truncated"] = value.truncation.truncated;
            result["omittedFields"] = value.truncation.omittedPaths;
            result["connectionInvalidated"] = value.connectionInvalidated;
            return result;
        }

        Json encodeProcess(const model::ProcessState& value) {
            Json result = detailObject(value.extensions);
            result["processHandle"] = value.handle.value();
            result["lifecycle"] = value.lifecycle.value_or(value.status.value_or("unknown"));
            addOptional(result, "stdoutBytes", value.stdoutBytes);
            addOptional(result, "stderrBytes", value.stderrBytes);
            result["stdoutTruncated"] = value.stdoutTruncated;
            result["stderrTruncated"] = value.stderrTruncated;
            addOptional(result, "droppedOutputBytes", value.droppedOutputBytes);
            addOptional(result, "exitCode", value.exitCode);
            result["stamp"] = encodeSourceMetadata(value.stamp);
            result["connectionInvalidated"] = value.connectionInvalidated;
            result["stateUnavailable"] = value.terminal && !value.lifecycle.has_value();
            return result;
        }

        Json encodeFilesystemWatch(const model::FilesystemWatchRecord& value) {
            Json result = detailObject(value.extensions);
            result["watchId"] = value.watchId;
            addOptional(result, "root", value.root);
            addOptional(result, "changedPathCount", value.changedPathCount);
            result["stamp"] = encodeSourceMetadata(value.stamp);
            result["connectionInvalidated"] = value.connectionInvalidated;
            result["stateUnavailable"] = false;
            return result;
        }

        Json encodeFuzzySearch(const model::FuzzySearchRecord& value) {
            Json result = detailObject(value.extensions);
            result["sessionId"] = value.sessionId;
            addOptional(result, "resultCount", value.resultCount);
            result["complete"] = value.complete;
            result["stamp"] = encodeSourceMetadata(value.stamp);
            result["connectionInvalidated"] = value.connectionInvalidated;
            result["stateUnavailable"] = false;
            return result;
        }

        Json encodeNotice(const model::NoticeRecord& value) {
            Json result = detailObject(value.extensions);
            if (value.occurrence != 0) {
                result["occurrence"] = value.occurrence;
            }
            result["category"] = value.category;
            result["summary"] = value.summary;
            addOptional(result, "details", value.details);
            if (value.threadId.has_value()) {
                result["threadId"] = value.threadId->value();
            }
            result["stamp"] = encodeSourceMetadata(value.stamp);
            result["stateUnavailable"] = false;
            return result;
        }

        Json encodeActivity(const model::ActivityRecord& value) {
            Json result = detailObject(value.extensions);
            result["key"] = value.key;
            if (!value.subjectId.empty()) {
                result["subjectId"] = value.subjectId;
            }
            result["kind"] = value.kind;
            result["lifecycle"] = value.lifecycle;
            addOptional(result, "summary", value.summary);
            addOptional(result, "details", value.details);
            if (value.threadId.has_value()) {
                result["threadId"] = value.threadId->value();
            }
            if (value.turnId.has_value()) {
                result["turnId"] = value.turnId->value();
            }
            result["active"] = value.active;
            result["stamp"] = encodeSourceMetadata(value.stamp);
            result["stateUnavailable"] = false;
            return result;
        }

        Json encodeDiagnostic(const model::DiagnosticRecord& value) {
            Json result = detailObject(value.extensions);
            addOptional(result, "received", value.received);
            result["detailsOmitted"] = value.detailsOmitted;
            addOptional(result, "message", value.message);
            if (!value.safeDetails.empty()) {
                result["details"] = value.safeDetails.json();
            }
            return result;
        }

        template <typename Entry, typename Encoder>
        Json encodeCollection(const std::vector<Entry>& entries, const model::TruncationMetadata& truncation, Encoder encoder) {
            Json result{{"entries", Json::array()}, {"truncation", encodeTruncation(truncation)}};
            for (const Entry& entry : entries) {
                result["entries"].push_back(encoder(entry));
            }
            return result;
        }

        Json encodeCapacity(const model::CapacityState& value) {
            Json result = detailObject(value.extensions);
#define AISUITE_ENCODE_CAPACITY(member) addOptional(result, #member, value.member)
            AISUITE_ENCODE_CAPACITY(sessions);
            AISUITE_ENCODE_CAPACITY(observers);
            AISUITE_ENCODE_CAPACITY(activeOperations);
            AISUITE_ENCODE_CAPACITY(pendingRequests);
            AISUITE_ENCODE_CAPACITY(retainedThreads);
            AISUITE_ENCODE_CAPACITY(retainedTurns);
            AISUITE_ENCODE_CAPACITY(retainedItems);
            AISUITE_ENCODE_CAPACITY(accumulatedContentBytes);
            AISUITE_ENCODE_CAPACITY(retainedNotices);
            AISUITE_ENCODE_CAPACITY(retainedProcesses);
            AISUITE_ENCODE_CAPACITY(accumulatedProcessOutputBytes);
            AISUITE_ENCODE_CAPACITY(retainedFilesystemWatches);
            AISUITE_ENCODE_CAPACITY(retainedFuzzySearchSessions);
            AISUITE_ENCODE_CAPACITY(retainedActivityRecords);
            AISUITE_ENCODE_CAPACITY(evictedNotices);
            AISUITE_ENCODE_CAPACITY(evictedProcesses);
            AISUITE_ENCODE_CAPACITY(droppedProcessOutputBytes);
            AISUITE_ENCODE_CAPACITY(evictedFilesystemWatches);
            AISUITE_ENCODE_CAPACITY(evictedFuzzySearchSessions);
            AISUITE_ENCODE_CAPACITY(evictedActivityRecords);
#undef AISUITE_ENCODE_CAPACITY
            return result;
        }

        Json encodeBackendCursor(const model::BackendCursorMetadata& value) {
            Json result = Json::object();
            addOptional(result, "backendRevision", value.backendRevision);
            if (value.oldestReplayableAfter.has_value()) {
                result["oldestReplayableAfter"] = value.oldestReplayableAfter->value();
            }
            if (value.currentSequence.has_value()) {
                result["currentSequence"] = value.currentSequence->value();
            }
            if (value.oldestRetainedSequence.has_value()) {
                result["oldestRetainedSequence"] = value.oldestRetainedSequence->value();
            }
            if (value.newestRetainedSequence.has_value()) {
                result["newestRetainedSequence"] = value.newestRetainedSequence->value();
            }
            addOptional(result, "backendSequenceExhausted", value.backendSequenceExhausted);
            addOptional(result, "frontendSequenceExhausted", value.frontendSequenceExhausted);
            return result;
        }

        Json encodeCapabilities(const std::vector<FrontendCapability>& capabilities) {
            Json result = Json::array();
            for (FrontendCapability capability : capabilities) {
                result.push_back(toString(capability));
            }
            return result;
        }

        Json encodeMethods(const std::optional<std::vector<generated::MethodId>>& methods) {
            Json result{{"present", methods.has_value()}};
            if (methods.has_value()) {
                result["values"] = Json::array();
                for (generated::MethodId method : *methods) {
                    result["values"].push_back(generated::methodString(method));
                }
            }
            return result;
        }

        Json encodeSessionInfo(const SessionInfo& value) {
            Json result{{"sessionId", value.id.value()},
                        {"role", toString(value.role)},
                        {"syncMode", toString(value.synchronizationMode)},
                        {"serverCurrentSequence", value.serverCurrentSequence.value()},
                        {"requestedRepresentationCapabilities", encodeCapabilities(value.requestedCapabilities)},
                        {"selectedRepresentationCapabilities", encodeCapabilities(value.selectedCapabilities)}};
            addOptional(result, "serverVersion", value.serverVersion);
            std::vector<FrontendCapability> mechanisms;
            std::vector<FrontendCapability> topology;
            std::vector<FrontendCapability> products;
            for (FrontendCapability capability : value.observedCapabilities) {
                const auto found = std::ranges::find_if(generated::AllCapabilities, [capability](const auto& metadata) {
                    return metadata.id == static_cast<generated::Capability>(capability);
                });
                if (found == generated::AllCapabilities.end()) {
                    continue;
                }
                switch (found->category) {
                    case generated::CapabilityCategory::StaticMechanism:
                        mechanisms.push_back(capability);
                        break;
                    case generated::CapabilityCategory::ConditionalTopology:
                        topology.push_back(capability);
                        break;
                    case generated::CapabilityCategory::Product:
                        products.push_back(capability);
                        break;
                }
            }
            result["observedMechanismCapabilities"] = encodeCapabilities(mechanisms);
            result["observedTopologyCapabilities"] = encodeCapabilities(topology);
            result["observedProductCapabilities"] = encodeCapabilities(products);
            result["availableMethods"] = encodeMethods(value.availableMethods);
            result["permittedMethods"] = encodeMethods(value.permittedMethods);
            result["permittedScopes"] = Json{{"present", value.permittedScopes.has_value()}};
            if (value.permittedScopes.has_value()) {
                result["permittedScopes"]["values"] = Json::array();
                for (FrontendScope scope : *value.permittedScopes) {
                    result["permittedScopes"]["values"].push_back(toString(scope));
                }
            }
            return result;
        }

        std::optional<Json> encodePublishedState(const model::CanonicalSnapshot& snapshot,
                                                 std::uint64_t revision,
                                                 PublishedFreshness freshness,
                                                 RepresentationMode representation,
                                                 std::optional<model::FrontendSequence> visibleSequence,
                                                 std::optional<model::FrontendSequence> synchronizedThrough,
                                                 const SessionInfo* session,
                                                 const std::optional<std::string>& projectionFingerprint) noexcept {
            try {
                Json result{{"revision", revision},
                            {"freshness", static_cast<unsigned>(freshness)},
                            {"representationMode", static_cast<unsigned>(representation)}};
                if (visibleSequence.has_value()) {
                    result["visibleSequence"] = visibleSequence->value();
                }
                if (synchronizedThrough.has_value()) {
                    result["synchronizedThrough"] = synchronizedThrough->value();
                }
                if (session != nullptr) {
                    result["session"] = encodeSessionInfo(*session);
                }
                Json backendCursor = encodeBackendCursor(snapshot.backendCursor);
                if (!backendCursor.empty()) {
                    result["backendCursor"] = std::move(backendCursor);
                }
                if (projectionFingerprint.has_value()) {
                    result["projectionFingerprint"] = *projectionFingerprint;
                }
                if (!snapshot.projection.omittedPaths.empty() || !snapshot.projection.redactedPaths.empty()) {
                    result["projectionMetadata"] =
                        Json{{"omittedFields", snapshot.projection.omittedPaths}, {"redactedFields", snapshot.projection.redactedPaths}};
                }

                result["provider"] = projected(informationRetainsValue(snapshot.provider.information)
                                                   ? std::optional<Json>{encodeProvider(snapshot.provider)}
                                                   : std::nullopt);
                result["controller"] = projected(informationRetainsValue(snapshot.controller.information)
                                                     ? std::optional<Json>{encodeController(snapshot.controller, session)}
                                                     : std::nullopt);
                Json sessions = Json::array();
                for (const model::SessionState& value : snapshot.sessions) {
                    sessions.push_back(encodeSession(value));
                }
                result["sessions"] = projected(std::optional<Json>{std::move(sessions)});
                result["threadList"] = projected(std::optional<Json>{encodeThreadList(snapshot.threadList)});
                result["threads"] = Json::array();
                result["threadProjectionPresent"] = true;
                for (const model::ThreadState& value : snapshot.threads) {
                    result["threads"].push_back(encodeThread(value, snapshot));
                }
                result["turns"] = Json::array();
                result["turnProjectionPresent"] = true;
                for (const model::TurnState& value : snapshot.turns) {
                    result["turns"].push_back(encodeTurn(value, snapshot));
                }
                result["items"] = Json::array();
                result["itemProjectionPresent"] = true;
                std::vector<std::pair<std::size_t, Json>> orderedItems;
                std::size_t fallbackItemIndex = 0;
                for (const model::ThreadItem& value : snapshot.items) {
                    orderedItems.emplace_back(model::itemData(value).sourceIndex.value_or(fallbackItemIndex++), encodeItem(value));
                }
                for (const model::LegacyItemCompatibility& value : snapshot.legacyItems) {
                    orderedItems.emplace_back(value.sourceIndex, encodeItem(value));
                }
                std::stable_sort(orderedItems.begin(), orderedItems.end(), [](const auto& left, const auto& right) {
                    return left.first < right.first;
                });
                for (auto& [index, value] : orderedItems) {
                    (void) index;
                    result["items"].push_back(std::move(value));
                }
                result["pendingRequests"] = Json::array();
                result["pendingRequestProjectionPresent"] = true;
                std::vector<std::pair<std::size_t, Json>> orderedPending;
                std::size_t fallbackPendingIndex = 0;
                for (const model::PendingRequest& value : snapshot.pendingRequests) {
                    orderedPending.emplace_back(model::pendingRequestData(value).sourceIndex.value_or(fallbackPendingIndex++),
                                                encodePendingRequest(value, representation));
                }
                for (const model::LegacyPendingRequestCompatibility& value : snapshot.legacyPendingRequests) {
                    Json request{{"pendingRequestId", value.value.id.value()}, {"kind", "unknown"}};
                    if (value.value.safeDetails.has_value()) {
                        request["details"] = value.value.safeDetails->json();
                    }
                    orderedPending.emplace_back(value.sourceIndex, std::move(request));
                }
                std::stable_sort(orderedPending.begin(), orderedPending.end(), [](const auto& left, const auto& right) {
                    return left.first < right.first;
                });
                for (auto& [index, value] : orderedPending) {
                    (void) index;
                    result["pendingRequests"].push_back(std::move(value));
                }

#define AISUITE_PROJECT_DOMAIN(member) result[#member] = projectedDomain(snapshot.member.state)
                AISUITE_PROJECT_DOMAIN(accounts);
                AISUITE_PROJECT_DOMAIN(models);
                AISUITE_PROJECT_DOMAIN(configuration);
                AISUITE_PROJECT_DOMAIN(permissionProfiles);
                AISUITE_PROJECT_DOMAIN(reviews);
                AISUITE_PROJECT_DOMAIN(apps);
                AISUITE_PROJECT_DOMAIN(externalAgents);
                AISUITE_PROJECT_DOMAIN(hooks);
                AISUITE_PROJECT_DOMAIN(marketplace);
                AISUITE_PROJECT_DOMAIN(plugins);
                AISUITE_PROJECT_DOMAIN(skills);
                AISUITE_PROJECT_DOMAIN(mcp);
                AISUITE_PROJECT_DOMAIN(windowsSandbox);
                AISUITE_PROJECT_DOMAIN(platform);
#undef AISUITE_PROJECT_DOMAIN

                Json processes = encodeCollection(snapshot.processes, {}, encodeProcess);
                result["processes"] = projected(snapshot.processes.empty() ? std::nullopt : std::optional<Json>{std::move(processes)});
                result["filesystemWatches"] = projected(
                    informationRetainsValue(snapshot.filesystemWatches.state.information) || !snapshot.filesystemWatches.entries.empty()
                        ? std::optional<Json>{encodeCollection(
                              snapshot.filesystemWatches.entries, snapshot.filesystemWatches.state.truncation, encodeFilesystemWatch)}
                        : std::nullopt,
                    snapshot.filesystemWatches.state.truncation);
                result["fuzzySearches"] =
                    projected(informationRetainsValue(snapshot.fuzzySearches.state.information) || !snapshot.fuzzySearches.entries.empty()
                                  ? std::optional<Json>{encodeCollection(
                                        snapshot.fuzzySearches.entries, snapshot.fuzzySearches.state.truncation, encodeFuzzySearch)}
                                  : std::nullopt,
                              snapshot.fuzzySearches.state.truncation);
                result["notices"] = projected(
                    informationRetainsValue(snapshot.notices.state.information) || !snapshot.notices.entries.empty()
                        ? std::optional<Json>{encodeCollection(snapshot.notices.entries, snapshot.notices.state.truncation, encodeNotice)}
                        : std::nullopt,
                    snapshot.notices.state.truncation);
                result["activities"] =
                    projected(informationRetainsValue(snapshot.activities.state.information) || !snapshot.activities.entries.empty()
                                  ? std::optional<Json>{encodeCollection(
                                        snapshot.activities.entries, snapshot.activities.state.truncation, encodeActivity)}
                                  : std::nullopt,
                              snapshot.activities.state.truncation);
                const bool capacityPresent =
                    representation == RepresentationMode::ExpandedV1 || snapshot.capacity.sessions.has_value() ||
                    snapshot.capacity.observers.has_value() || snapshot.capacity.activeOperations.has_value() ||
                    snapshot.capacity.pendingRequests.has_value() || snapshot.capacity.retainedThreads.has_value() ||
                    snapshot.capacity.retainedTurns.has_value() || snapshot.capacity.retainedItems.has_value() ||
                    snapshot.capacity.accumulatedContentBytes.has_value() || snapshot.capacity.retainedNotices.has_value() ||
                    snapshot.capacity.retainedProcesses.has_value() || snapshot.capacity.accumulatedProcessOutputBytes.has_value() ||
                    snapshot.capacity.retainedFilesystemWatches.has_value() || snapshot.capacity.retainedFuzzySearchSessions.has_value() ||
                    snapshot.capacity.retainedActivityRecords.has_value() || snapshot.capacity.evictedNotices.has_value() ||
                    snapshot.capacity.evictedProcesses.has_value() || snapshot.capacity.droppedProcessOutputBytes.has_value() ||
                    snapshot.capacity.evictedFilesystemWatches.has_value() || snapshot.capacity.evictedFuzzySearchSessions.has_value() ||
                    snapshot.capacity.evictedActivityRecords.has_value() || !snapshot.capacity.extensions.empty();
                result["capacity"] = projected(capacityPresent ? std::optional<Json>{encodeCapacity(snapshot.capacity)} : std::nullopt);
                result["truncation"] = projected(std::optional<Json>{encodeTruncation(snapshot.truncation)}, snapshot.truncation);
                Json diagnostics{{"entries", Json::array()}};
                addOptional(diagnostics, "received", snapshot.diagnostics.received);
                for (const model::DiagnosticRecord& entry : snapshot.diagnostics.entries) {
                    diagnostics["entries"].push_back(encodeDiagnostic(entry));
                }
                result["diagnostics"] =
                    projected(informationRetainsValue(snapshot.diagnostics.state.information) || !snapshot.diagnostics.entries.empty()
                                  ? std::optional<Json>{std::move(diagnostics)}
                                  : std::nullopt,
                              snapshot.diagnostics.state.truncation);

                Json compatibilityExtensions = Json::object();
                Json stateExtensions = snapshot.stateExtensions.json();
                if (stateExtensions.is_object()) {
                    if (const auto codex = stateExtensions.find("codexExtensions"); codex != stateExtensions.end()) {
                        if (!codex->is_array() || !codex->empty()) {
                            compatibilityExtensions["codexExtensions"] = *codex;
                        }
                        stateExtensions.erase(codex);
                    }
                    if (!stateExtensions.empty()) {
                        compatibilityExtensions["state"] = std::move(stateExtensions);
                    }
                }
                if (snapshot.extensions.json().is_object()) {
                    for (const auto& [key, value] : snapshot.extensions.json().items()) {
                        if (key != "scopeProjection") {
                            compatibilityExtensions[key] = value;
                        }
                    }
                }
                result["compatibilityExtensions"] = std::move(compatibilityExtensions);

                return std::optional<Json>{std::move(result)};
            } catch (...) {
                return std::nullopt;
            }
        }

        std::optional<std::size_t>
        measurePublishedStateBytes(const model::CanonicalSnapshot& snapshot,
                                   std::uint64_t revision,
                                   PublishedFreshness freshness,
                                   RepresentationMode representation,
                                   std::optional<model::FrontendSequence> visibleSequence,
                                   std::optional<model::FrontendSequence> synchronizedThrough,
                                   const SessionInfo* session,
                                   const std::optional<std::string>& projectionFingerprint,
                                   std::optional<model::FrontendSequence> retainedReplayThrough,
                                   std::optional<model::FrontendSequence> lastSynchronizationBatchSequence) noexcept {
            const auto encoded = encodePublishedState(
                snapshot, revision, freshness, representation, visibleSequence, synchronizedThrough, session, projectionFingerprint);
            if (!encoded.has_value()) {
                return std::nullopt;
            }
            std::size_t bytes = encoded->dump().size();
            const std::size_t internalSequences = static_cast<std::size_t>(retainedReplayThrough.has_value()) +
                                                  static_cast<std::size_t>(lastSynchronizationBatchSequence.has_value());
            if (internalSequences > (std::numeric_limits<std::size_t>::max() - bytes) / sizeof(SequenceNumber)) {
                return std::nullopt;
            }
            bytes += internalSequences * sizeof(SequenceNumber);
            return bytes;
        }

        std::size_t retainedEntityCount(const model::CanonicalSnapshot& snapshot) noexcept {
            std::size_t result = snapshot.sessions.size();
            const auto add = [&result](std::size_t count) {
                if (count > std::numeric_limits<std::size_t>::max() - result) {
                    result = std::numeric_limits<std::size_t>::max();
                } else {
                    result += count;
                }
            };
            add(snapshot.threads.size());
            add(snapshot.turns.size());
            add(snapshot.items.size());
            add(snapshot.legacyItems.size());
            add(snapshot.pendingRequests.size());
            add(snapshot.legacyPendingRequests.size());
            add(snapshot.processes.size());
            add(snapshot.filesystemWatches.entries.size());
            add(snapshot.fuzzySearches.entries.size());
            add(snapshot.notices.entries.size());
            add(snapshot.activities.entries.size());
            add(snapshot.diagnostics.entries.size());
#define AISUITE_COUNT_DOMAIN_RESULTS(member) add(snapshot.member.state.latestResults.size())
            AISUITE_COUNT_DOMAIN_RESULTS(accounts);
            AISUITE_COUNT_DOMAIN_RESULTS(models);
            AISUITE_COUNT_DOMAIN_RESULTS(configuration);
            AISUITE_COUNT_DOMAIN_RESULTS(filesystemWatches);
            AISUITE_COUNT_DOMAIN_RESULTS(fuzzySearches);
            AISUITE_COUNT_DOMAIN_RESULTS(permissionProfiles);
            AISUITE_COUNT_DOMAIN_RESULTS(reviews);
            AISUITE_COUNT_DOMAIN_RESULTS(apps);
            AISUITE_COUNT_DOMAIN_RESULTS(externalAgents);
            AISUITE_COUNT_DOMAIN_RESULTS(hooks);
            AISUITE_COUNT_DOMAIN_RESULTS(marketplace);
            AISUITE_COUNT_DOMAIN_RESULTS(plugins);
            AISUITE_COUNT_DOMAIN_RESULTS(skills);
            AISUITE_COUNT_DOMAIN_RESULTS(mcp);
            AISUITE_COUNT_DOMAIN_RESULTS(windowsSandbox);
            AISUITE_COUNT_DOMAIN_RESULTS(platform);
            AISUITE_COUNT_DOMAIN_RESULTS(remoteControl);
            AISUITE_COUNT_DOMAIN_RESULTS(integrations);
            AISUITE_COUNT_DOMAIN_RESULTS(notices);
            AISUITE_COUNT_DOMAIN_RESULTS(activities);
            AISUITE_COUNT_DOMAIN_RESULTS(diagnostics);
#undef AISUITE_COUNT_DOMAIN_RESULTS
            return result;
        }

        bool stateWithinCapacity(const model::CanonicalSnapshot& snapshot,
                                 const ClientLimits& limits,
                                 std::uint64_t revision,
                                 PublishedFreshness freshness,
                                 RepresentationMode representation,
                                 std::optional<model::FrontendSequence> synchronizedThrough,
                                 const SessionInfo* session,
                                 const std::optional<std::string>& projectionFingerprint,
                                 std::optional<model::FrontendSequence> retainedReplayThrough,
                                 std::optional<model::FrontendSequence> lastSynchronizationBatchSequence,
                                 std::string& error) noexcept {
            if (retainedEntityCount(snapshot) > limits.maximumRetainedEntities) {
                error = "decoded frontend state exceeds its retained-entity limit";
                return false;
            }
            const auto measured = measurePublishedStateBytes(snapshot,
                                                             revision,
                                                             freshness,
                                                             representation,
                                                             snapshot.sequence,
                                                             synchronizedThrough,
                                                             session,
                                                             projectionFingerprint,
                                                             retainedReplayThrough,
                                                             lastSynchronizationBatchSequence);
            if (!measured.has_value()) {
                error = "canonical frontend state cannot be measured";
                return false;
            }
            if (*measured > limits.maximumDecodedStateBytes) {
                error = "decoded frontend state exceeds its byte limit";
                return false;
            }
            return true;
        }

        void boundProtocolDiagnostics(model::CanonicalSnapshot& snapshot, std::size_t maximumRetainedDiagnostics) noexcept {
            if (snapshot.diagnostics.entries.size() <= maximumRetainedDiagnostics) {
                return;
            }
            const std::size_t removed = snapshot.diagnostics.entries.size() - maximumRetainedDiagnostics;
            snapshot.diagnostics.entries.erase(snapshot.diagnostics.entries.begin(),
                                               snapshot.diagnostics.entries.begin() + static_cast<std::ptrdiff_t>(removed));
            snapshot.diagnostics.state.truncation.truncated = true;
            std::size_t& omitted = snapshot.diagnostics.state.truncation.omittedEntries.emplace(
                snapshot.diagnostics.state.truncation.omittedEntries.value_or(0));
            if (removed <= std::numeric_limits<std::size_t>::max() - omitted) {
                omitted += removed;
            } else {
                omitted = std::numeric_limits<std::size_t>::max();
            }
        }

        void markSnapshotStale(model::CanonicalSnapshot& snapshot) noexcept {
            for (model::PendingRequest& request : snapshot.pendingRequests) {
                std::visit(
                    [](auto& value) {
                        value.value.connectionInvalidated = true;
                    },
                    request);
            }
            for (model::LegacyPendingRequestCompatibility& request : snapshot.legacyPendingRequests) {
                request.value.connectionInvalidated = true;
            }
        }

        struct ReductionResult {
            std::optional<model::CanonicalSnapshot> value;
            std::string error;
        };

        ReductionResult reduceCanonicalOccurrence(const model::CanonicalSnapshot& snapshot,
                                                  const model::CanonicalOccurrence& occurrence) noexcept;

        std::optional<ExpandedEventType> expandedFamily(std::string_view type) noexcept {
            return expandedEventTypeFromString(type);
        }

        std::optional<FrontendCapability> eventRepresentationCapability(std::string_view type) noexcept {
            if (type == "codex.extension") {
                return std::nullopt;
            }
            if (type == "item.updated" || type == "item.content.updated" || type == "item.upserted") {
                return FrontendCapability::CompleteThreadItems;
            }
            if (type == "request.pending" || type == "request.resolved" || type == "pendingRequests.updated") {
                return FrontendCapability::DedicatedPendingRequests;
            }
            return FrontendCapability::DedicatedNotificationEvents;
        }

        bool eventUsesExpandedRepresentation(const SessionInfo* session, std::string_view type) noexcept {
            switch (detail::eventRepresentation(type)) {
                case detail::EventRepresentation::Legacy:
                    return false;
                case detail::EventRepresentation::Expanded:
                    return true;
                case detail::EventRepresentation::Either: {
                    const auto capability = eventRepresentationCapability(type);
                    return session != nullptr && capability.has_value() && contains(session->selectedCapabilities, *capability);
                }
                case detail::EventRepresentation::None:
                    return false;
            }
            return false;
        }

        bool eventRepresentationWasNegotiated(const SessionInfo* session, std::string_view type, bool expanded) noexcept {
            if (session == nullptr || type == "codex.extension") {
                return true;
            }
            const auto capability = eventRepresentationCapability(type);
            if (!capability.has_value()) {
                return true;
            }
            const bool selected = contains(session->selectedCapabilities, *capability);
            switch (detail::eventRepresentation(type)) {
                case detail::EventRepresentation::Legacy:
                    return !selected;
                case detail::EventRepresentation::Expanded:
                    return selected;
                case detail::EventRepresentation::Either:
                    return expanded == selected;
                case detail::EventRepresentation::None:
                    return false;
            }
            return false;
        }

        model::OccurrenceDecodeContext
        occurrenceContext(PhysicalGeneration generation, SequenceNumber sequence, std::uint32_t groupIndex, std::uint32_t groupCount) {
            const std::string suffix = std::to_string(generation) + ":" + std::to_string(sequence.value());
            return model::OccurrenceDecodeContext{
                model::OccurrenceGroupIdentity{"wire-group:" + suffix},
                groupIndex,
                groupCount,
                model::SourceStamp{"wire-source:" + suffix},
            };
        }
    } // namespace

    bool OutboundMessage::isHello() const noexcept {
        return std::holds_alternative<Hello>(value);
    }

    bool OutboundMessage::isCommand() const noexcept {
        return std::holds_alternative<generated::DefinedCommand>(value);
    }

    const model::ThreadState* PublishedState::thread(std::string_view id) const noexcept {
        if (!snapshot) {
            return nullptr;
        }
        const auto found = std::find_if(snapshot->threads.begin(), snapshot->threads.end(), [id](const model::ThreadState& value) {
            return value.id.value() == id;
        });
        return found == snapshot->threads.end() ? nullptr : &*found;
    }

    const model::SessionState* PublishedState::sessionById(std::string_view id) const noexcept {
        if (!snapshot) {
            return nullptr;
        }
        const auto found = std::find_if(snapshot->sessions.begin(), snapshot->sessions.end(), [id](const model::SessionState& value) {
            return value.id.value() == id;
        });
        return found == snapshot->sessions.end() ? nullptr : &*found;
    }

    const model::TurnState* PublishedState::turn(std::string_view id) const noexcept {
        if (!snapshot) {
            return nullptr;
        }
        const auto found = std::find_if(snapshot->turns.begin(), snapshot->turns.end(), [id](const model::TurnState& value) {
            return value.id.value() == id;
        });
        return found == snapshot->turns.end() ? nullptr : &*found;
    }

    const model::ThreadItem* PublishedState::item(std::string_view id) const noexcept {
        if (!snapshot) {
            return nullptr;
        }
        const auto found = std::find_if(snapshot->items.begin(), snapshot->items.end(), [id](const model::ThreadItem& value) {
            return model::itemData(value).id.value() == id;
        });
        return found == snapshot->items.end() ? nullptr : &*found;
    }

    const model::PendingRequest* PublishedState::pendingRequest(std::string_view id) const noexcept {
        if (!snapshot) {
            return nullptr;
        }
        const auto found =
            std::find_if(snapshot->pendingRequests.begin(), snapshot->pendingRequests.end(), [id](const model::PendingRequest& value) {
                return model::pendingRequestData(value).id.value() == id;
            });
        return found == snapshot->pendingRequests.end() ? nullptr : &*found;
    }

    const model::ProcessState* PublishedState::process(std::string_view handle) const noexcept {
        if (!snapshot) {
            return nullptr;
        }
        const auto found = std::find_if(snapshot->processes.begin(), snapshot->processes.end(), [handle](const model::ProcessState& value) {
            return value.handle.value() == handle;
        });
        return found == snapshot->processes.end() ? nullptr : &*found;
    }

    const model::FilesystemWatchRecord* PublishedState::filesystemWatch(std::string_view id) const noexcept {
        if (!snapshot) {
            return nullptr;
        }
        const auto found = std::find_if(snapshot->filesystemWatches.entries.begin(),
                                        snapshot->filesystemWatches.entries.end(),
                                        [id](const model::FilesystemWatchRecord& value) {
                                            return value.watchId == id;
                                        });
        return found == snapshot->filesystemWatches.entries.end() ? nullptr : &*found;
    }

    const model::FuzzySearchRecord* PublishedState::fuzzySearch(std::string_view id) const noexcept {
        if (!snapshot) {
            return nullptr;
        }
        const auto found = std::find_if(
            snapshot->fuzzySearches.entries.begin(), snapshot->fuzzySearches.entries.end(), [id](const model::FuzzySearchRecord& value) {
                return value.sessionId == id;
            });
        return found == snapshot->fuzzySearches.entries.end() ? nullptr : &*found;
    }

    const model::ActivityRecord* PublishedState::activity(std::string_view key) const noexcept {
        if (!snapshot) {
            return nullptr;
        }
        const auto found = std::find_if(
            snapshot->activities.entries.begin(), snapshot->activities.entries.end(), [key](const model::ActivityRecord& value) {
                return value.key == key;
            });
        return found == snapshot->activities.entries.end() ? nullptr : &*found;
    }

    const model::DomainState* PublishedState::domain(SnapshotDomain selected) const noexcept {
        if (!snapshot) {
            return nullptr;
        }
        switch (selected) {
            case SnapshotDomain::Accounts:
                return &snapshot->accounts.state;
            case SnapshotDomain::Models:
                return &snapshot->models.state;
            case SnapshotDomain::Configuration:
                return &snapshot->configuration.state;
            case SnapshotDomain::FilesystemWatches:
                return &snapshot->filesystemWatches.state;
            case SnapshotDomain::FuzzySearches:
                return &snapshot->fuzzySearches.state;
            case SnapshotDomain::PermissionProfiles:
                return &snapshot->permissionProfiles.state;
            case SnapshotDomain::Reviews:
                return &snapshot->reviews.state;
            case SnapshotDomain::Apps:
                return &snapshot->apps.state;
            case SnapshotDomain::ExternalAgents:
                return &snapshot->externalAgents.state;
            case SnapshotDomain::Hooks:
                return &snapshot->hooks.state;
            case SnapshotDomain::Marketplace:
                return &snapshot->marketplace.state;
            case SnapshotDomain::Plugins:
                return &snapshot->plugins.state;
            case SnapshotDomain::Skills:
                return &snapshot->skills.state;
            case SnapshotDomain::Mcp:
                return &snapshot->mcp.state;
            case SnapshotDomain::WindowsSandbox:
                return &snapshot->windowsSandbox.state;
            case SnapshotDomain::Platform:
                return &snapshot->platform.state;
            case SnapshotDomain::RemoteControl:
                return &snapshot->remoteControl.state;
            case SnapshotDomain::Integrations:
                return &snapshot->integrations.state;
            case SnapshotDomain::Notices:
                return &snapshot->notices.state;
            case SnapshotDomain::Activities:
                return &snapshot->activities.state;
            case SnapshotDomain::Diagnostics:
                return &snapshot->diagnostics.state;
        }
        return nullptr;
    }

    const model::DomainResultSummary* PublishedState::domainResult(SnapshotDomain selected, std::string_view identity) const noexcept {
        const model::DomainState* selectedDomain = domain(selected);
        if (selectedDomain == nullptr) {
            return nullptr;
        }
        const auto found =
            std::find_if(selectedDomain->latestResults.begin(), selectedDomain->latestResults.end(), [identity](const auto& value) {
                return value.subjectId.has_value() && *value.subjectId == identity;
            });
        return found == selectedDomain->latestResults.end() ? nullptr : &*found;
    }

    std::optional<std::size_t> PublishedState::measuredBytes() const noexcept {
        if (!snapshot) {
            return std::nullopt;
        }
        return measurePublishedStateBytes(*snapshot,
                                          revision,
                                          freshness,
                                          representation,
                                          visibleSequence,
                                          synchronizedThrough,
                                          session ? &*session : nullptr,
                                          projectionFingerprint,
                                          std::nullopt,
                                          std::nullopt);
    }

    Json PublishedState::serializeForTesting() const noexcept {
        if (!snapshot) {
            Json result{{"revision", revision},
                        {"threads", Json::array()},
                        {"threadProjectionPresent", false},
                        {"turns", Json::array()},
                        {"turnProjectionPresent", false},
                        {"items", Json::array()},
                        {"itemProjectionPresent", false},
                        {"pendingRequests", Json::array()},
                        {"pendingRequestProjectionPresent", false}};
            switch (freshness) {
                case PublishedFreshness::Current:
                    result["freshness"] = "current";
                    break;
                case PublishedFreshness::Stale:
                    result["freshness"] = "stale";
                    break;
                case PublishedFreshness::Synchronizing:
                    result["freshness"] = "synchronizing";
                    break;
            }
            switch (representation) {
                case RepresentationMode::Unknown:
                    result["representationMode"] = "unknown";
                    break;
                case RepresentationMode::LegacyV1:
                    result["representationMode"] = "legacyV1";
                    break;
                case RepresentationMode::ExpandedV1:
                    result["representationMode"] = "expandedV1";
                    break;
            }
            if (visibleSequence.has_value()) {
                result["visibleSequence"] = visibleSequence->value();
            }
            if (synchronizedThrough.has_value()) {
                result["synchronizedThrough"] = synchronizedThrough->value();
            }
            if (session.has_value()) {
                result["session"] = encodeSessionInfo(*session);
            }
            if (projectionFingerprint.has_value()) {
                result["projectionFingerprint"] = *projectionFingerprint;
            }
            return result;
        }
        auto encoded = encodePublishedState(*snapshot,
                                            revision,
                                            freshness,
                                            representation,
                                            visibleSequence,
                                            synchronizedThrough,
                                            session ? &*session : nullptr,
                                            projectionFingerprint);
        if (!encoded.has_value()) {
            return Json::object();
        }
        Json result = std::move(*encoded);
        switch (freshness) {
            case PublishedFreshness::Current:
                result["freshness"] = "current";
                break;
            case PublishedFreshness::Stale:
                result["freshness"] = "stale";
                break;
            case PublishedFreshness::Synchronizing:
                result["freshness"] = "synchronizing";
                break;
        }
        switch (representation) {
            case RepresentationMode::Unknown:
                result["representationMode"] = "unknown";
                break;
            case RepresentationMode::LegacyV1:
                result["representationMode"] = "legacyV1";
                break;
            case RepresentationMode::ExpandedV1:
                result["representationMode"] = "expandedV1";
                break;
        }
        static constexpr std::array projectedNames{
            std::string_view{"provider"},       std::string_view{"controller"},
            std::string_view{"sessions"},       std::string_view{"threadList"},
            std::string_view{"accounts"},       std::string_view{"models"},
            std::string_view{"configuration"},  std::string_view{"permissionProfiles"},
            std::string_view{"reviews"},        std::string_view{"apps"},
            std::string_view{"externalAgents"}, std::string_view{"hooks"},
            std::string_view{"marketplace"},    std::string_view{"plugins"},
            std::string_view{"skills"},         std::string_view{"mcp"},
            std::string_view{"windowsSandbox"}, std::string_view{"platform"},
            std::string_view{"processes"},      std::string_view{"filesystemWatches"},
            std::string_view{"fuzzySearches"},  std::string_view{"notices"},
            std::string_view{"activities"},     std::string_view{"capacity"},
            std::string_view{"truncation"},     std::string_view{"diagnostics"},
        };
        for (std::string_view name : projectedNames) {
            auto found = result.find(std::string(name));
            if (found == result.end() || !found->is_object()) {
                continue;
            }
            if (!found->value("present", false)) {
                result.erase(found);
                continue;
            }
            found->erase("present");
            if (!found->value("truncated", false)) {
                found->erase("truncated");
            }
            const auto omitted = found->find("omittedFields");
            if (omitted != found->end() && omitted->is_array() && omitted->empty()) {
                found->erase(omitted);
            }
        }
        if (auto sessionValue = result.find("session"); sessionValue != result.end() && sessionValue->is_object()) {
            for (const char* name : {"requestedRepresentationCapabilities",
                                     "selectedRepresentationCapabilities",
                                     "observedMechanismCapabilities",
                                     "observedTopologyCapabilities",
                                     "observedProductCapabilities"}) {
                const auto value = sessionValue->find(name);
                if (value != sessionValue->end() && value->is_array() && value->empty()) {
                    sessionValue->erase(value);
                }
            }
            for (const char* name : {"availableMethods", "permittedMethods", "permittedScopes"}) {
                const auto value = sessionValue->find(name);
                if (value != sessionValue->end() && value->is_object() && !value->value("present", false)) {
                    sessionValue->erase(value);
                }
            }
        }
        if (const auto extensions = result.find("compatibilityExtensions");
            extensions != result.end() && extensions->is_object() && extensions->empty()) {
            result.erase(extensions);
        }
        return result;
    }

    class ClientCore::Impl {
    public:
        struct Attachment {
            PhysicalGeneration generation = 0;
            TransportCallbacks transport;
            bool connected = false;
            bool helloSent = false;
        };

        struct PendingOperation {
            std::string requestId;
            generated::MethodId method = generated::MethodId::ControllerAcquire;
            OperationCompletion completion;
            bool synchronization = false;
        };

        struct DeferredCommand {
            PhysicalGeneration generation = 0;
            std::string requestId;
            OutboundMessage message;
        };

        struct LifecycleCheckpoint {
            std::optional<PhysicalGeneration> generation;
            ConnectionState state = ConnectionState::Disconnected;
        };

        struct Synchronization {
            SyncMode mode = SyncMode::Snapshot;
            model::FrontendSequence target;
            std::optional<model::CanonicalSnapshot> staging;
            std::optional<model::FrontendSequence> representedThrough;
            std::optional<model::FrontendSequence> lastBatchSequence;
            RepresentationMode representation = RepresentationMode::Unknown;
            std::size_t appliedOccurrences = 0;
            std::size_t ignoredOccurrences = 0;
            bool initial = true;
            bool snapshotFallback = false;
            bool explicitRequest = false;
            bool responseAccepted = true;
            bool projectionRefreshRequired = false;
            bool projectionSnapshotRequested = false;
            bool projectionSnapshotResponseAccepted = false;
            bool sawSnapshot = false;
            bool sawEvents = false;
            std::optional<PendingOperation> acceptedOperation;
            std::optional<generated::CompleteCommandResult> acceptedResult;
        };

        explicit Impl(ClientOptions configuredOptions, ClientCallbacks configuredCallbacks)
            : options(std::move(configuredOptions))
            , callbacks(std::move(configuredCallbacks))
            , nextRequestId(options.initialRequestId)
            , published(std::make_shared<const PublishedState>()) {
            validateOptions();
        }

        void validateOptions();
        std::optional<PhysicalGeneration> attach(TransportCallbacks transport);
        void transportConnected(PhysicalGeneration generation);
        void transportDisconnected(PhysicalGeneration generation, TransportError error);
        void detach(PhysicalGeneration generation, std::string_view reason);
        bool receive(PhysicalGeneration generation, const ServerMessage& message);
        bool receiveEncoded(PhysicalGeneration generation, std::string_view message);
        Submission submit(generated::CompleteCommandParameters parameters, OperationCompletion completion);
        void close(std::string_view reason);

        bool owns(PhysicalGeneration generation) const noexcept {
            return attachment.has_value() && attachment->generation == generation;
        }

        [[nodiscard]] LifecycleCheckpoint lifecycleCheckpoint() const noexcept {
            return {attachment ? std::optional<PhysicalGeneration>{attachment->generation} : std::nullopt, connectionState};
        }

        [[nodiscard]] bool continues(const LifecycleCheckpoint& checkpoint) const noexcept {
            return connectionState == checkpoint.state &&
                   (checkpoint.generation.has_value() ? owns(*checkpoint.generation) : !attachment.has_value());
        }

        [[nodiscard]] bool continues(PhysicalGeneration generation, ConnectionState expectedState) const noexcept {
            return owns(generation) && connectionState == expectedState;
        }

        void transition(ConnectionState next, std::optional<ClientError> error = std::nullopt) noexcept;
        void diagnostic(DiagnosticSeverity severity,
                        std::string message,
                        std::optional<ClientErrorCode> code = std::nullopt,
                        std::optional<ClientError> error = std::nullopt) noexcept;
        void notifyError(const ClientError& error) noexcept;
        void notifyStateUpdate(UpdateCause cause,
                               std::optional<model::FrontendSequence> fromSequence,
                               std::optional<model::FrontendSequence> toSequence,
                               std::vector<Change> changes) noexcept;
        void publish(model::CanonicalSnapshot snapshot,
                     PublishedFreshness freshness,
                     RepresentationMode representation,
                     std::optional<model::FrontendSequence> synchronizedThrough,
                     bool emitCursor) noexcept;
        void publishMetadata(PublishedFreshness freshness) noexcept;
        void makeRetainedStateStale() noexcept;
        void failConnection(ClientError error, std::string_view closeReason, bool requestTransportClose) noexcept;
        [[nodiscard]] bool failPending(ClientError error) noexcept;
        void complete(PendingOperation operation,
                      std::optional<generated::CompleteCommandResult> value,
                      std::optional<ClientError> error) noexcept;
        void handleWelcome(const Welcome& welcome);
        void handleSnapshot(const Snapshot& snapshot);
        void handleEvents(const EventBatch& batch);
        void handleSyncComplete(const SyncComplete& message);
        void handleResponse(const Response& response);
        void handleProtocolError(const ProtocolErrorMessage& error, bool& accepted, bool& observeAfterClosure);
        std::optional<model::CanonicalSnapshot> decodeSnapshot(const Snapshot& snapshot, RepresentationMode& representation);
        bool validateAndApplyBatch(model::CanonicalSnapshot& candidate,
                                   const EventBatch& batch,
                                   std::size_t& applied,
                                   std::size_t& ignored,
                                   std::vector<Change>& changes);
        std::optional<std::string> allocateRequestId() noexcept;
        void rememberCompleted(std::string requestId);
        [[nodiscard]] bool cancelSynchronization() noexcept;
        bool requestProjectionSnapshot();
        void flushDeferredCommands() noexcept;
        bool methodAvailable(generated::MethodId method) const noexcept;
        bool methodPermitted(generated::MethodId method) const noexcept;
        std::size_t outstandingOperationCount() const noexcept;

        ClientOptions options;
        ClientCallbacks callbacks;
        ConnectionState connectionState = ConnectionState::Disconnected;
        PhysicalGeneration generationCounter = 0;
        std::optional<Attachment> attachment;
        std::optional<SessionInfo> session;
        std::optional<CapabilityAdvertisement> capabilities;
        std::optional<Synchronization> synchronization;
        std::vector<PendingOperation> pending;
        std::deque<DeferredCommand> deferredCommands;
        std::deque<std::string> completedRequestIds;
        std::vector<Diagnostic> retainedDiagnostics;
        std::uint64_t nextRequestId = 1;
        bool requestIdsExhausted = false;
        std::shared_ptr<const PublishedState> published;
        std::optional<model::FrontendSequence> helloResumeAfter;
        std::optional<std::string> retainedContinuityKey;
        std::optional<std::string> activeContinuityKey;
        std::optional<std::string> activeProjectionFingerprint;
        std::optional<std::string> projectionSnapshotRequestId;
        bool failureInProgress = false;
        bool clientCloseInProgress = false;
        std::size_t dispatchDepth = 0;
        bool flushingDeferredCommands = false;
    };

    void ClientCore::Impl::validateOptions() {
        const ClientLimits& limits = options.limits;
        if (limits.maximumRetainedEntities == 0 || limits.maximumCompletedRequestIds == 0) {
            throw std::invalid_argument("Codex frontend client internal retention limits must be nonzero");
        }
        if (options.requestIdPrefix.empty() || options.requestIdPrefix.size() > 64 ||
            options.requestIdPrefix.find('\0') != std::string::npos || options.initialRequestId == 0) {
            throw std::invalid_argument("Codex frontend client request-ID configuration is invalid");
        }
        if (!options.credentialProvider) {
            throw std::invalid_argument("frontend client requires a credential provider");
        }
        if (!validUniqueCapabilities(options.requestedCapabilities) ||
            !std::ranges::all_of(options.requestedCapabilities, isRepresentationCapability)) {
            throw std::invalid_argument("requested capabilities must be unique representation capabilities");
        }
        if (!validUniqueCapabilities(options.requiredCapabilities)) {
            throw std::invalid_argument("required capabilities must be valid and unique");
        }
        for (FrontendCapability required : options.requiredCapabilities) {
            if (isRepresentationCapability(required) && !contains(options.requestedCapabilities, required)) {
                throw std::invalid_argument("a required representation capability must also be requested");
            }
        }
        if (!options.allowLegacyV1 && !completeExpandedSelection(options.requestedCapabilities)) {
            throw std::invalid_argument("disabling legacy Frontend Protocol v1 requires every expanded representation capability");
        }
    }

    void ClientCore::Impl::diagnostic(DiagnosticSeverity severity,
                                      std::string message,
                                      std::optional<ClientErrorCode> code,
                                      std::optional<ClientError> error) noexcept {
        if (!error.has_value() && code.has_value()) {
            error = localError(*code, message);
        }
        Diagnostic value{severity, std::move(message), code, std::move(error), attachment ? attachment->generation : generationCounter};
        if (options.limits.maximumLocalDiagnostics != 0) {
            if (retainedDiagnostics.size() == options.limits.maximumLocalDiagnostics) {
                retainedDiagnostics.erase(retainedDiagnostics.begin());
            }
            retainedDiagnostics.push_back(value);
        }
        if (callbacks.onDiagnostic) {
            try {
                callbacks.onDiagnostic(value);
            } catch (...) {
                // Diagnostics are the final exception-containment boundary.
            }
        }
    }

    void ClientCore::Impl::notifyError(const ClientError& error) noexcept {
        if (!callbacks.onError) {
            return;
        }
        try {
            callbacks.onError(error);
        } catch (...) {
            diagnostic(DiagnosticSeverity::Warning, "an error callback threw and was contained", ClientErrorCode::CallbackFailure);
        }
    }

    void ClientCore::Impl::notifyStateUpdate(UpdateCause cause,
                                             std::optional<model::FrontendSequence> fromSequence,
                                             std::optional<model::FrontendSequence> toSequence,
                                             std::vector<Change> changes) noexcept {
        if (!callbacks.onStateUpdated) {
            return;
        }
        try {
            callbacks.onStateUpdated(StateUpdate{published, cause, fromSequence, toSequence, std::move(changes)});
        } catch (...) {
            diagnostic(DiagnosticSeverity::Warning, "a state-update callback threw and was contained", ClientErrorCode::CallbackFailure);
        }
    }

    void ClientCore::Impl::transition(ConnectionState next, std::optional<ClientError> error) noexcept {
        if (connectionState == next) {
            return;
        }
        const StateChange change{connectionState, next, std::move(error), attachment ? attachment->generation : generationCounter};
        connectionState = next;
        if (callbacks.onConnectionStateChanged) {
            try {
                callbacks.onConnectionStateChanged(change);
            } catch (...) {
                diagnostic(
                    DiagnosticSeverity::Warning, "a connection-state callback threw and was contained", ClientErrorCode::CallbackFailure);
            }
        }
    }

    void ClientCore::Impl::publish(model::CanonicalSnapshot snapshot,
                                   PublishedFreshness freshness,
                                   RepresentationMode representation,
                                   std::optional<model::FrontendSequence> synchronizedThrough,
                                   bool emitCursor) noexcept {
        try {
            auto next = std::make_shared<PublishedState>();
            next->revision =
                published->revision == std::numeric_limits<std::uint64_t>::max() ? published->revision : published->revision + 1;
            next->freshness = freshness;
            next->representation = representation;
            next->visibleSequence = snapshot.sequence;
            next->synchronizedThrough = synchronizedThrough;
            next->session = session;
            next->projectionFingerprint = activeProjectionFingerprint;
            next->snapshot = std::make_shared<const model::CanonicalSnapshot>(std::move(snapshot));
            published = std::move(next);

            const LifecycleCheckpoint checkpoint = lifecycleCheckpoint();
            const std::shared_ptr<const PublishedState> publication = published;

            if (callbacks.onStatePublished) {
                try {
                    callbacks.onStatePublished(publication);
                } catch (...) {
                    diagnostic(DiagnosticSeverity::Warning,
                               "a state-publication callback threw and was contained",
                               ClientErrorCode::CallbackFailure);
                }
            }
            if (emitCursor && continues(checkpoint) && publication->visibleSequence.has_value() && callbacks.onCursorAdvanced) {
                try {
                    callbacks.onCursorAdvanced(*publication->visibleSequence);
                } catch (...) {
                    diagnostic(DiagnosticSeverity::Warning, "a cursor callback threw and was contained", ClientErrorCode::CallbackFailure);
                }
            }
        } catch (...) {
            diagnostic(DiagnosticSeverity::Error, "immutable frontend state publication failed", ClientErrorCode::StateCapacityExceeded);
        }
    }

    void ClientCore::Impl::publishMetadata(PublishedFreshness freshness) noexcept {
        try {
            auto next = std::make_shared<PublishedState>(*published);
            next->revision =
                published->revision == std::numeric_limits<std::uint64_t>::max() ? published->revision : published->revision + 1;
            next->freshness = freshness;
            next->session = session;
            next->projectionFingerprint = activeProjectionFingerprint;
            published = std::move(next);
            if (callbacks.onStatePublished) {
                try {
                    callbacks.onStatePublished(published);
                } catch (...) {
                    diagnostic(DiagnosticSeverity::Warning,
                               "a state-publication callback threw and was contained",
                               ClientErrorCode::CallbackFailure);
                }
            }
        } catch (...) {
            diagnostic(
                DiagnosticSeverity::Error, "immutable frontend state metadata publication failed", ClientErrorCode::StateCapacityExceeded);
        }
    }

    void ClientCore::Impl::makeRetainedStateStale() noexcept {
        if (!published->snapshot) {
            if (published->freshness != PublishedFreshness::Stale || published->session.has_value()) {
                session.reset();
                const LifecycleCheckpoint checkpoint = lifecycleCheckpoint();
                publishMetadata(PublishedFreshness::Stale);
                if (continues(checkpoint)) {
                    notifyStateUpdate(UpdateCause::ConnectionBecameStale, std::nullopt, std::nullopt, {});
                }
            }
            return;
        }
        try {
            model::CanonicalSnapshot stale = *published->snapshot;
            markSnapshotStale(stale);
            session.reset();
            const LifecycleCheckpoint checkpoint = lifecycleCheckpoint();
            publish(std::move(stale), PublishedFreshness::Stale, published->representation, published->synchronizedThrough, false);
            if (continues(checkpoint)) {
                notifyStateUpdate(UpdateCause::ConnectionBecameStale, std::nullopt, std::nullopt, {});
            }
        } catch (...) {
            session.reset();
            const LifecycleCheckpoint checkpoint = lifecycleCheckpoint();
            publishMetadata(PublishedFreshness::Stale);
            if (continues(checkpoint)) {
                notifyStateUpdate(UpdateCause::ConnectionBecameStale, std::nullopt, std::nullopt, {});
            }
        }
    }

    void ClientCore::Impl::complete(PendingOperation operation,
                                    std::optional<generated::CompleteCommandResult> value,
                                    std::optional<ClientError> error) noexcept {
        if (!operation.completion) {
            return;
        }
        try {
            operation.completion(OperationResult{std::move(operation.requestId), operation.method, std::move(value), std::move(error)});
        } catch (...) {
            diagnostic(
                DiagnosticSeverity::Warning, "an operation completion callback threw and was contained", ClientErrorCode::CallbackFailure);
        }
    }

    bool ClientCore::Impl::failPending(ClientError error) noexcept {
        const LifecycleCheckpoint checkpoint = lifecycleCheckpoint();
        std::vector<PendingOperation> abandoned = std::move(pending);
        pending.clear();
        for (DeferredCommand& deferred : deferredCommands) {
            securelyErase(deferred.message);
        }
        deferredCommands.clear();
        for (PendingOperation& operation : abandoned) {
            rememberCompleted(operation.requestId);
            complete(std::move(operation), std::nullopt, error);
            if (!continues(checkpoint)) {
                return false;
            }
        }
        return true;
    }

    void ClientCore::Impl::failConnection(ClientError error, std::string_view closeReason, bool requestTransportClose) noexcept {
        if (failureInProgress) {
            return;
        }
        failureInProgress = true;
        const std::optional<PhysicalGeneration> failingGeneration =
            attachment ? std::optional<PhysicalGeneration>{attachment->generation} : std::nullopt;
        const auto invalidated = [this, &failingGeneration]() noexcept {
            return connectionState == ConnectionState::Closed ||
                   (failingGeneration.has_value() && !owns(*failingGeneration));
        };
        const auto stopFailure = [this]() noexcept {
            failureInProgress = false;
        };
        std::optional<TransportCallbacks> transport;
        if (attachment.has_value()) {
            transport = attachment->transport;
        }
        notifyError(error);
        if (invalidated()) {
            stopFailure();
            return;
        }
        if (requestTransportClose && connectionState != ConnectionState::Closed) {
            transition(ConnectionState::Closing, error);
            if (invalidated() || connectionState != ConnectionState::Closing) {
                stopFailure();
                return;
            }
        }
        if (requestTransportClose && transport.has_value() && transport->close) {
            try {
                transport->close(closeReason);
            } catch (...) {
                diagnostic(
                    DiagnosticSeverity::Warning, "the transport close callback threw and was contained", ClientErrorCode::CallbackFailure);
            }
            if (invalidated()) {
                stopFailure();
                return;
            }
        }
        std::optional<PendingOperation> synchronizationOperation;
        if (synchronization.has_value() && synchronization->acceptedOperation.has_value()) {
            synchronizationOperation = std::move(synchronization->acceptedOperation);
        }
        synchronization.reset();
        capabilities.reset();
        projectionSnapshotRequestId.reset();
        helloResumeAfter.reset();
        makeRetainedStateStale();
        if (invalidated()) {
            stopFailure();
            return;
        }
        if (!failPending(error) || invalidated()) {
            stopFailure();
            return;
        }
        if (synchronizationOperation.has_value()) {
            complete(std::move(*synchronizationOperation), std::nullopt, error);
            if (invalidated()) {
                stopFailure();
                return;
            }
        }
        activeContinuityKey.reset();
        activeProjectionFingerprint.reset();
        attachment.reset();
        if (connectionState != ConnectionState::Closed) {
            stopFailure();
            transition(ConnectionState::Disconnected, error);
            return;
        }
        stopFailure();
    }

    std::optional<PhysicalGeneration> ClientCore::Impl::attach(TransportCallbacks transport) {
        if (connectionState == ConnectionState::Closed || clientCloseInProgress) {
            diagnostic(DiagnosticSeverity::Warning, "a closed frontend client rejected attachment", ClientErrorCode::Closed);
            return std::nullopt;
        }
        if (attachment.has_value()) {
            diagnostic(
                DiagnosticSeverity::Warning, "the frontend client already has an active attachment", ClientErrorCode::AlreadyConnected);
            return std::nullopt;
        }
        if (!transport.send || !transport.close || generationCounter == std::numeric_limits<PhysicalGeneration>::max()) {
            diagnostic(DiagnosticSeverity::Error,
                       "the frontend transport attachment is invalid or generations are exhausted",
                       ClientErrorCode::InvalidConfiguration);
            return std::nullopt;
        }
        const PhysicalGeneration generation = ++generationCounter;
        attachment = Attachment{generation, std::move(transport), false, false};
        transition(ConnectionState::Connecting);
        return owns(generation) ? std::optional<PhysicalGeneration>{generation} : std::nullopt;
    }

    void ClientCore::Impl::transportConnected(PhysicalGeneration generation) {
        if (!owns(generation) || attachment->connected || connectionState != ConnectionState::Connecting) {
            return;
        }
        attachment->connected = true;

        AuthenticationContext authentication;
        try {
            authentication = options.credentialProvider();
        } catch (...) {
            if (continues(generation, ConnectionState::Connecting)) {
                failConnection(localError(ClientErrorCode::InvalidConfiguration, "credential provider failed"),
                               "frontend credential provider failed",
                               true);
            }
            return;
        }
        if (!continues(generation, ConnectionState::Connecting) || !attachment->connected) {
            return;
        }

        if (authentication.continuityKey.has_value() && authentication.continuityKey->size() > MaximumContinuityKeyBytes) {
            failConnection(localError(ClientErrorCode::InvalidConfiguration, "continuity key exceeds its resource bound"),
                           "frontend continuity key rejected",
                           true);
            return;
        }
        activeContinuityKey = std::move(authentication.continuityKey);
        helloResumeAfter.reset();
        if (published->synchronizedThrough.has_value() && activeContinuityKey.has_value() && retainedContinuityKey == activeContinuityKey) {
            helloResumeAfter = *published->synchronizedThrough;
        }

        const bool sensitive = std::holds_alternative<BearerCredential>(authentication.credential);
        std::optional<AuthenticationCredential> wireCredential;
        if (sensitive) {
            wireCredential = std::move(authentication.credential);
        }
        Hello hello{helloResumeAfter ? std::optional<SequenceNumber>{helloResumeAfter->protocolValue()} : std::nullopt,
                    Json::object(),
                    options.requestedCapabilities,
                    std::move(wireCredential)};
        attachment->helloSent = true;
        SendResult sent;
        bool sendThrew = false;
        OutboundMessage outbound{std::move(hello), sensitive};
        try {
            sent = attachment->transport.send(std::move(outbound));
        } catch (...) {
            sendThrew = true;
            sent = {SendStatus::Failed, TransportError{"transport send callback failed", true}};
        }
        securelyErase(outbound);
        if (!continues(generation, ConnectionState::Connecting) || !attachment->connected) {
            return;
        }
        if (sent.status != SendStatus::Accepted) {
            const bool retryable = sendThrew || (sent.error.has_value() && sent.error->retryable);
            failConnection(transportError(ClientErrorCode::TransportFailure,
                                          sendThrew ? "transport send callback failed" : "transport rejected frontend Hello",
                                          retryable),
                           sendThrew ? "frontend transport send failed" : "frontend Hello rejected",
                           true);
            return;
        }
        transition(ConnectionState::Authenticating);
    }

    void ClientCore::Impl::transportDisconnected(PhysicalGeneration generation, TransportError error) {
        if (clientCloseInProgress) {
            return;
        }
        if (!owns(generation)) {
            return;
        }
        failConnection(
            transportError(ClientErrorCode::TransportFailure, std::move(error.message), error.retryable), "transport disconnected", false);
    }

    void ClientCore::Impl::detach(PhysicalGeneration generation, std::string_view reason) {
        if (clientCloseInProgress) {
            return;
        }
        if (!owns(generation)) {
            return;
        }
        failConnection(transportError(ClientErrorCode::TransportFailure, std::string(reason), true), reason, true);
    }

    void ClientCore::Impl::handleWelcome(const Welcome& welcome) {
        if (connectionState != ConnectionState::Authenticating || session.has_value()) {
            failConnection(
                protocolError(ClientErrorCode::UnexpectedMessage, "unexpected or duplicate Welcome"), "frontend protocol violation", true);
            return;
        }
        const PhysicalGeneration generation = attachment->generation;
        auto sessionId = model::SessionIdentity::parse(welcome.sessionId);
        if (!sessionId.has_value()) {
            failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "Welcome contains an invalid session identity"),
                           "frontend protocol violation",
                           true);
            return;
        }

        SessionInfo decoded{std::move(*sessionId)};
        decoded.role = welcome.role;
        decoded.synchronizationMode = welcome.syncMode;
        decoded.serverCurrentSequence = model::FrontendSequence(welcome.currentSequence);
        decoded.serverVersion = welcome.serverVersion;
        decoded.requestedCapabilities = options.requestedCapabilities;

        if (welcome.capabilities.has_value()) {
            const CapabilityAdvertisement& advertised = *welcome.capabilities;
            const bool inconsistent = !validUniqueCapabilities(advertised.defined) || !validUniqueCapabilities(advertised.implemented) ||
                                      !validUniqueCapabilities(advertised.permitted) ||
                                      std::ranges::any_of(advertised.implemented,
                                                          [&advertised](FrontendCapability value) {
                                                              return !contains(advertised.defined, value);
                                                          }) ||
                                      std::ranges::any_of(advertised.permitted, [&advertised](FrontendCapability value) {
                                          return !contains(advertised.implemented, value);
                                      });
            if (inconsistent) {
                failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "capability advertisement has inconsistent sets"),
                               "frontend capability advertisement rejected",
                               true);
                return;
            }
            capabilities = advertised;
            decoded.observedCapabilities = advertised.implemented;
            for (FrontendCapability requested : options.requestedCapabilities) {
                if (contains(advertised.implemented, requested) && contains(advertised.permitted, requested)) {
                    decoded.selectedCapabilities.push_back(requested);
                }
            }
            for (FrontendCapability required : options.requiredCapabilities) {
                if (!contains(advertised.implemented, required) || !contains(advertised.permitted, required)) {
                    failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "a required frontend capability is unavailable"),
                                   "required frontend capability unavailable",
                                   true);
                    return;
                }
            }
        } else {
            capabilities.reset();
            if (!options.requiredCapabilities.empty() || !options.allowLegacyV1) {
                failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "required capability advertisement is absent"),
                               "required frontend capability unavailable",
                               true);
                return;
            }
        }

        if (!options.allowLegacyV1 && !completeExpandedSelection(decoded.selectedCapabilities)) {
            failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "expanded Frontend Protocol v1 representation is unavailable"),
                           "expanded frontend representation unavailable",
                           true);
            return;
        }

        bool methodsValid = true;
        decoded.availableMethods = decodeMethodSet(welcome.availableMethods, methodsValid);
        if (!methodsValid) {
            failConnection(
                protocolError(ClientErrorCode::UnexpectedMessage, "available-method discovery contains an unknown or duplicate method"),
                "frontend method discovery rejected",
                true);
            return;
        }
        decoded.permittedMethods = decodeMethodSet(welcome.permittedMethods, methodsValid);
        if (!methodsValid) {
            failConnection(
                protocolError(ClientErrorCode::UnexpectedMessage, "permitted-method discovery contains an unknown or duplicate method"),
                "frontend method discovery rejected",
                true);
            return;
        }
        if (decoded.availableMethods.has_value() && decoded.permittedMethods.has_value() &&
            std::ranges::any_of(*decoded.permittedMethods, [&decoded](generated::MethodId id) {
                return !contains(*decoded.availableMethods, id);
            })) {
            failConnection(
                protocolError(ClientErrorCode::UnexpectedMessage, "permitted-method discovery is not a subset of available methods"),
                "frontend method discovery rejected",
                true);
            return;
        }
        decoded.permittedScopes = decodeScopes(welcome.extensions, methodsValid);
        if (!methodsValid) {
            failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "permitted-scope metadata contains an invalid value"),
                           "frontend projection metadata rejected",
                           true);
            return;
        }
        std::optional<Json> explicitProjectionMetadata;
        if (const auto projection = welcome.extensions.find("projection"); projection != welcome.extensions.end()) {
            if (!model::SafeDetail::fromJson(*projection).has_value()) {
                failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "projection metadata is unsafe or over capacity"),
                               "frontend projection metadata rejected",
                               true);
                return;
            }
            explicitProjectionMetadata = *projection;
        }
        try {
            activeProjectionFingerprint = projectionFingerprint(decoded.requestedCapabilities,
                                                                decoded.selectedCapabilities,
                                                                activeContinuityKey,
                                                                decoded.permittedScopes,
                                                                decoded.permittedMethods,
                                                                decoded.availableMethods,
                                                                explicitProjectionMetadata);
        } catch (...) {
            failConnection(protocolError(ClientErrorCode::StateCapacityExceeded, "projection fingerprint could not be bounded"),
                           "frontend projection metadata rejected",
                           true);
            return;
        }
        if (helloResumeAfter.has_value() && welcome.currentSequence.value() < helloResumeAfter->value()) {
            failConnection(protocolError(ClientErrorCode::StateDivergence, "Welcome sequence precedes the requested replay cursor"),
                           "frontend synchronization rejected",
                           true);
            return;
        }

        const bool initial = !published->synchronizedThrough.has_value();
        const bool fallback = helloResumeAfter.has_value() && welcome.syncMode == SyncMode::Snapshot;
        Synchronization nextSync;
        nextSync.mode = welcome.syncMode;
        nextSync.target = model::FrontendSequence(welcome.currentSequence);
        nextSync.initial = initial;
        nextSync.snapshotFallback = fallback;
        nextSync.representedThrough = published->synchronizedThrough;
        const bool completeContinuityMetadata =
            decoded.permittedScopes.has_value() && decoded.permittedMethods.has_value() && decoded.availableMethods.has_value();
        nextSync.projectionRefreshRequired =
            published->synchronizedThrough.has_value() && welcome.syncMode == SyncMode::Replay &&
            (!helloResumeAfter.has_value() || !completeContinuityMetadata || !published->projectionFingerprint.has_value() ||
             published->projectionFingerprint != activeProjectionFingerprint);
        if (welcome.syncMode == SyncMode::Replay) {
            if (published->visibleSequence.has_value() &&
                (!nextSync.representedThrough.has_value() || *published->visibleSequence > *nextSync.representedThrough)) {
                nextSync.representedThrough = published->visibleSequence;
            }
            if (published->snapshot) {
                nextSync.staging = *published->snapshot;
            } else {
                model::CanonicalSnapshot empty;
                empty.sequence = helloResumeAfter.value_or(model::FrontendSequence{});
                nextSync.staging = std::move(empty);
            }
            nextSync.representation = published->representation;
            if (nextSync.representation == RepresentationMode::Unknown) {
                nextSync.representation =
                    completeExpandedSelection(decoded.selectedCapabilities) ? RepresentationMode::ExpandedV1 : RepresentationMode::LegacyV1;
            }
        }

        session = std::move(decoded);
        synchronization = std::move(nextSync);
        if (welcome.syncMode == SyncMode::Replay && published->synchronizedThrough.has_value() &&
            !synchronization->projectionRefreshRequired) {
            publishMetadata(PublishedFreshness::Synchronizing);
            if (!continues(generation, ConnectionState::Authenticating) || !synchronization.has_value()) {
                return;
            }
        }
        transition(ConnectionState::Synchronizing);
    }

    std::optional<model::CanonicalSnapshot> ClientCore::Impl::decodeSnapshot(const Snapshot& snapshot, RepresentationMode& representation) {
        if (!session.has_value()) {
            return std::nullopt;
        }
        const bool expandedDomains = contains(session->selectedCapabilities, FrontendCapability::CompleteBackendDomains);
        if (!expandedDomains && !options.allowLegacyV1) {
            return std::nullopt;
        }
        auto decoded = model::decodeProjectedSnapshot(snapshot, session->selectedCapabilities);
        if (!decoded) {
            return std::nullopt;
        }
        representation = expandedDomains ? RepresentationMode::ExpandedV1 : RepresentationMode::LegacyV1;
        return std::move(decoded).value();
    }

    void ClientCore::Impl::handleSnapshot(const Snapshot& snapshot) {
        if (connectionState != ConnectionState::Synchronizing && connectionState != ConnectionState::Ready) {
            failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "Snapshot arrived outside synchronization or live delivery"),
                           "frontend protocol violation",
                           true);
            return;
        }
        const PhysicalGeneration generation = attachment->generation;
        RepresentationMode representation = RepresentationMode::Unknown;
        auto decoded = decodeSnapshot(snapshot, representation);
        if (!decoded.has_value() || decoded->sequence != model::FrontendSequence(snapshot.sequence)) {
            failConnection(protocolError(ClientErrorCode::DecodeFailure, "frontend snapshot failed canonical typed decoding"),
                           "frontend snapshot rejected",
                           true);
            return;
        }
        boundProtocolDiagnostics(*decoded, options.limits.maximumRetainedDiagnostics);
        std::string capacityError;
        std::optional<model::FrontendSequence> capacitySynchronizedThrough;
        std::optional<model::FrontendSequence> capacityRetainedReplayThrough;
        if (connectionState == ConnectionState::Synchronizing && synchronization.has_value()) {
            capacitySynchronizedThrough = synchronization->representedThrough;
            capacityRetainedReplayThrough = synchronization->representedThrough;
        } else {
            capacitySynchronizedThrough = published->synchronizedThrough;
            if (!capacitySynchronizedThrough.has_value() || decoded->sequence > *capacitySynchronizedThrough) {
                capacitySynchronizedThrough = decoded->sequence;
            }
        }
        const std::uint64_t capacityRevision =
            published->revision == std::numeric_limits<std::uint64_t>::max() ? published->revision : published->revision + 1;
        if (!stateWithinCapacity(*decoded,
                                 options.limits,
                                 capacityRevision,
                                 connectionState == ConnectionState::Synchronizing ? PublishedFreshness::Synchronizing
                                                                                   : PublishedFreshness::Current,
                                 representation,
                                 capacitySynchronizedThrough,
                                 session ? &*session : nullptr,
                                 activeProjectionFingerprint,
                                 capacityRetainedReplayThrough,
                                 std::nullopt,
                                 capacityError)) {
            failConnection(protocolError(ClientErrorCode::StateCapacityExceeded, std::move(capacityError), ErrorCode::CapacityExceeded),
                           "frontend state capacity exceeded",
                           true);
            return;
        }

        if (connectionState == ConnectionState::Synchronizing) {
            if (!synchronization.has_value()) {
                failConnection(protocolError(ClientErrorCode::StateDivergence, "synchronization state is unavailable"),
                               "frontend synchronization diverged",
                               true);
                return;
            }
            if ((synchronization->explicitRequest && !synchronization->responseAccepted) ||
                (synchronization->projectionSnapshotRequested && !synchronization->projectionSnapshotResponseAccepted)) {
                failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "snapshot data preceded its successful command response"),
                               "frontend synchronization ordering violation",
                               true);
                return;
            }
            if (synchronization->mode != SyncMode::Snapshot || synchronization->sawSnapshot || synchronization->sawEvents) {
                failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "invalid mixed or duplicate snapshot synchronization"),
                               "frontend synchronization mode violation",
                               true);
                return;
            }
            if (decoded->sequence.value() > synchronization->target.value()) {
                failConnection(protocolError(ClientErrorCode::StateDivergence, "snapshot does not match the active synchronization"),
                               "frontend synchronization diverged",
                               true);
                return;
            }
            if (synchronization->explicitRequest && synchronization->target == model::FrontendSequence::maximum()) {
                synchronization->target = decoded->sequence;
            }
            std::vector<Change> changes;
            changes.emplace_back(StateReplacedChange{});
            if (!published->visibleSequence.has_value() || decoded->sequence > *published->visibleSequence) {
                changes.emplace_back(CursorAdvancedChange{decoded->sequence});
            }
            synchronization->staging = std::move(*decoded);
            synchronization->representation = representation;
            synchronization->sawSnapshot = true;
            const UpdateCause cause =
                synchronization->projectionSnapshotRequested
                    ? UpdateCause::ProjectionRefresh
                    : (synchronization->explicitRequest
                           ? UpdateCause::ExplicitSnapshot
                           : (synchronization->snapshotFallback ? UpdateCause::SnapshotFallback : UpdateCause::InitialSnapshot));
            publish(
                *synchronization->staging, PublishedFreshness::Synchronizing, representation, synchronization->representedThrough, false);
            if (!continues(generation, ConnectionState::Synchronizing) || !synchronization.has_value()) {
                return;
            }
            notifyStateUpdate(cause, decoded->sequence, decoded->sequence, std::move(changes));
            return;
        }

        std::optional<model::FrontendSequence> representedThrough = published->synchronizedThrough;
        if (published->visibleSequence.has_value() &&
            (!representedThrough.has_value() || *published->visibleSequence > *representedThrough)) {
            representedThrough = published->visibleSequence;
        }
        if (representedThrough.has_value() && decoded->sequence < *representedThrough) {
            failConnection(
                protocolError(ClientErrorCode::StateDivergence, "live Snapshot sequence regressed behind the represented cursor"),
                "frontend live Snapshot rejected",
                true);
            return;
        }
        const model::FrontendSequence sequence = decoded->sequence;
        const bool advanced = !representedThrough.has_value() || sequence > *representedThrough;
        const model::FrontendSequence synchronizedThrough = advanced ? sequence : *representedThrough;
        std::vector<Change> changes{StateReplacedChange{}};
        if (advanced) {
            changes.emplace_back(CursorAdvancedChange{sequence});
        }
        publish(std::move(*decoded), PublishedFreshness::Current, representation, synchronizedThrough, false);
        if (!continues(generation, ConnectionState::Ready)) {
            return;
        }
        notifyStateUpdate(UpdateCause::SnapshotFallback, sequence, sequence, std::move(changes));
        if (!continues(generation, ConnectionState::Ready)) {
            return;
        }
        if (advanced && callbacks.onCursorAdvanced) {
            try {
                callbacks.onCursorAdvanced(sequence);
            } catch (...) {
                diagnostic(DiagnosticSeverity::Warning, "a cursor callback threw and was contained", ClientErrorCode::CallbackFailure);
            }
        }
    }

    namespace {
        ReductionResult reduceCanonicalOccurrence(const model::CanonicalSnapshot& snapshot,
                                                  const model::CanonicalOccurrence& occurrence) noexcept {
            auto reduced = model::reduceOccurrence(snapshot, occurrence);
            if (!reduced) {
                return {std::nullopt, reduced.error().path + ": " + reduced.error().message};
            }
            return {std::move(reduced).value(), {}};
        }
    } // namespace

    bool ClientCore::Impl::validateAndApplyBatch(model::CanonicalSnapshot& candidate,
                                                 const EventBatch& batch,
                                                 std::size_t& applied,
                                                 std::size_t& ignored,
                                                 std::vector<Change>& changes) {
        if (batch.events.empty() || batch.fromSequence != batch.events.front().sequence ||
            batch.toSequence != batch.events.back().sequence) {
            return false;
        }
        const bool synchronizing = connectionState == ConnectionState::Synchronizing;
        if (synchronizing && synchronization->lastBatchSequence.has_value() &&
            model::FrontendSequence(batch.fromSequence) <= *synchronization->lastBatchSequence) {
            return false;
        }
        detail::EventRepresentation batchRepresentation = detail::EventRepresentation::Either;
        std::size_t begin = 0;
        std::optional<model::FrontendSequence> priorGroupSequence;
        while (begin < batch.events.size()) {
            std::size_t end = begin + 1;
            while (end < batch.events.size() && batch.events[end].sequence == batch.events[begin].sequence) {
                ++end;
            }
            const std::size_t count = end - begin;
            if (count > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            const model::FrontendSequence sequence(batch.events[begin].sequence);
            if (priorGroupSequence.has_value() && sequence <= *priorGroupSequence) {
                return false;
            }
            priorGroupSequence = sequence;

            detail::EventRepresentation occurrenceRepresentation = detail::EventRepresentation::Either;
            std::vector<model::CanonicalOccurrence> decodedMembers;
            decodedMembers.reserve(count);
            for (std::size_t index = begin; index < end; ++index) {
                const FrontendEvent& event = batch.events[index];
                const bool compatibilityExtension = event.type == "codex.extension";
                const bool expandedRepresentation = eventUsesExpandedRepresentation(session ? &*session : nullptr, event.type);
                if (!eventRepresentationWasNegotiated(session ? &*session : nullptr, event.type, expandedRepresentation) ||
                    (!expandedRepresentation && !options.allowLegacyV1 && !compatibilityExtension) ||
                    (count != 1 && !expandedRepresentation)) {
                    return false;
                }
                detail::EventRepresentation eventRepresentation = detail::eventRepresentation(event.type);
                if (!compatibilityExtension) {
                    eventRepresentation = detail::intersectRepresentations(eventRepresentation,
                                                                           expandedRepresentation ? detail::EventRepresentation::Expanded
                                                                                                  : detail::EventRepresentation::Legacy);
                }
                occurrenceRepresentation = detail::intersectRepresentations(occurrenceRepresentation, eventRepresentation);
                model::OccurrenceDecodeContext context = occurrenceContext(
                    attachment->generation, event.sequence, static_cast<std::uint32_t>(index - begin), static_cast<std::uint32_t>(count));
                model::OccurrenceResult<model::CanonicalOccurrence> decoded = [&]() {
                    if (expandedRepresentation) {
                        ExpandedFrontendEvent expanded{event.sequence, *expandedFamily(event.type), event.data, event.extensions};
                        const auto validated = Codec::encodeExpandedEvent(expanded);
                        if (!validated) {
                            return model::OccurrenceResult<model::CanonicalOccurrence>{model::OccurrenceError{
                                model::OccurrenceErrorCode::InvalidPayload, "/events", "expanded event validation failed"}};
                        }
                        return model::decodeExpandedOccurrence(expanded, context);
                    }
                    return model::decodeLegacyOccurrence(event, context);
                }();
                if (!decoded || decoded.value().identity().sequence != sequence) {
                    return false;
                }
                decodedMembers.push_back(std::move(decoded).value());
            }
            if (count != 1) {
                occurrenceRepresentation =
                    detail::intersectRepresentations(occurrenceRepresentation, detail::EventRepresentation::Expanded);
            }
            batchRepresentation = detail::intersectRepresentations(batchRepresentation, occurrenceRepresentation);
            if (batchRepresentation == detail::EventRepresentation::None) {
                return false;
            }
            model::OccurrenceError groupError;
            if (!model::validateOccurrenceGroup(std::span<const model::CanonicalOccurrence>{decodedMembers}, &groupError)) {
                return false;
            }
            std::optional<model::CanonicalOccurrence> mergedStorage;
            const model::CanonicalOccurrence* mergedOccurrence = nullptr;
            if (decodedMembers.size() == 1 && decodedMembers.front().expandedPayloads().empty()) {
                mergedOccurrence = &decodedMembers.front();
            } else {
                auto merged = model::mergeOccurrenceGroup(std::span<const model::CanonicalOccurrence>{decodedMembers});
                if (!merged) {
                    return false;
                }
                mergedStorage = std::move(merged).value();
                mergedOccurrence = &*mergedStorage;
            }
            if (synchronizing && synchronization->representedThrough.has_value() && sequence <= *synchronization->representedThrough) {
                ignored += count;
                begin = end;
                continue;
            }
            if (!synchronizing && published->synchronizedThrough.has_value() && sequence <= *published->synchronizedThrough) {
                return false;
            }
            if (sequence <= candidate.sequence) {
                return false;
            }
            ReductionResult reduced = reduceCanonicalOccurrence(candidate, *mergedOccurrence);
            if (!reduced.value.has_value()) {
                diagnostic(
                    DiagnosticSeverity::Error, "canonical occurrence reduction rejected an event group", ClientErrorCode::StateDivergence);
                return false;
            }
            model::CanonicalSnapshot groupCandidate = std::move(*reduced.value);
            groupCandidate.sequence = sequence;
            boundProtocolDiagnostics(groupCandidate, options.limits.maximumRetainedDiagnostics);
            candidate = std::move(groupCandidate);
            for (const model::OccurrencePayload& payload : mergedOccurrence->expandedPayloads()) {
                std::visit(
                    [&changes](const auto& value) {
                        changes.emplace_back(value);
                    },
                    payload);
            }
            if (mergedOccurrence->expandedPayloads().empty()) {
                model::SafeDetail retained;
                const model::LegacyCompatibilityPayload& compatibility = mergedOccurrence->legacyCompatibility();
                if (compatibility.safeExtension.has_value()) {
                    const model::LegacySafeExtension& extension = *compatibility.safeExtension;
                    Json encoded = detailObject(extension.extensions);
                    encoded["method"] = extension.method;
                    if (extension.paramsKnown) {
                        encoded["params"] = extension.params.json();
                    }
                    addOptional(encoded, "decodingError", extension.decodingError);
                    if (extension.sensitiveFieldsRedacted) {
                        encoded["sensitiveFieldsRedacted"] = true;
                    }
                    Json truncation = model::encodeLegacyExtensionTruncation(extension);
                    if (!truncation.empty()) {
                        encoded["truncation"] = std::move(truncation);
                    }
                    if (auto bounded = model::SafeDetail::fromJson(std::move(encoded)); bounded.has_value()) {
                        retained = std::move(*bounded);
                    }
                }
                changes.emplace_back(CompatibilityExtensionChange{batch.events[begin].type, std::move(retained)});
            }
            applied += count;
            begin = end;
        }

        std::string capacityError;
        RepresentationMode candidateRepresentation = synchronizing ? synchronization->representation : published->representation;
        if (candidateRepresentation == RepresentationMode::Unknown && batch.events.front().type != "codex.extension") {
            candidateRepresentation = eventUsesExpandedRepresentation(session ? &*session : nullptr, batch.events.front().type)
                                          ? RepresentationMode::ExpandedV1
                                          : RepresentationMode::LegacyV1;
        }
        const std::uint64_t candidateRevision =
            published->revision == std::numeric_limits<std::uint64_t>::max() ? published->revision : published->revision + 1;
        return stateWithinCapacity(
            candidate,
            options.limits,
            candidateRevision,
            synchronizing ? PublishedFreshness::Synchronizing : PublishedFreshness::Current,
            candidateRepresentation,
            synchronizing ? synchronization->representedThrough : std::optional<model::FrontendSequence>{candidate.sequence},
            session ? &*session : nullptr,
            activeProjectionFingerprint,
            synchronizing ? synchronization->representedThrough : std::nullopt,
            synchronizing ? std::optional<model::FrontendSequence>{model::FrontendSequence(batch.toSequence)} : std::nullopt,
            capacityError);
    }

    void ClientCore::Impl::handleEvents(const EventBatch& batch) {
        if (connectionState != ConnectionState::Synchronizing && connectionState != ConnectionState::Ready) {
            failConnection(
                protocolError(ClientErrorCode::UnexpectedMessage, "event batch arrived outside synchronization or live delivery"),
                "frontend protocol violation",
                true);
            return;
        }
        const PhysicalGeneration generation = attachment->generation;
        const ConnectionState expectedState = connectionState;
        if (connectionState == ConnectionState::Synchronizing &&
            (!synchronization.has_value() || synchronization->mode != SyncMode::Replay)) {
            failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "event batch conflicts with snapshot synchronization"),
                           "frontend synchronization diverged",
                           true);
            return;
        }
        if (connectionState == ConnectionState::Synchronizing &&
            ((synchronization->explicitRequest && !synchronization->responseAccepted) || synchronization->projectionSnapshotRequested)) {
            failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "replay data violated synchronization command ordering"),
                           "frontend synchronization ordering violation",
                           true);
            return;
        }

        std::optional<model::CanonicalSnapshot> candidate;
        if (connectionState == ConnectionState::Synchronizing) {
            candidate = synchronization->staging;
        } else if (published->snapshot) {
            candidate = *published->snapshot;
        }
        if (!candidate.has_value()) {
            failConnection(protocolError(ClientErrorCode::StateDivergence, "event batch has no canonical base state"),
                           "frontend state diverged",
                           true);
            return;
        }

        std::size_t applied = 0;
        std::size_t ignored = 0;
        std::vector<Change> changes;
        if (!validateAndApplyBatch(*candidate, batch, applied, ignored, changes)) {
            if (!continues(generation, expectedState)) {
                return;
            }
            failConnection(protocolError(ClientErrorCode::StateDivergence, "event sequence, representation, or reduction is invalid"),
                           "frontend event batch rejected",
                           true);
            return;
        }

        RepresentationMode representation =
            connectionState == ConnectionState::Synchronizing ? synchronization->representation : published->representation;
        if (representation == RepresentationMode::Unknown && batch.events.front().type != "codex.extension") {
            representation = eventUsesExpandedRepresentation(session ? &*session : nullptr, batch.events.front().type)
                                 ? RepresentationMode::ExpandedV1
                                 : RepresentationMode::LegacyV1;
        }
        if (connectionState == ConnectionState::Synchronizing) {
            if (applied > std::numeric_limits<std::size_t>::max() - synchronization->appliedOccurrences ||
                ignored > std::numeric_limits<std::size_t>::max() - synchronization->ignoredOccurrences) {
                failConnection(protocolError(ClientErrorCode::StateCapacityExceeded, "synchronization event counters exhausted"),
                               "frontend synchronization capacity exceeded",
                               true);
                return;
            }
            synchronization->staging = std::move(*candidate);
            synchronization->representation = representation;
            synchronization->appliedOccurrences += applied;
            synchronization->ignoredOccurrences += ignored;
            synchronization->lastBatchSequence = model::FrontendSequence(batch.toSequence);
            synchronization->sawEvents = true;
            if (!synchronization->projectionRefreshRequired) {
                const UpdateCause cause = synchronization->explicitRequest
                                              ? UpdateCause::ExplicitReplay
                                              : (synchronization->initial ? UpdateCause::InitialReplay : UpdateCause::ReconnectReplay);
                publish(*synchronization->staging,
                        PublishedFreshness::Synchronizing,
                        representation,
                        synchronization->representedThrough,
                        false);
                if (!continues(generation, ConnectionState::Synchronizing) || !synchronization.has_value()) {
                    return;
                }
                notifyStateUpdate(
                    cause, model::FrontendSequence(batch.fromSequence), model::FrontendSequence(batch.toSequence), std::move(changes));
            }
            return;
        }
        if (applied != 0) {
            const model::FrontendSequence sequence = candidate->sequence;
            publish(std::move(*candidate), PublishedFreshness::Current, representation, sequence, false);
            if (!continues(generation, ConnectionState::Ready)) {
                return;
            }
            notifyStateUpdate(UpdateCause::Live,
                              model::FrontendSequence(batch.fromSequence),
                              model::FrontendSequence(batch.toSequence),
                              std::move(changes));
            if (!continues(generation, ConnectionState::Ready)) {
                return;
            }
            if (callbacks.onCursorAdvanced) {
                try {
                    callbacks.onCursorAdvanced(sequence);
                } catch (...) {
                    diagnostic(DiagnosticSeverity::Warning, "a cursor callback threw and was contained", ClientErrorCode::CallbackFailure);
                }
            }
        }
    }

    void ClientCore::Impl::handleSyncComplete(const SyncComplete& message) {
        if (connectionState != ConnectionState::Synchronizing || !synchronization.has_value()) {
            failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "unexpected synchronization completion"),
                           "frontend protocol violation",
                           true);
            return;
        }
        const PhysicalGeneration completingGeneration = attachment->generation;
        const model::FrontendSequence sequence(message.sequence);
        if (synchronization->explicitRequest && synchronization->target == model::FrontendSequence::maximum()) {
            synchronization->target = sequence;
        }
        const bool orderingInvalid = (synchronization->explicitRequest && !synchronization->responseAccepted) ||
                                     (synchronization->projectionSnapshotRequested && !synchronization->projectionSnapshotResponseAccepted);
        if (orderingInvalid) {
            failConnection(
                protocolError(ClientErrorCode::UnexpectedMessage, "synchronization data preceded its successful command response"),
                "frontend synchronization ordering violation",
                true);
            return;
        }
        const bool streamIncomplete =
            synchronization->mode == SyncMode::Snapshot
                ? (!synchronization->sawSnapshot || synchronization->sawEvents || !synchronization->staging.has_value())
                : (synchronization->sawSnapshot || !synchronization->staging.has_value());
        if (streamIncomplete) {
            failConnection(
                protocolError(ClientErrorCode::UnexpectedMessage, "synchronization completed with an incomplete or mixed stream"),
                "frontend synchronization incomplete",
                true);
            return;
        }
        const bool streamCursorInvalid = synchronization->mode == SyncMode::Snapshot ? synchronization->staging->sequence != sequence
                                                                                     : synchronization->staging->sequence > sequence;
        if (sequence != synchronization->target || streamCursorInvalid ||
            (synchronization->lastBatchSequence.has_value() && *synchronization->lastBatchSequence > sequence)) {
            failConnection(protocolError(ClientErrorCode::StateDivergence, "synchronization completion sequence does not match state"),
                           "frontend synchronization diverged",
                           true);
            return;
        }
        if (synchronization->projectionRefreshRequired && !synchronization->projectionSnapshotRequested) {
            if (!requestProjectionSnapshot()) {
                if (!continues(completingGeneration, ConnectionState::Synchronizing)) {
                    return;
                }
                failConnection(localError(ClientErrorCode::SendRejected, "projection-refresh snapshot request failed"),
                               "projection-refresh snapshot request failed",
                               true);
            }
            return;
        }

        const SyncMode finishedMode = synchronization->mode;
        const std::size_t appliedOccurrences = synchronization->appliedOccurrences;
        const std::size_t ignoredOccurrences = synchronization->ignoredOccurrences;
        const bool initial = synchronization->initial;
        const bool snapshotFallback = synchronization->snapshotFallback;
        const RepresentationMode finishedRepresentation = synchronization->representation;
        model::CanonicalSnapshot finishedSnapshot = std::move(*synchronization->staging);
        std::string capacityError;
        const std::uint64_t readyRevision =
            published->revision == std::numeric_limits<std::uint64_t>::max() ? published->revision : published->revision + 1;
        if (!stateWithinCapacity(finishedSnapshot,
                                 options.limits,
                                 readyRevision,
                                 PublishedFreshness::Current,
                                 finishedRepresentation,
                                 sequence,
                                 session ? &*session : nullptr,
                                 activeProjectionFingerprint,
                                 std::nullopt,
                                 std::nullopt,
                                 capacityError)) {
            failConnection(protocolError(ClientErrorCode::StateCapacityExceeded, std::move(capacityError), ErrorCode::CapacityExceeded),
                           "frontend ready-state capacity exceeded",
                           true);
            return;
        }
        const bool cursorAdvanced = !published->synchronizedThrough.has_value() || sequence > *published->synchronizedThrough;
        retainedContinuityKey = activeContinuityKey;
        transition(ConnectionState::Ready);
        if (!owns(completingGeneration) || connectionState != ConnectionState::Ready) {
            return;
        }
        publish(std::move(finishedSnapshot), PublishedFreshness::Current, finishedRepresentation, sequence, false);
        if (!owns(completingGeneration) || connectionState != ConnectionState::Ready) {
            return;
        }
        std::vector<Change> synchronizationChanges;
        if (cursorAdvanced) {
            synchronizationChanges.emplace_back(CursorAdvancedChange{sequence});
        }
        notifyStateUpdate(UpdateCause::SynchronizationCompleted, std::nullopt, sequence, std::move(synchronizationChanges));
        if (!owns(completingGeneration) || connectionState != ConnectionState::Ready) {
            return;
        }
        std::optional<PendingOperation> explicitOperation = std::move(synchronization->acceptedOperation);
        std::optional<generated::CompleteCommandResult> explicitResult = std::move(synchronization->acceptedResult);
        synchronization.reset();
        if (explicitOperation.has_value()) {
            this->complete(std::move(*explicitOperation), std::move(explicitResult), std::nullopt);
            if (!owns(completingGeneration) || connectionState != ConnectionState::Ready) {
                return;
            }
        }
        if (callbacks.onCursorAdvanced) {
            try {
                callbacks.onCursorAdvanced(sequence);
            } catch (...) {
                diagnostic(DiagnosticSeverity::Warning, "a cursor callback threw and was contained", ClientErrorCode::CallbackFailure);
            }
            if (!owns(completingGeneration) || connectionState != ConnectionState::Ready) {
                return;
            }
        }

        if (callbacks.onSynchronized) {
            try {
                callbacks.onSynchronized(
                    SynchronizationInfo{finishedMode, sequence, appliedOccurrences, ignoredOccurrences, initial, snapshotFallback});
            } catch (...) {
                diagnostic(
                    DiagnosticSeverity::Warning, "a synchronization callback threw and was contained", ClientErrorCode::CallbackFailure);
            }
        }
    }

    std::optional<std::string> ClientCore::Impl::allocateRequestId() noexcept {
        if (requestIdsExhausted || !attachment.has_value() || nextRequestId == std::numeric_limits<std::uint64_t>::max()) {
            requestIdsExhausted = true;
            return std::nullopt;
        }
        try {
            std::string result = options.requestIdPrefix + std::to_string(attachment->generation) + "-r" + std::to_string(nextRequestId);
            ++nextRequestId;
            return result;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool ClientCore::Impl::methodAvailable(generated::MethodId method) const noexcept {
        const generated::MethodMetadata* metadata = methodMetadata(method);
        if (metadata == nullptr || !metadata->currentlyImplemented || !session.has_value()) {
            return false;
        }
        return !session->availableMethods.has_value() || contains(*session->availableMethods, method);
    }

    bool ClientCore::Impl::methodPermitted(generated::MethodId method) const noexcept {
        if (!methodAvailable(method) || !session.has_value()) {
            return false;
        }
        return !session->permittedMethods.has_value() || contains(*session->permittedMethods, method);
    }

    std::size_t ClientCore::Impl::outstandingOperationCount() const noexcept {
        std::size_t result = pending.size();
        const auto add = [&result](bool present) {
            if (present && result != std::numeric_limits<std::size_t>::max()) {
                ++result;
            }
        };
        add(synchronization.has_value() && synchronization->acceptedOperation.has_value());
        add(synchronization.has_value() && synchronization->projectionSnapshotRequested);
        return result;
    }

    void ClientCore::Impl::rememberCompleted(std::string requestId) {
        if (completedRequestIds.size() == options.limits.maximumCompletedRequestIds) {
            completedRequestIds.pop_front();
        }
        completedRequestIds.push_back(std::move(requestId));
    }

    bool ClientCore::Impl::requestProjectionSnapshot() {
        if (!attachment.has_value() || !synchronization.has_value() ||
            outstandingOperationCount() >= options.limits.maximumPendingOperations) {
            return false;
        }
        std::optional<std::string> requestId = allocateRequestId();
        if (!requestId.has_value()) {
            return false;
        }
        const PhysicalGeneration generation = attachment->generation;
        generated::DefinedCommand command{
            *requestId,
            generated::makeParameters(generated::MethodId::SnapshotGet, Json::object()),
            Json::object(),
            Json::object(),
        };
        auto validated = Codec::encodeDefinedCommand(command);
        if (!validated) {
            securelyErase(command);
            return false;
        }
        Json validatedWire = std::move(validated).value();
        securelyErase(validatedWire);
        projectionSnapshotRequestId = *requestId;
        synchronization->projectionSnapshotRequested = true;
        synchronization->projectionSnapshotResponseAccepted = false;
        SendResult sent;
        OutboundMessage outbound{std::move(command), false};
        try {
            sent = attachment->transport.send(std::move(outbound));
        } catch (...) {
            sent = {SendStatus::Failed, TransportError{"transport send callback failed", true}};
        }
        securelyErase(outbound);
        if (!continues(generation, ConnectionState::Synchronizing) || !synchronization.has_value()) {
            return false;
        }
        if (sent.status != SendStatus::Accepted) {
            if (projectionSnapshotRequestId == requestId) {
                projectionSnapshotRequestId.reset();
                synchronization->projectionSnapshotRequested = false;
            }
            return false;
        }
        return true;
    }

    bool ClientCore::Impl::cancelSynchronization() noexcept {
        if (!synchronization.has_value()) {
            return true;
        }
        LifecycleCheckpoint checkpoint = lifecycleCheckpoint();
        const bool explicitRequest = synchronization->explicitRequest;
        synchronization.reset();
        if (explicitRequest && connectionState == ConnectionState::Synchronizing) {
            checkpoint.state = ConnectionState::Ready;
            transition(ConnectionState::Ready);
        }
        return continues(checkpoint);
    }

    void ClientCore::Impl::flushDeferredCommands() noexcept {
        if (dispatchDepth != 0 || flushingDeferredCommands) {
            return;
        }
        flushingDeferredCommands = true;
        try {
            while (!deferredCommands.empty()) {
                DeferredCommand deferred = std::move(deferredCommands.front());
                securelyErase(deferredCommands.front().message);
                deferredCommands.pop_front();

                auto operationFound = std::find_if(pending.begin(), pending.end(), [&deferred](const PendingOperation& operation) {
                    return operation.requestId == deferred.requestId;
                });
                if (operationFound == pending.end()) {
                    securelyErase(deferred.message);
                    continue;
                }

                const bool synchronizationCommand = operationFound->synchronization;
                const bool activatesSynchronization = synchronizationCommand && synchronization.has_value() &&
                                                      synchronization->explicitRequest && !synchronization->responseAccepted &&
                                                      connectionState == ConnectionState::Ready;
                if (activatesSynchronization) {
                    transition(ConnectionState::Synchronizing);
                    operationFound = std::find_if(pending.begin(), pending.end(), [&deferred](const PendingOperation& operation) {
                        return operation.requestId == deferred.requestId;
                    });
                    if (operationFound == pending.end()) {
                        securelyErase(deferred.message);
                        continue;
                    }
                }

                const bool sendableState =
                    connectionState == ConnectionState::Ready ||
                    (connectionState == ConnectionState::Synchronizing && synchronizationCommand && synchronization.has_value());
                if (!owns(deferred.generation) || !attachment->connected || !sendableState) {
                    PendingOperation operation = std::move(*operationFound);
                    pending.erase(operationFound);
                    securelyErase(deferred.message);
                    if (synchronizationCommand) {
                        if (!cancelSynchronization()) {
                            break;
                        }
                    }
                    complete(std::move(operation),
                             std::nullopt,
                             localError(ClientErrorCode::NotConnected, "frontend connection closed before deferred command send", true));
                    continue;
                }

                SendResult sent;
                bool sendThrew = false;
                try {
                    sent = attachment->transport.send(std::move(deferred.message));
                } catch (...) {
                    sendThrew = true;
                    sent = {SendStatus::Failed, TransportError{"transport send callback failed", true}};
                }
                securelyErase(deferred.message);
                if (!owns(deferred.generation)) {
                    continue;
                }
                if (sent.status == SendStatus::Accepted) {
                    continue;
                }

                const bool retryable = sendThrew || (sent.error.has_value() && sent.error->retryable);
                operationFound = std::find_if(pending.begin(), pending.end(), [&deferred](const PendingOperation& operation) {
                    return operation.requestId == deferred.requestId;
                });
                if (operationFound != pending.end()) {
                    PendingOperation operation = std::move(*operationFound);
                    pending.erase(operationFound);
                    if (synchronizationCommand) {
                        if (!cancelSynchronization()) {
                            break;
                        }
                    }
                    complete(std::move(operation),
                             std::nullopt,
                             localError(ClientErrorCode::SendRejected,
                                        sendThrew ? "frontend transport send failed" : "frontend transport rejected command",
                                        retryable));
                }
                if (owns(deferred.generation)) {
                    failConnection(transportError(ClientErrorCode::TransportFailure,
                                                  sendThrew ? "transport send callback failed" : "transport rejected frontend command",
                                                  retryable),
                                   "frontend deferred command send failed",
                                   true);
                    break;
                }
            }
        } catch (...) {
            const std::optional<PhysicalGeneration> failingGeneration =
                attachment ? std::optional<PhysicalGeneration>{attachment->generation} : std::nullopt;
            if (failingGeneration.has_value() && owns(*failingGeneration)) {
                failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "frontend deferred command dispatch failed"),
                               "frontend deferred command dispatch failed",
                               true);
            }
        }
        flushingDeferredCommands = false;
    }

    Submission ClientCore::Impl::submit(generated::CompleteCommandParameters parameters, OperationCompletion completion) {
        const generated::MethodId method = generated::commandMethod(parameters);
        const generated::MethodMetadata* metadata = methodMetadata(method);
        if (metadata == nullptr) {
            return {std::nullopt, localError(ClientErrorCode::InvalidConfiguration, "unknown generated frontend method")};
        }
        if (connectionState == ConnectionState::Closed) {
            return {std::nullopt, localError(ClientErrorCode::Closed, "frontend client is closed")};
        }
        if (connectionState != ConnectionState::Ready || !attachment.has_value() || !attachment->connected) {
            return {std::nullopt, localError(ClientErrorCode::NotReady, "frontend client is not ready")};
        }
        if (!methodAvailable(method)) {
            return {std::nullopt, localError(ClientErrorCode::MethodUnavailable, "frontend method is unavailable")};
        }
        if (!methodPermitted(method)) {
            return {std::nullopt, localError(ClientErrorCode::MethodNotPermitted, "frontend method is not permitted")};
        }
        if (outstandingOperationCount() >= options.limits.maximumPendingOperations) {
            return {std::nullopt,
                    localError(ClientErrorCode::TooManyPendingOperations, "frontend pending-operation capacity is exhausted", true)};
        }

        const bool requestsSynchronization = method == generated::MethodId::SnapshotGet || method == generated::MethodId::EventsReplay;
        if (requestsSynchronization && synchronization.has_value()) {
            return {std::nullopt, localError(ClientErrorCode::SynchronizationAlreadyActive, "frontend synchronization is already active")};
        }

        std::optional<std::string> reverseIdentity = reverseResponseIdentity(parameters);
        if (metadata->category == generated::MethodCategory::ReverseResponse) {
            if (!reverseIdentity.has_value()) {
                return {std::nullopt,
                        localError(ClientErrorCode::SerializationFailed, "reverse response lacks a stable pending-request identity")};
            }
            const model::PendingRequest* pendingRequest = published->pendingRequest(*reverseIdentity);
            if (pendingRequest != nullptr && model::pendingRequestData(*pendingRequest).connectionInvalidated) {
                return {
                    std::nullopt,
                    localError(ClientErrorCode::MethodNotPermitted, "frontend pending request belongs to an inactive connection session")};
            }
        }

        std::optional<std::string> requestId = allocateRequestId();
        if (!requestId.has_value()) {
            return {std::nullopt, localError(ClientErrorCode::RequestIdExhausted, "frontend request IDs are exhausted")};
        }
        generated::DefinedCommand command{*requestId, std::move(parameters), Json::object(), Json::object()};
        auto validated = Codec::encodeDefinedCommand(command);
        if (!validated) {
            securelyErase(command);
            return {std::nullopt, localError(ClientErrorCode::SerializationFailed, "generated frontend command failed schema validation")};
        }
        Json validatedWire = std::move(validated).value();
        securelyErase(validatedWire);

        PendingOperation operation{*requestId, method, std::move(completion), requestsSynchronization};
        pending.push_back(operation);

        if (requestsSynchronization) {
            Synchronization next;
            next.mode = method == generated::MethodId::SnapshotGet ? SyncMode::Snapshot : SyncMode::Replay;
            next.target = model::FrontendSequence::maximum();
            next.initial = false;
            next.explicitRequest = true;
            next.responseAccepted = false;
            next.representation = published->representation;
            next.representedThrough = published->synchronizedThrough;
            if (next.mode == SyncMode::Replay) {
                if (published->visibleSequence.has_value() &&
                    (!next.representedThrough.has_value() || *published->visibleSequence > *next.representedThrough)) {
                    next.representedThrough = published->visibleSequence;
                }
                if (published->snapshot) {
                    next.staging = *published->snapshot;
                } else {
                    model::CanonicalSnapshot empty;
                    empty.sequence = next.representedThrough.value_or(model::FrontendSequence{});
                    next.staging = std::move(empty);
                }
            }
            synchronization = std::move(next);
        }

        const PhysicalGeneration submittingGeneration = attachment->generation;
        const bool sensitive = bindingIsSensitive(method);
        OutboundMessage outbound{std::move(command), sensitive};
        if (dispatchDepth != 0 || flushingDeferredCommands) {
            deferredCommands.push_back(DeferredCommand{submittingGeneration, *requestId, std::move(outbound)});
            securelyErase(outbound);
            return {std::move(requestId), std::nullopt};
        }

        SendResult sent;
        bool sendThrew = false;
        try {
            sent = attachment->transport.send(std::move(outbound));
        } catch (...) {
            sendThrew = true;
            sent = {SendStatus::Failed, TransportError{"transport send callback failed", true}};
        }
        securelyErase(outbound);

        if (!owns(submittingGeneration)) {
            return {std::nullopt,
                    localError(ClientErrorCode::NotConnected,
                               "frontend connection changed during command send",
                               true)};
        }

        if (sent.status != SendStatus::Accepted) {
            const auto found = std::find_if(pending.begin(), pending.end(), [&requestId](const PendingOperation& value) {
                return value.requestId == *requestId;
            });
            if (found != pending.end()) {
                pending.erase(found);
            }
            if (requestsSynchronization && synchronization.has_value() && !synchronization->responseAccepted) {
                synchronization.reset();
            }
            const bool retryable = sendThrew || (sent.error.has_value() && sent.error->retryable);
            if (owns(submittingGeneration)) {
                failConnection(transportError(ClientErrorCode::TransportFailure,
                                              sendThrew ? "transport send callback failed" : "transport rejected frontend command",
                                              retryable),
                               sendThrew ? "frontend command send failed" : "frontend command rejected",
                               true);
            }
            return {std::nullopt,
                    localError(ClientErrorCode::SendRejected,
                               sendThrew ? "frontend transport send failed" : "frontend transport rejected command",
                               retryable)};
        }
        if (requestsSynchronization && synchronization.has_value() && owns(submittingGeneration) &&
            connectionState == ConnectionState::Ready) {
            transition(ConnectionState::Synchronizing);
        }
        return {std::move(requestId), std::nullopt};
    }

    void ClientCore::Impl::handleResponse(const Response& response) {
        if (projectionSnapshotRequestId.has_value() && response.requestId == *projectionSnapshotRequestId) {
            if (!synchronization.has_value() || !response.ok || !response.result.has_value()) {
                failConnection(protocolError(ClientErrorCode::ResponseTypeMismatch, "projection-refresh snapshot command failed"),
                               "projection-refresh snapshot command failed",
                               true);
                return;
            }
            auto decoded = Codec::decodeDefinedResult(generated::MethodId::SnapshotGet, *response.result);
            if (!decoded || generated::commandMethod(decoded.value()) != generated::MethodId::SnapshotGet) {
                failConnection(protocolError(ClientErrorCode::ResponseTypeMismatch, "projection-refresh snapshot result type is invalid"),
                               "projection-refresh snapshot command failed",
                               true);
                return;
            }
            rememberCompleted(response.requestId);
            projectionSnapshotRequestId.reset();
            synchronization->projectionSnapshotResponseAccepted = true;
            synchronization->projectionRefreshRequired = false;
            synchronization->mode = SyncMode::Snapshot;
            synchronization->staging.reset();
            synchronization->representation = RepresentationMode::Unknown;
            synchronization->lastBatchSequence.reset();
            synchronization->sawSnapshot = false;
            synchronization->sawEvents = false;
            return;
        }
        const auto found = std::find_if(pending.begin(), pending.end(), [&response](const PendingOperation& operation) {
            return operation.requestId == response.requestId;
        });
        if (found == pending.end()) {
            failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "unsolicited or duplicate frontend response"),
                           "frontend response correlation failed",
                           true);
            return;
        }

        PendingOperation operation = std::move(*found);
        pending.erase(found);
        rememberCompleted(response.requestId);

        if (!response.ok) {
            if (operation.synchronization) {
                if (!cancelSynchronization()) {
                    return;
                }
            }
            complete(std::move(operation),
                     std::nullopt,
                     response.error.has_value()
                         ? commandError(*response.error)
                         : ClientError{ErrorOrigin::Command, std::nullopt, std::nullopt, "frontend command failed", std::nullopt, false});
            return;
        }
        if (!response.result.has_value()) {
            const ClientError error = protocolError(ClientErrorCode::ResponseTypeMismatch, "successful frontend response has no result");
            const std::optional<PhysicalGeneration> completingGeneration =
                attachment ? std::optional<PhysicalGeneration>{attachment->generation} : std::nullopt;
            complete(std::move(operation), std::nullopt, error);
            if (completingGeneration.has_value() && owns(*completingGeneration)) {
                failConnection(error, "frontend response result missing", true);
            }
            return;
        }
        auto decoded = Codec::decodeDefinedResult(operation.method, *response.result);
        if (!decoded || generated::commandMethod(decoded.value()) != operation.method) {
            const ClientError error =
                protocolError(ClientErrorCode::ResponseTypeMismatch, "frontend response result does not match request");
            const std::optional<PhysicalGeneration> completingGeneration =
                attachment ? std::optional<PhysicalGeneration>{attachment->generation} : std::nullopt;
            complete(std::move(operation), std::nullopt, error);
            if (completingGeneration.has_value() && owns(*completingGeneration)) {
                failConnection(error, "frontend response type mismatch", true);
            }
            return;
        }
        if (operation.synchronization && synchronization.has_value()) {
            synchronization->responseAccepted = true;
            synchronization->acceptedOperation = std::move(operation);
            synchronization->acceptedResult = std::move(decoded).value();
            if (connectionState == ConnectionState::Ready) {
                transition(ConnectionState::Synchronizing);
            }
            return;
        }
        complete(std::move(operation), std::move(decoded).value(), std::nullopt);
    }

    void ClientCore::Impl::handleProtocolError(const ProtocolErrorMessage& error, bool& accepted, bool& observeAfterClosure) {
        ClientError decoded{ErrorOrigin::Protocol, std::nullopt, error.code, error.message, error.details, false};
        if (error.closeConnection) {
            accepted = true;
            observeAfterClosure = true;
            failConnection(std::move(decoded), "frontend requested protocol close", true);
            return;
        }
        if (!error.requestId.has_value()) {
            accepted = true;
            const LifecycleCheckpoint checkpoint = lifecycleCheckpoint();
            notifyError(decoded);
            if (!continues(checkpoint)) {
                return;
            }
            diagnostic(DiagnosticSeverity::Warning, "frontend server reported a non-closing protocol error", std::nullopt, decoded);
            return;
        }
        if (projectionSnapshotRequestId.has_value() && *error.requestId == *projectionSnapshotRequestId) {
            accepted = true;
            observeAfterClosure = true;
            failConnection(std::move(decoded), "frontend projection refresh was rejected", true);
            return;
        }
        const auto found = std::find_if(pending.begin(), pending.end(), [&error](const PendingOperation& operation) {
            return operation.requestId == *error.requestId;
        });
        if (found == pending.end()) {
            failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "uncorrelated frontend protocol error response", error.code),
                           "frontend protocol-error correlation failed",
                           true);
            return;
        }
        PendingOperation operation = std::move(*found);
        pending.erase(found);
        rememberCompleted(*error.requestId);
        if (operation.synchronization) {
            if (!cancelSynchronization()) {
                return;
            }
        }
        accepted = true;
        complete(std::move(operation), std::nullopt, std::move(decoded));
    }

    bool ClientCore::Impl::receive(PhysicalGeneration generation, const ServerMessage& message) {
        if (!owns(generation) || !attachment->connected || connectionState == ConnectionState::Connecting ||
            connectionState == ConnectionState::Disconnected || connectionState == ConnectionState::Closing ||
            connectionState == ConnectionState::Closed) {
            return false;
        }

        const auto encoded = Codec::encodeServer(message);
        if (!encoded) {
            failConnection(protocolError(ClientErrorCode::DecodeFailure, "failed to encode frontend server message"),
                           "frontend server message encoding failed",
                           true);
            return false;
        }
        try {
            if (encoded.value().dump().size() > options.limits.maximumInboundMessageBytes) {
                failConnection(protocolError(ClientErrorCode::DecodeFailure,
                                             "frontend message exceeds the inbound byte limit",
                                             ErrorCode::FrameTooLarge),
                               "frontend message too large",
                               true);
                return false;
            }
        } catch (...) {
            failConnection(protocolError(ClientErrorCode::DecodeFailure, "frontend message size could not be measured"),
                           "frontend message rejected",
                           true);
            return false;
        }

        if (dispatchDepth == std::numeric_limits<std::size_t>::max()) {
            failConnection(protocolError(ClientErrorCode::StateCapacityExceeded, "frontend dispatch depth exhausted"),
                           "frontend dispatch depth exhausted",
                           true);
            return false;
        }
        ++dispatchDepth;

        bool semanticallyAccepted = false;
        bool observeAfterProtocolErrorClosure = false;
        try {
            std::visit(
                [this, &semanticallyAccepted, &observeAfterProtocolErrorClosure](const auto& value) {
                    using Value = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, Welcome>) {
                        handleWelcome(value);
                    } else if constexpr (std::is_same_v<Value, Snapshot>) {
                        handleSnapshot(value);
                    } else if constexpr (std::is_same_v<Value, EventBatch>) {
                        handleEvents(value);
                    } else if constexpr (std::is_same_v<Value, SyncComplete>) {
                        handleSyncComplete(value);
                    } else if constexpr (std::is_same_v<Value, Response>) {
                        handleResponse(value);
                    } else if constexpr (std::is_same_v<Value, ProtocolErrorMessage>) {
                        handleProtocolError(value, semanticallyAccepted, observeAfterProtocolErrorClosure);
                    }
                },
                message);
            const bool stillAttached = owns(generation) && connectionState != ConnectionState::Closed;
            if (!std::holds_alternative<ProtocolErrorMessage>(message)) {
                semanticallyAccepted = stillAttached;
            }
            if (semanticallyAccepted && (stillAttached || observeAfterProtocolErrorClosure) && callbacks.onProtocolMessage) {
                try {
                    callbacks.onProtocolMessage(message);
                } catch (...) {
                    diagnostic(DiagnosticSeverity::Warning,
                               "a protocol-message callback threw and was contained",
                               ClientErrorCode::CallbackFailure);
                }
            }
        } catch (...) {
            --dispatchDepth;
            if (owns(generation)) {
                failConnection(protocolError(ClientErrorCode::UnexpectedMessage, "frontend internal message dispatch failed"),
                               "frontend internal message dispatch failed",
                               true);
            }
            return false;
        }
        --dispatchDepth;
        if (dispatchDepth == 0) {
            flushDeferredCommands();
        }
        return owns(generation) && connectionState != ConnectionState::Disconnected;
    }

    bool ClientCore::Impl::receiveEncoded(PhysicalGeneration generation, std::string_view message) {
        if (!owns(generation)) {
            return false;
        }
        if (message.size() > options.limits.maximumInboundMessageBytes) {
            failConnection(
                protocolError(ClientErrorCode::DecodeFailure, "frontend message exceeds the inbound byte limit", ErrorCode::FrameTooLarge),
                "frontend message too large",
                true);
            return false;
        }
        auto decoded = Codec::decodeServer(message);
        if (!decoded) {
            failConnection(protocolError(ClientErrorCode::DecodeFailure, "failed to decode frontend server message", decoded.error().code),
                           "frontend server message decode failed",
                           true);
            return false;
        }
        return receive(generation, decoded.value());
    }

    void ClientCore::Impl::close(std::string_view reason) {
        if (connectionState == ConnectionState::Closed || clientCloseInProgress) {
            return;
        }
        clientCloseInProgress = true;
        const std::optional<PhysicalGeneration> closingGeneration =
            attachment ? std::optional<PhysicalGeneration>{attachment->generation} : std::nullopt;
        transition(ConnectionState::Closing);
        std::optional<TransportCallbacks> transport;
        if (attachment.has_value()) {
            transport = attachment->transport;
        }
        std::optional<PendingOperation> synchronizationOperation;
        if (synchronization.has_value() && synchronization->acceptedOperation.has_value()) {
            synchronizationOperation = std::move(synchronization->acceptedOperation);
        }
        synchronization.reset();
        const ClientError closedError = localError(ClientErrorCode::Closed, "frontend client closed");
        static_cast<void>(failPending(closedError));
        if (synchronizationOperation.has_value()) {
            complete(std::move(*synchronizationOperation), std::nullopt, closedError);
        }
        if (closingGeneration.has_value() && owns(*closingGeneration) && transport.has_value() && transport->close) {
            try {
                transport->close(reason);
            } catch (...) {
                diagnostic(
                    DiagnosticSeverity::Warning, "the transport close callback threw and was contained", ClientErrorCode::CallbackFailure);
            }
        }
        capabilities.reset();
        activeContinuityKey.reset();
        activeProjectionFingerprint.reset();
        projectionSnapshotRequestId.reset();
        helloResumeAfter.reset();
        makeRetainedStateStale();
        attachment.reset();
        transition(ConnectionState::Closed, closedError);
        clientCloseInProgress = false;
    }

    ClientCore::ClientCore(ClientOptions options, ClientCallbacks callbacks)
        : impl(std::make_unique<Impl>(std::move(options), std::move(callbacks))) {
    }

    ClientCore::~ClientCore() {
        if (!impl) {
            return;
        }
        try {
            impl->close("frontend client destroyed");
        } catch (...) {
            // Destruction is the terminal exception-containment boundary.
        }
    }
    ClientCore::ClientCore(ClientCore&&) noexcept = default;
    ClientCore& ClientCore::operator=(ClientCore&&) noexcept = default;

    std::optional<PhysicalGeneration> ClientCore::attach(TransportCallbacks callbacks) {
        return impl->attach(std::move(callbacks));
    }

    void ClientCore::transportConnected(PhysicalGeneration generation) {
        impl->transportConnected(generation);
    }

    void ClientCore::transportDisconnected(PhysicalGeneration generation, TransportError error) {
        impl->transportDisconnected(generation, std::move(error));
    }

    void ClientCore::detach(PhysicalGeneration generation, std::string_view reason) {
        impl->detach(generation, reason);
    }

    bool ClientCore::receive(PhysicalGeneration generation, const ServerMessage& message) {
        return impl->receive(generation, message);
    }

    bool ClientCore::receiveEncoded(PhysicalGeneration generation, std::string_view message) {
        return impl->receiveEncoded(generation, message);
    }

    Submission ClientCore::submit(generated::CompleteCommandParameters parameters, OperationCompletion completion) {
        return impl->submit(std::move(parameters), std::move(completion));
    }

    Submission ClientCore::requestSnapshot(OperationCompletion completion) {
        return submit(generated::makeParameters(generated::MethodId::SnapshotGet, Json::object()), std::move(completion));
    }

    Submission ClientCore::requestReplay(model::FrontendSequence after, OperationCompletion completion) {
        return submit(generated::makeParameters(generated::MethodId::EventsReplay, Json{{"after", after.value()}}), std::move(completion));
    }

    void ClientCore::close(std::string_view reason) {
        impl->close(reason);
    }

    ConnectionState ClientCore::connectionState() const noexcept {
        return impl->connectionState;
    }

    bool ClientCore::ready() const noexcept {
        return impl->connectionState == ConnectionState::Ready;
    }

    std::optional<PhysicalGeneration> ClientCore::activeGeneration() const noexcept {
        return impl->attachment ? std::optional<PhysicalGeneration>{impl->attachment->generation} : std::nullopt;
    }

    std::size_t ClientCore::pendingOperationCount() const noexcept {
        return impl->outstandingOperationCount();
    }

    std::shared_ptr<const PublishedState> ClientCore::state() const noexcept {
        return impl->published;
    }

    const std::vector<Diagnostic>& ClientCore::diagnostics() const noexcept {
        return impl->retainedDiagnostics;
    }

    bool ClientCore::capabilitySelected(FrontendCapability capability) const noexcept {
        return impl->session.has_value() && contains(impl->session->selectedCapabilities, capability);
    }

    CapabilityStatus ClientCore::capabilityStatus(FrontendCapability capability) const noexcept {
        CapabilityStatus result;
        result.capability = capability;
        if (!validCapability(capability)) {
            return result;
        }
        result.defined = Availability::Yes;
        if (!impl->capabilities.has_value()) {
            return result;
        }
        result.implemented = contains(impl->capabilities->implemented, capability) ? Availability::Yes : Availability::No;
        result.permitted = contains(impl->capabilities->permitted, capability) ? Availability::Yes : Availability::No;
        return result;
    }

    bool ClientCore::methodAvailable(generated::MethodId method) const noexcept {
        return impl->methodAvailable(method);
    }

    bool ClientCore::methodPermitted(generated::MethodId method) const noexcept {
        return impl->methodPermitted(method);
    }

    MethodStatus ClientCore::methodStatus(generated::MethodId method) const noexcept {
        MethodStatus result;
        result.method = method;
        const generated::MethodMetadata* metadata = methodMetadata(method);
        if (metadata == nullptr) {
            return result;
        }
        result.controllerRequired = metadata->controllerRequired;
        result.providerReadyRequired = metadata->providerReadyRequired;
        result.defaultEnabled = metadata->defaultEnabled;
        result.requiredScopes.assign(metadata->requiredScopes.begin(), metadata->requiredScopes.end());
        if (impl->session.has_value() && impl->session->availableMethods.has_value()) {
            result.available = contains(*impl->session->availableMethods, method) ? Availability::Yes : Availability::No;
        }
        if (impl->session.has_value() && impl->session->permittedMethods.has_value()) {
            result.permitted = contains(*impl->session->permittedMethods, method) ? Availability::Yes : Availability::No;
        }
        return result;
    }

} // namespace ai::openai::codex::frontend::internal::client
