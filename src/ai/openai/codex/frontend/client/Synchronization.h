#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_SYNCHRONIZATION_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_SYNCHRONIZATION_H

#include "ai/openai/codex/frontend/client/Export.h"
#include "ai/openai/codex/frontend/client/Results.h"
#include "ai/openai/codex/frontend/client/State.h"

#include <cstddef>

namespace ai::openai::codex::frontend::client {
    class Client;

    struct SynchronizationResult {
        frontend::SyncMode mode = frontend::SyncMode::Snapshot;
        frontend::SequenceNumber synchronizedThrough{};
        State state;
        std::size_t receivedEvents = 0;
        std::size_t appliedEvents = 0;
        std::size_t ignoredAlreadyAppliedEvents = 0;
        bool snapshotFallback = false;
    };

    class AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_EXPORT Synchronization {
    public:
        Synchronization(const Synchronization&) = delete;
        Synchronization(Synchronization&&) = delete;
        Synchronization& operator=(const Synchronization&) = delete;
        Synchronization& operator=(Synchronization&&) = delete;

        [[nodiscard]] Submission snapshot(CompletionHandler<SynchronizationResult> handler);
        [[nodiscard]] Submission replay(frontend::SequenceNumber after, CompletionHandler<SynchronizationResult> handler);

    private:
        friend class Client;
        explicit Synchronization(Client& owner) noexcept
            : client(&owner) {
        }
        Client* client;
    };
} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_SYNCHRONIZATION_H
