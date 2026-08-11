                return false;
            }
            for (const frontend::Json& session : *sessions) {
                if (!requireObject(session, "legacy snapshot session", error) || !requireDecimalIdMember(session, "sessionId", error) ||
                    !requireStringMember(session, "role", true, error))
                    return false;
                const auto role = stringMember(session, "role");
                if (*role != "observer" && *role != "controller") {
                    error = "legacy snapshot session role is not a defined stable value";
                    return false;
                }
            }

            const auto extensions = state.find("codexExtensions");
            if (extensions == state.end() || !extensions->is_array() || extensions->size() > 64) {
                error = "legacy snapshot codexExtensions must be an array of at most 64 entries";
                return false;
            }
            for (const frontend::Json& extension : *extensions) {
                if (!validateLegacyEvent(frontend::FrontendEvent{frontend::SequenceNumber{1}, "codex.extension", extension}, error))
                    return false;
            }

            const auto threadList = state.find("threadList");
            if (threadList == state.end() || !threadList->is_object() ||
                !validateLegacyEvent(frontend::FrontendEvent{frontend::SequenceNumber{1}, "thread.list.updated", *threadList}, error))
                return false;

            const auto journal = state.find("journal");
            if (journal == state.end() || !journal->is_object() || !requireUnsignedMember(*journal, "oldestReplayableAfter", error) ||
                !requireUnsignedMember(*journal, "currentSequence", error)) {
                if (error.empty())
                    error = "legacy snapshot journal lacks required cursor fields";
                return false;
            }
            for (const std::string_view key : {"oldestRetainedSequence", "newestRetainedSequence"}) {
                if (journal->contains(std::string(key)) && !requireUnsignedMember(*journal, key, error))
                    return false;
            }
            const std::uint64_t replayFloor = *optionalUnsigned(*journal, "oldestReplayableAfter");
            const std::uint64_t currentSequence = *optionalUnsigned(*journal, "currentSequence");
            const auto oldestRetained = optionalUnsigned(*journal, "oldestRetainedSequence");
            const auto newestRetained = optionalUnsigned(*journal, "newestRetainedSequence");
            if (replayFloor > currentSequence || (oldestRetained && *oldestRetained > currentSequence) ||
                (newestRetained && *newestRetained > currentSequence) ||
                (oldestRetained && newestRetained && *oldestRetained > *newestRetained)) {
                error = "legacy snapshot journal cursors are inconsistent";
                return false;
            }

            const auto validateDomainMembers = [&error](const frontend::Json& domains, std::string_view context) {
                for (const std::string_view name :
                     {"accounts", "models", "configuration", "reviews", "integrations", "pluginsAndSkills", "mcp", "platform"}) {
                    const auto value = domains.find(std::string(name));
                    if (value != domains.end() && !value->is_object()) {
                        error = std::string(context) + " member '" + std::string(name) + "' must be an object";
                        return false;
                    }
                }
                return true;
            };
            if (!validateDomainMembers(state, "legacy snapshot domain"))
                return false;
            if (const auto domains = state.find("domains"); domains != state.end()) {
                if (!domains->is_object()) {
                    error = "legacy snapshot domains must be an object";
                    return false;
                }
                if (!validateDomainMembers(*domains, "legacy snapshot domains"))
                    return false;
            }

            const auto validateCollection = [&state, &error](std::string_view name) {
                const auto value = state.find(std::string(name));
                if (value == state.end())
                    return true;
                if (value->is_array())
                    return true;
                if (!value->is_object()) {
                    error = "legacy snapshot collection '" + std::string(name) + "' must be an array or object";
                    return false;
                }
                const auto entries = value->find("entries");
                if (entries == value->end() || !entries->is_array()) {
                    error = "legacy snapshot collection '" + std::string(name) + "' must contain an entries array";
                    return false;
                }
                if (const auto truncation = value->find("truncation"); truncation != value->end()) {
                    TruncationMetadata decoded;
                    if (!decodeTruncation(*truncation, decoded, error)) {
                        error = "legacy snapshot collection '" + std::string(name) + "' has invalid truncation metadata: " + error;
                        return false;
                    }
                }
                return true;
            };
            for (const std::string_view name :
                 {"processes", "filesystemWatches", "fuzzySearches", "fuzzySearchSessions", "notices", "activities"}) {
                if (!validateCollection(name))
                    return false;
            }

            if (const auto capacity = state.find("capacity"); capacity != state.end()) {
                if (!capacity->is_object()) {
                    error = "legacy snapshot capacity must be an object";
                    return false;
                }
                for (const std::string_view name : {"sessions",
                                                    "observers",
                                                    "activeOperations",
                                                    "pendingRequests",
                                                    "retainedThreads",
                                                    "retainedTurns",
                                                    "retainedItems",
                                                    "accumulatedContentBytes",
                                                    "retainedNotices",
                                                    "retainedProcesses",
                                                    "accumulatedProcessOutputBytes",
                                                    "retainedFilesystemWatches",
                                                    "retainedFuzzySearchSessions",
                                                    "retainedActivityRecords",
                                                    "evictedNotices",
                                                    "evictedProcesses",
                                                    "droppedProcessOutputBytes",
                                                    "evictedFilesystemWatches",
                                                    "evictedFuzzySearchSessions",
                                                    "evictedActivityRecords"}) {
                    if (capacity->contains(std::string(name)) && !optionalSize(*capacity, name)) {
                        error = "legacy snapshot capacity member '" + std::string(name) + "' must be an unsigned integer";
                        return false;
                    }
                }
            }
            return true;
        }

        std::optional<frontend::Json> sanitizeLegacySnapshot(const frontend::Json& state, std::string& error) {
            frontend::detail::CanonicalSnapshotRecord record;
            record.sequence = frontend::SequenceNumber{1};
            record.legacyState.value = state;
            record = frontend::detail::canonicalizeSnapshot(std::move(record));
            if (record.sanitization.failed || !record.legacyState.value.is_object()) {
                error = "legacy snapshot could not be bounded and sanitized";
                return std::nullopt;
            }
            return std::optional<frontend::Json>{std::move(record.legacyState.value)};
        }

        std::optional<frontend::Json> sanitizeLegacyEventData(const frontend::FrontendEvent& event, std::string& error) {
            frontend::detail::CanonicalEventRecord record;
            record.sequence = event.sequence;
            record.legacyType = event.type;
            record.legacyData.value = event.data;
            record = frontend::detail::canonicalizeEvent(std::move(record));
            if (record.sanitization.failed || !record.legacyData.value.is_object()) {
                error = "legacy event could not be bounded and sanitized";
                return std::nullopt;
            }
            return std::optional<frontend::Json>{std::move(record.legacyData.value)};
        }

        bool decodeDomain(const frontend::Json& value,
                          DomainProjectionState& result,
                          std::string& error,
                          const frontend::Json** detailsValue = nullptr) {
            if (!requireObject(value, "domain projection", error))
                return false;
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                SourceStamp decoded;
                if (!decodeStamp(*stamp, decoded, error))
                    return false;
                result.stamp = decoded;
            }
            result.status = stringMember(value, "status");
            result.summary = stringMember(value, "summary");
            result.nextCursor = stringMember(value, "nextCursor");
            result.complete = optionalBool(value, "complete");
            result.itemCount = optionalSize(value, "itemCount");
            if (const auto details = value.find("details"); details != value.end()) {
                if (!details->is_object()) {
                    error = "domain details must be an object";
                    return false;
                }
                result.notificationCount = optionalSize(*details, "notificationCount");
                if (details->contains("notificationCount") && !result.notificationCount) {
                    error = "domain notificationCount must be an unsigned integer";
                    return false;
                }
                if (const auto methods = details->find("latestNotificationMethods"); methods != details->end()) {
                    if (!methods->is_array()) {
                        error = "domain latestNotificationMethods must be an array";
                        return false;
                    }
                    for (const frontend::Json& method : *methods) {
                        if (!method.is_string()) {
                            error = "domain latestNotificationMethods must contain strings";
                            return false;
                        }
                        result.latestNotificationMethods.push_back(method.get<std::string>());
                    }
                }
                result.opaqueDetails = extensionsOf(*details, {"notificationCount", "latestNotificationMethods"});
                if (detailsValue)
                    *detailsValue = &*details;
            }
            if (const auto truncation = value.find("truncation"); truncation != value.end()) {
                TruncationMetadata decoded;
                if (!decodeTruncation(*truncation, decoded, error))
                    return false;
                result.truncation = decoded;
            }
            if (const auto latest = value.find("latestResults"); latest != value.end()) {
                if (!latest->is_array()) {
                    error = "domain latestResults must be an array";
                    return false;
                }
                for (const frontend::Json& entry : *latest) {
                    const auto method = stringMember(entry, "method");
                    const auto status = stringMember(entry, "status");
                    const auto stamp = entry.find("stamp");
                    if (!method || !status || stamp == entry.end()) {
                        error = "domain result summary lacks required fields";
                        return false;
                    }
                    DomainResultSummaryState decoded;
                    decoded.method = *method;
                    decoded.status = *status;
                    decoded.subjectId = stringMember(entry, "subjectId");
                    decoded.nextCursor = stringMember(entry, "nextCursor");
                    decoded.itemCount = optionalSize(entry, "itemCount");
                    decoded.complete = optionalBool(entry, "complete");
                    if (!decodeStamp(*stamp, decoded.stamp, error))
                        return false;
                    decoded.extensions =
                        extensionsOf(entry, {"method", "status", "subjectId", "nextCursor", "itemCount", "complete", "stamp"});
                    result.latestResults.push_back(std::move(decoded));
                }
            }
            result.extensions = extensionsOf(
                value, {"stamp", "status", "summary", "nextCursor", "complete", "itemCount", "latestResults", "details", "truncation"});
            return true;
        }

        template <typename T>
        bool decodeSpecificDomainDetails(const frontend::Json*, T&, std::string&) {
            return true;
        }

        bool decodeSpecificDomainDetails(const frontend::Json* value, AccountState& result, std::string& error) {
            if (!value)
                return true;
#define AISUITE_ACCOUNT_DETAIL_BOOL(name)                                                                                                  \
    result.details.name = optionalBool(*value, #name);                                                                                     \
    if (value->contains(#name) && !result.details.name) {                                                                                  \
        error = "account domain detail '" #name "' must be boolean";                                                                       \
        return false;                                                                                                                      \
    }
#define AISUITE_ACCOUNT_DETAIL_STRING(name)                                                                                                \
    result.details.name = stringMember(*value, #name);                                                                                     \
    if (value->contains(#name) && !result.details.name) {                                                                                  \
        error = "account domain detail '" #name "' must be a string";                                                                      \
        return false;                                                                                                                      \
    }
            AISUITE_ACCOUNT_DETAIL_BOOL(loggedOut)
            AISUITE_ACCOUNT_DETAIL_STRING(loginLifecycle)
            AISUITE_ACCOUNT_DETAIL_STRING(loginMethod)
            AISUITE_ACCOUNT_DETAIL_BOOL(loginSucceeded)
            AISUITE_ACCOUNT_DETAIL_BOOL(authenticated)
            AISUITE_ACCOUNT_DETAIL_STRING(accountType)
            if (const auto authMode = stringMember(*value, "authMode"))
                result.details.authMode = typed::AuthMode{*authMode};
            if (value->contains("authMode") && !result.details.authMode) {
                error = "account domain detail 'authMode' must be a string";
                return false;
            }
            if (const auto planType = stringMember(*value, "planType"))
                result.details.planType = typed::PlanType{*planType};
            if (value->contains("planType") && !result.details.planType) {
                error = "account domain detail 'planType' must be a string";
                return false;
            }
            result.details.primaryUsedPercent = optionalNumber(*value, "primaryUsedPercent");
            result.details.secondaryUsedPercent = optionalNumber(*value, "secondaryUsedPercent");
            if ((value->contains("primaryUsedPercent") && !result.details.primaryUsedPercent) ||
                (value->contains("secondaryUsedPercent") && !result.details.secondaryUsedPercent)) {
                error = "account utilization detail must be numeric";
                return false;
            }
            AISUITE_ACCOUNT_DETAIL_BOOL(hasCredits)
#undef AISUITE_ACCOUNT_DETAIL_STRING
#undef AISUITE_ACCOUNT_DETAIL_BOOL
            result.projection.opaqueDetails = extensionsOf(*value,
                                                           {"notificationCount",
                                                            "latestNotificationMethods",
                                                            "loggedOut",
                                                            "loginLifecycle",
                                                            "loginMethod",
                                                            "loginSucceeded",
                                                            "authenticated",
                                                            "accountType",
                                                            "authMode",
                                                            "planType",
                                                            "primaryUsedPercent",
                                                            "secondaryUsedPercent",
                                                            "hasCredits"});
            return true;
        }

        bool decodeSpecificDomainDetails(const frontend::Json* value, ConfigurationState& result, std::string& error) {
            if (!value)
                return true;
            if (const auto path = stringMember(*value, "filePath"))
                result.details.filePath = typed::AbsolutePath{*path};
            if (const auto writeStatus = stringMember(*value, "writeStatus"))
                result.details.writeStatus = typed::WriteStatus{*writeStatus};
            result.details.writeVersion = stringMember(*value, "writeVersion");
            result.details.writeOverridden = optionalBool(*value, "writeOverridden");
            result.details.featureCount = optionalSize(*value, "featureCount");
            result.details.featureListTruncated = optionalBool(*value, "featureListTruncated");
            if ((value->contains("filePath") && !result.details.filePath) ||
                (value->contains("writeStatus") && !result.details.writeStatus) ||
                (value->contains("writeVersion") && !result.details.writeVersion) ||
                (value->contains("writeOverridden") && !result.details.writeOverridden) ||
                (value->contains("featureCount") && !result.details.featureCount) ||
                (value->contains("featureListTruncated") && !result.details.featureListTruncated)) {
                error = "configuration domain detail has an invalid stable type";
                return false;
            }
            result.projection.opaqueDetails = extensionsOf(*value,
                                                           {"notificationCount",
                                                            "latestNotificationMethods",
                                                            "filePath",
                                                            "writeStatus",
                                                            "writeVersion",
                                                            "writeOverridden",
                                                            "featureCount",
                                                            "featureListTruncated"});
            return true;
        }

        template <typename T>
        bool decodeIntegrationDetails(const frontend::Json* value, T& result, std::string& error) {
            if (!value)
                return true;
            result.details.appCount = optionalSize(*value, "appCount");
            result.details.appListTruncated = optionalBool(*value, "appListTruncated");
            result.details.marketplaceAddStatus = stringMember(*value, "marketplaceAddStatus");
            result.details.marketplaceRemoveStatus = stringMember(*value, "marketplaceRemoveStatus");
            result.details.marketplaceUpgradeStatus = stringMember(*value, "marketplaceUpgradeStatus");
            if ((value->contains("appCount") && !result.details.appCount) ||
                (value->contains("appListTruncated") && !result.details.appListTruncated) ||
                (value->contains("marketplaceAddStatus") && !result.details.marketplaceAddStatus) ||
                (value->contains("marketplaceRemoveStatus") && !result.details.marketplaceRemoveStatus) ||
                (value->contains("marketplaceUpgradeStatus") && !result.details.marketplaceUpgradeStatus)) {
                error = "integration domain detail has an invalid stable type";
                return false;
            }
            result.projection.opaqueDetails = extensionsOf(*value,
                                                           {"notificationCount",
                                                            "latestNotificationMethods",
                                                            "appCount",
                                                            "appListTruncated",
                                                            "marketplaceAddStatus",
                                                            "marketplaceRemoveStatus",
                                                            "marketplaceUpgradeStatus"});
            return true;
        }

#define AISUITE_INTEGRATION_DETAIL_DECODER(typeName)                                                                                       \
    bool decodeSpecificDomainDetails(const frontend::Json* value, typeName& result, std::string& error) {                                  \
        return decodeIntegrationDetails(value, result, error);                                                                             \
    }
        AISUITE_INTEGRATION_DETAIL_DECODER(AppsState)
        AISUITE_INTEGRATION_DETAIL_DECODER(ExternalAgentsState)
        AISUITE_INTEGRATION_DETAIL_DECODER(HooksState)
        AISUITE_INTEGRATION_DETAIL_DECODER(MarketplaceState)
#undef AISUITE_INTEGRATION_DETAIL_DECODER

        template <typename T>
        bool decodePluginDetails(const frontend::Json* value, T& result, std::string& error) {
            if (!value)
                return true;
            result.details.lastPluginOperation = stringMember(*value, "lastPluginOperation");
            result.details.lastSkillsOperation = stringMember(*value, "lastSkillsOperation");
            result.details.extraRootCount = optionalSize(*value, "extraRootCount");
            result.details.extraRootsTruncated = optionalBool(*value, "extraRootsTruncated");
            if ((value->contains("lastPluginOperation") && !result.details.lastPluginOperation) ||
                (value->contains("lastSkillsOperation") && !result.details.lastSkillsOperation) ||
                (value->contains("extraRootCount") && !result.details.extraRootCount) ||
                (value->contains("extraRootsTruncated") && !result.details.extraRootsTruncated)) {
                error = "plugin/skill domain detail has an invalid stable type";
                return false;
            }
            result.projection.opaqueDetails = extensionsOf(*value,
                                                           {"notificationCount",
                                                            "latestNotificationMethods",
                                                            "lastPluginOperation",
                                                            "lastSkillsOperation",
                                                            "extraRootCount",
                                                            "extraRootsTruncated"});
            return true;
        }

        bool decodeSpecificDomainDetails(const frontend::Json* value, PluginsState& result, std::string& error) {
            return decodePluginDetails(value, result, error);
        }
        bool decodeSpecificDomainDetails(const frontend::Json* value, SkillsState& result, std::string& error) {
            return decodePluginDetails(value, result, error);
        }

        bool decodeSpecificDomainDetails(const frontend::Json* value, McpState& result, std::string& error) {
            if (!value)
                return true;
            result.details.oauthStatus = stringMember(*value, "oauthStatus");
            if (const auto startupStatus = stringMember(*value, "startupStatus"))
                result.details.startupStatus = typed::McpServerStartupState{*startupStatus};
            result.details.serverCount = optionalSize(*value, "serverCount");
            result.details.statusListComplete = optionalBool(*value, "statusListComplete");
            if ((value->contains("oauthStatus") && !result.details.oauthStatus) ||
                (value->contains("startupStatus") && !result.details.startupStatus) ||
                (value->contains("serverCount") && !result.details.serverCount) ||
                (value->contains("statusListComplete") && !result.details.statusListComplete)) {
                error = "MCP domain detail has an invalid stable type";
                return false;
            }
            result.projection.opaqueDetails = extensionsOf(
                *value,
                {"notificationCount", "latestNotificationMethods", "oauthStatus", "startupStatus", "serverCount", "statusListComplete"});
            return true;
        }

        template <typename T>
        bool decodePlatformDetails(const frontend::Json* value, T& result, std::string& error) {
            if (!value)
                return true;
            if (const auto remoteControlStatus = stringMember(*value, "remoteControlStatus"))
                result.details.remoteControlStatus = typed::RemoteControlConnectionStatus{*remoteControlStatus};
            result.details.windowsSandboxStatus = stringMember(*value, "windowsSandboxStatus");
            if ((value->contains("remoteControlStatus") && !result.details.remoteControlStatus) ||
                (value->contains("windowsSandboxStatus") && !result.details.windowsSandboxStatus)) {
                error = "platform domain detail has an invalid stable type";
                return false;
            }
            result.projection.opaqueDetails =
                extensionsOf(*value, {"notificationCount", "latestNotificationMethods", "remoteControlStatus", "windowsSandboxStatus"});
            return true;
        }

        bool decodeSpecificDomainDetails(const frontend::Json* value, PlatformState& result, std::string& error) {
            return decodePlatformDetails(value, result, error);
        }
        bool decodeSpecificDomainDetails(const frontend::Json* value, WindowsSandboxState& result, std::string& error) {
            return decodePlatformDetails(value, result, error);
        }

        template <typename T>
        bool decodeDomainWrapper(const frontend::Json& value, Projected<T>& target, std::string& error) {
            T decoded;
            const frontend::Json* details = nullptr;
            if (!decodeDomain(value, decoded.projection, error, &details) || !decodeSpecificDomainDetails(details, decoded, error))
                return false;
            target.truncated = false;
            target.omittedFields.clear();
            target.value = std::move(decoded);
            if (target.value->projection.truncation) {
                target.truncated = target.value->projection.truncation->truncated;
                target.omittedFields = target.value->projection.truncation->omittedFields;
            }
            return true;
        }

        bool decodeProcess(const frontend::Json& value, ProcessState& result, std::string& error) {
            if (!requireObject(value, "process", error))
                return false;
            const auto handle = stringMember(value, "processHandle");
            const auto lifecycle = stringMember(value, "lifecycle");
            const auto stamp = value.find("stamp");
            if (!handle || handle->empty() || !lifecycle || stamp == value.end() || !decodeStamp(*stamp, result.stamp, error)) {
                if (error.empty())
                    error = "process lacks required stable fields";
                return false;
            }
            result.processHandle = ProcessHandle{*handle};
            result.lifecycle = *lifecycle;
            result.standardOutput = stringMember(value, "stdout");
            result.standardError = stringMember(value, "stderr");
            result.stdoutBytes = optionalSize(value, "stdoutBytes");
            result.stderrBytes = optionalSize(value, "stderrBytes");
            result.stdoutTruncated = optionalBool(value, "stdoutTruncated").value_or(false);
            result.stderrTruncated = optionalBool(value, "stderrTruncated").value_or(false);
            result.droppedOutputBytes = optionalUnsigned(value, "droppedOutputBytes");
            result.exitCode = optionalInteger(value, "exitCode");
            result.connectionInvalidated = optionalBool(value, "connectionInvalidated").value_or(false);
            result.stateUnavailable = optionalBool(value, "stateUnavailable").value_or(false);
            result.extensions = extensionsOf(value,
                                             {"processHandle",
                                              "lifecycle",
                                              "stdout",
                                              "stderr",
                                              "stdoutBytes",
                                              "stderrBytes",
                                              "stdoutTruncated",
                                              "stderrTruncated",
                                              "droppedOutputBytes",
                                              "exitCode",
                                              "stamp",
                                              "connectionInvalidated",
                                              "stateUnavailable"});
            return true;
        }

        bool decodeWatch(const frontend::Json& value, FilesystemWatchState& result, std::string& error) {
            if (!requireObject(value, "filesystem watch", error))
                return false;
            const auto id = stringMember(value, "watchId");
            const auto stamp = value.find("stamp");
            if (!id || id->empty() || stamp == value.end() || !decodeStamp(*stamp, result.stamp, error)) {
                if (error.empty())
                    error = "filesystem watch lacks required stable fields";
                return false;
            }
            result.watchId = typed::FsWatchId{*id};
            if (const auto root = stringMember(value, "root"))
                result.root = typed::AbsolutePath{*root};
            result.changedPathCount = optionalSize(value, "changedPathCount");
            result.connectionInvalidated = optionalBool(value, "connectionInvalidated").value_or(false);
            result.stateUnavailable = optionalBool(value, "stateUnavailable").value_or(false);
            result.extensions =
                extensionsOf(value, {"watchId", "root", "changedPathCount", "stamp", "connectionInvalidated", "stateUnavailable"});
            return true;
        }

        bool decodeSearch(const frontend::Json& value, FuzzySearchState& result, std::string& error) {
            if (!requireObject(value, "fuzzy search", error))
                return false;
            const auto id = stringMember(value, "sessionId");
            const auto complete = optionalBool(value, "complete");
            const auto stamp = value.find("stamp");
            if (!id || id->empty() || !complete || stamp == value.end() || !decodeStamp(*stamp, result.stamp, error)) {
                if (error.empty())
                    error = "fuzzy search lacks required stable fields";
                return false;
            }
            result.sessionId = FuzzySearchSessionId{*id};
            result.resultCount = optionalSize(value, "resultCount");
            result.complete = *complete;
            result.connectionInvalidated = optionalBool(value, "connectionInvalidated").value_or(false);
            result.stateUnavailable = optionalBool(value, "stateUnavailable").value_or(false);
            result.extensions =
                extensionsOf(value, {"sessionId", "resultCount", "complete", "stamp", "connectionInvalidated", "stateUnavailable"});
            return true;
        }

        bool decodeNotice(const frontend::Json& value, NoticeState& result, std::string& error) {
            if (!requireObject(value, "notice", error))
                return false;
            const auto category = stringMember(value, "category");
            const auto summary = stringMember(value, "summary");
            const auto stamp = value.find("stamp");
            if (!category || !summary || stamp == value.end() || !decodeStamp(*stamp, result.stamp, error)) {
                if (error.empty())
                    error = "notice lacks required stable fields";
                return false;
            }
            result.occurrence = optionalUnsigned(value, "occurrence");
            result.category = *category;
            result.summary = *summary;
            result.details = stringMember(value, "details");
            if (const auto threadId = stringMember(value, "threadId"))
                result.threadId = typed::ThreadId{*threadId};
            result.stateUnavailable = optionalBool(value, "stateUnavailable").value_or(false);
            result.extensions =
                extensionsOf(value, {"occurrence", "category", "summary", "details", "threadId", "stamp", "stateUnavailable"});
            return true;
        }

        bool decodeActivity(const frontend::Json& value, ActivityState& result, std::string& error) {
            if (!requireObject(value, "activity", error))
                return false;
            const auto key = stringMember(value, "key");
            const auto kind = stringMember(value, "kind");
            const auto lifecycle = stringMember(value, "lifecycle");
            const auto active = optionalBool(value, "active");
            const auto stamp = value.find("stamp");
            if (!key || key->empty() || !kind || !lifecycle || !active || stamp == value.end() ||
                !decodeStamp(*stamp, result.stamp, error)) {
                if (error.empty())
                    error = "activity lacks required stable identity or state";
                return false;
            }
            result.key = ActivityKey{*key};
            result.subjectId = stringMember(value, "subjectId");
            result.kind = *kind;
            result.lifecycle = *lifecycle;
            result.summary = stringMember(value, "summary");
            result.details = stringMember(value, "details");
            if (const auto threadId = stringMember(value, "threadId"))
                result.threadId = typed::ThreadId{*threadId};
            if (const auto turnId = stringMember(value, "turnId"))
                result.turnId = typed::TurnId{*turnId};
            result.active = *active;
            result.stateUnavailable = optionalBool(value, "stateUnavailable").value_or(false);
            result.extensions = extensionsOf(value,
                                             {"key",
                                              "subjectId",
                                              "kind",
                                              "lifecycle",
                                              "summary",
                                              "details",
                                              "threadId",
                                              "turnId",
                                              "active",
                                              "stamp",
                                              "stateUnavailable"});
            return true;
        }

        CapacityState decodeCapacity(const frontend::Json& value) {
            CapacityState result;
#define AISUITE_CAPACITY_MEMBER(name) result.name = optionalSize(value, #name)
            AISUITE_CAPACITY_MEMBER(sessions);
            AISUITE_CAPACITY_MEMBER(observers);
            AISUITE_CAPACITY_MEMBER(activeOperations);
            AISUITE_CAPACITY_MEMBER(pendingRequests);
            AISUITE_CAPACITY_MEMBER(retainedThreads);
            AISUITE_CAPACITY_MEMBER(retainedTurns);
            AISUITE_CAPACITY_MEMBER(retainedItems);
            AISUITE_CAPACITY_MEMBER(accumulatedContentBytes);
            AISUITE_CAPACITY_MEMBER(retainedNotices);
            AISUITE_CAPACITY_MEMBER(retainedProcesses);
            AISUITE_CAPACITY_MEMBER(accumulatedProcessOutputBytes);
            AISUITE_CAPACITY_MEMBER(retainedFilesystemWatches);
            AISUITE_CAPACITY_MEMBER(retainedFuzzySearchSessions);
            AISUITE_CAPACITY_MEMBER(retainedActivityRecords);
            AISUITE_CAPACITY_MEMBER(evictedNotices);
            AISUITE_CAPACITY_MEMBER(evictedProcesses);
            result.droppedProcessOutputBytes = optionalUnsigned(value, "droppedProcessOutputBytes");
            AISUITE_CAPACITY_MEMBER(evictedFilesystemWatches);
            AISUITE_CAPACITY_MEMBER(evictedFuzzySearchSessions);
            AISUITE_CAPACITY_MEMBER(evictedActivityRecords);
#undef AISUITE_CAPACITY_MEMBER
            result.extensions = extensionsOf(value,
                                             {"sessions",
                                              "observers",
                                              "activeOperations",
                                              "pendingRequests",
                                              "retainedThreads",
                                              "retainedTurns",
                                              "retainedItems",
                                              "accumulatedContentBytes",
                                              "retainedNotices",
                                              "retainedProcesses",
                                              "accumulatedProcessOutputBytes",
                                              "retainedFilesystemWatches",
                                              "retainedFuzzySearchSessions",
                                              "retainedActivityRecords",
                                              "evictedNotices",
                                              "evictedProcesses",
                                              "droppedProcessOutputBytes",
                                              "evictedFilesystemWatches",
                                              "evictedFuzzySearchSessions",
                                              "evictedActivityRecords"});
            return result;
        }

        DiagnosticState decodeDiagnostic(const frontend::Json& value) {
            DiagnosticState result;
            result.received = optionalUnsigned(value, "received");
            result.detailsOmitted = optionalBool(value, "detailsOmitted").value_or(false);
            result.message = stringMember(value, "message");
            if (const auto details = value.find("details"); details != value.end())
                result.opaqueDetails = *details;
            result.extensions = extensionsOf(value, {"received", "detailsOmitted", "message", "details"});
            return result;
        }

        template <typename T, typename Key>
        bool upsert(std::vector<T>& values, T value, Key key) {
            const auto found = std::find_if(values.begin(), values.end(), [&](const T& existing) {
                return key(existing) == key(value);
            });
            if (found == values.end()) {
                values.push_back(std::move(value));
                return true;
            }
            *found = std::move(value);
            return false;
        }

        template <typename T>
        void appendUnique(std::vector<T>& values, const T& value) {
            if (std::find(values.begin(), values.end(), value) == values.end())
                values.push_back(value);
        }

        template <typename T, typename Key>
        bool appendDistinct(std::vector<T>& values, T value, Key key, std::string_view description, std::string& error) {
            const auto identity = key(value);
            if (std::find_if(values.begin(), values.end(), [&](const T& existing) {
                    return key(existing) == identity;
                }) != values.end()) {
                error = std::string(description) + " contains a duplicate stable identity";
                return false;
            }
            values.push_back(std::move(value));
            return true;
        }

        template <typename T, typename Affected>
        void replaceOrderedSubset(std::vector<T>& current, std::vector<T> replacement, Affected affected) {
            std::vector<T> rebuilt;
            rebuilt.reserve(current.size() + replacement.size());
            bool inserted = false;
            for (T& value : current) {
                if (affected(value)) {
                    if (!inserted) {
                        for (T& replacementValue : replacement)
                            rebuilt.push_back(std::move(replacementValue));
                        inserted = true;
                    }
                } else {
                    rebuilt.push_back(std::move(value));
                }
            }
            if (!inserted) {
                for (T& replacementValue : replacement)
                    rebuilt.push_back(std::move(replacementValue));
            }
            current = std::move(rebuilt);
        }

        bool decodeCompleteLegacyTurn(const frontend::Json& value,
                                      std::optional<typed::ThreadId> fallbackThread,
                                      TurnState& turn,
                                      std::vector<ItemState>& items,
                                      std::string& error) {
            if (!decodeTurn(value, turn, error, fallbackThread))
                return false;
            const auto encodedItems = value.find("items");
            if (encodedItems == value.end() || !encodedItems->is_array()) {
                error = "complete legacy turn lacks its items array";
                return false;
            }
            for (const frontend::Json& encodedItem : *encodedItems) {
                ItemState item;
                if (!decodeLegacyItem(encodedItem, item, error, turn.threadId, turn.id))
                    return false;
                if ((item.threadId && *item.threadId != turn.threadId) || (item.turnId && *item.turnId != turn.id)) {
                    error = "legacy item parent identifiers conflict with its enclosing turn";
                    return false;
                }
                item.threadId = turn.threadId;
                item.turnId = turn.id;
                if (!appendDistinct(
                        items,
                        std::move(item),
                        [](const ItemState& entry) {
                            return entry.id;
                        },
                        "legacy turn items",
                        error))
                    return false;
            }
            return true;
        }

        bool applyCompleteLegacyTurn(detail::StateStorage& state, TurnState turn, std::vector<ItemState> items, std::string& error) {
            const typed::TurnId turnId = turn.id;
            const typed::ThreadId threadId = turn.threadId;
            const auto priorTurn = std::find_if(state.turns.begin(), state.turns.end(), [&](const TurnState& candidate) {
                return candidate.id == turnId;
            });
            if (priorTurn != state.turns.end() && priorTurn->threadId != threadId) {
                const auto priorThread = std::find_if(state.threads.begin(), state.threads.end(), [&](const ThreadState& candidate) {
                    return candidate.id == priorTurn->threadId;
                });
                if (priorThread != state.threads.end())
                    std::erase(priorThread->orderedTurns, turnId);
            }

            for (const ItemState& item : items) {
                const auto priorItem = std::find_if(state.items.begin(), state.items.end(), [&](const ItemState& candidate) {
                    return candidate.id == item.id;
                });
                if (priorItem != state.items.end() && priorItem->turnId && *priorItem->turnId != turnId) {
                    const auto priorParent = std::find_if(state.turns.begin(), state.turns.end(), [&](const TurnState& candidate) {
                        return candidate.id == *priorItem->turnId;
                    });
                    if (priorParent != state.turns.end())
                        std::erase(priorParent->orderedItems, item.id);
                }
            }
            const std::vector<typed::ItemId> itemIds = turn.orderedItems;
            replaceOrderedSubset(state.items, std::move(items), [&](const ItemState& candidate) {
                return (candidate.turnId && *candidate.turnId == turnId) ||
                       std::find(itemIds.begin(), itemIds.end(), candidate.id) != itemIds.end();
            });
            upsert(state.turns, std::move(turn), [](const TurnState& candidate) {
                return candidate.id;
            });
            const auto parent = std::find_if(state.threads.begin(), state.threads.end(), [&](const ThreadState& candidate) {
                return candidate.id == threadId;
            });
            if (parent != state.threads.end())
                appendUnique(parent->orderedTurns, turnId);
            (void) error;
            return true;
        }

        bool decodeCompleteLegacyThread(const frontend::Json& value,
                                        ThreadState& thread,
                                        std::vector<TurnState>& turns,
                                        std::vector<ItemState>& items,
                                        std::string& error) {
            if (!decodeThread(value, thread, error))
                return false;
            const auto encodedTurns = value.find("turns");
            if (encodedTurns == value.end() || !encodedTurns->is_array()) {
                error = "complete legacy thread lacks its turns array";
                return false;
            }
            for (const frontend::Json& encodedTurn : *encodedTurns) {
                TurnState turn;
                std::vector<ItemState> turnItems;
                if (!decodeCompleteLegacyTurn(encodedTurn, thread.id, turn, turnItems, error))
                    return false;
                if (turn.threadId != thread.id) {
                    error = "legacy turn threadId conflicts with its enclosing thread";
                    return false;
                }
                if (!appendDistinct(
                        turns,
                        std::move(turn),
                        [](const TurnState& entry) {
                            return entry.id;
                        },
                        "legacy thread turns",
                        error))
                    return false;
                for (ItemState& item : turnItems) {
                    if (!appendDistinct(
                            items,
                            std::move(item),
                            [](const ItemState& entry) {
                                return entry.id;
                            },
                            "legacy thread items",
                            error))
                        return false;
                }
            }
            return true;
        }

        bool applyCompleteLegacyThread(detail::StateStorage& state,
                                       ThreadState thread,
                                       std::vector<TurnState> turns,
                                       std::vector<ItemState> items,
                                       std::string& error) {
            const typed::ThreadId threadId = thread.id;
            const std::vector<typed::TurnId> turnIds = thread.orderedTurns;
            std::vector<typed::TurnId> affectedTurnIds = turnIds;
            for (const TurnState& existing : state.turns) {
                if (existing.threadId == threadId)
                    appendUnique(affectedTurnIds, existing.id);
            }
            const std::vector<typed::ItemId> itemIds = [&] {
                std::vector<typed::ItemId> values;
                values.reserve(items.size());
                for (const ItemState& item : items)
                    values.push_back(item.id);
                return values;
            }();

            for (const TurnState& replacement : turns) {
                const auto prior = std::find_if(state.turns.begin(), state.turns.end(), [&](const TurnState& candidate) {
                    return candidate.id == replacement.id;
                });
                if (prior != state.turns.end() && prior->threadId != threadId) {
                    const auto oldParent = std::find_if(state.threads.begin(), state.threads.end(), [&](const ThreadState& candidate) {
                        return candidate.id == prior->threadId;
                    });
                    if (oldParent != state.threads.end())
                        std::erase(oldParent->orderedTurns, replacement.id);
                }
            }
            for (const ItemState& replacement : items) {
                const auto prior = std::find_if(state.items.begin(), state.items.end(), [&](const ItemState& candidate) {
                    return candidate.id == replacement.id;
                });
                if (prior != state.items.end() && prior->turnId &&
                    std::find(affectedTurnIds.begin(), affectedTurnIds.end(), *prior->turnId) == affectedTurnIds.end()) {
                    const auto oldParent = std::find_if(state.turns.begin(), state.turns.end(), [&](const TurnState& candidate) {
                        return candidate.id == *prior->turnId;
                    });
                    if (oldParent != state.turns.end())
                        std::erase(oldParent->orderedItems, replacement.id);
                }
            }
