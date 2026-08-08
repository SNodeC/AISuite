#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_CLIENT_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_CLIENT_H

#include "ai/openai/codex/frontend/client/Changes.h"
#include "ai/openai/codex/frontend/client/Connection.h"
#include "ai/openai/codex/frontend/client/Export.h"
#include "ai/openai/codex/frontend/client/Types.h"

#include <memory>
#include <optional>
#include <string>

namespace ai::openai::codex::frontend::client {

    namespace detail {
        struct BoundOperationCompletion;
        struct ClientTestAccess;
    } // namespace detail

    class Accounts;
    class Apps;
    class Commands;
    class Configuration;
    class Controller;
    class ExternalAgents;
    class Feedback;
    class Filesystem;
    class Hooks;
    class Marketplace;
    class Mcp;
    class Models;
    class PermissionProfiles;
    class Plugins;
    class Provider;
    class Requests;
    class Reviews;
    class Skills;
    class Synchronization;
    struct SynchronizationResult;
    class Threads;
    class Turns;
    class WindowsSandbox;

    class AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_EXPORT Client {
    public:
        explicit Client(ClientOptions options = {}, ClientCallbacks callbacks = {});
        Client(const Client&) = delete;
        Client(Client&&) = delete;
        Client& operator=(const Client&) = delete;
        Client& operator=(Client&&) = delete;
        ~Client();

        [[nodiscard]] Connection openConnection(TransportCallbacks callbacks);
        void setCallbacks(ClientCallbacks callbacks);
        void close(std::string reason = "frontend client closed") noexcept;
        [[nodiscard]] bool isOpen() const noexcept;
        [[nodiscard]] bool hasActiveConnection() const noexcept;
        [[nodiscard]] bool isReady() const noexcept;
        [[nodiscard]] ConnectionState connectionState() const noexcept;
        [[nodiscard]] State state() const noexcept;
        [[nodiscard]] std::optional<SessionInfo> session() const;
        [[nodiscard]] std::optional<frontend::SequenceNumber> visibleSequence() const;
        [[nodiscard]] std::optional<frontend::SequenceNumber> synchronizedThrough() const;
        [[nodiscard]] std::size_t pendingOperationCount() const noexcept;
        [[nodiscard]] MethodStatus methodStatus(frontend::generated::MethodId method) const;
        [[nodiscard]] CapabilityStatus capabilityStatus(frontend::FrontendCapability capability) const;

        [[nodiscard]] Submission submit(frontend::generated::CompleteCommandParameters parameters, GeneratedCompletionHandler handler);

        Controller& controller() noexcept;
        Provider& provider() noexcept;
        Synchronization& synchronization() noexcept;
        Accounts& accounts() noexcept;
        Apps& apps() noexcept;
        Commands& commands() noexcept;
        Configuration& configuration() noexcept;
        ExternalAgents& externalAgents() noexcept;
        Feedback& feedback() noexcept;
        Filesystem& filesystem() noexcept;
        Hooks& hooks() noexcept;
        Marketplace& marketplace() noexcept;
        Mcp& mcp() noexcept;
        Models& models() noexcept;
        PermissionProfiles& permissionProfiles() noexcept;
        Plugins& plugins() noexcept;
        Requests& requests() noexcept;
        Reviews& reviews() noexcept;
        Skills& skills() noexcept;
        Threads& threads() noexcept;
        Turns& turns() noexcept;
        WindowsSandbox& windowsSandbox() noexcept;

    private:
        friend class Accounts;
        friend class Apps;
        friend class Commands;
        friend class Configuration;
        friend class Controller;
        friend class Connection;
        friend class ExternalAgents;
        friend class Feedback;
        friend class Filesystem;
        friend class Hooks;
        friend class Marketplace;
        friend class Mcp;
        friend class Models;
        friend class PermissionProfiles;
        friend class Plugins;
        friend class Provider;
        friend class Requests;
        friend class Reviews;
        friend class Skills;
        friend class Synchronization;
        friend class Threads;
        friend class Turns;
        friend class WindowsSandbox;
        friend struct detail::ClientTestAccess;
        AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_NO_EXPORT [[nodiscard]] Submission
        submitBound(frontend::generated::CompleteCommandParameters parameters, detail::BoundOperationCompletion completion);
        AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_NO_EXPORT [[nodiscard]] Submission
        submitReverseBound(const PendingRequestId& pendingRequestId,
                           frontend::generated::CompleteCommandParameters parameters,
                           detail::BoundOperationCompletion completion);
        AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_NO_EXPORT [[nodiscard]] Submission beginSynchronization(
            frontend::SyncMode mode, std::optional<frontend::SequenceNumber> after, CompletionHandler<SynchronizationResult> handler);
        struct AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_NO_EXPORT Impl;
        std::unique_ptr<Impl> impl;
    };

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_CLIENT_H
