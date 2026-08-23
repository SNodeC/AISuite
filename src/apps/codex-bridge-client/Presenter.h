/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BRIDGE_CLIENT_PRESENTER_H
#define APPS_CODEX_BRIDGE_CLIENT_PRESENTER_H

#include <iosfwd>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace apps::codex_bridge_client {

    enum class OutputMode { Human, Json };

    class Presenter {
    public:
        explicit Presenter(OutputMode mode = OutputMode::Human);
        Presenter(OutputMode mode, std::ostream& output, std::ostream& diagnostics);

        void appServerMessage(const nlohmann::json& message);
        void bridgeEvent(const nlohmann::json& message);
        void localMessage(std::string_view message);
        void error(std::string_view message);
        void connected(std::string_view transport);
        void disconnected();

        void setWatchEnabled(bool enabled) noexcept;
        bool watchEnabled() const noexcept;
        OutputMode outputMode() const noexcept;

    private:
        void emitJson(nlohmann::json message);

        OutputMode mode_;
        std::ostream* output_;
        std::ostream* diagnostics_;
        bool watching_ = true;
    };

} // namespace apps::codex_bridge_client

#endif
