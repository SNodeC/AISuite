/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_DETAIL_BACKENDCOMMANDMAPPER_H
#define AI_OPENAI_CODEX_FRONTEND_DETAIL_BACKENDCOMMANDMAPPER_H

#include "ai/openai/codex/backend/BackendCommand.h"
#include "ai/openai/codex/frontend/GeneratedProtocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace ai::openai::codex::frontend::detail {

    enum class NativeServiceAction {
        ControllerAcquire,
        ControllerRelease,
        SnapshotGet,
        EventsReplay,
        ProviderStart,
        ProviderStop,
        ProviderRestart
    };

    struct NativeCommandMapping {
        NativeServiceAction action;
        std::optional<std::uint64_t> replayAfter;
    };

    struct BackendCommandMappingError {
        std::string message;
    };

    using DefinedCommandMapping = std::variant<backend::BackendCommand, NativeCommandMapping, BackendCommandMappingError>;

    [[nodiscard]] DefinedCommandMapping mapDefinedCommand(const generated::DefinedCommand& command) noexcept;

    // Private metadata seam used by exhaustive production-table tests.  The
    // value is derived from the concrete variant alternative, never from the
    // generated backendCommand string being checked.
    [[nodiscard]] std::string_view backendCommandTypeName(const backend::BackendCommand& command) noexcept;

} // namespace ai::openai::codex::frontend::detail

#endif // AI_OPENAI_CODEX_FRONTEND_DETAIL_BACKENDCOMMANDMAPPER_H
