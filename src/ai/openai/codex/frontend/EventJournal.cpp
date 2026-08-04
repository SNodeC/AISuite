/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/EventJournal.h"

#include <utility>

namespace ai::openai::codex::frontend {

    EventJournal::EventJournal(EventJournalConfig config)
        : journalConfig(config)
        , current(config.initialSequence)
        , replayFloor(config.initialSequence) {
    }

    EventJournal::OpaqueAppendResult EventJournal::appendOpaque(std::shared_ptr<const void> record, std::size_t serializedBytes) noexcept {
        try {
            if (!record || serializedBytes == 0) {
                return {JournalAppendStatus::InvalidEvent, std::nullopt, 0};
            }
            if (current == SequenceNumber::maximum()) {
                return {JournalAppendStatus::SequenceOverflow, std::nullopt, 0};
            }

            const SequenceNumber next(current.value() + 1);
            OpaqueAppendResult result{JournalAppendStatus::Appended, next, serializedBytes};
            if (journalConfig.maxEntries == 0 || journalConfig.maxBytes == 0 || serializedBytes > journalConfig.maxBytes) {
                entries.clear();
                byteCount = 0;
                current = next;
                replayFloor = next;
                result.status = JournalAppendStatus::NotRetained;
                return result;
            }

            entries.push_back(Entry{next, std::move(record), serializedBytes});
            current = next;
            while (entries.size() > 1 &&
                   (entries.size() > journalConfig.maxEntries || byteCount > journalConfig.maxBytes - serializedBytes)) {
                evictFront();
            }
            byteCount += serializedBytes;
            return result;
        } catch (...) {
            return {JournalAppendStatus::EncodingFailure, std::nullopt, 0};
        }
    }

    EventJournal::OpaqueReplayResult EventJournal::replayOpaqueAfter(SequenceNumber sequence) const {
        OpaqueReplayResult result;
        result.requestedAfter = sequence;
        result.oldestReplayableAfter = replayFloor;
        result.currentSequence = current;

        if (sequence > current) {
            result.status = JournalReplayStatus::FutureSequence;
            return result;
        }
        if (sequence < replayFloor || (replayAtCurrentUnavailable && sequence == current)) {
            result.status = JournalReplayStatus::Gap;
            return result;
        }

        result.status = JournalReplayStatus::Available;
        for (const Entry& entry : entries) {
            if (entry.sequence <= sequence) {
                continue;
            }
            if (!entry.opaqueRecord) {
                result.status = JournalReplayStatus::Gap;
                result.records.clear();
                return result;
            }
            result.records.push_back(entry.opaqueRecord);
        }
        return result;
    }

    bool EventJournal::invalidateReplay() noexcept {
        entries.clear();
        byteCount = 0;
        if (current == SequenceNumber::maximum()) {
            replayFloor = current;
            replayAtCurrentUnavailable = true;
            return false;
        }
        current = SequenceNumber(current.value() + 1);
        replayFloor = current;
        replayAtCurrentUnavailable = false;
        return true;
    }

    EventJournalConfig EventJournal::config() const noexcept {
        return journalConfig;
    }

    SequenceNumber EventJournal::currentSequence() const noexcept {
        return current;
    }

    SequenceNumber EventJournal::oldestReplayableAfter() const noexcept {
        return replayFloor;
    }

    std::optional<SequenceNumber> EventJournal::oldestRetainedSequence() const noexcept {
        if (entries.empty()) {
            return std::nullopt;
        }
        return entries.front().sequence;
    }

    std::optional<SequenceNumber> EventJournal::newestRetainedSequence() const noexcept {
        if (entries.empty()) {
            return std::nullopt;
        }
        return entries.back().sequence;
    }

    std::size_t EventJournal::retainedEntryCount() const noexcept {
        return entries.size();
    }

    std::size_t EventJournal::retainedBytes() const noexcept {
        return byteCount;
    }

    void EventJournal::evictFront() noexcept {
        if (entries.empty()) {
            return;
        }
        replayFloor = entries.front().sequence;
        byteCount -= entries.front().serializedBytes;
        entries.pop_front();
    }

} // namespace ai::openai::codex::frontend
