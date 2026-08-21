#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_TYPES_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_TYPES_H

#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/Security.h"
#include "ai/openai/codex/frontend/client/Results.h"
#include "ai/openai/codex/typed/ServerRequests.h"

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
        std::size_t maximumInboundMessageBytes = frontend::DefaultFrontendMaximumServerMessageBytes;
        std::size_t maximumOutboundMessageBytes = frontend::DefaultFrontendMaximumInboundMessageBytes;
        // Pragmatic headroom for the public JSON-shaped immutable State after
        // restoring backend-bounded item output. This is not a universal
        // proof for arbitrarily large operator-configured backend limits.
        std::size_t maximumDecodedStateBytes = frontend::DefaultFrontendMaximumDecodedStateBytes;
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
        std::optional<std::uint64_t> serverMaximumInboundMessageBytes;
        std::vector<frontend::FrontendCapability> requestedRepresentationCapabilities;
        std::vector<frontend::FrontendCapability> selectedRepresentationCapabilities;
        std::vector<frontend::FrontendCapability> observedMechanismCapabilities;
        std::vector<frontend::FrontendCapability> observedTopologyCapabilities;
        std::vector<frontend::FrontendCapability> observedProductCapabilities;
        std::optional<std::vector<frontend::generated::MethodId>> availableMethods;
        std::optional<std::vector<frontend::generated::MethodId>> permittedMethods;
        std::optional<std::vector<frontend::FrontendScope>> permittedScopes;
    };

    // Reverse-request parameters deliberately combine the frontend journal's
    // stable pending-request identity with the existing typed App Server
    // response value. The encoder flattens or nests that value exactly as the
    // Frontend Protocol schema requires.
    struct ApprovalRespondParams {
        PendingRequestId pendingRequestId;
        typed::ApprovalDecision decision;
    };

    struct UserInputRespondParams {
        PendingRequestId pendingRequestId;
        std::vector<typed::UserInputAnswer> answers;
    };

    struct AuthenticationRespondParams {
        PendingRequestId pendingRequestId;
        typed::AuthenticationResponse response;
    };

    struct UnknownRequestRespondParams {
        PendingRequestId pendingRequestId;
        frontend::Json result = nullptr;
    };

    struct UnknownRequestRejectParams {
        PendingRequestId pendingRequestId;
        ::ai::openai::codex::ProtocolError error;
    };

    struct ApplyPatchApprovalRespondParams {
        PendingRequestId pendingRequestId;
        typed::ApplyPatchApprovalResponse response;
    };

    struct AttestationRespondParams {
        PendingRequestId pendingRequestId;
        typed::AttestationGenerateResponse response;
    };

    struct DynamicToolRespondParams {
        PendingRequestId pendingRequestId;
        typed::DynamicToolCallResponse response;
    };

    struct ExecCommandApprovalRespondParams {
        PendingRequestId pendingRequestId;
        typed::ExecCommandApprovalResponse response;
    };

    struct KnownRequestRejectParams {
        PendingRequestId pendingRequestId;
        ::ai::openai::codex::ProtocolError error;
    };

    struct McpElicitationRespondParams {
        PendingRequestId pendingRequestId;
        typed::McpServerElicitationRequestResponse response;
    };

    struct PermissionsApprovalRespondParams {
        PendingRequestId pendingRequestId;
        typed::PermissionsRequestApprovalResponse response;
    };

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_TYPES_H
