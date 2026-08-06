/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_DETAIL_FRONTENDSERVICETESTACCESS_H
#define AI_OPENAI_CODEX_FRONTEND_DETAIL_FRONTENDSERVICETESTACCESS_H

#include "ai/openai/codex/frontend/Messages.h"

namespace ai::openai::codex::frontend {

    class FrontendConnection;
    class FrontendService;

    // Source-private test seam. This header is deliberately not installed;
    // only private friend declarations appear in FrontendService.h.
    struct FrontendServiceTestAccess {
        [[nodiscard]] static bool enqueue(FrontendService& service, FrontendConnection& connection, ServerMessage message) noexcept;
    };

} // namespace ai::openai::codex::frontend

#endif // AI_OPENAI_CODEX_FRONTEND_DETAIL_FRONTENDSERVICETESTACCESS_H
