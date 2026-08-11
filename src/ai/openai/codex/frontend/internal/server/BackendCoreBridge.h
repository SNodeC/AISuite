/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_INTERNAL_SERVER_BACKENDCOREBRIDGE_H
#define AI_OPENAI_CODEX_FRONTEND_INTERNAL_SERVER_BACKENDCOREBRIDGE_H

#include "ai/openai/codex/frontend/internal/server/ServerCore.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace ai::openai::codex::backend::detail {
    class BackendCoreRuntime;
}

namespace ai::openai::codex::frontend::internal::server {

    // The sole production adapter between BackendCore's typed session API and
    // ServerCore's canonical frontend semantics. It borrows BackendCoreRuntime
    // and owns exactly one observer subscription plus one BackendCore session
    // for each authenticated ServerCore session.
    class BackendCoreBridge final : public BackendPort {
    public:
        explicit BackendCoreBridge(backend::detail::BackendCoreRuntime& backend, std::size_t maximumResultBytes);
        BackendCoreBridge(const BackendCoreBridge&) = delete;
        BackendCoreBridge(BackendCoreBridge&&) = delete;
        BackendCoreBridge& operator=(const BackendCoreBridge&) = delete;
        BackendCoreBridge& operator=(BackendCoreBridge&&) = delete;
        ~BackendCoreBridge() override;

        void bind(ServerCore& core) noexcept override;
        void unbind(ServerCore& core) noexcept override;
        // FrontendService installs this private lifetime binding immediately
        // after constructing its shared ServerCore and before starting the
        // BackendCore observer. No callback may retain a naked ServerCore.
        void bindLifetime(const std::shared_ptr<ServerCore>& core);
        void start();
        void close() noexcept;

        [[nodiscard]] bool providerReady() const noexcept override;
        [[nodiscard]] model::CanonicalSnapshot snapshot() const override;
        [[nodiscard]] BackendSubmitStatus submit(BackendInvocation invocation) override;
        [[nodiscard]] bool performProviderLifecycleAction(ProviderLifecycleAction action) override;
        [[nodiscard]] bool sessionOpened(const FrontendSessionToken& token, const FrontendPrincipal& principal) override;
        void sessionClosed(const FrontendSessionToken& token) noexcept override;

    private:
        friend struct BackendCoreBridgeTestAccess;
        class State;
        std::shared_ptr<State> state;
    };

    // Permanent non-installed conformance seam for the controller transaction
    // invariant. Production and tests exercise the same validation authority.
    struct BackendCoreBridgeTestAccess {
        [[nodiscard]] static bool controllerResultValid(generated::MethodId method,
                                                        std::uint64_t expectedBackendSession,
                                                        std::optional<std::uint64_t> reportedBackendController,
                                                        bool reportedControllerRole) noexcept;
        [[nodiscard]] static bool
        closeBackendSession(BackendCoreBridge& bridge, const model::SessionIdentity& frontendSession, std::string reason) noexcept;
    };

} // namespace ai::openai::codex::frontend::internal::server

#endif // AI_OPENAI_CODEX_FRONTEND_INTERNAL_SERVER_BACKENDCOREBRIDGE_H
