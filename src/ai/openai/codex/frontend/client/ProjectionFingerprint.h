#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_PROJECTIONFINGERPRINT_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_PROJECTIONFINGERPRINT_H

#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/frontend/Security.h"
#include "ai/openai/codex/frontend/client/Export.h"

#include <optional>
#include <string>
#include <vector>

namespace ai::openai::codex::frontend::client {

    struct ProjectionFingerprintInput {
        std::vector<frontend::FrontendCapability> requestedRepresentationCapabilities;
        std::vector<frontend::FrontendCapability> selectedRepresentationCapabilities;
        std::optional<std::string> continuityKey;
        std::optional<std::vector<frontend::FrontendScope>> permittedScopes;
        std::optional<std::vector<frontend::generated::MethodId>> permittedMethods;
        std::optional<std::vector<frontend::generated::MethodId>> availableMethods;
        std::optional<frontend::Json> explicitProjectionMetadata;
    };

    AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_EXPORT [[nodiscard]] std::string projectionFingerprint(const ProjectionFingerprintInput& input);

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_PROJECTIONFINGERPRINT_H
