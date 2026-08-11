
            replaceOrderedSubset(state.items, std::move(items), [&](const ItemState& candidate) {
                return (candidate.turnId &&
                        std::find(affectedTurnIds.begin(), affectedTurnIds.end(), *candidate.turnId) != affectedTurnIds.end()) ||
                       std::find(itemIds.begin(), itemIds.end(), candidate.id) != itemIds.end();
            });
            replaceOrderedSubset(state.turns, std::move(turns), [&](const TurnState& candidate) {
                return candidate.threadId == threadId || std::find(turnIds.begin(), turnIds.end(), candidate.id) != turnIds.end();
            });
            upsert(state.threads, std::move(thread), [](const ThreadState& candidate) {
                return candidate.id;
            });
            (void) error;
            return true;
        }

        void trimDiagnostics(Projected<DiagnosticCollectionState>& diagnostics, std::size_t maximum) {
            if (!diagnostics.value)
                return;
            if (maximum == 0) {
                diagnostics.truncated = diagnostics.truncated || !diagnostics.value->entries.empty();
                diagnostics.value->entries.clear();
                return;
            }
            if (diagnostics.value->entries.size() > maximum) {
                const std::size_t remove = diagnostics.value->entries.size() - maximum;
                diagnostics.value->entries.erase(diagnostics.value->entries.begin(),
                                                 diagnostics.value->entries.begin() + static_cast<std::ptrdiff_t>(remove));
                diagnostics.truncated = true;
            }
        }

        void appendDiagnostic(detail::StateStorage& state, DiagnosticState diagnostic, std::size_t maximum) {
            if (!state.diagnostics.value)
                state.diagnostics.value.emplace();
            if (diagnostic.received)
                state.diagnostics.value->received = diagnostic.received;
            state.diagnostics.value->entries.push_back(std::move(diagnostic));
            trimDiagnostics(state.diagnostics, maximum);
        }

        bool decodeProcessCollection(const frontend::Json& value, ProcessCollectionState& result, std::string& error) {
            const frontend::Json* entries = nullptr;
            if (!requireArrayMember(value, "entries", entries, error))
                return false;
            for (const frontend::Json& entry : *entries) {
                ProcessState decoded;
                if (!decodeProcess(entry, decoded, error))
                    return false;
                if (!appendDistinct(
                        result.entries,
                        std::move(decoded),
                        [](const ProcessState& process) {
                            return process.processHandle;
                        },
                        "process collection",
                        error))
                    return false;
            }
            if (const auto truncation = value.find("truncation");
                truncation != value.end() && !decodeTruncation(*truncation, result.truncation, error))
                return false;
            result.extensions = extensionsOf(value, {"entries", "truncation"});
            return true;
        }

        bool decodeWatchCollection(const frontend::Json& value, FilesystemWatchCollectionState& result, std::string& error) {
            const frontend::Json* entries = nullptr;
            if (!requireArrayMember(value, "entries", entries, error))
                return false;
            for (const frontend::Json& entry : *entries) {
                FilesystemWatchState decoded;
                if (!decodeWatch(entry, decoded, error))
                    return false;
                if (!appendDistinct(
                        result.entries,
                        std::move(decoded),
                        [](const FilesystemWatchState& watch) {
                            return watch.watchId;
                        },
                        "filesystem watch collection",
                        error))
                    return false;
            }
            if (const auto truncation = value.find("truncation");
                truncation != value.end() && !decodeTruncation(*truncation, result.truncation, error))
                return false;
            result.extensions = extensionsOf(value, {"entries", "truncation"});
            return true;
        }

        bool decodeSearchCollection(const frontend::Json& value, FuzzySearchCollectionState& result, std::string& error) {
            const frontend::Json* entries = nullptr;
            if (!requireArrayMember(value, "entries", entries, error))
                return false;
            for (const frontend::Json& entry : *entries) {
                FuzzySearchState decoded;
                if (!decodeSearch(entry, decoded, error))
                    return false;
                if (!appendDistinct(
                        result.entries,
                        std::move(decoded),
                        [](const FuzzySearchState& search) {
                            return search.sessionId;
                        },
                        "fuzzy-search collection",
                        error))
                    return false;
            }
            if (const auto truncation = value.find("truncation");
                truncation != value.end() && !decodeTruncation(*truncation, result.truncation, error))
                return false;
            result.extensions = extensionsOf(value, {"entries", "truncation"});
            return true;
        }

        bool decodeNoticeCollection(const frontend::Json& value, NoticeCollectionState& result, std::string& error) {
            const frontend::Json* entries = nullptr;
            if (!requireArrayMember(value, "entries", entries, error))
                return false;
            for (const frontend::Json& entry : *entries) {
                NoticeState decoded;
                if (!decodeNotice(entry, decoded, error))
                    return false;
                result.entries.push_back(std::move(decoded));
            }
            if (const auto truncation = value.find("truncation");
                truncation != value.end() && !decodeTruncation(*truncation, result.truncation, error))
                return false;
            result.extensions = extensionsOf(value, {"entries", "truncation"});
            return true;
        }

        bool decodeActivityCollection(const frontend::Json& value, ActivityCollectionState& result, std::string& error) {
            const frontend::Json* entries = nullptr;
            if (!requireArrayMember(value, "entries", entries, error))
                return false;
            for (const frontend::Json& entry : *entries) {
                ActivityState decoded;
                if (!decodeActivity(entry, decoded, error))
                    return false;
                if (!appendDistinct(
                        result.entries,
                        std::move(decoded),
                        [](const ActivityState& activity) {
                            return activity.key;
                        },
                        "activity collection",
                        error))
                    return false;
            }
            if (const auto truncation = value.find("truncation");
                truncation != value.end() && !decodeTruncation(*truncation, result.truncation, error))
                return false;
            result.extensions = extensionsOf(value, {"entries", "truncation"});
            return true;
        }

        bool hasSelectedRepresentationCapability(const SessionInfo& session, frontend::FrontendCapability capability) {
            return std::find(session.selectedRepresentationCapabilities.begin(),
                             session.selectedRepresentationCapabilities.end(),
                             capability) != session.selectedRepresentationCapabilities.end();
        }

        bool selectedCompleteExpandedRepresentation(const SessionInfo& session) {
            constexpr std::array required{
                frontend::FrontendCapability::CompleteBackendDomains,
                frontend::FrontendCapability::DedicatedPendingRequests,
                frontend::FrontendCapability::DedicatedNotificationEvents,
                frontend::FrontendCapability::CompleteThreadItems,
                frontend::FrontendCapability::ScopeProjectedState,
            };
            return std::all_of(required.begin(), required.end(), [&](frontend::FrontendCapability capability) {
                return hasSelectedRepresentationCapability(session, capability);
            });
        }

        bool selectedExpandedSnapshot(const SessionInfo& session) {
            return hasSelectedRepresentationCapability(session, frontend::FrontendCapability::CompleteBackendDomains);
        }

        std::optional<frontend::FrontendCapability> eventRepresentationCapability(std::string_view type) {
            if (type == "codex.extension")
                return std::nullopt;
            if (type == "item.updated" || type == "item.content.updated" || type == "item.upserted")
                return frontend::FrontendCapability::CompleteThreadItems;
            if (type == "request.pending" || type == "request.resolved" || type == "pendingRequests.updated")
                return frontend::FrontendCapability::DedicatedPendingRequests;
            return frontend::FrontendCapability::DedicatedNotificationEvents;
        }

        bool eventUsesExpandedRepresentation(const SessionInfo* session, std::string_view type) {
            switch (frontend::detail::eventRepresentation(type)) {
                case frontend::detail::EventRepresentation::Legacy:
                    return false;
                case frontend::detail::EventRepresentation::Expanded:
                    return true;
                case frontend::detail::EventRepresentation::Either: {
                    const auto capability = eventRepresentationCapability(type);
                    return session != nullptr && capability && hasSelectedRepresentationCapability(*session, *capability);
                }
                case frontend::detail::EventRepresentation::None:
                    return false;
            }
            return false;
        }

        bool eventRepresentationWasNegotiated(const SessionInfo* session, std::string_view type, bool expanded, std::string& error) {
            if (session == nullptr || type == "codex.extension")
                return true;
            const auto capability = eventRepresentationCapability(type);
            if (!capability)
                return true;
            const bool selected = hasSelectedRepresentationCapability(*session, *capability);
            const auto representation = frontend::detail::eventRepresentation(type);
            if ((representation == frontend::detail::EventRepresentation::Legacy && selected) ||
                (representation == frontend::detail::EventRepresentation::Expanded && !selected) ||
                (representation == frontend::detail::EventRepresentation::Either && expanded != selected)) {
                error = "frontend event representation conflicts with capability negotiation";
                return false;
            }
            return true;
        }

        bool decodeExpanded(detail::StateStorage& result,
                            const frontend::ExpandedSnapshot& snapshot,
                            std::size_t maximumRetainedDiagnostics,
                            std::string& error) {
            const frontend::ExpandedBackendSnapshotState& state = snapshot.state;
            result.representationMode = RepresentationMode::ExpandedV1;
            ProviderState provider;
            if (!decodeProvider(state.provider, provider, error))
                return false;
            result.provider.value = std::move(provider);
            ControllerState controller;
            if (!decodeController(state.controller, result.session, controller, error))
                return false;
            result.controller.value = std::move(controller);
            result.sessions.value.emplace();
            for (const frontend::Json& value : state.sessions) {
                SessionState session;
                if (!decodeSession(value, session, error))
                    return false;
                if (!appendDistinct(
                        *result.sessions.value,
                        std::move(session),
                        [](const SessionState& entry) {
                            return entry.sessionId;
                        },
                        "session collection",
                        error))
                    return false;
            }
            ThreadListState threadList;
            if (!decodeThreadList(state.threadList, threadList, error))
                return false;
            result.threadList.value = std::move(threadList);
            if (state.threads) {
                result.threadProjectionPresent = true;
                for (const frontend::Json& value : *state.threads) {
                    ThreadState thread;
                    if (!decodeThread(value, thread, error))
                        return false;
                    if (!appendDistinct(
                            result.threads,
                            std::move(thread),
                            [](const ThreadState& entry) {
                                return entry.id;
                            },
                            "thread collection",
                            error))
                        return false;
                }
            }
            if (state.turns) {
                result.turnProjectionPresent = true;
                for (const frontend::Json& value : *state.turns) {
                    TurnState turn;
                    if (!decodeTurn(value, turn, error))
                        return false;
                    if (!appendDistinct(
                            result.turns,
                            std::move(turn),
                            [](const TurnState& entry) {
                                return entry.id;
                            },
                            "turn collection",
                            error))
                        return false;
                }
            }
            if (state.items) {
                result.itemProjectionPresent = true;
                for (const frontend::ExpandedThreadItem& value : *state.items) {
                    if (!appendDistinct(
                            result.items,
                            decodeItem(value),
                            [](const ItemState& entry) {
                                return entry.id;
                            },
                            "item collection",
                            error))
                        return false;
                }
            }
            if (state.pendingRequests) {
                result.pendingRequestProjectionPresent = true;
                for (const frontend::ExpandedPendingRequest& value : *state.pendingRequests) {
                    if (!appendDistinct(
                            result.pendingRequests,
                            decodePendingRequest(value),
                            [](const PendingRequestState& entry) {
                                return entry.id;
                            },
                            "pending-request collection",
                            error))
                        return false;
                }
            }
            // Rebuild flat expanded snapshot relationships without destroying
            // an explicitly supplied stable order.
            for (const TurnState& turn : result.turns) {
                const auto thread = std::find_if(result.threads.begin(), result.threads.end(), [&](const ThreadState& candidate) {
                    return candidate.id == turn.threadId;
                });
                if (thread != result.threads.end())
                    appendUnique(thread->orderedTurns, turn.id);
            }
            for (const ItemState& item : result.items) {
                if (!item.turnId)
                    continue;
                const auto turn = std::find_if(result.turns.begin(), result.turns.end(), [&](const TurnState& candidate) {
                    return candidate.id == *item.turnId;
                });
                if (turn != result.turns.end())
                    appendUnique(turn->orderedItems, item.id);
            }

#define AISUITE_DECODE_DOMAIN(member, type)                                                                                                \
    if (state.member) {                                                                                                                    \
        if (!decodeDomainWrapper(*state.member, result.member, error))                                                                     \
            return false;                                                                                                                  \
    }
            AISUITE_DECODE_DOMAIN(accounts, AccountState)
            AISUITE_DECODE_DOMAIN(models, ModelsState)
            AISUITE_DECODE_DOMAIN(configuration, ConfigurationState)
            AISUITE_DECODE_DOMAIN(permissionProfiles, PermissionProfilesState)
            AISUITE_DECODE_DOMAIN(reviews, ReviewsState)
            AISUITE_DECODE_DOMAIN(apps, AppsState)
            AISUITE_DECODE_DOMAIN(externalAgents, ExternalAgentsState)
            AISUITE_DECODE_DOMAIN(hooks, HooksState)
            AISUITE_DECODE_DOMAIN(marketplace, MarketplaceState)
            AISUITE_DECODE_DOMAIN(plugins, PluginsState)
            AISUITE_DECODE_DOMAIN(skills, SkillsState)
            AISUITE_DECODE_DOMAIN(mcp, McpState)
            AISUITE_DECODE_DOMAIN(windowsSandbox, WindowsSandboxState)
#undef AISUITE_DECODE_DOMAIN
            if (state.remoteControl && !decodeDomainWrapper(*state.remoteControl, result.platform, error))
                return false;
            if (state.processes) {
                ProcessCollectionState decoded;
                if (!decodeProcessCollection(*state.processes, decoded, error))
                    return false;
                result.processes.value = std::move(decoded);
                result.processes.truncated = result.processes.value->truncation.truncated;
                result.processes.omittedFields = result.processes.value->truncation.omittedFields;
            }
            if (state.filesystemWatches) {
                FilesystemWatchCollectionState decoded;
                if (!decodeWatchCollection(*state.filesystemWatches, decoded, error))
                    return false;
                result.filesystemWatches.value = std::move(decoded);
                result.filesystemWatches.truncated = result.filesystemWatches.value->truncation.truncated;
                result.filesystemWatches.omittedFields = result.filesystemWatches.value->truncation.omittedFields;
            }
            if (state.fuzzySearches) {
                FuzzySearchCollectionState decoded;
                if (!decodeSearchCollection(*state.fuzzySearches, decoded, error))
                    return false;
                result.fuzzySearches.value = std::move(decoded);
                result.fuzzySearches.truncated = result.fuzzySearches.value->truncation.truncated;
                result.fuzzySearches.omittedFields = result.fuzzySearches.value->truncation.omittedFields;
            }
            if (state.notices) {
                NoticeCollectionState decoded;
                if (!decodeNoticeCollection(*state.notices, decoded, error))
                    return false;
                result.notices.value = std::move(decoded);
                result.notices.truncated = result.notices.value->truncation.truncated;
                result.notices.omittedFields = result.notices.value->truncation.omittedFields;
            }
            if (state.activities) {
                ActivityCollectionState decoded;
                if (!decodeActivityCollection(*state.activities, decoded, error))
                    return false;
                result.activities.value = std::move(decoded);
                result.activities.truncated = result.activities.value->truncation.truncated;
                result.activities.omittedFields = result.activities.value->truncation.omittedFields;
            }
            result.capacity.value = decodeCapacity(state.capacity);
            TruncationMetadata truncation;
            if (!decodeTruncation(state.truncation, truncation, error))
                return false;
            result.truncation.value = truncation;
            result.truncation.truncated = truncation.truncated;
            result.truncation.omittedFields = truncation.omittedFields;
            result.backendCursor.frontendSequenceExhausted = optionalBool(state.extensions, "frontendSequenceExhausted");
            frontend::Json stateExtensions = extensionsOf(state.extensions, {"frontendSequenceExhausted"});
            if (!stateExtensions.empty())
                result.compatibilityExtensions["state"] = std::move(stateExtensions);
            trimDiagnostics(result.diagnostics, maximumRetainedDiagnostics);
            return true;
        }

        bool decodeLegacy(detail::StateStorage& result,
                          const frontend::Json& wireState,
                          std::size_t maximumRetainedDiagnostics,
                          std::string& error) {
            if (!validateLegacySnapshotSchema(wireState, error))
                return false;
            auto sanitized = sanitizeLegacySnapshot(wireState, error);
            if (!sanitized)
                return false;
            const frontend::Json& state = *sanitized;
            if (!validateLegacySnapshotSchema(state, error)) {
                error = "bounded legacy snapshot no longer satisfies its stable schema: " + error;
                return false;
            }
            result.representationMode = RepresentationMode::LegacyV1;
            result.backendCursor.backendRevision = *optionalUnsigned(state, "backendRevision");
            const frontend::Json& journal = state.at("journal");
            result.backendCursor.oldestReplayableAfter = frontend::SequenceNumber{*optionalUnsigned(journal, "oldestReplayableAfter")};
            result.backendCursor.currentSequence = frontend::SequenceNumber{*optionalUnsigned(journal, "currentSequence")};
            if (const auto value = optionalUnsigned(journal, "oldestRetainedSequence"))
                result.backendCursor.oldestRetainedSequence = frontend::SequenceNumber{*value};
            if (const auto value = optionalUnsigned(journal, "newestRetainedSequence"))
                result.backendCursor.newestRetainedSequence = frontend::SequenceNumber{*value};
            result.backendCursor.backendSequenceExhausted = *optionalBool(state, "sequenceExhausted");
            result.backendCursor.frontendSequenceExhausted = optionalBool(state, "frontendSequenceExhausted");
            frontend::Json normalizedProvider = frontend::Json::object();
            normalizedProvider["lifecycle"] = *stringMember(state, "lifecycle");
            if (const auto lastError = state.find("lastLifecycleError"); lastError != state.end())
                normalizedProvider["lastError"] = *lastError;
            ProviderState provider;
            if (!decodeProvider(normalizedProvider, provider, error, false))
                return false;
            result.provider.value = std::move(provider);
            if (const auto controller = state.find("controller"); controller != state.end()) {
                frontend::Json normalized = frontend::Json::object();
                if (controller->is_string()) {
                    normalized["controllerSessionId"] = controller->get<std::string>();
                    normalized["present"] = true;
                } else if (controller->is_number_unsigned()) {
                    normalized["controllerSessionId"] = std::to_string(controller->get<std::uint64_t>());
                    normalized["present"] = true;
                } else if (controller->is_object()) {
                    normalized = *controller;
                }
                ControllerState decoded;
                if (!decodeController(normalized, result.session, decoded, error))
                    return false;
                result.controller.value = std::move(decoded);
            } else if (const auto controllerSessionId = stringMember(state, "controllerSessionId")) {
                frontend::Json normalizedController = frontend::Json::object();
                normalizedController["controllerSessionId"] = *controllerSessionId;
                normalizedController["present"] = true;
                ControllerState decoded;
                if (!decodeController(normalizedController, result.session, decoded, error))
                    return false;
                result.controller.value = std::move(decoded);
            } else if (state.contains("backendRevision") || state.contains("lifecycle")) {
                frontend::Json normalizedController = frontend::Json::object();
                normalizedController["present"] = false;
                ControllerState decoded;
                if (!decodeController(normalizedController, result.session, decoded, error))
                    return false;
                result.controller.value = std::move(decoded);
            }
            if (const auto sessions = state.find("sessions"); sessions != state.end()) {
                result.sessions.value.emplace();
                for (const frontend::Json& value : *sessions) {
                    SessionState decoded;
                    if (!decodeSession(value, decoded, error))
                        return false;
                    if (!appendDistinct(
                            *result.sessions.value,
                            std::move(decoded),
                            [](const SessionState& entry) {
                                return entry.sessionId;
                            },
                            "session collection",
                            error))
                        return false;
                }
            }
            if (const auto threadList = state.find("threadList"); threadList != state.end()) {
                ThreadListState decoded;
                if (!decodeThreadList(*threadList, decoded, error))
                    return false;
                result.threadList.value = std::move(decoded);
            }
            if (const auto threads = state.find("threads"); threads != state.end()) {
                result.threadProjectionPresent = true;
                result.turnProjectionPresent = true;
                result.itemProjectionPresent = true;
                if (!threads->is_array()) {
                    error = "legacy threads must be an array";
                    return false;
                }
                for (const frontend::Json& threadValue : *threads) {
                    ThreadState thread;
                    if (!decodeThread(threadValue, thread, error))
                        return false;
                    const typed::ThreadId threadId = thread.id;
                    if (!appendDistinct(
                            result.threads,
                            std::move(thread),
                            [](const ThreadState& entry) {
                                return entry.id;
                            },
                            "thread collection",
                            error))
                        return false;
                    if (const auto turns = threadValue.find("turns"); turns != threadValue.end()) {
                        if (!turns->is_array()) {
                            error = "legacy thread turns must be an array";
                            return false;
                        }
                        for (const frontend::Json& turnValue : *turns) {
                            TurnState turn;
                            if (!decodeTurn(turnValue, turn, error, threadId))
                                return false;
                            const typed::TurnId turnId = turn.id;
                            if (!appendDistinct(
                                    result.turns,
                                    std::move(turn),
                                    [](const TurnState& entry) {
                                        return entry.id;
                                    },
                                    "turn collection",
                                    error))
                                return false;
                            if (const auto items = turnValue.find("items"); items != turnValue.end()) {
                                if (!items->is_array()) {
                                    error = "legacy turn items must be an array";
                                    return false;
                                }
                                for (const frontend::Json& itemValue : *items) {
                                    ItemState item;
                                    if (!decodeLegacyItem(itemValue, item, error, threadId, turnId))
                                        return false;
                                    if (!appendDistinct(
                                            result.items,
                                            std::move(item),
                                            [](const ItemState& entry) {
                                                return entry.id;
                                            },
                                            "item collection",
                                            error))
                                        return false;
                                }
                            }
                        }
                    }
                }
            }
            if (const auto completeItems = state.find("items"); completeItems != state.end()) {
                result.itemProjectionPresent = true;
                for (const frontend::Json& itemValue : *completeItems) {
                    ItemState decoded;
                    if (!decodeExpandedItem(itemValue, decoded, error))
                        return false;
                    const auto prior = std::find_if(result.items.begin(), result.items.end(), [&](const ItemState& item) {
                        return item.id == decoded.id;
                    });
                    if (prior != result.items.end()) {
                        if (!decoded.threadId)
                            decoded.threadId = prior->threadId;
                        if (!decoded.turnId)
                            decoded.turnId = prior->turnId;
                        if (prior->turnId && prior->turnId != decoded.turnId) {
                            const auto oldTurn = std::find_if(result.turns.begin(), result.turns.end(), [&](const TurnState& turn) {
                                return turn.id == *prior->turnId;
                            });
                            if (oldTurn != result.turns.end())
                                std::erase(oldTurn->orderedItems, decoded.id);
                        }
                    }
                    const typed::ItemId id = decoded.id;
                    const std::optional<typed::TurnId> parent = decoded.turnId;
                    upsert(result.items, std::move(decoded), [](const ItemState& item) {
                        return item.id;
                    });
                    if (parent) {
                        const auto turn = std::find_if(result.turns.begin(), result.turns.end(), [&](const TurnState& value) {
                            return value.id == *parent;
                        });
                        if (turn == result.turns.end()) {
                            error = "complete thread item references an unknown turn";
                            return false;
                        }
                        appendUnique(turn->orderedItems, id);
                    }
                }
            }
            if (const auto requests = state.find("pendingRequests"); requests != state.end()) {
                result.pendingRequestProjectionPresent = true;
                if (!requests->is_array()) {
                    error = "legacy pendingRequests must be an array";
                    return false;
                }
                for (const frontend::Json& value : *requests) {
                    PendingRequestState request;
                    if (!decodePendingRequestJson(value, request, error, false))
                        return false;
                    if (!appendDistinct(
                            result.pendingRequests,
                            std::move(request),
                            [](const PendingRequestState& entry) {
                                return entry.id;
                            },
                            "pending-request collection",
                            error))
                        return false;
                }
            }

            const frontend::Json* domains = &state;
            if (const auto nested = state.find("domains"); nested != state.end() && nested->is_object())
                domains = &*nested;
#define AISUITE_LEGACY_DOMAIN(jsonName, member)                                                                                            \
    if (const auto value = domains->find(jsonName); value != domains->end() && value->is_object()) {                                       \
        if (!decodeDomainWrapper(*value, result.member, error))                                                                            \
            return false;                                                                                                                  \
    }
            AISUITE_LEGACY_DOMAIN("accounts", accounts)
            AISUITE_LEGACY_DOMAIN("models", models)
            AISUITE_LEGACY_DOMAIN("configuration", configuration)
            AISUITE_LEGACY_DOMAIN("reviews", reviews)
            AISUITE_LEGACY_DOMAIN("integrations", apps)
            AISUITE_LEGACY_DOMAIN("pluginsAndSkills", plugins)
            AISUITE_LEGACY_DOMAIN("mcp", mcp)
            AISUITE_LEGACY_DOMAIN("platform", platform)
#undef AISUITE_LEGACY_DOMAIN
            if (result.reviews.value) {
                result.permissionProfiles.value = PermissionProfilesState{result.reviews.value->projection};
                result.permissionProfiles.truncated = result.reviews.truncated;
                result.permissionProfiles.omittedFields = result.reviews.omittedFields;
            }
            if (result.apps.value) {
                result.externalAgents.value = ExternalAgentsState{result.apps.value->projection, result.apps.value->details};
                result.hooks.value = HooksState{result.apps.value->projection, result.apps.value->details};
                result.marketplace.value = MarketplaceState{result.apps.value->projection, result.apps.value->details};
                result.externalAgents.truncated = result.hooks.truncated = result.marketplace.truncated = result.apps.truncated;
                result.externalAgents.omittedFields = result.hooks.omittedFields = result.marketplace.omittedFields =
                    result.apps.omittedFields;
            }
            if (result.plugins.value) {
                result.skills.value = SkillsState{result.plugins.value->projection, result.plugins.value->details};
                result.skills.truncated = result.plugins.truncated;
                result.skills.omittedFields = result.plugins.omittedFields;
            }
            if (result.platform.value) {
                result.windowsSandbox.value = WindowsSandboxState{result.platform.value->projection, result.platform.value->details};
                result.windowsSandbox.truncated = result.platform.truncated;
                result.windowsSandbox.omittedFields = result.platform.omittedFields;
            }

            // The explicit forms avoid exposing a generic domain container and
            // keep each collection's stable element type visible to callers.
            const auto legacyCollectionWrapper = [&error](const frontend::Json& value,
                                                          std::string_view name) -> std::optional<frontend::Json> {
                frontend::Json wrapper = frontend::Json::object();
                if (value.is_array()) {
                    wrapper["entries"] = value;
                } else if (value.is_object()) {
                    wrapper = value;
                } else {
                    error = "legacy snapshot collection '" + std::string(name) + "' has an invalid type";
                    return std::nullopt;
                }
                const auto entries = wrapper.find("entries");
                if (entries == wrapper.end() || !entries->is_array()) {
                    error = "legacy snapshot collection '" + std::string(name) + "' lacks its entries array";
                    return std::nullopt;
                }
                if (!wrapper.contains("truncation")) {
                    wrapper["truncation"] = frontend::Json::object();
                    wrapper["truncation"]["truncated"] = false;
                }
                return std::optional<frontend::Json>{std::move(wrapper)};
            };
            if (const auto found = state.find("processes"); found != state.end()) {
                auto wrapper = legacyCollectionWrapper(*found, "processes");
                if (!wrapper)
                    return false;
                ProcessCollectionState decoded;
                if (!decodeProcessCollection(*wrapper, decoded, error))
                    return false;
                result.processes.value = std::move(decoded);
                result.processes.truncated = result.processes.value->truncation.truncated;
                result.processes.omittedFields = result.processes.value->truncation.omittedFields;
            }
            if (const auto found = state.find("filesystemWatches"); found != state.end()) {
                auto wrapper = legacyCollectionWrapper(*found, "filesystemWatches");
                if (!wrapper)
                    return false;
                FilesystemWatchCollectionState decoded;
                if (!decodeWatchCollection(*wrapper, decoded, error))
                    return false;
                result.filesystemWatches.value = std::move(decoded);
                result.filesystemWatches.truncated = result.filesystemWatches.value->truncation.truncated;
                result.filesystemWatches.omittedFields = result.filesystemWatches.value->truncation.omittedFields;
            }
            const auto fuzzy = state.find("fuzzySearches") != state.end() ? state.find("fuzzySearches") : state.find("fuzzySearchSessions");
            if (fuzzy != state.end()) {
                auto wrapper = legacyCollectionWrapper(*fuzzy, "fuzzy searches");
                if (!wrapper)
                    return false;
                FuzzySearchCollectionState decoded;
                if (!decodeSearchCollection(*wrapper, decoded, error))
                    return false;
                result.fuzzySearches.value = std::move(decoded);
                result.fuzzySearches.truncated = result.fuzzySearches.value->truncation.truncated;
                result.fuzzySearches.omittedFields = result.fuzzySearches.value->truncation.omittedFields;
            }
            if (const auto found = state.find("notices"); found != state.end()) {
                auto wrapper = legacyCollectionWrapper(*found, "notices");
                if (!wrapper)
                    return false;
                NoticeCollectionState decoded;
                if (!decodeNoticeCollection(*wrapper, decoded, error))
                    return false;
                result.notices.value = std::move(decoded);
                result.notices.truncated = result.notices.value->truncation.truncated;
                result.notices.omittedFields = result.notices.value->truncation.omittedFields;
            }
            if (const auto found = state.find("activities"); found != state.end()) {
                auto wrapper = legacyCollectionWrapper(*found, "activities");
                if (!wrapper)
                    return false;
                ActivityCollectionState decoded;
                if (!decodeActivityCollection(*wrapper, decoded, error))
                    return false;
                result.activities.value = std::move(decoded);
                result.activities.truncated = result.activities.value->truncation.truncated;
                result.activities.omittedFields = result.activities.value->truncation.omittedFields;
            }
            if (const auto capacity = state.find("capacity"); capacity != state.end() && capacity->is_object())
                result.capacity.value = decodeCapacity(*capacity);
            if (const auto diagnostics = state.find("diagnostics"); diagnostics != state.end()) {
                result.diagnostics.value.emplace();
                const auto received = optionalUnsigned(*diagnostics, "received");
                for (const frontend::Json& message : diagnostics->at("recent")) {
                    DiagnosticState diagnostic;
                    diagnostic.received = received;
                    diagnostic.message = message.get<std::string>();
                    result.diagnostics.value->entries.push_back(std::move(diagnostic));
                }
                result.diagnostics.value->received = received;
                trimDiagnostics(result.diagnostics, maximumRetainedDiagnostics);
            }
            const std::size_t omittedExtensions = static_cast<std::size_t>(*optionalUnsigned(state, "omittedCodexExtensions"));
            result.truncation.value.emplace();
            result.truncation.value->truncated = omittedExtensions != 0;
            result.truncation.value->omittedEntries = omittedExtensions;
            result.truncation.truncated = result.truncation.value->truncated;
            result.compatibilityExtensions = extensionsOf(state,
                                                          {"backendRevision",
                                                           "provider",
                                                           "lifecycle",
                                                           "lastLifecycleError",
                                                           "controller",
                                                           "controllerSessionId",
                                                           "sessions",
                                                           "threadList",
                                                           "threads",
                                                           "items",
                                                           "pendingRequests",
                                                           "domains",
                                                           "accounts",
                                                           "models",
                                                           "configuration",
                                                           "reviews",
                                                           "integrations",
                                                           "pluginsAndSkills",
                                                           "mcp",
                                                           "platform",
                                                           "processes",
                                                           "filesystemWatches",
                                                           "fuzzySearches",
                                                           "fuzzySearchSessions",
                                                           "notices",
                                                           "activities",
                                                           "capacity",
                                                           "diagnostics",
                                                           "codexExtensions",
                                                           "omittedCodexExtensions",
                                                           "journal",
                                                           "sequenceExhausted",
                                                           "frontendSequenceExhausted"});
            if (!state.at("codexExtensions").empty())
                result.compatibilityExtensions["codexExtensions"] = state.at("codexExtensions");
            return true;
        }

        bool decodeSnapshotContents(detail::StateStorage& candidate,
                                    const frontend::Snapshot& snapshotMessage,
                                    const SessionInfo& session,
                                    std::size_t maximumRetainedDiagnostics,
                                    bool allowLegacyV1,
                                    std::string& error) {
            if (!allowLegacyV1 && !selectedCompleteExpandedRepresentation(session)) {
                error = "legacy Frontend Protocol v1 snapshot is not permitted";
                return false;
            }
            if (selectedExpandedSnapshot(session)) {
                const auto encoded = frontend::Codec::encodeServer(frontend::ServerMessage{snapshotMessage});
                const auto expanded = encoded ? frontend::Codec::decodeExpandedSnapshot(encoded.value())
                                              : frontend::CodecResult<frontend::ExpandedSnapshot>{encoded.error()};
                if (!expanded) {
                    error = "expanded Frontend Protocol v1 snapshot is required by the negotiated representation";
                    return false;
                }
                if (!decodeExpanded(candidate, expanded.value(), maximumRetainedDiagnostics, error))
                    return false;
            } else if (!decodeLegacy(candidate, snapshotMessage.state, maximumRetainedDiagnostics, error)) {
                return false;
            }
            return applySnapshotProjectionMetadata(candidate, snapshotMessage.extensions, error);
        }

        frontend::Json encodeStamp(const SourceStamp& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["generation"] = value.generation;
            result["freshness"] = std::string(frontend::toString(value.freshness));
            return result;
        }

        frontend::Json encodeTruncation(const TruncationMetadata& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["truncated"] = value.truncated;
            result["omittedFields"] = frontend::Json::array();
            for (const std::string& field : value.omittedFields)
                result["omittedFields"].push_back(field);
            if (value.omittedEntries)
                result["omittedEntries"] = *value.omittedEntries;
            if (value.droppedBytes)
                result["droppedBytes"] = *value.droppedBytes;
            return result;
        }

        template <typename T>
        void addOptional(frontend::Json& object, std::string_view key, const std::optional<T>& value) {
            if (value)
                object[std::string(key)] = *value;
        }

        frontend::Json encodeDomain(const DomainProjectionState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            if (value.stamp)
                result["stamp"] = encodeStamp(*value.stamp);
            addOptional(result, "status", value.status);
            addOptional(result, "summary", value.summary);
            addOptional(result, "nextCursor", value.nextCursor);
            addOptional(result, "complete", value.complete);
            addOptional(result, "itemCount", value.itemCount);
            result["latestResults"] = frontend::Json::array();
            for (const DomainResultSummaryState& entry : value.latestResults) {
                frontend::Json encoded = entry.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
                encoded["method"] = entry.method;
                encoded["status"] = entry.status;
                addOptional(encoded, "subjectId", entry.subjectId);
                addOptional(encoded, "nextCursor", entry.nextCursor);
                addOptional(encoded, "itemCount", entry.itemCount);
                addOptional(encoded, "complete", entry.complete);
                encoded["stamp"] = encodeStamp(entry.stamp);
                result["latestResults"].push_back(std::move(encoded));
            }
            frontend::Json details = value.opaqueDetails;
            if (!details.is_object())
                details = frontend::Json::object();
            addOptional(details, "notificationCount", value.notificationCount);
            if (!value.latestNotificationMethods.empty()) {
                details["latestNotificationMethods"] = frontend::Json::array();
                for (const std::string& method : value.latestNotificationMethods)
                    details["latestNotificationMethods"].push_back(method);
            }
            if (!details.empty())
                result["details"] = std::move(details);
            if (value.truncation)
                result["truncation"] = encodeTruncation(*value.truncation);
            return result;
