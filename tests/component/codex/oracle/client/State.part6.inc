                return false;
            }
            return true;
        }

        bool
        setSectionJson(detail::StateStorage& state, detail::StateSizeSection section, const std::optional<frontend::Json>& value) noexcept {
            if (!value)
                return setSectionBytes(state, section, std::nullopt);
            const std::optional<std::size_t> bytes = compactJsonBytes(*value);
            if (!bytes) {
                state.sizeLedger.failed = true;
                return false;
            }
            return setSectionBytes(state, section, bytes);
        }

        bool setSectionJson(detail::StateStorage& state, detail::StateSizeSection section, const frontend::Json& value) noexcept {
            const std::optional<std::size_t> bytes = compactJsonBytes(value);
            if (!bytes) {
                state.sizeLedger.failed = true;
                return false;
            }
            return setSectionBytes(state, section, bytes);
        }

        std::size_t encodedEntityBytes(const frontend::Json& value) {
            return value.dump().size();
        }

        bool adjustArrayContribution(detail::StateArrayContribution& contribution,
                                     std::optional<std::size_t> oldBytes,
                                     std::optional<std::size_t> newBytes) noexcept {
            detail::StateArrayContribution next = contribution;
            if (oldBytes) {
                if (next.count == 0 || !checkedSubtract(next.elementBytes, *oldBytes, next.elementBytes))
                    return false;
                --next.count;
            }
            if (newBytes) {
                if (next.count == std::numeric_limits<std::size_t>::max() || !checkedAdd(next.elementBytes, *newBytes, next.elementBytes))
                    return false;
                ++next.count;
            }
            contribution = next;
            return true;
        }

        bool refreshDirectArraySection(detail::StateStorage& state,
                                       detail::StateSizeSection section,
                                       const detail::StateArrayContribution& contribution) noexcept {
            return setSectionBytes(state, section, arrayBytes(contribution));
        }

        template <typename T, typename Encoder>
        bool rebuildDirectArraySection(detail::StateStorage& state,
                                       detail::StateSizeSection section,
                                       detail::StateArrayContribution& contribution,
                                       const std::vector<T>& values,
                                       Encoder encoder) noexcept {
            detail::StateArrayContribution next;
            try {
                for (const T& value : values) {
                    const std::size_t bytes = encodedEntityBytes(encoder(value));
                    if (next.count == std::numeric_limits<std::size_t>::max() || !checkedAdd(next.elementBytes, bytes, next.elementBytes)) {
                        state.sizeLedger.failed = true;
                        return false;
                    }
                    ++next.count;
                }
            } catch (...) {
                state.sizeLedger.failed = true;
                return false;
            }
            contribution = next;
            return refreshDirectArraySection(state, section, contribution);
        }

        template <typename Collection>
        frontend::Json encodeCollectionSkeleton(const Collection& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["entries"] = frontend::Json::array();
            result["truncation"] = encodeTruncation(value.truncation);
            return result;
        }

        frontend::Json encodeCollectionSkeleton(const DiagnosticCollectionState& value) {
            frontend::Json result = frontend::Json::object();
            addOptional(result, "received", value.received);
            result["entries"] = frontend::Json::array();
            return result;
        }

        template <typename Collection>
        std::optional<std::size_t> projectedCollectionBytes(const Projected<Collection>& projected,
                                                            const detail::StateArrayContribution& contribution) noexcept {
            try {
                const frontend::Json encoded = encodeProjected(projected, [](const Collection& value) {
                    return encodeCollectionSkeleton(value);
                });
                std::size_t bytes = encoded.dump().size();
                if (!projected.value)
                    return bytes;
                const std::optional<std::size_t> entriesBytes = arrayBytes(contribution);
                if (!entriesBytes || !checkedSubtract(bytes, 2, bytes) || !checkedAdd(bytes, *entriesBytes, bytes))
                    return std::nullopt;
                return bytes;
            } catch (...) {
                return std::nullopt;
            }
        }

        template <typename Collection>
        bool refreshProjectedCollectionSection(detail::StateStorage& state,
                                               detail::StateSizeSection section,
                                               const Projected<Collection>& projected,
                                               const detail::StateArrayContribution& contribution) noexcept {
            const std::optional<std::size_t> bytes = projectedCollectionBytes(projected, contribution);
            if (!bytes || !setSectionBytes(state, section, bytes)) {
                state.sizeLedger.failed = true;
                return false;
            }
            return true;
        }

        template <typename Collection, typename Encoder>
        bool rebuildProjectedCollectionSection(detail::StateStorage& state,
                                               detail::StateSizeSection section,
                                               detail::StateArrayContribution& contribution,
                                               const Projected<Collection>& projected,
                                               Encoder encoder) noexcept {
            detail::StateArrayContribution next;
            try {
                if (projected.value) {
                    for (const auto& value : projected.value->entries) {
                        const std::size_t bytes = encodedEntityBytes(encoder(value));
                        if (next.count == std::numeric_limits<std::size_t>::max() ||
                            !checkedAdd(next.elementBytes, bytes, next.elementBytes)) {
                            state.sizeLedger.failed = true;
                            return false;
                        }
                        ++next.count;
                    }
                }
            } catch (...) {
                state.sizeLedger.failed = true;
                return false;
            }
            contribution = next;
            return refreshProjectedCollectionSection(state, section, projected, contribution);
        }

        bool refreshProjectedSessionsSection(detail::StateStorage& state) noexcept {
            try {
                detail::StateArrayContribution next;
                if (state.sessions.value) {
                    for (const SessionState& value : *state.sessions.value) {
                        const std::size_t bytes = encodedEntityBytes(encodeSessionState(value));
                        if (next.count == std::numeric_limits<std::size_t>::max() ||
                            !checkedAdd(next.elementBytes, bytes, next.elementBytes)) {
                            state.sizeLedger.failed = true;
                            return false;
                        }
                        ++next.count;
                    }
                }
                state.sizeLedger.sessions = next;
                const frontend::Json skeleton = encodeProjected(state.sessions, [](const std::vector<SessionState>&) {
                    return frontend::Json::array();
                });
                std::size_t bytes = skeleton.dump().size();
                if (state.sessions.value) {
                    const std::optional<std::size_t> valuesBytes = arrayBytes(next);
                    if (!valuesBytes || !checkedSubtract(bytes, 2, bytes) || !checkedAdd(bytes, *valuesBytes, bytes))
                        return false;
                }
                return setSectionBytes(state, detail::StateSizeSection::Sessions, bytes);
            } catch (...) {
                state.sizeLedger.failed = true;
                return false;
            }
        }

        bool refreshProjectedSessionsMetadata(detail::StateStorage& state) noexcept {
            try {
                const frontend::Json skeleton = encodeProjected(state.sessions, [](const std::vector<SessionState>&) {
                    return frontend::Json::array();
                });
                std::size_t bytes = skeleton.dump().size();
                if (state.sessions.value) {
                    const std::optional<std::size_t> valuesBytes = arrayBytes(state.sizeLedger.sessions);
                    if (!valuesBytes || !checkedSubtract(bytes, 2, bytes) || !checkedAdd(bytes, *valuesBytes, bytes))
                        return false;
                }
                return setSectionBytes(state, detail::StateSizeSection::Sessions, bytes);
            } catch (...) {
                state.sizeLedger.failed = true;
                return false;
            }
        }

        bool accountingFailure(detail::StateStorage& state, std::string& error) noexcept;

        bool refreshEventProjectionAccounting(detail::StateStorage& state, frontend::ExpandedEventType type, std::string& error) {
            const auto setTyped = [&state, &error](detail::StateSizeSection section, const auto& projected) {
                if (!setSectionJson(state, section, encodeProjected(projected, [](const auto& value) {
                                        return encodeTypedDomain(value);
                                    })))
                    return accountingFailure(state, error);
                return true;
            };
            using enum frontend::ExpandedEventType;
            switch (type) {
                case ProviderUpdated:
                    return setSectionJson(state, detail::StateSizeSection::Provider, encodeProjected(state.provider, encodeProvider)) ||
                           accountingFailure(state, error);
                case ControllerUpdated:
                    return setSectionJson(
                               state, detail::StateSizeSection::Controller, encodeProjected(state.controller, encodeControllerState)) ||
                           accountingFailure(state, error);
                case SessionsUpdated:
                    return refreshProjectedSessionsMetadata(state) || accountingFailure(state, error);
                case ThreadListUpdated:
                    return setSectionJson(
                               state, detail::StateSizeSection::ThreadList, encodeProjected(state.threadList, encodeThreadListState)) ||
                           accountingFailure(state, error);
                case AccountUpdated:
                    return setTyped(detail::StateSizeSection::Accounts, state.accounts);
                case ModelsUpdated:
                    return setTyped(detail::StateSizeSection::Models, state.models);
                case ConfigurationUpdated:
                    return setTyped(detail::StateSizeSection::Configuration, state.configuration);
                case ProcessUpdated:
                    return refreshProjectedCollectionSection(
                               state, detail::StateSizeSection::Processes, state.processes, state.sizeLedger.processes) ||
                           accountingFailure(state, error);
                case FilesystemWatchUpdated:
                    return refreshProjectedCollectionSection(state,
                                                             detail::StateSizeSection::FilesystemWatches,
                                                             state.filesystemWatches,
                                                             state.sizeLedger.filesystemWatches) ||
                           accountingFailure(state, error);
                case FuzzySearchUpdated:
                    return refreshProjectedCollectionSection(
                               state, detail::StateSizeSection::FuzzySearches, state.fuzzySearches, state.sizeLedger.fuzzySearches) ||
                           accountingFailure(state, error);
                case ReviewsUpdated:
                    return setTyped(detail::StateSizeSection::Reviews, state.reviews) &&
                           setTyped(detail::StateSizeSection::PermissionProfiles, state.permissionProfiles);
                case IntegrationsUpdated:
                    return setTyped(detail::StateSizeSection::Apps, state.apps) &&
                           setTyped(detail::StateSizeSection::ExternalAgents, state.externalAgents) &&
                           setTyped(detail::StateSizeSection::Hooks, state.hooks) &&
                           setTyped(detail::StateSizeSection::Marketplace, state.marketplace);
                case PluginsUpdated:
                case SkillsUpdated:
                    return setTyped(detail::StateSizeSection::Plugins, state.plugins) &&
                           setTyped(detail::StateSizeSection::Skills, state.skills);
                case McpUpdated:
                    return setTyped(detail::StateSizeSection::Mcp, state.mcp);
                case PlatformUpdated:
                    return setTyped(detail::StateSizeSection::Platform, state.platform) &&
                           setTyped(detail::StateSizeSection::WindowsSandbox, state.windowsSandbox);
                case NoticeAdded:
                    return refreshProjectedCollectionSection(
                               state, detail::StateSizeSection::Notices, state.notices, state.sizeLedger.notices) ||
                           accountingFailure(state, error);
                case ActivityUpdated:
                    return refreshProjectedCollectionSection(
                               state, detail::StateSizeSection::Activities, state.activities, state.sizeLedger.activities) ||
                           accountingFailure(state, error);
                case CapacityUpdated:
                    return setSectionJson(
                               state, detail::StateSizeSection::Capacity, encodeProjected(state.capacity, encodeCapacityState)) ||
                           accountingFailure(state, error);
                case DiagnosticsUpdated:
                    return refreshProjectedCollectionSection(
                               state, detail::StateSizeSection::Diagnostics, state.diagnostics, state.sizeLedger.diagnostics) ||
                           accountingFailure(state, error);
                case ThreadUpserted:
                case ThreadRemoved:
                case TurnUpserted:
                case ItemUpserted:
                case ItemContentUpdated:
                case PendingRequestsUpdated:
                    return true;
            }
            return true;
        }

        bool refreshCommitStateSections(detail::StateStorage& state) noexcept {
            // Ordinary reducer candidates inherit an initialized compact
            // ledger. Rebuilding here would silently put canonical whole-State
            // serialization back on the live-event admission path.
            if (!state.sizeLedger.initialized)
                return false;
            try {
                const auto optionalSection = [&state](detail::StateSizeSection section, std::optional<frontend::Json> value) {
                    return setSectionJson(state, section, value);
                };
                if (!setSectionJson(state, detail::StateSizeSection::Revision, frontend::Json(state.revision)) ||
                    !setSectionJson(state, detail::StateSizeSection::Freshness, frontend::Json(static_cast<unsigned>(state.freshness))) ||
                    !setSectionJson(state,
                                    detail::StateSizeSection::RepresentationMode,
                                    frontend::Json(static_cast<unsigned>(state.representationMode))) ||
                    !optionalSection(detail::StateSizeSection::VisibleSequence,
                                     state.visibleSequence ? std::optional<frontend::Json>{frontend::Json(state.visibleSequence->value())}
                                                           : std::nullopt) ||
                    !optionalSection(detail::StateSizeSection::SynchronizedThrough,
                                     state.synchronizedThrough
                                         ? std::optional<frontend::Json>{frontend::Json(state.synchronizedThrough->value())}
                                         : std::nullopt) ||
                    !setSectionJson(
                        state, detail::StateSizeSection::ThreadProjectionPresent, frontend::Json(state.threadProjectionPresent)) ||
                    !setSectionJson(state, detail::StateSizeSection::TurnProjectionPresent, frontend::Json(state.turnProjectionPresent)) ||
                    !setSectionJson(state, detail::StateSizeSection::ItemProjectionPresent, frontend::Json(state.itemProjectionPresent)) ||
                    !setSectionJson(state,
                                    detail::StateSizeSection::PendingRequestProjectionPresent,
                                    frontend::Json(state.pendingRequestProjectionPresent)))
                    return false;
            } catch (...) {
                state.sizeLedger.failed = true;
                return false;
            }

            const std::size_t internalSequences = static_cast<std::size_t>(state.retainedReplayThrough.has_value()) +
                                                  static_cast<std::size_t>(state.lastSynchronizationBatchSequence.has_value());
            if (internalSequences > std::numeric_limits<std::size_t>::max() / sizeof(frontend::SequenceNumber)) {
                state.sizeLedger.failed = true;
                return false;
            }
            state.sizeLedger.internalSequenceBytes = internalSequences * sizeof(frontend::SequenceNumber);
            return true;
        }

        bool refreshConnectionStateSections(detail::StateStorage& state) noexcept {
            try {
                const std::optional<frontend::Json> session =
                    state.session ? std::optional<frontend::Json>{encodeSessionInfo(*state.session)} : std::nullopt;
                const std::optional<frontend::Json> fingerprint =
                    state.projectionFingerprint ? std::optional<frontend::Json>{frontend::Json(state.projectionFingerprint->canonical)}
                                                : std::nullopt;
                return setSectionJson(state, detail::StateSizeSection::Session, session) &&
                       setSectionJson(state, detail::StateSizeSection::ProjectionFingerprint, fingerprint);
            } catch (...) {
                state.sizeLedger.failed = true;
                return false;
            }
        }

        bool refreshProjectionMetadataSection(detail::StateStorage& state) noexcept {
            try {
                const std::optional<frontend::Json> metadata =
                    state.projectionMetadata.omittedFields.empty() && state.projectionMetadata.redactedFields.empty()
                        ? std::nullopt
                        : std::optional<frontend::Json>{encodeProjectionMetadata(state.projectionMetadata)};
                return setSectionJson(state, detail::StateSizeSection::ProjectionMetadata, metadata);
            } catch (...) {
                state.sizeLedger.failed = true;
                return false;
            }
        }

        bool accountingFailure(detail::StateStorage& state, std::string& error) noexcept {
            state.sizeLedger.failed = true;
            error = "decoded frontend state size accounting failed";
            return false;
        }

        bool accountDirectEntityMutation(detail::StateStorage& state,
                                         detail::StateSizeSection section,
                                         detail::StateArrayContribution& contribution,
                                         std::optional<std::size_t> oldBytes,
                                         std::optional<std::size_t> newBytes,
                                         std::string& error) noexcept {
            if (!adjustArrayContribution(contribution, oldBytes, newBytes) || !refreshDirectArraySection(state, section, contribution))
                return accountingFailure(state, error);
            return true;
        }

        template <typename Collection>
        bool accountProjectedEntityMutation(detail::StateStorage& state,
                                            detail::StateSizeSection section,
                                            detail::StateArrayContribution& contribution,
                                            const Projected<Collection>& projected,
                                            std::optional<std::size_t> oldBytes,
                                            std::optional<std::size_t> newBytes,
                                            std::string& error) noexcept {
            if (!adjustArrayContribution(contribution, oldBytes, newBytes) ||
                !refreshProjectedCollectionSection(state, section, projected, contribution))
                return accountingFailure(state, error);
            return true;
        }

        template <typename T, typename Id, typename IdAccessor, typename Encoder>
        std::optional<std::size_t>
        encodedEntityBytesById(const std::vector<T>& values, const Id& id, IdAccessor idAccessor, Encoder encoder) {
            const auto found = std::find_if(values.begin(), values.end(), [&](const T& value) {
                return idAccessor(value) == id;
            });
            return found == values.end() ? std::nullopt : std::optional<std::size_t>{encodedEntityBytes(encoder(*found))};
        }

        std::optional<std::size_t> referenceStateBytes(const detail::StateStorage& state) noexcept {
            try {
                std::size_t bytes = encodeState(state).dump().size();
                const auto accountInternalSequence = [&bytes](const std::optional<frontend::SequenceNumber>& sequence) {
                    return !sequence || checkedAdd(bytes, sizeof(frontend::SequenceNumber), bytes);
                };
                if (!accountInternalSequence(state.retainedReplayThrough) ||
                    !accountInternalSequence(state.lastSynchronizationBatchSequence))
                    return std::nullopt;
                return bytes;
            } catch (...) {
                return std::nullopt;
            }
        }

        std::optional<std::size_t> accountedStateBytes(const detail::StateStorage& state) noexcept {
            if (!state.sizeLedger.initialized || state.sizeLedger.failed)
                return std::nullopt;
            std::size_t total = 0;
            if (!checkedAdd(state.sizeLedger.canonicalBytes, state.sizeLedger.internalSequenceBytes, total))
                return std::nullopt;
            return total;
        }

#ifndef NDEBUG
        thread_local std::size_t DebugAccountingVerificationCount = 0;
#endif

        bool stateFits(const detail::StateStorage& state, std::size_t maximumBytes, std::string& error) {
            const std::optional<std::size_t> encodedBytes = accountedStateBytes(state);
            if (!encodedBytes) {
                error = "decoded frontend state size accounting failed";
                return false;
            }
#ifndef NDEBUG
            const std::optional<std::size_t> referenceBytes = referenceStateBytes(state);
            if (!referenceBytes || *referenceBytes != *encodedBytes)
                std::fprintf(stderr,
                             "frontend State accounting invariant failed: revision=%llu accounted=%zu reference=%zu\n",
                             static_cast<unsigned long long>(state.revision),
                             *encodedBytes,
                             referenceBytes.value_or(0));
            assert(referenceBytes && *referenceBytes == *encodedBytes);
            ++DebugAccountingVerificationCount;
#endif
            if (*encodedBytes <= maximumBytes)
                return true;
            error = "decoded frontend state exceeds maximumDecodedStateBytes";
            return false;
        }

        bool advanceRevision(detail::StateStorage& state, std::string_view operation, std::string& error) {
            if (state.revision == std::numeric_limits<std::uint64_t>::max()) {
                error = "frontend state revision exhausted while ";
                error.append(operation);
                return false;
            }
            ++state.revision;
            return true;
        }

        bool validateExpandedEvent(const frontend::FrontendEvent& event, frontend::ExpandedFrontendEvent& decoded, std::string& error) {
            const auto encoded = frontend::Codec::encodeEvent(event);
            if (!encoded) {
                error = encoded.error().message;
                return false;
            }
            const auto expanded = frontend::Codec::decodeExpandedEvent(encoded.value());
            if (!expanded) {
                error = expanded.error().message;
                return false;
            }
            decoded = expanded.value();
            return true;
        }

        bool
        replaceContent(detail::StateStorage& state, const frontend::Json& data, ItemContentReplacedChange& change, std::string& error) {
            const auto itemId = stringMember(data, "itemId");
            const auto content = stringMember(data, "content");
            const auto channelName = stringMember(data, "channel");
            const auto channel = channelName ? itemContentChannel(*channelName) : std::nullopt;
            if (!itemId || itemId->empty() || !content || !channelName) {
                error = "item content update lacks itemId, channel, or content";
                return false;
            }
            if (!channel) {
                error = "item content update uses an unknown channel";
                return false;
            }
            const auto found = std::find_if(state.items.begin(), state.items.end(), [&](const ItemState& item) {
                return item.id.value == *itemId;
            });
            if (found == state.items.end()) {
                error = "item content update references an unknown item";
                return false;
            }
            const auto threadId = stringMember(data, "threadId");
            const auto turnId = stringMember(data, "turnId");
            if (threadId && found->threadId && found->threadId->value != *threadId) {
                error = "item content update threadId does not match the item";
                return false;
            }
            if (turnId && found->turnId && found->turnId->value != *turnId) {
                error = "item content update turnId does not match the item";
                return false;
            }
            switch (*channel) {
                case ItemContentChannel::AgentText:
                    found->agentText = *content;
                    break;
                case ItemContentChannel::ReasoningText:
                    found->reasoningText = *content;
                    break;
                case ItemContentChannel::ReasoningSummary:
                    found->reasoningSummary = *content;
                    break;
                case ItemContentChannel::CommandOutput:
                    found->commandOutput = *content;
                    break;
            }
            if (const auto dropped = data.find("droppedContentBytes"); dropped != data.end()) {
                const auto decoded = optionalUnsigned(data, "droppedContentBytes");
                if (!decoded) {
                    error = "item content update has invalid droppedContentBytes";
                    return false;
                }
                found->droppedContentBytes = *decoded;
            }
            if (const auto truncated = data.find("contentTruncated"); truncated != data.end()) {
                const auto decoded = optionalBool(data, "contentTruncated");
                if (!decoded) {
                    error = "item content update has invalid contentTruncated";
                    return false;
                }
                found->contentTruncated = *decoded;
            }
            change = ItemContentReplacedChange{typed::ItemId{*itemId}, *channel};
            return true;
        }

        bool applyExpanded(detail::StateStorage& state,
                           const frontend::ExpandedFrontendEvent& event,
                           std::size_t maximumRetainedDiagnostics,
                           std::vector<Change>& changes,
                           std::string& error) {
            const frontend::Json& data = event.data;
            const auto domainProjection = [](const auto& value) {
                return encodeTypedDomain(value);
            };
            using enum frontend::ExpandedEventType;
            switch (event.type) {
                case ProviderUpdated: {
                    const frontend::Json* value = nullptr;
                    ProviderState decoded;
                    if (!requireObjectMember(data, "provider", value, error) || !decodeProvider(*value, decoded, error))
                        return false;
                    state.provider.value = decoded;
                    if (!setSectionJson(state, detail::StateSizeSection::Provider, encodeProjected(state.provider, encodeProvider)))
                        return accountingFailure(state, error);
                    changes.push_back(ProviderUpdatedChange{std::move(decoded)});
                    return true;
                }
                case ControllerUpdated: {
                    const frontend::Json* value = nullptr;
                    ControllerState decoded;
                    if (!requireObjectMember(data, "controller", value, error) || !decodeController(*value, state.session, decoded, error))
                        return false;
                    state.controller.value = decoded;
                    if (!setSectionJson(
                            state, detail::StateSizeSection::Controller, encodeProjected(state.controller, encodeControllerState)))
                        return accountingFailure(state, error);
                    changes.push_back(ControllerUpdatedChange{std::move(decoded)});
                    return true;
                }
                case SessionsUpdated: {
                    const frontend::Json* values = nullptr;
                    if (!requireArrayMember(data, "sessions", values, error))
                        return false;
                    std::vector<SessionState> decoded;
                    for (const frontend::Json& value : *values) {
                        SessionState session;
                        if (!decodeSession(value, session, error))
                            return false;
                        if (!appendDistinct(
                                decoded,
                                std::move(session),
                                [](const SessionState& entry) {
                                    return entry.sessionId;
                                },
                                "session collection",
                                error))
                            return false;
                    }
                    state.sessions.value = std::move(decoded);
                    if (!refreshProjectedSessionsSection(state))
                        return accountingFailure(state, error);
                    changes.push_back(SessionsUpdatedChange{});
                    return true;
                }
                case ThreadListUpdated: {
                    const frontend::Json* value = nullptr;
                    ThreadListState decoded;
                    if (!requireObjectMember(data, "threadList", value, error) || !decodeThreadList(*value, decoded, error))
                        return false;
                    state.threadList.value = std::move(decoded);
                    if (!setSectionJson(
                            state, detail::StateSizeSection::ThreadList, encodeProjected(state.threadList, encodeThreadListState)))
                        return accountingFailure(state, error);
                    changes.push_back(ThreadListUpdatedChange{});
                    return true;
                }
                case ThreadUpserted: {
                    state.threadProjectionPresent = true;
                    const frontend::Json* value = nullptr;
                    ThreadState decoded;
                    if (!requireObjectMember(data, "thread", value, error) || !decodeThread(*value, decoded, error))
                        return false;
                    const auto prior = std::find_if(state.threads.begin(), state.threads.end(), [&](const ThreadState& thread) {
                        return thread.id == decoded.id;
                    });
                    const bool existed = prior != state.threads.end();
                    const std::optional<std::size_t> oldBytes =
                        existed ? std::optional<std::size_t>{encodedEntityBytes(encodeThreadState(*prior))} : std::nullopt;
                    if (existed && value->find("turns") == value->end())
                        decoded.orderedTurns = prior->orderedTurns;
                    const typed::ThreadId id = decoded.id;
                    const std::optional<std::size_t> newBytes = encodedEntityBytes(encodeThreadState(decoded));
                    upsert(state.threads, std::move(decoded), [](const ThreadState& thread) {
                        return thread.id;
                    });
                    if (existed && !oldBytes)
                        return accountingFailure(state, error);
                    if (!newBytes || !accountDirectEntityMutation(
                                         state, detail::StateSizeSection::Threads, state.sizeLedger.threads, oldBytes, newBytes, error))
                        return accountingFailure(state, error);
                    changes.push_back(ThreadUpsertedChange{id});
                    return true;
                }
                case ThreadRemoved: {
                    state.threadProjectionPresent = true;
                    const auto idValue = stringMember(data, "threadId");
                    if (!idValue || idValue->empty()) {
                        error = "thread removal lacks a nonempty threadId";
                        return false;
                    }
                    const typed::ThreadId id{*idValue};
                    std::vector<typed::TurnId> removedTurns;
                    for (const TurnState& turn : state.turns) {
                        if (turn.threadId == id)
                            removedTurns.push_back(turn.id);
                    }
                    std::erase_if(state.items, [&](const ItemState& item) {
                        return item.threadId == std::optional(id) ||
                               (item.turnId && std::find(removedTurns.begin(), removedTurns.end(), *item.turnId) != removedTurns.end());
                    });
                    std::erase_if(state.turns, [&](const TurnState& turn) {
                        return turn.threadId == id;
                    });
                    std::erase_if(state.threads, [&](const ThreadState& thread) {
                        return thread.id == id;
                    });
                    if (!rebuildDirectArraySection(
                            state, detail::StateSizeSection::Threads, state.sizeLedger.threads, state.threads, encodeThreadState) ||
                        !rebuildDirectArraySection(
                            state, detail::StateSizeSection::Turns, state.sizeLedger.turns, state.turns, encodeTurnState) ||
                        !rebuildDirectArraySection(
                            state, detail::StateSizeSection::Items, state.sizeLedger.items, state.items, encodeItemState))
                        return accountingFailure(state, error);
                    changes.push_back(ThreadRemovedChange{id});
                    return true;
                }
                case TurnUpserted: {
                    state.turnProjectionPresent = true;
                    const frontend::Json* value = nullptr;
                    TurnState decoded;
                    if (!requireObjectMember(data, "turn", value, error) || !decodeTurn(*value, decoded, error))
                        return false;
                    const auto prior = std::find_if(state.turns.begin(), state.turns.end(), [&](const TurnState& turn) {
                        return turn.id == decoded.id;
                    });
                    const std::optional<std::size_t> oldTurnBytes =
                        prior == state.turns.end() ? std::nullopt : std::optional<std::size_t>{encodedEntityBytes(encodeTurnState(*prior))};
                    const std::optional<typed::ThreadId> priorThread =
                        prior == state.turns.end() ? std::nullopt : std::optional(prior->threadId);
                    if (prior != state.turns.end() && value->find("items") == value->end())
                        decoded.orderedItems = prior->orderedItems;
                    const typed::TurnId id = decoded.id;
                    const typed::ThreadId threadId = decoded.threadId;
                    const std::optional<std::size_t> newTurnBytes = encodedEntityBytes(encodeTurnState(decoded));
                    const auto threadBytes = [&state](const typed::ThreadId& parentId) {
                        return encodedEntityBytesById(
                            state.threads,
                            parentId,
                            [](const ThreadState& entry) {
                                return entry.id;
                            },
                            encodeThreadState);
                    };
                    const std::optional<std::size_t> oldPriorThreadBytes = priorThread ? threadBytes(*priorThread) : std::nullopt;
                    const std::optional<std::size_t> oldThreadBytes =
                        priorThread && *priorThread == threadId ? oldPriorThreadBytes : threadBytes(threadId);
                    upsert(state.turns, std::move(decoded), [](const TurnState& turn) {
                        return turn.id;
                    });
                    if (priorThread && *priorThread != threadId) {
                        const auto oldThread = std::find_if(state.threads.begin(), state.threads.end(), [&](const ThreadState& entry) {
                            return entry.id == *priorThread;
                        });
                        if (oldThread != state.threads.end())
                            std::erase(oldThread->orderedTurns, id);
                    }
                    const auto thread = std::find_if(state.threads.begin(), state.threads.end(), [&](const ThreadState& entry) {
                        return entry.id == threadId;
                    });
                    if (thread != state.threads.end())
                        appendUnique(thread->orderedTurns, id);
                    const std::optional<std::size_t> newPriorThreadBytes = priorThread ? threadBytes(*priorThread) : std::nullopt;
                    const std::optional<std::size_t> newThreadBytes =
                        priorThread && *priorThread == threadId ? newPriorThreadBytes : threadBytes(threadId);
                    if (!newTurnBytes ||
                        !accountDirectEntityMutation(
                            state, detail::StateSizeSection::Turns, state.sizeLedger.turns, oldTurnBytes, newTurnBytes, error) ||
                        (priorThread && !accountDirectEntityMutation(state,
                                                                     detail::StateSizeSection::Threads,
                                                                     state.sizeLedger.threads,
                                                                     oldPriorThreadBytes,
                                                                     newPriorThreadBytes,
                                                                     error)) ||
                        ((!priorThread || *priorThread != threadId) &&
                         !accountDirectEntityMutation(
                             state, detail::StateSizeSection::Threads, state.sizeLedger.threads, oldThreadBytes, newThreadBytes, error)))
                        return accountingFailure(state, error);
                    changes.push_back(TurnUpsertedChange{id});
                    return true;
                }
                case ItemUpserted: {
                    state.itemProjectionPresent = true;
                    const frontend::Json* value = nullptr;
                    ItemState decoded;
                    if (!requireObjectMember(data, "item", value, error) || !decodeExpandedItem(*value, decoded, error))
                        return false;
                    const typed::ItemId id = decoded.id;
                    const std::optional<typed::TurnId> turnId = decoded.turnId;
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
                            [](const TurnState& entry) {
                                return entry.id;
                            },
                            encodeTurnState);
                    };
                    const std::optional<std::size_t> oldPriorTurnBytes = priorTurn ? turnBytes(*priorTurn) : std::nullopt;
                    const std::optional<std::size_t> oldTurnBytes =
                        turnId && priorTurn && *turnId == *priorTurn ? oldPriorTurnBytes : (turnId ? turnBytes(*turnId) : std::nullopt);
                    upsert(state.items, std::move(decoded), [](const ItemState& item) {
                        return item.id;
                    });
                    if (priorTurn && priorTurn != turnId) {
                        const auto oldTurn = std::find_if(state.turns.begin(), state.turns.end(), [&](const TurnState& entry) {
                            return entry.id == *priorTurn;
                        });
                        if (oldTurn != state.turns.end())
                            std::erase(oldTurn->orderedItems, id);
                    }
                    if (turnId) {
                        const auto turn = std::find_if(state.turns.begin(), state.turns.end(), [&](const TurnState& entry) {
                            return entry.id == *turnId;
                        });
                        if (turn != state.turns.end())
                            appendUnique(turn->orderedItems, id);
                    }
                    const std::optional<std::size_t> newPriorTurnBytes = priorTurn ? turnBytes(*priorTurn) : std::nullopt;
                    const std::optional<std::size_t> newTurnBytes =
                        turnId && priorTurn && *turnId == *priorTurn ? newPriorTurnBytes : (turnId ? turnBytes(*turnId) : std::nullopt);
                    if (!newItemBytes ||
                        !accountDirectEntityMutation(
                            state, detail::StateSizeSection::Items, state.sizeLedger.items, oldItemBytes, newItemBytes, error) ||
                        (priorTurn && !accountDirectEntityMutation(state,
                                                                   detail::StateSizeSection::Turns,
                                                                   state.sizeLedger.turns,
                                                                   oldPriorTurnBytes,
                                                                   newPriorTurnBytes,
                                                                   error)) ||
                        (turnId && (!priorTurn || *turnId != *priorTurn) &&
                         !accountDirectEntityMutation(
                             state, detail::StateSizeSection::Turns, state.sizeLedger.turns, oldTurnBytes, newTurnBytes, error)))
                        return accountingFailure(state, error);
                    changes.push_back(ItemUpsertedChange{id});
                    return true;
                }
                case ItemContentUpdated: {
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
                        !accountDirectEntityMutation(
                            state, detail::StateSizeSection::Items, state.sizeLedger.items, oldBytes, newBytes, error))
                        return accountingFailure(state, error);
                    changes.push_back(std::move(change));
                    return true;
                }
                case PendingRequestsUpdated: {
                    state.pendingRequestProjectionPresent = true;
                    const frontend::Json* values = nullptr;
                    if (!requireArrayMember(data, "pendingRequests", values, error))
                        return false;
                    std::vector<PendingRequestState> decoded;
                    for (const frontend::Json& value : *values) {
                        PendingRequestState request;
                        if (!decodePendingRequestJson(value, request, error, true))
                            return false;
                        if (!appendDistinct(
                                decoded,
                                std::move(request),
                                [](const PendingRequestState& entry) {
                                    return entry.id;
                                },
                                "pending-request collection",
                                error))
                            return false;
                    }
                    state.pendingRequests = std::move(decoded);
                    if (!rebuildDirectArraySection(state,
                                                   detail::StateSizeSection::PendingRequests,
                                                   state.sizeLedger.pendingRequests,
                                                   state.pendingRequests,
                                                   encodePendingRequestState))
                        return accountingFailure(state, error);
                    changes.push_back(PendingRequestsUpdatedChange{});
                    return true;
                }
                case AccountUpdated:
                case ModelsUpdated:
                case ConfigurationUpdated:
                case ReviewsUpdated:
                case IntegrationsUpdated:
                case PluginsUpdated:
                case SkillsUpdated:
                case McpUpdated:
                case PlatformUpdated: {
                    const frontend::Json* value = nullptr;
                    if (!requireObjectMember(data, "domain", value, error))
                        return false;
                    if (event.type == AccountUpdated) {
                        if (!decodeDomainWrapper(*value, state.accounts, error))
                            return false;
                        if (!setSectionJson(state, detail::StateSizeSection::Accounts, encodeProjected(state.accounts, domainProjection)))
                            return accountingFailure(state, error);
                        changes.push_back(AccountUpdatedChange{});
                    } else if (event.type == ModelsUpdated) {
                        if (!decodeDomainWrapper(*value, state.models, error))
                            return false;
                        if (!setSectionJson(state, detail::StateSizeSection::Models, encodeProjected(state.models, domainProjection)))
                            return accountingFailure(state, error);
                        changes.push_back(ModelsUpdatedChange{});
                    } else if (event.type == ConfigurationUpdated) {
                        if (!decodeDomainWrapper(*value, state.configuration, error))
                            return false;
                        if (!setSectionJson(
                                state, detail::StateSizeSection::Configuration, encodeProjected(state.configuration, domainProjection)))
                            return accountingFailure(state, error);
                        changes.push_back(ConfigurationUpdatedChange{});
                    } else if (event.type == ReviewsUpdated) {
                        if (!decodeDomainWrapper(*value, state.reviews, error))
                            return false;
                        state.permissionProfiles.value = PermissionProfilesState{state.reviews.value->projection};
                        state.permissionProfiles.truncated = state.reviews.truncated;
                        state.permissionProfiles.omittedFields = state.reviews.omittedFields;
                        if (!setSectionJson(state, detail::StateSizeSection::Reviews, encodeProjected(state.reviews, domainProjection)) ||
