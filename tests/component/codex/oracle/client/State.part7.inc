                            !setSectionJson(state,
                                            detail::StateSizeSection::PermissionProfiles,
                                            encodeProjected(state.permissionProfiles, domainProjection)))
                            return accountingFailure(state, error);
                        changes.push_back(ReviewsUpdatedChange{});
                    } else if (event.type == IntegrationsUpdated) {
                        if (!decodeDomainWrapper(*value, state.apps, error))
                            return false;
                        state.externalAgents.value = ExternalAgentsState{state.apps.value->projection, state.apps.value->details};
                        state.hooks.value = HooksState{state.apps.value->projection, state.apps.value->details};
                        state.marketplace.value = MarketplaceState{state.apps.value->projection, state.apps.value->details};
                        state.externalAgents.truncated = state.hooks.truncated = state.marketplace.truncated = state.apps.truncated;
                        state.externalAgents.omittedFields = state.hooks.omittedFields = state.marketplace.omittedFields =
                            state.apps.omittedFields;
                        if (!setSectionJson(state, detail::StateSizeSection::Apps, encodeProjected(state.apps, domainProjection)) ||
                            !setSectionJson(
                                state, detail::StateSizeSection::ExternalAgents, encodeProjected(state.externalAgents, domainProjection)) ||
                            !setSectionJson(state, detail::StateSizeSection::Hooks, encodeProjected(state.hooks, domainProjection)) ||
                            !setSectionJson(
                                state, detail::StateSizeSection::Marketplace, encodeProjected(state.marketplace, domainProjection)))
                            return accountingFailure(state, error);
                        changes.push_back(IntegrationsUpdatedChange{});
                    } else if (event.type == PluginsUpdated) {
                        if (!decodeDomainWrapper(*value, state.plugins, error))
                            return false;
                        state.skills.value = SkillsState{state.plugins.value->projection, state.plugins.value->details};
                        state.skills.truncated = state.plugins.truncated;
                        state.skills.omittedFields = state.plugins.omittedFields;
                        if (!setSectionJson(state, detail::StateSizeSection::Plugins, encodeProjected(state.plugins, domainProjection)) ||
                            !setSectionJson(state, detail::StateSizeSection::Skills, encodeProjected(state.skills, domainProjection)))
                            return accountingFailure(state, error);
                        changes.push_back(PluginsUpdatedChange{});
                    } else if (event.type == SkillsUpdated) {
                        if (!decodeDomainWrapper(*value, state.skills, error))
                            return false;
                        state.plugins.value = PluginsState{state.skills.value->projection, state.skills.value->details};
                        state.plugins.truncated = state.skills.truncated;
                        state.plugins.omittedFields = state.skills.omittedFields;
                        if (!setSectionJson(state, detail::StateSizeSection::Skills, encodeProjected(state.skills, domainProjection)) ||
                            !setSectionJson(state, detail::StateSizeSection::Plugins, encodeProjected(state.plugins, domainProjection)))
                            return accountingFailure(state, error);
                        changes.push_back(SkillsUpdatedChange{});
                    } else if (event.type == McpUpdated) {
                        if (!decodeDomainWrapper(*value, state.mcp, error))
                            return false;
                        if (!setSectionJson(state, detail::StateSizeSection::Mcp, encodeProjected(state.mcp, domainProjection)))
                            return accountingFailure(state, error);
                        changes.push_back(McpUpdatedChange{});
                    } else {
                        if (!decodeDomainWrapper(*value, state.platform, error))
                            return false;
                        state.windowsSandbox.value = WindowsSandboxState{state.platform.value->projection, state.platform.value->details};
                        state.windowsSandbox.truncated = state.platform.truncated;
                        state.windowsSandbox.omittedFields = state.platform.omittedFields;
                        if (!setSectionJson(state, detail::StateSizeSection::Platform, encodeProjected(state.platform, domainProjection)) ||
                            !setSectionJson(
                                state, detail::StateSizeSection::WindowsSandbox, encodeProjected(state.windowsSandbox, domainProjection)))
                            return accountingFailure(state, error);
                        changes.push_back(PlatformUpdatedChange{});
                    }
                    return true;
                }
                case ProcessUpdated: {
                    const frontend::Json* value = nullptr;
                    ProcessState decoded;
                    if (!requireObjectMember(data, "process", value, error) || !decodeProcess(*value, decoded, error))
                        return false;
                    if (!state.processes.value)
                        state.processes.value.emplace();
                    const ProcessHandle handle = decoded.processHandle;
                    const auto prior = std::find_if(
                        state.processes.value->entries.begin(), state.processes.value->entries.end(), [&](const ProcessState& process) {
                            return process.processHandle == handle;
                        });
                    const std::optional<std::size_t> oldBytes =
                        prior == state.processes.value->entries.end()
                            ? std::nullopt
                            : std::optional<std::size_t>{encodedEntityBytes(encodeProcessState(*prior))};
                    const std::optional<std::size_t> newBytes = encodedEntityBytes(encodeProcessState(decoded));
                    upsert(state.processes.value->entries, std::move(decoded), [](const ProcessState& process) {
                        return process.processHandle;
                    });
                    if (!newBytes || !accountProjectedEntityMutation(state,
                                                                     detail::StateSizeSection::Processes,
                                                                     state.sizeLedger.processes,
                                                                     state.processes,
                                                                     oldBytes,
                                                                     newBytes,
                                                                     error))
                        return accountingFailure(state, error);
                    changes.push_back(ProcessUpdatedChange{handle});
                    return true;
                }
                case FilesystemWatchUpdated: {
                    const frontend::Json* value = nullptr;
                    FilesystemWatchState decoded;
                    if (!requireObjectMember(data, "filesystemWatch", value, error) || !decodeWatch(*value, decoded, error))
                        return false;
                    if (!state.filesystemWatches.value)
                        state.filesystemWatches.value.emplace();
                    const typed::FsWatchId id = decoded.watchId;
                    const auto prior = std::find_if(state.filesystemWatches.value->entries.begin(),
                                                    state.filesystemWatches.value->entries.end(),
                                                    [&](const FilesystemWatchState& watch) {
                                                        return watch.watchId == id;
                                                    });
                    const std::optional<std::size_t> oldBytes =
                        prior == state.filesystemWatches.value->entries.end()
                            ? std::nullopt
                            : std::optional<std::size_t>{encodedEntityBytes(encodeFilesystemWatchState(*prior))};
                    const std::optional<std::size_t> newBytes = encodedEntityBytes(encodeFilesystemWatchState(decoded));
                    upsert(state.filesystemWatches.value->entries, std::move(decoded), [](const FilesystemWatchState& watch) {
                        return watch.watchId;
                    });
                    if (!newBytes || !accountProjectedEntityMutation(state,
                                                                     detail::StateSizeSection::FilesystemWatches,
                                                                     state.sizeLedger.filesystemWatches,
                                                                     state.filesystemWatches,
                                                                     oldBytes,
                                                                     newBytes,
                                                                     error))
                        return accountingFailure(state, error);
                    changes.push_back(FilesystemWatchUpdatedChange{id});
                    return true;
                }
                case FuzzySearchUpdated: {
                    const frontend::Json* value = nullptr;
                    FuzzySearchState decoded;
                    if (!requireObjectMember(data, "fuzzySearch", value, error) || !decodeSearch(*value, decoded, error))
                        return false;
                    if (!state.fuzzySearches.value)
                        state.fuzzySearches.value.emplace();
                    const FuzzySearchSessionId id = decoded.sessionId;
                    const auto prior = std::find_if(state.fuzzySearches.value->entries.begin(),
                                                    state.fuzzySearches.value->entries.end(),
                                                    [&](const FuzzySearchState& search) {
                                                        return search.sessionId == id;
                                                    });
                    const std::optional<std::size_t> oldBytes =
                        prior == state.fuzzySearches.value->entries.end()
                            ? std::nullopt
                            : std::optional<std::size_t>{encodedEntityBytes(encodeFuzzySearchState(*prior))};
                    const std::optional<std::size_t> newBytes = encodedEntityBytes(encodeFuzzySearchState(decoded));
                    upsert(state.fuzzySearches.value->entries, std::move(decoded), [](const FuzzySearchState& search) {
                        return search.sessionId;
                    });
                    if (!newBytes || !accountProjectedEntityMutation(state,
                                                                     detail::StateSizeSection::FuzzySearches,
                                                                     state.sizeLedger.fuzzySearches,
                                                                     state.fuzzySearches,
                                                                     oldBytes,
                                                                     newBytes,
                                                                     error))
                        return accountingFailure(state, error);
                    changes.push_back(FuzzySearchUpdatedChange{id});
                    return true;
                }
                case NoticeAdded: {
                    const frontend::Json* value = nullptr;
                    NoticeState decoded;
                    if (!requireObjectMember(data, "notice", value, error) || !decodeNotice(*value, decoded, error))
                        return false;
                    if (!state.notices.value)
                        state.notices.value.emplace();
                    const std::optional<std::size_t> evictedBytes =
                        state.notices.value->entries.size() >= MaximumRetainedNotices && !state.notices.value->entries.empty()
                            ? std::optional<std::size_t>{encodedEntityBytes(encodeNoticeState(state.notices.value->entries.front()))}
                            : std::nullopt;
                    const std::optional<std::size_t> appendedBytes = encodedEntityBytes(encodeNoticeState(decoded));
                    state.notices.value->entries.push_back(decoded);
                    if (state.notices.value->entries.size() > MaximumRetainedNotices) {
                        state.notices.value->entries.erase(state.notices.value->entries.begin());
                        state.notices.truncated = true;
                        state.notices.value->truncation.truncated = true;
                        std::size_t& omitted = state.notices.value->truncation.omittedEntries.emplace(
                            state.notices.value->truncation.omittedEntries.value_or(0));
                        if (omitted != std::numeric_limits<std::size_t>::max())
                            ++omitted;
                    }
                    if (!appendedBytes || !accountProjectedEntityMutation(state,
                                                                          detail::StateSizeSection::Notices,
                                                                          state.sizeLedger.notices,
                                                                          state.notices,
                                                                          evictedBytes,
                                                                          appendedBytes,
                                                                          error))
                        return accountingFailure(state, error);
                    changes.push_back(NoticeAddedChange{decoded.occurrence});
                    return true;
                }
                case ActivityUpdated: {
                    const frontend::Json* value = nullptr;
                    ActivityState decoded;
                    if (!requireObjectMember(data, "activity", value, error) || !decodeActivity(*value, decoded, error))
                        return false;
                    if (!state.activities.value)
                        state.activities.value.emplace();
                    const ActivityKey key = decoded.key;
                    const auto prior = std::find_if(
                        state.activities.value->entries.begin(), state.activities.value->entries.end(), [&](const ActivityState& activity) {
                            return activity.key == key;
                        });
                    const std::optional<std::size_t> oldBytes =
                        prior == state.activities.value->entries.end()
                            ? std::nullopt
                            : std::optional<std::size_t>{encodedEntityBytes(encodeActivityState(*prior))};
                    const std::optional<std::size_t> newBytes = encodedEntityBytes(encodeActivityState(decoded));
                    upsert(state.activities.value->entries, std::move(decoded), [](const ActivityState& activity) {
                        return activity.key;
                    });
                    if (!newBytes || !accountProjectedEntityMutation(state,
                                                                     detail::StateSizeSection::Activities,
                                                                     state.sizeLedger.activities,
                                                                     state.activities,
                                                                     oldBytes,
                                                                     newBytes,
                                                                     error))
                        return accountingFailure(state, error);
                    changes.push_back(ActivityUpdatedChange{key});
                    return true;
                }
                case CapacityUpdated: {
                    const frontend::Json* value = nullptr;
                    if (!requireObjectMember(data, "capacity", value, error))
                        return false;
                    state.capacity.value = decodeCapacity(*value);
                    if (!setSectionJson(state, detail::StateSizeSection::Capacity, encodeProjected(state.capacity, encodeCapacityState)))
                        return accountingFailure(state, error);
                    changes.push_back(CapacityUpdatedChange{});
                    return true;
                }
                case DiagnosticsUpdated: {
                    const frontend::Json* value = nullptr;
                    if (!requireObjectMember(data, "diagnostic", value, error))
                        return false;
                    DiagnosticState decoded = decodeDiagnostic(*value);
                    const auto received = decoded.received;
                    std::vector<std::size_t> evictedBytes;
                    if (state.diagnostics.value) {
                        const std::size_t oldCount = state.diagnostics.value->entries.size();
                        const std::size_t retainedAfterAppend = maximumRetainedDiagnostics;
                        const std::size_t removeCount = oldCount + 1 > retainedAfterAppend ? oldCount + 1 - retainedAfterAppend : 0;
                        const std::size_t removeOld = std::min(removeCount, oldCount);
                        evictedBytes.reserve(removeOld);
                        for (std::size_t index = 0; index < removeOld; ++index) {
                            const std::optional<std::size_t> bytes =
                                encodedEntityBytes(encodeDiagnosticState(state.diagnostics.value->entries[index]));
                            if (!bytes)
                                return accountingFailure(state, error);
                            evictedBytes.push_back(*bytes);
                        }
                    }
                    const std::optional<std::size_t> appendedBytes = encodedEntityBytes(encodeDiagnosticState(decoded));
                    appendDiagnostic(state, std::move(decoded), maximumRetainedDiagnostics);
                    for (const std::size_t bytes : evictedBytes) {
                        if (!adjustArrayContribution(state.sizeLedger.diagnostics, bytes, std::nullopt))
                            return accountingFailure(state, error);
                    }
                    const bool appendedRetained = maximumRetainedDiagnostics != 0;
                    if (!appendedBytes ||
                        (appendedRetained && !adjustArrayContribution(state.sizeLedger.diagnostics, std::nullopt, appendedBytes)) ||
                        !refreshProjectedCollectionSection(
                            state, detail::StateSizeSection::Diagnostics, state.diagnostics, state.sizeLedger.diagnostics))
                        return accountingFailure(state, error);
                    changes.push_back(DiagnosticUpdatedChange{received});
                    return true;
                }
            }
            error = "unknown expanded event family";
            return false;
        }

        bool applyLegacy(detail::StateStorage& state,
                         const frontend::FrontendEvent& event,
                         std::size_t maximumRetainedDiagnostics,
                         std::vector<Change>& changes,
                         std::string& error) {
            if (!validateLegacyEvent(event, error))
                return false;
            auto sanitized = sanitizeLegacyEventData(event, error);
            if (!sanitized)
                return false;
            const frontend::Json& data = *sanitized;
            if (!validateLegacyEvent(frontend::FrontendEvent{event.sequence, event.type, data, event.extensions}, error)) {
                error = "bounded legacy event no longer satisfies its stable schema: " + error;
                return false;
            }
            if (event.type == "thread.updated") {
                state.threadProjectionPresent = true;
                state.turnProjectionPresent = true;
                state.itemProjectionPresent = true;
                const frontend::Json* value = nullptr;
                ThreadState decoded;
                std::vector<TurnState> turns;
                std::vector<ItemState> items;
                if (!requireObjectMember(data, "thread", value, error) || !decodeCompleteLegacyThread(*value, decoded, turns, items, error))
                    return false;
                const typed::ThreadId id = decoded.id;
                if (!applyCompleteLegacyThread(state, std::move(decoded), std::move(turns), std::move(items), error))
                    return false;
                if (!rebuildDirectArraySection(
                        state, detail::StateSizeSection::Threads, state.sizeLedger.threads, state.threads, encodeThreadState) ||
                    !rebuildDirectArraySection(
                        state, detail::StateSizeSection::Turns, state.sizeLedger.turns, state.turns, encodeTurnState) ||
                    !rebuildDirectArraySection(
                        state, detail::StateSizeSection::Items, state.sizeLedger.items, state.items, encodeItemState))
                    return accountingFailure(state, error);
                changes.push_back(ThreadUpsertedChange{id});
                return true;
            }
            if (event.type == "thread.list.updated") {
                ThreadListState decoded;
                if (!decodeThreadList(data, decoded, error))
                    return false;
                state.threadList.value = std::move(decoded);
                if (!setSectionJson(state, detail::StateSizeSection::ThreadList, encodeProjected(state.threadList, encodeThreadListState)))
                    return accountingFailure(state, error);
                changes.push_back(ThreadListUpdatedChange{});
                return true;
            }
            if (event.type == "turn.updated") {
                state.turnProjectionPresent = true;
                state.itemProjectionPresent = true;
                const frontend::Json* value = nullptr;
                TurnState decoded;
                std::vector<ItemState> items;
                if (!requireObjectMember(data, "turn", value, error) ||
                    !decodeCompleteLegacyTurn(*value, std::nullopt, decoded, items, error))
                    return false;
                const typed::TurnId id = decoded.id;
                if (!applyCompleteLegacyTurn(state, std::move(decoded), std::move(items), error))
                    return false;
                if (!rebuildDirectArraySection(
                        state, detail::StateSizeSection::Threads, state.sizeLedger.threads, state.threads, encodeThreadState) ||
                    !rebuildDirectArraySection(
                        state, detail::StateSizeSection::Turns, state.sizeLedger.turns, state.turns, encodeTurnState) ||
                    !rebuildDirectArraySection(
                        state, detail::StateSizeSection::Items, state.sizeLedger.items, state.items, encodeItemState))
                    return accountingFailure(state, error);
                changes.push_back(TurnUpsertedChange{id});
                return true;
            }
            if (event.type == "item.updated") {
                state.itemProjectionPresent = true;
                const frontend::Json* value = nullptr;
                if (!requireObjectMember(data, "item", value, error))
                    return false;
                ItemState decoded;
                const auto threadId = stringMember(data, "threadId");
                const auto turnId = stringMember(data, "turnId");
                if (!decodeLegacyItem(*value,
                                      decoded,
                                      error,
                                      threadId ? std::optional(typed::ThreadId{*threadId}) : std::nullopt,
                                      turnId ? std::optional(typed::TurnId{*turnId}) : std::nullopt))
                    return false;
                const typed::ItemId id = decoded.id;
                const std::optional<typed::TurnId> newTurn = decoded.turnId;
                const auto prior = std::find_if(state.items.begin(), state.items.end(), [&](const ItemState& item) {
                    return item.id == id;
                });
                const std::optional<std::size_t> oldItemBytes =
                    prior == state.items.end() ? std::nullopt : std::optional<std::size_t>{encodedEntityBytes(encodeItemState(*prior))};
                const std::optional<typed::TurnId> priorTurn = prior == state.items.end() ? std::nullopt : prior->turnId;
                const std::optional<std::size_t> newItemBytes = encodedEntityBytes(encodeItemState(decoded));
                const auto turnBytes = [&state](const typed::TurnId& parentId) {
                    return encodedEntityBytesById(
                        state.turns,
                        parentId,
                        [](const TurnState& turn) {
                            return turn.id;
                        },
                        encodeTurnState);
                };
                const std::optional<std::size_t> oldPriorTurnBytes = priorTurn ? turnBytes(*priorTurn) : std::nullopt;
                const std::optional<std::size_t> oldNewTurnBytes =
                    newTurn && priorTurn && *newTurn == *priorTurn ? oldPriorTurnBytes : (newTurn ? turnBytes(*newTurn) : std::nullopt);
                upsert(state.items, std::move(decoded), [](const ItemState& item) {
                    return item.id;
                });
                if (priorTurn && priorTurn != newTurn) {
                    const auto oldTurn = std::find_if(state.turns.begin(), state.turns.end(), [&](const TurnState& turn) {
                        return turn.id == *priorTurn;
                    });
                    if (oldTurn != state.turns.end())
                        std::erase(oldTurn->orderedItems, id);
                }
                if (newTurn) {
                    const auto parentTurn = std::find_if(state.turns.begin(), state.turns.end(), [&](const TurnState& turn) {
                        return turn.id == *newTurn;
                    });
                    if (parentTurn != state.turns.end())
                        appendUnique(parentTurn->orderedItems, id);
                }
                const std::optional<std::size_t> newPriorTurnBytes = priorTurn ? turnBytes(*priorTurn) : std::nullopt;
                const std::optional<std::size_t> newNewTurnBytes =
                    newTurn && priorTurn && *newTurn == *priorTurn ? newPriorTurnBytes : (newTurn ? turnBytes(*newTurn) : std::nullopt);
                if (!newItemBytes ||
                    !accountDirectEntityMutation(
                        state, detail::StateSizeSection::Items, state.sizeLedger.items, oldItemBytes, newItemBytes, error) ||
                    (priorTurn &&
                     !accountDirectEntityMutation(
                         state, detail::StateSizeSection::Turns, state.sizeLedger.turns, oldPriorTurnBytes, newPriorTurnBytes, error)) ||
                    (newTurn && (!priorTurn || *newTurn != *priorTurn) &&
                     !accountDirectEntityMutation(
                         state, detail::StateSizeSection::Turns, state.sizeLedger.turns, oldNewTurnBytes, newNewTurnBytes, error)))
                    return accountingFailure(state, error);
                changes.push_back(ItemUpsertedChange{id});
                return true;
            }
            if (event.type == "item.content.updated") {
                state.itemProjectionPresent = true;
                const auto idValue = stringMember(data, "itemId");
                const std::optional<std::size_t> oldBytes = idValue ? encodedEntityBytesById(
                                                                          state.items,
                                                                          typed::ItemId{*idValue},
                                                                          [](const ItemState& item) {
                                                                              return item.id;
                                                                          },
                                                                          encodeItemState)
                                                                    : std::nullopt;
                ItemContentReplacedChange change;
                if (!replaceContent(state, data, change, error))
                    return false;
                const std::optional<std::size_t> newBytes = encodedEntityBytesById(
                    state.items,
                    change.itemId,
                    [](const ItemState& item) {
                        return item.id;
                    },
                    encodeItemState);
                if (!oldBytes || !newBytes ||
                    !accountDirectEntityMutation(state, detail::StateSizeSection::Items, state.sizeLedger.items, oldBytes, newBytes, error))
                    return accountingFailure(state, error);
                changes.push_back(std::move(change));
                return true;
            }
            if (event.type == "request.pending") {
                state.pendingRequestProjectionPresent = true;
                const frontend::Json* value = nullptr;
                if (!requireObjectMember(data, "request", value, error))
                    return false;
                PendingRequestState decoded;
                if (!decodePendingRequestJson(*value, decoded, error, false))
                    return false;
                const std::optional<std::size_t> oldBytes = encodedEntityBytesById(
                    state.pendingRequests,
                    decoded.id,
                    [](const PendingRequestState& request) {
                        return request.id;
                    },
                    encodePendingRequestState);
                const std::optional<std::size_t> newBytes = encodedEntityBytes(encodePendingRequestState(decoded));
                upsert(state.pendingRequests, std::move(decoded), [](const PendingRequestState& request) {
                    return request.id;
                });
                if (!newBytes ||
                    !accountDirectEntityMutation(
                        state, detail::StateSizeSection::PendingRequests, state.sizeLedger.pendingRequests, oldBytes, newBytes, error))
                    return accountingFailure(state, error);
                changes.push_back(PendingRequestsUpdatedChange{});
                return true;
            }
            if (event.type == "request.resolved") {
                state.pendingRequestProjectionPresent = true;
                const auto id = stringMember(data, "pendingRequestId");
                if (!id || id->empty()) {
                    error = "request resolution lacks pendingRequestId";
                    return false;
                }
                const std::optional<std::size_t> oldBytes = encodedEntityBytesById(
                    state.pendingRequests,
                    PendingRequestId{*id},
                    [](const PendingRequestState& request) {
                        return request.id;
                    },
                    encodePendingRequestState);
                std::erase_if(state.pendingRequests, [&](const PendingRequestState& request) {
                    return request.id.value == *id;
                });
                if (oldBytes &&
                    !accountDirectEntityMutation(
                        state, detail::StateSizeSection::PendingRequests, state.sizeLedger.pendingRequests, oldBytes, std::nullopt, error))
                    return accountingFailure(state, error);
                changes.push_back(PendingRequestsUpdatedChange{});
                return true;
            }
            if (event.type == "controller.changed") {
                frontend::Json value = frontend::Json::object();
                if (const auto id = stringMember(data, "controllerSessionId")) {
                    value["controllerSessionId"] = *id;
                    value["present"] = true;
                } else {
                    value["present"] = false;
                }
                ControllerState decoded;
                if (!decodeController(value, state.session, decoded, error))
                    return false;
                state.controller.value = decoded;
                if (!setSectionJson(state, detail::StateSizeSection::Controller, encodeProjected(state.controller, encodeControllerState)))
                    return accountingFailure(state, error);
                changes.push_back(ControllerUpdatedChange{std::move(decoded)});
                return true;
            }
            if (event.type == "session.changed") {
                SessionState decoded;
                if (!decodeSession(data, decoded, error))
                    return false;
                if (!state.sessions.value)
                    state.sessions.value.emplace();
                if (*optionalBool(data, "connected")) {
                    upsert(*state.sessions.value, std::move(decoded), [](const SessionState& session) {
                        return session.sessionId;
                    });
                } else {
                    std::erase_if(*state.sessions.value, [&](const SessionState& session) {
                        return session.sessionId == decoded.sessionId;
                    });
                }
                if (!refreshProjectedSessionsSection(state))
                    return accountingFailure(state, error);
                changes.push_back(SessionsUpdatedChange{});
                return true;
            }
            if (event.type == "backend.lifecycle.changed") {
                frontend::Json value = frontend::Json::object();
                value["lifecycle"] = *stringMember(data, "lifecycle");
                if (const auto legacyError = data.find("error"); legacyError != data.end())
                    value["lastError"] = *legacyError;
                ProviderState decoded;
                if (!decodeProvider(value, decoded, error, false)) {
                    error = "legacy provider lifecycle event lacks a decodable provider state";
                    return false;
                }
                state.provider.value = decoded;
                if (!setSectionJson(state, detail::StateSizeSection::Provider, encodeProjected(state.provider, encodeProvider)))
                    return accountingFailure(state, error);
                changes.push_back(ProviderUpdatedChange{std::move(decoded)});
                return true;
            }
            if (event.type == "diagnostics.updated") {
                const std::uint64_t received = *optionalUnsigned(data, "received");
                std::vector<DiagnosticState> decoded;
                for (const frontend::Json& message : data.at("recent")) {
                    DiagnosticState diagnostic;
                    diagnostic.received = received;
                    diagnostic.message = message.get<std::string>();
                    decoded.push_back(std::move(diagnostic));
                }
                if (decoded.size() > maximumRetainedDiagnostics) {
                    decoded.erase(decoded.begin(), decoded.end() - static_cast<std::ptrdiff_t>(maximumRetainedDiagnostics));
                    state.diagnostics.truncated = true;
                }
                if (!state.diagnostics.value)
                    state.diagnostics.value.emplace();
                state.diagnostics.value->received = received;
                state.diagnostics.value->entries = std::move(decoded);
                if (!rebuildProjectedCollectionSection(state,
                                                       detail::StateSizeSection::Diagnostics,
                                                       state.sizeLedger.diagnostics,
                                                       state.diagnostics,
                                                       encodeDiagnosticState))
                    return accountingFailure(state, error);
                changes.push_back(DiagnosticUpdatedChange{received});
                return true;
            }
            if (event.type == "codex.extension") {
                frontend::Json& extensions = state.compatibilityExtensions["codexExtensions"];
                if (!extensions.is_array())
                    extensions = frontend::Json::array();
                if (extensions.size() == MaximumRetainedCompatibilityExtensions) {
                    extensions.erase(extensions.begin());
                    if (!state.truncation.value)
                        state.truncation.value.emplace();
                    state.truncation.value->truncated = true;
                    state.truncation.truncated = true;
                    const std::size_t omitted = state.truncation.value->omittedEntries.value_or(0);
                    state.truncation.value->omittedEntries = omitted == std::numeric_limits<std::size_t>::max() ? omitted : omitted + 1;
                }
                extensions.push_back(data);
                if (!setSectionJson(state, detail::StateSizeSection::CompatibilityExtensions, state.compatibilityExtensions) ||
                    !setSectionJson(state, detail::StateSizeSection::Truncation, encodeProjected(state.truncation, encodeTruncation)))
                    return accountingFailure(state, error);
                changes.push_back(CompatibilityExtensionChange{event.type});
                return true;
            }
            error = "unknown legacy frontend event type '" + event.type + "'";
            return false;
        }

        frontend::Json serializeChanges(std::span<const Change> changes) {
            frontend::Json result = frontend::Json::array();
            for (const Change& change : changes) {
                frontend::Json encoded = frontend::Json::object();
                std::visit(
                    [&encoded](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, StateReplacedChange>) {
                            encoded["type"] = "state.replaced";
                        } else if constexpr (std::is_same_v<T, CursorAdvancedChange>) {
                            encoded["type"] = "cursor.advanced";
                            encoded["sequence"] = value.sequence.value();
                        } else if constexpr (std::is_same_v<T, ProviderUpdatedChange>) {
                            encoded["type"] = "provider.updated";
                        } else if constexpr (std::is_same_v<T, ControllerUpdatedChange>) {
                            encoded["type"] = "controller.updated";
                        } else if constexpr (std::is_same_v<T, SessionsUpdatedChange>) {
                            encoded["type"] = "sessions.updated";
                        } else if constexpr (std::is_same_v<T, ThreadListUpdatedChange>) {
                            encoded["type"] = "threadList.updated";
                        } else if constexpr (std::is_same_v<T, ThreadUpsertedChange>) {
                            encoded["type"] = "thread.upserted";
                            encoded["threadId"] = value.threadId.value;
                        } else if constexpr (std::is_same_v<T, ThreadRemovedChange>) {
                            encoded["type"] = "thread.removed";
                            encoded["threadId"] = value.threadId.value;
                        } else if constexpr (std::is_same_v<T, TurnUpsertedChange>) {
                            encoded["type"] = "turn.upserted";
                            encoded["turnId"] = value.turnId.value;
                        } else if constexpr (std::is_same_v<T, ItemUpsertedChange>) {
                            encoded["type"] = "item.upserted";
                            encoded["itemId"] = value.itemId.value;
                        } else if constexpr (std::is_same_v<T, ItemContentReplacedChange>) {
                            encoded["type"] = "item.content.replaced";
                            encoded["itemId"] = value.itemId.value;
                            encoded["channel"] = std::string(toString(value.channel));
                        } else if constexpr (std::is_same_v<T, PendingRequestsUpdatedChange>) {
                            encoded["type"] = "pendingRequests.updated";
                        } else if constexpr (std::is_same_v<T, AccountUpdatedChange>) {
                            encoded["type"] = "account.updated";
                        } else if constexpr (std::is_same_v<T, ModelsUpdatedChange>) {
                            encoded["type"] = "models.updated";
                        } else if constexpr (std::is_same_v<T, ConfigurationUpdatedChange>) {
                            encoded["type"] = "configuration.updated";
                        } else if constexpr (std::is_same_v<T, ProcessUpdatedChange>) {
                            encoded["type"] = "process.updated";
                            encoded["processHandle"] = value.processHandle.value;
                        } else if constexpr (std::is_same_v<T, FilesystemWatchUpdatedChange>) {
                            encoded["type"] = "filesystemWatch.updated";
                            encoded["watchId"] = value.watchId.value;
                        } else if constexpr (std::is_same_v<T, FuzzySearchUpdatedChange>) {
                            encoded["type"] = "fuzzySearch.updated";
                            encoded["sessionId"] = value.sessionId.value;
                        } else if constexpr (std::is_same_v<T, ReviewsUpdatedChange>) {
                            encoded["type"] = "reviews.updated";
                        } else if constexpr (std::is_same_v<T, IntegrationsUpdatedChange>) {
                            encoded["type"] = "integrations.updated";
                        } else if constexpr (std::is_same_v<T, PluginsUpdatedChange>) {
                            encoded["type"] = "plugins.updated";
                        } else if constexpr (std::is_same_v<T, SkillsUpdatedChange>) {
                            encoded["type"] = "skills.updated";
                        } else if constexpr (std::is_same_v<T, McpUpdatedChange>) {
                            encoded["type"] = "mcp.updated";
                        } else if constexpr (std::is_same_v<T, PlatformUpdatedChange>) {
                            encoded["type"] = "platform.updated";
                        } else if constexpr (std::is_same_v<T, NoticeAddedChange>) {
                            encoded["type"] = "notice.added";
                            addOptional(encoded, "occurrence", value.occurrence);
                        } else if constexpr (std::is_same_v<T, ActivityUpdatedChange>) {
                            encoded["type"] = "activity.updated";
                            encoded["key"] = value.key.value;
                        } else if constexpr (std::is_same_v<T, CapacityUpdatedChange>) {
                            encoded["type"] = "capacity.updated";
                        } else if constexpr (std::is_same_v<T, DiagnosticUpdatedChange>) {
                            encoded["type"] = "diagnostics.updated";
                            addOptional(encoded, "received", value.received);
                        } else if constexpr (std::is_same_v<T, CompatibilityExtensionChange>) {
                            encoded["type"] = "compatibility.extension";
                            encoded["eventType"] = value.type;
                        }
                    },
                    change);
                result.push_back(std::move(encoded));
            }
            return result;
        }

        frontend::Json serializeFixtureState(const detail::StateStorage& state) {
            frontend::Json result = encodeState(state);
            switch (state.freshness) {
                case StateFreshness::Current:
                    result["freshness"] = "current";
                    break;
                case StateFreshness::Stale:
                    result["freshness"] = "stale";
                    break;
                case StateFreshness::Synchronizing:
                    result["freshness"] = "synchronizing";
                    break;
            }
            switch (state.representationMode) {
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
                if (found == result.end() || !found->is_object())
                    continue;
                if (!found->value("present", false)) {
                    result.erase(found);
                    continue;
                }
                found->erase("present");
                if (!found->value("truncated", false))
                    found->erase("truncated");
                const auto omitted = found->find("omittedFields");
                if (omitted != found->end() && omitted->is_array() && omitted->empty())
                    found->erase(omitted);
            }
            if (auto session = result.find("session"); session != result.end() && session->is_object()) {
                for (const char* name : {"requestedRepresentationCapabilities",
                                         "selectedRepresentationCapabilities",
                                         "observedMechanismCapabilities",
                                         "observedTopologyCapabilities",
                                         "observedProductCapabilities"}) {
                    const auto value = session->find(name);
                    if (value != session->end() && value->is_array() && value->empty())
                        session->erase(value);
                }
                for (const char* name : {"availableMethods", "permittedMethods", "permittedScopes"}) {
                    const auto value = session->find(name);
                    if (value != session->end() && value->is_object() && !value->value("present", false))
                        session->erase(value);
                }
            }
            if (const auto extensions = result.find("compatibilityExtensions");
                extensions != result.end() && extensions->is_object() && extensions->empty())
                result.erase(extensions);
            return result;
        }
    } // namespace

    State::State()
        : impl([] {
            auto storage = std::make_shared<detail::StateStorage>();
            std::string accountingError;
            if (!rebuildStateSizeLedger(*storage) || !stateFits(*storage, std::numeric_limits<std::size_t>::max(), accountingError))
                storage->sizeLedger.failed = true;
            return std::shared_ptr<const detail::StateStorage>{std::move(storage)};
        }()) {
    }

    State::State(std::shared_ptr<const detail::StateStorage> implementation) noexcept
        : impl(std::move(implementation)) {
    }

    std::uint64_t State::revision() const noexcept {
        return impl->revision;
    }
    StateFreshness State::freshness() const noexcept {
        return impl->freshness;
    }
    RepresentationMode State::representationMode() const noexcept {
        return impl->representationMode;
    }
    std::optional<frontend::SequenceNumber> State::visibleSequence() const noexcept {
        return impl->visibleSequence;
    }
    std::optional<frontend::SequenceNumber> State::synchronizedThrough() const noexcept {
        return impl->synchronizedThrough;
    }
    std::optional<SessionInfo> State::session() const {
        return impl->session;
    }
    const BackendCursorState& State::backendCursor() const noexcept {
        return impl->backendCursor;
    }
    const ProjectionMetadataState& State::projectionMetadata() const noexcept {
        return impl->projectionMetadata;
    }
    const std::optional<ProjectionFingerprintMetadata>& State::projectionFingerprintMetadata() const noexcept {
        return impl->projectionFingerprint;
    }
    const Projected<ProviderState>& State::provider() const noexcept {
        return impl->provider;
    }
    const Projected<ControllerState>& State::controller() const noexcept {
        return impl->controller;
    }
    const Projected<std::vector<SessionState>>& State::sessions() const noexcept {
        return impl->sessions;
    }
    const SessionState* State::session(const FrontendSessionId& id) const noexcept {
        return session(id.value);
    }
    const SessionState* State::session(std::string_view id) const noexcept {
        if (!impl->sessions.value)
            return nullptr;
        const auto found = std::find_if(impl->sessions.value->begin(), impl->sessions.value->end(), [id](const SessionState& value) {
            return value.sessionId.value == id;
        });
        return found == impl->sessions.value->end() ? nullptr : &*found;
    }
    const Projected<ThreadListState>& State::threadList() const noexcept {
        return impl->threadList;
    }
    bool State::hasThreadProjection() const noexcept {
        return impl->threadProjectionPresent;
    }
    std::span<const ThreadState> State::threads() const noexcept {
        return impl->threads;
    }
    bool State::hasTurnProjection() const noexcept {
        return impl->turnProjectionPresent;
    }
    std::span<const TurnState> State::turns() const noexcept {
        return impl->turns;
    }
    bool State::hasItemProjection() const noexcept {
        return impl->itemProjectionPresent;
    }
    std::span<const ItemState> State::items() const noexcept {
        return impl->items;
    }
    bool State::hasPendingRequestProjection() const noexcept {
        return impl->pendingRequestProjectionPresent;
    }
    std::span<const PendingRequestState> State::pendingRequests() const noexcept {
        return impl->pendingRequests;
    }
    const ThreadState* State::thread(const typed::ThreadId& id) const noexcept {
        return thread(id.value);
    }
    const ThreadState* State::thread(std::string_view id) const noexcept {
        const auto found = std::find_if(impl->threads.begin(), impl->threads.end(), [id](const ThreadState& value) {
            return value.id.value == id;
        });
        return found == impl->threads.end() ? nullptr : &*found;
    }
    const TurnState* State::turn(const typed::TurnId& id) const noexcept {
        return turn(id.value);
    }
    const TurnState* State::turn(std::string_view id) const noexcept {
        const auto found = std::find_if(impl->turns.begin(), impl->turns.end(), [id](const TurnState& value) {
            return value.id.value == id;
        });
        return found == impl->turns.end() ? nullptr : &*found;
    }
    const ItemState* State::item(const typed::ItemId& id) const noexcept {
        return item(id.value);
    }
    const ItemState* State::item(std::string_view id) const noexcept {
        const auto found = std::find_if(impl->items.begin(), impl->items.end(), [id](const ItemState& value) {
            return value.id.value == id;
        });
        return found == impl->items.end() ? nullptr : &*found;
    }
    const PendingRequestState* State::pendingRequest(const PendingRequestId& id) const noexcept {
        const auto found = std::find_if(impl->pendingRequests.begin(), impl->pendingRequests.end(), [&](const PendingRequestState& value) {
            return value.id == id;
        });
        return found == impl->pendingRequests.end() ? nullptr : &*found;
    }

#define AISUITE_STATE_GETTER(type, name)                                                                                                   \
    const Projected<type>& State::name() const noexcept {                                                                                  \
        return impl->name;                                                                                                                 \
    }
    AISUITE_STATE_GETTER(AccountState, accounts)
    AISUITE_STATE_GETTER(ModelsState, models)
    AISUITE_STATE_GETTER(ConfigurationState, configuration)
    AISUITE_STATE_GETTER(ProcessCollectionState, processes)
    AISUITE_STATE_GETTER(FilesystemWatchCollectionState, filesystemWatches)
    AISUITE_STATE_GETTER(FuzzySearchCollectionState, fuzzySearches)
    AISUITE_STATE_GETTER(PermissionProfilesState, permissionProfiles)
    AISUITE_STATE_GETTER(ReviewsState, reviews)
    AISUITE_STATE_GETTER(AppsState, apps)
    AISUITE_STATE_GETTER(ExternalAgentsState, externalAgents)
    AISUITE_STATE_GETTER(HooksState, hooks)
    AISUITE_STATE_GETTER(MarketplaceState, marketplace)
    AISUITE_STATE_GETTER(PluginsState, plugins)
    AISUITE_STATE_GETTER(SkillsState, skills)
    AISUITE_STATE_GETTER(McpState, mcp)
    AISUITE_STATE_GETTER(WindowsSandboxState, windowsSandbox)
    AISUITE_STATE_GETTER(PlatformState, platform)
    AISUITE_STATE_GETTER(NoticeCollectionState, notices)
    AISUITE_STATE_GETTER(ActivityCollectionState, activities)
    AISUITE_STATE_GETTER(CapacityState, capacity)
