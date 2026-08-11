    AISUITE_STATE_GETTER(TruncationMetadata, truncation)
#undef AISUITE_STATE_GETTER

    const Projected<DiagnosticCollectionState>& State::diagnostics() const noexcept {
        return impl->diagnostics;
    }
    const ProcessState* State::process(const ProcessHandle& handle) const noexcept {
        return process(handle.value);
    }
    const ProcessState* State::process(std::string_view handle) const noexcept {
        if (!impl->processes.value)
            return nullptr;
        const auto found =
            std::find_if(impl->processes.value->entries.begin(), impl->processes.value->entries.end(), [handle](const auto& value) {
                return value.processHandle.value == handle;
            });
        return found == impl->processes.value->entries.end() ? nullptr : &*found;
    }
    const FilesystemWatchState* State::filesystemWatch(const typed::FsWatchId& id) const noexcept {
        if (!impl->filesystemWatches.value)
            return nullptr;
        const auto found = std::find_if(
            impl->filesystemWatches.value->entries.begin(), impl->filesystemWatches.value->entries.end(), [&](const auto& value) {
                return value.watchId == id;
            });
        return found == impl->filesystemWatches.value->entries.end() ? nullptr : &*found;
    }
    const FuzzySearchState* State::fuzzySearch(const FuzzySearchSessionId& id) const noexcept {
        if (!impl->fuzzySearches.value)
            return nullptr;
        const auto found =
            std::find_if(impl->fuzzySearches.value->entries.begin(), impl->fuzzySearches.value->entries.end(), [&](const auto& value) {
                return value.sessionId == id;
            });
        return found == impl->fuzzySearches.value->entries.end() ? nullptr : &*found;
    }
    const ActivityState* State::activity(const ActivityKey& key) const noexcept {
        if (!impl->activities.value)
            return nullptr;
        const auto found =
            std::find_if(impl->activities.value->entries.begin(), impl->activities.value->entries.end(), [&](const auto& value) {
                return value.key == key;
            });
        return found == impl->activities.value->entries.end() ? nullptr : &*found;
    }
    const frontend::Json& State::compatibilityExtensions() const noexcept {
        return impl->compatibilityExtensions;
    }

    namespace detail {
        State StateReducer::initial() {
            return {};
        }

        std::optional<State> StateReducer::stale(const State& state, std::size_t maximumBytes, std::string& error) {
            const auto boundedFallback = [maximumBytes, &error]() -> std::optional<State> {
                auto fallback = std::make_shared<StateStorage>();
                std::string fallbackError;
                if (!rebuildStateSizeLedger(*fallback) || !stateFits(*fallback, maximumBytes, fallbackError)) {
                    if (error.empty())
                        error = std::move(fallbackError);
                    return std::nullopt;
                }
                return State{std::move(fallback)};
            };
            if (state.impl->freshness == StateFreshness::Stale && !state.impl->session) {
                if (stateFits(*state.impl, maximumBytes, error))
                    return state;
                return boundedFallback();
            }
            auto candidate = std::make_shared<StateStorage>(*state.impl);
            if (!advanceRevision(*candidate, "marking the connection stale", error))
                return std::nullopt;
            candidate->freshness = StateFreshness::Stale;
            candidate->session.reset();
            if (candidate->controller.value)
                candidate->controller.value->ownedByThisClient = false;
            for (PendingRequestState& request : candidate->pendingRequests)
                request.connectionInvalidated = true;
            try {
                if (!rebuildDirectArraySection(*candidate,
                                               StateSizeSection::PendingRequests,
                                               candidate->sizeLedger.pendingRequests,
                                               candidate->pendingRequests,
                                               encodePendingRequestState) ||
                    !setSectionJson(
                        *candidate, StateSizeSection::Controller, encodeProjected(candidate->controller, encodeControllerState)) ||
                    !refreshConnectionStateSections(*candidate) || !refreshCommitStateSections(*candidate)) {
                    error = "decoded frontend state size accounting failed";
                    return std::nullopt;
                }
            } catch (...) {
                error = "decoded frontend state size accounting failed";
                return std::nullopt;
            }
            if (stateFits(*candidate, maximumBytes, error))
                return State{std::move(candidate)};

            // A stale projection is normally smaller because it drops all
            // connection-local SessionInfo. If a byte-exact boundary (for
            // example a revision-width increase) still exceeds the configured
            // limit, discard retained projection data instead of keeping a
            // current/session-owned state after physical disconnect.
            return boundedFallback();
        }

        std::optional<State> StateReducer::beginSynchronization(const State& current,
                                                                const SessionInfo& session,
                                                                std::size_t maximumBytes,
                                                                std::string& error,
                                                                std::optional<ProjectionFingerprintMetadata> projectionFingerprint) {
            if (!accountedStateBytes(*current.impl)) {
                error = "decoded frontend state size accounting failed";
                return std::nullopt;
            }
            auto candidate = std::make_shared<StateStorage>(*current.impl);
            if (!advanceRevision(*candidate, "beginning synchronization", error))
                return std::nullopt;
            candidate->freshness = StateFreshness::Synchronizing;
            candidate->session = session;
            if (projectionFingerprint)
                candidate->projectionFingerprint = std::move(projectionFingerprint);
            candidate->retainedReplayThrough = current.impl->visibleSequence;
            candidate->lastSynchronizationBatchSequence.reset();
            if (candidate->controller.value) {
                candidate->controller.value->ownedByThisClient =
                    candidate->controller.value->sessionId && candidate->controller.value->sessionId->value == session.sessionId;
            }
            try {
                if (!setSectionJson(
                        *candidate, StateSizeSection::Controller, encodeProjected(candidate->controller, encodeControllerState)) ||
                    !refreshConnectionStateSections(*candidate) || !refreshCommitStateSections(*candidate)) {
                    error = "decoded frontend state size accounting failed";
                    return std::nullopt;
                }
            } catch (...) {
                error = "decoded frontend state size accounting failed";
                return std::nullopt;
            }
            if (!stateFits(*candidate, maximumBytes, error))
                return std::nullopt;
            return State{std::move(candidate)};
        }

        std::optional<State> StateReducer::synchronizationStaging(const SessionInfo& session,
                                                                  std::optional<frontend::SequenceNumber> resumeAfter,
                                                                  std::size_t maximumBytes,
                                                                  bool allowLegacyV1,
                                                                  std::string& error,
                                                                  std::optional<ProjectionFingerprintMetadata> projectionFingerprint) {
            auto candidate = std::make_shared<StateStorage>();
            candidate->freshness = StateFreshness::Synchronizing;
            candidate->session = session;
            candidate->projectionFingerprint = std::move(projectionFingerprint);
            candidate->synchronizedThrough = resumeAfter;
            if (!allowLegacyV1 && !selectedCompleteExpandedRepresentation(session)) {
                error = "legacy Frontend Protocol v1 synchronization is not permitted";
                return std::nullopt;
            }
            if (selectedExpandedSnapshot(session)) {
                candidate->representationMode = RepresentationMode::ExpandedV1;
            } else {
                candidate->representationMode = RepresentationMode::LegacyV1;
            }
            if (!rebuildStateSizeLedger(*candidate) || !stateFits(*candidate, maximumBytes, error)) {
                if (error.empty())
                    error = "decoded frontend state size accounting failed";
                return std::nullopt;
            }
            return State{std::move(candidate)};
        }

        std::optional<StateReduction> StateReducer::snapshot(const State& current,
                                                             const frontend::Snapshot& snapshotMessage,
                                                             const SessionInfo& session,
                                                             std::size_t maximumBytes,
                                                             std::size_t maximumRetainedDiagnostics,
                                                             bool allowLegacyV1,
                                                             std::string& error,
                                                             std::optional<ProjectionFingerprintMetadata> projectionFingerprint) {
            std::optional<frontend::SequenceNumber> representedThrough = current.impl->synchronizedThrough;
            if (current.impl->visibleSequence && (!representedThrough || *representedThrough < *current.impl->visibleSequence))
                representedThrough = current.impl->visibleSequence;
            if (representedThrough && snapshotMessage.sequence < *representedThrough) {
                error = "frontend snapshot sequence regressed behind the represented state";
                return std::nullopt;
            }
            auto candidate = std::make_shared<StateStorage>();
            candidate->revision = current.impl->revision;
            if (!advanceRevision(*candidate, "replacing a snapshot", error))
                return std::nullopt;
            candidate->freshness = StateFreshness::Synchronizing;
            candidate->visibleSequence = snapshotMessage.sequence;
            candidate->session = session;
            candidate->projectionFingerprint = std::move(projectionFingerprint);
            if (!decodeSnapshotContents(*candidate, snapshotMessage, session, maximumRetainedDiagnostics, allowLegacyV1, error))
                return std::nullopt;
            if (!rebuildStateSizeLedger(*candidate) || !stateFits(*candidate, maximumBytes, error)) {
                if (error.empty())
                    error = "decoded frontend state size accounting failed";
                return std::nullopt;
            }
            return StateReduction{State{std::move(candidate)}, {StateReplacedChange{}}, 0, 0, 0};
        }

        std::optional<StateReduction> StateReducer::liveSnapshot(const State& current,
                                                                 const frontend::Snapshot& snapshotMessage,
                                                                 std::size_t maximumBytes,
                                                                 std::size_t maximumRetainedDiagnostics,
                                                                 bool allowLegacyV1,
                                                                 std::string& error) {
            if (!current.impl->session) {
                error = "live frontend snapshot has no active session";
                return std::nullopt;
            }
            std::optional<frontend::SequenceNumber> representedThrough = current.impl->synchronizedThrough;
            if (current.impl->visibleSequence && (!representedThrough || *representedThrough < *current.impl->visibleSequence))
                representedThrough = current.impl->visibleSequence;
            if (representedThrough && snapshotMessage.sequence < *representedThrough) {
                error = "live frontend snapshot sequence regressed behind the represented state";
                return std::nullopt;
            }

            const bool cursorAdvanced = !current.impl->synchronizedThrough || *current.impl->synchronizedThrough < snapshotMessage.sequence;
            auto candidate = std::make_shared<StateStorage>();
            candidate->revision = current.impl->revision;
            if (!advanceRevision(*candidate, "applying a live snapshot barrier", error))
                return std::nullopt;
            candidate->freshness = StateFreshness::Current;
            candidate->visibleSequence = snapshotMessage.sequence;
            candidate->synchronizedThrough = snapshotMessage.sequence;
            candidate->session = current.impl->session;
            candidate->projectionFingerprint = current.impl->projectionFingerprint;
            candidate->retainedReplayThrough.reset();
            candidate->lastSynchronizationBatchSequence.reset();
            if (!decodeSnapshotContents(
                    *candidate, snapshotMessage, *current.impl->session, maximumRetainedDiagnostics, allowLegacyV1, error))
                return std::nullopt;
            if (!rebuildStateSizeLedger(*candidate) || !stateFits(*candidate, maximumBytes, error)) {
                if (error.empty())
                    error = "decoded frontend state size accounting failed";
                return std::nullopt;
            }

            std::vector<Change> changes;
            changes.emplace_back(StateReplacedChange{});
            if (cursorAdvanced)
                changes.emplace_back(CursorAdvancedChange{snapshotMessage.sequence});
            return StateReduction{State{std::move(candidate)}, std::move(changes), 0, 0, 0};
        }

        std::optional<StateReduction> StateReducer::events(const State& current,
                                                           const frontend::EventBatch& batch,
                                                           bool synchronizing,
                                                           std::size_t maximumBytes,
                                                           std::size_t maximumRetainedDiagnostics,
                                                           bool allowLegacyV1,
                                                           std::string& error) {
            if (batch.events.empty() || batch.fromSequence != batch.events.front().sequence ||
                batch.toSequence != batch.events.back().sequence) {
                error = "frontend event batch bounds do not match its nonempty event list";
                return std::nullopt;
            }
            if (synchronizing && current.impl->lastSynchronizationBatchSequence &&
                batch.fromSequence <= *current.impl->lastSynchronizationBatchSequence) {
                error = "frontend replay event batches overlap or split one occurrence group";
                return std::nullopt;
            }
            if (!accountedStateBytes(*current.impl)) {
                error = "decoded frontend state size accounting failed";
                return std::nullopt;
            }
            RepresentationMode mode = current.impl->representationMode;
            if (mode == RepresentationMode::Unknown) {
                mode = eventUsesExpandedRepresentation(current.impl->session ? &*current.impl->session : nullptr, batch.events.front().type)
                           ? RepresentationMode::ExpandedV1
                           : RepresentationMode::LegacyV1;
            }
            auto candidate = std::make_shared<StateStorage>(*current.impl);
            candidate->representationMode = mode;
            if (!advanceRevision(*candidate, "applying an event batch", error))
                return std::nullopt;
            candidate->freshness = synchronizing ? StateFreshness::Synchronizing : StateFreshness::Current;
            std::vector<Change> changes;
            std::size_t applied = 0;
            std::size_t ignored = 0;
            std::optional<frontend::SequenceNumber> representedThrough = current.impl->synchronizedThrough;
            if (current.impl->retainedReplayThrough && (!representedThrough || *representedThrough < *current.impl->retainedReplayThrough))
                representedThrough = current.impl->retainedReplayThrough;
            std::optional<frontend::SequenceNumber> prior;
            bool priorExpanded = false;
            for (const frontend::FrontendEvent& event : batch.events) {
                const bool expanded =
                    eventUsesExpandedRepresentation(current.impl->session ? &*current.impl->session : nullptr, event.type);
                if (!eventRepresentationWasNegotiated(
                        current.impl->session ? &*current.impl->session : nullptr, event.type, expanded, error))
                    return std::nullopt;
                if (prior && event.sequence < *prior) {
                    error = "frontend event sequence regressed within a batch";
                    return std::nullopt;
                }
                if (prior && event.sequence == *prior && (!priorExpanded || !expanded)) {
                    error = "equal event sequences require adjacent expanded occurrence members";
                    return std::nullopt;
                }
                prior = event.sequence;
                priorExpanded = expanded;

                frontend::ExpandedFrontendEvent expandedEvent;
                std::optional<frontend::ExpandedEventType> expandedType;
                if (expanded) {
                    if (!validateExpandedEvent(event, expandedEvent, error))
                        return std::nullopt;
                    expandedType = expandedEvent.type;
                } else {
                    if (!allowLegacyV1 && event.type != "codex.extension") {
                        error = "legacy Frontend Protocol v1 events are not permitted";
                        return std::nullopt;
                    }
                    if (!validateLegacyEvent(event, error))
                        return std::nullopt;
                }
                bool projectionMetadataChanged = false;
                try {
                    if (!applyEventProjectionMetadata(*candidate, event, expandedType, projectionMetadataChanged, error))
                        return std::nullopt;
                    if (projectionMetadataChanged) {
                        if (!refreshProjectionMetadataSection(*candidate) ||
                            (expandedType && !refreshEventProjectionAccounting(*candidate, *expandedType, error)))
                            return std::nullopt;
                    }
                } catch (...) {
                    (void) accountingFailure(*candidate, error);
                    return std::nullopt;
                }
                if (!synchronizing && ((representedThrough && event.sequence <= *representedThrough) ||
                                       (current.impl->visibleSequence && event.sequence <= *current.impl->visibleSequence))) {
                    error = "live frontend event sequence did not advance beyond the represented cursor";
                    return std::nullopt;
                }
                const bool alreadyApplied = synchronizing && representedThrough && event.sequence <= *representedThrough;
                if (alreadyApplied) {
                    // An overlapping replay occurrence is already fully
                    // represented by the committed cursor. Validate its wire
                    // shape, but do not reapply referential semantics against
                    // a later state in which the referenced entity may have
                    // legitimately been removed.
                    ++ignored;
                    continue;
                }
                if (current.impl->visibleSequence && event.sequence <= *current.impl->visibleSequence) {
                    error = event.sequence == *current.impl->visibleSequence
                                ? "expanded occurrence group was split across frontend event batches"
                                : "frontend event sequence regressed behind visibleSequence";
                    return std::nullopt;
                }
                bool success = false;
                try {
                    success = expanded ? applyExpanded(*candidate, expandedEvent, maximumRetainedDiagnostics, changes, error)
                                       : applyLegacy(*candidate, event, maximumRetainedDiagnostics, changes, error);
                } catch (...) {
                    (void) accountingFailure(*candidate, error);
                    return std::nullopt;
                }
                if (!success)
                    return std::nullopt;
                candidate->visibleSequence = event.sequence;
                ++applied;
            }
            if (!synchronizing && candidate->visibleSequence)
                candidate->synchronizedThrough = candidate->visibleSequence;
            if (synchronizing)
                candidate->lastSynchronizationBatchSequence = batch.toSequence;
            if (!refreshCommitStateSections(*candidate)) {
                error = "decoded frontend state size accounting failed";
                return std::nullopt;
            }
            if (!stateFits(*candidate, maximumBytes, error))
                return std::nullopt;
            return StateReduction{State{std::move(candidate)}, std::move(changes), batch.events.size(), applied, ignored};
        }

        std::optional<StateReduction> StateReducer::validateSynchronizationEvents(
            const State& staging, const frontend::EventBatch& batch, std::size_t maximumBytes, bool allowLegacyV1, std::string& error) {
            if (batch.events.empty() || batch.fromSequence != batch.events.front().sequence ||
                batch.toSequence != batch.events.back().sequence) {
                error = "frontend event batch bounds do not match its nonempty event list";
                return std::nullopt;
            }
            if (staging.impl->lastSynchronizationBatchSequence && batch.fromSequence <= *staging.impl->lastSynchronizationBatchSequence) {
                error = "frontend replay event batches overlap or split one occurrence group";
                return std::nullopt;
            }
            const RepresentationMode mode = staging.impl->representationMode;
            if (mode == RepresentationMode::Unknown) {
                error = "synchronization staging state has no expected representation";
                return std::nullopt;
            }
            if (!accountedStateBytes(*staging.impl)) {
                error = "decoded frontend state size accounting failed";
                return std::nullopt;
            }

            auto candidate = std::make_shared<StateStorage>(*staging.impl);
            if (!advanceRevision(*candidate, "validating a synchronization event batch", error))
                return std::nullopt;
            candidate->freshness = StateFreshness::Synchronizing;
            std::optional<frontend::SequenceNumber> representedThrough = staging.impl->synchronizedThrough;
            if (staging.impl->retainedReplayThrough && (!representedThrough || *representedThrough < *staging.impl->retainedReplayThrough))
                representedThrough = staging.impl->retainedReplayThrough;
            std::optional<frontend::SequenceNumber> prior;
            bool priorExpanded = false;
            std::size_t ignored = 0;
            for (const frontend::FrontendEvent& event : batch.events) {
                const bool expanded =
                    eventUsesExpandedRepresentation(staging.impl->session ? &*staging.impl->session : nullptr, event.type);
                if (!eventRepresentationWasNegotiated(
                        staging.impl->session ? &*staging.impl->session : nullptr, event.type, expanded, error))
                    return std::nullopt;
                if (prior && event.sequence < *prior) {
                    error = "frontend event sequence regressed within a batch";
                    return std::nullopt;
                }
                if (prior && event.sequence == *prior && (!priorExpanded || !expanded)) {
                    error = "equal event sequences require adjacent expanded occurrence members";
                    return std::nullopt;
                }
                prior = event.sequence;
                priorExpanded = expanded;

                std::optional<frontend::ExpandedEventType> expandedType;
                if (expanded) {
                    frontend::ExpandedFrontendEvent decoded;
                    if (!validateExpandedEvent(event, decoded, error))
                        return std::nullopt;
                    expandedType = decoded.type;
                } else {
                    if (!allowLegacyV1 && event.type != "codex.extension") {
                        error = "legacy Frontend Protocol v1 events are not permitted";
                        return std::nullopt;
                    }
                    if (!validateLegacyEvent(event, error))
                        return std::nullopt;
                }
                bool projectionMetadataChanged = false;
                try {
                    if (!applyEventProjectionMetadata(*candidate, event, expandedType, projectionMetadataChanged, error))
                        return std::nullopt;
                    if (projectionMetadataChanged) {
                        if (!refreshProjectionMetadataSection(*candidate) ||
                            (expandedType && !refreshEventProjectionAccounting(*candidate, *expandedType, error)))
                            return std::nullopt;
                    }
                } catch (...) {
                    (void) accountingFailure(*candidate, error);
                    return std::nullopt;
                }

                if (representedThrough && event.sequence <= *representedThrough) {
                    ++ignored;
                    continue;
                }
                if (staging.impl->visibleSequence && event.sequence <= *staging.impl->visibleSequence) {
                    error = event.sequence == *staging.impl->visibleSequence
                                ? "expanded occurrence group was split across frontend event batches"
                                : "frontend event sequence regressed behind validation cursor";
                    return std::nullopt;
                }
                candidate->visibleSequence = event.sequence;
            }
            candidate->lastSynchronizationBatchSequence = batch.toSequence;
            if (!refreshCommitStateSections(*candidate)) {
                error = "decoded frontend state size accounting failed";
                return std::nullopt;
            }
            if (!stateFits(*candidate, maximumBytes, error))
                return std::nullopt;
            return StateReduction{State{std::move(candidate)}, {}, batch.events.size(), 0, ignored};
        }

        std::optional<StateReduction> StateReducer::synchronized(const State& current,
                                                                 frontend::SequenceNumber sequence,
                                                                 const SessionInfo& session,
                                                                 std::size_t maximumBytes,
                                                                 std::string& error,
                                                                 std::optional<ProjectionFingerprintMetadata> projectionFingerprint) {
            if (current.impl->visibleSequence && sequence < *current.impl->visibleSequence) {
                error = "sync.complete regressed behind visibleSequence";
                return std::nullopt;
            }
            if (current.impl->synchronizedThrough && sequence < *current.impl->synchronizedThrough) {
                error = "sync.complete sequence regressed";
                return std::nullopt;
            }
            if (!accountedStateBytes(*current.impl)) {
                error = "decoded frontend state size accounting failed";
                return std::nullopt;
            }
            auto candidate = std::make_shared<StateStorage>(*current.impl);
            if (!advanceRevision(*candidate, "committing synchronization", error))
                return std::nullopt;
            candidate->freshness = StateFreshness::Current;
            candidate->synchronizedThrough = sequence;
            candidate->retainedReplayThrough.reset();
            candidate->lastSynchronizationBatchSequence.reset();
            candidate->session = session;
            if (projectionFingerprint)
                candidate->projectionFingerprint = std::move(projectionFingerprint);
            if (!refreshConnectionStateSections(*candidate) || !refreshCommitStateSections(*candidate)) {
                error = "decoded frontend state size accounting failed";
                return std::nullopt;
            }
            if (!stateFits(*candidate, maximumBytes, error))
                return std::nullopt;
            return StateReduction{State{std::move(candidate)}, {CursorAdvancedChange{sequence}}, 0, 0, 0};
        }

        frontend::Json StateReducer::serializeForTesting(const State& state) noexcept {
            try {
                return serializeFixtureState(*state.impl);
            } catch (...) {
                return frontend::Json::object();
            }
        }

        frontend::Json StateReducer::serializeChangesForTesting(std::span<const Change> changes) noexcept {
            try {
                return serializeChanges(changes);
            } catch (...) {
                return frontend::Json::array();
            }
        }

        std::optional<std::size_t> StateReducer::accountedBytesForTesting(const State& state) noexcept {
            return accountedStateBytes(*state.impl);
        }

        std::optional<std::size_t> StateReducer::referenceBytesForTesting(const State& state) noexcept {
            return referenceStateBytes(*state.impl);
        }

        bool StateReducer::accountingIsConsistentForTesting(const State& state) noexcept {
            const std::optional<std::size_t> accounted = accountedStateBytes(*state.impl);
            const std::optional<std::size_t> reference = referenceStateBytes(*state.impl);
            return accounted && reference && *accounted == *reference;
        }

        bool StateReducer::accountingRejectsOverflowForTesting() noexcept {
            std::size_t ignored = 0;
            detail::StateArrayContribution contribution{1, std::numeric_limits<std::size_t>::max()};
            return !checkedAdd(std::numeric_limits<std::size_t>::max(), 1, ignored) &&
                   !adjustArrayContribution(contribution, std::nullopt, std::size_t{1});
        }

        bool StateReducer::accountingRejectsUnderflowForTesting() noexcept {
            std::size_t ignored = 0;
            detail::StateArrayContribution empty;
            return !checkedSubtract(0, 1, ignored) && !adjustArrayContribution(empty, std::size_t{1}, std::nullopt);
        }

        bool StateReducer::debugAccountingInvariantEnabledForTesting() noexcept {
#ifndef NDEBUG
            return true;
#else
            return false;
#endif
        }

        std::size_t StateReducer::debugAccountingVerificationCountForTesting() noexcept {
#ifndef NDEBUG
            return DebugAccountingVerificationCount;
#else
            return 0;
#endif
        }

        std::size_t StateReducer::debugAccountingRebuildCountForTesting() noexcept {
#ifndef NDEBUG
            return DebugAccountingRebuildCount;
#else
            return 0;
#endif
        }

        void StateReducer::resetDebugAccountingVerificationCountForTesting() noexcept {
#ifndef NDEBUG
            DebugAccountingVerificationCount = 0;
#endif
        }

        void StateReducer::resetDebugAccountingRebuildCountForTesting() noexcept {
#ifndef NDEBUG
            DebugAccountingRebuildCount = 0;
#endif
        }

        State StateReducer::withRevisionForTesting(const State& state, std::uint64_t revision) {
            auto candidate = std::make_shared<StateStorage>(*state.impl);
            if (!candidate->sizeLedger.initialized)
                (void) rebuildStateSizeLedger(*candidate);
            candidate->revision = revision;
            (void) refreshCommitStateSections(*candidate);
            return State{std::move(candidate)};
        }

        std::optional<ThreadState> StateReducer::decodeThreadState(const frontend::Json& value, std::string& error) {
            ThreadState decoded;
            if (!decodeThread(value, decoded, error))
                return std::nullopt;
            return decoded;
        }

        std::optional<TurnState> StateReducer::decodeTurnState(const frontend::Json& value, std::string& error) {
            TurnState decoded;
            if (!decodeTurn(value, decoded, error, std::nullopt, false))
                return std::nullopt;
            return decoded;
        }

        std::optional<TurnResultState> StateReducer::decodeTurnResultState(const frontend::Json& value, std::string& error) {
            TurnResultState result;
            if (!decodeTurn(value, result.state, error))
                return std::nullopt;
            if (const auto items = value.find("items"); items != value.end()) {
                for (const frontend::Json& itemValue : *items) {
                    ItemState item;
                    if (!decodeLegacyItem(itemValue, item, error, result.state.threadId, result.state.id))
                        return std::nullopt;
                    if (!item.threadId || *item.threadId != result.state.threadId || !item.turnId || *item.turnId != result.state.id) {
                        error = "turn result item does not belong to its containing turn";
                        return std::nullopt;
                    }
                    if (!appendDistinct(
                            result.items,
                            std::move(item),
                            [](const ItemState& candidate) {
                                return candidate.id;
                            },
                            "turn result item collection",
                            error))
                        return std::nullopt;
                }
            }
            if (result.state.orderedItems.size() != result.items.size()) {
                error = "turn result item ordering does not match its typed item collection";
                return std::nullopt;
            }
            for (std::size_t index = 0; index < result.items.size(); ++index) {
                if (result.state.orderedItems[index] != result.items[index].id) {
                    error = "turn result item ordering does not match its typed item collection";
                    return std::nullopt;
                }
            }
            return result;
        }

        std::optional<ThreadResultState> StateReducer::decodeThreadResultState(const frontend::Json& value, std::string& error) {
            ThreadResultState result;
            if (!decodeThread(value, result.state, error))
                return std::nullopt;
            if (const auto turns = value.find("turns"); turns != value.end()) {
                for (const frontend::Json& turnValue : *turns) {
                    TurnResultState turn;
                    if (!decodeTurn(turnValue, turn.state, error, result.state.id))
                        return std::nullopt;
                    if (turn.state.threadId != result.state.id) {
                        error = "thread result turn does not belong to its containing thread";
                        return std::nullopt;
                    }
                    if (const auto items = turnValue.find("items"); items != turnValue.end()) {
                        for (const frontend::Json& itemValue : *items) {
                            ItemState item;
                            if (!decodeLegacyItem(itemValue, item, error, result.state.id, turn.state.id))
                                return std::nullopt;
                            if (!item.threadId || *item.threadId != result.state.id || !item.turnId || *item.turnId != turn.state.id) {
                                error = "thread result item does not belong to its containing turn";
                                return std::nullopt;
                            }
                            if (!appendDistinct(
                                    turn.items,
                                    std::move(item),
                                    [](const ItemState& candidate) {
                                        return candidate.id;
                                    },
                                    "thread result item collection",
                                    error))
                                return std::nullopt;
                        }
                    }
                    if (turn.state.orderedItems.size() != turn.items.size()) {
                        error = "thread result item ordering does not match its typed item collection";
                        return std::nullopt;
                    }
                    for (std::size_t index = 0; index < turn.items.size(); ++index) {
                        if (turn.state.orderedItems[index] != turn.items[index].id) {
                            error = "thread result item ordering does not match its typed item collection";
                            return std::nullopt;
                        }
                    }
                    if (!appendDistinct(
                            result.turns,
                            std::move(turn),
                            [](const TurnResultState& candidate) {
                                return candidate.state.id;
                            },
                            "thread result turn collection",
                            error))
                        return std::nullopt;
                }
            }
            if (result.state.orderedTurns.size() != result.turns.size()) {
                error = "thread result turn ordering does not match its typed turn collection";
                return std::nullopt;
            }
            for (std::size_t index = 0; index < result.turns.size(); ++index) {
                if (result.state.orderedTurns[index] != result.turns[index].state.id) {
                    error = "thread result turn ordering does not match its typed turn collection";
                    return std::nullopt;
                }
            }
            return result;
        }
    } // namespace detail

} // namespace ai::openai::codex::frontend::client
