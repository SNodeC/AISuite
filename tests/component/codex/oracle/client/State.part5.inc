        }

        frontend::Json& domainDetails(frontend::Json& value) {
            auto details = value.find("details");
            if (details == value.end() || !details->is_object()) {
                value["details"] = frontend::Json::object();
                details = value.find("details");
            }
            return *details;
        }

        frontend::Json encodeTypedDomain(const AccountState& value) {
            frontend::Json result = encodeDomain(value.projection);
            frontend::Json& details = domainDetails(result);
            addOptional(details, "loggedOut", value.details.loggedOut);
            addOptional(details, "loginLifecycle", value.details.loginLifecycle);
            addOptional(details, "loginMethod", value.details.loginMethod);
            addOptional(details, "loginSucceeded", value.details.loginSucceeded);
            addOptional(details, "authenticated", value.details.authenticated);
            addOptional(details, "accountType", value.details.accountType);
            if (value.details.authMode)
                details["authMode"] = value.details.authMode->value;
            if (value.details.planType)
                details["planType"] = value.details.planType->value;
            addOptional(details, "primaryUsedPercent", value.details.primaryUsedPercent);
            addOptional(details, "secondaryUsedPercent", value.details.secondaryUsedPercent);
            addOptional(details, "hasCredits", value.details.hasCredits);
            if (details.empty())
                result.erase("details");
            return result;
        }

        frontend::Json encodeTypedDomain(const ConfigurationState& value) {
            frontend::Json result = encodeDomain(value.projection);
            frontend::Json& details = domainDetails(result);
            if (value.details.filePath)
                details["filePath"] = value.details.filePath->value;
            if (value.details.writeStatus)
                details["writeStatus"] = value.details.writeStatus->value;
            addOptional(details, "writeVersion", value.details.writeVersion);
            addOptional(details, "writeOverridden", value.details.writeOverridden);
            addOptional(details, "featureCount", value.details.featureCount);
            addOptional(details, "featureListTruncated", value.details.featureListTruncated);
            if (details.empty())
                result.erase("details");
            return result;
        }

        template <typename T>
        frontend::Json encodeIntegrationDomain(const T& value) {
            frontend::Json result = encodeDomain(value.projection);
            frontend::Json& details = domainDetails(result);
            addOptional(details, "appCount", value.details.appCount);
            addOptional(details, "appListTruncated", value.details.appListTruncated);
            addOptional(details, "marketplaceAddStatus", value.details.marketplaceAddStatus);
            addOptional(details, "marketplaceRemoveStatus", value.details.marketplaceRemoveStatus);
            addOptional(details, "marketplaceUpgradeStatus", value.details.marketplaceUpgradeStatus);
            if (details.empty())
                result.erase("details");
            return result;
        }

        frontend::Json encodeTypedDomain(const AppsState& value) {
            return encodeIntegrationDomain(value);
        }
        frontend::Json encodeTypedDomain(const ExternalAgentsState& value) {
            return encodeIntegrationDomain(value);
        }
        frontend::Json encodeTypedDomain(const HooksState& value) {
            return encodeIntegrationDomain(value);
        }
        frontend::Json encodeTypedDomain(const MarketplaceState& value) {
            return encodeIntegrationDomain(value);
        }

        template <typename T>
        frontend::Json encodePluginDomain(const T& value) {
            frontend::Json result = encodeDomain(value.projection);
            frontend::Json& details = domainDetails(result);
            addOptional(details, "lastPluginOperation", value.details.lastPluginOperation);
            addOptional(details, "lastSkillsOperation", value.details.lastSkillsOperation);
            addOptional(details, "extraRootCount", value.details.extraRootCount);
            addOptional(details, "extraRootsTruncated", value.details.extraRootsTruncated);
            if (details.empty())
                result.erase("details");
            return result;
        }

        frontend::Json encodeTypedDomain(const PluginsState& value) {
            return encodePluginDomain(value);
        }
        frontend::Json encodeTypedDomain(const SkillsState& value) {
            return encodePluginDomain(value);
        }

        frontend::Json encodeTypedDomain(const McpState& value) {
            frontend::Json result = encodeDomain(value.projection);
            frontend::Json& details = domainDetails(result);
            addOptional(details, "oauthStatus", value.details.oauthStatus);
            if (value.details.startupStatus)
                details["startupStatus"] = value.details.startupStatus->value;
            addOptional(details, "serverCount", value.details.serverCount);
            addOptional(details, "statusListComplete", value.details.statusListComplete);
            if (details.empty())
                result.erase("details");
            return result;
        }

        template <typename T>
        frontend::Json encodePlatformDomain(const T& value) {
            frontend::Json result = encodeDomain(value.projection);
            frontend::Json& details = domainDetails(result);
            if (value.details.remoteControlStatus)
                details["remoteControlStatus"] = value.details.remoteControlStatus->value;
            addOptional(details, "windowsSandboxStatus", value.details.windowsSandboxStatus);
            if (details.empty())
                result.erase("details");
            return result;
        }

        frontend::Json encodeTypedDomain(const PlatformState& value) {
            return encodePlatformDomain(value);
        }
        frontend::Json encodeTypedDomain(const WindowsSandboxState& value) {
            return encodePlatformDomain(value);
        }

        template <typename T>
        frontend::Json encodeTypedDomain(const T& value) {
            return encodeDomain(value.projection);
        }

        template <typename T, typename Encoder>
        frontend::Json encodeProjected(const Projected<T>& value, Encoder encoder) {
            frontend::Json result = frontend::Json::object();
            result["present"] = value.value.has_value();
            result["truncated"] = value.truncated;
            result["omittedFields"] = frontend::Json::array();
            for (const std::string& field : value.omittedFields)
                result["omittedFields"].push_back(field);
            if (value.value)
                result["value"] = encoder(*value.value);
            return result;
        }

        frontend::Json encodeProvider(const ProviderState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["lifecycle"] = std::string(providerLifecycleName(value.lifecycle));
            result["generation"] = value.generation;
            result["desiredRunning"] = value.desiredRunning;
            result["ready"] = value.ready;
            result["recovery"] = value.recovery.extensions;
            if (!result["recovery"].is_object())
                result["recovery"] = frontend::Json::object();
            result["recovery"]["status"] = std::string(providerRecoveryStatusName(value.recovery.status));
            result["recovery"]["attempts"] = value.recovery.attempts;
            if (value.recovery.delayMs)
                result["recovery"]["delayMs"] = *value.recovery.delayMs;
            if (value.lastError) {
                frontend::Json lastError = value.lastError->extensions;
                if (!lastError.is_object())
                    lastError = frontend::Json::object();
                lastError["category"] = value.lastError->category;
                lastError["code"] = value.lastError->code;
                addOptional(lastError, "detailsOmitted", value.lastError->detailsOmitted);
                addOptional(lastError, "message", value.lastError->message);
                result["lastError"] = std::move(lastError);
            }
            if (value.initialization) {
                frontend::Json initialization = value.initialization->extensions;
                if (!initialization.is_object())
                    initialization = frontend::Json::object();
                initialization["codexHome"] = value.initialization->codexHome.value;
                initialization["platformFamily"] = value.initialization->platformFamily;
                initialization["platformOs"] = value.initialization->platformOs;
                initialization["userAgent"] = value.initialization->userAgent;
                result["initialization"] = std::move(initialization);
            }
            return result;
        }

        frontend::Json encodeSessionInfo(const SessionInfo& value) {
            frontend::Json result = frontend::Json::object();
            result["sessionId"] = value.sessionId;
            result["role"] = std::string(frontend::toString(value.role));
            result["syncMode"] = std::string(frontend::toString(value.syncMode));
            result["serverCurrentSequence"] = value.serverCurrentSequence.value();
            addOptional(result, "serverVersion", value.serverVersion);
            const auto encodeCapabilities = [](const std::vector<frontend::FrontendCapability>& capabilities) {
                frontend::Json encoded = frontend::Json::array();
                for (frontend::FrontendCapability capability : capabilities)
                    encoded.push_back(std::string(frontend::toString(capability)));
                return encoded;
            };
            result["requestedRepresentationCapabilities"] = encodeCapabilities(value.requestedRepresentationCapabilities);
            result["selectedRepresentationCapabilities"] = encodeCapabilities(value.selectedRepresentationCapabilities);
            result["observedMechanismCapabilities"] = encodeCapabilities(value.observedMechanismCapabilities);
            result["observedTopologyCapabilities"] = encodeCapabilities(value.observedTopologyCapabilities);
            result["observedProductCapabilities"] = encodeCapabilities(value.observedProductCapabilities);
            const auto encodeMethods = [](const std::optional<std::vector<frontend::generated::MethodId>>& methods) {
                frontend::Json encoded = frontend::Json::object();
                encoded["present"] = methods.has_value();
                if (methods) {
                    encoded["values"] = frontend::Json::array();
                    for (frontend::generated::MethodId method : *methods)
                        encoded["values"].push_back(std::string(frontend::generated::methodString(method)));
                }
                return encoded;
            };
            result["availableMethods"] = encodeMethods(value.availableMethods);
            result["permittedMethods"] = encodeMethods(value.permittedMethods);
            frontend::Json scopes = frontend::Json::object();
            scopes["present"] = value.permittedScopes.has_value();
            if (value.permittedScopes) {
                scopes["values"] = frontend::Json::array();
                for (frontend::FrontendScope scope : *value.permittedScopes)
                    scopes["values"].push_back(std::string(frontend::toString(scope)));
            }
            result["permittedScopes"] = std::move(scopes);
            return result;
        }

        frontend::Json encodeBackendCursor(const BackendCursorState& value) {
            frontend::Json result = frontend::Json::object();
            addOptional(result, "backendRevision", value.backendRevision);
            if (value.oldestReplayableAfter)
                result["oldestReplayableAfter"] = value.oldestReplayableAfter->value();
            if (value.currentSequence)
                result["currentSequence"] = value.currentSequence->value();
            if (value.oldestRetainedSequence)
                result["oldestRetainedSequence"] = value.oldestRetainedSequence->value();
            if (value.newestRetainedSequence)
                result["newestRetainedSequence"] = value.newestRetainedSequence->value();
            addOptional(result, "backendSequenceExhausted", value.backendSequenceExhausted);
            addOptional(result, "frontendSequenceExhausted", value.frontendSequenceExhausted);
            return result;
        }

        frontend::Json encodeProjectionMetadata(const ProjectionMetadataState& value) {
            frontend::Json result = frontend::Json::object();
            result["omittedFields"] = frontend::Json::array();
            for (const std::string& path : value.omittedFields)
                result["omittedFields"].push_back(path);
            result["redactedFields"] = frontend::Json::array();
            for (const std::string& path : value.redactedFields)
                result["redactedFields"].push_back(path);
            return result;
        }

        frontend::Json encodeControllerState(const ControllerState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            if (value.sessionId)
                result["sessionId"] = value.sessionId->value;
            result["present"] = value.present;
            result["ownedByThisClient"] = value.ownedByThisClient;
            return result;
        }

        frontend::Json encodeSessionState(const SessionState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["sessionId"] = value.sessionId.value;
            result["role"] = std::string(frontend::toString(value.role));
            return result;
        }

        frontend::Json encodeThreadListState(const ThreadListState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["hasLoadedPage"] = value.hasLoadedPage;
            result["complete"] = value.complete;
            result["pagesLoaded"] = value.pagesLoaded;
            addOptional(result, "nextCursor", value.nextCursor);
            addOptional(result, "backwardsCursor", value.backwardsCursor);
            if (value.stamp)
                result["stamp"] = encodeStamp(*value.stamp);
            return result;
        }

        frontend::Json encodeThreadState(const ThreadState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["id"] = value.id.value;
            addOptional(result, "title", value.title);
            addOptional(result, "preview", value.preview);
            if (value.cwd)
                result["cwd"] = value.cwd->value;
            if (value.model)
                result["model"] = value.model->value;
            addOptional(result, "modelProvider", value.modelProvider);
            addOptional(result, "status", value.status);
            result["fullyLoaded"] = value.fullyLoaded;
            if (value.realtime) {
                frontend::Json realtime = value.realtime->extensions;
                if (!realtime.is_object())
                    realtime = frontend::Json::object();
                realtime["lifecycle"] = value.realtime->lifecycle;
                realtime["transcript"] = value.realtime->transcript;
                realtime["itemCount"] = value.realtime->itemCount;
                realtime["receivedAudioBytes"] = value.realtime->receivedAudioBytes;
                realtime["droppedAudioBytes"] = value.realtime->droppedAudioBytes;
                realtime["transcriptTruncated"] = value.realtime->transcriptTruncated;
                addOptional(realtime, "errorDetailsOmitted", value.realtime->errorDetailsOmitted);
                addOptional(realtime, "sessionId", value.realtime->sessionId);
                if (value.realtime->version)
                    realtime["version"] = value.realtime->version->value;
                addOptional(realtime, "lastSdpBytes", value.realtime->lastSdpBytes);
                result["realtime"] = std::move(realtime);
            }
            if (value.stamp)
                result["stamp"] = encodeStamp(*value.stamp);
            addOptional(result, "createdAtMs", value.createdAtMs);
            addOptional(result, "updatedAtMs", value.updatedAtMs);
            result["orderedTurns"] = frontend::Json::array();
            for (const typed::TurnId& id : value.orderedTurns)
                result["orderedTurns"].push_back(id.value);
            return result;
        }

        frontend::Json encodeTurnState(const TurnState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["id"] = value.id.value;
            result["threadId"] = value.threadId.value;
            result["status"] = value.status.value;
            result["active"] = value.active;
            result["terminal"] = value.terminal;
            result["connectionInvalidated"] = value.connectionInvalidated;
            if (value.stamp)
                result["stamp"] = encodeStamp(*value.stamp);
            result["orderedItems"] = frontend::Json::array();
            for (const typed::ItemId& id : value.orderedItems)
                result["orderedItems"].push_back(id.value);
            if (value.failure)
                result["failure"] = *value.failure;
            if (value.tokenUsage)
                result["tokenUsage"] = *value.tokenUsage;
            return result;
        }

        frontend::Json encodeItemState(const ItemState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["id"] = value.id.value;
            if (value.threadId)
                result["threadId"] = value.threadId->value;
            if (value.turnId)
                result["turnId"] = value.turnId->value;
            result["kind"] = value.kind.identity;
            addOptional(result, "status", value.status);
            addOptional(result, "summary", value.summary);
            if (value.location)
                result["location"] = *value.location;
            addOptional(result, "agentText", value.agentText);
            addOptional(result, "reasoningText", value.reasoningText);
            addOptional(result, "reasoningSummary", value.reasoningSummary);
            addOptional(result, "commandOutput", value.commandOutput);
            addOptional(result, "droppedContentBytes", value.droppedContentBytes);
            result["contentTruncated"] = value.contentTruncated;
            addOptional(result, "startedAtMs", value.startedAtMs);
            addOptional(result, "completedAtMs", value.completedAtMs);
            if (value.data)
                result["data"] = *value.data;
            result["truncated"] = value.truncated;
            result["omittedFields"] = frontend::Json::array();
            for (const std::string& field : value.omittedFields)
                result["omittedFields"].push_back(field);
            result["connectionInvalidated"] = value.connectionInvalidated;
            if (value.stamp)
                result["stamp"] = encodeStamp(*value.stamp);
            return result;
        }

        frontend::Json encodePendingRequestState(const PendingRequestState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["id"] = value.id.value;
            result["kind"] = std::string(frontend::toString(value.kind));
            if (value.threadId)
                result["threadId"] = value.threadId->value;
            if (value.turnId)
                result["turnId"] = value.turnId->value;
            if (value.itemId)
                result["itemId"] = value.itemId->value;
            addOptional(result, "summary", value.summary);
            if (value.opaqueDetails)
                result["details"] = *value.opaqueDetails;
            if (value.questions) {
                result["questions"] = frontend::Json::array();
                for (const PendingRequestQuestionState& question : *value.questions) {
                    frontend::Json questionJson = question.extensions;
                    if (!questionJson.is_object())
                        questionJson = frontend::Json::object();
                    questionJson["id"] = question.id;
                    questionJson["header"] = question.header;
                    questionJson["prompt"] = question.prompt;
                    questionJson["allowsFreeText"] = question.allowsFreeText;
                    questionJson["isSecret"] = question.isSecret;
                    questionJson["options"] = frontend::Json::array();
                    for (const PendingRequestOptionState& option : question.options) {
                        frontend::Json optionJson = option.extensions;
                        if (!optionJson.is_object())
                            optionJson = frontend::Json::object();
                        optionJson["label"] = option.label;
                        optionJson["description"] = option.description;
                        questionJson["options"].push_back(std::move(optionJson));
                    }
                    result["questions"].push_back(std::move(questionJson));
                }
            }
            addOptional(result, "autoResolutionMs", value.autoResolutionMs);
            result["truncated"] = value.truncated;
            result["omittedFields"] = frontend::Json::array();
            for (const std::string& field : value.omittedFields)
                result["omittedFields"].push_back(field);
            result["connectionInvalidated"] = value.connectionInvalidated;
            return result;
        }

        frontend::Json encodeProcessState(const ProcessState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["processHandle"] = value.processHandle.value;
            result["lifecycle"] = value.lifecycle;
            addOptional(result, "stdout", value.standardOutput);
            addOptional(result, "stderr", value.standardError);
            addOptional(result, "stdoutBytes", value.stdoutBytes);
            addOptional(result, "stderrBytes", value.stderrBytes);
            result["stdoutTruncated"] = value.stdoutTruncated;
            result["stderrTruncated"] = value.stderrTruncated;
            addOptional(result, "droppedOutputBytes", value.droppedOutputBytes);
            addOptional(result, "exitCode", value.exitCode);
            result["stamp"] = encodeStamp(value.stamp);
            result["connectionInvalidated"] = value.connectionInvalidated;
            result["stateUnavailable"] = value.stateUnavailable;
            return result;
        }

        frontend::Json encodeFilesystemWatchState(const FilesystemWatchState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["watchId"] = value.watchId.value;
            if (value.root)
                result["root"] = value.root->value;
            addOptional(result, "changedPathCount", value.changedPathCount);
            result["stamp"] = encodeStamp(value.stamp);
            result["connectionInvalidated"] = value.connectionInvalidated;
            result["stateUnavailable"] = value.stateUnavailable;
            return result;
        }

        frontend::Json encodeFuzzySearchState(const FuzzySearchState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["sessionId"] = value.sessionId.value;
            addOptional(result, "resultCount", value.resultCount);
            result["complete"] = value.complete;
            result["stamp"] = encodeStamp(value.stamp);
            result["connectionInvalidated"] = value.connectionInvalidated;
            result["stateUnavailable"] = value.stateUnavailable;
            return result;
        }

        frontend::Json encodeNoticeState(const NoticeState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            addOptional(result, "occurrence", value.occurrence);
            result["category"] = value.category;
            result["summary"] = value.summary;
            addOptional(result, "details", value.details);
            if (value.threadId)
                result["threadId"] = value.threadId->value;
            result["stamp"] = encodeStamp(value.stamp);
            result["stateUnavailable"] = value.stateUnavailable;
            return result;
        }

        frontend::Json encodeActivityState(const ActivityState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["key"] = value.key.value;
            addOptional(result, "subjectId", value.subjectId);
            result["kind"] = value.kind;
            result["lifecycle"] = value.lifecycle;
            addOptional(result, "summary", value.summary);
            addOptional(result, "details", value.details);
            if (value.threadId)
                result["threadId"] = value.threadId->value;
            if (value.turnId)
                result["turnId"] = value.turnId->value;
            result["active"] = value.active;
            result["stamp"] = encodeStamp(value.stamp);
            result["stateUnavailable"] = value.stateUnavailable;
            return result;
        }

        frontend::Json encodeDiagnosticState(const DiagnosticState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            addOptional(result, "received", value.received);
            result["detailsOmitted"] = value.detailsOmitted;
            addOptional(result, "message", value.message);
            if (value.opaqueDetails)
                result["details"] = *value.opaqueDetails;
            return result;
        }

        frontend::Json encodeProcessCollectionState(const ProcessCollectionState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["entries"] = frontend::Json::array();
            for (const ProcessState& entry : value.entries)
                result["entries"].push_back(encodeProcessState(entry));
            result["truncation"] = encodeTruncation(value.truncation);
            return result;
        }

        frontend::Json encodeFilesystemWatchCollectionState(const FilesystemWatchCollectionState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["entries"] = frontend::Json::array();
            for (const FilesystemWatchState& entry : value.entries)
                result["entries"].push_back(encodeFilesystemWatchState(entry));
            result["truncation"] = encodeTruncation(value.truncation);
            return result;
        }

        frontend::Json encodeFuzzySearchCollectionState(const FuzzySearchCollectionState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["entries"] = frontend::Json::array();
            for (const FuzzySearchState& entry : value.entries)
                result["entries"].push_back(encodeFuzzySearchState(entry));
            result["truncation"] = encodeTruncation(value.truncation);
            return result;
        }

        frontend::Json encodeNoticeCollectionState(const NoticeCollectionState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["entries"] = frontend::Json::array();
            for (const NoticeState& entry : value.entries)
                result["entries"].push_back(encodeNoticeState(entry));
            result["truncation"] = encodeTruncation(value.truncation);
            return result;
        }

        frontend::Json encodeActivityCollectionState(const ActivityCollectionState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
            result["entries"] = frontend::Json::array();
            for (const ActivityState& entry : value.entries)
                result["entries"].push_back(encodeActivityState(entry));
            result["truncation"] = encodeTruncation(value.truncation);
            return result;
        }

        frontend::Json encodeCapacityState(const CapacityState& value) {
            frontend::Json result = value.extensions;
            if (!result.is_object())
                result = frontend::Json::object();
#define AISUITE_ENCODE_CAPACITY(name) addOptional(result, #name, value.name)
            AISUITE_ENCODE_CAPACITY(sessions);
            AISUITE_ENCODE_CAPACITY(observers);
            AISUITE_ENCODE_CAPACITY(activeOperations);
            AISUITE_ENCODE_CAPACITY(pendingRequests);
            AISUITE_ENCODE_CAPACITY(retainedThreads);
            AISUITE_ENCODE_CAPACITY(retainedTurns);
            AISUITE_ENCODE_CAPACITY(retainedItems);
            AISUITE_ENCODE_CAPACITY(accumulatedContentBytes);
            AISUITE_ENCODE_CAPACITY(retainedNotices);
            AISUITE_ENCODE_CAPACITY(retainedProcesses);
            AISUITE_ENCODE_CAPACITY(accumulatedProcessOutputBytes);
            AISUITE_ENCODE_CAPACITY(retainedFilesystemWatches);
            AISUITE_ENCODE_CAPACITY(retainedFuzzySearchSessions);
            AISUITE_ENCODE_CAPACITY(retainedActivityRecords);
            AISUITE_ENCODE_CAPACITY(evictedNotices);
            AISUITE_ENCODE_CAPACITY(evictedProcesses);
            AISUITE_ENCODE_CAPACITY(droppedProcessOutputBytes);
            AISUITE_ENCODE_CAPACITY(evictedFilesystemWatches);
            AISUITE_ENCODE_CAPACITY(evictedFuzzySearchSessions);
            AISUITE_ENCODE_CAPACITY(evictedActivityRecords);
#undef AISUITE_ENCODE_CAPACITY
            return result;
        }

        frontend::Json encodeDiagnosticCollectionState(const DiagnosticCollectionState& value) {
            frontend::Json result = frontend::Json::object();
            addOptional(result, "received", value.received);
            result["entries"] = frontend::Json::array();
            for (const DiagnosticState& entry : value.entries)
                result["entries"].push_back(encodeDiagnosticState(entry));
            return result;
        }

        // IMPORTANT: encodeState() defines the canonical logical decoded-State
        // byte metric. Incremental accounting must remain byte-identical to
        // this compact JSON serialization plus the two explicitly documented
        // internal-only sequence contributions. Changes to one require
        // corresponding changes to the other.
        frontend::Json encodeState(const detail::StateStorage& state) {
            frontend::Json result = frontend::Json::object();
            result["revision"] = state.revision;
            result["freshness"] = static_cast<unsigned>(state.freshness);
            result["representationMode"] = static_cast<unsigned>(state.representationMode);
            if (state.visibleSequence)
                result["visibleSequence"] = state.visibleSequence->value();
            if (state.synchronizedThrough)
                result["synchronizedThrough"] = state.synchronizedThrough->value();
            if (state.session)
                result["session"] = encodeSessionInfo(*state.session);

            frontend::Json backendCursor = encodeBackendCursor(state.backendCursor);
            if (!backendCursor.empty())
                result["backendCursor"] = std::move(backendCursor);

            if (state.projectionFingerprint)
                result["projectionFingerprint"] = state.projectionFingerprint->canonical;

            if (!state.projectionMetadata.omittedFields.empty() || !state.projectionMetadata.redactedFields.empty())
                result["projectionMetadata"] = encodeProjectionMetadata(state.projectionMetadata);

            result["provider"] = encodeProjected(state.provider, encodeProvider);
            result["controller"] = encodeProjected(state.controller, encodeControllerState);
            result["sessions"] = encodeProjected(state.sessions, [](const std::vector<SessionState>& values) {
                frontend::Json encoded = frontend::Json::array();
                for (const SessionState& value : values)
                    encoded.push_back(encodeSessionState(value));
                return encoded;
            });
            result["threadList"] = encodeProjected(state.threadList, encodeThreadListState);
            result["threads"] = frontend::Json::array();
            result["threadProjectionPresent"] = state.threadProjectionPresent;
            for (const ThreadState& value : state.threads)
                result["threads"].push_back(encodeThreadState(value));
            result["turns"] = frontend::Json::array();
            result["turnProjectionPresent"] = state.turnProjectionPresent;
            for (const TurnState& value : state.turns)
                result["turns"].push_back(encodeTurnState(value));
            result["items"] = frontend::Json::array();
            result["itemProjectionPresent"] = state.itemProjectionPresent;
            for (const ItemState& value : state.items)
                result["items"].push_back(encodeItemState(value));
            result["pendingRequests"] = frontend::Json::array();
            result["pendingRequestProjectionPresent"] = state.pendingRequestProjectionPresent;
            for (const PendingRequestState& value : state.pendingRequests)
                result["pendingRequests"].push_back(encodePendingRequestState(value));

            const auto domainProjection = [](const auto& value) {
                return encodeTypedDomain(value);
            };
            result["accounts"] = encodeProjected(state.accounts, domainProjection);
            result["models"] = encodeProjected(state.models, domainProjection);
            result["configuration"] = encodeProjected(state.configuration, domainProjection);
            result["permissionProfiles"] = encodeProjected(state.permissionProfiles, domainProjection);
            result["reviews"] = encodeProjected(state.reviews, domainProjection);
            result["apps"] = encodeProjected(state.apps, domainProjection);
            result["externalAgents"] = encodeProjected(state.externalAgents, domainProjection);
            result["hooks"] = encodeProjected(state.hooks, domainProjection);
            result["marketplace"] = encodeProjected(state.marketplace, domainProjection);
            result["plugins"] = encodeProjected(state.plugins, domainProjection);
            result["skills"] = encodeProjected(state.skills, domainProjection);
            result["mcp"] = encodeProjected(state.mcp, domainProjection);
            result["windowsSandbox"] = encodeProjected(state.windowsSandbox, domainProjection);
            result["platform"] = encodeProjected(state.platform, domainProjection);

            result["processes"] = encodeProjected(state.processes, encodeProcessCollectionState);
            result["filesystemWatches"] = encodeProjected(state.filesystemWatches, encodeFilesystemWatchCollectionState);
            result["fuzzySearches"] = encodeProjected(state.fuzzySearches, encodeFuzzySearchCollectionState);
            result["notices"] = encodeProjected(state.notices, encodeNoticeCollectionState);
            result["activities"] = encodeProjected(state.activities, encodeActivityCollectionState);
            result["capacity"] = encodeProjected(state.capacity, encodeCapacityState);
            result["truncation"] = encodeProjected(state.truncation, encodeTruncation);
            result["diagnostics"] = encodeProjected(state.diagnostics, encodeDiagnosticCollectionState);
            result["compatibilityExtensions"] = state.compatibilityExtensions;
            return result;
        }

        constexpr std::array StateSizeSectionNames{
            std::string_view{"revision"},
            std::string_view{"freshness"},
            std::string_view{"representationMode"},
            std::string_view{"visibleSequence"},
            std::string_view{"synchronizedThrough"},
            std::string_view{"session"},
            std::string_view{"backendCursor"},
            std::string_view{"projectionFingerprint"},
            std::string_view{"projectionMetadata"},
            std::string_view{"provider"},
            std::string_view{"controller"},
            std::string_view{"sessions"},
            std::string_view{"threadList"},
            std::string_view{"threads"},
            std::string_view{"threadProjectionPresent"},
            std::string_view{"turns"},
            std::string_view{"turnProjectionPresent"},
            std::string_view{"items"},
            std::string_view{"itemProjectionPresent"},
            std::string_view{"pendingRequests"},
            std::string_view{"pendingRequestProjectionPresent"},
            std::string_view{"accounts"},
            std::string_view{"models"},
            std::string_view{"configuration"},
            std::string_view{"permissionProfiles"},
            std::string_view{"reviews"},
            std::string_view{"apps"},
            std::string_view{"externalAgents"},
            std::string_view{"hooks"},
            std::string_view{"marketplace"},
            std::string_view{"plugins"},
            std::string_view{"skills"},
            std::string_view{"mcp"},
            std::string_view{"windowsSandbox"},
            std::string_view{"platform"},
            std::string_view{"processes"},
            std::string_view{"filesystemWatches"},
            std::string_view{"fuzzySearches"},
            std::string_view{"notices"},
            std::string_view{"activities"},
            std::string_view{"capacity"},
            std::string_view{"truncation"},
            std::string_view{"diagnostics"},
            std::string_view{"compatibilityExtensions"},
        };
        static_assert(StateSizeSectionNames.size() == static_cast<std::size_t>(detail::StateSizeSection::Count));

        constexpr std::size_t sectionIndex(detail::StateSizeSection section) noexcept {
            return static_cast<std::size_t>(section);
        }

        bool checkedAdd(std::size_t left, std::size_t right, std::size_t& result) noexcept {
            if (left > std::numeric_limits<std::size_t>::max() - right)
                return false;
            result = left + right;
            return true;
        }

        bool checkedSubtract(std::size_t left, std::size_t right, std::size_t& result) noexcept {
            if (left < right)
                return false;
            result = left - right;
            return true;
        }

        std::optional<std::size_t> compactJsonBytes(const frontend::Json& value) noexcept {
            try {
                return value.dump().size();
            } catch (...) {
                return std::nullopt;
            }
        }

        std::optional<std::size_t> arrayBytes(const detail::StateArrayContribution& contribution) noexcept {
            std::size_t bytes = 2;
            if (!checkedAdd(bytes, contribution.elementBytes, bytes))
                return std::nullopt;
            if (contribution.count > 1 && !checkedAdd(bytes, contribution.count - 1, bytes))
                return std::nullopt;
            return bytes;
        }

        bool measureArray(const frontend::Json& value, detail::StateArrayContribution& result) noexcept {
            if (!value.is_array())
                return false;
            detail::StateArrayContribution measured;
            try {
                for (const frontend::Json& element : value) {
                    const std::size_t bytes = element.dump().size();
                    if (!checkedAdd(measured.elementBytes, bytes, measured.elementBytes))
                        return false;
                    if (measured.count == std::numeric_limits<std::size_t>::max())
                        return false;
                    ++measured.count;
                }
            } catch (...) {
                return false;
            }
            result = measured;
            return true;
        }

        const frontend::Json* projectedValue(const frontend::Json& encoded, std::string_view section) {
            const auto projected = encoded.find(std::string(section));
            if (projected == encoded.end() || !projected->is_object())
                return nullptr;
            const auto value = projected->find("value");
            return value == projected->end() ? nullptr : &*value;
        }

        const frontend::Json* projectedEntries(const frontend::Json& encoded, std::string_view section) {
            const frontend::Json* value = projectedValue(encoded, section);
            if (!value || !value->is_object())
                return nullptr;
            const auto entries = value->find("entries");
            return entries == value->end() ? nullptr : &*entries;
        }

#ifndef NDEBUG
        thread_local std::size_t DebugAccountingRebuildCount = 0;
#endif

        bool rebuildStateSizeLedger(const detail::StateStorage& state) noexcept {
#ifndef NDEBUG
            ++DebugAccountingRebuildCount;
#endif
            detail::StateSizeLedger next;
            try {
                const frontend::Json encoded = encodeState(state);
                next.canonicalBytes = 2;
                for (std::size_t index = 0; index < StateSizeSectionNames.size(); ++index) {
                    const auto found = encoded.find(std::string(StateSizeSectionNames[index]));
                    if (found == encoded.end())
                        continue;
                    const std::size_t valueBytes = found->dump().size();
                    std::size_t memberBytes = StateSizeSectionNames[index].size() + 3;
                    if (!checkedAdd(memberBytes, valueBytes, memberBytes) ||
                        (next.topLevelMemberCount != 0 && !checkedAdd(next.canonicalBytes, 1, next.canonicalBytes)) ||
                        !checkedAdd(next.canonicalBytes, memberBytes, next.canonicalBytes))
                        return false;
                    next.sectionPresent[index] = true;
                    next.sectionValueBytes[index] = valueBytes;
                    ++next.topLevelMemberCount;
                }
                if (encoded.size() != next.topLevelMemberCount)
                    return false;

                const frontend::Json* sessions = projectedValue(encoded, "sessions");
                if (sessions && !measureArray(*sessions, next.sessions))
                    return false;
                if (!measureArray(encoded.at("threads"), next.threads) || !measureArray(encoded.at("turns"), next.turns) ||
                    !measureArray(encoded.at("items"), next.items) || !measureArray(encoded.at("pendingRequests"), next.pendingRequests))
                    return false;
                const auto measureProjectedEntries = [&encoded](std::string_view name, detail::StateArrayContribution& contribution) {
                    const frontend::Json* entries = projectedEntries(encoded, name);
                    return !entries || measureArray(*entries, contribution);
                };
                if (!measureProjectedEntries("processes", next.processes) ||
                    !measureProjectedEntries("filesystemWatches", next.filesystemWatches) ||
                    !measureProjectedEntries("fuzzySearches", next.fuzzySearches) || !measureProjectedEntries("notices", next.notices) ||
                    !measureProjectedEntries("activities", next.activities) || !measureProjectedEntries("diagnostics", next.diagnostics))
                    return false;

                const std::size_t internalSequences = static_cast<std::size_t>(state.retainedReplayThrough.has_value()) +
                                                      static_cast<std::size_t>(state.lastSynchronizationBatchSequence.has_value());
                if (internalSequences != 0 && !checkedAdd(next.internalSequenceBytes,
                                                          internalSequences * sizeof(frontend::SequenceNumber),
                                                          next.internalSequenceBytes))
                    return false;
                next.initialized = true;
                state.sizeLedger = std::move(next);
                return true;
            } catch (...) {
                return false;
            }
        }

        bool recomputeCanonicalBytes(detail::StateSizeLedger& ledger) noexcept {
            std::size_t bytes = 2;
            std::size_t members = 0;
            for (std::size_t index = 0; index < StateSizeSectionNames.size(); ++index) {
                if (!ledger.sectionPresent[index])
                    continue;
                std::size_t memberBytes = StateSizeSectionNames[index].size() + 3;
                if (!checkedAdd(memberBytes, ledger.sectionValueBytes[index], memberBytes) ||
                    (members != 0 && !checkedAdd(bytes, 1, bytes)) || !checkedAdd(bytes, memberBytes, bytes))
                    return false;
                ++members;
            }
            ledger.canonicalBytes = bytes;
            ledger.topLevelMemberCount = members;
            return true;
        }

        bool setSectionBytes(detail::StateStorage& state, detail::StateSizeSection section, std::optional<std::size_t> bytes) noexcept {
            const std::size_t index = sectionIndex(section);
            state.sizeLedger.sectionPresent[index] = bytes.has_value();
            state.sizeLedger.sectionValueBytes[index] = bytes.value_or(0);
            if (!recomputeCanonicalBytes(state.sizeLedger)) {
                state.sizeLedger.failed = true;
