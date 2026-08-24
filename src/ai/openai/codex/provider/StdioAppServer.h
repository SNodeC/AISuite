/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_PROVIDER_STDIOAPPSERVER_H
#define AI_OPENAI_CODEX_PROVIDER_STDIOAPPSERVER_H

#include "ai/openai/codex/bridge/Endpoint.h"
#include "ai/openai/codex/protocol/JsonLineFramer.h"

#include <cstddef>
#include <functional>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace core::pipe {
    class PipeSink;
    class PipeSource;
} // namespace core::pipe

namespace ai::openai::codex::bridge {
    class CodexBridge;
}

namespace ai::openai::codex::provider {

    class ChildExitObserver;

    struct StdioAppServerOptions {
        std::string executable = "codex";
        std::vector<std::string> arguments{"app-server"};
        std::vector<std::pair<std::string, std::string>> environment;
        std::size_t maximumFrameBytes = 64 * 1024 * 1024;
        std::size_t maximumQueuedInputBytes = 64 * 1024 * 1024;
        std::function<void(int)> onExit;
    };

    class StdioAppServer final : public bridge::AppServerEndpoint {
    public:
        StdioAppServer(bridge::CodexBridge& bridge, StdioAppServerOptions options);
        ~StdioAppServer() override;

        StdioAppServer(const StdioAppServer&) = delete;
        StdioAppServer& operator=(const StdioAppServer&) = delete;

        bool start();
        void stop();
        bool send(const nlohmann::json& message) override;
        bool isConnected() const noexcept override;
        pid_t pid() const noexcept;

    private:
        void receiveStdout(const char* data, std::size_t length);
        void receiveStderr(const char* data, std::size_t length);
        void transportClosed(std::string reason);
        bool observeChild();
        void childExitReady(ChildExitObserver* observer);
        void childObserverClosed(ChildExitObserver* observer) noexcept;
        void detachChildObserver() noexcept;
        void detachCallbacks();

        bridge::CodexBridge& bridge_;
        StdioAppServerOptions options_;
        protocol::JsonLineFramer stdoutFramer_;
        core::pipe::PipeSource* stdinWriter_ = nullptr;
        core::pipe::PipeSink* stdoutReader_ = nullptr;
        core::pipe::PipeSink* stderrReader_ = nullptr;
        ChildExitObserver* childObserver_ = nullptr;
        pid_t childPid_ = -1;
        bool connected_ = false;
        bool stopping_ = false;
        std::string stderrBuffer_;
    };

} // namespace ai::openai::codex::provider

#endif
