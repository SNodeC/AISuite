#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_DETAIL_CLIENTTESTACCESS_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_DETAIL_CLIENTTESTACCESS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ai::openai::codex::frontend::client {

    class Client;
    class State;

    namespace detail {

        struct StateStorage;

        struct ClientTestAccess {
            static void setNextRequest(Client& client, std::uint64_t next) noexcept;
            static void setNextConnectionGeneration(Client& client, std::uint64_t next) noexcept;
            static void setSynchronizationCounts(Client& client, std::size_t received, std::size_t applied, std::size_t ignored) noexcept;
            [[nodiscard]] static bool
            tryAccumulateSynchronizationCounts(Client& client, std::size_t received, std::size_t applied, std::size_t ignored) noexcept;
            [[nodiscard]] static std::array<std::size_t, 3> synchronizationCounts(const Client& client) noexcept;
            static void failNextHelloConstruction(Client& client) noexcept;
            static void failAfterNextDispatch(Client& client) noexcept;
            [[nodiscard]] static bool rejectInvalidSynchronizationAdapterResultWithThrowingCallback(Client& client) noexcept;
            [[nodiscard]] static std::size_t erasedTransientBytes(const Client& client) noexcept;
            [[nodiscard]] static std::size_t verifiedMovedFromStringScrubs(const Client& client) noexcept;
            [[nodiscard]] static bool shortStringStorageScrubbed() noexcept;
            [[nodiscard]] static State adoptStateStorage(Client& client, std::shared_ptr<const StateStorage> storage) noexcept;
            [[nodiscard]] static std::shared_ptr<const StateStorage> stateStorage(const State& state) noexcept;
        };

    } // namespace detail

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_DETAIL_CLIENTTESTACCESS_H
