/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/internal/model/Journal.h"

#include "ai/openai/codex/frontend/Codec.h"

#include <cstdint>
#include <limits>
#include <string>

namespace ai::openai::codex::frontend::internal::model {
    namespace {
        void copyDraftIdentity(const OccurrenceDraft& draft, OccurrenceIdentity& identity) {
            identity.projectionStamp = draft.projectionStamp;
            identity.sessionId = draft.sessionId;
            identity.controllerId = draft.controllerId;
            identity.threadId = draft.threadId;
            identity.turnId = draft.turnId;
            identity.itemId = draft.itemId;
            identity.pendingRequestId = draft.pendingRequestId;
            identity.processHandle = draft.processHandle;
        }
    } // namespace

    TypedOccurrenceJournal::TypedOccurrenceJournal(JournalConfig config)
        : journalConfig(std::move(config))
        , sequence(journalConfig.initialSequence)
        , replayFloor(journalConfig.initialSequence) {
    }

    std::optional<std::size_t>
    TypedOccurrenceJournal::checkedByteTotal(std::size_t retained, std::size_t additional) noexcept {
        return additional > std::numeric_limits<std::size_t>::max() - retained
                   ? std::nullopt
                   : std::optional<std::size_t>{retained + additional};
    }

    JournalAppendResult TypedOccurrenceJournal::appendGroup(OccurrenceDraft draft) noexcept {
        JournalAppendResult result;
        OccurrenceError draftError;
        if (draft.expandedPayloads.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
            !validateOccurrenceDraft(draft, &draftError)) {
            result.status = JournalAppendStatus::InvalidGroup;
            return result;
        }
        if (sequence == FrontendSequence::maximum()) {
            result.status = JournalAppendStatus::SequenceExhausted;
            return result;
        }
        try {
            const FrontendSequence assigned(sequence.value() + 1);
            const auto groupId = OccurrenceGroupIdentity::parse("occurrence-group-" + std::to_string(assigned.value()));
            if (!groupId.has_value()) {
                result.status = JournalAppendStatus::InvalidGroup;
                return result;
            }

            const auto groupCount =
                draft.expandedPayloads.empty() ? std::uint32_t{1} : static_cast<std::uint32_t>(draft.expandedPayloads.size());
            OccurrenceIdentity identity{assigned, *groupId, 0, groupCount, draft.sourceStamp};
            copyDraftIdentity(draft, identity);
            auto occurrence = makeOccurrenceGroup(
                std::move(identity), std::move(draft.legacyCompatibility), std::move(draft.expandedPayloads));
            if (!occurrence) {
                result.status = JournalAppendStatus::InvalidGroup;
                return result;
            }

            std::size_t byteCount = 1'024;
            const auto expanded = encodeExpandedOccurrence(occurrence.value());
            if (!expanded) {
                result.status = JournalAppendStatus::EncodingFailure;
                return result;
            }
            for (const ExpandedFrontendEvent& event : expanded.value()) {
                const auto encoded = Codec::encodeExpandedEvent(event);
                if (!encoded) {
                    result.status = JournalAppendStatus::EncodingFailure;
                    return result;
                }
                const std::size_t eventBytes = encoded.value().dump().size();
                if (eventBytes > std::numeric_limits<std::size_t>::max() - byteCount) {
                    result.status = JournalAppendStatus::EncodingFailure;
                    return result;
                }
                byteCount += eventBytes;
            }
            const auto legacy = encodeLegacyOccurrence(occurrence.value());
            if (!legacy) {
                result.status = JournalAppendStatus::EncodingFailure;
                return result;
            }
            const auto encodedLegacy = Codec::encodeEvent(legacy.value());
            if (!encodedLegacy || encodedLegacy.value().dump().size() > std::numeric_limits<std::size_t>::max() - byteCount) {
                result.status = JournalAppendStatus::EncodingFailure;
                return result;
            }
            byteCount += encodedLegacy.value().dump().size();

            result.sequence = assigned;
            result.records = {occurrence.value()};
            result.accountedBytes = byteCount;

            if (journalConfig.maximumEntries == 0 || journalConfig.maximumBytes == 0 ||
                byteCount > journalConfig.maximumBytes) {
                sequence = assigned;
                groups.clear();
                retainedEntries = 0;
                retainedBytes = 0;
                replayFloor = assigned;
                replayAtCurrentUnavailable = false;
                result.status = JournalAppendStatus::NotRetained;
                return result;
            }

            const auto totalBytes = checkedByteTotal(retainedBytes, byteCount);
            if (!totalBytes.has_value()) {
                result.status = JournalAppendStatus::EncodingFailure;
                result.sequence.reset();
                result.records.clear();
                result.accountedBytes = 0;
                return result;
            }
            groups.push_back(GroupRecord{assigned, std::move(occurrence).value(), byteCount});
            sequence = assigned;
            replayAtCurrentUnavailable = false;
            ++retainedEntries;
            retainedBytes = *totalBytes;
            while (retainedEntries > journalConfig.maximumEntries || retainedBytes > journalConfig.maximumBytes) {
                const GroupRecord& evicted = groups.front();
                replayFloor = evicted.sequence;
                --retainedEntries;
                retainedBytes -= evicted.accountedBytes;
                groups.pop_front();
            }
            result.status = JournalAppendStatus::Appended;
            return result;
        } catch (...) {
            result.status = JournalAppendStatus::EncodingFailure;
            result.sequence.reset();
            result.records.clear();
            result.accountedBytes = 0;
            return result;
        }
    }

    JournalReplayResult TypedOccurrenceJournal::replayAfter(FrontendSequence after) const {
        JournalReplayResult result;
        result.requestedAfter = after;
        result.oldestReplayableAfter = replayFloor;
        result.currentSequence = sequence;
        if (after > sequence) {
            result.status = JournalReplayStatus::FutureSequence;
            return result;
        }
        if (after < replayFloor || (replayAtCurrentUnavailable && after == sequence)) {
            result.status = JournalReplayStatus::Gap;
            return result;
        }
        result.status = JournalReplayStatus::Available;
        for (const GroupRecord& group : groups) {
            if (group.sequence <= after) {
                continue;
            }
            result.records.push_back(group.occurrence);
            result.accountedBytes += group.accountedBytes;
        }
        return result;
    }

    bool TypedOccurrenceJournal::invalidateReplay() noexcept {
        if (sequence == FrontendSequence::maximum()) {
            groups.clear();
            retainedEntries = 0;
            retainedBytes = 0;
            replayFloor = sequence;
            replayAtCurrentUnavailable = true;
            return false;
        }
        sequence = FrontendSequence(sequence.value() + 1);
        replayFloor = sequence;
        groups.clear();
        retainedEntries = 0;
        retainedBytes = 0;
        replayAtCurrentUnavailable = false;
        return true;
    }

    bool TypedOccurrenceJournal::invalidateReplayAtCurrentSequence() noexcept {
        groups.clear();
        retainedEntries = 0;
        retainedBytes = 0;
        replayFloor = sequence;
        replayAtCurrentUnavailable = true;
        return sequence != FrontendSequence::maximum();
    }

    const JournalConfig& TypedOccurrenceJournal::config() const noexcept {
        return journalConfig;
    }

    FrontendSequence TypedOccurrenceJournal::currentSequence() const noexcept {
        return sequence;
    }

    FrontendSequence TypedOccurrenceJournal::oldestReplayableAfter() const noexcept {
        return replayFloor;
    }

    std::size_t TypedOccurrenceJournal::retainedEntryCount() const noexcept {
        return retainedEntries;
    }

    std::size_t TypedOccurrenceJournal::retainedByteCount() const noexcept {
        return retainedBytes;
    }

    std::optional<FrontendSequence> TypedOccurrenceJournal::oldestRetainedSequence() const noexcept {
        return groups.empty() ? std::nullopt : std::optional<FrontendSequence>{groups.front().sequence};
    }

    std::optional<FrontendSequence> TypedOccurrenceJournal::newestRetainedSequence() const noexcept {
        return groups.empty() ? std::nullopt : std::optional<FrontendSequence>{groups.back().sequence};
    }

} // namespace ai::openai::codex::frontend::internal::model
