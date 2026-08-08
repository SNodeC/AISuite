/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_PROVIDER_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_PROVIDER_H

#include "ai/openai/codex/frontend/client/Export.h"
#include "ai/openai/codex/frontend/client/Results.h"

namespace ai::openai::codex::frontend::client {
    class Client;

    class AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_EXPORT Provider {
    public:
        Provider(const Provider&) = delete;
        Provider(Provider&&) = delete;
        Provider& operator=(const Provider&) = delete;
        Provider& operator=(Provider&&) = delete;

        [[nodiscard]] Submission start(DoneHandler handler);
        [[nodiscard]] Submission stop(DoneHandler handler);
        [[nodiscard]] Submission restart(DoneHandler handler);

    private:
        friend class Client;
        explicit Provider(Client& owner) noexcept
            : client(&owner) {
        }
        Client* client;
    };
} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_PROVIDER_H
