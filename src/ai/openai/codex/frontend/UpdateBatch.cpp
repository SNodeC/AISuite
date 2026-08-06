/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/UpdateBatch.h"

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/detail/EventRepresentation.h"

#include <algorithm>
#include <compare>
#include <string>
#include <utility>

namespace ai::openai::codex::frontend {

    namespace {

        EventBatch makeBatch(std::vector<FrontendEvent> events) {
            EventBatch batch;
            batch.fromSequence = events.front().sequence;
            batch.toSequence = events.back().sequence;
            batch.events = std::move(events);
            return batch;
        }

        std::optional<std::size_t> encodedSize(const EventBatch& batch) {
            const auto serialized = Codec::serializeServer(ServerMessage{batch});
            if (!serialized) {
                return std::nullopt;
            }
            return serialized.value().size();
        }

        bool occurrenceOrderIsValid(const std::vector<FrontendEvent>& events) noexcept {
            for (std::size_t index = 1; index < events.size(); ++index) {
                if (events[index].sequence < events[index - 1].sequence) {
                    return false;
                }
                if (events[index].sequence != events[index - 1].sequence) {
                    continue;
                }
                if (!expandedEventTypeFromString(events[index - 1].type).has_value() ||
                    !expandedEventTypeFromString(events[index].type).has_value()) {
                    return false;
                }
            }
            return true;
        }

        detail::EventRepresentation
        occurrenceRepresentation(const std::vector<FrontendEvent>& events, std::size_t begin, std::size_t end) noexcept {
            detail::EventRepresentation possible = detail::EventRepresentation::Either;
            for (std::size_t index = begin; index < end; ++index) {
                possible = detail::intersectRepresentations(possible, detail::eventRepresentation(events[index].type));
            }
            if (end - begin > 1) {
                possible = detail::intersectRepresentations(possible, detail::EventRepresentation::Expanded);
            }
            return possible;
        }

    } // namespace

    UpdateBatchBuilder::UpdateBatchBuilder(UpdateBatchConfig config) noexcept
        : batchConfig(config) {
    }

    UpdateBatchConfig UpdateBatchBuilder::config() const noexcept {
        return batchConfig;
    }

    UpdateBatchResult UpdateBatchBuilder::build(const std::vector<FrontendEvent>& events) const noexcept {
        try {
            UpdateBatchResult result;
            if (batchConfig.maxEvents == 0 || batchConfig.maxSerializedBytes == 0) {
                result.status = UpdateBatchStatus::InvalidBounds;
                return result;
            }
            if (events.empty()) {
                result.status = UpdateBatchStatus::Success;
                return result;
            }
            if (!occurrenceOrderIsValid(events)) {
                result.status = UpdateBatchStatus::InvalidSequence;
                return result;
            }

            std::vector<FrontendEvent> pending;
            std::size_t pendingBytes = 0;
            detail::EventRepresentation pendingRepresentation = detail::EventRepresentation::Either;
            pending.reserve(std::min(batchConfig.maxEvents, events.size()));

            for (std::size_t offset = 0; offset < events.size();) {
                std::size_t end = offset + 1;
                while (end < events.size() && events[end].sequence == events[offset].sequence) {
                    ++end;
                }

                // Legacy and expanded event arrays are distinct schema
                // branches. A connection may negotiate expansion for one
                // family but retain the legacy representation for another, so
                // never combine the two representations in one wire batch.
                const detail::EventRepresentation occurrenceMask = occurrenceRepresentation(events, offset, end);
                if (occurrenceMask == detail::EventRepresentation::None) {
                    result.status = UpdateBatchStatus::EncodingFailure;
                    result.batches.clear();
                    return result;
                }
                if (!pending.empty() &&
                    detail::intersectRepresentations(pendingRepresentation, occurrenceMask) == detail::EventRepresentation::None) {
                    result.batches.push_back(BoundedEventBatch{makeBatch(std::move(pending)), pendingBytes});
                    pending.clear();
                    pending.reserve(std::min(batchConfig.maxEvents, events.size()));
                    pendingRepresentation = detail::EventRepresentation::Either;
                }

                std::vector<FrontendEvent> candidate = pending;
                candidate.insert(candidate.end(),
                                 events.begin() + static_cast<std::ptrdiff_t>(offset),
                                 events.begin() + static_cast<std::ptrdiff_t>(end));
                const EventBatch candidateBatch = makeBatch(std::move(candidate));
                const auto candidateBytes = encodedSize(candidateBatch);
                if (!candidateBytes.has_value()) {
                    result.status = UpdateBatchStatus::EncodingFailure;
                    result.batches.clear();
                    return result;
                }

                const bool countExceeded = candidateBatch.events.size() > batchConfig.maxEvents;
                const bool bytesExceeded = *candidateBytes > batchConfig.maxSerializedBytes;
                if (!countExceeded && !bytesExceeded) {
                    pending = candidateBatch.events;
                    pendingBytes = *candidateBytes;
                    pendingRepresentation = detail::intersectRepresentations(pendingRepresentation, occurrenceMask);
                    offset = end;
                    continue;
                }

                if (!pending.empty()) {
                    result.batches.push_back(BoundedEventBatch{makeBatch(std::move(pending)), pendingBytes});
                    pending.clear();
                    pending.reserve(std::min(batchConfig.maxEvents, events.size()));
                    pendingRepresentation = detail::EventRepresentation::Either;
                }

                std::vector<FrontendEvent> occurrence(events.begin() + static_cast<std::ptrdiff_t>(offset),
                                                      events.begin() + static_cast<std::ptrdiff_t>(end));
                EventBatch occurrenceBatch = makeBatch(std::move(occurrence));
                const auto occurrenceBytes = encodedSize(occurrenceBatch);
                if (!occurrenceBytes.has_value()) {
                    result.status = UpdateBatchStatus::EncodingFailure;
                    result.batches.clear();
                    return result;
                }
                if (occurrenceBatch.events.size() > batchConfig.maxEvents || *occurrenceBytes > batchConfig.maxSerializedBytes) {
                    result.status = UpdateBatchStatus::SnapshotRequired;
                    result.oversizedSequence = events[offset].sequence;
                    result.batches.clear();
                    return result;
                }
                pending = std::move(occurrenceBatch.events);
                pendingBytes = *occurrenceBytes;
                pendingRepresentation = occurrenceMask;
                offset = end;
            }

            if (!pending.empty()) {
                result.batches.push_back(BoundedEventBatch{makeBatch(std::move(pending)), pendingBytes});
            }
            result.status = UpdateBatchStatus::Success;
            return result;
        } catch (...) {
            return UpdateBatchResult{UpdateBatchStatus::EncodingFailure, {}, std::nullopt};
        }
    }

} // namespace ai::openai::codex::frontend
