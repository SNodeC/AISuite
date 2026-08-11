/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef TESTS_COMPONENT_CODEX_ORACLE_LEGACYFRONTENDCLIENTCAPTURE_H
#define TESTS_COMPONENT_CODEX_ORACLE_LEGACYFRONTENDCLIENTCAPTURE_H

#include "ai/openai/codex/frontend/Messages.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace tests::codex::oracle {

    struct LegacyFrontendClientCapture {
        bool accepted = false;
        bool ready = false;
        std::size_t closes = 0;
        std::vector<ai::openai::codex::frontend::Json> states;
        std::vector<std::string> callbacks;
        std::vector<std::string> diagnostics;
        std::vector<std::string> outbound;
    };

    [[nodiscard]] LegacyFrontendClientCapture
    captureLegacyFrontendClient(std::span<const ai::openai::codex::frontend::ServerMessage> messages,
                                bool expanded,
                                bool disconnectAfterMessages,
                                bool closeAfterMessages = false);

} // namespace tests::codex::oracle

#endif // TESTS_COMPONENT_CODEX_ORACLE_LEGACYFRONTENDCLIENTCAPTURE_H
