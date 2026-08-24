/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BRIDGE_CLIENT_CLIENTSESSION_H
#define APPS_CODEX_BRIDGE_CLIENT_CLIENTSESSION_H

#include "apps/codex-bridge-client/CommandParser.h"

#include <functional>

namespace ai::openai::codex::frontend {
    class CodexBridge;
}

namespace apps::codex_bridge_client {

    class Presenter;

    class ClientSession {
    public:
        ClientSession(ai::openai::codex::frontend::CodexBridge& sdk,
                      Presenter& presenter,
                      std::function<void()> reconnect,
                      std::function<void()> quit);

        void execute(ParsedCommand command);

    private:
        template <typename Response>
        void reportError(Response& response) {
            if (!response) {
                reportSdkError(response.jsonRpcErrorMessage().value_or("app-server request failed"));
            }
        }

        void reportSdkError(std::string message);
        void startNew(NewCommand command);

        ai::openai::codex::frontend::CodexBridge& sdk_;
        Presenter& presenter_;
        std::function<void()> reconnect_;
        std::function<void()> quit_;
    };

} // namespace apps::codex_bridge_client

#endif
