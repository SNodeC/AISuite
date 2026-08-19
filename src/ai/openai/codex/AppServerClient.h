/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *               2020, 2021, 2022, 2023, 2024, 2025, 2026
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_APPSERVERCLIENT_H
#define AI_OPENAI_CODEX_APPSERVERCLIENT_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/Types.h"

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace ai::openai::codex::detail {
    class Transport;
}

namespace ai::openai::codex::typed {
    class Accounts;
    class Apps;
    class Commands;
    class Configuration;
    class Events;
    class ExternalAgents;
    class Feedback;
    class Filesystem;
    class Hooks;
    class Marketplace;
    class Mcp;
    class Models;
    class PermissionProfiles;
    class Plugins;
    class Requests;
    class Reviews;
    class Skills;
    class Threads;
    class Turns;
    class WindowsSandbox;
} // namespace ai::openai::codex::typed

namespace ai::openai::codex {

    enum class State { Stopped, Starting, Initializing, Ready, Stopping, Failed };

    struct ClientInfo {
        std::string name = "aisuite";
        std::string title = "AISuite";
        std::string version = "0.2.0";
    };

    struct StateChange {
        State previous = State::Stopped;
        State current = State::Stopped;
        std::optional<Error> error;
    };

    struct Diagnostic {
        std::string message;
    };

    struct Callbacks {
        using StateChanged = std::function<void(const StateChange&)>;
        using DiagnosticReceived = std::function<void(const Diagnostic&)>;

        StateChanged onStateChanged;
        DiagnosticReceived onDiagnostic;
    };

    struct Submission {
        std::optional<ClientRequestId> id;
        std::optional<Error> error;

        explicit operator bool() const noexcept;
    };

    struct SendResult {
        bool accepted = false;
        std::optional<Error> error;

        explicit operator bool() const noexcept;
    };

    class AppServerClient {
    public:
        class RawProtocol;

        AppServerClient(const AppServerClient&) = delete;
        AppServerClient(AppServerClient&&) = delete;

        AppServerClient& operator=(const AppServerClient&) = delete;
        AppServerClient& operator=(AppServerClient&&) = delete;

        virtual ~AppServerClient();

        void start();
        void stop();

        State getState() const noexcept;
        bool isReady() const noexcept;

        RawProtocol& raw() noexcept;

        typed::Accounts& accounts() noexcept;
        typed::Apps& apps() noexcept;
        typed::Commands& commands() noexcept;
        typed::Configuration& configuration() noexcept;
        typed::Events& events() noexcept;
        typed::ExternalAgents& externalAgents() noexcept;
        typed::Feedback& feedback() noexcept;
        typed::Filesystem& filesystem() noexcept;
        typed::Hooks& hooks() noexcept;
        typed::Marketplace& marketplace() noexcept;
        typed::Mcp& mcp() noexcept;
        typed::Models& models() noexcept;
        typed::PermissionProfiles& permissionProfiles() noexcept;
        typed::Plugins& plugins() noexcept;
        typed::Requests& requests() noexcept;
        typed::Reviews& reviews() noexcept;
        typed::Skills& skills() noexcept;
        typed::Threads& threads() noexcept;
        typed::Turns& turns() noexcept;
        typed::WindowsSandbox& windowsSandbox() noexcept;

        std::optional<typed::InitializeResponse> getInitializeResponse() const;

        void setOnStateChanged(Callbacks::StateChanged callback);
        void setOnDiagnostic(Callbacks::DiagnosticReceived callback);

    protected:
        AppServerClient(std::unique_ptr<detail::Transport> transport, ClientInfo clientInfo);
        AppServerClient(std::unique_ptr<detail::Transport> transport, typed::InitializeParams initializeParams);

    private:
        friend class typed::Events;
        friend class typed::Requests;
        friend class typed::Threads;
        friend class typed::Turns;

        class Impl;
        std::unique_ptr<Impl> impl;
    };

    class AppServerClient::RawProtocol {
    public:
        using ResponseHandler = std::function<void(const Response&)>;
        using NotificationHandler = std::function<void(const Notification&)>;
        using ServerRequestHandler = std::function<void(const ServerRequest&)>;
        using UnknownMessageHandler = std::function<void(const UnknownMessage&)>;

        Submission request(std::string method, Json params, ResponseHandler handler);
        SendResult notify(std::string method, Json params = Json::object());
        SendResult respond(const ServerRequestId& id, Json result);
        SendResult reject(const ServerRequestId& id, ProtocolError error);

        void setOnNotification(NotificationHandler handler);
        void setOnServerRequest(ServerRequestHandler handler);
        void setOnUnknownMessage(UnknownMessageHandler handler);

    private:
        friend class AppServerClient::Impl;
        friend class typed::Events;
        friend class typed::Requests;

        explicit RawProtocol(Impl& impl) noexcept;

        void setTypedNotificationDispatcher(NotificationHandler handler);
        void setTypedServerRequestDispatcher(ServerRequestHandler handler);
        SendResult respondOwned(const ServerRequestId& id, ServerRequestToken token, Json result);
        SendResult respondOwned(const ServerRequestId& id, ServerRequestToken token, std::string_view expectedMethod, Json result);
        SendResult rejectOwned(const ServerRequestId& id, ServerRequestToken token, ProtocolError error);
        SendResult rejectOwned(const ServerRequestId& id, ServerRequestToken token, std::string_view expectedMethod, ProtocolError error);

        Impl* impl;
    };

} // namespace ai::openai::codex

#endif // AI_OPENAI_CODEX_APPSERVERCLIENT_H
