/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/internal/model/Occurrence.h"

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Protocol.h"

#include <algorithm>
#include <array>
#include <exception>
#include <iterator>
#include <limits>
#include <type_traits>

namespace ai::openai::codex::frontend::internal::model {
    namespace {
        struct OccurrenceFailure final : std::exception {
            explicit OccurrenceFailure(OccurrenceError value)
                : error(std::move(value)) {
            }
            OccurrenceError error;
        };

        [[noreturn]] void fail(OccurrenceErrorCode code, std::string path, std::string message) {
            throw OccurrenceFailure(OccurrenceError{code, std::move(path), std::move(message)});
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
                bool valid = true;
                for (std::size_t index = 1; index < width; ++index) {
                    valid = valid && (static_cast<unsigned char>(value[offset + index]) & 0xc0U) == 0x80U;
                }
                if (!valid) {
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

        SafeDetail safeDetail(Json value, const std::string& path) {
            SafeDetailError error = SafeDetailError::None;
            auto detail = SafeDetail::fromJson(std::move(value), &error);
            if (!detail.has_value()) {
                fail(OccurrenceErrorCode::UnsafeDetail,
                     path,
                     "unsafe or over-capacity occurrence detail (code " + std::to_string(static_cast<unsigned int>(error)) + ")");
            }
            return std::move(*detail);
        }

        const Json& member(const Json& value, std::string_view key, std::string_view path) {
            const auto found = value.find(key);
            if (found == value.end()) {
                fail(OccurrenceErrorCode::InvalidPayload,
                     std::string(path) + "/" + std::string(key),
                     "required occurrence member is missing");
            }
            return *found;
        }

        std::optional<std::string> optionalString(const Json& value, std::string_view key) {
            const auto found = value.find(key);
            if (found == value.end() || found->is_null() || !found->is_string()) {
                return std::nullopt;
            }
            return found->get<std::string>();
        }

        std::optional<std::uint64_t> optionalUnsigned(const Json& value, std::string_view key) {
            const auto found = value.find(key);
            if (found == value.end()) {
                return std::nullopt;
            }
            if (found->is_number_unsigned()) {
                return found->get<std::uint64_t>();
            }
            if (found->is_number_integer()) {
                const std::int64_t signedValue = found->get<std::int64_t>();
                if (signedValue >= 0) {
                    return static_cast<std::uint64_t>(signedValue);
                }
            }
            return std::nullopt;
        }

        Json encodedSnapshotState(const CanonicalSnapshot& snapshot,
                                  ItemContentWireMode itemContentMode = ItemContentWireMode::Replacement) {
            const auto typed = encodeSnapshot(snapshot, itemContentMode);
            if (!typed) {
                fail(OccurrenceErrorCode::EncodingFailure, typed.error().path, typed.error().message);
            }
            const auto encoded = Codec::encodeExpandedSnapshot(typed.value());
            if (!encoded) {
                fail(OccurrenceErrorCode::EncodingFailure, "/state", encoded.error().message);
            }
            return member(encoded.value(), "state", "");
        }

        CanonicalSnapshot decodedSnapshotState(Json state,
                                               FrontendSequence sequence,
                                               ItemContentWireMode itemContentMode = ItemContentWireMode::Replacement) {
            Json envelope{{"protocol", ProtocolIdentity},
                          {"version", ProtocolVersion},
                          {"kind", kind::Snapshot},
                          {"sequence", sequence.value()},
                          {"state", std::move(state)}};
            const auto wire = Codec::decodeExpandedSnapshot(envelope);
            if (!wire) {
                fail(OccurrenceErrorCode::InvalidPayload, "/data", wire.error().message);
            }
            auto decoded = decodeSnapshot(wire.value(), itemContentMode);
            if (!decoded) {
                fail(OccurrenceErrorCode::InvalidPayload, decoded.error().path, decoded.error().message);
            }
            return std::move(decoded).value();
        }

        CanonicalSnapshot decodedLegacySnapshotState(Json state, FrontendSequence sequence) {
            if (!state.is_object()) {
                fail(OccurrenceErrorCode::InvalidPayload, "/data", "legacy occurrence state must be an object");
            }
            // A legacy occurrence carries one state fragment, whereas the
            // public Snapshot contract requires these four roots.  Complete
            // only this private synthetic envelope; decodeLegacySnapshot must
            // remain strict for externally received full snapshots.
            state["lifecycle"] = state.value("lifecycle", "stopped");
            state["sessions"] = state.value("sessions", Json::array());
            state["threadList"] = state.value("threadList", Json{{"hasLoadedPage", false}, {"complete", false}, {"pagesLoaded", 0}});
            state["threads"] = state.value("threads", Json::array());
            state["pendingRequests"] = state.value("pendingRequests", Json::array());
            Snapshot wire{sequence.protocolValue(), std::move(state), Json::object()};
            auto decoded = decodeLegacySnapshot(wire);
            if (!decoded) {
                fail(OccurrenceErrorCode::InvalidPayload, decoded.error().path, decoded.error().message);
            }
            return std::move(decoded).value();
        }

        Json baseSnapshotState(FrontendSequence sequence) {
            CanonicalSnapshot snapshot;
            snapshot.sequence = sequence;
            return encodedSnapshotState(snapshot);
        }

        template <typename Value>
        const Value& payloadAs(const OccurrencePayload& payload) {
            const auto* value = std::get_if<Value>(&payload);
            if (value == nullptr) {
                fail(OccurrenceErrorCode::InvalidPayload, "/payload", "occurrence payload family does not match its discriminator");
            }
            return *value;
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

        Json legacyThread(const ThreadState& thread) {
            Json encoded = thread.safeDetails.json();
            if (!encoded.is_object()) {
                encoded = Json::object();
            }
            encoded.erase("realtime");
            encoded["id"] = thread.id.value();
            encoded["fullyLoaded"] = thread.fullyLoaded;
            if (thread.title.has_value()) {
                encoded["title"] = *thread.title;
            }
            if (thread.createdAtMs.has_value()) {
                encoded["createdAt"] = *thread.createdAtMs;
            }
            if (thread.updatedAtMs.has_value()) {
                encoded["updatedAt"] = *thread.updatedAtMs;
            }
            if (!encoded.contains("turns") || !encoded["turns"].is_array()) {
                encoded["turns"] = Json::array();
            }
            encoded["extensions"] = thread.legacyExtensions.json().is_object() ? thread.legacyExtensions.json() : Json::object();
            return encoded;
        }

        Json legacyTurn(const TurnState& turn) {
            Json encoded = turn.safeDetails.json();
            if (!encoded.is_object()) {
                encoded = Json::object();
            }
            encoded["id"] = turn.id.value();
            encoded["threadId"] = turn.threadId.value();
            encoded["status"] = turn.status.value_or("unknown");
            encoded["active"] = turn.active;
            encoded["terminal"] = turn.terminal;
            if (turn.plan) {
                CanonicalSnapshot snapshot;
                snapshot.turns = {turn};
                encoded["plan"] = encodedSnapshotState(snapshot).at("turns").at(0).at("plan");
            }
            if (!encoded.contains("items") || !encoded["items"].is_array()) {
                encoded["items"] = Json::array();
            }
            encoded["extensions"] = turn.legacyExtensions.json().is_object() ? turn.legacyExtensions.json() : Json::object();
            return encoded;
        }

        Json legacyItem(const ThreadItem& item) {
            const ItemData& value = itemData(item);
            const std::string discriminator = value.legacyDiscriminator.value_or(std::string(toString(threadItemKind(item))));
            Json data = value.safeDetails.has_value() && value.safeDetails->json().is_object() ? value.safeDetails->json() : Json::object();
            if (legacyMetadataOnlyItem(threadItemKind(item))) {
                data = Json{{"codexType", discriminator}};
            }
            const Json& extensions = value.legacyDiscriminator.has_value() ? value.legacyExtensions.json() : value.extensions.json();
            Json encoded{{"id", value.id.value()},
                         {"type", discriminator},
                         {"status", value.status.value_or("unknown")},
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
            Json data = value.safeDetails.has_value() && value.safeDetails->json().is_object() ? value.safeDetails->json() : Json::object();
            Json encoded{{"id", value.id.value()},
                         {"type", item.discriminator.empty() ? std::string{"unknown"} : item.discriminator},
                         {"status", value.status.value_or("unknown")},
                         {"agentText", value.agentText.value_or("")},
                         {"reasoningText", value.reasoningText.value_or("")},
                         {"reasoningSummary", value.reasoningSummary.value_or("")},
                         {"commandOutput", value.commandOutput.value_or("")},
                         {"droppedContentBytes", value.droppedContentBytes.value_or(0)},
                         {"contentTruncated", value.contentTruncated || value.truncation.truncated},
                         {"data", std::move(data)},
                         {"extensions", value.legacyExtensions.json().is_object() ? value.legacyExtensions.json() : Json::object()}};
            return encoded;
        }

        Json legacyTurn(const TurnUpsertedOccurrence& update) {
            Json encoded = legacyTurn(update.turn);
            encoded["items"] = Json::array();
            for (const ThreadItem& item : update.items) {
                const ItemData& data = itemData(item);
                if (data.turnId == std::optional<TurnIdentity>{update.turn.id} &&
                    (data.threadId == std::optional<ThreadIdentity>{update.turn.threadId} || !data.threadId.has_value())) {
                    encoded["items"].push_back(legacyItem(item));
                }
            }
            return encoded;
        }

        Json legacyThread(const ThreadUpsertedOccurrence& update) {
            Json encoded = legacyThread(update.thread);
            encoded["turns"] = Json::array();
            for (const TurnState& turn : update.turns) {
                if (turn.threadId != update.thread.id) {
                    continue;
                }
                const bool unscopedItemsBelongToTurn =
                    std::none_of(update.turns.begin(), update.turns.end(), [&](const TurnState& candidate) {
                        return candidate.id == turn.id && candidate.threadId != turn.threadId;
                    }) &&
                    std::none_of(update.items.begin(), update.items.end(), [&](const ThreadItem& candidate) {
                        const ItemData& data = itemData(candidate);
                        return data.turnId == std::optional<TurnIdentity>{turn.id} && data.threadId.has_value() &&
                               data.threadId != std::optional<ThreadIdentity>{turn.threadId};
                    });
                TurnUpsertedOccurrence turnUpdate{turn};
                for (const ThreadItem& item : update.items) {
                    const ItemData& data = itemData(item);
                    if (data.turnId == std::optional<TurnIdentity>{turn.id} &&
                        (data.threadId == std::optional<ThreadIdentity>{turn.threadId} ||
                         (!data.threadId.has_value() && unscopedItemsBelongToTurn))) {
                        turnUpdate.items.push_back(item);
                    }
                }
                encoded["turns"].push_back(legacyTurn(turnUpdate));
            }
            return encoded;
        }

        Json payloadData(const OccurrencePayload& payload,
                         FrontendSequence sequence,
                         ItemContentWireMode itemContentMode = ItemContentWireMode::Replacement) {
            CanonicalSnapshot snapshot;
            snapshot.sequence = sequence;
            const ExpandedEventType type = occurrenceType(payload);
            switch (type) {
                case ExpandedEventType::ProviderUpdated:
                    snapshot.provider = payloadAs<ProviderUpdatedOccurrence>(payload).provider;
                    return Json{{"provider", encodedSnapshotState(snapshot).at("provider")}};
                case ExpandedEventType::ControllerUpdated:
                    snapshot.controller = payloadAs<ControllerUpdatedOccurrence>(payload).controller;
                    return Json{{"controller", encodedSnapshotState(snapshot).at("controller")}};
                case ExpandedEventType::SessionsUpdated:
                    snapshot.sessions = payloadAs<SessionsUpdatedOccurrence>(payload).sessions;
                    return Json{{"sessions", encodedSnapshotState(snapshot).at("sessions")}};
                case ExpandedEventType::ThreadListUpdated:
                    snapshot.threadList = payloadAs<ThreadListUpdatedOccurrence>(payload).threadList;
                    return Json{{"threadList", encodedSnapshotState(snapshot).at("threadList")}};
                case ExpandedEventType::ThreadUpserted:
                    snapshot.threads = {payloadAs<ThreadUpsertedOccurrence>(payload).thread};
                    return Json{{"thread", encodedSnapshotState(snapshot).at("threads").at(0)}};
                case ExpandedEventType::ThreadRemoved:
                    return Json{{"threadId", payloadAs<ThreadRemovedOccurrence>(payload).threadId.value()}};
                case ExpandedEventType::TurnUpserted:
                    snapshot.turns = {payloadAs<TurnUpsertedOccurrence>(payload).turn};
                    return Json{{"turn", encodedSnapshotState(snapshot).at("turns").at(0)}};
                case ExpandedEventType::ItemUpserted:
                    snapshot.items = {payloadAs<ItemUpsertedOccurrence>(payload).item};
                    return Json{{"item", encodedSnapshotState(snapshot, itemContentMode).at("items").at(0)}};
                case ExpandedEventType::ItemContentUpdated: {
                    const auto& update = payloadAs<ItemContentUpdatedOccurrence>(payload);
                    Json data = update.extensions.json();
                    if (data.erase(ItemContentOverflowV1Property) != 0) {
                        fail(OccurrenceErrorCode::EncodingFailure,
                             "/data/" + std::string(ItemContentOverflowV1Property),
                             "item content extensions collide with the reserved overflow member");
                    }
                    if (data.erase(CommandOutputOverflowV2Property) != 0) {
                        fail(OccurrenceErrorCode::EncodingFailure,
                             "/data/" + std::string(CommandOutputOverflowV2Property),
                             "item content extensions collide with the reserved command-output overflow member");
                    }
                    if (update.threadId.has_value()) {
                        data["threadId"] = update.threadId->value();
                    }
                    if (update.turnId.has_value()) {
                        data["turnId"] = update.turnId->value();
                    }
                    data["itemId"] = update.itemId.value();
                    if (update.channel.has_value()) {
                        data["channel"] = *update.channel;
                    }
                    const bool agentOverflow = update.channel == std::optional<std::string>{"agentText"};
                    const bool commandOverflow = update.channel == std::optional<std::string>{"commandOutput"};
                    const bool commandOutputItem =
                        update.itemKind == std::optional<ThreadItemKind>{ThreadItemKind::CommandExecution} ||
                        update.itemKind == std::optional<ThreadItemKind>{ThreadItemKind::FileChange};
                    const bool overflowEnabled = update.overflowV1.has_value() &&
                                                 ((agentOverflow && itemContentMode != ItemContentWireMode::Replacement) ||
                                                  (commandOverflow && itemContentMode == ItemContentWireMode::AppendV2 && commandOutputItem));
                    if (update.overflowV1.has_value()) {
                        const ItemContentOverflowV1& overflow = *update.overflowV1;
                        const std::uint64_t suffixBytes = static_cast<std::uint64_t>(overflow.suffix.size());
                        const bool supportedChannel = agentOverflow || commandOverflow;
                        if (!supportedChannel || !update.content.has_value() ||
                            overflow.baseContentBytes != static_cast<std::uint64_t>(update.content->size()) ||
                            !update.contentTruncatedKnown || !update.truncation.truncated || !update.droppedContentBytesKnown ||
                            update.truncation.droppedBytes < suffixBytes ||
                            update.truncation.droppedBytes - suffixBytes != overflow.droppedContentBytesBeforeProjection) {
                            fail(OccurrenceErrorCode::EncodingFailure,
                                 "/data/" + std::string(commandOverflow ? CommandOutputOverflowV2Property
                                                                        : ItemContentOverflowV1Property),
                                 "item content overflow does not match the projected occurrence");
                        }
                    }
                    const bool exactAppendHint = [&update, itemContentMode, overflowEnabled, commandOverflow, commandOutputItem] {
                        if (!update.content.has_value() || !update.appendHint.has_value()) {
                            return false;
                        }
                        const ItemContentAppendHint& hint = *update.appendHint;
                        if (overflowEnabled && !hint.sourceVerified) {
                            return false;
                        }
                        const bool rollingCommandOutput =
                            commandOverflow && itemContentMode == ItemContentWireMode::AppendV2 && commandOutputItem;
                        if (hint.discardPrefixBytes != 0 && !rollingCommandOutput) {
                            return false;
                        }
                        if (utf8CharacterPrefixLength(hint.delta, 16U * 1024U) != hint.delta.size()) {
                            return false;
                        }
                        std::string verifiedContent = *update.content;
                        if (hint.sourceVerified && overflowEnabled) {
                            verifiedContent.append(update.overflowV1->suffix);
                        }
                        if (hint.discardPrefixBytes > hint.baseContentBytes) {
                            return false;
                        }
                        const std::uint64_t retainedBaseBytes = hint.baseContentBytes - hint.discardPrefixBytes;
                        if (retainedBaseBytes > verifiedContent.size() ||
                            hint.delta.size() != verifiedContent.size() - static_cast<std::size_t>(retainedBaseBytes)) {
                            return false;
                        }
                        const std::size_t maximumBytes = update.channel == std::optional<std::string>{"commandOutput"} &&
                                                                 itemContentMode == ItemContentWireMode::AppendV2
                                                             ? MaximumCommandOutputOverflowV2Bytes
                                                             : MaximumItemContentOverflowV1Bytes;
                        return verifiedContent.size() <= maximumBytes &&
                               utf8CharacterPrefixLength(verifiedContent, verifiedContent.size()) == verifiedContent.size() &&
                               verifiedContent.compare(
                                   static_cast<std::size_t>(retainedBaseBytes), hint.delta.size(), hint.delta) == 0;
                    }();
                    if (itemContentMode != ItemContentWireMode::Replacement && exactAppendHint) {
                        data["content"] = "";
                        data["contentDelta"] = update.appendHint->delta;
                        data["baseContentBytes"] = update.appendHint->baseContentBytes;
                        if (commandOverflow && itemContentMode == ItemContentWireMode::AppendV2) {
                            data["discardPrefixBytes"] = update.appendHint->discardPrefixBytes;
                        }
                    } else if (update.content.has_value()) {
                        data["content"] = *update.content;
                        if (overflowEnabled) {
                            const auto overflow = commandOverflow ? encodeCommandOutputOverflowV2(*update.overflowV1)
                                                                  : encodeItemContentOverflowV1(*update.overflowV1);
                            if (!overflow) {
                                fail(OccurrenceErrorCode::EncodingFailure, overflow.error().path, overflow.error().message);
                            }
                            if (commandOverflow &&
                                overflow.value().dump().size() > MaximumCommandOutputOverflowV2EncodedBytes) {
                                fail(OccurrenceErrorCode::EncodingFailure,
                                     "/data/" + std::string(CommandOutputOverflowV2Property),
                                     "command-output overflow exceeds its negotiated encoded bound");
                            }
                            data[std::string(commandOverflow ? CommandOutputOverflowV2Property : ItemContentOverflowV1Property)] =
                                overflow.value();
                        }
                    }
                    if (update.contentTruncatedKnown) {
                        data["contentTruncated"] =
                            itemContentMode != ItemContentWireMode::Replacement && exactAppendHint && overflowEnabled
                                ? update.overflowV1->contentTruncatedBeforeProjection
                                : update.truncation.truncated;
                    }
                    if (update.droppedContentBytesKnown) {
                        data["droppedContentBytes"] =
                            itemContentMode != ItemContentWireMode::Replacement && exactAppendHint && overflowEnabled
                                ? update.overflowV1->droppedContentBytesBeforeProjection
                                : update.truncation.droppedBytes;
                    }
                    return data;
                }
                case ExpandedEventType::PendingRequestsUpdated:
                    snapshot.pendingRequests = payloadAs<PendingRequestsUpdatedOccurrence>(payload).pendingRequests;
                    return Json{{"pendingRequests", encodedSnapshotState(snapshot).at("pendingRequests")}};
                case ExpandedEventType::AccountUpdated:
                    snapshot.accounts = payloadAs<AccountUpdatedOccurrence>(payload).account;
                    return Json{{"domain", encodedSnapshotState(snapshot).at("accounts")}};
                case ExpandedEventType::ModelsUpdated:
                    snapshot.models = payloadAs<ModelsUpdatedOccurrence>(payload).models;
                    return Json{{"domain", encodedSnapshotState(snapshot).at("models")}};
                case ExpandedEventType::ConfigurationUpdated:
                    snapshot.configuration = payloadAs<ConfigurationUpdatedOccurrence>(payload).configuration;
                    return Json{{"domain", encodedSnapshotState(snapshot).at("configuration")}};
                case ExpandedEventType::ProcessUpdated:
                    snapshot.processes = {payloadAs<ProcessUpdatedOccurrence>(payload).process};
                    return Json{{"process", encodedSnapshotState(snapshot).at("processes").at("entries").at(0)}};
                case ExpandedEventType::FilesystemWatchUpdated:
                    snapshot.filesystemWatches.state = DomainState::present();
                    snapshot.filesystemWatches.entries = {payloadAs<FilesystemWatchUpdatedOccurrence>(payload).filesystemWatch};
                    return Json{{"filesystemWatch", encodedSnapshotState(snapshot).at("filesystemWatches").at("entries").at(0)}};
                case ExpandedEventType::FuzzySearchUpdated:
                    snapshot.fuzzySearches.state = DomainState::present();
                    snapshot.fuzzySearches.entries = {payloadAs<FuzzySearchUpdatedOccurrence>(payload).fuzzySearch};
                    return Json{{"fuzzySearch", encodedSnapshotState(snapshot).at("fuzzySearches").at("entries").at(0)}};
                case ExpandedEventType::ReviewsUpdated:
                    snapshot.reviews = payloadAs<ReviewsUpdatedOccurrence>(payload).reviews;
                    return Json{{"domain", encodedSnapshotState(snapshot).at("reviews")}};
                case ExpandedEventType::IntegrationsUpdated:
                    snapshot.apps.state = payloadAs<IntegrationsUpdatedOccurrence>(payload).integrations.state;
                    return Json{{"domain", encodedSnapshotState(snapshot).at("apps")}};
                case ExpandedEventType::PluginsUpdated:
                    snapshot.plugins = payloadAs<PluginsUpdatedOccurrence>(payload).plugins;
                    return Json{{"domain", encodedSnapshotState(snapshot).at("plugins")}};
                case ExpandedEventType::SkillsUpdated:
                    snapshot.skills = payloadAs<SkillsUpdatedOccurrence>(payload).skills;
                    return Json{{"domain", encodedSnapshotState(snapshot).at("skills")}};
                case ExpandedEventType::McpUpdated:
                    snapshot.mcp = payloadAs<McpUpdatedOccurrence>(payload).mcp;
                    return Json{{"domain", encodedSnapshotState(snapshot).at("mcp")}};
                case ExpandedEventType::PlatformUpdated:
                    snapshot.windowsSandbox.state = payloadAs<PlatformUpdatedOccurrence>(payload).platform.state;
                    return Json{{"domain", encodedSnapshotState(snapshot).at("windowsSandbox")}};
                case ExpandedEventType::NoticeAdded:
                    snapshot.notices.state = DomainState::present();
                    snapshot.notices.entries = {payloadAs<NoticeAddedOccurrence>(payload).notice};
                    return Json{{"notice", encodedSnapshotState(snapshot).at("notices").at("entries").at(0)}};
                case ExpandedEventType::ActivityUpdated:
                    snapshot.activities.state = DomainState::present();
                    snapshot.activities.entries = {payloadAs<ActivityUpdatedOccurrence>(payload).activity};
                    return Json{{"activity", encodedSnapshotState(snapshot).at("activities").at("entries").at(0)}};
                case ExpandedEventType::CapacityUpdated:
                    snapshot.capacity = payloadAs<CapacityUpdatedOccurrence>(payload).capacity;
                    snapshot.capacityPresent = true;
                    return Json{{"capacity", encodedSnapshotState(snapshot).at("capacity")}};
                case ExpandedEventType::DiagnosticsUpdated: {
                    const DiagnosticRecord& diagnostic = payloadAs<DiagnosticsUpdatedOccurrence>(payload).diagnostic;
                    Json encoded = diagnostic.extensions.json();
                    if (diagnostic.received.has_value()) {
                        encoded["received"] = *diagnostic.received;
                    }
                    encoded["detailsOmitted"] = diagnostic.detailsOmitted;
                    if (diagnostic.message.has_value()) {
                        encoded["message"] = *diagnostic.message;
                    }
                    if (!diagnostic.safeDetails.empty()) {
                        encoded["details"] = diagnostic.safeDetails.json();
                    }
                    return Json{{"diagnostic", std::move(encoded)}};
                }
            }
            fail(OccurrenceErrorCode::UnsupportedFamily, "/payload", "unsupported expanded occurrence family");
        }

        void requireExpandedSemantics(const OccurrencePayload& payload, std::size_t index) {
            const std::string path = "/expandedPayloads/" + std::to_string(index);
            std::visit(
                [&](const auto& update) {
                    using Update = std::decay_t<decltype(update)>;
                    if constexpr (std::is_same_v<Update, SessionsUpdatedOccurrence>) {
                        if (!update.completeProjection) {
                            fail(OccurrenceErrorCode::EncodingFailure,
                                 path + "/completeProjection",
                                 "legacy session deltas have no lossless expanded-v1 encoding");
                        }
                    } else if constexpr (std::is_same_v<Update, ThreadUpsertedOccurrence>) {
                        if (update.replaceDescendants) {
                            fail(OccurrenceErrorCode::EncodingFailure,
                                 path + "/replaceDescendants",
                                 "legacy descendant replacement has no lossless expanded-v1 encoding");
                        }
                    } else if constexpr (std::is_same_v<Update, TurnUpsertedOccurrence>) {
                        if (update.replaceItems) {
                            fail(OccurrenceErrorCode::EncodingFailure,
                                 path + "/replaceItems",
                                 "legacy item replacement has no lossless expanded-v1 encoding");
                        }
                    } else if constexpr (std::is_same_v<Update, PendingRequestsUpdatedOccurrence>) {
                        if (!update.completeProjection) {
                            fail(OccurrenceErrorCode::EncodingFailure,
                                 path + "/completeProjection",
                                 "legacy pending-request deltas have no lossless expanded-v1 encoding");
                        }
                    } else if constexpr (std::is_same_v<Update, DiagnosticsUpdatedOccurrence>) {
                        if (update.aggregateLegacyUpdate) {
                            fail(OccurrenceErrorCode::EncodingFailure,
                                 path + "/aggregateLegacyUpdate",
                                 "legacy diagnostic aggregate replacement has no lossless expanded-v1 encoding");
                        }
                    }
                },
                payload);
        }

        void setWireExtensions(OccurrencePayload& payload, SafeDetail extensions) {
            std::visit(
                [&extensions](auto& value) {
                    value.extensions = std::move(extensions);
                },
                payload);
        }

        OccurrencePayload decodePayload(ExpandedEventType type,
                                        const Json& data,
                                        FrontendSequence sequence,
                                        ItemContentWireMode itemContentMode = ItemContentWireMode::Replacement) {
            Json state = baseSnapshotState(sequence);
            switch (type) {
                case ExpandedEventType::ProviderUpdated:
                    state["provider"] = member(data, "provider", "/data");
                    return ProviderUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).provider};
                case ExpandedEventType::ControllerUpdated:
                    state["controller"] = member(data, "controller", "/data");
                    return ControllerUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).controller};
                case ExpandedEventType::SessionsUpdated:
                    state["sessions"] = member(data, "sessions", "/data");
                    return SessionsUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).sessions};
                case ExpandedEventType::ThreadListUpdated:
                    state["threadList"] = member(data, "threadList", "/data");
                    return ThreadListUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).threadList};
                case ExpandedEventType::ThreadUpserted:
                    state["threads"] = Json::array({member(data, "thread", "/data")});
                    return ThreadUpsertedOccurrence{decodedSnapshotState(std::move(state), sequence).threads.at(0)};
                case ExpandedEventType::ThreadRemoved: {
                    const auto identity = optionalString(data, "threadId");
                    auto parsed = identity.has_value() ? ThreadIdentity::parse(*identity) : std::nullopt;
                    if (!parsed.has_value()) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/threadId", "thread identifier is invalid");
                    }
                    return ThreadRemovedOccurrence{std::move(*parsed)};
                }
                case ExpandedEventType::TurnUpserted:
                    state["turns"] = Json::array({member(data, "turn", "/data")});
                    return TurnUpsertedOccurrence{decodedSnapshotState(std::move(state), sequence).turns.at(0)};
                case ExpandedEventType::ItemUpserted:
                    state["items"] = Json::array({member(data, "item", "/data")});
                    return ItemUpsertedOccurrence{decodedSnapshotState(std::move(state), sequence, itemContentMode).items.at(0)};
                case ExpandedEventType::ItemContentUpdated: {
                    const auto identity = optionalString(data, "itemId");
                    auto parsed = identity.has_value() ? ItemIdentity::parse(*identity) : std::nullopt;
                    if (!parsed.has_value()) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/itemId", "item identifier is invalid");
                    }
                    ItemContentUpdatedOccurrence update{std::move(*parsed)};
                    if (const auto thread = optionalString(data, "threadId"); thread.has_value()) {
                        update.threadId = ThreadIdentity::parse(*thread);
                        if (!update.threadId.has_value()) {
                            fail(OccurrenceErrorCode::InvalidPayload, "/data/threadId", "thread identifier is invalid");
                        }
                    }
                    if (const auto turn = optionalString(data, "turnId"); turn.has_value()) {
                        update.turnId = TurnIdentity::parse(*turn);
                        if (!update.turnId.has_value()) {
                            fail(OccurrenceErrorCode::InvalidPayload, "/data/turnId", "turn identifier is invalid");
                        }
                    }
                    update.channel = optionalString(data, "channel");
                    const bool hasContent = data.contains("content");
                    const bool hasDelta = data.contains("contentDelta");
                    const bool hasBase = data.contains("baseContentBytes");
                    const bool hasDiscard = data.contains("discardPrefixBytes");
                    const auto overflowMember = data.find(ItemContentOverflowV1Property);
                    const auto commandOverflowMember = data.find(CommandOutputOverflowV2Property);
                    const bool hasAgentOverflow = overflowMember != data.end();
                    const bool hasCommandOverflow = commandOverflowMember != data.end();
                    const bool hasOverflow = hasAgentOverflow || hasCommandOverflow;
                    if (hasAgentOverflow && hasCommandOverflow) {
                        fail(OccurrenceErrorCode::InvalidPayload,
                             "/data/" + std::string(CommandOutputOverflowV2Property),
                             "item content update contains conflicting overflow representations");
                    }
                    if (!hasContent) {
                        fail(OccurrenceErrorCode::InvalidPayload,
                             "/data/content",
                             "item content update is missing its schema-required content field");
                    }
                    const auto wireContent = optionalString(data, "content");
                    if (!wireContent.has_value()) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/content", "item content representation is invalid");
                    }
                    if (hasDelta != hasBase || (hasDiscard && !hasDelta)) {
                        fail(OccurrenceErrorCode::InvalidPayload,
                             "/data/contentDelta",
                             "item content append representation is incomplete");
                    }
                    if (hasDelta && itemContentMode == ItemContentWireMode::Replacement) {
                        fail(OccurrenceErrorCode::InvalidPayload,
                             "/data/contentDelta",
                             "item content append representation requires negotiated append-v1");
                    }
                    if (hasDiscard && (itemContentMode != ItemContentWireMode::AppendV2 ||
                                       update.channel != std::optional<std::string>{"commandOutput"})) {
                        fail(OccurrenceErrorCode::InvalidPayload,
                             "/data/discardPrefixBytes",
                             "rolling item content requires negotiated append-v2 command output");
                    }
                    if (hasAgentOverflow && itemContentMode == ItemContentWireMode::Replacement) {
                        fail(OccurrenceErrorCode::InvalidPayload,
                             "/data/" + std::string(ItemContentOverflowV1Property),
                             "item content overflow requires negotiated append-v1");
                    }
                    if (hasCommandOverflow && itemContentMode != ItemContentWireMode::AppendV2) {
                        fail(OccurrenceErrorCode::InvalidPayload,
                             "/data/" + std::string(CommandOutputOverflowV2Property),
                             "command-output overflow requires negotiated append-v2");
                    }
                    if (hasOverflow && hasDelta) {
                        fail(OccurrenceErrorCode::InvalidPayload,
                             "/data/" + std::string(ItemContentOverflowV1Property),
                             "item content overflow cannot accompany an append representation");
                    }
                    if (!hasDelta) {
                        update.content = *wireContent;
                    } else {
                        if (!wireContent->empty()) {
                            fail(OccurrenceErrorCode::InvalidPayload,
                                 "/data/content",
                                 "item content append placeholder must be empty");
                        }
                        const auto delta = optionalString(data, "contentDelta");
                        const auto base = optionalUnsigned(data, "baseContentBytes");
                        const auto discard = optionalUnsigned(data, "discardPrefixBytes");
                        if (!delta.has_value() || !base.has_value() || (hasDiscard && !discard.has_value()) ||
                            discard.value_or(0) > *base) {
                            fail(OccurrenceErrorCode::InvalidPayload, "/data/contentDelta", "item content append representation is invalid");
                        }
                        update.appendHint = ItemContentAppendHint{*base, *delta, discard.value_or(0)};
                        update.appendWireRepresentation = true;
                    }
                    const auto truncated = data.find("contentTruncated");
                    update.contentTruncatedKnown = truncated != data.end();
                    if (truncated != data.end() && !truncated->is_boolean()) {
                        fail(OccurrenceErrorCode::InvalidPayload,
                             "/data/contentTruncated",
                             "item content truncation marker is invalid");
                    }
                    update.truncation.truncated = truncated != data.end() && truncated->get<bool>();
                    update.droppedContentBytesKnown = data.contains("droppedContentBytes");
                    const auto dropped = optionalUnsigned(data, "droppedContentBytes");
                    if (update.droppedContentBytesKnown && !dropped.has_value()) {
                        fail(OccurrenceErrorCode::InvalidPayload,
                             "/data/droppedContentBytes",
                             "item content dropped-byte count is invalid");
                    }
                    update.truncation.droppedBytes = dropped.value_or(0);
                    if (hasOverflow) {
                        const bool expectedChannel = hasAgentOverflow ? update.channel == std::optional<std::string>{"agentText"}
                                                                      : update.channel == std::optional<std::string>{"commandOutput"};
                        if (!expectedChannel || !update.content.has_value() ||
                            !update.contentTruncatedKnown || !update.truncation.truncated || !update.droppedContentBytesKnown) {
                            fail(OccurrenceErrorCode::InvalidPayload,
                                 "/data/" + std::string(ItemContentOverflowV1Property),
                                 "item content overflow metadata is incomplete");
                        }
                        const std::string overflowPath = "/data/" +
                                                         std::string(hasAgentOverflow ? ItemContentOverflowV1Property
                                                                                     : CommandOutputOverflowV2Property);
                        const auto overflow = hasAgentOverflow ? decodeItemContentOverflowV1(*overflowMember, overflowPath)
                                                               : decodeCommandOutputOverflowV2(*commandOverflowMember, overflowPath);
                        if (!overflow) {
                            fail(OccurrenceErrorCode::InvalidPayload, overflow.error().path, overflow.error().message);
                        }
                        const ItemContentOverflowV1& value = overflow.value();
                        const std::uint64_t prefixBytes = static_cast<std::uint64_t>(update.content->size());
                        const std::uint64_t suffixBytes = static_cast<std::uint64_t>(value.suffix.size());
                        if (value.baseContentBytes != prefixBytes ||
                            value.droppedContentBytesBeforeProjection >
                                std::numeric_limits<std::uint64_t>::max() - suffixBytes ||
                            update.truncation.droppedBytes != value.droppedContentBytesBeforeProjection + suffixBytes) {
                            fail(OccurrenceErrorCode::InvalidPayload,
                                 "/data/" + std::string(ItemContentOverflowV1Property),
                                 "item content overflow does not match the retained prefix metadata");
                        }
                        std::string restored = *update.content;
                        restored.append(value.suffix);
                        const std::size_t maximumBytes =
                            hasAgentOverflow ? MaximumItemContentOverflowV1Bytes : MaximumCommandOutputOverflowV2Bytes;
                        if (restored.size() > maximumBytes ||
                            utf8CharacterPrefixLength(restored, restored.size()) != restored.size()) {
                            fail(OccurrenceErrorCode::InvalidPayload,
                                 "/data/" + std::string(ItemContentOverflowV1Property),
                                 "item content overflow exceeds the retained channel bound");
                        }
                        update.content = std::move(restored);
                        update.truncation.droppedBytes = value.droppedContentBytesBeforeProjection;
                        update.truncation.truncated = value.contentTruncatedBeforeProjection;
                        update.overflowWireRepresentation = true;
                    }
                    update.extendedCommandOutputWireRepresentation =
                        itemContentMode == ItemContentWireMode::AppendV2 &&
                        update.channel == std::optional<std::string>{"commandOutput"} &&
                        (update.appendWireRepresentation || update.overflowWireRepresentation);
                    return update;
                }
                case ExpandedEventType::PendingRequestsUpdated:
                    state["pendingRequests"] = member(data, "pendingRequests", "/data");
                    return PendingRequestsUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).pendingRequests};
                case ExpandedEventType::AccountUpdated:
                    state["accounts"] = member(data, "domain", "/data");
                    return AccountUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).accounts};
                case ExpandedEventType::ModelsUpdated:
                    state["models"] = member(data, "domain", "/data");
                    return ModelsUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).models};
                case ExpandedEventType::ConfigurationUpdated:
                    state["configuration"] = member(data, "domain", "/data");
                    return ConfigurationUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).configuration};
                case ExpandedEventType::ProcessUpdated:
                    state["processes"] = Json{{"entries", Json::array({member(data, "process", "/data")})}};
                    return ProcessUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).processes.at(0)};
                case ExpandedEventType::FilesystemWatchUpdated:
                    state["filesystemWatches"] = Json{{"entries", Json::array({member(data, "filesystemWatch", "/data")})}};
                    return FilesystemWatchUpdatedOccurrence{
                        decodedSnapshotState(std::move(state), sequence).filesystemWatches.entries.at(0)};
                case ExpandedEventType::FuzzySearchUpdated:
                    state["fuzzySearches"] = Json{{"entries", Json::array({member(data, "fuzzySearch", "/data")})}};
                    return FuzzySearchUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).fuzzySearches.entries.at(0)};
                case ExpandedEventType::ReviewsUpdated:
                    state["reviews"] = member(data, "domain", "/data");
                    return ReviewsUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).reviews};
                case ExpandedEventType::IntegrationsUpdated:
                    state["apps"] = member(data, "domain", "/data");
                    return IntegrationsUpdatedOccurrence{IntegrationsState{decodedSnapshotState(std::move(state), sequence).apps.state}};
                case ExpandedEventType::PluginsUpdated:
                    state["plugins"] = member(data, "domain", "/data");
                    return PluginsUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).plugins};
                case ExpandedEventType::SkillsUpdated:
                    state["skills"] = member(data, "domain", "/data");
                    return SkillsUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).skills};
                case ExpandedEventType::McpUpdated:
                    state["mcp"] = member(data, "domain", "/data");
                    return McpUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).mcp};
                case ExpandedEventType::PlatformUpdated:
                    state["windowsSandbox"] = member(data, "domain", "/data");
                    return PlatformUpdatedOccurrence{PlatformState{decodedSnapshotState(std::move(state), sequence).windowsSandbox.state}};
                case ExpandedEventType::NoticeAdded:
                    state["notices"] = Json{{"entries", Json::array({member(data, "notice", "/data")})}};
                    return NoticeAddedOccurrence{decodedSnapshotState(std::move(state), sequence).notices.entries.at(0)};
                case ExpandedEventType::ActivityUpdated:
                    state["activities"] = Json{{"entries", Json::array({member(data, "activity", "/data")})}};
                    return ActivityUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).activities.entries.at(0)};
                case ExpandedEventType::CapacityUpdated:
                    state["capacity"] = member(data, "capacity", "/data");
                    return CapacityUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).capacity};
                case ExpandedEventType::DiagnosticsUpdated: {
                    const Json& encoded = member(data, "diagnostic", "/data");
                    if (!encoded.is_object()) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/diagnostic", "diagnostic must be an object");
                    }
                    DiagnosticRecord diagnostic;
                    diagnostic.received = optionalUnsigned(encoded, "received");
                    diagnostic.detailsOmitted = encoded.value("detailsOmitted", false);
                    diagnostic.message = optionalString(encoded, "message");
                    if (const auto details = encoded.find("details"); details != encoded.end()) {
                        diagnostic.safeDetails = safeDetail(*details, "/data/diagnostic/details");
                    }
                    Json remaining = encoded;
                    for (std::string_view key : {"received", "detailsOmitted", "message", "details"}) {
                        remaining.erase(key);
                    }
                    diagnostic.extensions = safeDetail(std::move(remaining), "/data/diagnostic/extensions");
                    return DiagnosticsUpdatedOccurrence{std::move(diagnostic)};
                }
            }
            fail(OccurrenceErrorCode::UnsupportedFamily, "/data", "unsupported expanded occurrence family");
        }

        LegacyCompatibilityPayload defaultLegacy(const OccurrencePayload& payload) {
            LegacyCompatibilityPayload legacy;
            switch (occurrenceType(payload)) {
                case ExpandedEventType::ProviderUpdated:
                    legacy.kind = LegacyCompatibilityKind::ProviderChanged;
                    break;
                case ExpandedEventType::ControllerUpdated:
                    legacy.kind = LegacyCompatibilityKind::ControllerChanged;
                    break;
                case ExpandedEventType::SessionsUpdated:
                    legacy.kind = LegacyCompatibilityKind::SessionChanged;
                    break;
                case ExpandedEventType::ThreadListUpdated:
                    legacy.kind = LegacyCompatibilityKind::ThreadListUpdated;
                    break;
                case ExpandedEventType::ThreadUpserted:
                    legacy.kind = LegacyCompatibilityKind::ThreadUpdated;
                    break;
                case ExpandedEventType::ThreadRemoved:
                    legacy.kind = LegacyCompatibilityKind::DirectExpanded;
                    break;
                case ExpandedEventType::TurnUpserted:
                    legacy.kind = LegacyCompatibilityKind::TurnUpdated;
                    break;
                case ExpandedEventType::ItemUpserted:
                    legacy.kind = LegacyCompatibilityKind::ItemUpdated;
                    break;
                case ExpandedEventType::ItemContentUpdated:
                    legacy.kind = LegacyCompatibilityKind::ItemContentUpdated;
                    break;
                case ExpandedEventType::PendingRequestsUpdated:
                    legacy.kind = LegacyCompatibilityKind::PendingRequestAdded;
                    break;
                case ExpandedEventType::DiagnosticsUpdated:
                    legacy.kind = LegacyCompatibilityKind::DiagnosticsUpdated;
                    break;
                default:
                    legacy.kind = LegacyCompatibilityKind::DirectExpanded;
                    break;
            }
            return legacy;
        }

        bool expectedLegacyFamily(LegacyCompatibilityKind kind, ExpandedEventType type) noexcept {
            switch (kind) {
                case LegacyCompatibilityKind::ProviderChanged:
                    return type == ExpandedEventType::ProviderUpdated;
                case LegacyCompatibilityKind::ControllerChanged:
                    return type == ExpandedEventType::ControllerUpdated;
                case LegacyCompatibilityKind::SessionChanged:
                    return type == ExpandedEventType::SessionsUpdated;
                case LegacyCompatibilityKind::ThreadListUpdated:
                    return type == ExpandedEventType::ThreadListUpdated;
                case LegacyCompatibilityKind::ThreadUpdated:
                    return type == ExpandedEventType::ThreadUpserted;
                case LegacyCompatibilityKind::ThreadRemoved:
                    return type == ExpandedEventType::ThreadRemoved;
                case LegacyCompatibilityKind::TurnUpdated:
                    return type == ExpandedEventType::TurnUpserted;
                case LegacyCompatibilityKind::ItemUpdated:
                    return type == ExpandedEventType::ItemUpserted;
                case LegacyCompatibilityKind::ItemContentUpdated:
                    return type == ExpandedEventType::ItemContentUpdated;
                case LegacyCompatibilityKind::PendingRequestAdded:
                case LegacyCompatibilityKind::PendingRequestResolved:
                    return type == ExpandedEventType::PendingRequestsUpdated;
                case LegacyCompatibilityKind::DiagnosticsUpdated:
                    return type == ExpandedEventType::DiagnosticsUpdated;
                case LegacyCompatibilityKind::DirectExpanded:
                case LegacyCompatibilityKind::CodexExtension:
                case LegacyCompatibilityKind::LegacyItem:
                case LegacyCompatibilityKind::LegacyPendingRequest:
                    return true;
            }
            return false;
        }

        bool validateLegacyCompatibility(const LegacyCompatibilityPayload& legacy,
                                         std::span<const OccurrencePayload> expanded,
                                         OccurrenceError* error) noexcept {
            const auto reject = [error](std::string message) {
                if (error != nullptr) {
                    *error = {OccurrenceErrorCode::InvalidGroup, "/legacyCompatibility", std::move(message)};
                }
                return false;
            };
            if (expanded.empty()) {
                const bool extension = legacy.kind == LegacyCompatibilityKind::CodexExtension && legacy.sourcePayloadIndex == 0 &&
                                       legacy.safeExtension.has_value() && !legacy.safeExtension->method.empty();
                const bool item = legacy.kind == LegacyCompatibilityKind::LegacyItem && legacy.sourcePayloadIndex == 0 &&
                                  legacy.legacyItem.has_value();
                const bool pending = legacy.kind == LegacyCompatibilityKind::LegacyPendingRequest && legacy.sourcePayloadIndex == 0 &&
                                     legacy.legacyPendingRequest.has_value();
                return extension || item || pending
                           ? true
                           : reject("an empty expanded group requires one bounded legacy compatibility descriptor");
            }
            if (legacy.sourcePayloadIndex >= expanded.size() ||
                !expectedLegacyFamily(legacy.kind, occurrenceType(expanded[legacy.sourcePayloadIndex]))) {
                return reject("legacy compatibility does not match the typed group");
            }
            if (legacy.kind == LegacyCompatibilityKind::CodexExtension &&
                (!legacy.safeExtension.has_value() || legacy.safeExtension->method.empty())) {
                return reject("codex.extension compatibility requires a bounded method and payload");
            }
            if (legacy.kind != LegacyCompatibilityKind::CodexExtension && legacy.safeExtension.has_value()) {
                return reject("only codex.extension compatibility may own a safe extension payload");
            }
            if (legacy.kind != LegacyCompatibilityKind::LegacyItem && legacy.legacyItem.has_value()) {
                return reject("only legacy-item compatibility may own a future item");
            }
            if (legacy.kind != LegacyCompatibilityKind::LegacyPendingRequest && legacy.legacyPendingRequest.has_value()) {
                return reject("only legacy-pending compatibility may own a generic request");
            }
            return true;
        }

        OccurrenceIdentity identityFromContext(FrontendSequence sequence, const OccurrenceDecodeContext& context) {
            OccurrenceIdentity identity{sequence, context.groupId, context.groupIndex, context.groupCount, context.sourceStamp};
            identity.projectionStamp = context.projectionStamp;
            identity.sessionId = context.sessionId;
            identity.controllerId = context.controllerId;
            identity.threadId = context.threadId;
            identity.turnId = context.turnId;
            identity.itemId = context.itemId;
            identity.pendingRequestId = context.pendingRequestId;
            identity.processHandle = context.processHandle;
            return identity;
        }

        ModelError occurrenceModelError(const OccurrenceError& error) {
            const ModelErrorCode code =
                error.code == OccurrenceErrorCode::UnsafeDetail ? ModelErrorCode::UnsafeDetail : ModelErrorCode::InvalidShape;
            return {code, error.path, error.message};
        }

        ItemData& mutableItemData(ThreadItem& item) {
            return std::visit(
                [](auto& value) -> ItemData& {
                    return value.value;
                },
                item);
        }

        PendingRequestData& mutablePendingRequestData(PendingRequest& request) {
            return std::visit(
                [](auto& value) -> PendingRequestData& {
                    return value.value;
                },
                request);
        }

        void normalizeItemSourceOrder(CanonicalSnapshot& snapshot) {
            struct OrderedItem {
                std::size_t sourceIndex;
                ItemData* value;
                LegacyItemCompatibility* legacy;
            };

            std::vector<OrderedItem> ordered;
            ordered.reserve(snapshot.items.size() + snapshot.legacyItems.size());
            std::size_t fallbackIndex = 0;
            for (ThreadItem& item : snapshot.items) {
                ItemData& value = mutableItemData(item);
                ordered.push_back({value.sourceIndex.value_or(fallbackIndex++), &value, nullptr});
            }
            for (LegacyItemCompatibility& item : snapshot.legacyItems) {
                ordered.push_back({item.sourceIndex, &item.value, &item});
            }
            std::stable_sort(ordered.begin(), ordered.end(), [](const OrderedItem& left, const OrderedItem& right) {
                return left.sourceIndex < right.sourceIndex;
            });
            for (std::size_t index = 0; index < ordered.size(); ++index) {
                ordered[index].value->sourceIndex = index;
                if (ordered[index].legacy != nullptr) {
                    ordered[index].legacy->sourceIndex = index;
                }
            }
        }

        void normalizePendingRequestSourceOrder(CanonicalSnapshot& snapshot) {
            struct OrderedRequest {
                std::size_t sourceIndex;
                PendingRequestData* value;
                LegacyPendingRequestCompatibility* legacy;
            };

            std::vector<OrderedRequest> ordered;
            ordered.reserve(snapshot.pendingRequests.size() + snapshot.legacyPendingRequests.size());
            std::size_t fallbackIndex = 0;
            for (PendingRequest& request : snapshot.pendingRequests) {
                PendingRequestData& value = mutablePendingRequestData(request);
                ordered.push_back({value.sourceIndex.value_or(fallbackIndex++), &value, nullptr});
            }
            for (LegacyPendingRequestCompatibility& request : snapshot.legacyPendingRequests) {
                ordered.push_back({request.sourceIndex, &request.value, &request});
            }
            std::stable_sort(ordered.begin(), ordered.end(), [](const OrderedRequest& left, const OrderedRequest& right) {
                return left.sourceIndex < right.sourceIndex;
            });
            for (std::size_t index = 0; index < ordered.size(); ++index) {
                ordered[index].value->sourceIndex = index;
                if (ordered[index].legacy != nullptr) {
                    ordered[index].legacy->sourceIndex = index;
                }
            }
        }

        bool sameItemIdentity(const ItemData& left, const ItemData& right) noexcept {
            return left.id == right.id && left.threadId == right.threadId && left.turnId == right.turnId;
        }

        bool turnIdentityIsUniqueToThread(std::span<const TurnState> retained,
                                          std::span<const TurnState> replacements,
                                          std::span<const ThreadItem> retainedItems,
                                          std::span<const ThreadItem> replacementItems,
                                          std::span<const LegacyItemCompatibility> retainedLegacyItems,
                                          const ThreadIdentity& threadId,
                                          const TurnIdentity& turnId) noexcept {
            bool belongsToThread = false;
            const auto inspect = [&](std::span<const TurnState> turns) {
                for (const TurnState& turn : turns) {
                    if (turn.id != turnId) {
                        continue;
                    }
                    if (turn.threadId != threadId) {
                        return false;
                    }
                    belongsToThread = true;
                }
                return true;
            };
            const auto inspectItems = [&](std::span<const ThreadItem> items) {
                return std::none_of(items.begin(), items.end(), [&](const ThreadItem& item) {
                    const ItemData& data = itemData(item);
                    return data.turnId == std::optional<TurnIdentity>{turnId} && data.threadId.has_value() &&
                           data.threadId != std::optional<ThreadIdentity>{threadId};
                });
            };
            const bool legacyItemsAgree =
                std::none_of(retainedLegacyItems.begin(), retainedLegacyItems.end(), [&](const LegacyItemCompatibility& item) {
                    const ItemData& data = item.value;
                    return data.turnId == std::optional<TurnIdentity>{turnId} && data.threadId.has_value() &&
                           data.threadId != std::optional<ThreadIdentity>{threadId};
                });
            return inspect(retained) && inspect(replacements) && inspectItems(retainedItems) &&
                   inspectItems(replacementItems) && legacyItemsAgree && belongsToThread;
        }

        std::optional<std::size_t> itemSourceIndex(const CanonicalSnapshot& snapshot, const ItemData& identity) {
            std::optional<std::size_t> result;
            for (const ThreadItem& item : snapshot.items) {
                const ItemData& value = itemData(item);
                if (sameItemIdentity(value, identity) && value.sourceIndex.has_value()) {
                    if (!result.has_value() || *value.sourceIndex < *result) {
                        result = *value.sourceIndex;
                    }
                }
            }
            for (const LegacyItemCompatibility& item : snapshot.legacyItems) {
                if (sameItemIdentity(item.value, identity) && (!result.has_value() || item.sourceIndex < *result)) {
                    result = item.sourceIndex;
                }
            }
            return result;
        }

        std::optional<std::size_t> pendingRequestSourceIndex(const CanonicalSnapshot& snapshot, const PendingRequestIdentity& id) {
            std::optional<std::size_t> result;
            for (const PendingRequest& request : snapshot.pendingRequests) {
                const PendingRequestData& value = pendingRequestData(request);
                if (value.id == id && value.sourceIndex.has_value()) {
                    if (!result.has_value() || *value.sourceIndex < *result) {
                        result = *value.sourceIndex;
                    }
                }
            }
            for (const LegacyPendingRequestCompatibility& request : snapshot.legacyPendingRequests) {
                if (request.value.id == id && (!result.has_value() || request.sourceIndex < *result)) {
                    result = request.sourceIndex;
                }
            }
            return result;
        }

        void upsertItem(CanonicalSnapshot& snapshot, ThreadItem replacement) {
            normalizeItemSourceOrder(snapshot);
            ItemData& replacementData = mutableItemData(replacement);
            const std::size_t sourceIndex =
                itemSourceIndex(snapshot, replacementData).value_or(snapshot.items.size() + snapshot.legacyItems.size());
            std::erase_if(snapshot.items, [&](const ThreadItem& item) {
                return sameItemIdentity(itemData(item), replacementData);
            });
            std::erase_if(snapshot.legacyItems, [&](const LegacyItemCompatibility& item) {
                return sameItemIdentity(item.value, replacementData);
            });
            replacementData.sourceIndex = sourceIndex;
            snapshot.items.push_back(std::move(replacement));
            normalizeItemSourceOrder(snapshot);
        }

        void upsertLegacyItem(CanonicalSnapshot& snapshot, LegacyItemCompatibility replacement) {
            normalizeItemSourceOrder(snapshot);
            const std::size_t sourceIndex =
                itemSourceIndex(snapshot, replacement.value).value_or(snapshot.items.size() + snapshot.legacyItems.size());
            std::erase_if(snapshot.items, [&](const ThreadItem& item) {
                return sameItemIdentity(itemData(item), replacement.value);
            });
            std::erase_if(snapshot.legacyItems, [&](const LegacyItemCompatibility& item) {
                return sameItemIdentity(item.value, replacement.value);
            });
            replacement.sourceIndex = sourceIndex;
            replacement.value.sourceIndex = sourceIndex;
            snapshot.legacyItems.push_back(std::move(replacement));
            normalizeItemSourceOrder(snapshot);
        }

        void upsertPendingRequest(CanonicalSnapshot& snapshot, PendingRequest replacement) {
            normalizePendingRequestSourceOrder(snapshot);
            PendingRequestData& replacementData = mutablePendingRequestData(replacement);
            const PendingRequestIdentity id = replacementData.id;
            const std::size_t sourceIndex =
                pendingRequestSourceIndex(snapshot, id).value_or(snapshot.pendingRequests.size() + snapshot.legacyPendingRequests.size());
            std::erase_if(snapshot.pendingRequests, [&](const PendingRequest& request) {
                return pendingRequestData(request).id == id;
            });
            std::erase_if(snapshot.legacyPendingRequests, [&](const LegacyPendingRequestCompatibility& request) {
                return request.value.id == id;
            });
            replacementData.sourceIndex = sourceIndex;
            snapshot.pendingRequests.push_back(std::move(replacement));
            normalizePendingRequestSourceOrder(snapshot);
        }

        void upsertLegacyPendingRequest(CanonicalSnapshot& snapshot, LegacyPendingRequestCompatibility replacement) {
            normalizePendingRequestSourceOrder(snapshot);
            const PendingRequestIdentity id = replacement.value.id;
            const std::size_t sourceIndex =
                pendingRequestSourceIndex(snapshot, id).value_or(snapshot.pendingRequests.size() + snapshot.legacyPendingRequests.size());
            std::erase_if(snapshot.pendingRequests, [&](const PendingRequest& request) {
                return pendingRequestData(request).id == id;
            });
            std::erase_if(snapshot.legacyPendingRequests, [&](const LegacyPendingRequestCompatibility& request) {
                return request.value.id == id;
            });
            replacement.sourceIndex = sourceIndex;
            replacement.value.sourceIndex = sourceIndex;
            snapshot.legacyPendingRequests.push_back(std::move(replacement));
            normalizePendingRequestSourceOrder(snapshot);
        }

        void erasePendingRequest(CanonicalSnapshot& snapshot, const PendingRequestIdentity& id) {
            normalizePendingRequestSourceOrder(snapshot);
            std::erase_if(snapshot.pendingRequests, [&](const PendingRequest& request) {
                return pendingRequestData(request).id == id;
            });
            std::erase_if(snapshot.legacyPendingRequests, [&](const LegacyPendingRequestCompatibility& request) {
                return request.value.id == id;
            });
            normalizePendingRequestSourceOrder(snapshot);
        }

        template <typename Affected>
        void replaceOrderedItems(CanonicalSnapshot& snapshot, std::vector<ThreadItem> replacements, Affected&& affected) {
            normalizeItemSourceOrder(snapshot);
            std::size_t insertionIndex = snapshot.items.size() + snapshot.legacyItems.size();
            for (const ThreadItem& item : snapshot.items) {
                const ItemData& value = itemData(item);
                if (affected(value)) {
                    insertionIndex = std::min(insertionIndex, *value.sourceIndex);
                }
            }
            for (const LegacyItemCompatibility& item : snapshot.legacyItems) {
                if (affected(item.value)) {
                    insertionIndex = std::min(insertionIndex, item.sourceIndex);
                }
            }
            std::erase_if(snapshot.items, [&](const ThreadItem& item) {
                return affected(itemData(item));
            });
            std::erase_if(snapshot.legacyItems, [&](const LegacyItemCompatibility& item) {
                return affected(item.value);
            });
            normalizeItemSourceOrder(snapshot);
            insertionIndex = std::min(insertionIndex, snapshot.items.size() + snapshot.legacyItems.size());
            for (ThreadItem& item : snapshot.items) {
                ItemData& value = mutableItemData(item);
                if (*value.sourceIndex >= insertionIndex) {
                    *value.sourceIndex += replacements.size();
                }
            }
            for (LegacyItemCompatibility& item : snapshot.legacyItems) {
                if (item.sourceIndex >= insertionIndex) {
                    item.sourceIndex += replacements.size();
                    item.value.sourceIndex = item.sourceIndex;
                }
            }
            for (std::size_t index = 0; index < replacements.size(); ++index) {
                mutableItemData(replacements[index]).sourceIndex = insertionIndex + index;
                snapshot.items.push_back(std::move(replacements[index]));
            }
            normalizeItemSourceOrder(snapshot);
        }

        template <typename Value, typename Affected>
        void replaceOrderedSubset(std::vector<Value>& current, std::vector<Value> replacement, Affected&& affected) {
            std::vector<Value> rebuilt;
            rebuilt.reserve(current.size() + replacement.size());
            bool inserted = false;
            for (Value& value : current) {
                if (affected(value)) {
                    if (!inserted) {
                        for (Value& replacementValue : replacement) {
                            rebuilt.push_back(std::move(replacementValue));
                        }
                        inserted = true;
                    }
                } else {
                    rebuilt.push_back(std::move(value));
                }
            }
            if (!inserted) {
                for (Value& replacementValue : replacement) {
                    rebuilt.push_back(std::move(replacementValue));
                }
            }
            current = std::move(rebuilt);
        }
    } // namespace

    bool OccurrenceIdentity::valid() const noexcept {
        return groupCount != 0 && groupIndex < groupCount;
    }

    Json encodeLegacyExtensionTruncation(const LegacySafeExtension& extension) {
        const auto encodeField = [](const LegacySafeExtension::FieldTruncation& field) {
            Json encoded = field.extensions.json();
            if (field.originalBytes.has_value()) {
                encoded["originalBytes"] = *field.originalBytes;
            }
            if (field.retainedBytes.has_value()) {
                encoded["retainedBytes"] = *field.retainedBytes;
            }
            return encoded;
        };

        if (!extension.wireTruncation.empty()) {
            Json encoded = extension.wireTruncation.extensions.json();
            if (extension.wireTruncation.method.has_value()) {
                encoded["method"] = encodeField(*extension.wireTruncation.method);
            }
            if (extension.wireTruncation.params.has_value()) {
                encoded["params"] = encodeField(*extension.wireTruncation.params);
            }
            if (extension.wireTruncation.decodingError.has_value()) {
                encoded["decodingError"] = encodeField(*extension.wireTruncation.decodingError);
            }
            return encoded;
        }

        if (!extension.truncation.truncated && !extension.truncation.omittedEntries.has_value() && extension.truncation.droppedBytes == 0 &&
            extension.truncation.omittedPaths.empty()) {
            return Json::object();
        }

        // Internally detected bounded-detail loss has no provider-owned
        // per-field byte record.  The frozen legacy vocabulary represents
        // that conservatively as params truncation, never as a second generic
        // truncation schema.
        Json params = Json::object();
        if (extension.truncation.droppedBytes != 0) {
            const std::uint64_t retained = extension.params.serializedBytes();
            params["originalBytes"] = extension.truncation.droppedBytes > std::numeric_limits<std::uint64_t>::max() - retained
                                          ? std::numeric_limits<std::uint64_t>::max()
                                          : retained + extension.truncation.droppedBytes;
        }
        return Json{{"params", std::move(params)}};
    }

    ExpandedEventType occurrenceType(const OccurrencePayload& payload) noexcept {
        return static_cast<ExpandedEventType>(payload.index());
    }

    const SafeDetail& occurrenceExtensions(const OccurrencePayload& payload) noexcept {
        return std::visit(
            [](const auto& value) -> const SafeDetail& {
                return value.extensions;
            },
            payload);
    }

    OccurrenceDraft::OccurrenceDraft(SourceStamp occurrenceSourceStamp, OccurrencePayload occurrencePayload)
        : sourceStamp(std::move(occurrenceSourceStamp))
        , legacyCompatibility(defaultLegacy(occurrencePayload))
        , expandedPayloads{std::move(occurrencePayload)} {
    }

    CanonicalOccurrence::CanonicalOccurrence(OccurrenceIdentity identity,
                                             LegacyCompatibilityPayload legacy,
                                             std::vector<OccurrencePayload> expanded)
        : occurrenceIdentity(std::move(identity))
        , legacyPayload(std::move(legacy))
        , occurrencePayloads(std::move(expanded)) {
    }

    const OccurrenceIdentity& CanonicalOccurrence::identity() const noexcept {
        return occurrenceIdentity;
    }

    const LegacyCompatibilityPayload& CanonicalOccurrence::legacyCompatibility() const noexcept {
        return legacyPayload;
    }

    const std::vector<OccurrencePayload>& CanonicalOccurrence::expandedPayloads() const noexcept {
        return occurrencePayloads;
    }

    bool validateOccurrenceGroup(std::span<const OccurrencePayload> expanded, OccurrenceError* error) noexcept {
        const auto reject = [error](std::string message) {
            if (error != nullptr) {
                *error = {OccurrenceErrorCode::InvalidGroup, "/expanded", std::move(message)};
            }
            return false;
        };
        try {
            if (expanded.empty()) {
                return reject("occurrence group has no expanded family");
            }
            std::vector<ExpandedEventType> types;
            types.reserve(expanded.size());
            for (const OccurrencePayload& payload : expanded) {
                const ExpandedEventType type = occurrenceType(payload);
                if (std::find(types.begin(), types.end(), type) != types.end()) {
                    return reject("occurrence group repeats an expanded family");
                }
                types.push_back(type);
            }
            if (types.size() == 1) {
                return true;
            }
            for (const generated::ProjectionMetadata& metadata : generated::AllNotificationProjections) {
                if (metadata.expandedMappings.size() != types.size()) {
                    continue;
                }
                bool equal = true;
                for (std::size_t index = 0; index < types.size(); ++index) {
                    const auto expected = expandedEventTypeFromString(metadata.expandedMappings[index]);
                    if (!expected.has_value() || *expected != types[index]) {
                        equal = false;
                        break;
                    }
                }
                if (equal) {
                    return true;
                }
            }
            return reject("expanded family order is not a generated notification mapping");
        } catch (...) {
            return reject("occurrence group validation failed");
        }
    }

    bool validateOccurrenceGroup(std::span<const CanonicalOccurrence> members, OccurrenceError* error) noexcept {
        const auto reject = [error](std::string message) {
            if (error != nullptr) {
                *error = {OccurrenceErrorCode::InvalidGroup, "/members", std::move(message)};
            }
            return false;
        };
        try {
            if (members.empty()) {
                return reject("occurrence group has no members");
            }
            const OccurrenceIdentity& first = members.front().identity();
            if (members.size() == 1 && members.front().expandedPayloads().empty()) {
                OccurrenceError legacyError;
                return first.valid() && first.groupIndex == 0 && first.groupCount == 1 &&
                               validateLegacyCompatibility(members.front().legacyCompatibility(), {}, &legacyError)
                           ? true
                           : reject(legacyError.message.empty() ? "contained extension identity is invalid" : legacyError.message);
            }
            if (members.size() != first.groupCount) {
                return reject("occurrence group is incomplete");
            }
            std::vector<const CanonicalOccurrence*> ordered;
            ordered.reserve(members.size());
            for (const CanonicalOccurrence& memberValue : members) {
                const OccurrenceIdentity& identity = memberValue.identity();
                if (identity.sequence != first.sequence || identity.groupId != first.groupId || identity.groupCount != first.groupCount ||
                    identity.sourceStamp != first.sourceStamp || identity.projectionStamp != first.projectionStamp ||
                    identity.sessionId != first.sessionId || identity.controllerId != first.controllerId ||
                    identity.threadId != first.threadId || identity.turnId != first.turnId || identity.itemId != first.itemId ||
                    identity.pendingRequestId != first.pendingRequestId || identity.processHandle != first.processHandle ||
                    memberValue.expandedPayloads().size() != 1) {
                    return reject("occurrence members do not share one canonical identity");
                }
                OccurrenceError legacyError;
                if (!validateLegacyCompatibility(memberValue.legacyCompatibility(), memberValue.expandedPayloads(), &legacyError)) {
                    return reject(legacyError.message.empty() ? "occurrence member has invalid legacy compatibility" : legacyError.message);
                }
                ordered.push_back(&memberValue);
            }
            std::sort(ordered.begin(), ordered.end(), [](const CanonicalOccurrence* left, const CanonicalOccurrence* right) {
                return left->identity().groupIndex < right->identity().groupIndex;
            });
            std::vector<OccurrencePayload> expanded;
            expanded.reserve(ordered.size());
            for (std::size_t index = 0; index < ordered.size(); ++index) {
                if (ordered[index]->identity().groupIndex != index) {
                    return reject("occurrence group indices are duplicated or gapped");
                }
                expanded.push_back(ordered[index]->expandedPayloads().front());
            }
            return validateOccurrenceGroup(expanded, error);
        } catch (...) {
            return reject("occurrence member validation failed");
        }
    }

    bool validateOccurrenceDraft(const OccurrenceDraft& draft, OccurrenceError* error) noexcept {
        try {
            if (!draft.expandedPayloads.empty() && !validateOccurrenceGroup(draft.expandedPayloads, error)) {
                return false;
            }
            return validateLegacyCompatibility(draft.legacyCompatibility, draft.expandedPayloads, error);
        } catch (...) {
            if (error != nullptr) {
                *error = {OccurrenceErrorCode::InvalidPayload, "/draft", "occurrence draft validation failed"};
            }
            return false;
        }
    }

    OccurrenceResult<CanonicalOccurrence> makeOccurrence(OccurrenceIdentity identity, OccurrencePayload payload) noexcept {
        try {
            if (!identity.valid()) {
                return OccurrenceError{OccurrenceErrorCode::InvalidGroup, "/identity", "occurrence group identity is invalid"};
            }
            LegacyCompatibilityPayload legacy = defaultLegacy(payload);
            return CanonicalOccurrence(std::move(identity), std::move(legacy), std::vector<OccurrencePayload>{std::move(payload)});
        } catch (...) {
            return OccurrenceError{OccurrenceErrorCode::InvalidPayload, "/payload", "occurrence construction failed"};
        }
    }

    OccurrenceResult<CanonicalOccurrence>
    makeOccurrenceGroup(OccurrenceIdentity identity, LegacyCompatibilityPayload legacy, std::vector<OccurrencePayload> expanded) noexcept {
        try {
            OccurrenceError validation;
            const bool containedExtension = expanded.empty();
            const std::size_t expectedGroupCount = containedExtension ? 1 : expanded.size();
            if (!identity.valid() || identity.groupIndex != 0 || identity.groupCount != expectedGroupCount ||
                (!containedExtension && !validateOccurrenceGroup(expanded, &validation))) {
                return validation.message.empty()
                           ? OccurrenceError{OccurrenceErrorCode::InvalidGroup, "/identity", "occurrence group identity is invalid"}
                           : validation;
            }
            if (!validateLegacyCompatibility(legacy, expanded, &validation)) {
                return validation;
            }
            return CanonicalOccurrence(std::move(identity), std::move(legacy), std::move(expanded));
        } catch (...) {
            return OccurrenceError{OccurrenceErrorCode::InvalidPayload, "/payload", "occurrence group construction failed"};
        }
    }

    OccurrenceResult<CanonicalOccurrence> mergeOccurrenceGroup(std::span<const CanonicalOccurrence> members) noexcept {
        OccurrenceError validation;
        if (!validateOccurrenceGroup(members, &validation)) {
            return validation;
        }
        try {
            // A contained future-safe extension is already a complete
            // singleton occurrence and intentionally has no expanded payload
            // to merge.  Preserve that canonical value instead of entering
            // the expanded-family merge path below.
            if (members.size() == 1 && members.front().expandedPayloads().empty()) {
                return members.front();
            }
            std::vector<const CanonicalOccurrence*> ordered;
            ordered.reserve(members.size());
            for (const CanonicalOccurrence& memberValue : members) {
                ordered.push_back(&memberValue);
            }
            std::sort(ordered.begin(), ordered.end(), [](const CanonicalOccurrence* left, const CanonicalOccurrence* right) {
                return left->identity().groupIndex < right->identity().groupIndex;
            });
            OccurrenceIdentity identity = ordered.front()->identity();
            identity.groupIndex = 0;
            identity.groupCount = static_cast<std::uint32_t>(ordered.size());
            std::vector<OccurrencePayload> expanded;
            expanded.reserve(ordered.size());
            for (const CanonicalOccurrence* memberValue : ordered) {
                expanded.push_back(memberValue->expandedPayloads().front());
            }
            std::optional<LegacyCompatibilityPayload> explicitLegacy;
            for (std::size_t index = 0; index < ordered.size(); ++index) {
                const LegacyCompatibilityPayload& candidate = ordered[index]->legacyCompatibility();
                if (candidate == defaultLegacy(ordered[index]->expandedPayloads().front())) {
                    continue;
                }
                LegacyCompatibilityPayload adjusted = candidate;
                adjusted.sourcePayloadIndex = index;
                if (explicitLegacy.has_value() && *explicitLegacy != adjusted) {
                    return OccurrenceError{OccurrenceErrorCode::InvalidGroup,
                                           "/members/legacyCompatibility",
                                           "occurrence members contain conflicting explicit legacy descriptors"};
                }
                explicitLegacy = std::move(adjusted);
            }
            LegacyCompatibilityPayload legacy =
                explicitLegacy.has_value() ? std::move(*explicitLegacy) : ordered.front()->legacyCompatibility();
            if (!explicitLegacy.has_value()) {
                legacy.sourcePayloadIndex = 0;
            }
            return makeOccurrenceGroup(std::move(identity), std::move(legacy), std::move(expanded));
        } catch (...) {
            return OccurrenceError{OccurrenceErrorCode::InvalidGroup, "/members", "occurrence group merge failed"};
        }
    }

    OccurrenceResult<std::vector<ExpandedFrontendEvent>>
    encodeExpandedOccurrence(const CanonicalOccurrence& occurrence, ItemContentWireMode itemContentMode) noexcept {
        try {
            std::vector<ExpandedFrontendEvent> events;
            events.reserve(occurrence.expandedPayloads().size());
            for (std::size_t index = 0; index < occurrence.expandedPayloads().size(); ++index) {
                const OccurrencePayload& payload = occurrence.expandedPayloads()[index];
                requireExpandedSemantics(payload, index);
                ExpandedFrontendEvent event{occurrence.identity().sequence.protocolValue(),
                                            occurrenceType(payload),
                                            payloadData(payload, occurrence.identity().sequence, itemContentMode),
                                            occurrenceExtensions(payload).json()};
                const auto validated = Codec::encodeExpandedEvent(event);
                if (!validated) {
                    fail(OccurrenceErrorCode::EncodingFailure, "/event", validated.error().message);
                }
                events.push_back(std::move(event));
            }
            return events;
        } catch (const OccurrenceFailure& failure) {
            return failure.error;
        } catch (const std::exception& error) {
            return OccurrenceError{OccurrenceErrorCode::EncodingFailure, "/event", error.what()};
        } catch (...) {
            return OccurrenceError{OccurrenceErrorCode::EncodingFailure, "/event", "expanded occurrence encoding failed"};
        }
    }

    OccurrenceResult<FrontendEvent> encodeLegacyOccurrence(const CanonicalOccurrence& occurrence) noexcept {
        try {
            const LegacyCompatibilityPayload& legacy = occurrence.legacyCompatibility();
            std::string type;
            Json data = legacy.extensions.json();
            if (legacy.kind == LegacyCompatibilityKind::CodexExtension) {
                if (!legacy.safeExtension.has_value()) {
                    fail(OccurrenceErrorCode::InvalidGroup, "/legacyCompatibility/safeExtension", "safe extension is missing");
                }
                type = "codex.extension";
                const LegacySafeExtension& extension = *legacy.safeExtension;
                data = extension.extensions.json();
                data["method"] = extension.method;
                if (extension.paramsKnown) {
                    data["params"] = extension.params.json();
                }
                if (extension.decodingError.has_value()) {
                    data["decodingError"] = *extension.decodingError;
                }
                if (extension.sensitiveFieldsRedacted) {
                    data["sensitiveFieldsRedacted"] = true;
                }
                Json truncation = encodeLegacyExtensionTruncation(extension);
                if (!truncation.empty()) {
                    data["truncation"] = std::move(truncation);
                }
                FrontendEvent event{
                    occurrence.identity().sequence.protocolValue(), std::move(type), std::move(data), legacy.extensions.json()};
                const auto validated = Codec::encodeEvent(event);
                if (!validated) {
                    fail(OccurrenceErrorCode::EncodingFailure, "/legacy", validated.error().message);
                }
                return event;
            }
            if (legacy.kind == LegacyCompatibilityKind::LegacyItem && legacy.legacyItem.has_value()) {
                const LegacyItemCompatibility& item = *legacy.legacyItem;
                Json eventData{{"item", legacyItem(item)}};
                if (item.value.threadId.has_value()) {
                    eventData["threadId"] = item.value.threadId->value();
                }
                if (item.value.turnId.has_value()) {
                    eventData["turnId"] = item.value.turnId->value();
                }
                FrontendEvent event{occurrence.identity().sequence.protocolValue(), "item.updated", std::move(eventData), legacy.extensions.json()};
                const auto validated = Codec::encodeEvent(event);
                if (!validated) {
                    fail(OccurrenceErrorCode::EncodingFailure, "/legacy", validated.error().message);
                }
                return event;
            }
            if (legacy.kind == LegacyCompatibilityKind::LegacyPendingRequest && legacy.legacyPendingRequest.has_value()) {
                CanonicalSnapshot snapshot;
                snapshot.legacyPendingRequests.push_back(*legacy.legacyPendingRequest);
                const auto encoded = encodeLegacySnapshot(snapshot);
                if (!encoded || encoded.value().state.value("pendingRequests", Json::array()).size() != 1) {
                    fail(OccurrenceErrorCode::EncodingFailure, "/legacy/request", "legacy pending request encoding failed");
                }
                FrontendEvent event{occurrence.identity().sequence.protocolValue(),
                                    "request.pending",
                                    Json{{"request", encoded.value().state.at("pendingRequests").at(0)}},
                                    legacy.extensions.json()};
                const auto validated = Codec::encodeEvent(event);
                if (!validated) {
                    fail(OccurrenceErrorCode::EncodingFailure, "/legacy", validated.error().message);
                }
                return event;
            }
            if (legacy.sourcePayloadIndex >= occurrence.expandedPayloads().size()) {
                fail(OccurrenceErrorCode::InvalidGroup, "/legacyCompatibility/sourcePayloadIndex", "legacy source index is invalid");
            }
            const OccurrencePayload& source = occurrence.expandedPayloads()[legacy.sourcePayloadIndex];
            Json expanded = payloadData(source, occurrence.identity().sequence);
            switch (legacy.kind) {
                case LegacyCompatibilityKind::ProviderChanged:
                    type = "backend.lifecycle.changed";
                    data = Json{{"lifecycle", member(expanded, "provider", "/data").value("lifecycle", "stopped")}};
                    if (data["lifecycle"] == "recovering") {
                        data["lifecycle"] = "starting";
                    }
                    if (const ProviderState& provider = payloadAs<ProviderUpdatedOccurrence>(source).provider; provider.lastError) {
                        data["error"] = provider.lastError->json();
                    }
                    break;
                case LegacyCompatibilityKind::ControllerChanged:
                    type = "controller.changed";
                    data = Json::object();
                    if (const auto& session = payloadAs<ControllerUpdatedOccurrence>(source).controller.session; session) {
                        data["controllerSessionId"] = session->value();
                    }
                    break;
                case LegacyCompatibilityKind::SessionChanged: {
                    type = "session.changed";
                    const auto& sessions = payloadAs<SessionsUpdatedOccurrence>(source);
                    if (legacy.changedSessionId && occurrence.identity().sessionId &&
                        *legacy.changedSessionId != *occurrence.identity().sessionId) {
                        fail(OccurrenceErrorCode::EncodingFailure,
                             "/legacy/sessionId",
                             "legacy session identity contradicts the canonical occurrence identity");
                    }
                    if (sessions.changedSession && occurrence.identity().sessionId &&
                        sessions.changedSession->id != *occurrence.identity().sessionId) {
                        fail(OccurrenceErrorCode::EncodingFailure,
                             "/payload/changedSession/id",
                             "changed session contradicts the canonical occurrence identity");
                    }
                    if (legacy.changedSessionId && sessions.changedSession && *legacy.changedSessionId != sessions.changedSession->id) {
                        fail(OccurrenceErrorCode::EncodingFailure,
                             "/payload/changedSession/id",
                             "changed session contradicts the legacy compatibility identity");
                    }
                    const SessionState* selected = nullptr;
                    const std::optional<SessionIdentity> selectedId =
                        legacy.changedSessionId.has_value()
                            ? legacy.changedSessionId
                            : (sessions.changedSession.has_value() ? std::optional<SessionIdentity>{sessions.changedSession->id}
                                                                   : occurrence.identity().sessionId);
                    if (sessions.changedSession.has_value() && (!selectedId.has_value() || sessions.changedSession->id == *selectedId)) {
                        selected = &*sessions.changedSession;
                    }
                    if (selected == nullptr && selectedId.has_value()) {
                        const auto found = std::find_if(sessions.sessions.begin(), sessions.sessions.end(), [&](const SessionState& value) {
                            return value.id == *selectedId;
                        });
                        if (found != sessions.sessions.end()) {
                            selected = &*found;
                        }
                    }
                    if (selected == nullptr && sessions.sessions.size() == 1) {
                        selected = &sessions.sessions.front();
                    }
                    if (selected == nullptr && !selectedId.has_value()) {
                        fail(OccurrenceErrorCode::EncodingFailure, "/legacy/session", "session compatibility identity is ambiguous");
                    }
                    data = Json{{"sessionId", selected != nullptr ? selected->id.value() : selectedId->value()},
                                {"connected", sessions.connected.value_or(legacy.connected)},
                                {"role", selected != nullptr ? std::string(toString(selected->role)) : std::string("observer")}};
                    break;
                }
                case LegacyCompatibilityKind::ThreadListUpdated:
                    type = "thread.list.updated";
                    data = member(expanded, "threadList", "/data");
                    break;
                case LegacyCompatibilityKind::ThreadUpdated:
                    type = "thread.updated";
                    data = Json{{"thread", legacyThread(payloadAs<ThreadUpsertedOccurrence>(source))}};
                    break;
                case LegacyCompatibilityKind::ThreadRemoved:
                    type = "thread.removed";
                    data = expanded;
                    break;
                case LegacyCompatibilityKind::TurnUpdated:
                    type = "turn.updated";
                    data = Json{{"turn", legacyTurn(payloadAs<TurnUpsertedOccurrence>(source))}};
                    break;
                case LegacyCompatibilityKind::ItemUpdated: {
                    type = "item.updated";
                    const ItemData& item = itemData(payloadAs<ItemUpsertedOccurrence>(source).item);
                    data = Json{{"item", legacyItem(payloadAs<ItemUpsertedOccurrence>(source).item)}};
                    if (item.threadId.has_value()) {
                        data["threadId"] = item.threadId->value();
                    }
                    if (item.turnId.has_value()) {
                        data["turnId"] = item.turnId->value();
                    }
                    break;
                }
                case LegacyCompatibilityKind::ItemContentUpdated:
                    type = "item.content.updated";
                    data = expanded;
                    if (!data.contains("contentTruncated")) {
                        data["contentTruncated"] = false;
                    }
                    if (!data.contains("droppedContentBytes")) {
                        data["droppedContentBytes"] = std::uint64_t{0};
                    }
                    break;
                case LegacyCompatibilityKind::PendingRequestAdded: {
                    type = "request.pending";
                    const auto& pending = payloadAs<PendingRequestsUpdatedOccurrence>(source);
                    const PendingRequest* request = nullptr;
                    if (occurrence.identity().pendingRequestId.has_value()) {
                        const auto selected =
                            std::find_if(pending.pendingRequests.begin(), pending.pendingRequests.end(), [&](const PendingRequest& value) {
                                return pendingRequestData(value).id == *occurrence.identity().pendingRequestId;
                            });
                        if (selected == pending.pendingRequests.end()) {
                            fail(OccurrenceErrorCode::EncodingFailure,
                                 "/legacy/request",
                                 "pending-request compatibility identity is absent from the typed projection");
                        }
                        if (std::find_if(std::next(selected), pending.pendingRequests.end(), [&](const PendingRequest& value) {
                                return pendingRequestData(value).id == *occurrence.identity().pendingRequestId;
                            }) != pending.pendingRequests.end()) {
                            fail(OccurrenceErrorCode::EncodingFailure,
                                 "/legacy/request",
                                 "pending-request compatibility identity is duplicated in the typed projection");
                        }
                        request = &*selected;
                    } else if (pending.pendingRequests.size() == 1) {
                        request = &pending.pendingRequests.front();
                    } else {
                        fail(
                            OccurrenceErrorCode::EncodingFailure, "/legacy/request", "pending-request compatibility identity is ambiguous");
                    }
                    CanonicalSnapshot requestSnapshot;
                    requestSnapshot.pendingRequests.push_back(*request);
                    const auto encodedSnapshot = encodeLegacySnapshot(requestSnapshot);
                    if (!encodedSnapshot || !encodedSnapshot.value().state.contains("pendingRequests") ||
                        !encodedSnapshot.value().state.at("pendingRequests").is_array() ||
                        encodedSnapshot.value().state.at("pendingRequests").size() != 1) {
                        fail(OccurrenceErrorCode::EncodingFailure,
                             "/legacy/request",
                             "typed pending request could not be encoded through the legacy snapshot authority");
                    }
                    data = Json{{"request", encodedSnapshot.value().state.at("pendingRequests").at(0)}};
                    break;
                }
                case LegacyCompatibilityKind::PendingRequestResolved: {
                    type = "request.resolved";
                    const auto& pending = payloadAs<PendingRequestsUpdatedOccurrence>(source);
                    if (legacy.resolvedRequestId && pending.removedRequestId && *legacy.resolvedRequestId != *pending.removedRequestId) {
                        fail(OccurrenceErrorCode::EncodingFailure,
                             "/legacy/pendingRequestId",
                             "legacy resolution identity contradicts the typed payload identity");
                    }
                    if (legacy.resolvedRequestId && occurrence.identity().pendingRequestId &&
                        *legacy.resolvedRequestId != *occurrence.identity().pendingRequestId) {
                        fail(OccurrenceErrorCode::EncodingFailure,
                             "/legacy/pendingRequestId",
                             "legacy resolution identity contradicts the canonical occurrence identity");
                    }
                    if (pending.removedRequestId && occurrence.identity().pendingRequestId &&
                        *pending.removedRequestId != *occurrence.identity().pendingRequestId) {
                        fail(OccurrenceErrorCode::EncodingFailure,
                             "/payload/removedRequestId",
                             "typed resolution identity contradicts the canonical occurrence identity");
                    }
                    const std::optional<PendingRequestIdentity> id =
                        legacy.resolvedRequestId.has_value()
                            ? legacy.resolvedRequestId
                            : (pending.removedRequestId.has_value() ? pending.removedRequestId : occurrence.identity().pendingRequestId);
                    if (!id.has_value()) {
                        fail(OccurrenceErrorCode::EncodingFailure, "/legacy/pendingRequestId", "resolved request identity is missing");
                    }
                    data = Json{{"pendingRequestId", id->value()},
                                {"reason", legacy.resolutionReason.value_or(pending.resolutionReason.value_or("resolved"))}};
                    break;
                }
                case LegacyCompatibilityKind::DiagnosticsUpdated: {
                    type = "diagnostics.updated";
                    const DiagnosticsUpdatedOccurrence& update = payloadAs<DiagnosticsUpdatedOccurrence>(source);
                    const DiagnosticRecord& diagnostic = update.diagnostic;
                    data = Json{{"received", diagnostic.received.value_or(0)}, {"recent", Json::array()}};
                    if (update.aggregateLegacyUpdate || !update.aggregateEntries.empty()) {
                        for (const DiagnosticRecord& entry : update.aggregateEntries) {
                            if (entry.message.has_value()) {
                                data["recent"].push_back(*entry.message);
                            }
                        }
                    } else if (diagnostic.message.has_value()) {
                        data["recent"].push_back(*diagnostic.message);
                    }
                    break;
                }
                case LegacyCompatibilityKind::DirectExpanded:
                    type = std::string(toString(occurrenceType(source)));
                    data = std::move(expanded);
                    break;
                case LegacyCompatibilityKind::CodexExtension:
                    fail(OccurrenceErrorCode::InvalidGroup, "/legacyCompatibility", "unreachable codex.extension branch");
                case LegacyCompatibilityKind::LegacyItem:
                case LegacyCompatibilityKind::LegacyPendingRequest:
                    fail(OccurrenceErrorCode::InvalidGroup, "/legacyCompatibility", "unreachable legacy-only branch");
            }
            FrontendEvent event{occurrence.identity().sequence.protocolValue(), std::move(type), std::move(data), legacy.extensions.json()};
            const auto validated = Codec::encodeEvent(event);
            if (!validated) {
                fail(OccurrenceErrorCode::EncodingFailure, "/legacy", validated.error().message);
            }
            return event;
        } catch (const OccurrenceFailure& failure) {
            return failure.error;
        } catch (const std::exception& error) {
            return OccurrenceError{OccurrenceErrorCode::EncodingFailure, "/legacy", error.what()};
        } catch (...) {
            return OccurrenceError{OccurrenceErrorCode::EncodingFailure, "/legacy", "legacy occurrence encoding failed"};
        }
    }

    OccurrenceResult<CanonicalOccurrence> decodeExpandedOccurrence(const ExpandedFrontendEvent& event,
                                                                   const OccurrenceDecodeContext& context,
                                                                   ItemContentWireMode itemContentMode) noexcept {
        try {
            const auto validated = Codec::encodeExpandedEvent(event);
            if (!validated) {
                fail(OccurrenceErrorCode::InvalidPayload, "/event", validated.error().message);
            }
            OccurrencePayload payload = decodePayload(event.type, event.data, FrontendSequence(event.sequence), itemContentMode);
            setWireExtensions(payload, safeDetail(event.extensions, "/extensions"));
            return makeOccurrence(identityFromContext(FrontendSequence(event.sequence), context), std::move(payload));
        } catch (const OccurrenceFailure& failure) {
            return failure.error;
        } catch (const std::exception& error) {
            return OccurrenceError{OccurrenceErrorCode::InvalidPayload, "/event", error.what()};
        } catch (...) {
            return OccurrenceError{OccurrenceErrorCode::InvalidPayload, "/event", "expanded occurrence decoding failed"};
        }
    }

    OccurrenceResult<CanonicalOccurrence> decodeLegacyOccurrence(const FrontendEvent& event,
                                                                 const OccurrenceDecodeContext& context,
                                                                 std::optional<ExpandedEventType> familyHint) noexcept {
        try {
            const auto validated = Codec::encodeEvent(event);
            if (!validated) {
                fail(OccurrenceErrorCode::InvalidPayload, "/event", validated.error().message);
            }
            const FrontendSequence sequence(event.sequence);
            if (event.type == "item.updated") {
                const Json& item = member(event.data, "item", "/data");
                const auto threadId = optionalString(event.data, "threadId");
                const auto turnId = optionalString(event.data, "turnId");
                Json state;
                if (threadId.has_value() && turnId.has_value()) {
                    Json turn{{"id", *turnId},
                              {"threadId", *threadId},
                              {"status", "unknown"},
                              {"active", false},
                              {"terminal", false},
                              {"items", Json::array({item})},
                              {"extensions", Json::object()}};
                    Json thread{{"id", *threadId},
                                {"fullyLoaded", false},
                                {"turns", Json::array({std::move(turn)})},
                                {"extensions", Json::object()}};
                    state = Json{{"threads", Json::array({std::move(thread)})}};
                } else {
                    state = Json{{"items", Json::array({item})}};
                }
                CanonicalSnapshot decoded = decodedLegacySnapshotState(std::move(state), sequence);
                if (decoded.legacyItems.size() == 1) {
                    LegacyCompatibilityPayload legacy;
                    legacy.kind = LegacyCompatibilityKind::LegacyItem;
                    legacy.legacyItem = std::move(decoded.legacyItems.front());
                    legacy.extensions = safeDetail(event.extensions, "/extensions");
                    OccurrenceIdentity identity = identityFromContext(sequence, context);
                    identity.itemId = legacy.legacyItem->value.id;
                    identity.threadId = legacy.legacyItem->value.threadId;
                    identity.turnId = legacy.legacyItem->value.turnId;
                    return makeOccurrenceGroup(std::move(identity), std::move(legacy), {});
                }
            }
            if (event.type == "request.pending") {
                const Json& request = member(event.data, "request", "/data");
                CanonicalSnapshot decoded =
                    decodedLegacySnapshotState(Json{{"pendingRequests", Json::array({request})}}, sequence);
                if (decoded.legacyPendingRequests.size() == 1) {
                    LegacyCompatibilityPayload legacy;
                    legacy.kind = LegacyCompatibilityKind::LegacyPendingRequest;
                    legacy.legacyPendingRequest = std::move(decoded.legacyPendingRequests.front());
                    legacy.extensions = safeDetail(event.extensions, "/extensions");
                    OccurrenceIdentity identity = identityFromContext(sequence, context);
                    identity.pendingRequestId = legacy.legacyPendingRequest->value.id;
                    return makeOccurrenceGroup(std::move(identity), std::move(legacy), {});
                }
            }
            if (event.type == "codex.extension") {
                const auto method = optionalString(event.data, "method");
                if (!method.has_value() || method->empty()) {
                    fail(OccurrenceErrorCode::InvalidPayload, "/data/method", "contained extension method is missing");
                }
                LegacySafeExtension extension;
                extension.method = *method;
                if (const auto params = event.data.find("params"); params != event.data.end()) {
                    extension.params = safeDetail(*params, "/data/params");
                } else {
                    extension.paramsKnown = false;
                }
                extension.decodingError = optionalString(event.data, "decodingError");
                if (const auto redacted = event.data.find("sensitiveFieldsRedacted");
                    redacted != event.data.end() && redacted->is_boolean()) {
                    extension.sensitiveFieldsRedacted = redacted->get<bool>();
                }
                Json remaining = event.data;
                remaining.erase("method");
                remaining.erase("params");
                remaining.erase("decodingError");
                remaining.erase("sensitiveFieldsRedacted");
                if (const auto truncation = remaining.find("truncation"); truncation != remaining.end()) {
                    if (!truncation->is_object()) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/truncation", "extension truncation must be an object");
                    }
                    Json truncationRemaining = *truncation;
                    const auto decodeField = [&](std::string_view name) -> std::optional<LegacySafeExtension::FieldTruncation> {
                        const auto field = truncation->find(name);
                        if (field == truncation->end()) {
                            return std::nullopt;
                        }
                        if (!field->is_object()) {
                            fail(OccurrenceErrorCode::InvalidPayload,
                                 "/data/truncation/" + std::string(name),
                                 "extension field truncation must be an object");
                        }
                        LegacySafeExtension::FieldTruncation decoded;
                        decoded.originalBytes = optionalUnsigned(*field, "originalBytes");
                        decoded.retainedBytes = optionalUnsigned(*field, "retainedBytes");
                        if ((field->contains("originalBytes") && !decoded.originalBytes.has_value()) ||
                            (field->contains("retainedBytes") && !decoded.retainedBytes.has_value())) {
                            fail(OccurrenceErrorCode::InvalidPayload,
                                 "/data/truncation/" + std::string(name),
                                 "extension field byte count must be unsigned");
                        }
                        Json fieldRemaining = *field;
                        fieldRemaining.erase("originalBytes");
                        fieldRemaining.erase("retainedBytes");
                        decoded.extensions = safeDetail(std::move(fieldRemaining), "/data/truncation/" + std::string(name) + "/extensions");
                        truncationRemaining.erase(name);
                        return decoded;
                    };
                    extension.wireTruncation.method = decodeField("method");
                    extension.wireTruncation.params = decodeField("params");
                    extension.wireTruncation.decodingError = decodeField("decodingError");
                    extension.truncation.truncated = truncation->value("truncated", false);
                    extension.truncation.droppedBytes = optionalUnsigned(*truncation, "droppedBytes").value_or(0);
                    if (const auto omitted = optionalUnsigned(*truncation, "omittedEntries"); omitted.has_value()) {
                        if (*omitted > std::numeric_limits<std::size_t>::max()) {
                            fail(OccurrenceErrorCode::InvalidPayload,
                                 "/data/truncation/omittedEntries",
                                 "omitted entry count exceeds the platform limit");
                        }
                        extension.truncation.omittedEntries = static_cast<std::size_t>(*omitted);
                    }
                    const auto paths = truncation->find("omittedPaths");
                    if (paths != truncation->end()) {
                        if (!paths->is_array()) {
                            fail(OccurrenceErrorCode::InvalidPayload, "/data/truncation/omittedPaths", "omitted paths must be an array");
                        }
                        for (const Json& path : *paths) {
                            if (!path.is_string()) {
                                fail(OccurrenceErrorCode::InvalidPayload, "/data/truncation/omittedPaths", "omitted path must be a string");
                            }
                            extension.truncation.omittedPaths.push_back(path.get<std::string>());
                        }
                    }
                    for (std::string_view key : {"truncated", "droppedBytes", "omittedEntries", "omittedPaths"}) {
                        truncationRemaining.erase(key);
                    }
                    extension.wireTruncation.extensions = safeDetail(std::move(truncationRemaining), "/data/truncation/extensions");

                    const auto addDropped = [&](std::uint64_t original, std::uint64_t retained) {
                        if (original <= retained) {
                            return;
                        }
                        const std::uint64_t amount = original - retained;
                        extension.truncation.droppedBytes =
                            amount > std::numeric_limits<std::uint64_t>::max() - extension.truncation.droppedBytes
                                ? std::numeric_limits<std::uint64_t>::max()
                                : extension.truncation.droppedBytes + amount;
                    };
                    const auto accountField = [&](const std::optional<LegacySafeExtension::FieldTruncation>& field,
                                                  std::uint64_t retainedFallback) {
                        if (!field.has_value()) {
                            return;
                        }
                        extension.truncation.truncated = true;
                        if (field->originalBytes.has_value()) {
                            addDropped(*field->originalBytes, field->retainedBytes.value_or(retainedFallback));
                        }
                    };
                    accountField(extension.wireTruncation.method, extension.method.size());
                    accountField(extension.wireTruncation.params, extension.params.serializedBytes());
                    accountField(extension.wireTruncation.decodingError, extension.decodingError.value_or("").size());
                    remaining.erase(truncation);
                }
                extension.extensions = safeDetail(std::move(remaining), "/data/extensions");
                LegacyCompatibilityPayload legacy;
                legacy.kind = LegacyCompatibilityKind::CodexExtension;
                legacy.safeExtension = std::move(extension);
                legacy.extensions = safeDetail(event.extensions, "/extensions");
                OccurrenceIdentity identity = identityFromContext(sequence, context);
                identity.groupIndex = 0;
                identity.groupCount = 1;
                return makeOccurrenceGroup(std::move(identity), std::move(legacy), {});
            }
            OccurrencePayload payload = [&]() -> OccurrencePayload {
                if (event.type == "backend.lifecycle.changed") {
                    ProviderState provider;
                    const auto lifecycle = optionalString(event.data, "lifecycle");
                    const auto decoded = lifecycle.has_value() ? providerLifecycleFromString(*lifecycle) : std::nullopt;
                    if (!decoded.has_value()) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/lifecycle", "provider lifecycle is invalid");
                    }
                    provider.lifecycle = *decoded;
                    provider.generation = optionalUnsigned(event.data, "generation").value_or(0);
                    provider.desiredRunning = event.data.value("desiredRunning", provider.lifecycle != ProviderLifecycle::Stopped);
                    if (const auto error = event.data.find("error"); error != event.data.end()) {
                        provider.lastError = safeDetail(*error, "/data/error");
                    }
                    return ProviderUpdatedOccurrence{std::move(provider)};
                }
                if (event.type == "controller.changed") {
                    Json state = baseSnapshotState(sequence);
                    state["controller"] = event.data;
                    return ControllerUpdatedOccurrence{decodedSnapshotState(std::move(state), sequence).controller};
                }
                if (event.type == "session.changed") {
                    const auto id = optionalString(event.data, "sessionId");
                    auto parsed = id.has_value() ? SessionIdentity::parse(*id) : std::nullopt;
                    if (!parsed.has_value()) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/sessionId", "session identifier is invalid");
                    }
                    SessionState changed{std::move(*parsed)};
                    const bool connected = event.data.value("connected", true);
                    if (const auto role = optionalString(event.data, "role"); role.has_value()) {
                        const auto decodedRole = sessionRoleFromString(*role);
                        if (!decodedRole.has_value()) {
                            fail(OccurrenceErrorCode::InvalidPayload, "/data/role", "session role is invalid");
                        }
                        changed.role = *decodedRole;
                    }
                    changed.safeDetails = safeDetail(Json{{"connected", connected}}, "/data/session");
                    SessionsUpdatedOccurrence update;
                    update.changedSession = changed;
                    update.connected = connected;
                    update.completeProjection = false;
                    if (*update.connected) {
                        update.sessions.push_back(std::move(changed));
                    }
                    return update;
                }
                if (event.type == "thread.updated") {
                    const Json& thread = member(event.data, "thread", "/data");
                    CanonicalSnapshot decoded = decodedLegacySnapshotState(Json{{"threads", Json::array({thread})}}, sequence);
                    if (decoded.threads.size() != 1) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/thread", "legacy thread projection is incomplete");
                    }
                    ThreadUpsertedOccurrence update{std::move(decoded.threads.front())};
                    update.turns = std::move(decoded.turns);
                    update.items = std::move(decoded.items);
                    update.replaceDescendants = true;
                    return update;
                }
                if (event.type == "thread.list.updated") {
                    return decodePayload(ExpandedEventType::ThreadListUpdated, Json{{"threadList", event.data}}, sequence);
                }
                if (event.type == "thread.removed") {
                    return decodePayload(ExpandedEventType::ThreadRemoved, event.data, sequence);
                }
                if (event.type == "turn.updated") {
                    const Json& turn = member(event.data, "turn", "/data");
                    const auto threadId = optionalString(turn, "threadId");
                    if (!threadId.has_value()) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/turn/threadId", "legacy turn parent is missing");
                    }
                    Json thread{{"id", *threadId}, {"fullyLoaded", false}, {"turns", Json::array({turn})}, {"extensions", Json::object()}};
                    CanonicalSnapshot decoded = decodedLegacySnapshotState(Json{{"threads", Json::array({std::move(thread)})}}, sequence);
                    if (decoded.turns.size() != 1) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/turn", "legacy turn projection is incomplete");
                    }
                    TurnUpsertedOccurrence update{std::move(decoded.turns.front())};
                    update.items = std::move(decoded.items);
                    update.replaceItems = true;
                    return update;
                }
                if (event.type == "item.updated") {
                    const Json& item = member(event.data, "item", "/data");
                    const auto threadId = optionalString(event.data, "threadId");
                    const auto turnId = optionalString(event.data, "turnId");
                    Json state;
                    if (threadId.has_value() && turnId.has_value()) {
                        Json turn{{"id", *turnId},
                                  {"threadId", *threadId},
                                  {"status", "unknown"},
                                  {"active", false},
                                  {"terminal", false},
                                  {"items", Json::array({item})},
                                  {"extensions", Json::object()}};
                        Json thread{{"id", *threadId},
                                    {"fullyLoaded", false},
                                    {"turns", Json::array({std::move(turn)})},
                                    {"extensions", Json::object()}};
                        state = Json{{"threads", Json::array({std::move(thread)})}};
                    } else {
                        state = Json{{"items", Json::array({item})}};
                    }
                    CanonicalSnapshot decoded = decodedLegacySnapshotState(std::move(state), sequence);
                    if (decoded.items.size() != 1) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/item", "legacy item projection is incomplete");
                    }
                    return ItemUpsertedOccurrence{std::move(decoded.items.front())};
                }
                if (event.type == "item.content.updated") {
                    return decodePayload(ExpandedEventType::ItemContentUpdated, event.data, sequence);
                }
                if (event.type == "request.pending") {
                    const Json& request = member(event.data, "request", "/data");
                    CanonicalSnapshot decoded = decodedLegacySnapshotState(Json{{"pendingRequests", Json::array({request})}}, sequence);
                    if (decoded.pendingRequests.size() != 1) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/request", "legacy pending-request projection is incomplete");
                    }
                    PendingRequestsUpdatedOccurrence update;
                    update.pendingRequests.push_back(std::move(decoded.pendingRequests.front()));
                    update.completeProjection = false;
                    return update;
                }
                if (event.type == "request.resolved") {
                    const auto id = optionalString(event.data, "pendingRequestId");
                    auto parsed = id.has_value() ? PendingRequestIdentity::parse(*id) : std::nullopt;
                    if (!parsed.has_value()) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/data/pendingRequestId", "pending request identifier is invalid");
                    }
                    PendingRequestsUpdatedOccurrence update;
                    update.removedRequestId = std::move(*parsed);
                    update.resolutionReason = optionalString(event.data, "reason");
                    update.completeProjection = false;
                    return update;
                }
                if (event.type == "diagnostics.updated") {
                    DiagnosticRecord diagnostic;
                    diagnostic.received = optionalUnsigned(event.data, "received");
                    diagnostic.detailsOmitted = true;
                    DiagnosticsUpdatedOccurrence update{std::move(diagnostic)};
                    update.aggregateLegacyUpdate = true;
                    const auto recent = event.data.find("recent");
                    if (recent != event.data.end()) {
                        if (!recent->is_array()) {
                            fail(OccurrenceErrorCode::InvalidPayload, "/data/recent", "diagnostic aggregate must be an array");
                        }
                        update.aggregateEntries.reserve(recent->size());
                        for (std::size_t index = 0; index < recent->size(); ++index) {
                            if (!recent->at(index).is_string()) {
                                fail(OccurrenceErrorCode::InvalidPayload,
                                     "/data/recent/" + std::to_string(index),
                                     "diagnostic aggregate member must be a string");
                            }
                            DiagnosticRecord entry;
                            entry.message = recent->at(index).get<std::string>();
                            entry.detailsOmitted = true;
                            update.aggregateEntries.push_back(std::move(entry));
                        }
                    }
                    return update;
                }
                std::optional<ExpandedEventType> family = expandedEventTypeFromString(event.type);
                if (familyHint.has_value()) {
                    if (family.has_value() && family != familyHint) {
                        fail(OccurrenceErrorCode::InvalidPayload, "/type", "legacy family hint conflicts with the wire type");
                    }
                    family = familyHint;
                }
                if (!family.has_value()) {
                    fail(OccurrenceErrorCode::AmbiguousLegacyFamily,
                         "/type",
                         "legacy compatibility event does not contain a complete typed expanded projection");
                }
                return decodePayload(*family, event.data, sequence);
            }();
            setWireExtensions(payload, safeDetail(event.extensions, "/extensions"));
            LegacyCompatibilityPayload legacy;
            legacy.extensions = safeDetail(event.extensions, "/extensions");
            if (event.type == "backend.lifecycle.changed") {
                legacy.kind = LegacyCompatibilityKind::ProviderChanged;
            } else if (event.type == "controller.changed") {
                legacy.kind = LegacyCompatibilityKind::ControllerChanged;
            } else if (event.type == "session.changed") {
                legacy.kind = LegacyCompatibilityKind::SessionChanged;
                if (const auto changed = optionalString(event.data, "sessionId"); changed.has_value()) {
                    legacy.changedSessionId = SessionIdentity::parse(*changed);
                }
                legacy.connected = event.data.value("connected", true);
            } else if (event.type == "thread.list.updated") {
                legacy.kind = LegacyCompatibilityKind::ThreadListUpdated;
            } else if (event.type == "thread.updated") {
                legacy.kind = LegacyCompatibilityKind::ThreadUpdated;
            } else if (event.type == "thread.removed") {
                legacy.kind = LegacyCompatibilityKind::ThreadRemoved;
            } else if (event.type == "turn.updated") {
                legacy.kind = LegacyCompatibilityKind::TurnUpdated;
            } else if (event.type == "item.updated") {
                legacy.kind = LegacyCompatibilityKind::ItemUpdated;
            } else if (event.type == "item.content.updated") {
                legacy.kind = LegacyCompatibilityKind::ItemContentUpdated;
            } else if (event.type == "request.pending") {
                legacy.kind = LegacyCompatibilityKind::PendingRequestAdded;
            } else if (event.type == "request.resolved") {
                legacy.kind = LegacyCompatibilityKind::PendingRequestResolved;
                const auto id = optionalString(event.data, "pendingRequestId");
                legacy.resolvedRequestId = id.has_value() ? PendingRequestIdentity::parse(*id) : std::nullopt;
                legacy.resolutionReason = optionalString(event.data, "reason");
            } else if (event.type == "diagnostics.updated") {
                legacy.kind = LegacyCompatibilityKind::DiagnosticsUpdated;
            } else {
                legacy.kind = LegacyCompatibilityKind::DirectExpanded;
            }
            OccurrenceIdentity identity = identityFromContext(sequence, context);
            identity.groupIndex = 0;
            identity.groupCount = 1;
            return makeOccurrenceGroup(std::move(identity), std::move(legacy), {std::move(payload)});
        } catch (const OccurrenceFailure& failure) {
            return failure.error;
        } catch (const std::exception& error) {
            return OccurrenceError{OccurrenceErrorCode::InvalidPayload, "/event", error.what()};
        } catch (...) {
            return OccurrenceError{OccurrenceErrorCode::InvalidPayload, "/event", "legacy occurrence decoding failed"};
        }
    }

    ModelResult<bool> applyOccurrence(CanonicalSnapshot& reduced, const CanonicalOccurrence& occurrence) noexcept {
        try {
            OccurrenceError validation;
            if ((occurrence.expandedPayloads().empty() &&
                 !validateLegacyCompatibility(occurrence.legacyCompatibility(), occurrence.expandedPayloads(), &validation)) ||
                (!occurrence.expandedPayloads().empty() && !validateOccurrenceGroup(occurrence.expandedPayloads(), &validation))) {
                return occurrenceModelError(validation);
            }
            const auto upsert = []<typename Value, typename Identity>(std::vector<Value>& values, Value replacement, Identity&& identity) {
                const auto found = std::find_if(values.begin(), values.end(), [&](const Value& value) {
                    return identity(value);
                });
                if (found == values.end()) {
                    values.push_back(std::move(replacement));
                } else {
                    *found = std::move(replacement);
                }
            };
            const auto markSparseCollectionRepresented = [](DomainState& state) {
                if (state.information == InformationState::Absent || state.information == InformationState::Omitted ||
                    state.information == InformationState::NullValue) {
                    state.information = InformationState::Present;
                }
            };
            const auto markProjectionRootRepresented = [&reduced](std::string_view root) {
                const auto eraseRoot = [root](std::vector<std::string>& paths) {
                    std::erase_if(paths, [root](const std::string& path) {
                        return path == root;
                    });
                };
                eraseRoot(reduced.projection.omittedPaths);
                eraseRoot(reduced.projection.absentPaths);
                eraseRoot(reduced.projection.nullPaths);
            };
            if (occurrence.legacyCompatibility().kind == LegacyCompatibilityKind::LegacyItem &&
                occurrence.legacyCompatibility().legacyItem.has_value()) {
                upsertLegacyItem(reduced, *occurrence.legacyCompatibility().legacyItem);
            }
            if (occurrence.legacyCompatibility().kind == LegacyCompatibilityKind::LegacyPendingRequest &&
                occurrence.legacyCompatibility().legacyPendingRequest.has_value()) {
                upsertLegacyPendingRequest(reduced, *occurrence.legacyCompatibility().legacyPendingRequest);
            }
            for (const OccurrencePayload& payload : occurrence.expandedPayloads()) {
                std::visit(
                    [&](const auto& update) {
                        using Update = std::decay_t<decltype(update)>;
                        if constexpr (std::is_same_v<Update, ProviderUpdatedOccurrence>) {
                            reduced.provider = update.provider;
                        } else if constexpr (std::is_same_v<Update, ControllerUpdatedOccurrence>) {
                            reduced.controller = update.controller;
                        } else if constexpr (std::is_same_v<Update, SessionsUpdatedOccurrence>) {
                            reduced.sessionsPresent = true;
                            if (update.completeProjection) {
                                reduced.sessions = update.sessions;
                            } else if (update.changedSession.has_value()) {
                                std::erase_if(reduced.sessions, [&](const SessionState& value) {
                                    return value.id == update.changedSession->id;
                                });
                                if (update.connected.value_or(true)) {
                                    reduced.sessions.push_back(*update.changedSession);
                                }
                            }
                        } else if constexpr (std::is_same_v<Update, ThreadListUpdatedOccurrence>) {
                            reduced.threadListPresent = true;
                            reduced.threadList = update.threadList;
                        } else if constexpr (std::is_same_v<Update, ThreadUpsertedOccurrence>) {
                            reduced.threadsPresent = true;
                            upsert(reduced.threads, update.thread, [&](const ThreadState& value) {
                                return value.id == update.thread.id;
                            });
                            if (update.replaceDescendants) {
                                reduced.turnsPresent = true;
                                reduced.itemsPresent = true;
                                replaceOrderedItems(reduced, update.items, [&](const ItemData& item) {
                                    return item.threadId == std::optional<ThreadIdentity>{update.thread.id} ||
                                           (!item.threadId.has_value() && item.turnId.has_value() &&
                                            turnIdentityIsUniqueToThread(
                                                reduced.turns,
                                                update.turns,
                                                reduced.items,
                                                update.items,
                                                reduced.legacyItems,
                                                update.thread.id,
                                                *item.turnId));
                                });
                                replaceOrderedSubset(reduced.turns, update.turns, [&](const TurnState& turn) {
                                    return turn.threadId == update.thread.id;
                                });
                            }
                        } else if constexpr (std::is_same_v<Update, ThreadRemovedOccurrence>) {
                            reduced.threadsPresent = true;
                            reduced.turnsPresent = true;
                            reduced.itemsPresent = true;
                            replaceOrderedItems(reduced, {}, [&](const ItemData& item) {
                                return item.threadId == std::optional<ThreadIdentity>{update.threadId} ||
                                       (!item.threadId.has_value() && item.turnId.has_value() &&
                                        turnIdentityIsUniqueToThread(reduced.turns,
                                                                     {},
                                                                     reduced.items,
                                                                     {},
                                                                     reduced.legacyItems,
                                                                     update.threadId,
                                                                     *item.turnId));
                            });
                            std::erase_if(reduced.threads, [&](const ThreadState& value) {
                                return value.id == update.threadId;
                            });
                            std::erase_if(reduced.turns, [&](const TurnState& value) {
                                return value.threadId == update.threadId;
                            });
                        } else if constexpr (std::is_same_v<Update, TurnUpsertedOccurrence>) {
                            reduced.turnsPresent = true;
                            upsert(reduced.turns, update.turn, [&](const TurnState& value) {
                                return value.threadId == update.turn.threadId && value.id == update.turn.id;
                            });
                            if (update.replaceItems) {
                                reduced.itemsPresent = true;
                                replaceOrderedItems(reduced, update.items, [&](const ItemData& item) {
                                    return (item.threadId == std::optional<ThreadIdentity>{update.turn.threadId} &&
                                            item.turnId == std::optional<TurnIdentity>{update.turn.id}) ||
                                           (!item.threadId.has_value() && item.turnId == std::optional<TurnIdentity>{update.turn.id} &&
                                            turnIdentityIsUniqueToThread(
                                                reduced.turns,
                                                {},
                                                reduced.items,
                                                update.items,
                                                reduced.legacyItems,
                                                update.turn.threadId,
                                                update.turn.id));
                                });
                            }
                        } else if constexpr (std::is_same_v<Update, ItemUpsertedOccurrence>) {
                            reduced.itemsPresent = true;
                            upsertItem(reduced, update.item);
                        } else if constexpr (std::is_same_v<Update, ItemContentUpdatedOccurrence>) {
                            reduced.itemsPresent = true;
                            const auto foundById =
                                std::find_if(reduced.items.begin(), reduced.items.end(), [&](const ThreadItem& value) {
                                    return itemData(value).id == update.itemId;
                                });
                            const auto found = std::find_if(reduced.items.begin(), reduced.items.end(), [&](const ThreadItem& value) {
                                const ItemData& item = itemData(value);
                                return item.id == update.itemId && (!update.threadId.has_value() || item.threadId == update.threadId) &&
                                       (!update.turnId.has_value() || item.turnId == update.turnId);
                            });
                            if (found == reduced.items.end()) {
                                if (foundById != reduced.items.end()) {
                                    const ItemData& target = itemData(*foundById);
                                    if (update.threadId.has_value() && target.threadId != update.threadId) {
                                        fail(OccurrenceErrorCode::InvalidPayload,
                                             "/threadId",
                                             "item content parent thread does not match the target item");
                                    }
                                    if (update.turnId.has_value() && target.turnId != update.turnId) {
                                        fail(OccurrenceErrorCode::InvalidPayload,
                                             "/turnId",
                                             "item content parent turn does not match the target item");
                                    }
                                }
                                fail(OccurrenceErrorCode::InvalidPayload, "/itemId", "item content target is missing");
                            }
                            const ItemData& target = itemData(*found);
                            if (update.threadId.has_value() && target.threadId != update.threadId) {
                                fail(OccurrenceErrorCode::InvalidPayload,
                                     "/threadId",
                                     "item content parent thread does not match the target item");
                            }
                            if (update.turnId.has_value() && target.turnId != update.turnId) {
                                fail(OccurrenceErrorCode::InvalidPayload,
                                     "/turnId",
                                     "item content parent turn does not match the target item");
                            }
                            std::visit(
                                [&](auto& item) {
                                    using Item = std::decay_t<decltype(item)>;
                                    constexpr bool CommandOutputItem = std::is_same_v<Item, CommandExecutionItem> ||
                                                                       std::is_same_v<Item, FileChangeItem>;
                                    std::optional<std::string>* content = nullptr;
                                    if (update.channel == std::optional<std::string>{"agentText"}) {
                                        content = &item.value.agentText;
                                    } else if (update.channel == std::optional<std::string>{"reasoningText"}) {
                                        content = &item.value.reasoningText;
                                    } else if (update.channel == std::optional<std::string>{"reasoningSummary"}) {
                                        content = &item.value.reasoningSummary;
                                    } else if (update.channel == std::optional<std::string>{"commandOutput"}) {
                                        content = &item.value.commandOutput;
                                    }
                                    if (content == nullptr) {
                                        fail(OccurrenceErrorCode::InvalidPayload, "/channel", "item content channel is invalid");
                                    }
                                    const bool extendedAgentText =
                                        update.channel == std::optional<std::string>{"agentText"} &&
                                        (update.appendWireRepresentation || update.overflowWireRepresentation);
                                    const bool extendedCommandOutput =
                                        update.channel == std::optional<std::string>{"commandOutput"} &&
                                        update.extendedCommandOutputWireRepresentation;
                                    if (extendedCommandOutput && !CommandOutputItem) {
                                        fail(OccurrenceErrorCode::InvalidPayload,
                                             "/channel",
                                             "append-v2 command output targets an unrelated item kind");
                                    }
                                    if (update.overflowWireRepresentation && !extendedAgentText && !extendedCommandOutput) {
                                        fail(OccurrenceErrorCode::InvalidPayload,
                                             "/channel",
                                             "extended item content representation is invalid for this channel");
                                    }
                                    std::optional<std::string> replacement;
                                    if (update.appendWireRepresentation) {
                                        if (!update.appendHint.has_value()) {
                                            fail(OccurrenceErrorCode::InvalidPayload,
                                                 "/contentDelta",
                                                 "item content append representation is incomplete");
                                        }
                                        const std::size_t retainedBytes = content->has_value() ? (*content)->size() : 0;
                                        if (update.appendHint->baseContentBytes != static_cast<std::uint64_t>(retainedBytes)) {
                                            fail(OccurrenceErrorCode::InvalidPayload,
                                                 "/baseContentBytes",
                                                 "item content append base does not match the retained canonical content");
                                        }
                                        replacement = content->value_or(std::string{});
                                        if (update.appendHint->discardPrefixBytes > retainedBytes ||
                                            (update.appendHint->discardPrefixBytes != 0 && !extendedCommandOutput)) {
                                            fail(OccurrenceErrorCode::InvalidPayload,
                                                 "/discardPrefixBytes",
                                                 "item content discard is invalid for the retained canonical content");
                                        }
                                        const std::size_t discard =
                                            static_cast<std::size_t>(update.appendHint->discardPrefixBytes);
                                        if (discard < replacement->size() &&
                                            (static_cast<unsigned char>((*replacement)[discard]) & 0xc0U) == 0x80U) {
                                            fail(OccurrenceErrorCode::InvalidPayload,
                                                 "/discardPrefixBytes",
                                                 "item content discard splits a UTF-8 code point");
                                        }
                                        replacement->erase(0, discard);
                                        replacement->append(update.appendHint->delta);
                                    } else {
                                        replacement = update.content;
                                    }
                                    std::uint64_t additionalDropped = 0;
                                    if (replacement.has_value() && (extendedAgentText || extendedCommandOutput)) {
                                        const std::size_t maximumBytes = extendedCommandOutput
                                                                             ? MaximumCommandOutputOverflowV2Bytes
                                                                             : MaximumItemContentOverflowV1Bytes;
                                        if (replacement->size() > maximumBytes ||
                                            utf8CharacterPrefixLength(*replacement, replacement->size()) != replacement->size()) {
                                            fail(OccurrenceErrorCode::InvalidPayload,
                                                 "/content",
                                                 "extended item content exceeds the retained channel bound or is invalid UTF-8");
                                        }
                                    } else if (replacement.has_value()) {
                                        const std::size_t retained = utf8CharacterPrefixLength(*replacement, 16'384);
                                        const std::size_t excess = replacement->size() - retained;
                                        additionalDropped = excess > std::numeric_limits<std::uint64_t>::max()
                                                                ? std::numeric_limits<std::uint64_t>::max()
                                                                : static_cast<std::uint64_t>(excess);
                                        replacement->resize(retained);
                                    }
                                    *content = std::move(replacement);
                                    if (update.contentTruncatedKnown) {
                                        item.value.contentTruncated = update.truncation.truncated;
                                    }
                                    if (additionalDropped != 0) {
                                        item.value.contentTruncated = true;
                                    }
                                    if (update.droppedContentBytesKnown && update.appendWireRepresentation) {
                                        // Append metadata is the authoritative post-update
                                        // count and already includes any trimmed suffix.
                                        item.value.droppedContentBytes = update.truncation.droppedBytes;
                                    } else if (update.droppedContentBytesKnown) {
                                        const std::uint64_t baseDropped = update.truncation.droppedBytes;
                                        item.value.droppedContentBytes =
                                            additionalDropped > std::numeric_limits<std::uint64_t>::max() - baseDropped
                                                ? std::numeric_limits<std::uint64_t>::max()
                                                : baseDropped + additionalDropped;
                                    } else if (additionalDropped != 0) {
                                        const std::uint64_t baseDropped = item.value.droppedContentBytes.value_or(0);
                                        item.value.droppedContentBytes =
                                            additionalDropped > std::numeric_limits<std::uint64_t>::max() - baseDropped
                                                ? std::numeric_limits<std::uint64_t>::max()
                                                : baseDropped + additionalDropped;
                                    }
                                },
                                *found);
                        } else if constexpr (std::is_same_v<Update, PendingRequestsUpdatedOccurrence>) {
                            reduced.pendingRequestsPresent = true;
                            if (update.completeProjection) {
                                reduced.pendingRequests = update.pendingRequests;
                                reduced.legacyPendingRequests.clear();
                                normalizePendingRequestSourceOrder(reduced);
                            } else {
                                if (update.removedRequestId.has_value()) {
                                    erasePendingRequest(reduced, *update.removedRequestId);
                                }
                                for (const PendingRequest& request : update.pendingRequests) {
                                    upsertPendingRequest(reduced, request);
                                }
                            }
                        } else if constexpr (std::is_same_v<Update, AccountUpdatedOccurrence>) {
                            reduced.accounts = update.account;
                        } else if constexpr (std::is_same_v<Update, ModelsUpdatedOccurrence>) {
                            reduced.models = update.models;
                        } else if constexpr (std::is_same_v<Update, ConfigurationUpdatedOccurrence>) {
                            reduced.configuration = update.configuration;
                        } else if constexpr (std::is_same_v<Update, ProcessUpdatedOccurrence>) {
                            markProjectionRootRepresented("/processes");
                            markSparseCollectionRepresented(reduced.processesState);
                            upsert(reduced.processes, update.process, [&](const ProcessState& value) {
                                return value.handle == update.process.handle;
                            });
                        } else if constexpr (std::is_same_v<Update, FilesystemWatchUpdatedOccurrence>) {
                            markSparseCollectionRepresented(reduced.filesystemWatches.state);
                            upsert(reduced.filesystemWatches.entries, update.filesystemWatch, [&](const FilesystemWatchRecord& value) {
                                return value.watchId == update.filesystemWatch.watchId;
                            });
                        } else if constexpr (std::is_same_v<Update, FuzzySearchUpdatedOccurrence>) {
                            markSparseCollectionRepresented(reduced.fuzzySearches.state);
                            upsert(reduced.fuzzySearches.entries, update.fuzzySearch, [&](const FuzzySearchRecord& value) {
                                return value.sessionId == update.fuzzySearch.sessionId;
                            });
                        } else if constexpr (std::is_same_v<Update, ReviewsUpdatedOccurrence>) {
                            reduced.reviews = update.reviews;
                            reduced.permissionProfiles.state = update.reviews.state;
                        } else if constexpr (std::is_same_v<Update, IntegrationsUpdatedOccurrence>) {
                            reduced.integrations = update.integrations;
                            reduced.apps.state = update.integrations.state;
                            reduced.externalAgents.state = update.integrations.state;
                            reduced.hooks.state = update.integrations.state;
                            reduced.marketplace.state = update.integrations.state;
                        } else if constexpr (std::is_same_v<Update, PluginsUpdatedOccurrence>) {
                            reduced.plugins = update.plugins;
                            reduced.skills.state = update.plugins.state;
                        } else if constexpr (std::is_same_v<Update, SkillsUpdatedOccurrence>) {
                            reduced.skills = update.skills;
                            reduced.plugins.state = update.skills.state;
                        } else if constexpr (std::is_same_v<Update, McpUpdatedOccurrence>) {
                            reduced.mcp = update.mcp;
                        } else if constexpr (std::is_same_v<Update, PlatformUpdatedOccurrence>) {
                            reduced.platform = update.platform;
                            reduced.windowsSandbox.state = update.platform.state;
                            reduced.remoteControl.state = update.platform.state;
                        } else if constexpr (std::is_same_v<Update, NoticeAddedOccurrence>) {
                            markSparseCollectionRepresented(reduced.notices.state);
                            reduced.notices.entries.push_back(update.notice);
                        } else if constexpr (std::is_same_v<Update, ActivityUpdatedOccurrence>) {
                            markSparseCollectionRepresented(reduced.activities.state);
                            upsert(reduced.activities.entries, update.activity, [&](const ActivityRecord& value) {
                                return value.key == update.activity.key;
                            });
                        } else if constexpr (std::is_same_v<Update, CapacityUpdatedOccurrence>) {
                            reduced.capacity = update.capacity;
                            reduced.capacityPresent = true;
                        } else if constexpr (std::is_same_v<Update, DiagnosticsUpdatedOccurrence>) {
                            const TruncationMetadata retainedTruncation = reduced.diagnostics.state.truncation;
                            reduced.diagnostics.state = DomainState::present(Freshness::Unknown);
                            reduced.diagnostics.state.truncation = retainedTruncation;
                            if (update.aggregateLegacyUpdate) {
                                reduced.diagnostics.entries = update.aggregateEntries;
                            } else {
                                reduced.diagnostics.entries.push_back(update.diagnostic);
                            }
                            if (update.diagnostic.received.has_value()) {
                                reduced.diagnostics.received = update.diagnostic.received;
                            }
                        }
                    },
                    payload);
            }
            const auto markOmitted = [&](TruncationMetadata& truncation, std::size_t amount) {
                if (amount == 0) {
                    return;
                }
                truncation.truncated = true;
                const std::size_t current = truncation.omittedEntries.value_or(0);
                truncation.omittedEntries =
                    amount > std::numeric_limits<std::size_t>::max() - current ? std::numeric_limits<std::size_t>::max() : current + amount;
            };
            const auto trimFront = [&](auto& values, std::size_t maximum, TruncationMetadata& truncation) {
                if (values.size() <= maximum) {
                    return std::size_t{0};
                }
                const std::size_t removed = values.size() - maximum;
                values.erase(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(removed));
                markOmitted(truncation, removed);
                return removed;
            };

            (void) trimFront(reduced.sessions, 128, reduced.truncation);
            (void) trimFront(reduced.threads, 2'048, reduced.truncation);
            (void) trimFront(reduced.turns, 16'384, reduced.truncation);
            (void) trimFront(reduced.items, 65'536, reduced.truncation);
            (void) trimFront(reduced.pendingRequests, 1'024, reduced.truncation);
            (void) trimFront(reduced.processes, 256, reduced.truncation);
            (void) trimFront(reduced.filesystemWatches.entries, 1'024, reduced.filesystemWatches.state.truncation);
            (void) trimFront(reduced.fuzzySearches.entries, 256, reduced.fuzzySearches.state.truncation);
            (void) trimFront(reduced.notices.entries, 256, reduced.notices.state.truncation);
            (void) trimFront(reduced.activities.entries, 512, reduced.activities.state.truncation);
            (void) trimFront(reduced.diagnostics.entries, 64, reduced.diagnostics.state.truncation);

            if (occurrence.legacyCompatibility().kind == LegacyCompatibilityKind::CodexExtension) {
                const auto encoded = encodeLegacyOccurrence(occurrence);
                if (!encoded) {
                    return occurrenceModelError(encoded.error());
                }
                Json stateExtensions = reduced.stateExtensions.json();
                if (!stateExtensions.is_object()) {
                    stateExtensions = Json::object();
                }
                Json& retained = stateExtensions["codexExtensions"];
                if (!retained.is_array()) {
                    retained = Json::array();
                }
                retained.push_back(encoded.value().data);
                std::size_t omitted = 0;
                while (retained.size() > 64) {
                    retained.erase(retained.begin());
                    ++omitted;
                }
                SafeDetailError detailError = SafeDetailError::None;
                auto bounded = SafeDetail::fromJson(stateExtensions, &detailError);
                while (!bounded.has_value() && !retained.empty()) {
                    retained.erase(retained.begin());
                    ++omitted;
                    bounded = SafeDetail::fromJson(stateExtensions, &detailError);
                }
                if (bounded.has_value()) {
                    reduced.stateExtensions = std::move(*bounded);
                } else {
                    ++omitted;
                }
                markOmitted(reduced.truncation, omitted);
            }

            reduced.sequence = occurrence.identity().sequence;
            reduced.projection.sourceStamp = occurrence.identity().sourceStamp;
            reduced.projection.projectionStamp = occurrence.identity().projectionStamp;
            return true;
        } catch (const OccurrenceFailure& failure) {
            return occurrenceModelError(failure.error);
        } catch (const std::exception& error) {
            return ModelError{ModelErrorCode::InvalidShape, "/occurrence", error.what()};
        } catch (...) {
            return ModelError{ModelErrorCode::InvalidShape, "/occurrence", "occurrence reduction failed"};
        }
    }

    ModelResult<CanonicalSnapshot> reduceOccurrence(const CanonicalSnapshot& snapshot, const CanonicalOccurrence& occurrence) noexcept {
        try {
            CanonicalSnapshot reduced = snapshot;
            ModelResult<bool> applied = applyOccurrence(reduced, occurrence);
            if (!applied) {
                return applied.error();
            }
            return reduced;
        } catch (const std::exception& error) {
            return ModelError{ModelErrorCode::InvalidShape, "/occurrence", error.what()};
        } catch (...) {
            return ModelError{ModelErrorCode::InvalidShape, "/occurrence", "occurrence reduction failed"};
        }
    }

} // namespace ai::openai::codex::frontend::internal::model
