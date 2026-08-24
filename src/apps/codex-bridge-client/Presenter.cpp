/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-bridge-client/Presenter.h"

#include "ai/openai/codex/protocol/Envelope.h"

#include <iostream>

#include <nlohmann/json.hpp>

namespace apps::codex_bridge_client {

    Presenter::Presenter(OutputMode mode)
        : Presenter(mode, std::cout, std::cerr) {
    }

    Presenter::Presenter(OutputMode mode, std::ostream& output, std::ostream& diagnostics)
        : mode_(mode)
        , output_(&output)
        , diagnostics_(&diagnostics) {
    }

    void Presenter::appServerMessage(const nlohmann::json& message) {
        const auto kind = ai::openai::codex::protocol::classifyJsonRpc(message);
        if (kind != ai::openai::codex::protocol::JsonRpcKind::Response && !watching_) {
            return;
        }
        if (mode_ == OutputMode::Json) {
            emitJson({{"kind", "appserver"}, {"payload", message}});
            return;
        }
        if (const auto method = ai::openai::codex::protocol::jsonRpcMethod(message)) {
            *output_ << *method << ' ';
        }
        *output_ << message.dump(2) << '\n';
    }

    void Presenter::bridgeEvent(const nlohmann::json& message) {
        if (mode_ == OutputMode::Json) {
            emitJson(message);
        } else {
            *output_ << message.dump(2) << '\n';
        }
    }

    void Presenter::localMessage(std::string_view message) {
        if (mode_ == OutputMode::Json) {
            emitJson({{"kind", "client.message"}, {"message", message}});
        } else {
            *diagnostics_ << message << '\n';
        }
    }

    void Presenter::error(std::string_view message) {
        if (mode_ == OutputMode::Json) {
            emitJson({{"kind", "client.error"}, {"message", message}});
        } else {
            *diagnostics_ << "error: " << message << '\n';
        }
    }

    void Presenter::connected(std::string_view transport) {
        if (mode_ == OutputMode::Json) {
            emitJson({{"kind", "client.connection"}, {"event", "connected"}, {"transport", transport}});
        } else {
            *diagnostics_ << "connected using " << transport << '\n';
        }
    }

    void Presenter::disconnected() {
        if (mode_ == OutputMode::Json) {
            emitJson({{"kind", "client.connection"}, {"event", "disconnected"}});
        } else {
            *diagnostics_ << "disconnected\n";
        }
    }

    void Presenter::setWatchEnabled(bool enabled) noexcept {
        watching_ = enabled;
    }

    bool Presenter::watchEnabled() const noexcept {
        return watching_;
    }

    OutputMode Presenter::outputMode() const noexcept {
        return mode_;
    }

    void Presenter::emitJson(nlohmann::json message) {
        *output_ << message.dump() << '\n' << std::flush;
    }

} // namespace apps::codex_bridge_client
