/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/detail/FrontendCapabilities.h"

#include "ai/openai/codex/frontend/GeneratedProtocol.h"

namespace ai::openai::codex::frontend::detail {

    CapabilityTruth computeCapabilities(bool cppClientSdkBuilt, std::size_t declaredTransportFamilies) {
        CapabilityTruth truth;
        for (const generated::CapabilityMetadata& metadata : generated::AllCapabilities) {
            const auto capability = frontendCapabilityFromString(metadata.key);
            if (!capability) {
                continue;
            }
            truth.advertisement.defined.push_back(*capability);
            bool implemented = false;
            switch (metadata.category) {
                case generated::CapabilityCategory::StaticMechanism:
                    implemented = metadata.implementedByCurrentRuntime;
                    truth.staticMechanisms += implemented ? 1U : 0U;
                    break;
                case generated::CapabilityCategory::ConditionalTopology:
                    implemented = *capability == FrontendCapability::MultiTransport && declaredTransportFamilies > 1U;
                    truth.conditionalTopology += implemented ? 1U : 0U;
                    break;
                case generated::CapabilityCategory::Product:
                    implemented = *capability == FrontendCapability::CppClientSdk && cppClientSdkBuilt;
                    truth.implementedProducts += implemented ? 1U : 0U;
                    break;
            }
            if (implemented) {
                truth.advertisement.implemented.push_back(*capability);
                truth.advertisement.permitted.push_back(*capability);
            }
        }
        return truth;
    }

} // namespace ai::openai::codex::frontend::detail
