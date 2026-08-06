/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/ClientAuthentication.h"

#include <stdexcept>
#include <utility>

namespace apps::codex_backend_client {
    namespace frontend = ai::openai::codex::frontend;
    namespace sdk = ai::openai::codex::frontend::client;

    void ClientAuthentication::prepare(bool verifiedLocalUnix) noexcept {
        preparedTransport = verifiedLocalUnix ? PreparedTransport::VerifiedLocalUnix : PreparedTransport::Remote;
    }

    sdk::AuthenticationContext ClientAuthentication::provide(std::optional<frontend::BearerCredential> bearerCredential,
                                                             std::string verifiedLocalContinuityKey) {
        const PreparedTransport selected = std::exchange(preparedTransport, PreparedTransport::None);
        if (selected == PreparedTransport::None) {
            throw std::runtime_error("frontend transport authentication was not prepared");
        }
        if (bearerCredential) {
            return {std::move(*bearerCredential), "bearer-profile:configured"};
        }
        if (selected != PreparedTransport::VerifiedLocalUnix) {
            throw std::runtime_error("remote frontend transport requires --bearer-token-file");
        }
        return {frontend::NoCredential{}, std::move(verifiedLocalContinuityKey)};
    }

} // namespace apps::codex_backend_client
