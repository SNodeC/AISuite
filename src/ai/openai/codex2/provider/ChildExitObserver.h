/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX2_PROVIDER_CHILDEXITOBSERVER_H
#define AI_OPENAI_CODEX2_PROVIDER_CHILDEXITOBSERVER_H

#include "core/eventreceiver/ReadEventReceiver.h"

#include <functional>
#include <sys/types.h>

namespace ai::openai::codex2::provider {

    class ChildExitObserver final : public core::eventreceiver::ReadEventReceiver {
    public:
        using Callback = std::function<void(ChildExitObserver*)>;

        static ChildExitObserver* observe(pid_t pid, Callback onExit, Callback onClosed);
        static void terminateAndReap(pid_t pid) noexcept;

        void detach() noexcept;

    private:
        ChildExitObserver(int pidfd, Callback onExit, Callback onClosed);
        ~ChildExitObserver() override;

        void readEvent() override;
        void unobservedEvent() override;
        void shutdownEvent(const core::ShutdownContext& context) override;

        Callback onExit_;
        Callback onClosed_;
    };

} // namespace ai::openai::codex2::provider

#endif
