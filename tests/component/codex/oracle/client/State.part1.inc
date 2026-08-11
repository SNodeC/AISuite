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
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <limits>
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

        bool
        decodeProjectionMetadata(const frontend::Json& extensions, std::optional<ProjectionMetadataState>& result, std::string& error) {
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

        void applyEventProjectionOmission(detail::StateStorage& state, frontend::ExpandedEventType type, const std::string& path) {
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
                case ThreadListUpdated:
                    addProjectedOmission(state.threadList, path);
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

        bool applySnapshotProjectionMetadata(detail::StateStorage& state, const frontend::Json& extensions, std::string& error) {
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
                                          bool& changed,
                                          std::string& error) {
            changed = false;
            std::optional<ProjectionMetadataState> metadata;
            if (!decodeProjectionMetadata(event.extensions, metadata, error))
                return false;
            if (!metadata)
                return true;
            changed = true;
            mergePaths(state.projectionMetadata.omittedFields, metadata->omittedFields);
            mergePaths(state.projectionMetadata.redactedFields, metadata->redactedFields);
            if (expandedType) {
                for (const std::string& path : metadata->omittedFields)
                    applyEventProjectionOmission(state, *expandedType, path);
            }
            return true;
        }

        bool requireArrayMember(const frontend::Json& object, std::string_view key, const frontend::Json*& value, std::string& error) {
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

        bool requireObjectMember(const frontend::Json& object, std::string_view key, const frontend::Json*& value, std::string& error) {
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

        bool requireStringMember(const frontend::Json& object, std::string_view key, bool nonempty, std::string& error) {
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

        bool
        optionalMemberHasType(const frontend::Json& object, std::string_view key, frontend::Json::value_t expected, std::string& error) {
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

        bool decodeTruncation(const frontend::Json& value, TruncationMetadata& result, std::string& error, bool strict = true) {
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
            result.extensions = extensionsOf(value, {"truncated", "omittedFields", "omittedEntries", "droppedBytes"});
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
            result.extensions =
                extensionsOf(value, {"lifecycle", "generation", "desiredRunning", "recovery", "lastError", "initialization"});
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
            result.extensions = extensionsOf(value, {"hasLoadedPage", "complete", "pagesLoaded", "nextCursor", "backwardsCursor", "stamp"});
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
