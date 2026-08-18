#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_CHANGES_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_CHANGES_H

#include "ai/openai/codex/frontend/client/Results.h"
#include "ai/openai/codex/frontend/client/State.h"

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ai::openai::codex::frontend::client {

    enum class UpdateCause {
        InitialSnapshot,
        InitialReplay,
        ReconnectReplay,
        ProjectionRefresh,
        SnapshotFallback,
        ExplicitSnapshot,
        ExplicitReplay,
        Live,
        ConnectionBecameStale,
        SynchronizationCompleted,
    };

    struct StateReplacedChange {};
    struct CursorAdvancedChange {
        frontend::SequenceNumber sequence{};
    };
    struct ProviderUpdatedChange {
        ProviderState state;
    };
    struct ControllerUpdatedChange {
        ControllerState state;
    };
    struct SessionsUpdatedChange {};
    struct ThreadListUpdatedChange {};
    struct ThreadUpsertedChange {
        typed::ThreadId threadId;
    };
    struct ThreadRemovedChange {
        typed::ThreadId threadId;
    };
    struct TurnUpsertedChange {
        typed::TurnId turnId;
    };
    // Provider item IDs are not globally unique. Consumers must use the
    // available parent scope when resolving either item-change record.
    struct ItemUpsertedChange {
        typed::ItemId itemId;
        std::optional<typed::ThreadId> threadId;
        std::optional<typed::TurnId> turnId;
    };
    struct ItemContentReplacedChange {
        typed::ItemId itemId;
        ItemContentChannel channel = ItemContentChannel::AgentText;
        std::optional<typed::ThreadId> threadId;
        std::optional<typed::TurnId> turnId;
    };
    struct PendingRequestsUpdatedChange {};
    struct AccountUpdatedChange {};
    struct ModelsUpdatedChange {};
    struct ConfigurationUpdatedChange {};
    struct ProcessUpdatedChange {
        ProcessHandle processHandle;
    };
    struct FilesystemWatchUpdatedChange {
        typed::FsWatchId watchId;
    };
    struct FuzzySearchUpdatedChange {
        FuzzySearchSessionId sessionId;
    };
    struct ReviewsUpdatedChange {};
    struct IntegrationsUpdatedChange {};
    struct PluginsUpdatedChange {};
    struct SkillsUpdatedChange {};
    struct McpUpdatedChange {};
    struct PlatformUpdatedChange {};
    struct NoticeAddedChange {
        std::optional<std::uint64_t> occurrence;
    };
    struct ActivityUpdatedChange {
        ActivityKey key;
    };
    struct CapacityUpdatedChange {};
    struct DiagnosticUpdatedChange {
        std::optional<std::uint64_t> received;
    };
    struct CompatibilityExtensionChange {
        std::string type;
    };

    using Change = std::variant<StateReplacedChange,
                                CursorAdvancedChange,
                                ProviderUpdatedChange,
                                ControllerUpdatedChange,
                                SessionsUpdatedChange,
                                ThreadListUpdatedChange,
                                ThreadUpsertedChange,
                                ThreadRemovedChange,
                                TurnUpsertedChange,
                                ItemUpsertedChange,
                                ItemContentReplacedChange,
                                PendingRequestsUpdatedChange,
                                AccountUpdatedChange,
                                ModelsUpdatedChange,
                                ConfigurationUpdatedChange,
                                ProcessUpdatedChange,
                                FilesystemWatchUpdatedChange,
                                FuzzySearchUpdatedChange,
                                ReviewsUpdatedChange,
                                IntegrationsUpdatedChange,
                                PluginsUpdatedChange,
                                SkillsUpdatedChange,
                                McpUpdatedChange,
                                PlatformUpdatedChange,
                                NoticeAddedChange,
                                ActivityUpdatedChange,
                                CapacityUpdatedChange,
                                DiagnosticUpdatedChange,
                                CompatibilityExtensionChange>;

    struct StateUpdate {
        State state;
        UpdateCause cause = UpdateCause::Live;
        std::optional<frontend::SequenceNumber> fromSequence;
        std::optional<frontend::SequenceNumber> toSequence;
        std::vector<Change> changes;
    };

    struct ConnectionStateChange {
        ConnectionState previous = ConnectionState::Disconnected;
        ConnectionState current = ConnectionState::Disconnected;
        std::optional<Error> error;
    };

    struct SynchronizationInfo {
        frontend::SyncMode mode = frontend::SyncMode::Snapshot;
        frontend::SequenceNumber synchronizedThrough{};
        State state;
        bool reconnect = false;
        bool snapshotFallback = false;
    };

    struct Diagnostic {
        enum class Severity { Debug, Information, Warning, Error };
        Severity severity = Severity::Information;
        std::string message;
        std::optional<Error> error;
    };

    struct ClientCallbacks {
        std::function<void(const ConnectionStateChange&)> onConnectionStateChanged;
        std::function<void(const StateUpdate&)> onStateUpdated;
        std::function<void(const SynchronizationInfo&)> onSynchronized;
        std::function<void(frontend::SequenceNumber)> onCursorAdvanced;
        std::function<void(const frontend::ServerMessage&)> onProtocolMessage;
        std::function<void(const Diagnostic&)> onDiagnostic;
    };

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_CHANGES_H
