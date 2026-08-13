/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/State.h"

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/client/detail/OperationCodecs.h"
#include "ai/openai/codex/frontend/internal/client/CanonicalStateBuilder.h"
#include "ai/openai/codex/frontend/internal/client/ClientCore.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>

namespace ai::openai::codex::frontend::client {

    namespace detail {
        enum class StateSizeSection : std::size_t {
            Revision,
            Freshness,
            RepresentationMode,
            VisibleSequence,
            SynchronizedThrough,
            Session,
            BackendCursor,
            ProjectionFingerprint,
            ProjectionMetadata,
            Provider,
            Controller,
            Sessions,
            ThreadList,
            Threads,
            ThreadProjectionPresent,
            Turns,
            TurnProjectionPresent,
            Items,
            ItemProjectionPresent,
            PendingRequests,
            PendingRequestProjectionPresent,
            Accounts,
            Models,
            Configuration,
            PermissionProfiles,
            Reviews,
            Apps,
            ExternalAgents,
            Hooks,
            Marketplace,
            Plugins,
            Skills,
            Mcp,
            WindowsSandbox,
            Platform,
            Processes,
            FilesystemWatches,
            FuzzySearches,
            Notices,
            Activities,
            Capacity,
            Truncation,
            Diagnostics,
            CompatibilityExtensions,
            Count,
        };

        struct StateArrayContribution {
            std::size_t count = 0;
            std::size_t elementBytes = 0;
        };

        struct StateSizeLedger {
            bool initialized = false;
            bool failed = false;
            std::size_t canonicalBytes = 0;
            std::size_t topLevelMemberCount = 0;
            std::size_t internalSequenceBytes = 0;
            std::array<bool, static_cast<std::size_t>(StateSizeSection::Count)> sectionPresent{};
            std::array<std::size_t, static_cast<std::size_t>(StateSizeSection::Count)> sectionValueBytes{};
            StateArrayContribution sessions;
            StateArrayContribution threads;
            StateArrayContribution turns;
            StateArrayContribution items;
            StateArrayContribution pendingRequests;
            StateArrayContribution processes;
            StateArrayContribution filesystemWatches;
            StateArrayContribution fuzzySearches;
            StateArrayContribution notices;
            StateArrayContribution activities;
            StateArrayContribution diagnostics;
        };
    } // namespace detail

    struct detail::StateStorage {
        std::uint64_t revision = 0;
        StateFreshness freshness = StateFreshness::Stale;
        RepresentationMode representationMode = RepresentationMode::Unknown;
        std::optional<frontend::SequenceNumber> visibleSequence;
        std::optional<frontend::SequenceNumber> synchronizedThrough;
        // The visible boundary retained from an interrupted synchronization.
        // It is validation-only: a later replay may overlap it without
        // duplicating append-oriented records, while a same-sequence group
        // split during the current synchronization remains an error.
        std::optional<frontend::SequenceNumber> retainedReplayThrough;
        std::optional<frontend::SequenceNumber> lastSynchronizationBatchSequence;
        std::optional<SessionInfo> session;
        BackendCursorState backendCursor;
        ProjectionMetadataState projectionMetadata;
        std::optional<ProjectionFingerprintMetadata> projectionFingerprint;
        Projected<ProviderState> provider;
        Projected<ControllerState> controller;
        Projected<std::vector<SessionState>> sessions;
        Projected<ThreadListState> threadList;
        bool threadProjectionPresent = false;
        std::vector<ThreadState> threads;
        bool turnProjectionPresent = false;
        std::vector<TurnState> turns;
        bool itemProjectionPresent = false;
        std::vector<ItemState> items;
        bool pendingRequestProjectionPresent = false;
        std::vector<PendingRequestState> pendingRequests;
        Projected<AccountState> accounts;
        Projected<ModelsState> models;
        Projected<ConfigurationState> configuration;
        Projected<ProcessCollectionState> processes;
        Projected<FilesystemWatchCollectionState> filesystemWatches;
        Projected<FuzzySearchCollectionState> fuzzySearches;
        Projected<PermissionProfilesState> permissionProfiles;
        Projected<ReviewsState> reviews;
        Projected<AppsState> apps;
        Projected<ExternalAgentsState> externalAgents;
        Projected<HooksState> hooks;
        Projected<MarketplaceState> marketplace;
        Projected<PluginsState> plugins;
        Projected<SkillsState> skills;
        Projected<McpState> mcp;
        Projected<WindowsSandboxState> windowsSandbox;
        Projected<PlatformState> platform;
        Projected<NoticeCollectionState> notices;
        Projected<ActivityCollectionState> activities;
        Projected<CapacityState> capacity;
        Projected<TruncationMetadata> truncation;
        Projected<DiagnosticCollectionState> diagnostics;
        frontend::Json compatibilityExtensions = frontend::Json::object();
        mutable StateSizeLedger sizeLedger;
    };

    namespace {
        constexpr std::size_t MaximumRetainedNotices = 256;
        constexpr std::size_t MaximumRetainedCompatibilityExtensions = 64;

        std::optional<std::string> stringMember(const frontend::Json& object, std::string_view key) {
            if (!object.is_object()) {
                return std::nullopt;
            }
            const auto found = object.find(std::string(key));
            return found != object.end() && found->is_string() ? std::optional(found->get<std::string>()) : std::nullopt;
        }

        std::optional<bool> optionalBool(const frontend::Json& object, std::string_view key) {
            if (!object.is_object()) {
                return std::nullopt;
            }
            const auto found = object.find(std::string(key));
            return found != object.end() && found->is_boolean() ? std::optional(found->get<bool>()) : std::nullopt;
        }

        std::optional<std::uint64_t> optionalUnsigned(const frontend::Json& object, std::string_view key) {
            if (!object.is_object()) {
                return std::nullopt;
            }
            const auto found = object.find(std::string(key));
            if (found == object.end()) {
                return std::nullopt;
            }
            if (found->is_number_unsigned()) {
                return found->get<std::uint64_t>();
            }
            if (found->is_number_integer()) {
                const std::int64_t value = found->get<std::int64_t>();
                if (value >= 0) {
                    return static_cast<std::uint64_t>(value);
                }
            }
            return std::nullopt;
        }

        std::optional<std::size_t> optionalSize(const frontend::Json& object, std::string_view key) {
            const auto value = optionalUnsigned(object, key);
            if (!value || *value > std::numeric_limits<std::size_t>::max()) {
                return std::nullopt;
            }
            return static_cast<std::size_t>(*value);
        }

        std::optional<std::int64_t> optionalInteger(const frontend::Json& object, std::string_view key) {
            if (!object.is_object()) {
                return std::nullopt;
            }
            const auto found = object.find(std::string(key));
            return found != object.end() && found->is_number_integer() ? std::optional(found->get<std::int64_t>()) : std::nullopt;
        }

        std::optional<double> optionalNumber(const frontend::Json& object, std::string_view key) {
            if (!object.is_object())
                return std::nullopt;
            const auto found = object.find(std::string(key));
            return found != object.end() && found->is_number() ? std::optional(found->get<double>()) : std::nullopt;
        }

        frontend::Json extensionsOf(const frontend::Json& object, std::initializer_list<std::string_view> known) {
            frontend::Json result = frontend::Json::object();
            if (!object.is_object()) {
                return result;
            }
            for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
                const bool recognized = std::any_of(known.begin(), known.end(), [&](std::string_view name) {
                    return iterator.key() == name;
                });
                if (!recognized) {
                    result[iterator.key()] = iterator.value();
                }
            }
            return result;
        }

        bool decodeExtensions(const frontend::Json& value,
                              std::initializer_list<std::string_view> known,
                              frontend::Json& result,
                              std::string_view description,
                              std::string& error) {
            result = frontend::Json::object();
            if (const auto extensions = value.find("extensions"); extensions != value.end()) {
                if (!extensions->is_object()) {
                    error = std::string(description) + " extensions must be an object";
                    return false;
                }
                result = *extensions;
            }
            const frontend::Json unknown = extensionsOf(value, known);
            for (auto member = unknown.begin(); member != unknown.end(); ++member)
                result[member.key()] = member.value();
            return true;
        }

        bool requireObject(const frontend::Json& value, std::string_view description, std::string& error) {
            if (value.is_object()) {
                return true;
            }
            error = std::string(description) + " must be an object";
            return false;
        }

        std::string_view projectionPathRoot(std::string_view path) {
            if (!path.starts_with('/') || path.size() == 1)
                return {};
            path.remove_prefix(1);
            const std::size_t separator = path.find('/');
            return path.substr(0, separator);
        }

        template <typename T>
        void addProjectedOmission(Projected<T>& projection, const std::string& path) {
            projection.omittedFields.push_back(path);
            std::sort(projection.omittedFields.begin(), projection.omittedFields.end());
            projection.omittedFields.erase(std::unique(projection.omittedFields.begin(), projection.omittedFields.end()),
                                           projection.omittedFields.end());
        }

        void applySnapshotProjectionOmission(detail::StateStorage& state, const std::string& path) {
            const std::string_view root = projectionPathRoot(path);
            if (root == "provider")
                addProjectedOmission(state.provider, path);
            else if (root == "controller")
                addProjectedOmission(state.controller, path);
            else if (root == "sessions")
                addProjectedOmission(state.sessions, path);
            else if (root == "threadList")
                addProjectedOmission(state.threadList, path);
            else if (root == "accounts")
                addProjectedOmission(state.accounts, path);
            else if (root == "models")
                addProjectedOmission(state.models, path);
            else if (root == "configuration")
                addProjectedOmission(state.configuration, path);
            else if (root == "processes")
                addProjectedOmission(state.processes, path);
            else if (root == "filesystemWatches")
                addProjectedOmission(state.filesystemWatches, path);
            else if (root == "fuzzySearches")
                addProjectedOmission(state.fuzzySearches, path);
            else if (root == "permissionProfiles")
                addProjectedOmission(state.permissionProfiles, path);
            else if (root == "reviews")
                addProjectedOmission(state.reviews, path);
            else if (root == "apps")
                addProjectedOmission(state.apps, path);
            else if (root == "externalAgents")
                addProjectedOmission(state.externalAgents, path);
            else if (root == "hooks")
                addProjectedOmission(state.hooks, path);
            else if (root == "marketplace")
                addProjectedOmission(state.marketplace, path);
            else if (root == "plugins")
                addProjectedOmission(state.plugins, path);
            else if (root == "skills")
                addProjectedOmission(state.skills, path);
            else if (root == "mcp")
                addProjectedOmission(state.mcp, path);
            else if (root == "windowsSandbox")
                addProjectedOmission(state.windowsSandbox, path);
            else if (root == "remoteControl")
                addProjectedOmission(state.platform, path);
            else if (root == "notices")
                addProjectedOmission(state.notices, path);
            else if (root == "activities")
                addProjectedOmission(state.activities, path);
            else if (root == "capacity")
                addProjectedOmission(state.capacity, path);
            else if (root == "truncation")
                addProjectedOmission(state.truncation, path);
            else if (root == "diagnostics")
                addProjectedOmission(state.diagnostics, path);
        }

        std::string_view providerLifecycleName(ProviderLifecycle value) noexcept {
            using enum ProviderLifecycle;
            switch (value) {
                case Stopped:
                    return "stopped";
                case Starting:
                    return "starting";
                case Initializing:
                    return "initializing";
                case Ready:
                    return "ready";
                case Stopping:
                    return "stopping";
                case Failed:
                    return "failed";
                case Recovering:
                    return "recovering";
            }
            return "stopped";
        }

        std::string_view providerRecoveryStatusName(ProviderRecoveryStatus value) noexcept {
            using enum ProviderRecoveryStatus;
            switch (value) {
                case Idle:
                    return "idle";
                case Waiting:
                    return "waiting";
                case Exhausted:
                    return "exhausted";
            }
            return "idle";
        }

        bool decodeStamp(const frontend::Json& value, SourceStamp& result, std::string& error, bool strict = true) {
            if (!value.is_object()) {
                if (!strict) {
                    return false;
                }
                error = "source stamp must be an object";
                return false;
            }
            const auto generation = optionalUnsigned(value, "generation");
            const auto freshnessName = stringMember(value, "freshness");
            const auto freshness = freshnessName ? frontend::stateFreshnessFromString(*freshnessName) : std::nullopt;
            if (!generation || !freshness) {
                if (!strict) {
                    return false;
                }
                error = "source stamp requires generation and a known freshness";
                return false;
            }
            result = SourceStamp{*generation, *freshness};
            result.extensions = extensionsOf(value, {"generation", "freshness"});
            return true;
        }

        bool decodeProviderError(const frontend::Json& value, ProviderErrorState& result, std::string& error, bool strict) {
            if (!requireObject(value, "provider error", error))
                return false;
            const auto category = stringMember(value, "category");
            const auto code = optionalInteger(value, "code");
            const auto detailsOmitted = optionalBool(value, "detailsOmitted");
            if (!category || !code || (strict && !detailsOmitted)) {
                error = "provider error lacks required stable fields";
                return false;
            }
            result.category = *category;
            result.code = *code;
            result.detailsOmitted = detailsOmitted;
            result.message = stringMember(value, "message");
            result.extensions = extensionsOf(value, {"category", "code", "detailsOmitted", "message"});
            return true;
        }

        bool decodeProviderInitialization(const frontend::Json& value, ProviderInitializationState& result, std::string& error) {
            if (!requireObject(value, "provider initialization", error))
                return false;
            const auto platformFamily = stringMember(value, "platformFamily");
            const auto platformOs = stringMember(value, "platformOs");
            const auto userAgent = stringMember(value, "userAgent");
            const auto codexHome = stringMember(value, "codexHome");
            if (!codexHome || !platformFamily || !platformOs || !userAgent) {
                error = "provider initialization lacks required stable fields";
                return false;
            }
            result.codexHome = typed::AbsolutePath{*codexHome};
            result.platformFamily = *platformFamily;
            result.platformOs = *platformOs;
            result.userAgent = *userAgent;
            result.extensions = extensionsOf(value, {"codexHome", "platformFamily", "platformOs", "userAgent"});
            return true;
        }

        bool decodeThreadRealtime(const frontend::Json& value, ThreadRealtimeState& result, std::string& error) {
            if (!requireObject(value, "thread realtime state", error))
                return false;
            const auto lifecycle = stringMember(value, "lifecycle");
            const auto transcript = stringMember(value, "transcript");
            const auto itemCount = optionalSize(value, "itemCount");
            const auto receivedAudioBytes = optionalUnsigned(value, "receivedAudioBytes");
            const auto droppedAudioBytes = optionalUnsigned(value, "droppedAudioBytes");
            const auto transcriptTruncated = optionalBool(value, "transcriptTruncated");
            if (!lifecycle || !transcript || !itemCount || !receivedAudioBytes || !droppedAudioBytes || !transcriptTruncated) {
                error = "thread realtime state lacks required stable fields";
                return false;
            }
            result.lifecycle = *lifecycle;
            result.transcript = *transcript;
            result.itemCount = *itemCount;
            result.receivedAudioBytes = *receivedAudioBytes;
            result.droppedAudioBytes = *droppedAudioBytes;
            result.transcriptTruncated = *transcriptTruncated;
            result.errorDetailsOmitted = optionalBool(value, "errorDetailsOmitted");
            result.sessionId = stringMember(value, "sessionId");
            if (const auto version = stringMember(value, "version"))
                result.version = typed::RealtimeConversationVersion{*version};
            result.lastSdpBytes = optionalUnsigned(value, "lastSdpBytes");
            result.extensions = extensionsOf(value,
                                             {"lifecycle",
                                              "transcript",
                                              "itemCount",
                                              "receivedAudioBytes",
                                              "droppedAudioBytes",
                                              "transcriptTruncated",
                                              "errorDetailsOmitted",
                                              "sessionId",
                                              "version",
                                              "lastSdpBytes"});
            return true;
        }

        bool decodeThread(const frontend::Json& value, ThreadState& result, std::string& error) {
            if (!requireObject(value, "thread", error))
                return false;
            const auto id = stringMember(value, "id");
            if (!id || id->empty()) {
                error = "thread state lacks a nonempty id";
                return false;
            }
            result.id = typed::ThreadId{*id};
            result.title = stringMember(value, "title");
            if (!result.title)
                result.title = stringMember(value, "name");
            result.preview = stringMember(value, "preview");
            if (const auto cwd = stringMember(value, "cwd"))
                result.cwd = typed::AbsolutePath{*cwd};
            if (const auto model = stringMember(value, "model"))
                result.model = typed::ModelId{*model};
            result.modelProvider = stringMember(value, "modelProvider");
            result.status = stringMember(value, "status");
            result.fullyLoaded = optionalBool(value, "fullyLoaded").value_or(false);
            if (const auto realtime = value.find("realtime"); realtime != value.end()) {
                ThreadRealtimeState decoded;
                if (!decodeThreadRealtime(*realtime, decoded, error))
                    return false;
                result.realtime = std::move(decoded);
            }
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                SourceStamp decoded;
                if (!decodeStamp(*stamp, decoded, error))
                    return false;
                result.stamp = decoded;
            }
            result.createdAtMs = optionalInteger(value, "createdAtMs");
            if (!result.createdAtMs)
                result.createdAtMs = optionalInteger(value, "createdAt");
            result.updatedAtMs = optionalInteger(value, "updatedAtMs");
            if (!result.updatedAtMs)
                result.updatedAtMs = optionalInteger(value, "updatedAt");
            if (const auto turns = value.find("turns"); turns != value.end()) {
                if (!turns->is_array()) {
                    error = "thread turns must be an array";
                    return false;
                }
                for (const frontend::Json& turn : *turns) {
                    const auto turnId = stringMember(turn, "id");
                    if (!turnId || turnId->empty()) {
                        error = "thread turn lacks a nonempty id";
                        return false;
                    }
                    const typed::TurnId idValue{*turnId};
                    if (std::find(result.orderedTurns.begin(), result.orderedTurns.end(), idValue) != result.orderedTurns.end()) {
                        error = "thread turns contain a duplicate id";
                        return false;
                    }
                    result.orderedTurns.push_back(idValue);
                }
            }
            return decodeExtensions(value,
                                    {"id",
                                     "title",
                                     "name",
                                     "preview",
                                     "cwd",
                                     "model",
                                     "modelProvider",
                                     "status",
                                     "fullyLoaded",
                                     "realtime",
                                     "stamp",
                                     "createdAt",
                                     "updatedAt",
                                     "createdAtMs",
                                     "updatedAtMs",
                                     "turns",
                                     "extensions"},
                                    result.extensions,
                                    "thread",
                                    error);
        }

        bool decodeTurn(const frontend::Json& value,
                        TurnState& result,
                        std::string& error,
                        std::optional<typed::ThreadId> fallbackThread = std::nullopt,
                        bool strict = true) {
            if (!requireObject(value, "turn", error))
                return false;
            const auto id = stringMember(value, "id");
            const auto threadId = stringMember(value, "threadId");
            const auto status = stringMember(value, "status");
            const auto active = optionalBool(value, "active");
            const auto terminal = optionalBool(value, "terminal");
            if ((!id || id->empty() || (!threadId && !fallbackThread) || (strict && (!status || !active || !terminal)))) {
                error = "turn state lacks required stable fields";
                return false;
            }
            result.id = typed::TurnId{*id};
            result.threadId = threadId ? typed::ThreadId{*threadId} : *fallbackThread;
            result.status = typed::TurnStatus{status.value_or("unknown")};
            result.active = active.value_or(false);
            result.terminal = terminal.value_or(false);
            result.connectionInvalidated = optionalBool(value, "connectionInvalidated").value_or(false);
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                SourceStamp decoded;
                if (!decodeStamp(*stamp, decoded, error))
                    return false;
                result.stamp = decoded;
            }
            if (const auto items = value.find("items"); items != value.end()) {
                if (!items->is_array()) {
                    error = "turn items must be an array";
                    return false;
                }
                for (const frontend::Json& item : *items) {
                    const auto itemId = stringMember(item, "id");
                    if (!itemId || itemId->empty()) {
                        error = "turn item lacks a nonempty id";
                        return false;
                    }
                    const typed::ItemId idValue{*itemId};
                    if (std::find(result.orderedItems.begin(), result.orderedItems.end(), idValue) != result.orderedItems.end()) {
                        error = "turn items contain a duplicate id";
                        return false;
                    }
                    result.orderedItems.push_back(idValue);
                }
            }
            if (const auto failure = value.find("failure"); failure != value.end())
                result.failure = *failure;
            if (const auto usage = value.find("tokenUsage"); usage != value.end())
                result.tokenUsage = *usage;
            return decodeExtensions(value,
                                    {"id",
                                     "threadId",
                                     "status",
                                     "active",
                                     "terminal",
                                     "connectionInvalidated",
                                     "stamp",
                                     "items",
                                     "failure",
                                     "tokenUsage",
                                     "extensions"},
                                    result.extensions,
                                    "turn",
                                    error);
        }

        std::optional<frontend::ThreadItemKind> legacyItemKind(const frontend::Json& value) noexcept {
            try {
                auto typeName = stringMember(value, "type");
                if (!typeName)
                    typeName = stringMember(value, "kind");
                if (!typeName)
                    return std::nullopt;
                if (const auto direct = frontend::threadItemKindFromString(*typeName))
                    return direct;
                if (*typeName == "agent_message")
                    return frontend::ThreadItemKind::AgentMessage;
                if (*typeName == "user_message")
                    return frontend::ThreadItemKind::UserMessage;
                if (*typeName == "command_execution")
                    return frontend::ThreadItemKind::CommandExecution;
                if (*typeName == "file_change")
                    return frontend::ThreadItemKind::FileChange;
                if (*typeName == "web_search")
                    return frontend::ThreadItemKind::WebSearch;
                if (*typeName == "tool_call") {
                    const auto data = value.find("data");
                    return data != value.end() && data->is_object() && data->contains("server") ? frontend::ThreadItemKind::McpToolCall
                                                                                                : frontend::ThreadItemKind::DynamicToolCall;
                }
            } catch (...) {
            }
            return std::nullopt;
        }

        bool decodeLegacyItem(const frontend::Json& value,
                              ItemState& result,
                              std::string& error,
                              std::optional<typed::ThreadId> fallbackThread = std::nullopt,
                              std::optional<typed::TurnId> fallbackTurn = std::nullopt) {
            if (!requireObject(value, "legacy item", error))
                return false;
            const auto id = stringMember(value, "id");
            const auto typeName = stringMember(value, "type");
            const auto knownKind = legacyItemKind(value);
            if (!id || id->empty() || !typeName || typeName->empty()) {
                error = "legacy item lacks a valid id or type";
                return false;
            }
            result.id = typed::ItemId{*id};
            result.kind = ItemKind{knownKind ? std::string(frontend::toString(*knownKind)) : *typeName, knownKind};
            if (const auto threadId = stringMember(value, "threadId"))
                result.threadId = typed::ThreadId{*threadId};
            else
                result.threadId = std::move(fallbackThread);
            if (const auto turnId = stringMember(value, "turnId"))
                result.turnId = typed::TurnId{*turnId};
            else
                result.turnId = std::move(fallbackTurn);
            result.status = stringMember(value, "status");
            result.summary = stringMember(value, "summary");
            if (const auto location = value.find("location"); location != value.end())
                result.location = *location;
            result.agentText = stringMember(value, "agentText");
            result.reasoningText = stringMember(value, "reasoningText");
            result.reasoningSummary = stringMember(value, "reasoningSummary");
            result.commandOutput = stringMember(value, "commandOutput");
            result.droppedContentBytes = optionalUnsigned(value, "droppedContentBytes");
            result.contentTruncated = optionalBool(value, "contentTruncated").value_or(false);
            result.startedAtMs = optionalInteger(value, "startedAtMs");
            result.completedAtMs = optionalInteger(value, "completedAtMs");
            if (const auto data = value.find("data"); data != value.end())
                result.data = *data;
            result.truncated = optionalBool(value, "truncated").value_or(false);
            if (const auto omitted = value.find("omittedFields"); omitted != value.end()) {
                if (!omitted->is_array()) {
                    error = "legacy item omittedFields must be an array";
                    return false;
                }
                for (const frontend::Json& field : *omitted) {
                    if (!field.is_string()) {
                        error = "legacy item omittedFields must contain strings";
                        return false;
                    }
                    result.omittedFields.push_back(field.get<std::string>());
                }
            }
            result.connectionInvalidated = optionalBool(value, "connectionInvalidated").value_or(false);
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                SourceStamp decoded;
                if (!decodeStamp(*stamp, decoded, error))
                    return false;
                result.stamp = decoded;
            } else {
                const auto generation = optionalUnsigned(value, "generation");
                const auto freshnessName = stringMember(value, "freshness");
                const auto freshness = freshnessName ? frontend::stateFreshnessFromString(*freshnessName) : std::nullopt;
                if (generation && freshness)
                    result.stamp = SourceStamp{*generation, *freshness};
            }
            if (const auto extensions = value.find("extensions"); extensions != value.end()) {
                if (!extensions->is_object()) {
                    error = "legacy item extensions must be an object";
                    return false;
                }
                result.extensions = *extensions;
            }
            const frontend::Json unknown = extensionsOf(value,
                                                        {"id",
                                                         "type",
                                                         "kind",
                                                         "threadId",
                                                         "turnId",
                                                         "status",
                                                         "summary",
                                                         "location",
                                                         "agentText",
                                                         "reasoningText",
                                                         "reasoningSummary",
                                                         "commandOutput",
                                                         "droppedContentBytes",
                                                         "contentTruncated",
                                                         "startedAtMs",
                                                         "completedAtMs",
                                                         "data",
                                                         "truncated",
                                                         "omittedFields",
                                                         "connectionInvalidated",
                                                         "stamp",
                                                         "generation",
                                                         "freshness",
                                                         "extensions"});
            for (auto member = unknown.begin(); member != unknown.end(); ++member)
                result.extensions[member.key()] = member.value();
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
                return false;
            }
            return true;
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

        bool accountingFailure(detail::StateStorage& state, std::string& error) noexcept;

        bool accountingFailure(detail::StateStorage& state, std::string& error) noexcept {
            state.sizeLedger.failed = true;
            error = "decoded frontend state size accounting failed";
            return false;
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

    namespace {
        namespace canonical = frontend::internal::model;
        namespace canonical_client = frontend::internal::client;

        StateFreshness publicFreshness(canonical_client::PublishedFreshness value) noexcept {
            switch (value) {
                case canonical_client::PublishedFreshness::Current:
                    return StateFreshness::Current;
                case canonical_client::PublishedFreshness::Stale:
                    return StateFreshness::Stale;
                case canonical_client::PublishedFreshness::Synchronizing:
                    return StateFreshness::Synchronizing;
            }
            return StateFreshness::Stale;
        }

        RepresentationMode publicRepresentation(canonical_client::RepresentationMode value) noexcept {
            switch (value) {
                case canonical_client::RepresentationMode::Unknown:
                    return RepresentationMode::Unknown;
                case canonical_client::RepresentationMode::LegacyV1:
                    return RepresentationMode::LegacyV1;
                case canonical_client::RepresentationMode::ExpandedV1:
                    return RepresentationMode::ExpandedV1;
            }
            return RepresentationMode::Unknown;
        }

        frontend::StateFreshness publicSourceFreshness(canonical::Freshness value) noexcept {
            switch (value) {
                case canonical::Freshness::Unknown:
                    return frontend::StateFreshness::Unknown;
                case canonical::Freshness::Current:
                    return frontend::StateFreshness::Current;
                case canonical::Freshness::Stale:
                    return frontend::StateFreshness::Stale;
            }
            return frontend::StateFreshness::Unknown;
        }

        SourceStamp publicStamp(const canonical::SourceMetadata& value) {
            return SourceStamp{value.generation, publicSourceFreshness(value.freshness), value.extensions.json()};
        }

        TruncationMetadata publicTruncation(const canonical::TruncationMetadata& value) {
            TruncationMetadata result;
            result.truncated = value.truncated;
            result.omittedFields = value.omittedPaths;
            result.omittedEntries = value.omittedEntries;
            if (value.droppedBytesPresent || value.droppedBytes != 0) {
                result.droppedBytes = value.droppedBytes;
            }
            result.extensions = value.extensions.json();
            return result;
        }

        std::string_view informationName(canonical::InformationState value) noexcept {
            switch (value) {
                case canonical::InformationState::Present:
                    return "present";
                case canonical::InformationState::Omitted:
                    return "omitted";
                case canonical::InformationState::Redacted:
                    return "redacted";
                case canonical::InformationState::Truncated:
                    return "truncated";
                case canonical::InformationState::Unavailable:
                    return "unavailable";
                case canonical::InformationState::Stale:
                    return "stale";
                case canonical::InformationState::Unknown:
                    return "unknown";
                case canonical::InformationState::Absent:
                    return "absent";
                case canonical::InformationState::NullValue:
                    return "null";
            }
            return "unknown";
        }

        bool represented(canonical::InformationState value) noexcept {
            return value != canonical::InformationState::Absent && value != canonical::InformationState::Omitted &&
                   value != canonical::InformationState::NullValue;
        }

        std::optional<std::size_t> publicSize(const std::optional<std::uint64_t>& value) noexcept {
            if (!value.has_value() || *value > std::numeric_limits<std::size_t>::max()) {
                return std::nullopt;
            }
            return static_cast<std::size_t>(*value);
        }

        SessionInfo publicSession(const canonical_client::SessionInfo& value) {
            SessionInfo result;
            result.sessionId = value.id.value();
            result.role = value.role;
            result.syncMode = value.synchronizationMode;
            result.serverCurrentSequence = value.serverCurrentSequence.protocolValue();
            result.serverVersion = value.serverVersion;
            result.requestedRepresentationCapabilities = value.requestedCapabilities;
            result.selectedRepresentationCapabilities = value.selectedCapabilities;
            result.availableMethods = value.availableMethods;
            result.permittedMethods = value.permittedMethods;
            result.permittedScopes = value.permittedScopes;
            for (frontend::FrontendCapability capability : value.observedCapabilities) {
                const auto metadata = std::ranges::find_if(frontend::generated::AllCapabilities, [capability](const auto& candidate) {
                    return candidate.id == static_cast<frontend::generated::Capability>(capability);
                });
                if (metadata == frontend::generated::AllCapabilities.end()) {
                    continue;
                }
                switch (metadata->category) {
                    case frontend::generated::CapabilityCategory::StaticMechanism:
                        result.observedMechanismCapabilities.push_back(capability);
                        break;
                    case frontend::generated::CapabilityCategory::ConditionalTopology:
                        result.observedTopologyCapabilities.push_back(capability);
                        break;
                    case frontend::generated::CapabilityCategory::Product:
                        result.observedProductCapabilities.push_back(capability);
                        break;
                }
            }
            return result;
        }

        ProviderLifecycle publicProviderLifecycle(canonical::ProviderLifecycle value) noexcept {
            return static_cast<ProviderLifecycle>(value);
        }

        ProviderRecoveryStatus publicRecoveryStatus(canonical::ProviderRecoveryStatus value) noexcept {
            return static_cast<ProviderRecoveryStatus>(value);
        }

        bool buildProvider(const canonical::ProviderState& source, ProviderState& result, std::string& error) {
            result.lifecycle = publicProviderLifecycle(source.lifecycle);
            result.generation = source.generation;
            result.desiredRunning = source.desiredRunning;
            result.ready = source.ready();
            result.recovery.status = publicRecoveryStatus(source.recovery.status);
            if (source.recovery.attempts > std::numeric_limits<std::size_t>::max()) {
                error = "provider recovery attempts exceed the public State size range";
                return false;
            }
            result.recovery.attempts = static_cast<std::size_t>(source.recovery.attempts);
            result.recovery.delayMs = source.recovery.delayMs;
            result.recovery.extensions = source.recovery.extensions.json();
            if (source.lastError.has_value()) {
                ProviderErrorState decoded;
                if (!decodeProviderError(source.lastError->json(), decoded, error, false)) {
                    return false;
                }
                result.lastError = std::move(decoded);
            }
            if (source.initialization.has_value()) {
                ProviderInitializationState decoded;
                if (!decodeProviderInitialization(source.initialization->json(), decoded, error)) {
                    return false;
                }
                result.initialization = std::move(decoded);
            }
            result.extensions = source.extensions.json();
            if (source.provider.has_value()) {
                result.extensions["provider"] = *source.provider;
            }
            return true;
        }

        DomainProjectionState publicDomainProjection(const canonical::DomainState& source) {
            DomainProjectionState result;
            if (source.stampKnown) {
                result.stamp = publicStamp(source.stamp);
            }
            result.status = source.status;
            result.summary = source.summary;
            result.nextCursor = source.nextCursor;
            result.complete = source.completeKnown ? std::optional<bool>{source.complete} : std::nullopt;
            result.itemCount = publicSize(source.itemCount);
            result.latestResults.reserve(source.latestResults.size());
            for (const canonical::DomainResultSummary& entry : source.latestResults) {
                DomainResultSummaryState decoded;
                decoded.method = entry.method;
                decoded.status = entry.status;
                decoded.subjectId = entry.subjectId;
                decoded.nextCursor = entry.nextCursor;
                decoded.itemCount = publicSize(entry.itemCount);
                decoded.complete = entry.completeKnown ? std::optional<bool>{entry.complete} : std::nullopt;
                decoded.stamp = publicStamp(entry.stamp);
                decoded.extensions = entry.extensions.json();
                result.latestResults.push_back(std::move(decoded));
            }
            if (source.safeDetailsKnown) {
                const frontend::Json& details = source.safeDetails.json();
                result.notificationCount = optionalSize(details, "notificationCount");
                if (const auto methods = details.find("latestNotificationMethods"); methods != details.end() && methods->is_array()) {
                    for (const frontend::Json& method : *methods) {
                        if (method.is_string()) {
                            result.latestNotificationMethods.push_back(method.get<std::string>());
                        }
                    }
                }
                result.opaqueDetails = extensionsOf(details, {"notificationCount", "latestNotificationMethods"});
            }
            if (source.truncationKnown) {
                result.truncation = publicTruncation(source.truncation);
            }
            result.extensions = source.extensions.json();
            if (source.information != canonical::InformationState::Present) {
                result.extensions["informationState"] = informationName(source.information);
            }
            return result;
        }

        template <typename PublicDomain>
        bool buildDomain(const canonical::DomainState& source, Projected<PublicDomain>& destination, std::string& error) {
            if (!represented(source.information)) {
                return true;
            }
            PublicDomain value;
            value.projection = publicDomainProjection(source);
            const frontend::Json* details = source.safeDetailsKnown ? &source.safeDetails.json() : nullptr;
            if (!decodeSpecificDomainDetails(details, value, error)) {
                return false;
            }
            destination.value = std::move(value);
            destination.truncated = source.information == canonical::InformationState::Truncated || source.truncation.truncated;
            destination.omittedFields = source.truncation.omittedPaths;
            return true;
        }

        frontend::Json domainCollectionExtensions(const canonical::DomainState& source) {
            frontend::Json result = source.extensions.json();
            if (source.information != canonical::InformationState::Present) {
                result["informationState"] = informationName(source.information);
            }
            if (source.stampKnown) {
                result["stamp"] = frontend::Json{{"generation", source.stamp.generation},
                                                 {"freshness", frontend::toString(publicSourceFreshness(source.stamp.freshness))}};
                for (auto member = source.stamp.extensions.json().begin(); member != source.stamp.extensions.json().end(); ++member) {
                    result["stamp"][member.key()] = member.value();
                }
            }
            if (source.status.has_value()) {
                result["status"] = *source.status;
            }
            if (source.summary.has_value()) {
                result["summary"] = *source.summary;
            }
            if (source.nextCursor.has_value()) {
                result["nextCursor"] = *source.nextCursor;
            }
            if (source.itemCount.has_value()) {
                result["itemCount"] = *source.itemCount;
            }
            if (source.completeKnown) {
                result["complete"] = source.complete;
            }
            if (source.safeDetailsKnown) {
                result["details"] = source.safeDetails.json();
            }
            return result;
        }

        ItemState publicItem(const canonical::ItemData& source, ItemKind kind) {
            ItemState result;
            result.id = typed::ItemId{source.id.value()};
            if (source.threadId.has_value()) {
                result.threadId = typed::ThreadId{source.threadId->value()};
            }
            if (source.turnId.has_value()) {
                result.turnId = typed::TurnId{source.turnId->value()};
            }
            result.kind = std::move(kind);
            result.status = source.status;
            result.summary = source.summary;
            if (source.location.has_value()) {
                result.location = source.location->json();
            }
            result.agentText = source.agentText;
            result.reasoningText = source.reasoningText;
            result.reasoningSummary = source.reasoningSummary;
            result.commandOutput = source.commandOutput;
            result.droppedContentBytes = source.droppedContentBytes;
            result.contentTruncated = source.contentTruncated;
            result.startedAtMs = source.startedAtMs;
            result.completedAtMs = source.completedAtMs;
            if (source.safeDetails.has_value()) {
                result.data = source.safeDetails->json();
            }
            result.truncated = source.truncation.truncated;
            result.omittedFields = source.truncation.omittedPaths;
            result.connectionInvalidated = source.connectionInvalidated;
            if (source.generation.has_value()) {
                result.stamp = SourceStamp{*source.generation, publicSourceFreshness(source.freshness), source.stampExtensions.json()};
            }
            result.extensions = source.legacyExtensions.json();
            for (auto member = source.extensions.json().begin(); member != source.extensions.json().end(); ++member) {
                result.extensions[member.key()] = member.value();
            }
            return result;
        }

        PendingRequestState publicPending(const canonical::PendingRequestData& source, frontend::PendingRequestKind kind) {
            PendingRequestState result;
            result.id = PendingRequestId{source.id.value()};
            result.kind = kind;
            if (source.threadId.has_value()) {
                result.threadId = typed::ThreadId{source.threadId->value()};
            }
            if (source.turnId.has_value()) {
                result.turnId = typed::TurnId{source.turnId->value()};
            }
            if (source.itemId.has_value()) {
                result.itemId = typed::ItemId{source.itemId->value()};
            }
            result.summary = source.summary;
            if (source.safeDetails.has_value()) {
                result.opaqueDetails = source.safeDetails->json();
            }
            if (source.questionsPresent || !source.questions.empty()) {
                result.questions.emplace();
                for (const canonical::PendingRequestQuestion& question : source.questions) {
                    PendingRequestQuestionState decoded;
                    decoded.id = question.id;
                    decoded.header = question.header;
                    decoded.prompt = question.prompt;
                    decoded.allowsFreeText = question.allowsFreeText;
                    decoded.isSecret = question.secretAnswer;
                    decoded.extensions = question.extensions.json();
                    for (const canonical::PendingRequestOption& option : question.options) {
                        decoded.options.push_back({option.label, option.description, option.extensions.json()});
                    }
                    result.questions->push_back(std::move(decoded));
                }
            }
            result.autoResolutionMs = source.autoResolutionMs;
            result.truncated = source.truncation.truncated;
            result.omittedFields = source.truncation.omittedPaths;
            result.connectionInvalidated = source.connectionInvalidated;
            result.extensions = source.extensions.json();
            return result;
        }

        bool rootListed(const std::vector<std::string>& paths, std::string_view root) {
            return std::ranges::any_of(paths, [root](const std::string& path) {
                return path == root;
            });
        }

        bool rootUnrepresented(const canonical::ProjectionMetadata& projection, std::string_view root) {
            return rootListed(projection.omittedPaths, root) || rootListed(projection.absentPaths, root) ||
                   rootListed(projection.nullPaths, root);
        }

        bool insertCanonicalIdentity(std::set<std::string>& identities,
                                     std::string_view identity,
                                     std::string_view collection,
                                     std::string& error) {
            if (identity.empty()) {
                error = "canonical state contains an empty " + std::string(collection) + " identity";
                return false;
            }
            if (identities.emplace(identity).second) {
                return true;
            }
            error = "canonical state contains a duplicate " + std::string(collection) + " identity";
            return false;
        }

        bool validateCanonicalLookupIdentities(const canonical::CanonicalSnapshot& source, std::string& error) {
            std::set<std::string> identities;
            for (const canonical::SessionState& session : source.sessions) {
                if (!insertCanonicalIdentity(identities, session.id.value(), "session", error)) {
                    return false;
                }
            }

            identities.clear();
            for (const canonical::ThreadState& thread : source.threads) {
                if (!insertCanonicalIdentity(identities, thread.id.value(), "thread", error)) {
                    return false;
                }
            }

            identities.clear();
            for (const canonical::TurnState& turn : source.turns) {
                if (!insertCanonicalIdentity(identities, turn.id.value(), "turn", error)) {
                    return false;
                }
            }

            identities.clear();
            for (const canonical::ThreadItem& item : source.items) {
                if (!insertCanonicalIdentity(identities, canonical::itemData(item).id.value(), "item", error)) {
                    return false;
                }
            }
            for (const canonical::LegacyItemCompatibility& item : source.legacyItems) {
                if (!insertCanonicalIdentity(identities, item.value.id.value(), "item", error)) {
                    return false;
                }
            }

            identities.clear();
            for (const canonical::PendingRequest& request : source.pendingRequests) {
                if (!insertCanonicalIdentity(identities, canonical::pendingRequestData(request).id.value(), "pending request", error)) {
                    return false;
                }
            }
            for (const canonical::LegacyPendingRequestCompatibility& request : source.legacyPendingRequests) {
                if (!insertCanonicalIdentity(identities, request.value.id.value(), "pending request", error)) {
                    return false;
                }
            }

            identities.clear();
            for (const canonical::ProcessState& process : source.processes) {
                if (!insertCanonicalIdentity(identities, process.handle.value(), "process", error)) {
                    return false;
                }
            }

            identities.clear();
            for (const canonical::FilesystemWatchRecord& watch : source.filesystemWatches.entries) {
                if (!insertCanonicalIdentity(identities, watch.watchId, "filesystem watch", error)) {
                    return false;
                }
            }

            identities.clear();
            for (const canonical::FuzzySearchRecord& search : source.fuzzySearches.entries) {
                if (!insertCanonicalIdentity(identities, search.sessionId, "fuzzy search", error)) {
                    return false;
                }
            }

            identities.clear();
            for (const canonical::ActivityRecord& activity : source.activities.entries) {
                if (!insertCanonicalIdentity(identities, activity.key, "activity", error)) {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    std::optional<std::shared_ptr<const detail::StateStorage>>
    detail::CanonicalStateBuilder::build(const canonical_client::PublishedState& publication,
                                         std::size_t maximumBytes,
                                         std::size_t maximumRetainedDiagnostics,
                                         std::string& error,
                                         CanonicalStateBuildFailure* failure) noexcept {
        if (failure != nullptr) {
            *failure = CanonicalStateBuildFailure::StateDivergence;
        }
        try {
            auto result = std::make_shared<StateStorage>();
            result->revision = publication.revision;
            result->freshness = publicFreshness(publication.freshness);
            result->representationMode = publicRepresentation(publication.representation);
            if (publication.visibleSequence.has_value()) {
                result->visibleSequence = publication.visibleSequence->protocolValue();
            }
            if (publication.synchronizedThrough.has_value()) {
                result->synchronizedThrough = publication.synchronizedThrough->protocolValue();
            }
            if (publication.session.has_value()) {
                result->session = publicSession(*publication.session);
            }
            if (publication.projectionFingerprint.has_value()) {
                result->projectionFingerprint = ProjectionFingerprintMetadata{*publication.projectionFingerprint};
            }

            if (publication.snapshot) {
                const canonical::CanonicalSnapshot& source = *publication.snapshot;
                if (!validateCanonicalLookupIdentities(source, error)) {
                    return std::nullopt;
                }
                if (!source.legacyPendingRequests.empty()) {
                    error =
                        "canonical state contains a legacy pending-request kind that the frozen public PendingRequestKind cannot represent";
                    return std::nullopt;
                }
                result->backendCursor.backendRevision = source.backendCursor.backendRevision;
                if (source.backendCursor.oldestReplayableAfter.has_value()) {
                    result->backendCursor.oldestReplayableAfter = source.backendCursor.oldestReplayableAfter->protocolValue();
                }
                if (source.backendCursor.currentSequence.has_value()) {
                    result->backendCursor.currentSequence = source.backendCursor.currentSequence->protocolValue();
                }
                if (source.backendCursor.oldestRetainedSequence.has_value()) {
                    result->backendCursor.oldestRetainedSequence = source.backendCursor.oldestRetainedSequence->protocolValue();
                }
                if (source.backendCursor.newestRetainedSequence.has_value()) {
                    result->backendCursor.newestRetainedSequence = source.backendCursor.newestRetainedSequence->protocolValue();
                }
                result->backendCursor.backendSequenceExhausted = source.backendCursor.backendSequenceExhausted;
                result->backendCursor.frontendSequenceExhausted = source.backendCursor.frontendSequenceExhausted;

                result->projectionMetadata.omittedFields = source.projection.omittedPaths;
                result->projectionMetadata.redactedFields = source.projection.redactedPaths;

                ProviderState provider;
                if (!buildProvider(source.provider, provider, error)) {
                    return std::nullopt;
                }
                result->provider.value = std::move(provider);

                ControllerState controller;
                if (source.controller.session.has_value()) {
                    controller.sessionId = FrontendSessionId{source.controller.session->value()};
                }
                frontend::Json controllerExtensions = source.controller.safeDetails.json();
                controller.present = optionalBool(controllerExtensions, "present")
                                         .value_or(source.controller.session.has_value() || source.controller.controller.has_value());
                controller.ownedByThisClient = result->session.has_value() && controller.sessionId.has_value() &&
                                               controller.sessionId->value == result->session->sessionId;
                controllerExtensions.erase("present");
                controller.extensions = std::move(controllerExtensions);
                if (source.controller.controller.has_value()) {
                    controller.extensions["controllerId"] = source.controller.controller->value();
                }
                result->controller.value = std::move(controller);

                if (source.sessionsPresent) {
                    result->sessions.value.emplace();
                    result->sessions.value->reserve(source.sessions.size());
                    for (const canonical::SessionState& session : source.sessions) {
                        frontend::Json extensions = session.safeDetails.json();
                        if (session.principalId.has_value()) {
                            extensions["principalId"] = *session.principalId;
                        }
                        result->sessions.value->push_back(
                            SessionState{FrontendSessionId{session.id.value()}, session.role, std::move(extensions)});
                    }
                }

                if (source.threadListPresent) {
                    ThreadListState threadList;
                    threadList.hasLoadedPage = source.threadList.hasLoadedPage;
                    threadList.complete = source.threadList.complete;
                    if (source.threadList.pagesLoaded > std::numeric_limits<std::size_t>::max()) {
                        error = "thread-list page count exceeds the public State size range";
                        return std::nullopt;
                    }
                    threadList.pagesLoaded = static_cast<std::size_t>(source.threadList.pagesLoaded);
                    threadList.nextCursor = source.threadList.nextCursor;
                    threadList.backwardsCursor = source.threadList.backwardsCursor;
                    if (source.threadList.stampKnown) {
                        threadList.stamp = publicStamp(source.threadList.stamp);
                    }
                    threadList.extensions = source.threadList.safeDetails.json();
                    result->threadList.value = std::move(threadList);
                }

                result->threadProjectionPresent = source.threadsPresent && !rootUnrepresented(source.projection, "/threads");
                result->turnProjectionPresent = source.turnsPresent && !rootUnrepresented(source.projection, "/turns");
                result->itemProjectionPresent = source.itemsPresent && !rootUnrepresented(source.projection, "/items");
                result->pendingRequestProjectionPresent =
                    source.pendingRequestsPresent && !rootUnrepresented(source.projection, "/pendingRequests");

                result->threads.reserve(source.threads.size());
                for (const canonical::ThreadState& thread : source.threads) {
                    ThreadState decoded;
                    decoded.id = typed::ThreadId{thread.id.value()};
                    decoded.title = thread.title;
                    decoded.fullyLoaded = thread.fullyLoaded;
                    decoded.createdAtMs = thread.createdAtMs;
                    decoded.updatedAtMs = thread.updatedAtMs;
                    if (thread.stampKnown) {
                        decoded.stamp = publicStamp(thread.stamp);
                    }
                    const frontend::Json& details = thread.safeDetails.json();
                    if (!decoded.title.has_value()) {
                        decoded.title = stringMember(details, "name");
                    }
                    decoded.preview = stringMember(details, "preview");
                    if (const auto cwd = stringMember(details, "cwd")) {
                        decoded.cwd = typed::AbsolutePath{*cwd};
                    }
                    if (const auto model = stringMember(details, "model")) {
                        decoded.model = typed::ModelId{*model};
                    }
                    decoded.modelProvider = stringMember(details, "modelProvider");
                    decoded.status = stringMember(details, "status");
                    if (const auto realtime = details.find("realtime"); realtime != details.end()) {
                        ThreadRealtimeState value;
                        if (!decodeThreadRealtime(*realtime, value, error)) {
                            return std::nullopt;
                        }
                        decoded.realtime = std::move(value);
                    }
                    frontend::Json canonicalExtensions;
                    if (!decodeExtensions(details,
                                          {"name", "preview", "cwd", "model", "modelProvider", "status", "realtime", "extensions"},
                                          canonicalExtensions,
                                          "canonical thread",
                                          error)) {
                        return std::nullopt;
                    }
                    decoded.extensions = thread.legacyExtensions.json();
                    for (auto member = canonicalExtensions.begin(); member != canonicalExtensions.end(); ++member) {
                        decoded.extensions[member.key()] = member.value();
                    }
                    for (const canonical::TurnState& turn : source.turns) {
                        if (turn.threadId == thread.id) {
                            decoded.orderedTurns.push_back(typed::TurnId{turn.id.value()});
                        }
                    }
                    result->threads.push_back(std::move(decoded));
                }

                result->turns.reserve(source.turns.size());
                for (const canonical::TurnState& turn : source.turns) {
                    TurnState decoded;
                    decoded.id = typed::TurnId{turn.id.value()};
                    decoded.threadId = typed::ThreadId{turn.threadId.value()};
                    decoded.status = typed::TurnStatus{turn.status.value_or("unknown")};
                    decoded.active = turn.active;
                    decoded.terminal = turn.terminal;
                    decoded.connectionInvalidated = turn.connectionInvalidated;
                    if (turn.stampKnown) {
                        decoded.stamp = publicStamp(turn.stamp);
                    }
                    const frontend::Json& details = turn.safeDetails.json();
                    if (const auto failure = details.find("failure"); failure != details.end()) {
                        decoded.failure = *failure;
                    }
                    if (const auto usage = details.find("tokenUsage"); usage != details.end()) {
                        decoded.tokenUsage = *usage;
                    }
                    frontend::Json canonicalExtensions;
                    if (!decodeExtensions(details, {"failure", "tokenUsage", "extensions"}, canonicalExtensions, "canonical turn", error)) {
                        return std::nullopt;
                    }
                    decoded.extensions = turn.legacyExtensions.json();
                    for (auto member = canonicalExtensions.begin(); member != canonicalExtensions.end(); ++member) {
                        decoded.extensions[member.key()] = member.value();
                    }
                    std::vector<std::pair<std::size_t, typed::ItemId>> orderedTurnItems;
                    std::size_t fallbackTurnItemIndex = 0;
                    for (const canonical::ThreadItem& item : source.items) {
                        const canonical::ItemData& data = canonical::itemData(item);
                        if (data.turnId.has_value() && *data.turnId == turn.id) {
                            orderedTurnItems.emplace_back(data.sourceIndex.value_or(fallbackTurnItemIndex++),
                                                          typed::ItemId{data.id.value()});
                        }
                    }
                    for (const canonical::LegacyItemCompatibility& item : source.legacyItems) {
                        if (item.value.turnId.has_value() && *item.value.turnId == turn.id) {
                            orderedTurnItems.emplace_back(item.sourceIndex, typed::ItemId{item.value.id.value()});
                        }
                    }
                    std::stable_sort(orderedTurnItems.begin(), orderedTurnItems.end(), [](const auto& left, const auto& right) {
                        return left.first < right.first;
                    });
                    for (auto& [index, item] : orderedTurnItems) {
                        (void) index;
                        decoded.orderedItems.push_back(std::move(item));
                    }
                    result->turns.push_back(std::move(decoded));
                }

                std::vector<std::pair<std::size_t, ItemState>> orderedItems;
                orderedItems.reserve(source.items.size() + source.legacyItems.size());
                std::size_t fallbackItemIndex = 0;
                for (const canonical::ThreadItem& item : source.items) {
                    const canonical::ItemData& data = canonical::itemData(item);
                    orderedItems.emplace_back(data.sourceIndex.value_or(fallbackItemIndex++),
                                              publicItem(data, ItemKind{canonical::threadItemKind(item)}));
                }
                for (const canonical::LegacyItemCompatibility& item : source.legacyItems) {
                    const auto known = frontend::threadItemKindFromString(item.discriminator);
                    orderedItems.emplace_back(item.sourceIndex, publicItem(item.value, ItemKind{item.discriminator, known}));
                }
                std::stable_sort(orderedItems.begin(), orderedItems.end(), [](const auto& left, const auto& right) {
                    return left.first < right.first;
                });
                result->items.reserve(orderedItems.size());
                for (auto& [index, item] : orderedItems) {
                    (void) index;
                    result->items.push_back(std::move(item));
                }

                std::vector<std::pair<std::size_t, PendingRequestState>> orderedPending;
                orderedPending.reserve(source.pendingRequests.size());
                std::size_t fallbackPendingIndex = 0;
                for (const canonical::PendingRequest& request : source.pendingRequests) {
                    const canonical::PendingRequestData& data = canonical::pendingRequestData(request);
                    orderedPending.emplace_back(data.sourceIndex.value_or(fallbackPendingIndex++),
                                                publicPending(data, canonical::pendingRequestKind(request)));
                }
                std::stable_sort(orderedPending.begin(), orderedPending.end(), [](const auto& left, const auto& right) {
                    return left.first < right.first;
                });
                result->pendingRequests.reserve(orderedPending.size());
                for (auto& [index, request] : orderedPending) {
                    (void) index;
                    result->pendingRequests.push_back(std::move(request));
                }

                const bool legacyRepresentation = publication.representation == canonical_client::RepresentationMode::LegacyV1;
                const canonical::DomainState& permissionProfiles =
                    legacyRepresentation && !represented(source.permissionProfiles.state.information) ? source.reviews.state
                                                                                                      : source.permissionProfiles.state;
                const canonical::DomainState& apps =
                    legacyRepresentation && !represented(source.apps.state.information) ? source.integrations.state : source.apps.state;
                const canonical::DomainState& externalAgents = legacyRepresentation && !represented(source.externalAgents.state.information)
                                                                   ? source.integrations.state
                                                                   : source.externalAgents.state;
                const canonical::DomainState& hooks =
                    legacyRepresentation && !represented(source.hooks.state.information) ? source.integrations.state : source.hooks.state;
                const canonical::DomainState& marketplace = legacyRepresentation && !represented(source.marketplace.state.information)
                                                                ? source.integrations.state
                                                                : source.marketplace.state;
                const canonical::DomainState& skills =
                    legacyRepresentation && !represented(source.skills.state.information) ? source.plugins.state : source.skills.state;
                const canonical::DomainState& platform = !legacyRepresentation || represented(source.remoteControl.state.information)
                                                             ? source.remoteControl.state
                                                             : source.platform.state;
                const canonical::DomainState& windowsSandbox = legacyRepresentation && !represented(source.windowsSandbox.state.information)
                                                                   ? source.platform.state
                                                                   : source.windowsSandbox.state;

#define AISUITE_BUILD_CANONICAL_DOMAIN(sourceMember, destinationMember)                                                                    \
    if (!buildDomain(sourceMember, result->destinationMember, error)) {                                                                    \
        return std::nullopt;                                                                                                               \
    }
                AISUITE_BUILD_CANONICAL_DOMAIN(source.accounts.state, accounts)
                AISUITE_BUILD_CANONICAL_DOMAIN(source.models.state, models)
                AISUITE_BUILD_CANONICAL_DOMAIN(source.configuration.state, configuration)
                AISUITE_BUILD_CANONICAL_DOMAIN(permissionProfiles, permissionProfiles)
                AISUITE_BUILD_CANONICAL_DOMAIN(source.reviews.state, reviews)
                AISUITE_BUILD_CANONICAL_DOMAIN(apps, apps)
                AISUITE_BUILD_CANONICAL_DOMAIN(externalAgents, externalAgents)
                AISUITE_BUILD_CANONICAL_DOMAIN(hooks, hooks)
                AISUITE_BUILD_CANONICAL_DOMAIN(marketplace, marketplace)
                AISUITE_BUILD_CANONICAL_DOMAIN(source.plugins.state, plugins)
                AISUITE_BUILD_CANONICAL_DOMAIN(skills, skills)
                AISUITE_BUILD_CANONICAL_DOMAIN(source.mcp.state, mcp)
                AISUITE_BUILD_CANONICAL_DOMAIN(windowsSandbox, windowsSandbox)
                AISUITE_BUILD_CANONICAL_DOMAIN(platform, platform)
#undef AISUITE_BUILD_CANONICAL_DOMAIN

                if (!rootUnrepresented(source.projection, "/processes") && represented(source.processesState.information)) {
                    ProcessCollectionState processes;
                    processes.truncation = publicTruncation(source.processesState.truncation);
                    processes.extensions = domainCollectionExtensions(source.processesState);
                    for (const canonical::ProcessState& process : source.processes) {
                        ProcessState decoded;
                        decoded.processHandle = ProcessHandle{process.handle.value()};
                        decoded.lifecycle = process.lifecycle.value_or(process.status.value_or("unknown"));
                        decoded.stdoutBytes = publicSize(process.stdoutBytes);
                        decoded.stderrBytes = publicSize(process.stderrBytes);
                        decoded.stdoutTruncated = process.stdoutTruncated;
                        decoded.stderrTruncated = process.stderrTruncated;
                        decoded.droppedOutputBytes = process.droppedOutputBytes;
                        decoded.exitCode = process.exitCode;
                        decoded.stamp = publicStamp(process.stamp);
                        decoded.connectionInvalidated = process.connectionInvalidated;
                        frontend::Json canonicalExtensions = process.extensions.json();
                        decoded.standardOutput = stringMember(canonicalExtensions, "stdout");
                        decoded.standardError = stringMember(canonicalExtensions, "stderr");
                        decoded.stateUnavailable = optionalBool(canonicalExtensions, "stateUnavailable").value_or(false) ||
                                                   source.processesState.information == canonical::InformationState::Unavailable;
                        if (process.publicExtensionsKnown) {
                            decoded.extensions = process.publicExtensions.json();
                        } else {
                            canonicalExtensions.erase("stdout");
                            canonicalExtensions.erase("stderr");
                            canonicalExtensions.erase("stateUnavailable");
                            decoded.extensions = std::move(canonicalExtensions);
                            if (!process.safeDetails.empty()) {
                                decoded.extensions["details"] = process.safeDetails.json();
                            }
                        }
                        processes.entries.push_back(std::move(decoded));
                    }
                    result->processes.value = std::move(processes);
                    result->processes.truncated = result->processes.value->truncation.truncated;
                    result->processes.omittedFields = result->processes.value->truncation.omittedFields;
                }

                if (represented(source.filesystemWatches.state.information)) {
                    FilesystemWatchCollectionState watches;
                    watches.truncation = publicTruncation(source.filesystemWatches.state.truncation);
                    watches.extensions = domainCollectionExtensions(source.filesystemWatches.state);
                    for (const canonical::FilesystemWatchRecord& watch : source.filesystemWatches.entries) {
                        FilesystemWatchState decoded;
                        decoded.watchId = typed::FsWatchId{watch.watchId};
                        if (watch.root.has_value()) {
                            decoded.root = typed::AbsolutePath{*watch.root};
                        }
                        decoded.changedPathCount = publicSize(watch.changedPathCount);
                        decoded.stamp = publicStamp(watch.stamp);
                        decoded.connectionInvalidated = watch.connectionInvalidated;
                        frontend::Json canonicalExtensions = watch.extensions.json();
                        decoded.stateUnavailable = optionalBool(canonicalExtensions, "stateUnavailable").value_or(false) ||
                                                   source.filesystemWatches.state.information == canonical::InformationState::Unavailable;
                        if (watch.publicExtensionsKnown) {
                            decoded.extensions = watch.publicExtensions.json();
                        } else {
                            canonicalExtensions.erase("stateUnavailable");
                            decoded.extensions = std::move(canonicalExtensions);
                            if (!watch.safeDetails.empty()) {
                                decoded.extensions["details"] = watch.safeDetails.json();
                            }
                        }
                        watches.entries.push_back(std::move(decoded));
                    }
                    result->filesystemWatches.value = std::move(watches);
                    result->filesystemWatches.truncated = result->filesystemWatches.value->truncation.truncated;
                    result->filesystemWatches.omittedFields = result->filesystemWatches.value->truncation.omittedFields;
                }

                if (represented(source.fuzzySearches.state.information)) {
                    FuzzySearchCollectionState searches;
                    searches.truncation = publicTruncation(source.fuzzySearches.state.truncation);
                    searches.extensions = domainCollectionExtensions(source.fuzzySearches.state);
                    for (const canonical::FuzzySearchRecord& search : source.fuzzySearches.entries) {
                        FuzzySearchState decoded;
                        decoded.sessionId = FuzzySearchSessionId{search.sessionId};
                        decoded.resultCount = publicSize(search.resultCount);
                        decoded.complete = search.complete;
                        decoded.stamp = publicStamp(search.stamp);
                        decoded.connectionInvalidated = search.connectionInvalidated;
                        frontend::Json canonicalExtensions = search.extensions.json();
                        decoded.stateUnavailable = optionalBool(canonicalExtensions, "stateUnavailable").value_or(false) ||
                                                   source.fuzzySearches.state.information == canonical::InformationState::Unavailable;
                        if (search.publicExtensionsKnown) {
                            decoded.extensions = search.publicExtensions.json();
                        } else {
                            canonicalExtensions.erase("stateUnavailable");
                            decoded.extensions = std::move(canonicalExtensions);
                            if (!search.safeDetails.empty()) {
                                decoded.extensions["details"] = search.safeDetails.json();
                            }
                        }
                        searches.entries.push_back(std::move(decoded));
                    }
                    result->fuzzySearches.value = std::move(searches);
                    result->fuzzySearches.truncated = result->fuzzySearches.value->truncation.truncated;
                    result->fuzzySearches.omittedFields = result->fuzzySearches.value->truncation.omittedFields;
                }

                if (represented(source.notices.state.information)) {
                    NoticeCollectionState notices;
                    notices.truncation = publicTruncation(source.notices.state.truncation);
                    notices.extensions = domainCollectionExtensions(source.notices.state);
                    for (const canonical::NoticeRecord& notice : source.notices.entries) {
                        NoticeState decoded;
                        if (notice.occurrence != 0) {
                            decoded.occurrence = notice.occurrence;
                        }
                        decoded.category = notice.category;
                        decoded.summary = notice.summary;
                        decoded.details = notice.details;
                        if (notice.threadId.has_value()) {
                            decoded.threadId = typed::ThreadId{notice.threadId->value()};
                        }
                        decoded.stamp = publicStamp(notice.stamp);
                        decoded.extensions = notice.extensions.json();
                        decoded.stateUnavailable = optionalBool(decoded.extensions, "stateUnavailable").value_or(false) ||
                                                   source.notices.state.information == canonical::InformationState::Unavailable;
                        decoded.extensions.erase("stateUnavailable");
                        if (!notice.safeDetails.empty()) {
                            decoded.extensions["safeDetails"] = notice.safeDetails.json();
                        }
                        notices.entries.push_back(std::move(decoded));
                    }
                    result->notices.value = std::move(notices);
                    result->notices.truncated = result->notices.value->truncation.truncated;
                    result->notices.omittedFields = result->notices.value->truncation.omittedFields;
                }

                if (represented(source.activities.state.information)) {
                    ActivityCollectionState activities;
                    activities.truncation = publicTruncation(source.activities.state.truncation);
                    activities.extensions = domainCollectionExtensions(source.activities.state);
                    for (const canonical::ActivityRecord& activity : source.activities.entries) {
                        ActivityState decoded;
                        decoded.key = ActivityKey{activity.key};
                        if (!activity.subjectId.empty()) {
                            decoded.subjectId = activity.subjectId;
                        }
                        decoded.kind = activity.kind;
                        decoded.lifecycle = activity.lifecycle;
                        decoded.summary = activity.summary;
                        decoded.details = activity.details;
                        if (activity.threadId.has_value()) {
                            decoded.threadId = typed::ThreadId{activity.threadId->value()};
                        }
                        if (activity.turnId.has_value()) {
                            decoded.turnId = typed::TurnId{activity.turnId->value()};
                        }
                        decoded.active = activity.active;
                        decoded.stamp = publicStamp(activity.stamp);
                        decoded.extensions = activity.extensions.json();
                        decoded.stateUnavailable = optionalBool(decoded.extensions, "stateUnavailable").value_or(false) ||
                                                   source.activities.state.information == canonical::InformationState::Unavailable;
                        decoded.extensions.erase("stateUnavailable");
                        if (!activity.safeDetails.empty()) {
                            decoded.extensions["safeDetails"] = activity.safeDetails.json();
                        }
                        activities.entries.push_back(std::move(decoded));
                    }
                    result->activities.value = std::move(activities);
                    result->activities.truncated = result->activities.value->truncation.truncated;
                    result->activities.omittedFields = result->activities.value->truncation.omittedFields;
                }

                if (source.capacityPresent) {
                    CapacityState capacity;
#define AISUITE_COPY_CAPACITY(member) capacity.member = source.capacity.member
                    AISUITE_COPY_CAPACITY(sessions);
                    AISUITE_COPY_CAPACITY(observers);
                    AISUITE_COPY_CAPACITY(activeOperations);
                    AISUITE_COPY_CAPACITY(pendingRequests);
                    AISUITE_COPY_CAPACITY(retainedThreads);
                    AISUITE_COPY_CAPACITY(retainedTurns);
                    AISUITE_COPY_CAPACITY(retainedItems);
                    AISUITE_COPY_CAPACITY(accumulatedContentBytes);
                    AISUITE_COPY_CAPACITY(retainedNotices);
                    AISUITE_COPY_CAPACITY(retainedProcesses);
                    AISUITE_COPY_CAPACITY(accumulatedProcessOutputBytes);
                    AISUITE_COPY_CAPACITY(retainedFilesystemWatches);
                    AISUITE_COPY_CAPACITY(retainedFuzzySearchSessions);
                    AISUITE_COPY_CAPACITY(retainedActivityRecords);
                    AISUITE_COPY_CAPACITY(evictedNotices);
                    AISUITE_COPY_CAPACITY(evictedProcesses);
                    AISUITE_COPY_CAPACITY(droppedProcessOutputBytes);
                    AISUITE_COPY_CAPACITY(evictedFilesystemWatches);
                    AISUITE_COPY_CAPACITY(evictedFuzzySearchSessions);
                    AISUITE_COPY_CAPACITY(evictedActivityRecords);
#undef AISUITE_COPY_CAPACITY
                    capacity.extensions = source.capacity.extensions.json();
                    result->capacity.value = std::move(capacity);
                }

                result->truncation.value = publicTruncation(source.truncation);
                result->truncation.truncated = source.truncation.truncated;
                result->truncation.omittedFields = source.truncation.omittedPaths;

                if (represented(source.diagnostics.state.information)) {
                    DiagnosticCollectionState diagnostics;
                    diagnostics.received = source.diagnostics.received;
                    diagnostics.entries.reserve(source.diagnostics.entries.size());
                    for (const canonical::DiagnosticRecord& diagnostic : source.diagnostics.entries) {
                        DiagnosticState decoded;
                        decoded.received = diagnostic.received;
                        decoded.detailsOmitted = diagnostic.detailsOmitted;
                        decoded.message = diagnostic.message;
                        if (!diagnostic.safeDetails.empty()) {
                            decoded.opaqueDetails = diagnostic.safeDetails.json();
                        }
                        decoded.extensions = diagnostic.extensions.json();
                        diagnostics.entries.push_back(std::move(decoded));
                    }
                    result->diagnostics.value = std::move(diagnostics);
                    result->diagnostics.truncated = source.diagnostics.state.information == canonical::InformationState::Truncated ||
                                                    source.diagnostics.state.truncation.truncated;
                    result->diagnostics.omittedFields = source.diagnostics.state.truncation.omittedPaths;
                    trimDiagnostics(result->diagnostics, maximumRetainedDiagnostics);
                }

                for (const std::string& path : source.projection.omittedPaths) {
                    applySnapshotProjectionOmission(*result, path);
                }

                frontend::Json stateExtensions = source.stateExtensions.json();
                if (publication.representation == canonical_client::RepresentationMode::LegacyV1) {
                    if (const auto codex = stateExtensions.find("codexExtensions"); codex != stateExtensions.end()) {
                        if (!codex->is_array() || !codex->empty()) {
                            result->compatibilityExtensions["codexExtensions"] = *codex;
                        }
                        stateExtensions.erase(codex);
                    }
                    for (auto member = stateExtensions.begin(); member != stateExtensions.end(); ++member) {
                        result->compatibilityExtensions[member.key()] = member.value();
                    }
                    for (const canonical::LegacyRootExtension& extension : source.legacyRootExtensions) {
                        result->compatibilityExtensions[extension.name] = extension.value.json();
                    }
                } else if (!stateExtensions.empty()) {
                    result->compatibilityExtensions["state"] = std::move(stateExtensions);
                }
                for (auto member = source.extensions.json().begin(); member != source.extensions.json().end(); ++member) {
                    if (member.key() != "scopeProjection") {
                        result->compatibilityExtensions[member.key()] = member.value();
                    }
                }
            }

            if (!rebuildStateSizeLedger(*result) || !stateFits(*result, maximumBytes, error)) {
                if (failure != nullptr) {
                    *failure = CanonicalStateBuildFailure::Capacity;
                }
                if (error.empty()) {
                    error = "canonical public State accounting failed";
                }
                return std::nullopt;
            }
            error.clear();
            return std::shared_ptr<const StateStorage>{std::move(result)};
        } catch (const std::exception& exception) {
            error = std::string{"canonical public State construction failed: "} + exception.what();
            return std::nullopt;
        } catch (...) {
            error = "canonical public State construction failed";
            return std::nullopt;
        }
    }

    namespace detail {
        std::optional<TurnResultState> decodeOperationTurnResultState(const frontend::Json& value, std::string& error) noexcept {
            try {
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
            } catch (...) {
                error = "turn result could not be decoded";
                return std::nullopt;
            }
        }

        std::optional<ThreadResultState> decodeOperationThreadResultState(const frontend::Json& value, std::string& error) noexcept {
            try {
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
            } catch (...) {
                error = "thread result could not be decoded";
                return std::nullopt;
            }
        }

    } // namespace detail

} // namespace ai::openai::codex::frontend::client
