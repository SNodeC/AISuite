/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/State.h"

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/client/detail/StateReducer.h"
#include "ai/openai/codex/frontend/detail/EventRepresentation.h"
#include "ai/openai/codex/frontend/detail/FrontendProjection.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <type_traits>
#include <utility>

namespace ai::openai::codex::frontend::client {

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

        void mergePaths(std::vector<std::string>& destination, const std::vector<std::string>& source) {
            destination.insert(destination.end(), source.begin(), source.end());
            std::sort(destination.begin(), destination.end());
            destination.erase(std::unique(destination.begin(), destination.end()), destination.end());
        }

        bool decodeProjectionPaths(const frontend::Json& projection,
                                   std::string_view member,
                                   std::vector<std::string>& result,
                                   std::string& error) {
            const auto values = projection.find(std::string(member));
            if (values == projection.end() || !values->is_array()) {
                error = "scopeProjection." + std::string(member) + " must be an array";
                return false;
            }
            for (const frontend::Json& value : *values) {
                if (!value.is_string()) {
                    error = "scopeProjection." + std::string(member) + " must contain strings";
                    return false;
                }
                result.push_back(value.get<std::string>());
            }
            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return true;
        }

        bool decodeProjectionMetadata(const frontend::Json& extensions,
                                      std::optional<ProjectionMetadataState>& result,
                                      std::string& error) {
            if (!extensions.is_object()) {
                error = "frontend message extensions must be an object";
                return false;
            }
            const auto projection = extensions.find("scopeProjection");
            if (projection == extensions.end())
                return true;
            if (!projection->is_object()) {
                error = "scopeProjection metadata must be an object";
                return false;
            }
            ProjectionMetadataState decoded;
            if (!decodeProjectionPaths(*projection, "omittedFields", decoded.omittedFields, error) ||
                !decodeProjectionPaths(*projection, "redactedFields", decoded.redactedFields, error))
                return false;
            result = std::move(decoded);
            return true;
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
            projection.omittedFields.erase(
                std::unique(projection.omittedFields.begin(), projection.omittedFields.end()), projection.omittedFields.end());
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

        void applyEventProjectionOmission(detail::StateStorage& state,
                                          frontend::ExpandedEventType type,
                                          const std::string& path) {
            // `/event/<family>` reports a completely hidden sibling family,
            // not an omitted field of the visible event's own domain.
            if (projectionPathRoot(path) == "event")
                return;
            using enum frontend::ExpandedEventType;
            switch (type) {
                case ProviderUpdated:
                    addProjectedOmission(state.provider, path);
                    break;
                case ControllerUpdated:
                    addProjectedOmission(state.controller, path);
                    break;
                case SessionsUpdated:
                    addProjectedOmission(state.sessions, path);
                    break;
                case AccountUpdated:
                    addProjectedOmission(state.accounts, path);
                    break;
                case ModelsUpdated:
                    addProjectedOmission(state.models, path);
                    break;
                case ConfigurationUpdated:
                    addProjectedOmission(state.configuration, path);
                    break;
                case ProcessUpdated:
                    addProjectedOmission(state.processes, path);
                    break;
                case FilesystemWatchUpdated:
                    addProjectedOmission(state.filesystemWatches, path);
                    break;
                case FuzzySearchUpdated:
                    addProjectedOmission(state.fuzzySearches, path);
                    break;
                case ReviewsUpdated:
                    addProjectedOmission(state.reviews, path);
                    addProjectedOmission(state.permissionProfiles, path);
                    break;
                case IntegrationsUpdated:
                    addProjectedOmission(state.apps, path);
                    addProjectedOmission(state.externalAgents, path);
                    addProjectedOmission(state.hooks, path);
                    addProjectedOmission(state.marketplace, path);
                    break;
                case PluginsUpdated:
                case SkillsUpdated:
                    addProjectedOmission(state.plugins, path);
                    addProjectedOmission(state.skills, path);
                    break;
                case McpUpdated:
                    addProjectedOmission(state.mcp, path);
                    break;
                case PlatformUpdated:
                    addProjectedOmission(state.platform, path);
                    addProjectedOmission(state.windowsSandbox, path);
                    break;
                case NoticeAdded:
                    addProjectedOmission(state.notices, path);
                    break;
                case ActivityUpdated:
                    addProjectedOmission(state.activities, path);
                    break;
                case CapacityUpdated:
                    addProjectedOmission(state.capacity, path);
                    break;
                case DiagnosticsUpdated:
                    addProjectedOmission(state.diagnostics, path);
                    break;
                case ThreadUpserted:
                case ThreadRemoved:
                case TurnUpserted:
                case ItemUpserted:
                case ItemContentUpdated:
                case PendingRequestsUpdated:
                    break;
            }
        }

        bool applySnapshotProjectionMetadata(detail::StateStorage& state,
                                             const frontend::Json& extensions,
                                             std::string& error) {
            std::optional<ProjectionMetadataState> metadata;
            if (!decodeProjectionMetadata(extensions, metadata, error))
                return false;
            if (metadata) {
                state.projectionMetadata = *metadata;
                for (const std::string& path : metadata->omittedFields)
                    applySnapshotProjectionOmission(state, path);
            }
            for (auto member = extensions.begin(); member != extensions.end(); ++member) {
                if (member.key() != "scopeProjection")
                    state.compatibilityExtensions[member.key()] = member.value();
            }
            return true;
        }

        bool applyEventProjectionMetadata(detail::StateStorage& state,
                                          const frontend::FrontendEvent& event,
                                          std::optional<frontend::ExpandedEventType> expandedType,
                                          std::string& error) {
            std::optional<ProjectionMetadataState> metadata;
            if (!decodeProjectionMetadata(event.extensions, metadata, error))
                return false;
            if (!metadata)
                return true;
            mergePaths(state.projectionMetadata.omittedFields, metadata->omittedFields);
            mergePaths(state.projectionMetadata.redactedFields, metadata->redactedFields);
            if (expandedType) {
                for (const std::string& path : metadata->omittedFields)
                    applyEventProjectionOmission(state, *expandedType, path);
            }
            return true;
        }

        bool requireArrayMember(const frontend::Json& object,
                                std::string_view key,
                                const frontend::Json*& value,
                                std::string& error) {
            if (!object.is_object()) {
                error = "event data must be an object";
                return false;
            }
            const auto found = object.find(std::string(key));
            if (found == object.end() || !found->is_array()) {
                error = "event wrapper field '" + std::string(key) + "' must be an array";
                return false;
            }
            value = &*found;
            return true;
        }

        bool requireObjectMember(const frontend::Json& object,
                                 std::string_view key,
                                 const frontend::Json*& value,
                                 std::string& error) {
            if (!object.is_object()) {
                error = "event data must be an object";
                return false;
            }
            const auto found = object.find(std::string(key));
            if (found == object.end() || !found->is_object()) {
                error = "event wrapper field '" + std::string(key) + "' must be an object";
                return false;
            }
            value = &*found;
            return true;
        }

        bool canonicalDecimalId(std::string_view value) noexcept {
            if (value.empty() || value.size() > 20 || value.front() == '0')
                return false;
            std::uint64_t decoded = 0;
            for (const char character : value) {
                if (character < '0' || character > '9')
                    return false;
                const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
                if (decoded > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
                    return false;
                decoded = decoded * 10 + digit;
            }
            return decoded != 0;
        }

        bool requireStringMember(const frontend::Json& object,
                                 std::string_view key,
                                 bool nonempty,
                                 std::string& error) {
            const auto value = stringMember(object, key);
            if (value && (!nonempty || !value->empty()))
                return true;
            error = "field '" + std::string(key) + "' must be " + (nonempty ? "a nonempty string" : "a string");
            return false;
        }

        bool requireBoolMember(const frontend::Json& object, std::string_view key, std::string& error) {
            if (optionalBool(object, key))
                return true;
            error = "field '" + std::string(key) + "' must be a boolean";
            return false;
        }

        bool requireUnsignedMember(const frontend::Json& object, std::string_view key, std::string& error) {
            if (optionalUnsigned(object, key))
                return true;
            error = "field '" + std::string(key) + "' must be an unsigned integer";
            return false;
        }

        bool requireDecimalIdMember(const frontend::Json& object, std::string_view key, std::string& error) {
            const auto value = stringMember(object, key);
            if (value && canonicalDecimalId(*value))
                return true;
            error = "field '" + std::string(key) + "' must be a canonical non-zero uint64 decimal string";
            return false;
        }

        bool optionalMemberHasType(const frontend::Json& object,
                                   std::string_view key,
                                   frontend::Json::value_t expected,
                                   std::string& error) {
            const auto found = object.find(std::string(key));
            if (found == object.end() || found->type() == expected)
                return true;
            error = "field '" + std::string(key) + "' has the wrong JSON type";
            return false;
        }

        std::optional<ProviderLifecycle> providerLifecycle(std::string_view value) noexcept {
            if (value == "stopped")
                return ProviderLifecycle::Stopped;
            if (value == "starting")
                return ProviderLifecycle::Starting;
            if (value == "initializing")
                return ProviderLifecycle::Initializing;
            if (value == "ready")
                return ProviderLifecycle::Ready;
            if (value == "stopping")
                return ProviderLifecycle::Stopping;
            if (value == "failed")
                return ProviderLifecycle::Failed;
            if (value == "recovering")
                return ProviderLifecycle::Recovering;
            return std::nullopt;
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

        std::optional<ProviderRecoveryStatus> providerRecoveryStatus(std::string_view value) noexcept {
            if (value == "idle")
                return ProviderRecoveryStatus::Idle;
            if (value == "waiting")
                return ProviderRecoveryStatus::Waiting;
            if (value == "exhausted")
                return ProviderRecoveryStatus::Exhausted;
            return std::nullopt;
        }

        std::optional<ItemContentChannel> itemContentChannel(std::string_view value) noexcept {
            if (value == "agentText")
                return ItemContentChannel::AgentText;
            if (value == "reasoningText")
                return ItemContentChannel::ReasoningText;
            if (value == "reasoningSummary")
                return ItemContentChannel::ReasoningSummary;
            if (value == "commandOutput")
                return ItemContentChannel::CommandOutput;
            return std::nullopt;
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

        bool decodeTruncation(const frontend::Json& value,
                              TruncationMetadata& result,
                              std::string& error,
                              bool strict = true) {
            if (!value.is_object()) {
                if (!strict)
                    return false;
                error = "truncation metadata must be an object";
                return false;
            }
            const auto truncated = optionalBool(value, "truncated");
            if (!truncated && strict) {
                error = "truncation metadata requires boolean truncated";
                return false;
            }
            result = TruncationMetadata{};
            result.truncated = truncated.value_or(false);
            result.omittedEntries = optionalSize(value, "omittedEntries");
            result.droppedBytes = optionalUnsigned(value, "droppedBytes");
            if (const auto omitted = value.find("omittedFields"); omitted != value.end()) {
                if (!omitted->is_array()) {
                    error = "truncation omittedFields must be an array";
                    return false;
                }
                for (const frontend::Json& field : *omitted) {
                    if (!field.is_string()) {
                        error = "truncation omittedFields must contain strings";
                        return false;
                    }
                    result.omittedFields.push_back(field.get<std::string>());
                }
            }
            result.extensions =
                extensionsOf(value, {"truncated", "omittedFields", "omittedEntries", "droppedBytes"});
            return true;
        }

        bool decodeProviderError(const frontend::Json& value,
                                 ProviderErrorState& result,
                                 std::string& error,
                                 bool strict) {
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

        bool decodeProviderInitialization(const frontend::Json& value,
                                          ProviderInitializationState& result,
                                          std::string& error) {
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
            if (!lifecycle || !transcript || !itemCount || !receivedAudioBytes || !droppedAudioBytes ||
                !transcriptTruncated) {
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
                                             {"lifecycle", "transcript", "itemCount", "receivedAudioBytes",
                                              "droppedAudioBytes", "transcriptTruncated", "errorDetailsOmitted",
                                              "sessionId", "version", "lastSdpBytes"});
            return true;
        }

        bool decodeProvider(const frontend::Json& value, ProviderState& result, std::string& error, bool strict = true) {
            if (!requireObject(value, "provider", error))
                return false;
            const auto lifecycleName = stringMember(value, "lifecycle");
            const auto lifecycle = lifecycleName ? providerLifecycle(*lifecycleName) : std::nullopt;
            const auto generation = optionalUnsigned(value, "generation");
            const auto desiredRunning = optionalBool(value, "desiredRunning");
            const auto recovery = value.find("recovery");
            if ((!lifecycle || !generation || !desiredRunning || recovery == value.end() || !recovery->is_object()) && strict) {
                error = "provider state lacks required stable fields";
                return false;
            }
            result.lifecycle = lifecycle.value_or(ProviderLifecycle::Stopped);
            result.generation = generation.value_or(0);
            result.desiredRunning = desiredRunning.value_or(false);
            result.ready = result.lifecycle == ProviderLifecycle::Ready;
            if (recovery != value.end() && recovery->is_object()) {
                const auto statusName = stringMember(*recovery, "status");
                const auto status = statusName ? providerRecoveryStatus(*statusName) : std::nullopt;
                const auto attempts = optionalSize(*recovery, "attempts");
                if ((!status || !attempts) && strict) {
                    error = "provider recovery state lacks required stable fields";
                    return false;
                }
                result.recovery.status = status.value_or(ProviderRecoveryStatus::Idle);
                result.recovery.attempts = attempts.value_or(0);
                result.recovery.delayMs = optionalUnsigned(*recovery, "delayMs");
                result.recovery.extensions = extensionsOf(*recovery, {"status", "attempts", "delayMs"});
            }
            if (const auto lastError = value.find("lastError"); lastError != value.end()) {
                ProviderErrorState decoded;
                if (!decodeProviderError(*lastError, decoded, error, strict))
                    return false;
                result.lastError = std::move(decoded);
            }
            if (const auto initialization = value.find("initialization"); initialization != value.end()) {
                ProviderInitializationState decoded;
                if (!decodeProviderInitialization(*initialization, decoded, error))
                    return false;
                result.initialization = std::move(decoded);
            }
            result.extensions = extensionsOf(
                value, {"lifecycle", "generation", "desiredRunning", "recovery", "lastError", "initialization"});
            return true;
        }

        bool decodeController(const frontend::Json& value,
                              const std::optional<SessionInfo>& session,
                              ControllerState& result,
                              std::string& error) {
            if (!requireObject(value, "controller", error))
                return false;
            if (const auto id = stringMember(value, "controllerSessionId"); id && !id->empty())
                result.sessionId = FrontendSessionId{*id};
            result.present = optionalBool(value, "present").value_or(result.sessionId.has_value());
            result.ownedByThisClient = session && result.sessionId && result.sessionId->value == session->sessionId;
            result.extensions = extensionsOf(value, {"controllerSessionId", "present"});
            return true;
        }

        bool decodeSession(const frontend::Json& value, SessionState& result, std::string& error, bool strict = true) {
            if (!requireObject(value, "session", error))
                return false;
            auto id = stringMember(value, "sessionId");
            if (!id)
                id = stringMember(value, "id");
            const auto roleName = stringMember(value, "role");
            const auto role = roleName ? frontend::sessionRoleFromString(*roleName) : std::nullopt;
            if ((!id || id->empty() || !role) && strict) {
                error = "session state lacks a valid sessionId or role";
                return false;
            }
            result.sessionId = FrontendSessionId{id.value_or("unavailable")};
            result.role = role.value_or(frontend::SessionRole::Observer);
            result.extensions = extensionsOf(value, {"sessionId", "id", "role"});
            return true;
        }

        bool decodeThreadList(const frontend::Json& value, ThreadListState& result, std::string& error) {
            if (!requireObject(value, "thread list", error))
                return false;
            const auto hasLoadedPage = optionalBool(value, "hasLoadedPage");
            const auto complete = optionalBool(value, "complete");
            const auto pagesLoaded = optionalSize(value, "pagesLoaded");
            if (!hasLoadedPage || !complete || !pagesLoaded) {
                error = "thread list state lacks required stable fields";
                return false;
            }
            result.hasLoadedPage = *hasLoadedPage;
            result.complete = *complete;
            result.pagesLoaded = *pagesLoaded;
            result.nextCursor = stringMember(value, "nextCursor");
            result.backwardsCursor = stringMember(value, "backwardsCursor");
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                SourceStamp decoded;
                if (!decodeStamp(*stamp, decoded, error))
                    return false;
                result.stamp = decoded;
            }
            result.extensions = extensionsOf(
                value, {"hasLoadedPage", "complete", "pagesLoaded", "nextCursor", "backwardsCursor", "stamp"});
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
                                    {"id", "title", "name", "preview", "cwd", "model", "modelProvider", "status",
                                     "fullyLoaded", "realtime", "stamp", "createdAt", "updatedAt", "createdAtMs",
                                     "updatedAtMs", "turns", "extensions"},
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
                                    {"id", "threadId", "status", "active", "terminal", "connectionInvalidated",
                                     "stamp", "items", "failure", "tokenUsage", "extensions"},
                                    result.extensions,
                                    "turn",
                                    error);
        }

        ItemState decodeItem(const frontend::ExpandedThreadItem& value) {
            ItemState result;
            result.id = typed::ItemId{value.id};
            if (value.threadId)
                result.threadId = typed::ThreadId{*value.threadId};
            if (value.turnId)
                result.turnId = typed::TurnId{*value.turnId};
            result.kind = value.type;
            result.status = value.status;
            result.summary = value.summary;
            result.location = value.location;
            result.agentText = value.agentText;
            result.reasoningText = value.reasoningText;
            result.reasoningSummary = value.reasoningSummary;
            result.commandOutput = value.commandOutput;
            result.droppedContentBytes = value.droppedContentBytes;
            result.contentTruncated = value.contentTruncated.value_or(value.truncated);
            result.startedAtMs = value.startedAtMs;
            result.completedAtMs = value.completedAtMs;
            result.data = value.data;
            result.truncated = value.truncated;
            result.omittedFields = value.omittedFields;
            result.connectionInvalidated = value.connectionInvalidated;
            if (value.generation && value.freshness)
                result.stamp = SourceStamp{*value.generation, *value.freshness};
            result.extensions = value.extensions;
            return result;
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
                    return data != value.end() && data->is_object() && data->contains("server")
                               ? frontend::ThreadItemKind::McpToolCall
                               : frontend::ThreadItemKind::DynamicToolCall;
                }
                const auto data = value.find("data");
                if (data != value.end() && data->is_object()) {
                    const auto codexType = stringMember(*data, "codexType");
                    if (codexType)
                        return frontend::threadItemKindFromString(*codexType);
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
            const frontend::Json unknown = extensionsOf(
                value,
                {"id", "type", "kind", "threadId", "turnId", "status", "summary", "location", "agentText",
                 "reasoningText", "reasoningSummary", "commandOutput", "droppedContentBytes", "contentTruncated",
                 "startedAtMs", "completedAtMs", "data", "truncated", "omittedFields", "connectionInvalidated",
                 "stamp", "generation", "freshness", "extensions"});
            for (auto member = unknown.begin(); member != unknown.end(); ++member)
                result.extensions[member.key()] = member.value();
            return true;
        }

        bool decodeExpandedItem(const frontend::Json& value, ItemState& result, std::string& error) {
            frontend::ExpandedFrontendEvent synthetic{frontend::SequenceNumber{1},
                                                       frontend::ExpandedEventType::ItemUpserted,
                                                       frontend::Json::object(),
                                                       frontend::Json::object()};
            synthetic.data["item"] = value;
            const auto encoded = frontend::Codec::encodeExpandedEvent(synthetic);
            if (!encoded) {
                error = "invalid expanded item: " + encoded.error().message;
                return false;
            }
            const auto decoded = frontend::Codec::decodeExpandedEvent(encoded.value());
            if (!decoded) {
                error = "invalid expanded item: " + decoded.error().message;
                return false;
            }
            const auto itemObject = decoded.value().data.find("item");
            if (itemObject == decoded.value().data.end()) {
                error = "expanded item wrapper is missing item";
                return false;
            }
            // Codec's snapshot path exposes typed items; decode this validated
            // object directly without retaining the wire object as state.
            const auto kindName = stringMember(*itemObject, "type");
            const auto kind = kindName ? frontend::threadItemKindFromString(*kindName) : std::nullopt;
            const auto id = stringMember(*itemObject, "id");
            if (!kind || !id || id->empty()) {
                error = "expanded item lacks a valid id or type";
                return false;
            }
            result.id = typed::ItemId{*id};
            result.kind = *kind;
            if (const auto threadId = stringMember(*itemObject, "threadId"))
                result.threadId = typed::ThreadId{*threadId};
            if (const auto turnId = stringMember(*itemObject, "turnId"))
                result.turnId = typed::TurnId{*turnId};
            result.status = stringMember(*itemObject, "status");
            result.summary = stringMember(*itemObject, "summary");
            if (const auto location = itemObject->find("location"); location != itemObject->end())
                result.location = *location;
            result.agentText = stringMember(*itemObject, "agentText");
            result.reasoningText = stringMember(*itemObject, "reasoningText");
            result.reasoningSummary = stringMember(*itemObject, "reasoningSummary");
            result.commandOutput = stringMember(*itemObject, "commandOutput");
            result.droppedContentBytes = optionalUnsigned(*itemObject, "droppedContentBytes");
            result.contentTruncated = optionalBool(*itemObject, "contentTruncated").value_or(false);
            result.startedAtMs = optionalInteger(*itemObject, "startedAtMs");
            result.completedAtMs = optionalInteger(*itemObject, "completedAtMs");
            if (const auto data = itemObject->find("data"); data != itemObject->end())
                result.data = *data;
            result.truncated = optionalBool(*itemObject, "truncated").value_or(false);
            if (const auto omitted = itemObject->find("omittedFields"); omitted != itemObject->end()) {
                for (const frontend::Json& member : *omitted)
                    result.omittedFields.push_back(member.get<std::string>());
            }
            result.connectionInvalidated = optionalBool(*itemObject, "connectionInvalidated").value_or(false);
            const auto generation = optionalUnsigned(*itemObject, "generation");
            const auto freshnessName = stringMember(*itemObject, "freshness");
            const auto freshness = freshnessName ? frontend::stateFreshnessFromString(*freshnessName) : std::nullopt;
            if (generation && freshness)
                result.stamp = SourceStamp{*generation, *freshness};
            result.extensions = extensionsOf(*itemObject,
                                             {"id", "type", "threadId", "turnId", "status", "summary", "location", "agentText",
                                              "reasoningText", "reasoningSummary", "commandOutput", "droppedContentBytes",
                                              "contentTruncated", "startedAtMs", "completedAtMs", "data", "truncated",
                                              "omittedFields", "connectionInvalidated", "generation", "freshness"});
            return true;
        }

        PendingRequestState decodePendingRequest(const frontend::ExpandedPendingRequest& value) {
            PendingRequestState result;
            result.id = PendingRequestId{value.pendingRequestId};
            result.kind = value.kind;
            if (value.threadId)
                result.threadId = typed::ThreadId{*value.threadId};
            if (value.turnId)
                result.turnId = typed::TurnId{*value.turnId};
            if (value.itemId)
                result.itemId = typed::ItemId{*value.itemId};
            result.summary = value.summary;
            result.opaqueDetails = value.details;
            if (value.questions) {
                result.questions.emplace();
                for (const frontend::ExpandedPendingRequestQuestion& question : *value.questions) {
                    PendingRequestQuestionState decoded;
                    decoded.id = question.id;
                    decoded.header = question.header;
                    decoded.prompt = question.prompt;
                    decoded.allowsFreeText = question.allowsFreeText;
                    decoded.isSecret = question.isSecret;
                    decoded.extensions = question.extensions;
                    for (const frontend::ExpandedPendingRequestOption& option : question.options) {
                        decoded.options.push_back({option.label, option.description, option.extensions});
                    }
                    result.questions->push_back(std::move(decoded));
                }
            }
            result.autoResolutionMs = value.autoResolutionMs;
            result.truncated = value.truncated;
            if (const auto omitted = value.extensions.find("omittedFields");
                omitted != value.extensions.end() && omitted->is_array()) {
                for (const frontend::Json& field : *omitted)
                    result.omittedFields.push_back(field.get<std::string>());
            }
            result.extensions = extensionsOf(value.extensions, {"omittedFields"});
            return result;
        }

        std::optional<frontend::PendingRequestKind> legacyPendingKind(std::string_view value) noexcept {
            if (const auto direct = frontend::pendingRequestKindFromString(value))
                return direct;
            if (value == "command_approval")
                return frontend::PendingRequestKind::CommandExecutionApproval;
            if (value == "file_change_approval")
                return frontend::PendingRequestKind::FileChangeApproval;
            if (value == "apply_patch_approval")
                return frontend::PendingRequestKind::ApplyPatchApproval;
            if (value == "exec_command_approval")
                return frontend::PendingRequestKind::ExecCommandApproval;
            if (value == "permissions_approval")
                return frontend::PendingRequestKind::PermissionsApproval;
            if (value == "dynamic_tool_call")
                return frontend::PendingRequestKind::DynamicToolCall;
            if (value == "mcp_elicitation")
                return frontend::PendingRequestKind::McpElicitation;
            return std::nullopt;
        }

        std::optional<frontend::Json> sanitizeLegacyPendingDetails(const frontend::Json& value, std::string& error) {
            frontend::detail::CanonicalEventRecord record;
            record.sequence = frontend::SequenceNumber{1};
            record.legacyType = "request.pending";
            record.legacyData.value = value;
            record = frontend::detail::canonicalizeEvent(std::move(record));
            if (record.sanitization.failed || !record.legacyData.value.is_object()) {
                error = "legacy pending request details could not be bounded and sanitized";
                return std::nullopt;
            }
            return std::optional<frontend::Json>{std::move(record.legacyData.value)};
        }

        bool decodePendingRequestJson(const frontend::Json& value,
                                      PendingRequestState& result,
                                      std::string& error,
                                      bool strictExpanded) {
            if (!requireObject(value, "pending request", error))
                return false;
            auto id = stringMember(value, "pendingRequestId");
            if (!id)
                id = stringMember(value, "id");
            auto kindName = stringMember(value, "kind");
            if (!kindName)
                kindName = stringMember(value, "type");
            const auto kind = kindName ? legacyPendingKind(*kindName) : std::nullopt;
            if (!id || id->empty() || !kind) {
                error = "pending request lacks a valid id or kind";
                return false;
            }
            result.id = PendingRequestId{*id};
            result.kind = *kind;
            if (const auto threadId = stringMember(value, "threadId"))
                result.threadId = typed::ThreadId{*threadId};
            if (const auto turnId = stringMember(value, "turnId"))
                result.turnId = typed::TurnId{*turnId};
            if (const auto itemId = stringMember(value, "itemId"))
                result.itemId = typed::ItemId{*itemId};
            result.summary = stringMember(value, "summary");
            if (const auto details = value.find("details"); details != value.end()) {
                if (strictExpanded) {
                    result.opaqueDetails = *details;
                } else {
                    auto sanitized = sanitizeLegacyPendingDetails(*details, error);
                    if (!sanitized)
                        return false;
                    result.opaqueDetails = std::move(*sanitized);
                }
            }
            result.autoResolutionMs = optionalUnsigned(value, "autoResolutionMs");
            result.truncated = optionalBool(value, "truncated").value_or(false);
            result.connectionInvalidated = optionalBool(value, "connectionInvalidated").value_or(false);
            if (const auto omitted = value.find("omittedFields"); omitted != value.end() && omitted->is_array()) {
                for (const frontend::Json& field : *omitted) {
                    if (!field.is_string()) {
                        error = "pending request omittedFields must contain strings";
                        return false;
                    }
                    result.omittedFields.push_back(field.get<std::string>());
                }
            }
            if (const auto questions = value.find("questions"); questions != value.end()) {
                if (!questions->is_array()) {
                    error = "pending request questions must be an array";
                    return false;
                }
                result.questions.emplace();
                for (const frontend::Json& question : *questions) {
                    PendingRequestQuestionState decoded;
                    const auto questionId = stringMember(question, "id");
                    const auto header = stringMember(question, "header");
                    const auto prompt = stringMember(question, "prompt");
                    const auto allows = optionalBool(question, "allowsFreeText");
                    auto secret = optionalBool(question, "isSecret");
                    if (!secret)
                        secret = optionalBool(question, "secret");
                    const auto options = question.find("options");
                    if (!questionId || !header || !prompt || !allows || !secret || options == question.end() || !options->is_array()) {
                        error = "pending request question lacks required presentation fields";
                        return false;
                    }
                    decoded.id = *questionId;
                    decoded.header = *header;
                    decoded.prompt = *prompt;
                    decoded.allowsFreeText = *allows;
                    decoded.isSecret = *secret;
                    decoded.extensions = extensionsOf(question, {"id", "header", "prompt", "allowsFreeText", "isSecret", "secret", "options"});
                    for (const frontend::Json& option : *options) {
                        const auto label = stringMember(option, "label");
                        const auto description = stringMember(option, "description");
                        if (!label || !description) {
                            error = "pending request option lacks label or description";
                            return false;
                        }
                        decoded.options.push_back({*label, *description, extensionsOf(option, {"label", "description"})});
                    }
                    result.questions->push_back(std::move(decoded));
                }
            }
            if (strictExpanded && result.kind == frontend::PendingRequestKind::UserInput && !result.questions) {
                error = "user-input pending request lacks questions";
                return false;
            }
            result.extensions = extensionsOf(value,
                                             {"pendingRequestId", "id", "kind", "type", "threadId", "turnId", "itemId",
                                              "summary", "details", "questions", "autoResolutionMs", "truncated", "omittedFields",
                                              "connectionInvalidated"});
            return true;
        }

        bool validateOptionalString(const frontend::Json& value, std::string_view key, std::string& error) {
            return optionalMemberHasType(value, key, frontend::Json::value_t::string, error);
        }

        bool validateOptionalInteger(const frontend::Json& value, std::string_view key, std::string& error) {
            const auto found = value.find(std::string(key));
            if (found == value.end())
                return true;
            if (found->is_number_integer())
                return true;
            if (found->is_number_unsigned() && found->get<std::uint64_t>() <=
                                                   static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                return true;
            error = "field '" + std::string(key) + "' must be a signed 64-bit integer";
            return false;
        }

        bool validateLegacyUserMessageData(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy user-message data", error))
                return false;
            const auto clientId = value.find("clientId");
            if (clientId == value.end() || (!clientId->is_string() && !clientId->is_null())) {
                error = "legacy user-message data clientId must be a string or null";
                return false;
            }
            const auto content = value.find("content");
            if (content == value.end() || !content->is_array()) {
                error = "legacy user-message data content must be an array";
                return false;
            }
            return requireBoolMember(value, "contentTruncated", error) &&
                   requireUnsignedMember(value, "originalContentBytes", error) &&
                   requireUnsignedMember(value, "retainedContentBytes", error) &&
                   requireUnsignedMember(value, "originalContentItems", error) &&
                   requireUnsignedMember(value, "retainedContentItems", error);
        }

        bool validateLegacyItemSchema(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy item", error) || !requireStringMember(value, "id", true, error) ||
                !requireStringMember(value, "type", true, error) || !requireStringMember(value, "status", false, error) ||
                !requireStringMember(value, "agentText", false, error) ||
                !requireStringMember(value, "reasoningText", false, error) ||
                !requireStringMember(value, "reasoningSummary", false, error) ||
                !requireStringMember(value, "commandOutput", false, error) ||
                !requireUnsignedMember(value, "droppedContentBytes", error) ||
                !requireBoolMember(value, "contentTruncated", error))
                return false;

            const auto type = stringMember(value, "type");
            const auto status = stringMember(value, "status");
            if (!status || (*status != "unknown" && *status != "started" && *status != "completed" && *status != "failed")) {
                error = "legacy item status is not a defined stable value";
                return false;
            }
            const auto data = value.find("data");
            const auto extensions = value.find("extensions");
            if (data == value.end() || !data->is_object() || extensions == value.end() || !extensions->is_object()) {
                error = "legacy item data and extensions must be objects";
                return false;
            }
            if (*type == "user_message" && !validateLegacyUserMessageData(*data, error))
                return false;
            return validateOptionalInteger(value, "startedAtMs", error) &&
                   validateOptionalInteger(value, "completedAtMs", error);
        }

        bool validateLegacyTurnSchema(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy turn", error) || !requireStringMember(value, "id", true, error) ||
                !requireStringMember(value, "threadId", true, error) || !requireStringMember(value, "status", false, error) ||
                !requireBoolMember(value, "active", error) || !requireBoolMember(value, "terminal", error))
                return false;
            const auto items = value.find("items");
            const auto extensions = value.find("extensions");
            if (items == value.end() || !items->is_array() || extensions == value.end() || !extensions->is_object()) {
                error = "legacy turn items must be an array and extensions must be an object";
                return false;
            }
            for (const frontend::Json& item : *items) {
                if (!validateLegacyItemSchema(item, error))
                    return false;
            }
            return true;
        }

        bool validateLegacyThreadSchema(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy thread", error) || !requireStringMember(value, "id", true, error) ||
                !requireBoolMember(value, "fullyLoaded", error))
                return false;
            const auto turns = value.find("turns");
            const auto extensions = value.find("extensions");
            if (turns == value.end() || !turns->is_array() || extensions == value.end() || !extensions->is_object()) {
                error = "legacy thread turns must be an array and extensions must be an object";
                return false;
            }
            for (const frontend::Json& turn : *turns) {
                if (!validateLegacyTurnSchema(turn, error))
                    return false;
            }
            for (const std::string_view key : {"title", "cwd", "model", "modelProvider", "preview", "status"}) {
                if (!validateOptionalString(value, key, error))
                    return false;
            }
            return validateOptionalInteger(value, "createdAt", error) && validateOptionalInteger(value, "updatedAt", error);
        }

        bool validateLegacyPendingRequestSchema(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy pending request", error) || !requireDecimalIdMember(value, "id", error) ||
                !requireStringMember(value, "type", true, error))
                return false;
            const auto type = stringMember(value, "type");
            if (!type || (*type != "command_approval" && *type != "file_change_approval" && *type != "user_input" &&
                          *type != "authentication" && *type != "unknown")) {
                error = "legacy pending request type is not a defined stable value";
                return false;
            }
            const auto details = value.find("details");
            if (details == value.end() || !details->is_object()) {
                error = "legacy pending request details must be an object";
                return false;
            }
            return validateOptionalString(value, "threadId", error) && validateOptionalString(value, "turnId", error) &&
                   validateOptionalString(value, "itemId", error);
        }

        bool validateLegacyErrorSnapshot(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy error snapshot", error) || !requireStringMember(value, "category", false, error) ||
                !requireStringMember(value, "message", false, error) || !validateOptionalInteger(value, "code", error) ||
                value.find("code") == value.end())
                return false;
            const auto category = stringMember(value, "category");
            constexpr std::array<std::string_view, 9> categories{
                "launch", "transport", "protocol", "initialization", "process", "invalid_state", "capacity", "cancelled", "enqueue"};
            if (!category || std::find(categories.begin(), categories.end(), *category) == categories.end()) {
                error = "legacy error snapshot category is not a defined stable value";
                return false;
            }
            return true;
        }

        bool validateLegacyExtensionTruncation(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy extension truncation", error))
                return false;
            for (auto member = value.begin(); member != value.end(); ++member) {
                if (member.key() != "originalBytes" && member.key() != "retainedBytes") {
                    error = "legacy extension truncation contains an unknown field";
                    return false;
                }
                if (!optionalUnsigned(value, member.key())) {
                    error = "legacy extension truncation byte counts must be unsigned integers";
                    return false;
                }
            }
            return true;
        }

        bool validateLegacyEvent(const frontend::FrontendEvent& event, std::string& error) {
            const frontend::Json& data = event.data;
            if (!requireObject(data, "legacy event data", error))
                return false;
            if (event.type == "backend.lifecycle.changed") {
                const auto lifecycle = stringMember(data, "lifecycle");
                constexpr std::array<std::string_view, 6> values{
                    "stopped", "starting", "initializing", "ready", "stopping", "failed"};
                if (!lifecycle || std::find(values.begin(), values.end(), *lifecycle) == values.end()) {
                    error = "legacy provider lifecycle is not a defined stable value";
                    return false;
                }
                const auto eventError = data.find("error");
                return eventError == data.end() || validateLegacyErrorSnapshot(*eventError, error);
            }
            if (event.type == "diagnostics.updated") {
                if (!requireUnsignedMember(data, "received", error))
                    return false;
                const auto recent = data.find("recent");
                if (recent == data.end() || !recent->is_array()) {
                    error = "legacy diagnostics update recent must be an array";
                    return false;
                }
                for (const frontend::Json& message : *recent) {
                    if (!message.is_string()) {
                        error = "legacy diagnostics update recent must contain strings";
                        return false;
                    }
                }
                return true;
            }
            if (event.type == "thread.updated") {
                const frontend::Json* thread = nullptr;
                return requireObjectMember(data, "thread", thread, error) && validateLegacyThreadSchema(*thread, error);
            }
            if (event.type == "thread.list.updated") {
                if (!requireBoolMember(data, "hasLoadedPage", error) || !requireBoolMember(data, "complete", error) ||
                    !requireUnsignedMember(data, "pagesLoaded", error))
                    return false;
                return validateOptionalString(data, "nextCursor", error) && validateOptionalString(data, "backwardsCursor", error);
            }
            if (event.type == "turn.updated") {
                const frontend::Json* turn = nullptr;
                return requireObjectMember(data, "turn", turn, error) && validateLegacyTurnSchema(*turn, error);
            }
            if (event.type == "item.updated") {
                const frontend::Json* item = nullptr;
                return requireStringMember(data, "threadId", true, error) && requireStringMember(data, "turnId", true, error) &&
                       requireObjectMember(data, "item", item, error) && validateLegacyItemSchema(*item, error);
            }
            if (event.type == "item.content.updated") {
                if (!requireStringMember(data, "threadId", true, error) || !requireStringMember(data, "turnId", true, error) ||
                    !requireStringMember(data, "itemId", true, error) || !requireStringMember(data, "channel", true, error) ||
                    !requireStringMember(data, "content", false, error) || !requireBoolMember(data, "contentTruncated", error) ||
                    !requireUnsignedMember(data, "droppedContentBytes", error))
                    return false;
                const auto channel = stringMember(data, "channel");
                if (*channel != "agentText" && *channel != "reasoningText" && *channel != "reasoningSummary" &&
                    *channel != "commandOutput") {
                    error = "legacy item content channel is not a defined stable value";
                    return false;
                }
                return true;
            }
            if (event.type == "request.pending") {
                const frontend::Json* request = nullptr;
                return requireObjectMember(data, "request", request, error) && validateLegacyPendingRequestSchema(*request, error);
            }
            if (event.type == "request.resolved")
                return requireDecimalIdMember(data, "pendingRequestId", error) && requireStringMember(data, "reason", false, error);
            if (event.type == "controller.changed") {
                const auto id = data.find("controllerSessionId");
                return id == data.end() || requireDecimalIdMember(data, "controllerSessionId", error);
            }
            if (event.type == "session.changed") {
                if (!requireDecimalIdMember(data, "sessionId", error) || !requireBoolMember(data, "connected", error) ||
                    !requireStringMember(data, "role", true, error))
                    return false;
                const auto role = stringMember(data, "role");
                if (*role != "observer" && *role != "controller") {
                    error = "legacy session role is not a defined stable value";
                    return false;
                }
                return true;
            }
            if (event.type == "codex.extension") {
                if (!requireStringMember(data, "method", true, error) || data.find("params") == data.end() ||
                    !validateOptionalString(data, "decodingError", error))
                    return false;
                const auto redacted = data.find("sensitiveFieldsRedacted");
                if (redacted != data.end() && !redacted->is_boolean()) {
                    error = "legacy extension sensitiveFieldsRedacted must be a boolean";
                    return false;
                }
                const auto truncation = data.find("truncation");
                if (truncation == data.end())
                    return true;
                if (!truncation->is_object()) {
                    error = "legacy extension truncation must be an object";
                    return false;
                }
                for (auto member = truncation->begin(); member != truncation->end(); ++member) {
                    if (member.key() != "method" && member.key() != "params" && member.key() != "decodingError") {
                        error = "legacy extension truncation contains an unknown channel";
                        return false;
                    }
                    if (!validateLegacyExtensionTruncation(member.value(), error))
                        return false;
                }
                return true;
            }
            error = "unknown legacy frontend event type '" + event.type + "'";
            return false;
        }

        bool validateLegacySnapshotSchema(const frontend::Json& state, std::string& error) {
            if (!requireObject(state, "legacy snapshot state", error) ||
                !requireUnsignedMember(state, "backendRevision", error) ||
                !requireStringMember(state, "lifecycle", true, error) ||
                !requireUnsignedMember(state, "omittedCodexExtensions", error) ||
                !requireBoolMember(state, "sequenceExhausted", error))
                return false;

            const auto lifecycle = stringMember(state, "lifecycle");
            constexpr std::array<std::string_view, 6> lifecycleValues{
                "stopped", "starting", "initializing", "ready", "stopping", "failed"};
            if (!lifecycle || std::find(lifecycleValues.begin(), lifecycleValues.end(), *lifecycle) == lifecycleValues.end()) {
                error = "legacy snapshot lifecycle is not a defined stable value";
                return false;
            }
            if (const auto exhausted = state.find("frontendSequenceExhausted");
                exhausted != state.end() && !exhausted->is_boolean()) {
                error = "legacy snapshot frontendSequenceExhausted must be a boolean";
                return false;
            }
            if (const auto controller = state.find("controllerSessionId");
                controller != state.end() && !requireDecimalIdMember(state, "controllerSessionId", error))
                return false;
            if (const auto controller = state.find("controller"); controller != state.end()) {
                if (controller->is_string()) {
                    if (controller->get_ref<const std::string&>().empty()) {
                        error = "legacy snapshot controller string must not be empty";
                        return false;
                    }
                } else if (!controller->is_number_unsigned() && !controller->is_object()) {
                    error = "legacy snapshot controller must be a string, unsigned integer, or object";
                    return false;
                }
                if (controller->is_object()) {
                    if (controller->contains("controllerSessionId") &&
                        !requireStringMember(*controller, "controllerSessionId", true, error))
                        return false;
                    if (controller->contains("present") && !requireBoolMember(*controller, "present", error))
                        return false;
                }
            }
            if (const auto lastError = state.find("lastLifecycleError");
                lastError != state.end() && !validateLegacyErrorSnapshot(*lastError, error))
                return false;

            const auto diagnostics = state.find("diagnostics");
            if (diagnostics == state.end() || !diagnostics->is_object() ||
                !validateLegacyEvent(frontend::FrontendEvent{frontend::SequenceNumber{1}, "diagnostics.updated", *diagnostics}, error))
                return false;

            const auto threads = state.find("threads");
            if (threads == state.end() || !threads->is_array()) {
                error = "legacy snapshot threads must be an array";
                return false;
            }
            for (const frontend::Json& thread : *threads) {
                if (!validateLegacyThreadSchema(thread, error))
                    return false;
            }

            if (const auto items = state.find("items"); items != state.end() && !items->is_array()) {
                error = "legacy snapshot complete-thread-items projection must be an array";
                return false;
            }

            const auto requests = state.find("pendingRequests");
            if (requests == state.end() || !requests->is_array()) {
                error = "legacy snapshot pendingRequests must be an array";
                return false;
            }
            for (const frontend::Json& request : *requests) {
                if (!validateLegacyPendingRequestSchema(request, error))
                    return false;
            }

            const auto sessions = state.find("sessions");
            if (sessions == state.end() || !sessions->is_array()) {
                error = "legacy snapshot sessions must be an array";
                return false;
            }
            for (const frontend::Json& session : *sessions) {
                if (!requireObject(session, "legacy snapshot session", error) ||
                    !requireDecimalIdMember(session, "sessionId", error) || !requireStringMember(session, "role", true, error))
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
                if (!validateLegacyEvent(
                        frontend::FrontendEvent{frontend::SequenceNumber{1}, "codex.extension", extension}, error))
                    return false;
            }

            const auto threadList = state.find("threadList");
            if (threadList == state.end() || !threadList->is_object() ||
                !validateLegacyEvent(frontend::FrontendEvent{frontend::SequenceNumber{1}, "thread.list.updated", *threadList}, error))
                return false;

            const auto journal = state.find("journal");
            if (journal == state.end() || !journal->is_object() ||
                !requireUnsignedMember(*journal, "oldestReplayableAfter", error) ||
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
                for (const std::string_view name : {"accounts", "models", "configuration", "reviews", "integrations",
                                                    "pluginsAndSkills", "mcp", "platform"}) {
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
            for (const std::string_view name : {"processes", "filesystemWatches", "fuzzySearches", "fuzzySearchSessions",
                                                "notices", "activities"}) {
                if (!validateCollection(name))
                    return false;
            }

            if (const auto capacity = state.find("capacity"); capacity != state.end()) {
                if (!capacity->is_object()) {
                    error = "legacy snapshot capacity must be an object";
                    return false;
                }
                for (const std::string_view name : {
                         "sessions", "observers", "activeOperations", "pendingRequests", "retainedThreads", "retainedTurns",
                         "retainedItems", "accumulatedContentBytes", "retainedNotices", "retainedProcesses",
                         "accumulatedProcessOutputBytes", "retainedFilesystemWatches", "retainedFuzzySearchSessions",
                         "retainedActivityRecords", "evictedNotices", "evictedProcesses", "droppedProcessOutputBytes",
                         "evictedFilesystemWatches", "evictedFuzzySearchSessions", "evictedActivityRecords"}) {
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
                    decoded.extensions = extensionsOf(entry, {"method", "status", "subjectId", "nextCursor", "itemCount", "complete", "stamp"});
                    result.latestResults.push_back(std::move(decoded));
                }
            }
            result.extensions = extensionsOf(value,
                                             {"stamp", "status", "summary", "nextCursor", "complete", "itemCount", "latestResults",
                                              "details", "truncation"});
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
    if (value->contains(#name) && !result.details.name) {                                                                                   \
        error = "account domain detail '" #name "' must be boolean";                                                                     \
        return false;                                                                                                                       \
    }
#define AISUITE_ACCOUNT_DETAIL_STRING(name)                                                                                                \
    result.details.name = stringMember(*value, #name);                                                                                     \
    if (value->contains(#name) && !result.details.name) {                                                                                   \
        error = "account domain detail '" #name "' must be a string";                                                                    \
        return false;                                                                                                                       \
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
            result.projection.opaqueDetails = extensionsOf(
                *value,
                {"notificationCount", "latestNotificationMethods", "loggedOut", "loginLifecycle", "loginMethod",
                 "loginSucceeded", "authenticated", "accountType", "authMode", "planType", "primaryUsedPercent",
                 "secondaryUsedPercent", "hasCredits"});
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
            result.projection.opaqueDetails = extensionsOf(
                *value,
                {"notificationCount", "latestNotificationMethods", "filePath", "writeStatus", "writeVersion",
                 "writeOverridden", "featureCount", "featureListTruncated"});
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
            result.projection.opaqueDetails = extensionsOf(
                *value,
                {"notificationCount", "latestNotificationMethods", "appCount", "appListTruncated", "marketplaceAddStatus",
                 "marketplaceRemoveStatus", "marketplaceUpgradeStatus"});
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
            result.projection.opaqueDetails = extensionsOf(
                *value,
                {"notificationCount", "latestNotificationMethods", "lastPluginOperation", "lastSkillsOperation",
                 "extraRootCount", "extraRootsTruncated"});
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
                {"notificationCount", "latestNotificationMethods", "oauthStatus", "startupStatus", "serverCount",
                 "statusListComplete"});
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
            result.projection.opaqueDetails = extensionsOf(
                *value,
                {"notificationCount", "latestNotificationMethods", "remoteControlStatus", "windowsSandboxStatus"});
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
                                             {"processHandle", "lifecycle", "stdout", "stderr", "stdoutBytes", "stderrBytes",
                                              "stdoutTruncated", "stderrTruncated", "droppedOutputBytes", "exitCode", "stamp",
                                              "connectionInvalidated", "stateUnavailable"});
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
            result.extensions = extensionsOf(value, {"watchId", "root", "changedPathCount", "stamp", "connectionInvalidated", "stateUnavailable"});
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
            result.extensions = extensionsOf(value, {"sessionId", "resultCount", "complete", "stamp", "connectionInvalidated", "stateUnavailable"});
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
            result.extensions = extensionsOf(value, {"occurrence", "category", "summary", "details", "threadId", "stamp", "stateUnavailable"});
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
                                             {"key", "subjectId", "kind", "lifecycle", "summary", "details", "threadId", "turnId",
                                              "active", "stamp", "stateUnavailable"});
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
                                             {"sessions", "observers", "activeOperations", "pendingRequests", "retainedThreads",
                                              "retainedTurns", "retainedItems", "accumulatedContentBytes", "retainedNotices",
                                              "retainedProcesses", "accumulatedProcessOutputBytes", "retainedFilesystemWatches",
                                              "retainedFuzzySearchSessions", "retainedActivityRecords", "evictedNotices",
                                              "evictedProcesses", "droppedProcessOutputBytes", "evictedFilesystemWatches",
                                              "evictedFuzzySearchSessions", "evictedActivityRecords"});
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
                if (!appendDistinct(items,
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

        bool applyCompleteLegacyTurn(detail::StateStorage& state,
                                     TurnState turn,
                                     std::vector<ItemState> items,
                                     std::string& error) {
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
                if (!appendDistinct(turns,
                                    std::move(turn),
                                    [](const TurnState& entry) {
                                        return entry.id;
                                    },
                                    "legacy thread turns",
                                    error))
                    return false;
                for (ItemState& item : turnItems) {
                    if (!appendDistinct(items,
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

            replaceOrderedSubset(state.items, std::move(items), [&](const ItemState& candidate) {
                return (candidate.turnId &&
                        std::find(affectedTurnIds.begin(), affectedTurnIds.end(), *candidate.turnId) != affectedTurnIds.end()) ||
                       std::find(itemIds.begin(), itemIds.end(), candidate.id) != itemIds.end();
            });
            replaceOrderedSubset(state.turns, std::move(turns), [&](const TurnState& candidate) {
                return candidate.threadId == threadId ||
                       std::find(turnIds.begin(), turnIds.end(), candidate.id) != turnIds.end();
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
                if (!appendDistinct(result.entries,
                                    std::move(decoded),
                                    [](const ProcessState& process) {
                                        return process.processHandle;
                                    },
                                    "process collection",
                                    error))
                    return false;
            }
            if (const auto truncation = value.find("truncation"); truncation != value.end() &&
                !decodeTruncation(*truncation, result.truncation, error))
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
                if (!appendDistinct(result.entries,
                                    std::move(decoded),
                                    [](const FilesystemWatchState& watch) {
                                        return watch.watchId;
                                    },
                                    "filesystem watch collection",
                                    error))
                    return false;
            }
            if (const auto truncation = value.find("truncation"); truncation != value.end() &&
                !decodeTruncation(*truncation, result.truncation, error))
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
                if (!appendDistinct(result.entries,
                                    std::move(decoded),
                                    [](const FuzzySearchState& search) {
                                        return search.sessionId;
                                    },
                                    "fuzzy-search collection",
                                    error))
                    return false;
            }
            if (const auto truncation = value.find("truncation"); truncation != value.end() &&
                !decodeTruncation(*truncation, result.truncation, error))
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
            if (const auto truncation = value.find("truncation"); truncation != value.end() &&
                !decodeTruncation(*truncation, result.truncation, error))
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
                if (!appendDistinct(result.entries,
                                    std::move(decoded),
                                    [](const ActivityState& activity) {
                                        return activity.key;
                                    },
                                    "activity collection",
                                    error))
                    return false;
            }
            if (const auto truncation = value.find("truncation"); truncation != value.end() &&
                !decodeTruncation(*truncation, result.truncation, error))
                return false;
            result.extensions = extensionsOf(value, {"entries", "truncation"});
            return true;
        }

        bool hasSelectedRepresentationCapability(const SessionInfo& session,
                                                 frontend::FrontendCapability capability) {
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

        bool eventRepresentationWasNegotiated(const SessionInfo* session,
                                              std::string_view type,
                                              bool expanded,
                                              std::string& error) {
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
                if (!appendDistinct(*result.sessions.value,
                                    std::move(session),
                                    [](const SessionState& entry) {
                                        return entry.sessionId;
                                    },
                                    "session collection",
                                    error))
                    return false;
            }
            if (state.threads) {
                result.threadProjectionPresent = true;
                for (const frontend::Json& value : *state.threads) {
                    ThreadState thread;
                    if (!decodeThread(value, thread, error))
                        return false;
                    if (!appendDistinct(result.threads,
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
                    if (!appendDistinct(result.turns,
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
                    if (!appendDistinct(result.items,
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
                    if (!appendDistinct(result.pendingRequests,
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
    if (state.member) {                                                                                                                     \
        if (!decodeDomainWrapper(*state.member, result.member, error))                                                                      \
            return false;                                                                                                                   \
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
            result.backendCursor.oldestReplayableAfter =
                frontend::SequenceNumber{*optionalUnsigned(journal, "oldestReplayableAfter")};
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
                    if (!appendDistinct(*result.sessions.value,
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
                    if (!appendDistinct(result.threads,
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
                            if (!appendDistinct(result.turns,
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
                                    if (!appendDistinct(result.items,
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
                    if (!appendDistinct(result.pendingRequests,
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
#define AISUITE_LEGACY_DOMAIN(jsonName, member)                                                                                             \
    if (const auto value = domains->find(jsonName); value != domains->end() && value->is_object()) {                                        \
        if (!decodeDomainWrapper(*value, result.member, error))                                                                             \
            return false;                                                                                                                   \
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
                result.windowsSandbox.value =
                    WindowsSandboxState{result.platform.value->projection, result.platform.value->details};
                result.windowsSandbox.truncated = result.platform.truncated;
                result.windowsSandbox.omittedFields = result.platform.omittedFields;
            }

            // The explicit forms avoid exposing a generic domain container and
            // keep each collection's stable element type visible to callers.
            const auto legacyCollectionWrapper = [&error](const frontend::Json& value, std::string_view name)
                -> std::optional<frontend::Json> {
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
            result.compatibilityExtensions = extensionsOf(
                state,
                {"backendRevision", "provider", "lifecycle", "lastLifecycleError", "controller", "controllerSessionId", "sessions",
                 "threadList", "threads", "items", "pendingRequests", "domains", "accounts", "models",
                 "configuration", "reviews", "integrations", "pluginsAndSkills", "mcp", "platform", "processes",
                 "filesystemWatches", "fuzzySearches", "fuzzySearchSessions", "notices", "activities", "capacity",
                 "diagnostics", "codexExtensions", "omittedCodexExtensions", "journal", "sequenceExhausted",
                 "frontendSequenceExhausted"});
            if (!state.at("codexExtensions").empty())
                result.compatibilityExtensions["codexExtensions"] = state.at("codexExtensions");
            return true;
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

        frontend::Json encodeState(const detail::StateStorage& state) {
            frontend::Json result = frontend::Json::object();
            result["revision"] = state.revision;
            result["freshness"] = static_cast<unsigned>(state.freshness);
            result["representationMode"] = static_cast<unsigned>(state.representationMode);
            if (state.visibleSequence)
                result["visibleSequence"] = state.visibleSequence->value();
            if (state.synchronizedThrough)
                result["synchronizedThrough"] = state.synchronizedThrough->value();
            if (state.session) {
                frontend::Json session = frontend::Json::object();
                session["sessionId"] = state.session->sessionId;
                session["role"] = std::string(frontend::toString(state.session->role));
                session["syncMode"] = std::string(frontend::toString(state.session->syncMode));
                session["serverCurrentSequence"] = state.session->serverCurrentSequence.value();
                addOptional(session, "serverVersion", state.session->serverVersion);
                const auto encodeCapabilities = [](const std::vector<frontend::FrontendCapability>& capabilities) {
                    frontend::Json encoded = frontend::Json::array();
                    for (frontend::FrontendCapability capability : capabilities)
                        encoded.push_back(std::string(frontend::toString(capability)));
                    return encoded;
                };
                session["requestedRepresentationCapabilities"] = encodeCapabilities(state.session->requestedRepresentationCapabilities);
                session["selectedRepresentationCapabilities"] = encodeCapabilities(state.session->selectedRepresentationCapabilities);
                session["observedMechanismCapabilities"] = encodeCapabilities(state.session->observedMechanismCapabilities);
                session["observedTopologyCapabilities"] = encodeCapabilities(state.session->observedTopologyCapabilities);
                session["observedProductCapabilities"] = encodeCapabilities(state.session->observedProductCapabilities);
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
                session["availableMethods"] = encodeMethods(state.session->availableMethods);
                session["permittedMethods"] = encodeMethods(state.session->permittedMethods);
                frontend::Json scopes = frontend::Json::object();
                scopes["present"] = state.session->permittedScopes.has_value();
                if (state.session->permittedScopes) {
                    scopes["values"] = frontend::Json::array();
                    for (frontend::FrontendScope scope : *state.session->permittedScopes)
                        scopes["values"].push_back(std::string(frontend::toString(scope)));
                }
                session["permittedScopes"] = std::move(scopes);
                result["session"] = std::move(session);
            }

            frontend::Json backendCursor = frontend::Json::object();
            addOptional(backendCursor, "backendRevision", state.backendCursor.backendRevision);
            if (state.backendCursor.oldestReplayableAfter)
                backendCursor["oldestReplayableAfter"] = state.backendCursor.oldestReplayableAfter->value();
            if (state.backendCursor.currentSequence)
                backendCursor["currentSequence"] = state.backendCursor.currentSequence->value();
            if (state.backendCursor.oldestRetainedSequence)
                backendCursor["oldestRetainedSequence"] = state.backendCursor.oldestRetainedSequence->value();
            if (state.backendCursor.newestRetainedSequence)
                backendCursor["newestRetainedSequence"] = state.backendCursor.newestRetainedSequence->value();
            addOptional(backendCursor, "backendSequenceExhausted", state.backendCursor.backendSequenceExhausted);
            addOptional(backendCursor, "frontendSequenceExhausted", state.backendCursor.frontendSequenceExhausted);
            if (!backendCursor.empty())
                result["backendCursor"] = std::move(backendCursor);

            if (state.projectionFingerprint)
                result["projectionFingerprint"] = state.projectionFingerprint->canonical;

            if (!state.projectionMetadata.omittedFields.empty() || !state.projectionMetadata.redactedFields.empty()) {
                frontend::Json projection = frontend::Json::object();
                projection["omittedFields"] = frontend::Json::array();
                for (const std::string& path : state.projectionMetadata.omittedFields)
                    projection["omittedFields"].push_back(path);
                projection["redactedFields"] = frontend::Json::array();
                for (const std::string& path : state.projectionMetadata.redactedFields)
                    projection["redactedFields"].push_back(path);
                result["projectionMetadata"] = std::move(projection);
            }

            result["provider"] = encodeProjected(state.provider, encodeProvider);
            result["controller"] = encodeProjected(state.controller, [](const ControllerState& value) {
                frontend::Json encoded = value.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
                if (value.sessionId)
                    encoded["sessionId"] = value.sessionId->value;
                encoded["present"] = value.present;
                encoded["ownedByThisClient"] = value.ownedByThisClient;
                return encoded;
            });
            result["sessions"] = encodeProjected(state.sessions, [](const std::vector<SessionState>& values) {
                frontend::Json encoded = frontend::Json::array();
                for (const SessionState& value : values) {
                    frontend::Json entry = value.extensions;
                    if (!entry.is_object())
                        entry = frontend::Json::object();
                    entry["sessionId"] = value.sessionId.value;
                    entry["role"] = std::string(frontend::toString(value.role));
                    encoded.push_back(std::move(entry));
                }
                return encoded;
            });
            result["threadList"] = encodeProjected(state.threadList, [](const ThreadListState& value) {
                frontend::Json encoded = value.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
                encoded["hasLoadedPage"] = value.hasLoadedPage;
                encoded["complete"] = value.complete;
                encoded["pagesLoaded"] = value.pagesLoaded;
                addOptional(encoded, "nextCursor", value.nextCursor);
                addOptional(encoded, "backwardsCursor", value.backwardsCursor);
                if (value.stamp)
                    encoded["stamp"] = encodeStamp(*value.stamp);
                return encoded;
            });
            result["threads"] = frontend::Json::array();
            result["threadProjectionPresent"] = state.threadProjectionPresent;
            for (const ThreadState& value : state.threads) {
                frontend::Json encoded = value.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
                encoded["id"] = value.id.value;
                addOptional(encoded, "title", value.title);
                addOptional(encoded, "preview", value.preview);
                if (value.cwd)
                    encoded["cwd"] = value.cwd->value;
                if (value.model)
                    encoded["model"] = value.model->value;
                addOptional(encoded, "modelProvider", value.modelProvider);
                addOptional(encoded, "status", value.status);
                encoded["fullyLoaded"] = value.fullyLoaded;
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
                    encoded["realtime"] = std::move(realtime);
                }
                if (value.stamp)
                    encoded["stamp"] = encodeStamp(*value.stamp);
                addOptional(encoded, "createdAtMs", value.createdAtMs);
                addOptional(encoded, "updatedAtMs", value.updatedAtMs);
                encoded["orderedTurns"] = frontend::Json::array();
                for (const typed::TurnId& id : value.orderedTurns)
                    encoded["orderedTurns"].push_back(id.value);
                result["threads"].push_back(std::move(encoded));
            }
            result["turns"] = frontend::Json::array();
            result["turnProjectionPresent"] = state.turnProjectionPresent;
            for (const TurnState& value : state.turns) {
                frontend::Json encoded = value.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
                encoded["id"] = value.id.value;
                encoded["threadId"] = value.threadId.value;
                encoded["status"] = value.status.value;
                encoded["active"] = value.active;
                encoded["terminal"] = value.terminal;
                encoded["connectionInvalidated"] = value.connectionInvalidated;
                if (value.stamp)
                    encoded["stamp"] = encodeStamp(*value.stamp);
                encoded["orderedItems"] = frontend::Json::array();
                for (const typed::ItemId& id : value.orderedItems)
                    encoded["orderedItems"].push_back(id.value);
                if (value.failure)
                    encoded["failure"] = *value.failure;
                if (value.tokenUsage)
                    encoded["tokenUsage"] = *value.tokenUsage;
                result["turns"].push_back(std::move(encoded));
            }
            result["items"] = frontend::Json::array();
            result["itemProjectionPresent"] = state.itemProjectionPresent;
            for (const ItemState& value : state.items) {
                frontend::Json encoded = value.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
                encoded["id"] = value.id.value;
                if (value.threadId)
                    encoded["threadId"] = value.threadId->value;
                if (value.turnId)
                    encoded["turnId"] = value.turnId->value;
                encoded["kind"] = value.kind.identity;
                addOptional(encoded, "status", value.status);
                addOptional(encoded, "summary", value.summary);
                if (value.location)
                    encoded["location"] = *value.location;
                addOptional(encoded, "agentText", value.agentText);
                addOptional(encoded, "reasoningText", value.reasoningText);
                addOptional(encoded, "reasoningSummary", value.reasoningSummary);
                addOptional(encoded, "commandOutput", value.commandOutput);
                addOptional(encoded, "droppedContentBytes", value.droppedContentBytes);
                encoded["contentTruncated"] = value.contentTruncated;
                addOptional(encoded, "startedAtMs", value.startedAtMs);
                addOptional(encoded, "completedAtMs", value.completedAtMs);
                if (value.data)
                    encoded["data"] = *value.data;
                encoded["truncated"] = value.truncated;
                encoded["omittedFields"] = frontend::Json::array();
                for (const std::string& field : value.omittedFields)
                    encoded["omittedFields"].push_back(field);
                encoded["connectionInvalidated"] = value.connectionInvalidated;
                if (value.stamp)
                    encoded["stamp"] = encodeStamp(*value.stamp);
                result["items"].push_back(std::move(encoded));
            }
            result["pendingRequests"] = frontend::Json::array();
            result["pendingRequestProjectionPresent"] = state.pendingRequestProjectionPresent;
            for (const PendingRequestState& value : state.pendingRequests) {
                frontend::Json encoded = value.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
                encoded["id"] = value.id.value;
                encoded["kind"] = std::string(frontend::toString(value.kind));
                if (value.threadId)
                    encoded["threadId"] = value.threadId->value;
                if (value.turnId)
                    encoded["turnId"] = value.turnId->value;
                if (value.itemId)
                    encoded["itemId"] = value.itemId->value;
                addOptional(encoded, "summary", value.summary);
                if (value.opaqueDetails)
                    encoded["details"] = *value.opaqueDetails;
                if (value.questions) {
                    encoded["questions"] = frontend::Json::array();
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
                        encoded["questions"].push_back(std::move(questionJson));
                    }
                }
                addOptional(encoded, "autoResolutionMs", value.autoResolutionMs);
                encoded["truncated"] = value.truncated;
                encoded["omittedFields"] = frontend::Json::array();
                for (const std::string& field : value.omittedFields)
                    encoded["omittedFields"].push_back(field);
                encoded["connectionInvalidated"] = value.connectionInvalidated;
                result["pendingRequests"].push_back(std::move(encoded));
            }

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

            result["processes"] = encodeProjected(state.processes, [](const ProcessCollectionState& value) {
                frontend::Json encoded = value.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
                encoded["entries"] = frontend::Json::array();
                for (const ProcessState& entry : value.entries) {
                    frontend::Json item = entry.extensions;
                    if (!item.is_object())
                        item = frontend::Json::object();
                    item["processHandle"] = entry.processHandle.value;
                    item["lifecycle"] = entry.lifecycle;
                    addOptional(item, "stdout", entry.standardOutput);
                    addOptional(item, "stderr", entry.standardError);
                    addOptional(item, "stdoutBytes", entry.stdoutBytes);
                    addOptional(item, "stderrBytes", entry.stderrBytes);
                    item["stdoutTruncated"] = entry.stdoutTruncated;
                    item["stderrTruncated"] = entry.stderrTruncated;
                    addOptional(item, "droppedOutputBytes", entry.droppedOutputBytes);
                    addOptional(item, "exitCode", entry.exitCode);
                    item["stamp"] = encodeStamp(entry.stamp);
                    item["connectionInvalidated"] = entry.connectionInvalidated;
                    item["stateUnavailable"] = entry.stateUnavailable;
                    encoded["entries"].push_back(std::move(item));
                }
                encoded["truncation"] = encodeTruncation(value.truncation);
                return encoded;
            });
            result["filesystemWatches"] = encodeProjected(state.filesystemWatches, [](const FilesystemWatchCollectionState& value) {
                frontend::Json encoded = value.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
                encoded["entries"] = frontend::Json::array();
                for (const FilesystemWatchState& entry : value.entries) {
                    frontend::Json item = entry.extensions;
                    if (!item.is_object())
                        item = frontend::Json::object();
                    item["watchId"] = entry.watchId.value;
                    if (entry.root)
                        item["root"] = entry.root->value;
                    addOptional(item, "changedPathCount", entry.changedPathCount);
                    item["stamp"] = encodeStamp(entry.stamp);
                    item["connectionInvalidated"] = entry.connectionInvalidated;
                    item["stateUnavailable"] = entry.stateUnavailable;
                    encoded["entries"].push_back(std::move(item));
                }
                encoded["truncation"] = encodeTruncation(value.truncation);
                return encoded;
            });
            result["fuzzySearches"] = encodeProjected(state.fuzzySearches, [](const FuzzySearchCollectionState& value) {
                frontend::Json encoded = value.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
                encoded["entries"] = frontend::Json::array();
                for (const FuzzySearchState& entry : value.entries) {
                    frontend::Json item = entry.extensions;
                    if (!item.is_object())
                        item = frontend::Json::object();
                    item["sessionId"] = entry.sessionId.value;
                    addOptional(item, "resultCount", entry.resultCount);
                    item["complete"] = entry.complete;
                    item["stamp"] = encodeStamp(entry.stamp);
                    item["connectionInvalidated"] = entry.connectionInvalidated;
                    item["stateUnavailable"] = entry.stateUnavailable;
                    encoded["entries"].push_back(std::move(item));
                }
                encoded["truncation"] = encodeTruncation(value.truncation);
                return encoded;
            });
            result["notices"] = encodeProjected(state.notices, [](const NoticeCollectionState& value) {
                frontend::Json encoded = value.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
                encoded["entries"] = frontend::Json::array();
                for (const NoticeState& entry : value.entries) {
                    frontend::Json item = entry.extensions;
                    if (!item.is_object())
                        item = frontend::Json::object();
                    addOptional(item, "occurrence", entry.occurrence);
                    item["category"] = entry.category;
                    item["summary"] = entry.summary;
                    addOptional(item, "details", entry.details);
                    if (entry.threadId)
                        item["threadId"] = entry.threadId->value;
                    item["stamp"] = encodeStamp(entry.stamp);
                    item["stateUnavailable"] = entry.stateUnavailable;
                    encoded["entries"].push_back(std::move(item));
                }
                encoded["truncation"] = encodeTruncation(value.truncation);
                return encoded;
            });
            result["activities"] = encodeProjected(state.activities, [](const ActivityCollectionState& value) {
                frontend::Json encoded = value.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
                encoded["entries"] = frontend::Json::array();
                for (const ActivityState& entry : value.entries) {
                    frontend::Json item = entry.extensions;
                    if (!item.is_object())
                        item = frontend::Json::object();
                    item["key"] = entry.key.value;
                    addOptional(item, "subjectId", entry.subjectId);
                    item["kind"] = entry.kind;
                    item["lifecycle"] = entry.lifecycle;
                    addOptional(item, "summary", entry.summary);
                    addOptional(item, "details", entry.details);
                    if (entry.threadId)
                        item["threadId"] = entry.threadId->value;
                    if (entry.turnId)
                        item["turnId"] = entry.turnId->value;
                    item["active"] = entry.active;
                    item["stamp"] = encodeStamp(entry.stamp);
                    item["stateUnavailable"] = entry.stateUnavailable;
                    encoded["entries"].push_back(std::move(item));
                }
                encoded["truncation"] = encodeTruncation(value.truncation);
                return encoded;
            });
            result["capacity"] = encodeProjected(state.capacity, [](const CapacityState& value) {
                frontend::Json encoded = value.extensions;
                if (!encoded.is_object())
                    encoded = frontend::Json::object();
#define AISUITE_ENCODE_CAPACITY(name) addOptional(encoded, #name, value.name)
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
                return encoded;
            });
            result["truncation"] = encodeProjected(state.truncation, encodeTruncation);
            result["diagnostics"] = encodeProjected(state.diagnostics, [](const DiagnosticCollectionState& values) {
                frontend::Json encoded = frontend::Json::object();
                addOptional(encoded, "received", values.received);
                encoded["entries"] = frontend::Json::array();
                for (const DiagnosticState& value : values.entries) {
                    frontend::Json item = value.extensions;
                    if (!item.is_object())
                        item = frontend::Json::object();
                    addOptional(item, "received", value.received);
                    item["detailsOmitted"] = value.detailsOmitted;
                    addOptional(item, "message", value.message);
                    if (value.opaqueDetails)
                        item["details"] = *value.opaqueDetails;
                    encoded["entries"].push_back(std::move(item));
                }
                return encoded;
            });
            result["compatibilityExtensions"] = state.compatibilityExtensions;
            return result;
        }

        std::optional<std::size_t> stateBytes(const detail::StateStorage& state) noexcept {
            try {
                std::size_t bytes = encodeState(state).dump().size();
                const auto accountInternalSequence = [&bytes](const std::optional<frontend::SequenceNumber>& sequence) {
                    if (!sequence)
                        return true;
                    if (bytes > std::numeric_limits<std::size_t>::max() - sizeof(frontend::SequenceNumber))
                        return false;
                    bytes += sizeof(frontend::SequenceNumber);
                    return true;
                };
                if (!accountInternalSequence(state.retainedReplayThrough) ||
                    !accountInternalSequence(state.lastSynchronizationBatchSequence))
                    return std::nullopt;
                return bytes;
            } catch (...) {
                return std::nullopt;
            }
        }

        bool stateFits(const detail::StateStorage& state, std::size_t maximumBytes, std::string& error) {
            const std::optional<std::size_t> encodedBytes = stateBytes(state);
            if (!encodedBytes) {
                error = "decoded frontend state size accounting failed";
                return false;
            }
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

        bool validateExpandedEvent(const frontend::FrontendEvent& event,
                                   frontend::ExpandedFrontendEvent& decoded,
                                   std::string& error) {
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

        bool replaceContent(detail::StateStorage& state,
                            const frontend::Json& data,
                            ItemContentReplacedChange& change,
                            std::string& error) {
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
            using enum frontend::ExpandedEventType;
            switch (event.type) {
                case ProviderUpdated: {
                    const frontend::Json* value = nullptr;
                    ProviderState decoded;
                    if (!requireObjectMember(data, "provider", value, error) || !decodeProvider(*value, decoded, error))
                        return false;
                    state.provider.value = decoded;
                    changes.push_back(ProviderUpdatedChange{std::move(decoded)});
                    return true;
                }
                case ControllerUpdated: {
                    const frontend::Json* value = nullptr;
                    ControllerState decoded;
                    if (!requireObjectMember(data, "controller", value, error) || !decodeController(*value, state.session, decoded, error))
                        return false;
                    state.controller.value = decoded;
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
                        if (!appendDistinct(decoded,
                                            std::move(session),
                                            [](const SessionState& entry) {
                                                return entry.sessionId;
                                            },
                                            "session collection",
                                            error))
                            return false;
                    }
                    state.sessions.value = std::move(decoded);
                    changes.push_back(SessionsUpdatedChange{});
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
                    if (prior != state.threads.end() && value->find("turns") == value->end())
                        decoded.orderedTurns = prior->orderedTurns;
                    const typed::ThreadId id = decoded.id;
                    upsert(state.threads, std::move(decoded), [](const ThreadState& thread) {
                        return thread.id;
                    });
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
                    const std::optional<typed::ThreadId> priorThread =
                        prior == state.turns.end() ? std::nullopt : std::optional(prior->threadId);
                    if (prior != state.turns.end() && value->find("items") == value->end())
                        decoded.orderedItems = prior->orderedItems;
                    const typed::TurnId id = decoded.id;
                    const typed::ThreadId threadId = decoded.threadId;
                    upsert(state.turns, std::move(decoded), [](const TurnState& turn) {
                        return turn.id;
                    });
                    if (priorThread && *priorThread != threadId) {
                        const auto oldThread =
                            std::find_if(state.threads.begin(), state.threads.end(), [&](const ThreadState& entry) {
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
                    const std::optional<typed::TurnId> priorTurn =
                        prior == state.items.end() ? std::nullopt : prior->turnId;
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
                    changes.push_back(ItemUpsertedChange{id});
                    return true;
                }
                case ItemContentUpdated: {
                    state.itemProjectionPresent = true;
                    ItemContentReplacedChange change;
                    if (!replaceContent(state, data, change, error))
                        return false;
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
                        if (!appendDistinct(decoded,
                                            std::move(request),
                                            [](const PendingRequestState& entry) {
                                                return entry.id;
                                            },
                                            "pending-request collection",
                                            error))
                            return false;
                    }
                    state.pendingRequests = std::move(decoded);
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
                        changes.push_back(AccountUpdatedChange{});
                    } else if (event.type == ModelsUpdated) {
                        if (!decodeDomainWrapper(*value, state.models, error))
                            return false;
                        changes.push_back(ModelsUpdatedChange{});
                    } else if (event.type == ConfigurationUpdated) {
                        if (!decodeDomainWrapper(*value, state.configuration, error))
                            return false;
                        changes.push_back(ConfigurationUpdatedChange{});
                    } else if (event.type == ReviewsUpdated) {
                        if (!decodeDomainWrapper(*value, state.reviews, error))
                            return false;
                        state.permissionProfiles.value = PermissionProfilesState{state.reviews.value->projection};
                        state.permissionProfiles.truncated = state.reviews.truncated;
                        state.permissionProfiles.omittedFields = state.reviews.omittedFields;
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
                        changes.push_back(IntegrationsUpdatedChange{});
                    } else if (event.type == PluginsUpdated) {
                        if (!decodeDomainWrapper(*value, state.plugins, error))
                            return false;
                        state.skills.value = SkillsState{state.plugins.value->projection, state.plugins.value->details};
                        state.skills.truncated = state.plugins.truncated;
                        state.skills.omittedFields = state.plugins.omittedFields;
                        changes.push_back(PluginsUpdatedChange{});
                    } else if (event.type == SkillsUpdated) {
                        if (!decodeDomainWrapper(*value, state.skills, error))
                            return false;
                        state.plugins.value = PluginsState{state.skills.value->projection, state.skills.value->details};
                        state.plugins.truncated = state.skills.truncated;
                        state.plugins.omittedFields = state.skills.omittedFields;
                        changes.push_back(SkillsUpdatedChange{});
                    } else if (event.type == McpUpdated) {
                        if (!decodeDomainWrapper(*value, state.mcp, error))
                            return false;
                        changes.push_back(McpUpdatedChange{});
                    } else {
                        if (!decodeDomainWrapper(*value, state.platform, error))
                            return false;
                        state.windowsSandbox.value =
                            WindowsSandboxState{state.platform.value->projection, state.platform.value->details};
                        state.windowsSandbox.truncated = state.platform.truncated;
                        state.windowsSandbox.omittedFields = state.platform.omittedFields;
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
                    upsert(state.processes.value->entries, std::move(decoded), [](const ProcessState& process) {
                        return process.processHandle;
                    });
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
                    upsert(state.filesystemWatches.value->entries, std::move(decoded), [](const FilesystemWatchState& watch) {
                        return watch.watchId;
                    });
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
                    upsert(state.fuzzySearches.value->entries, std::move(decoded), [](const FuzzySearchState& search) {
                        return search.sessionId;
                    });
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
                    upsert(state.activities.value->entries, std::move(decoded), [](const ActivityState& activity) {
                        return activity.key;
                    });
                    changes.push_back(ActivityUpdatedChange{key});
                    return true;
                }
                case CapacityUpdated: {
                    const frontend::Json* value = nullptr;
                    if (!requireObjectMember(data, "capacity", value, error))
                        return false;
                    state.capacity.value = decodeCapacity(*value);
                    changes.push_back(CapacityUpdatedChange{});
                    return true;
                }
                case DiagnosticsUpdated: {
                    const frontend::Json* value = nullptr;
                    if (!requireObjectMember(data, "diagnostic", value, error))
                        return false;
                    DiagnosticState decoded = decodeDiagnostic(*value);
                    const auto received = decoded.received;
                    appendDiagnostic(state, std::move(decoded), maximumRetainedDiagnostics);
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
                if (!requireObjectMember(data, "thread", value, error) ||
                    !decodeCompleteLegacyThread(*value, decoded, turns, items, error))
                    return false;
                const typed::ThreadId id = decoded.id;
                if (!applyCompleteLegacyThread(state, std::move(decoded), std::move(turns), std::move(items), error))
                    return false;
                changes.push_back(ThreadUpsertedChange{id});
                return true;
            }
            if (event.type == "thread.list.updated") {
                ThreadListState decoded;
                if (!decodeThreadList(data, decoded, error))
                    return false;
                state.threadList.value = std::move(decoded);
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
                const std::optional<typed::TurnId> priorTurn = prior == state.items.end() ? std::nullopt : prior->turnId;
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
                changes.push_back(ItemUpsertedChange{id});
                return true;
            }
            if (event.type == "item.content.updated") {
                state.itemProjectionPresent = true;
                ItemContentReplacedChange change;
                if (!replaceContent(state, data, change, error))
                    return false;
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
                upsert(state.pendingRequests, std::move(decoded), [](const PendingRequestState& request) {
                    return request.id;
                });
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
                std::erase_if(state.pendingRequests, [&](const PendingRequestState& request) {
                    return request.id.value == *id;
                });
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
                    state.truncation.value->omittedEntries =
                        omitted == std::numeric_limits<std::size_t>::max() ? omitted : omitted + 1;
                }
                extensions.push_back(data);
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
                std::string_view{"provider"},          std::string_view{"controller"},       std::string_view{"sessions"},
                std::string_view{"threadList"},
                std::string_view{"accounts"},          std::string_view{"models"},           std::string_view{"configuration"},
                std::string_view{"permissionProfiles"}, std::string_view{"reviews"},          std::string_view{"apps"},
                std::string_view{"externalAgents"},    std::string_view{"hooks"},             std::string_view{"marketplace"},
                std::string_view{"plugins"},           std::string_view{"skills"},            std::string_view{"mcp"},
                std::string_view{"windowsSandbox"},    std::string_view{"platform"},          std::string_view{"processes"},
                std::string_view{"filesystemWatches"}, std::string_view{"fuzzySearches"},     std::string_view{"notices"},
                std::string_view{"activities"},        std::string_view{"capacity"},          std::string_view{"truncation"},
                std::string_view{"diagnostics"},
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
        : impl(std::make_shared<const detail::StateStorage>()) {
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
    const Projected<type>& State::name() const noexcept {                                                                                   \
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
        const auto found = std::find_if(impl->processes.value->entries.begin(), impl->processes.value->entries.end(), [handle](const auto& value) {
            return value.processHandle.value == handle;
        });
        return found == impl->processes.value->entries.end() ? nullptr : &*found;
    }
    const FilesystemWatchState* State::filesystemWatch(const typed::FsWatchId& id) const noexcept {
        if (!impl->filesystemWatches.value)
            return nullptr;
        const auto found = std::find_if(impl->filesystemWatches.value->entries.begin(),
                                        impl->filesystemWatches.value->entries.end(),
                                        [&](const auto& value) {
                                            return value.watchId == id;
                                        });
        return found == impl->filesystemWatches.value->entries.end() ? nullptr : &*found;
    }
    const FuzzySearchState* State::fuzzySearch(const FuzzySearchSessionId& id) const noexcept {
        if (!impl->fuzzySearches.value)
            return nullptr;
        const auto found = std::find_if(impl->fuzzySearches.value->entries.begin(), impl->fuzzySearches.value->entries.end(), [&](const auto& value) {
            return value.sessionId == id;
        });
        return found == impl->fuzzySearches.value->entries.end() ? nullptr : &*found;
    }
    const ActivityState* State::activity(const ActivityKey& key) const noexcept {
        if (!impl->activities.value)
            return nullptr;
        const auto found = std::find_if(impl->activities.value->entries.begin(), impl->activities.value->entries.end(), [&](const auto& value) {
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
                if (!stateFits(*fallback, maximumBytes, fallbackError)) {
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
                    candidate->controller.value->sessionId &&
                    candidate->controller.value->sessionId->value == session.sessionId;
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
            if (!stateFits(*candidate, maximumBytes, error))
                return std::nullopt;
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
            if (current.impl->visibleSequence &&
                (!representedThrough || *representedThrough < *current.impl->visibleSequence))
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
            if (!allowLegacyV1 && !selectedCompleteExpandedRepresentation(session)) {
                error = "legacy Frontend Protocol v1 snapshot is not permitted";
                return std::nullopt;
            }
            if (selectedExpandedSnapshot(session)) {
                const auto encoded = frontend::Codec::encodeServer(frontend::ServerMessage{snapshotMessage});
                const auto expanded = encoded ? frontend::Codec::decodeExpandedSnapshot(encoded.value())
                                              : frontend::CodecResult<frontend::ExpandedSnapshot>{encoded.error()};
                if (!expanded) {
                    error = "expanded Frontend Protocol v1 snapshot is required by the negotiated representation";
                    return std::nullopt;
                }
                if (!decodeExpanded(*candidate, expanded.value(), maximumRetainedDiagnostics, error))
                    return std::nullopt;
            } else {
                if (!decodeLegacy(*candidate, snapshotMessage.state, maximumRetainedDiagnostics, error))
                    return std::nullopt;
            }
            if (!applySnapshotProjectionMetadata(*candidate, snapshotMessage.extensions, error))
                return std::nullopt;
            if (!stateFits(*candidate, maximumBytes, error))
                return std::nullopt;
            return StateReduction{State{std::move(candidate)}, {StateReplacedChange{}}, 0, 0, 0};
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
            RepresentationMode mode = current.impl->representationMode;
            if (mode == RepresentationMode::Unknown) {
                mode = eventUsesExpandedRepresentation(current.impl->session ? &*current.impl->session : nullptr,
                                                       batch.events.front().type)
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
            if (current.impl->retainedReplayThrough &&
                (!representedThrough || *representedThrough < *current.impl->retainedReplayThrough))
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
                if (!applyEventProjectionMetadata(*candidate, event, expandedType, error))
                    return std::nullopt;
                if (!synchronizing &&
                    ((representedThrough && event.sequence <= *representedThrough) ||
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
                const bool success = expanded ? applyExpanded(*candidate, expandedEvent, maximumRetainedDiagnostics, changes, error)
                                              : applyLegacy(*candidate, event, maximumRetainedDiagnostics, changes, error);
                if (!success)
                    return std::nullopt;
                candidate->visibleSequence = event.sequence;
                ++applied;
            }
            if (!synchronizing && candidate->visibleSequence)
                candidate->synchronizedThrough = candidate->visibleSequence;
            if (synchronizing)
                candidate->lastSynchronizationBatchSequence = batch.toSequence;
            if (!stateFits(*candidate, maximumBytes, error))
                return std::nullopt;
            return StateReduction{State{std::move(candidate)}, std::move(changes), batch.events.size(), applied, ignored};
        }

        std::optional<StateReduction> StateReducer::validateSynchronizationEvents(const State& staging,
                                                                                   const frontend::EventBatch& batch,
                                                                                   std::size_t maximumBytes,
                                                                                   bool allowLegacyV1,
                                                                                   std::string& error) {
            if (batch.events.empty() || batch.fromSequence != batch.events.front().sequence ||
                batch.toSequence != batch.events.back().sequence) {
                error = "frontend event batch bounds do not match its nonempty event list";
                return std::nullopt;
            }
            if (staging.impl->lastSynchronizationBatchSequence &&
                batch.fromSequence <= *staging.impl->lastSynchronizationBatchSequence) {
                error = "frontend replay event batches overlap or split one occurrence group";
                return std::nullopt;
            }
            const RepresentationMode mode = staging.impl->representationMode;
            if (mode == RepresentationMode::Unknown) {
                error = "synchronization staging state has no expected representation";
                return std::nullopt;
            }

            auto candidate = std::make_shared<StateStorage>(*staging.impl);
            if (!advanceRevision(*candidate, "validating a synchronization event batch", error))
                return std::nullopt;
            candidate->freshness = StateFreshness::Synchronizing;
            std::optional<frontend::SequenceNumber> representedThrough = staging.impl->synchronizedThrough;
            if (staging.impl->retainedReplayThrough &&
                (!representedThrough || *representedThrough < *staging.impl->retainedReplayThrough))
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
                if (!applyEventProjectionMetadata(*candidate, event, expandedType, error))
                    return std::nullopt;

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

        State StateReducer::withRevisionForTesting(const State& state, std::uint64_t revision) {
            auto candidate = std::make_shared<StateStorage>(*state.impl);
            candidate->revision = revision;
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
                    if (!appendDistinct(result.items,
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
                            if (!appendDistinct(turn.items,
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
                    if (!appendDistinct(result.turns,
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
