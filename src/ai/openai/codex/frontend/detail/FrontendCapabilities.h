/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_DETAIL_FRONTENDCAPABILITIES_H
#define AI_OPENAI_CODEX_FRONTEND_DETAIL_FRONTENDCAPABILITIES_H

#include "ai/openai/codex/frontend/Messages.h"

#include <cstddef>

namespace ai::openai::codex::frontend::detail {

    struct CapabilityTruth {
        CapabilityAdvertisement advertisement;
        std::size_t staticMechanisms = 0;
        std::size_t conditionalTopology = 0;
        std::size_t implementedProducts = 0;
    };

    [[nodiscard]] CapabilityTruth computeCapabilities(bool cppClientSdkBuilt, std::size_t declaredTransportFamilies);

} // namespace ai::openai::codex::frontend::detail

#endif // AI_OPENAI_CODEX_FRONTEND_DETAIL_FRONTENDCAPABILITIES_H
