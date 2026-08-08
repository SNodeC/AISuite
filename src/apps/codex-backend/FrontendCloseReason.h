/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_FRONTENDCLOSEREASON_H
#define APPS_CODEX_BACKEND_FRONTENDCLOSEREASON_H

#include <iostream>
#include <string>
#include <string_view>

namespace apps::codex_backend {

    // FrontendService supplies lifecycle-only reasons, never protocol payloads.
    // Bound and flatten them before writing to the native transport diagnostic
    // stream so a reason cannot inject another log record.
    inline std::string safeFrontendCloseReason(std::string_view reason) {
        constexpr std::size_t MaximumReasonBytes = 240;
        std::string safe;
        safe.reserve(reason.size() < MaximumReasonBytes ? reason.size() : MaximumReasonBytes + 3U);
        for (const unsigned char character : reason.substr(0, MaximumReasonBytes)) {
            safe.push_back(character >= 0x20U && character < 0x7fU ? static_cast<char>(character) : '?');
        }
        if (reason.size() > MaximumReasonBytes) {
            safe += "...";
        }
        return safe.empty() ? "unspecified frontend service close" : safe;
    }

    inline void logFrontendCloseReason(std::string_view transport, std::string_view reason) noexcept {
        try {
            std::clog << "codex-backend: frontend " << transport << " closed: " << safeFrontendCloseReason(reason) << '\n';
        } catch (...) {
        }
    }

} // namespace apps::codex_backend

#endif // APPS_CODEX_BACKEND_FRONTENDCLOSEREASON_H
