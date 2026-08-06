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
    struct ProviderChanged {
        ProviderState state;
    };
    struct ControllerChanged {
        ControllerState state;
    };
    struct EntityChanged {
        std::string domain;
        std::string id;
    };
    struct ItemContentReplaced {
        std::string itemId;
        std::string channel;
    };
    struct DiagnosticChanged {
        DiagnosticState diagnostic;
    };

    using Change = std::variant<StateReplacedChange,
                                CursorAdvancedChange,
                                ProviderChanged,
                                ControllerChanged,
                                EntityChanged,
                                ItemContentReplaced,
                                DiagnosticChanged>;

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
