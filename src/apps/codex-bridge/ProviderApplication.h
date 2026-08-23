/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BRIDGE_PROVIDERAPPLICATION_H
#define APPS_CODEX_BRIDGE_PROVIDERAPPLICATION_H

#include <memory>
#include <sys/types.h>

namespace ai::openai::codex2::bridge {
    class CodexBridge;
}

namespace apps::codex_bridge {

    class Configuration;

    class ProviderApplication {
    public:
        class Runtime;

        ProviderApplication(ai::openai::codex2::bridge::CodexBridge& bridge, const Configuration& configuration);
        ~ProviderApplication();

        ProviderApplication(const ProviderApplication&) = delete;
        ProviderApplication& operator=(const ProviderApplication&) = delete;

        bool start();
        void stop() noexcept;
        pid_t appServerPid() const noexcept;

    private:
        void providerLifecycleChanged(bool connected);

        ai::openai::codex2::bridge::CodexBridge& bridge_;
        std::unique_ptr<Runtime> runtime_;
    };

} // namespace apps::codex_bridge

#endif
