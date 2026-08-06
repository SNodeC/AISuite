#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_STATE_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_STATE_H

#include "ai/openai/codex/frontend/client/StateTypes.h"
#include "ai/openai/codex/frontend/client/Types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ai::openai::codex::frontend::client {

    class State {
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
        [[nodiscard]] const Projected<ProviderState>& provider() const noexcept;
        [[nodiscard]] const Projected<ControllerState>& controller() const noexcept;
        [[nodiscard]] std::span<const ThreadState> threads() const noexcept;
        [[nodiscard]] const ThreadState* thread(std::string_view id) const noexcept;
        [[nodiscard]] std::span<const TurnState> turns() const noexcept;
        [[nodiscard]] const TurnState* turn(std::string_view id) const noexcept;
        [[nodiscard]] std::span<const ItemState> items() const noexcept;
        [[nodiscard]] const ItemState* item(std::string_view id) const noexcept;
        [[nodiscard]] std::span<const PendingRequestState> pendingRequests() const noexcept;

    private:
        friend class Client;
        struct Impl;
        explicit State(std::shared_ptr<const Impl> implementation) noexcept;
        std::shared_ptr<const Impl> impl;
    };

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_STATE_H
