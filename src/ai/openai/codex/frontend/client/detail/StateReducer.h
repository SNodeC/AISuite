#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_DETAIL_STATEREDUCER_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_DETAIL_STATEREDUCER_H

#include "ai/openai/codex/frontend/client/Changes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace ai::openai::codex::frontend::client::detail {

    struct StateReduction {
        State state;
        std::vector<Change> changes;
        std::size_t receivedEvents = 0;
        std::size_t appliedEvents = 0;
        std::size_t ignoredAlreadyAppliedEvents = 0;
    };

    class StateReducer {
    public:
        [[nodiscard]] static State initial();
        [[nodiscard]] static std::optional<State> stale(const State& state, std::size_t maximumBytes, std::string& error);
        // Builds a private synchronization cursor in the representation
        // selected by the new session.  A projection-refresh replay must not
        // be reduced into state retained from a different projection.
        [[nodiscard]] static std::optional<State>
        synchronizationStaging(const SessionInfo& session,
                               std::optional<frontend::SequenceNumber> resumeAfter,
                               std::size_t maximumBytes,
                               bool allowLegacyV1,
                               std::string& error,
                               std::optional<ProjectionFingerprintMetadata> projectionFingerprint = std::nullopt);
        // Rebinds a retained compatible projection to the new physical
        // session before any replay event is exposed to applications.
        [[nodiscard]] static std::optional<State>
        beginSynchronization(const State& current,
                             const SessionInfo& session,
                             std::size_t maximumBytes,
                             std::string& error,
                             std::optional<ProjectionFingerprintMetadata> projectionFingerprint = std::nullopt);
        [[nodiscard]] static std::optional<StateReduction>
        snapshot(const State& current,
                 const frontend::Snapshot& snapshot,
                 const SessionInfo& session,
                 std::size_t maximumBytes,
                 std::size_t maximumRetainedDiagnostics,
                 bool allowLegacyV1,
                 std::string& error,
                 std::optional<ProjectionFingerprintMetadata> projectionFingerprint = std::nullopt);
        [[nodiscard]] static std::optional<StateReduction> events(const State& current,
                                                                  const frontend::EventBatch& batch,
                                                                  bool synchronizing,
                                                                  std::size_t maximumBytes,
                                                                  std::size_t maximumRetainedDiagnostics,
                                                                  bool allowLegacyV1,
                                                                  std::string& error);
        // Validates a replay against a synchronization staging cursor without
        // applying its projected domain data.  This is used only while an
        // incompatible retained projection is awaiting snapshot replacement.
        [[nodiscard]] static std::optional<StateReduction> validateSynchronizationEvents(
            const State& staging, const frontend::EventBatch& batch, std::size_t maximumBytes, bool allowLegacyV1, std::string& error);
        [[nodiscard]] static std::optional<StateReduction>
        synchronized(const State& current,
                     frontend::SequenceNumber sequence,
                     const SessionInfo& session,
                     std::size_t maximumBytes,
                     std::string& error,
                     std::optional<ProjectionFingerprintMetadata> projectionFingerprint = std::nullopt);
        [[nodiscard]] static frontend::Json serializeForTesting(const State& state) noexcept;
        [[nodiscard]] static frontend::Json serializeChangesForTesting(std::span<const Change> changes) noexcept;
        [[nodiscard]] static State withRevisionForTesting(const State& state, std::uint64_t revision);
        [[nodiscard]] static std::optional<ThreadState> decodeThreadState(const frontend::Json& value, std::string& error);
        [[nodiscard]] static std::optional<TurnState> decodeTurnState(const frontend::Json& value, std::string& error);
        [[nodiscard]] static std::optional<ThreadResultState> decodeThreadResultState(const frontend::Json& value, std::string& error);
        [[nodiscard]] static std::optional<TurnResultState> decodeTurnResultState(const frontend::Json& value, std::string& error);
    };

} // namespace ai::openai::codex::frontend::client::detail

#endif
