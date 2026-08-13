#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_STATE_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_STATE_H

#include "ai/openai/codex/frontend/client/Export.h"
#include "ai/openai/codex/frontend/client/StateTypes.h"
#include "ai/openai/codex/frontend/client/Types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ai::openai::codex::frontend::client {

    namespace detail {
        struct StateStorage;
    } // namespace detail

    class AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_EXPORT State {
    public:
        State();
        State(const State&) noexcept = default;
        State(State&&) noexcept = default;
        State& operator=(const State&) noexcept = default;
        State& operator=(State&&) noexcept = default;
        ~State() = default;

        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] StateFreshness freshness() const noexcept;
        [[nodiscard]] RepresentationMode representationMode() const noexcept;
        [[nodiscard]] std::optional<frontend::SequenceNumber> visibleSequence() const noexcept;
        [[nodiscard]] std::optional<frontend::SequenceNumber> synchronizedThrough() const noexcept;
        [[nodiscard]] std::optional<SessionInfo> session() const;
        [[nodiscard]] const BackendCursorState& backendCursor() const noexcept;
        [[nodiscard]] const ProjectionMetadataState& projectionMetadata() const noexcept;
        [[nodiscard]] const std::optional<ProjectionFingerprintMetadata>& projectionFingerprintMetadata() const noexcept;
        [[nodiscard]] const Projected<ProviderState>& provider() const noexcept;
        [[nodiscard]] const Projected<ControllerState>& controller() const noexcept;
        [[nodiscard]] const Projected<std::vector<SessionState>>& sessions() const noexcept;
        [[nodiscard]] const SessionState* session(const FrontendSessionId& id) const noexcept;
        [[nodiscard]] const SessionState* session(std::string_view id) const noexcept;
        [[nodiscard]] const Projected<ThreadListState>& threadList() const noexcept;
        [[nodiscard]] bool hasThreadProjection() const noexcept;
        [[nodiscard]] std::span<const ThreadState> threads() const noexcept;
        [[nodiscard]] const ThreadState* thread(const typed::ThreadId& id) const noexcept;
        [[nodiscard]] const ThreadState* thread(std::string_view id) const noexcept;
        [[nodiscard]] bool hasTurnProjection() const noexcept;
        [[nodiscard]] std::span<const TurnState> turns() const noexcept;
        [[nodiscard]] const TurnState* turn(const typed::TurnId& id) const noexcept;
        [[nodiscard]] const TurnState* turn(std::string_view id) const noexcept;
        [[nodiscard]] bool hasItemProjection() const noexcept;
        [[nodiscard]] std::span<const ItemState> items() const noexcept;
        [[nodiscard]] const ItemState* item(const typed::ItemId& id) const noexcept;
        [[nodiscard]] const ItemState* item(std::string_view id) const noexcept;
        [[nodiscard]] bool hasPendingRequestProjection() const noexcept;
        [[nodiscard]] std::span<const PendingRequestState> pendingRequests() const noexcept;
        [[nodiscard]] const PendingRequestState* pendingRequest(const PendingRequestId& id) const noexcept;
        [[nodiscard]] const Projected<AccountState>& accounts() const noexcept;
        [[nodiscard]] const Projected<ModelsState>& models() const noexcept;
        [[nodiscard]] const Projected<ConfigurationState>& configuration() const noexcept;
        [[nodiscard]] const Projected<ProcessCollectionState>& processes() const noexcept;
        [[nodiscard]] const ProcessState* process(const ProcessHandle& handle) const noexcept;
        [[nodiscard]] const ProcessState* process(std::string_view handle) const noexcept;
        [[nodiscard]] const Projected<FilesystemWatchCollectionState>& filesystemWatches() const noexcept;
        [[nodiscard]] const FilesystemWatchState* filesystemWatch(const typed::FsWatchId& id) const noexcept;
        [[nodiscard]] const Projected<FuzzySearchCollectionState>& fuzzySearches() const noexcept;
        [[nodiscard]] const FuzzySearchState* fuzzySearch(const FuzzySearchSessionId& id) const noexcept;
        [[nodiscard]] const Projected<PermissionProfilesState>& permissionProfiles() const noexcept;
        [[nodiscard]] const Projected<ReviewsState>& reviews() const noexcept;
        [[nodiscard]] const Projected<AppsState>& apps() const noexcept;
        [[nodiscard]] const Projected<ExternalAgentsState>& externalAgents() const noexcept;
        [[nodiscard]] const Projected<HooksState>& hooks() const noexcept;
        [[nodiscard]] const Projected<MarketplaceState>& marketplace() const noexcept;
        [[nodiscard]] const Projected<PluginsState>& plugins() const noexcept;
        [[nodiscard]] const Projected<SkillsState>& skills() const noexcept;
        [[nodiscard]] const Projected<McpState>& mcp() const noexcept;
        [[nodiscard]] const Projected<WindowsSandboxState>& windowsSandbox() const noexcept;
        [[nodiscard]] const Projected<PlatformState>& platform() const noexcept;
        [[nodiscard]] const Projected<NoticeCollectionState>& notices() const noexcept;
        [[nodiscard]] const Projected<ActivityCollectionState>& activities() const noexcept;
        [[nodiscard]] const ActivityState* activity(const ActivityKey& key) const noexcept;
        [[nodiscard]] const Projected<CapacityState>& capacity() const noexcept;
        [[nodiscard]] const Projected<TruncationMetadata>& truncation() const noexcept;
        [[nodiscard]] const Projected<DiagnosticCollectionState>& diagnostics() const noexcept;
        [[nodiscard]] std::optional<ProviderOperationCollectionSemanticState> providerOperations() const;
        [[nodiscard]] std::optional<ConversationSemanticState> conversations() const;
        [[nodiscard]] std::optional<ProviderDomainSemanticState> filesystemProvider() const;
        [[nodiscard]] std::optional<CapacityProvenanceState> capacityProvenance() const;
        [[nodiscard]] const frontend::Json& compatibilityExtensions() const noexcept;

    private:
        friend class Client;
        AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_NO_EXPORT explicit State(std::shared_ptr<const detail::StateStorage> implementation) noexcept;
        std::shared_ptr<const detail::StateStorage> impl;
    };

    [[nodiscard]] AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_EXPORT std::optional<TurnTokenUsageView> tokenUsageView(const TurnState& turn);
    [[nodiscard]] AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_EXPORT std::optional<TurnFailureView> failureView(const TurnState& turn);
    [[nodiscard]] AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_EXPORT ThreadRealtimeSemanticView
    realtimeSemanticView(const ThreadRealtimeState& realtime);
    [[nodiscard]] AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_EXPORT std::optional<ItemSemanticView> itemSemanticView(const ItemState& item);
    [[nodiscard]] AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_EXPORT PendingRequestPresentationView
    pendingRequestPresentation(const PendingRequestState& request);

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_STATE_H
