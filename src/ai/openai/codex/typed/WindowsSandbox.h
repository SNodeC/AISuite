/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_WINDOWSSANDBOX_H
#define AI_OPENAI_CODEX_TYPED_WINDOWSSANDBOX_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/Configuration.h"
#include "ai/openai/codex/typed/Results.h"
#include "ai/openai/codex/typed/Types.h"

#include <compare>
#include <functional>
#include <string>
#include <vector>

namespace ai::openai::codex::typed {

    // Protocol string enums retain unknown future values. Their decoders add
    // a ForwardCompatibility diagnostic when isKnown() is false.
    struct WindowsSandboxReadiness {
        std::string value;

        [[nodiscard]] static WindowsSandboxReadiness ready() {
            return {"ready"};
        }

        [[nodiscard]] static WindowsSandboxReadiness notConfigured() {
            return {"notConfigured"};
        }

        [[nodiscard]] static WindowsSandboxReadiness updateRequired() {
            return {"updateRequired"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "ready" || value == "notConfigured" || value == "updateRequired";
        }

        auto operator<=>(const WindowsSandboxReadiness&) const = default;
    };

    struct WindowsSandboxReadinessResponse {
        WindowsSandboxReadiness status;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const WindowsSandboxReadinessResponse&) const = default;
    };

    struct WindowsSandboxSetupStartParams {
        OptionalNullable<AbsolutePath> cwd;
        WindowsSandboxSetupMode mode;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const WindowsSandboxSetupStartParams&) const = default;
    };

    struct WindowsSandboxSetupStartResponse {
        bool started = false;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const WindowsSandboxSetupStartResponse&) const = default;
    };

    class WindowsSandbox {
    public:
        WindowsSandbox(const WindowsSandbox&) = delete;
        WindowsSandbox(WindowsSandbox&&) = delete;
        WindowsSandbox& operator=(const WindowsSandbox&) = delete;
        WindowsSandbox& operator=(WindowsSandbox&&) = delete;

        Submission checkReadiness(CompletionHandler<WindowsSandboxReadinessResponse> handler);
        Submission startSetup(WindowsSandboxSetupStartParams params, CompletionHandler<WindowsSandboxSetupStartResponse> handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit WindowsSandbox(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_WINDOWSSANDBOX_H
