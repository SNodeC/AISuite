/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/model/Journal.h"
#include "support/TestResult.h"

#include <limits>
#include <string>

namespace {
    namespace model = ai::openai::codex::frontend::internal::model;

    model::OccurrenceDraft providerDraft(std::string suffix) {
        model::ProviderState provider;
        provider.lifecycle = model::ProviderLifecycle::Ready;
        return {*model::SourceStamp::parse("server_notification:ServerNotification:method:" + suffix),
                model::ProviderUpdatedOccurrence{std::move(provider)}};
    }

    model::OccurrenceDraft containedDraft() {
        model::LegacyCompatibilityPayload legacy;
        legacy.kind = model::LegacyCompatibilityKind::CodexExtension;
        model::LegacySafeExtension extension;
        extension.method = "future/unknownNotification";
        legacy.safeExtension = std::move(extension);
        return {*model::SourceStamp::parse("server_notification:ServerNotification:method:future/unknownNotification"),
                std::move(legacy),
                {}};
    }

    model::OccurrenceDraft oversizedProviderDraft() {
        model::ProviderState provider;
        provider.lifecycle = model::ProviderLifecycle::Ready;
        provider.extensions = *model::SafeDetail::fromJson(
            ai::openai::codex::frontend::Json{{"padding", std::string(8U * 1024U, 'x')}});
        return {*model::SourceStamp::parse("backend-event:oversized-journal-group"),
                model::ProviderUpdatedOccurrence{std::move(provider)}};
    }

    model::OccurrenceDraft replayableProviderDraft() {
        return providerDraft("provider/journal-normal");
    }

    model::OccurrenceDraft retainedCommandOverflowDraft() {
        const std::string prefix(16U * 1024U, 'p');
        const std::string suffix(512U * 1024U, 's');
        model::ItemContentUpdatedOccurrence update{model::ItemIdentity{"command-item"}};
        update.threadId = model::ThreadIdentity{"thread"};
        update.turnId = model::TurnIdentity{"turn"};
        update.channel = "commandOutput";
        update.itemKind = ai::openai::codex::frontend::ThreadItemKind::CommandExecution;
        update.content = prefix;
        update.truncation.truncated = true;
        update.truncation.droppedBytes = suffix.size();
        update.overflowV1 = model::ItemContentOverflowV1{prefix.size(), suffix, 0, false, false};
        update.appendHint = model::ItemContentAppendHint{prefix.size(), suffix, 0, true};
        return {*model::SourceStamp::parse("backend-event:900"), std::move(update)};
    }

    void testAccountingBoundary(tests::support::TestResult& result) {
        const std::size_t maximum = std::numeric_limits<std::size_t>::max();
        result.expectTrue(model::TypedOccurrenceJournal::checkedByteTotal(maximum - 1, 1) == maximum &&
                              !model::TypedOccurrenceJournal::checkedByteTotal(maximum, 1).has_value(),
                          "journal byte accounting detects size_t overflow before mutating retained state");
    }

    void testReplayEvictionAndFallback(tests::support::TestResult& result) {
        model::TypedOccurrenceJournal journal{{1, std::numeric_limits<std::size_t>::max(), model::FrontendSequence{}}};
        const auto first = journal.appendGroup(providerDraft("provider/ready"));
        const auto second = journal.appendGroup(containedDraft());
        const auto gap = journal.replayAfter(model::FrontendSequence{});
        const auto replay = journal.replayAfter(model::FrontendSequence{1});
        const auto future = journal.replayAfter(model::FrontendSequence{3});
        result.expectTrue(first.status == model::JournalAppendStatus::Appended &&
                              second.status == model::JournalAppendStatus::Appended && second.records.size() == 1 &&
                              second.records.front().expandedPayloads().empty() && journal.retainedEntryCount() == 1 &&
                              gap.status == model::JournalReplayStatus::Gap && replay.status == model::JournalReplayStatus::Available &&
                              replay.records.size() == 1 && future.status == model::JournalReplayStatus::FutureSequence,
                          "journal retains semantic groups, evicts deterministically, and distinguishes gaps from future cursors");
    }

    void testExhaustionAndNonRetention(tests::support::TestResult& result) {
        model::TypedOccurrenceJournal exhausted{
            {4, 4'096, model::FrontendSequence{std::numeric_limits<std::uint64_t>::max()}}};
        const auto append = exhausted.appendGroup(providerDraft("provider/ready"));
        const bool invalidated = exhausted.invalidateReplay();
        const auto replay = exhausted.replayAfter(exhausted.currentSequence());
        model::TypedOccurrenceJournal noRetention{{0, 0, model::FrontendSequence{}}};
        const auto omitted = noRetention.appendGroup(containedDraft());
        result.expectTrue(append.status == model::JournalAppendStatus::SequenceExhausted && !invalidated &&
                              replay.status == model::JournalReplayStatus::Gap &&
                              omitted.status == model::JournalAppendStatus::NotRetained &&
                              omitted.sequence == std::optional<model::FrontendSequence>{model::FrontendSequence{1}} &&
                              noRetention.retainedEntryCount() == 0,
                          "sequence exhaustion invalidates replay and non-retained occurrences still advance the cursor exactly once");
    }

    void testSameSequenceReplayInvalidation(tests::support::TestResult& result) {
        model::TypedOccurrenceJournal journal{{4, 64U * 1024U, model::FrontendSequence{}}};
        const auto committed = journal.appendGroup(providerDraft("provider/snapshot-fallback"));
        const bool canAdvance = journal.invalidateReplayAtCurrentSequence();
        const auto invalidated = journal.replayAfter(model::FrontendSequence{1});
        const auto next = journal.appendGroup(providerDraft("provider/after-snapshot-fallback"));
        const auto resumed = journal.replayAfter(model::FrontendSequence{1});
        result.expectTrue(committed.status == model::JournalAppendStatus::Appended &&
                              committed.sequence == model::FrontendSequence{1} && canAdvance &&
                              invalidated.status == model::JournalReplayStatus::Gap &&
                              invalidated.currentSequence == model::FrontendSequence{1} &&
                              journal.currentSequence() == model::FrontendSequence{2} &&
                              next.status == model::JournalAppendStatus::Appended && next.sequence == model::FrontendSequence{2} &&
                              resumed.status == model::JournalReplayStatus::Available && resumed.records.size() == 1 &&
                              resumed.records.front().identity().sequence == model::FrontendSequence{2},
                          "same-sequence Snapshot invalidation preserves the committed cursor and later append continuity");
    }

    void testOversizedGroupReplayHole(tests::support::TestResult& result) {
        model::TypedOccurrenceJournal normalMeasurement{
            {4, std::numeric_limits<std::size_t>::max(), model::FrontendSequence{}}};
        model::TypedOccurrenceJournal oversizedMeasurement{
            {4, std::numeric_limits<std::size_t>::max(), model::FrontendSequence{}}};
        const auto measuredNormal = normalMeasurement.appendGroup(replayableProviderDraft());
        const auto measuredOversized = oversizedMeasurement.appendGroup(oversizedProviderDraft());
        const bool measured = measuredNormal.status == model::JournalAppendStatus::Appended &&
                              measuredOversized.status == model::JournalAppendStatus::Appended &&
                              measuredOversized.accountedBytes > measuredNormal.accountedBytes;

        const std::size_t maximumBytes = measuredNormal.accountedBytes +
                                         (measuredOversized.accountedBytes - measuredNormal.accountedBytes) / 2;
        model::TypedOccurrenceJournal journal{{4, maximumBytes, model::FrontendSequence{}}};
        const auto sequence1 = journal.appendGroup(replayableProviderDraft());
        const auto sequence2 = journal.appendGroup(oversizedProviderDraft());
        const auto sequence3 = journal.appendGroup(replayableProviderDraft());

        const auto replayAfter0 = journal.replayAfter(model::FrontendSequence{});
        const auto replayAfter1 = journal.replayAfter(model::FrontendSequence{1});
        const auto replayAfter2 = journal.replayAfter(model::FrontendSequence{2});
        const auto replayAfter3 = journal.replayAfter(model::FrontendSequence{3});
        const auto future = journal.replayAfter(model::FrontendSequence{4});

        result.expectTrue(measured,
                          "the oversized-group replay test derives a byte limit strictly between the normal and oversized encodings");
        result.expectTrue(sequence1.status == model::JournalAppendStatus::Appended &&
                              sequence1.sequence == model::FrontendSequence{1} &&
                              sequence2.status == model::JournalAppendStatus::NotRetained &&
                              sequence2.sequence == model::FrontendSequence{2} && !sequence2.records.empty() &&
                              sequence3.status == model::JournalAppendStatus::Appended &&
                              sequence3.sequence == model::FrontendSequence{3} && journal.currentSequence() == model::FrontendSequence{3} &&
                              journal.oldestReplayableAfter() == model::FrontendSequence{2} && journal.retainedEntryCount() == 1,
                          "a live-deliverable oversized sequence advances the cursor, records the replay hole, and retains a later normal group");
        result.expectTrue(replayAfter0.status == model::JournalReplayStatus::Gap &&
                              replayAfter1.status == model::JournalReplayStatus::Gap,
                          "replay cursors before the non-retained sequence cannot cross its explicit replay hole");
        result.expectTrue(replayAfter2.status == model::JournalReplayStatus::Available && replayAfter2.records.size() == 1 &&
                              replayAfter2.records.front().identity().sequence == model::FrontendSequence{3},
                          "the cursor at the non-retained sequence can replay the later retained sequence without crossing the hole");
        result.expectTrue(replayAfter3.status == model::JournalReplayStatus::Available && replayAfter3.records.empty() &&
                              future.status == model::JournalReplayStatus::FutureSequence,
                          "the current cursor has no later records and a future cursor remains distinguishable from a replay gap");
    }

    void testRetainedOverflowAccounting(tests::support::TestResult& result) {
        model::TypedOccurrenceJournal measurement{
            {4, std::numeric_limits<std::size_t>::max(), model::FrontendSequence{}}};
        const auto measured = measurement.appendGroup(retainedCommandOverflowDraft());
        const std::size_t maximumBytes = measured.accountedBytes > 1 ? measured.accountedBytes - 1 : 0;
        model::TypedOccurrenceJournal bounded{{4, maximumBytes, model::FrontendSequence{}}};
        const auto omitted = bounded.appendGroup(retainedCommandOverflowDraft());
        const auto replay = bounded.replayAfter(model::FrontendSequence{});
        result.expectTrue(measured.status == model::JournalAppendStatus::Appended &&
                              measured.accountedBytes > 512U * 1024U &&
                              omitted.status == model::JournalAppendStatus::NotRetained &&
                              omitted.sequence == model::FrontendSequence{1} && bounded.retainedEntryCount() == 0 &&
                              replay.status == model::JournalReplayStatus::Gap,
                          "journal accounting includes retained append hints and overflow suffix memory before enforcing its byte bound");
    }
}

int main() {
    tests::support::TestResult result;
    testAccountingBoundary(result);
    testReplayEvictionAndFallback(result);
    testExhaustionAndNonRetention(result);
    testSameSequenceReplayInvalidation(result);
    testOversizedGroupReplayHole(result);
    testRetainedOverflowAccounting(result);
    return result.processResult();
}
