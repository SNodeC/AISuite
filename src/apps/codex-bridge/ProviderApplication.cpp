/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-bridge/ProviderApplication.h"

#include "ai/openai/codex/bridge/CodexBridge.h"
#include "ai/openai/codex/provider/StdioAppServer.h"
#include "ai/openai/codex/protocol/RuntimePaths.h"
#include "apps/codex-bridge/Configuration.h"
#include "core/SNodeC.h"

#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
#include "ai/openai/codex/provider/WebSocketAppServer.h"
#include "net/in/stream/legacy/SocketClient.h"
#include "net/in6/stream/legacy/SocketClient.h"
#include "net/un/stream/legacy/SocketClient.h"
#endif

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace apps::codex_bridge {

    namespace codex = ai::openai::codex;

    class ProviderApplication::Runtime {
    public:
        virtual ~Runtime() = default;
        virtual bool start() = 0;
        virtual void stop() noexcept = 0;
        virtual pid_t appServerPid() const noexcept = 0;
    };

    namespace {

        class StdioRuntime final : public ProviderApplication::Runtime {
        public:
            StdioRuntime(codex::bridge::CodexBridge& bridge, const Configuration& configuration)
                : endpoint_(bridge, options(configuration)) {
            }

            bool start() override {
                return endpoint_.start();
            }

            void stop() noexcept override {
                endpoint_.stop();
            }

            pid_t appServerPid() const noexcept override {
                return endpoint_.pid();
            }

        private:
            static codex::provider::StdioAppServerOptions options(const Configuration& configuration) {
                auto result = configuration.stdioAppServerOptions();
                result.onExit = [](int status) {
                    std::cerr << "codex-bridge: app-server process terminated status=" << status << '\n';
                    core::SNodeC::stop();
                };
                return result;
            }

            codex::provider::StdioAppServer endpoint_;
        };

#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        class NetworkRuntimeBase : public ProviderApplication::Runtime {
        public:
            NetworkRuntimeBase(codex::bridge::CodexBridge& bridge, const Configuration& configuration)
                : endpoint_(bridge, configuration.maximumFrameBytes()) {
                codex::provider::linkAppServerWebSocketClient();
            }

            bool start() final {
                connect();
                return true;
            }

            void stop() noexcept final {
                stopClient();
                endpoint_.stop();
            }

            pid_t appServerPid() const noexcept final {
                return -1;
            }

        protected:
            codex::provider::WebSocketAppServer& endpoint() noexcept {
                return endpoint_;
            }

        private:
            virtual void connect() = 0;
            virtual void stopClient() noexcept = 0;

            codex::provider::WebSocketAppServer endpoint_;
        };

        template <typename Client>
        void connectClient(Client& client, std::string transport) {
            client.connect([transport = std::move(transport)](const auto& address, core::socket::State state) {
                if (state == core::socket::State::OK) {
                    std::clog << "codex-bridge: app-server " << transport << " connected at " << address.toString() << '\n';
                } else if (state != core::socket::State::DISABLED) {
                    std::cerr << "codex-bridge: app-server " << transport << " connect failed at " << address.toString()
                              << ": " << state.what() << '\n';
                }
            });
        }

        template <typename Client>
        void stopClient(Client& client) noexcept {
            static_cast<void>(client.getFlowController()->terminateFlow());
        }

        class UnixRuntime final : public NetworkRuntimeBase {
        public:
            UnixRuntime(codex::bridge::CodexBridge& bridge, const Configuration& configuration)
                : NetworkRuntimeBase(bridge, configuration)
                , client_("codex-bridge-app-server-unix",
                          [this](const auto& request) { endpoint().beginUpgrade(request); },
                          [this](const auto& request) { endpoint().httpDisconnected(request); },
                          endpoint().state()) {
                client_.getConfig()->Remote::setSunPath(codex::protocol::defaultAppServerSocketPath());
                configure();
            }

        private:
            void connect() override {
                connectClient(client_, "Unix WebSocket");
            }

            void stopClient() noexcept override {
                apps::codex_bridge::stopClient(client_);
            }

            void configure() {
                client_.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
                client_.getConfig()->Connection::setWriteTimeout(utils::Timeval({0, 0}));
                client_.getConfig()->Connection::setMaximumWriteQueueBytes(DefaultMaximumWriteQueueBytes);
                client_.getConfig()->setRetry(true)->setRetryTimeout(0.05)->setRetryLimit(1);
                client_.getConfig()->setReconnect(true);
            }

            codex::provider::WebSocketHttpClient<net::un::stream::legacy::SocketClient> client_;
        };

        class IPv4Runtime final : public NetworkRuntimeBase {
        public:
            IPv4Runtime(codex::bridge::CodexBridge& bridge, const Configuration& configuration)
                : NetworkRuntimeBase(bridge, configuration)
                , client_("codex-bridge-app-server-websocket-ipv4",
                          [this](const auto& request) { endpoint().beginUpgrade(request); },
                          [this](const auto& request) { endpoint().httpDisconnected(request); },
                          endpoint().state()) {
                client_.getConfig()->Remote::setHost("127.0.0.1")->setPort(4501);
                configure();
            }

        private:
            void connect() override {
                connectClient(client_, "IPv4 WebSocket");
            }

            void stopClient() noexcept override {
                apps::codex_bridge::stopClient(client_);
            }

            void configure() {
                client_.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
                client_.getConfig()->Connection::setWriteTimeout(utils::Timeval({0, 0}));
                client_.getConfig()->Connection::setMaximumWriteQueueBytes(DefaultMaximumWriteQueueBytes);
                client_.getConfig()->setRetry(true)->setRetryTimeout(0.05)->setRetryLimit(1);
                client_.getConfig()->setReconnect(true);
            }

            codex::provider::WebSocketHttpClient<net::in::stream::legacy::SocketClient> client_;
        };

        class IPv6Runtime final : public NetworkRuntimeBase {
        public:
            IPv6Runtime(codex::bridge::CodexBridge& bridge, const Configuration& configuration)
                : NetworkRuntimeBase(bridge, configuration)
                , client_("codex-bridge-app-server-websocket-ipv6",
                          [this](const auto& request) { endpoint().beginUpgrade(request); },
                          [this](const auto& request) { endpoint().httpDisconnected(request); },
                          endpoint().state()) {
                client_.getConfig()->Remote::setHost("::1")->setPort(4501);
                configure();
            }

        private:
            void connect() override {
                connectClient(client_, "IPv6 WebSocket");
            }

            void stopClient() noexcept override {
                apps::codex_bridge::stopClient(client_);
            }

            void configure() {
                client_.getConfig()->Connection::setReadTimeout(utils::Timeval({0, 0}));
                client_.getConfig()->Connection::setWriteTimeout(utils::Timeval({0, 0}));
                client_.getConfig()->Connection::setMaximumWriteQueueBytes(DefaultMaximumWriteQueueBytes);
                client_.getConfig()->setRetry(true)->setRetryTimeout(0.05)->setRetryLimit(1);
                client_.getConfig()->setReconnect(true);
            }

            codex::provider::WebSocketHttpClient<net::in6::stream::legacy::SocketClient> client_;
        };
#endif

    } // namespace

    ProviderApplication::ProviderApplication(codex::bridge::CodexBridge& bridge,
                                             const Configuration& configuration)
        : bridge_(bridge) {
        switch (configuration.appServerTransport()) {
        case AppServerTransport::Stdio:
            runtime_ = std::make_unique<StdioRuntime>(bridge, configuration);
            break;
#if defined(AISUITE_CODEX_FRONTEND_WEBSOCKET)
        case AppServerTransport::Unix:
            runtime_ = std::make_unique<UnixRuntime>(bridge, configuration);
            break;
        case AppServerTransport::WebSocketIPv4:
            runtime_ = std::make_unique<IPv4Runtime>(bridge, configuration);
            break;
        case AppServerTransport::WebSocketIPv6:
            runtime_ = std::make_unique<IPv6Runtime>(bridge, configuration);
            break;
#else
        case AppServerTransport::Unix:
        case AppServerTransport::WebSocketIPv4:
        case AppServerTransport::WebSocketIPv6:
            throw std::runtime_error("selected app-server transport requires SNode.C WebSocket client support");
#endif
        }
        bridge_.onProviderLifecycle([this](bool connected) { providerLifecycleChanged(connected); });
    }

    ProviderApplication::~ProviderApplication() {
        bridge_.onProviderLifecycle({});
        stop();
    }

    bool ProviderApplication::start() {
        return runtime_->start();
    }

    void ProviderApplication::stop() noexcept {
        if (runtime_) {
            runtime_->stop();
        }
    }

    pid_t ProviderApplication::appServerPid() const noexcept {
        return runtime_ ? runtime_->appServerPid() : -1;
    }

    void ProviderApplication::providerLifecycleChanged(bool connected) {
        if (!connected) {
            return;
        }
        using Initialize = codex::generated::client_requests::Initialize;
        Initialize::Params parameters({
            {"clientInfo", {{"name", "codex_bridge"}, {"title", "Codex Bridge"}, {"version", "0.6.0"}}},
            {"capabilities", {{"experimentalApi", true}, {"requestAttestation", false}}},
        });
        bridge_.initialize(parameters, [this](Initialize::Response& response) {
            if (!response) {
                std::cerr << "codex-bridge: app-server initialize failed: "
                          << response.jsonRpcErrorMessage().value_or("unknown error") << '\n';
                core::SNodeC::stop();
                return;
            }
            if (!bridge_.initialized()) {
                std::cerr << "codex-bridge: app-server initialized notification was rejected\n";
                core::SNodeC::stop();
                return;
            }
            bridge_.setAppServerReady();
        });
    }

} // namespace apps::codex_bridge
