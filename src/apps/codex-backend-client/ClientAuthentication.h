/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_CLIENT_CLIENTAUTHENTICATION_H
#define APPS_CODEX_BACKEND_CLIENT_CLIENTAUTHENTICATION_H

#include "ai/openai/codex/frontend/client/Types.h"

#include <optional>
#include <string>

namespace apps::codex_backend_client {

    // Bridges application-owned physical-transport trust into the SDK's
    // transport-neutral CredentialProvider immediately before Hello is built.
    // One prepared selection is consumed by exactly one physical connection.
    class ClientAuthentication {
    public:
        void prepare(bool verifiedLocalUnix) noexcept;

        [[nodiscard]] ai::openai::codex::frontend::client::AuthenticationContext
        provide(std::optional<ai::openai::codex::frontend::BearerCredential> bearerCredential, std::string verifiedLocalContinuityKey);

    private:
        enum class PreparedTransport {
            None,
            VerifiedLocalUnix,
            Remote,
        };

        PreparedTransport preparedTransport = PreparedTransport::None;
    };

} // namespace apps::codex_backend_client

#endif // APPS_CODEX_BACKEND_CLIENT_CLIENTAUTHENTICATION_H
