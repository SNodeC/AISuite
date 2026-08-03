/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include <ai/openai/codex/backend/BackendCommand.h>
#include <ai/openai/codex/backend/BackendCore.h>
#include <ai/openai/codex/backend/BackendState.h>
#include <ai/openai/codex/backend/Snapshot.h>
#include <ai/openai/codex/frontend/BackendAdapter.h>
#include <ai/openai/codex/stdio/Client.h>
#include <algorithm>
#include <array>
#include <core/SNodeC.h>
#include <core/socket/State.h>
#include <core/socket/stream/SocketConnection.h>
#include <core/socket/stream/SocketContext.h>
#include <core/socket/stream/SocketContextFactory.h>
#include <cstddef>
#include <filesystem>
#include <net/un/SocketAddress.h>
#include <net/un/stream/legacy/SocketServer.h>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace {
    namespace codex = ai::openai::codex;

    static_assert(std::variant_size_v<codex::backend::ProviderOperationValue> == 65);
    static_assert(std::variant_size_v<codex::backend::CommandValue> == 68);
    static_assert(std::variant_size_v<codex::backend::BackendCommand> == 101);

    template <std::size_t... Index>
    bool exerciseInstalledCommandPolicies(std::index_sequence<Index...>) {
        const std::array policies{
            codex::backend::commandPolicy(codex::backend::BackendCommand{std::in_place_index<Index>})...,
        };
        return std::all_of(policies.begin(), policies.end(), [](const codex::backend::CommandPolicy& policy) {
            return policy.access == codex::backend::CommandAccess::Observer || policy.access == codex::backend::CommandAccess::Controller;
        });
    }

    bool exerciseInstalledBackendApi() {
        codex::backend::BackendState state;
        codex::backend::Snapshot snapshot;
        codex::backend::ProviderOperationValue providerValue{codex::typed::Unit{}};
        codex::backend::CommandValue commandValue{std::move(snapshot)};
        const codex::backend::BackendCommand command{codex::backend::SnapshotGet{}};
        const codex::backend::CommandPolicy policy = codex::backend::commandPolicy(command);

        return state.provider.lifecycle == codex::backend::ProviderLifecycle::Stopped &&
               std::holds_alternative<codex::typed::Unit>(providerValue) &&
               std::holds_alternative<codex::backend::Snapshot>(commandValue) && policy.access == codex::backend::CommandAccess::Observer &&
               !policy.requiresProviderReady &&
               exerciseInstalledCommandPolicies(std::make_index_sequence<std::variant_size_v<codex::backend::BackendCommand>>{});
    }

    class ModuleSocketContext final : public core::socket::stream::SocketContext {
    public:
        ModuleSocketContext(core::socket::stream::SocketConnection* socketConnection, codex::frontend::BackendAdapter& adapter)
            : core::socket::stream::SocketContext(socketConnection)
            , adapter(adapter) {
        }

    private:
        void onConnected() override {
            connection = adapter.openConnection({.onMessage =
                                                     [this](const codex::frontend::OutboundMessage& message) {
                                                         sendToPeer(message.compactJson.data(), message.compactJson.size());
                                                         static constexpr char newline = '\n';
                                                         sendToPeer(&newline, 1);
                                                         return true;
                                                     },
                                                 .onClosed =
                                                     [this](const std::string&) {
                                                         shutdownRead();
                                                         shutdownWrite();
                                                     }});
        }

        void onDisconnected() override {
            connection.close("installed module consumer disconnected");
        }

        std::size_t onReceivedFromPeer() override {
            std::array<char, 1024> bytes{};
            return readFromPeer(bytes.data(), bytes.size());
        }

        bool onSignal([[maybe_unused]] int signum) override {
            return true;
        }

        codex::frontend::BackendAdapter& adapter;
        codex::frontend::FrontendConnection connection;
    };

    class ModuleSocketContextFactory final : public core::socket::stream::SocketContextFactory {
    public:
        explicit ModuleSocketContextFactory(codex::frontend::BackendAdapter& adapter)
            : adapter(adapter) {
        }

        core::socket::stream::SocketContext* create(core::socket::stream::SocketConnection* socketConnection) override {
            return new ModuleSocketContext(socketConnection, adapter);
        }

    private:
        codex::frontend::BackendAdapter& adapter;
    };
} // namespace

int main(int argc, char* argv[]) {
    if (!exerciseInstalledBackendApi()) {
        return 1;
    }
    core::SNodeC::init(argc, argv);

    int result = 1;
    bool listening = false;
    const std::filesystem::path socketPath = std::filesystem::current_path() / "aisuite-module-consumer.sock";
    {
        codex::backend::BackendCore<codex::stdio::Client> backend;
        codex::frontend::BackendAdapter adapter(backend);
        auto server = net::un::stream::legacy::Server<ModuleSocketContextFactory>(
            "aisuite-installed-module-server",
            [&socketPath](net::un::stream::legacy::config::ConfigSocketServer* config) {
                config->Local::setSunPath(socketPath.string());
            },
            adapter);

        server.listen([&listening](const net::un::SocketAddress&, core::socket::State state) {
            listening = state == core::socket::State::OK;
            core::SNodeC::stop();
        });
        result = core::SNodeC::start();
        adapter.close("installed module consumer complete");
    }

    core::SNodeC::free();
    std::error_code removeError;
    std::filesystem::remove(socketPath, removeError);
    return result == 0 && listening ? 0 : 1;
}
