/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_BRIDGE_ENDPOINT_H
#define AI_OPENAI_CODEX_BRIDGE_ENDPOINT_H

#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace ai::openai::codex::bridge {

    class FrontendEndpoint {
    public:
        virtual ~FrontendEndpoint() = default;
        virtual bool send(const nlohmann::json& message) = 0;
        virtual void close(std::string_view reason) = 0;
    };

    class AppServerEndpoint {
    public:
        virtual ~AppServerEndpoint() = default;
        virtual bool send(const nlohmann::json& message) = 0;
        virtual bool isConnected() const noexcept = 0;
    };

} // namespace ai::openai::codex::bridge

#endif
