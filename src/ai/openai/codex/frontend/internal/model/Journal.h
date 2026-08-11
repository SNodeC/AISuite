/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_JOURNAL_H
#define AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_JOURNAL_H

#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/internal/model/Occurrence.h"

#include <cstddef>
#include <deque>
#include <limits>
#include <optional>
#include <vector>

namespace ai::openai::codex::frontend::internal::model {

    struct JournalConfig {
        std::size_t maximumEntries = DefaultJournalMaxEntries;
        std::size_t maximumBytes = DefaultJournalMaxBytes;
        FrontendSequence initialSequence;

        bool operator==(const JournalConfig&) const = default;
    };

    enum class JournalAppendStatus { Appended, NotRetained, InvalidGroup, SequenceExhausted, EncodingFailure };

    struct JournalAppendResult {
        JournalAppendStatus status = JournalAppendStatus::InvalidGroup;
        std::optional<FrontendSequence> sequence;
        std::vector<CanonicalOccurrence> records;
        std::size_t accountedBytes = 0;

        bool operator==(const JournalAppendResult&) const = default;
    };

    enum class JournalReplayStatus { Available, Gap, FutureSequence };

    struct JournalReplayResult {
        JournalReplayStatus status = JournalReplayStatus::Available;
        FrontendSequence requestedAfter;
        FrontendSequence oldestReplayableAfter;
        FrontendSequence currentSequence;
        std::vector<CanonicalOccurrence> records;
        std::size_t accountedBytes = 0;

        bool operator==(const JournalReplayResult&) const = default;
    };

    class TypedOccurrenceJournal {
    public:
        explicit TypedOccurrenceJournal(JournalConfig config = {});

        [[nodiscard]] static std::optional<std::size_t>
        checkedByteTotal(std::size_t retained, std::size_t additional) noexcept;

        [[nodiscard]] JournalAppendResult appendGroup(OccurrenceDraft draft) noexcept;
        [[nodiscard]] JournalReplayResult replayAfter(FrontendSequence after) const;
        [[nodiscard]] bool invalidateReplay() noexcept;
        [[nodiscard]] bool invalidateReplayAtCurrentSequence() noexcept;

        [[nodiscard]] const JournalConfig& config() const noexcept;
        [[nodiscard]] FrontendSequence currentSequence() const noexcept;
        [[nodiscard]] FrontendSequence oldestReplayableAfter() const noexcept;
        [[nodiscard]] std::size_t retainedEntryCount() const noexcept;
        [[nodiscard]] std::size_t retainedByteCount() const noexcept;
        [[nodiscard]] std::optional<FrontendSequence> oldestRetainedSequence() const noexcept;
        [[nodiscard]] std::optional<FrontendSequence> newestRetainedSequence() const noexcept;

    private:
        struct GroupRecord {
            FrontendSequence sequence;
            CanonicalOccurrence occurrence;
            std::size_t accountedBytes = 0;
        };

        JournalConfig journalConfig;
        FrontendSequence sequence;
        FrontendSequence replayFloor;
        std::deque<GroupRecord> groups;
        std::size_t retainedEntries = 0;
        std::size_t retainedBytes = 0;
        bool replayAtCurrentUnavailable = false;
    };

} // namespace ai::openai::codex::frontend::internal::model

#endif // AI_OPENAI_CODEX_FRONTEND_INTERNAL_MODEL_JOURNAL_H
