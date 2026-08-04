/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_DETAIL_EVENTREPRESENTATION_H
#define AI_OPENAI_CODEX_FRONTEND_DETAIL_EVENTREPRESENTATION_H

#include "ai/openai/codex/frontend/Messages.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace ai::openai::codex::frontend::detail {

    enum class EventRepresentation : std::uint8_t { None = 0, Legacy = 1, Expanded = 2, Either = 3 };

    inline constexpr std::array<std::string_view, 12> LegacyEventTypes{
        "backend.lifecycle.changed",
        "diagnostics.updated",
        "thread.updated",
        "thread.list.updated",
        "turn.updated",
        "item.updated",
        "item.content.updated",
        "request.pending",
        "request.resolved",
        "controller.changed",
        "session.changed",
        "codex.extension",
    };

    [[nodiscard]] inline constexpr EventRepresentation intersectRepresentations(EventRepresentation left,
                                                                                EventRepresentation right) noexcept {
        return static_cast<EventRepresentation>(static_cast<std::uint8_t>(left) & static_cast<std::uint8_t>(right));
    }

    [[nodiscard]] inline EventRepresentation eventRepresentation(std::string_view type) noexcept {
        const bool legacy = std::find(LegacyEventTypes.begin(), LegacyEventTypes.end(), type) != LegacyEventTypes.end();
        const bool expanded = expandedEventTypeFromString(type).has_value();
        return static_cast<EventRepresentation>((legacy ? static_cast<std::uint8_t>(EventRepresentation::Legacy) : 0U) |
                                                (expanded ? static_cast<std::uint8_t>(EventRepresentation::Expanded) : 0U));
    }

} // namespace ai::openai::codex::frontend::detail

#endif // AI_OPENAI_CODEX_FRONTEND_DETAIL_EVENTREPRESENTATION_H
