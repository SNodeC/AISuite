/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_CONTROLLER_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_CONTROLLER_H

#include "ai/openai/codex/frontend/client/Export.h"
#include "ai/openai/codex/frontend/client/Results.h"

#include <optional>
#include <string>

namespace ai::openai::codex::frontend::client {
    class Client;

    struct ControllerResult {
        std::optional<std::string> controllerSessionId;
        frontend::SessionRole role = frontend::SessionRole::Observer;
        bool ownedByThisClient = false;
    };

    class AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_EXPORT Controller {
    public:
        Controller(const Controller&) = delete;
        Controller(Controller&&) = delete;
        Controller& operator=(const Controller&) = delete;
        Controller& operator=(Controller&&) = delete;

        [[nodiscard]] Submission acquire(CompletionHandler<ControllerResult> handler);
        [[nodiscard]] Submission release(CompletionHandler<ControllerResult> handler);
        [[nodiscard]] bool ownedByThisClient() const noexcept;

    private:
        friend class Client;
        explicit Controller(Client& owner) noexcept
            : client(&owner) {
        }
        Client* client;
    };
} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_CONTROLLER_H
