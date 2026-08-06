#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_TYPES_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_TYPES_H

#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/frontend/Security.h"
#include "ai/openai/codex/frontend/client/Results.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ai::openai::codex::frontend::client {

    class State;

    enum class ConnectionState { Disconnected, Connecting, Authenticating, Synchronizing, Ready, Closing, Closed };
    enum class Availability { Unknown, No, Yes };
    enum class RepresentationMode { Unknown, LegacyV1, ExpandedV1 };
    enum class StateFreshness { Current, Stale, Synchronizing };

    struct AuthenticationContext {
        frontend::AuthenticationCredential credential;
        std::optional<std::string> continuityKey;
    };
    using CredentialProvider = std::function<AuthenticationContext()>;

    struct ClientOptions {
        std::vector<frontend::FrontendCapability> requestedCapabilities{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
        };
        std::vector<frontend::FrontendCapability> requiredCapabilities;
        CredentialProvider credentialProvider;
        std::size_t maximumInboundMessageBytes = 16U * 1024U * 1024U;
        std::size_t maximumDecodedStateBytes = 64U * 1024U * 1024U;
        std::size_t maximumPendingOperations = 256;
        std::size_t maximumRetainedDiagnostics = 64;
        bool allowLegacyV1 = true;
    };

    struct MethodStatus {
        frontend::generated::MethodId method = frontend::generated::MethodId::ControllerAcquire;
        Availability available = Availability::Unknown;
        Availability permitted = Availability::Unknown;
        bool controllerRequired = false;
        bool providerReadyRequired = false;
        bool defaultEnabled = false;
        std::vector<frontend::FrontendScope> requiredScopes;
    };

    struct CapabilityStatus {
        frontend::FrontendCapability capability = frontend::FrontendCapability::MethodDiscovery;
        Availability defined = Availability::Unknown;
        Availability implemented = Availability::Unknown;
        Availability permitted = Availability::Unknown;
    };

    struct SessionInfo {
        std::string sessionId;
        frontend::SessionRole role = frontend::SessionRole::Observer;
        frontend::SyncMode syncMode = frontend::SyncMode::Snapshot;
        frontend::SequenceNumber serverCurrentSequence{};
        std::optional<std::string> serverVersion;
        std::vector<frontend::FrontendCapability> requestedRepresentationCapabilities;
        std::vector<frontend::FrontendCapability> selectedRepresentationCapabilities;
        std::vector<frontend::FrontendCapability> observedMechanismCapabilities;
        std::vector<frontend::FrontendCapability> observedTopologyCapabilities;
        std::vector<frontend::FrontendCapability> observedProductCapabilities;
        std::vector<frontend::generated::MethodId> availableMethods;
        std::vector<frontend::generated::MethodId> permittedMethods;
        std::vector<frontend::FrontendScope> permittedScopes;
    };

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_TYPES_H
