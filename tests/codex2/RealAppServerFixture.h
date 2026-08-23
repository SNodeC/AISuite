/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef TESTS_CODEX2_REALAPPSERVERFIXTURE_H
#define TESTS_CODEX2_REALAPPSERVERFIXTURE_H

#include "CommunicationTrace.h"
#include "ai/openai/codex2/bridge/CodexBridge.h"
#include "ai/openai/codex2/provider/StdioAppServer.h"

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace tests::codex2 {

    class RealAppServerFixture {
    public:
        using Callback = std::function<void()>;
        using Failure = std::function<void(std::string)>;

        RealAppServerFixture(ai::openai::codex2::bridge::CodexBridge& bridge,
                             std::string executable,
                             std::string label,
                             Callback ready,
                             Failure failure)
            : bridge_(bridge)
            , label_(std::move(label))
            , ready_(std::move(ready))
            , failure_(std::move(failure))
            , codexHome_(makeCodexHome())
            , endpoint_(bridge_, options(std::move(executable), codexHome_, [this](int status) {
                traceCommunication(label_, "real-app-server", "lifecycle", "process-exited", {{"status", status}});
                if (!stopping_ && failure_) {
                    failure_("real app-server exited unexpectedly with status " + std::to_string(status));
                }
            })) {
            bridge_.onProviderLifecycle([this](bool connected) {
                traceCommunication(label_, "real-app-server", "lifecycle", connected ? "connected" : "disconnected");
                if (connected && !initialized_) {
                    initialize();
                }
            });
        }

        ~RealAppServerFixture() {
            stop();
            bridge_.onProviderLifecycle({});
            std::error_code error;
            std::filesystem::remove_all(codexHome_, error);
        }

        RealAppServerFixture(const RealAppServerFixture&) = delete;
        RealAppServerFixture& operator=(const RealAppServerFixture&) = delete;

        bool start() {
            const bool started = endpoint_.start();
            traceCommunication(label_,
                               "real-app-server",
                               "lifecycle",
                               started ? "spawned" : "spawn-failed",
                               {{"pid", endpoint_.pid()}, {"codexHome", codexHome_}});
            return started;
        }

        void stop() noexcept {
            if (stopping_) {
                return;
            }
            stopping_ = true;
            endpoint_.stop();
        }

        const std::string& codexHome() const noexcept {
            return codexHome_;
        }

    private:
        static std::string makeCodexHome() {
            static unsigned counter = 0;
            const std::filesystem::path path =
                std::filesystem::temp_directory_path() /
                ("codex2-real-appserver-" + std::to_string(::getpid()) + '-' + std::to_string(++counter));
            std::filesystem::create_directories(path);
            return path.string();
        }

        static ai::openai::codex2::provider::StdioAppServerOptions options(std::string executable,
                                                                           const std::string& codexHome,
                                                                           std::function<void(int)> onExit) {
            ai::openai::codex2::provider::StdioAppServerOptions result;
            result.executable = std::move(executable);
            result.environment.emplace_back("CODEX_HOME", codexHome);
            result.onExit = std::move(onExit);
            return result;
        }

        void initialize() {
            using Initialize = ai::openai::codex2::generated::client_requests::Initialize;
            Initialize::Params parameters({
                {"clientInfo", {{"name", "codex2_transport_test"}, {"title", "Codex2 Transport Test"}, {"version", "1"}}},
                {"capabilities", {{"experimentalApi", true}, {"requestAttestation", false}}},
            });
            bridge_.initialize(parameters, [this](Initialize::Response& response) {
                traceCommunication(label_, "real-app-server", "provider-to-bridge", "initialize-response", response.getRaw());
                if (!response) {
                    if (failure_) {
                        failure_("real app-server initialize failed: " +
                                 response.jsonRpcErrorMessage().value_or("unknown error"));
                    }
                    return;
                }
                if (!bridge_.initialized()) {
                    if (failure_) {
                        failure_("real app-server rejected initialized notification");
                    }
                    return;
                }
                initialized_ = true;
                bridge_.setAppServerReady();
                traceCommunication(label_, "real-app-server", "lifecycle", "ready");
                if (ready_) {
                    ready_();
                }
            });
        }

        ai::openai::codex2::bridge::CodexBridge& bridge_;
        std::string label_;
        Callback ready_;
        Failure failure_;
        std::string codexHome_;
        ai::openai::codex2::provider::StdioAppServer endpoint_;
        bool initialized_ = false;
        bool stopping_ = false;
    };

} // namespace tests::codex2

#endif
