/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_PROTOCOL_RUNTIMEPATHS_H
#define AI_OPENAI_CODEX_PROTOCOL_RUNTIMEPATHS_H

#include <string>

namespace ai::openai::codex::protocol {

    std::string runtimeDirectory();
    bool ensurePrivateRuntimeDirectory(std::string* error = nullptr);
    std::string defaultFrontendSocketPath();
    std::string defaultAppServerSocketPath();

} // namespace ai::openai::codex::protocol

#endif
