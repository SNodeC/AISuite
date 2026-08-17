/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/Client.h"

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/client/Accounts.h"
#include "ai/openai/codex/frontend/client/Apps.h"
#include "ai/openai/codex/frontend/client/Commands.h"
#include "ai/openai/codex/frontend/client/Configuration.h"
#include "ai/openai/codex/frontend/client/Controller.h"
#include "ai/openai/codex/frontend/client/ExternalAgents.h"
#include "ai/openai/codex/frontend/client/Feedback.h"
#include "ai/openai/codex/frontend/client/Filesystem.h"
#include "ai/openai/codex/frontend/client/GeneratedBindings.h"
#include "ai/openai/codex/frontend/client/Hooks.h"
#include "ai/openai/codex/frontend/client/Marketplace.h"
#include "ai/openai/codex/frontend/client/Mcp.h"
#include "ai/openai/codex/frontend/client/Models.h"
#include "ai/openai/codex/frontend/client/PermissionProfiles.h"
#include "ai/openai/codex/frontend/client/Plugins.h"
#include "ai/openai/codex/frontend/client/Provider.h"
#include "ai/openai/codex/frontend/client/Requests.h"
#include "ai/openai/codex/frontend/client/Reviews.h"
#include "ai/openai/codex/frontend/client/Skills.h"
#include "ai/openai/codex/frontend/client/Synchronization.h"
#include "ai/openai/codex/frontend/client/Threads.h"
#include "ai/openai/codex/frontend/client/Turns.h"
#include "ai/openai/codex/frontend/client/WindowsSandbox.h"
#include "ai/openai/codex/frontend/client/detail/BoundOperation.h"
#include "ai/openai/codex/frontend/client/detail/ClientTestAccess.h"
#include "ai/openai/codex/frontend/client/detail/OperationCodecs.h"
#include "ai/openai/codex/frontend/internal/client/CanonicalStateBuilder.h"
#include "ai/openai/codex/frontend/internal/client/ClientCore.h"

#include <algorithm>
#include <any>
#include <array>
#include <functional>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ai::openai::codex::frontend::client {
    namespace core = frontend::internal::client;
    namespace model = frontend::internal::model;

    namespace {
        template <typename Callback>
        class ScopeExit final {
        public:
            explicit ScopeExit(Callback callback) noexcept(std::is_nothrow_move_constructible_v<Callback>)
                : callback(std::move(callback)) {
            }

            ScopeExit(const ScopeExit&) = delete;
            ScopeExit& operator=(const ScopeExit&) = delete;

            ~ScopeExit() noexcept {
                run();
            }

            void run() noexcept {
                if (active) {
                    active = false;
                    callback();
                }
            }

            void release() noexcept {
                active = false;
            }

        private:
            Callback callback;
            bool active = true;
        };

        template <typename Callback>
        ScopeExit(Callback) -> ScopeExit<Callback>;

        Error publicError(const core::ClientError& source) {
            Error result;
            result.origin = static_cast<ErrorOrigin>(source.origin);
            if (source.clientCode.has_value()) {
                result.clientCode = source.publicLiveSnapshotStateDivergence ? ClientErrorCode::StateDivergence
                                                                             : static_cast<ClientErrorCode>(*source.clientCode);
            }
            result.protocolCode = source.protocolCode;
            result.message = source.message;
            result.details = source.remoteDetails;
            result.retryable = source.retryable;
            return result;
        }

        Error localError(ClientErrorCode code, std::string message, bool retryable = false) {
            return Error{ErrorOrigin::Client, code, std::nullopt, std::move(message), std::nullopt, std::nullopt, retryable};
        }

        Error resultError(std::string message) {
            return Error{ErrorOrigin::Protocol,
                         ClientErrorCode::ResponseTypeMismatch,
                         std::nullopt,
                         std::move(message),
                         std::nullopt,
                         std::nullopt,
                         false};
        }

        ConnectionState publicConnectionState(core::ConnectionState source) noexcept {
            return static_cast<ConnectionState>(source);
        }

        Availability publicAvailability(core::Availability source) noexcept {
            return static_cast<Availability>(source);
        }

        Diagnostic::Severity publicSeverity(core::DiagnosticSeverity source) noexcept {
            return static_cast<Diagnostic::Severity>(source);
        }

        SessionInfo publicSessionInfo(const core::SessionInfo& source) {
            SessionInfo result;
            result.sessionId = source.id.value();
            result.role = source.role;
            result.syncMode = source.synchronizationMode;
            result.serverCurrentSequence = source.serverCurrentSequence.protocolValue();
            result.serverVersion = source.serverVersion;
            result.requestedRepresentationCapabilities = source.requestedCapabilities;
            result.selectedRepresentationCapabilities = source.selectedCapabilities;
            result.availableMethods = source.availableMethods;
            result.permittedMethods = source.permittedMethods;
            result.permittedScopes = source.permittedScopes;
            for (frontend::FrontendCapability capability : source.observedCapabilities) {
                const auto metadata = std::ranges::find_if(frontend::generated::AllCapabilities, [capability](const auto& candidate) {
                    return candidate.id == static_cast<frontend::generated::Capability>(capability);
                });
                if (metadata == frontend::generated::AllCapabilities.end()) {
                    continue;
                }
                switch (metadata->category) {
                    case frontend::generated::CapabilityCategory::StaticMechanism:
                        result.observedMechanismCapabilities.push_back(capability);
                        break;
                    case frontend::generated::CapabilityCategory::ConditionalTopology:
                        result.observedTopologyCapabilities.push_back(capability);
                        break;
                    case frontend::generated::CapabilityCategory::Product:
                        result.observedProductCapabilities.push_back(capability);
                        break;
                }
            }
            return result;
        }

        UpdateCause publicCause(core::UpdateCause source) noexcept {
            return static_cast<UpdateCause>(source);
        }

        std::optional<ItemContentChannel> publicChannel(const std::optional<std::string>& source) noexcept {
            if (!source.has_value()) {
                return std::nullopt;
            }
            if (*source == "agentText") {
                return ItemContentChannel::AgentText;
            }
            if (*source == "reasoningText") {
                return ItemContentChannel::ReasoningText;
            }
            if (*source == "reasoningSummary") {
                return ItemContentChannel::ReasoningSummary;
            }
            if (*source == "commandOutput") {
                return ItemContentChannel::CommandOutput;
            }
            return std::nullopt;
        }

        void addSaturated(std::size_t& target, std::size_t value) noexcept {
            target = value > std::numeric_limits<std::size_t>::max() - target ? std::numeric_limits<std::size_t>::max() : target + value;
        }

        std::size_t securelyErase(std::string& value, bool* completeStorageWasZeroed = nullptr) noexcept {
            const std::size_t bytes = value.capacity();
            bool zeroed = false;
            try {
                value.resize(bytes, '\0');
                volatile char* bytes = value.empty() ? nullptr : value.data();
                for (std::size_t index = 0; index < value.size(); ++index) {
                    bytes[index] = '\0';
                }
                zeroed = std::ranges::all_of(value, [](char character) {
                    return character == '\0';
                });
                value.clear();
            } catch (...) {
                volatile char* bytes = value.empty() ? nullptr : value.data();
                for (std::size_t index = 0; index < value.size(); ++index) {
                    bytes[index] = '\0';
                }
                zeroed = value.empty() || std::ranges::all_of(value, [](char character) {
                             return character == '\0';
                         });
                value.clear();
            }
            if (completeStorageWasZeroed != nullptr) {
                *completeStorageWasZeroed = zeroed;
            }
            return bytes;
        }

        std::size_t securelyErase(frontend::Json& value) noexcept {
            std::size_t bytes = 0;
            try {
                if (value.is_string()) {
                    bytes = securelyErase(value.get_ref<std::string&>());
                } else if (value.is_array() || value.is_object()) {
                    for (frontend::Json& member : value) {
                        addSaturated(bytes, securelyErase(member));
                    }
                }
                value = nullptr;
            } catch (...) {
                value = nullptr;
            }
            return bytes;
        }

        std::size_t securelyErase(frontend::AuthenticationCredential& credential) noexcept {
            std::size_t bytes = 0;
            if (auto* bearer = std::get_if<frontend::BearerCredential>(&credential)) {
                bytes = securelyErase(bearer->token);
            }
            credential = frontend::NoCredential{};
            return bytes;
        }

        std::size_t securelyErase(AuthenticationContext& authentication) noexcept {
            std::size_t bytes = securelyErase(authentication.credential);
            if (authentication.continuityKey.has_value()) {
                addSaturated(bytes, securelyErase(*authentication.continuityKey));
                // The initialized, zeroed string can remain engaged until the
                // context's normal destruction. Resetting it here adds an
                // unnecessary second lifetime transition and triggers a GCC
                // 16 -Wmaybe-uninitialized false positive when inlined at -O3.
            }
            return bytes;
        }

        std::size_t securelyErase(frontend::generated::CompleteCommandParameters& parameters) noexcept {
            return std::visit(
                [](auto& value) {
                    return securelyErase(value.value);
                },
                parameters);
        }

        std::size_t securelyErase(frontend::generated::DefinedCommand& command) noexcept {
            std::size_t bytes = securelyErase(command.parameters);
            addSaturated(bytes, securelyErase(command.extensions));
            addSaturated(bytes, securelyErase(command.parameterExtensions));
            return bytes;
        }

        std::size_t securelyErase(frontend::ClientMessage& message) noexcept {
            auto* hello = std::get_if<frontend::Hello>(&message);
            if (hello == nullptr) {
                return 0;
            }
            std::size_t bytes = securelyErase(hello->extensions);
            if (hello->authentication.has_value()) {
                addSaturated(bytes, securelyErase(*hello->authentication));
                hello->authentication.reset();
            }
            return bytes;
        }

        std::size_t securelyErase(core::OutboundMessage& message) noexcept {
            std::size_t bytes = 0;
            if (auto* hello = std::get_if<frontend::Hello>(&message.value)) {
                bytes = securelyErase(hello->extensions);
                if (hello->authentication.has_value()) {
                    addSaturated(bytes, securelyErase(*hello->authentication));
                    hello->authentication.reset();
                }
            } else if (auto* command = std::get_if<frontend::generated::DefinedCommand>(&message.value)) {
                bytes = securelyErase(*command);
            }
            return bytes;
        }
    } // namespace

    struct Connection::Control {
        Client* owner = nullptr;
        TransportCallbacks transport;
        core::PhysicalGeneration generation = 0;
        bool open = true;
        bool connected = false;
    };

    struct Client::Impl {
        explicit Impl(Client& publicOwner,
                      ClientOptions configuredOptions,
                      ClientCallbacks configuredCallbacks,
                      std::function<State(std::shared_ptr<const detail::StateStorage>)> configuredStateFactory)
            : owner(&publicOwner)
            , options(std::move(configuredOptions))
            , callbacks(std::move(configuredCallbacks))
            , stateFactory(std::move(configuredStateFactory)) {
            if (!options.credentialProvider) {
                throw std::invalid_argument("frontend client requires a credential provider");
            }
            coreClient = std::make_unique<core::ClientCore>(coreOptions(), coreCallbacks());
        }

        core::ClientOptions coreOptions() {
            core::ClientOptions result;
            result.requestedCapabilities = options.requestedCapabilities;
            result.requiredCapabilities = options.requiredCapabilities;
            result.allowLegacyV1 = options.allowLegacyV1;
            result.limits.maximumInboundMessageBytes = options.maximumInboundMessageBytes;
            result.limits.maximumDecodedStateBytes = options.maximumDecodedStateBytes;
            result.limits.maximumPendingOperations = options.maximumPendingOperations;
            result.limits.maximumRetainedDiagnostics = options.maximumRetainedDiagnostics;
            result.limits.maximumLocalDiagnostics = options.maximumRetainedDiagnostics;
            result.credentialProvider = [this] {
                AuthenticationContext provided = options.credentialProvider();
                auto providedGuard = ScopeExit([this, &provided]() noexcept {
                    accountErasedTransientBytes(securelyErase(provided));
                });
                // Copy the credential into the core-owned result while the
                // provider's still-sized storage remains available for an
                // actual overwrite. Moving an SSO token first could leave
                // bytes behind an empty source string.
                AuthenticationContext adapted{provided.credential, provided.continuityKey};
                auto adaptedGuard = ScopeExit([this, &adapted]() noexcept {
                    accountErasedTransientBytes(securelyErase(adapted));
                });
                providedGuard.run();
                if (failNextHelloConstruction) {
                    failNextHelloConstruction = false;
                    throw std::runtime_error("injected public Hello construction failure");
                }
                return core::AuthenticationContext{adapted.credential, adapted.continuityKey};
            };
            return result;
        }

        core::ClientCallbacks coreCallbacks() {
            core::ClientCallbacks result;
            result.onConnectionStateChanged = [this](const core::StateChange& change) {
                if (change.current == core::ConnectionState::Disconnected || change.current == core::ConnectionState::Closed) {
                    if (active && active->generation == change.generation) {
                        active->open = false;
                        active->connected = false;
                        active->owner = nullptr;
                        active.reset();
                    }
                }
                if (callbacks.onConnectionStateChanged) {
                    callbacks.onConnectionStateChanged(
                        ConnectionStateChange{publicConnectionState(change.previous),
                                              publicConnectionState(change.current),
                                              change.error ? std::optional<Error>{publicError(*change.error)} : std::nullopt});
                }
            };
            result.prepareStatePublication = [this](const core::PublishedState& publication) -> std::optional<core::ClientError> {
                std::string error;
                detail::CanonicalStateBuildFailure failure = detail::CanonicalStateBuildFailure::StateDivergence;
                auto preparedStorage = detail::CanonicalStateBuilder::build(
                    publication, options.maximumDecodedStateBytes, options.maximumRetainedDiagnostics, error, &failure);
                if (preparedStorage.has_value()) {
                    preparedState = stateFactory(std::move(*preparedStorage));
                    return std::nullopt;
                }
                const bool capacity = failure == detail::CanonicalStateBuildFailure::Capacity;
                return core::ClientError{core::ErrorOrigin::Protocol,
                                         capacity ? core::ClientErrorCode::StateCapacityExceeded : core::ClientErrorCode::StateDivergence,
                                         capacity ? std::optional{frontend::ErrorCode::CapacityExceeded} : std::nullopt,
                                         error.empty() ? "canonical public State preparation failed" : std::move(error),
                                         std::nullopt,
                                         false};
            };
            result.commitStatePublication = [this](const core::PublishedState&) noexcept {
                if (preparedState.has_value()) {
                    currentState = std::move(*preparedState);
                    preparedState.reset();
                }
            };
            result.onStateUpdated = [this](const core::StateUpdate& update) {
                if (!callbacks.onStateUpdated) {
                    return;
                }
                StateUpdate publicUpdate;
                publicUpdate.state = currentState;
                publicUpdate.cause = publicCause(update.cause);
                if (update.fromSequence.has_value()) {
                    publicUpdate.fromSequence = update.fromSequence->protocolValue();
                }
                if (update.toSequence.has_value()) {
                    publicUpdate.toSequence = update.toSequence->protocolValue();
                }
                publicUpdate.changes.reserve(update.changes.size());
                for (const core::Change& change : update.changes) {
                    std::visit(
                        [this, &publicUpdate](const auto& value) {
                            using Value = std::remove_cvref_t<decltype(value)>;
                            if constexpr (std::is_same_v<Value, core::StateReplacedChange>) {
                                publicUpdate.changes.emplace_back(StateReplacedChange{});
                            } else if constexpr (std::is_same_v<Value, core::CursorAdvancedChange>) {
                                publicUpdate.changes.emplace_back(CursorAdvancedChange{value.sequence.protocolValue()});
                            } else if constexpr (std::is_same_v<Value, model::ProviderUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(
                                    ProviderUpdatedChange{currentState.provider().value.value_or(ProviderState{})});
                            } else if constexpr (std::is_same_v<Value, model::ControllerUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(
                                    ControllerUpdatedChange{currentState.controller().value.value_or(ControllerState{})});
                            } else if constexpr (std::is_same_v<Value, model::SessionsUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(SessionsUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::ThreadListUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(ThreadListUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::ThreadUpsertedOccurrence>) {
                                publicUpdate.changes.emplace_back(ThreadUpsertedChange{typed::ThreadId{value.thread.id.value()}});
                            } else if constexpr (std::is_same_v<Value, model::ThreadRemovedOccurrence>) {
                                publicUpdate.changes.emplace_back(ThreadRemovedChange{typed::ThreadId{value.threadId.value()}});
                            } else if constexpr (std::is_same_v<Value, model::TurnUpsertedOccurrence>) {
                                publicUpdate.changes.emplace_back(TurnUpsertedChange{typed::TurnId{value.turn.id.value()}});
                            } else if constexpr (std::is_same_v<Value, model::ItemUpsertedOccurrence>) {
                                publicUpdate.changes.emplace_back(
                                    ItemUpsertedChange{typed::ItemId{model::itemData(value.item).id.value()}});
                            } else if constexpr (std::is_same_v<Value, model::ItemContentUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(
                                    ItemContentReplacedChange{typed::ItemId{value.itemId.value()},
                                                              publicChannel(value.channel).value_or(ItemContentChannel::AgentText)});
                            } else if constexpr (std::is_same_v<Value, model::PendingRequestsUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(PendingRequestsUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::AccountUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(AccountUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::ModelsUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(ModelsUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::ConfigurationUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(ConfigurationUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::ProcessUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(ProcessUpdatedChange{ProcessHandle{value.process.handle.value()}});
                            } else if constexpr (std::is_same_v<Value, model::FilesystemWatchUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(
                                    FilesystemWatchUpdatedChange{typed::FsWatchId{value.filesystemWatch.watchId}});
                            } else if constexpr (std::is_same_v<Value, model::FuzzySearchUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(
                                    FuzzySearchUpdatedChange{FuzzySearchSessionId{value.fuzzySearch.sessionId}});
                            } else if constexpr (std::is_same_v<Value, model::ReviewsUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(ReviewsUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::IntegrationsUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(IntegrationsUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::PluginsUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(PluginsUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::SkillsUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(SkillsUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::McpUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(McpUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::PlatformUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(PlatformUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::NoticeAddedOccurrence>) {
                                publicUpdate.changes.emplace_back(NoticeAddedChange{
                                    value.notice.occurrence == 0 ? std::nullopt : std::optional<std::uint64_t>{value.notice.occurrence}});
                            } else if constexpr (std::is_same_v<Value, model::ActivityUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(ActivityUpdatedChange{ActivityKey{value.activity.key}});
                            } else if constexpr (std::is_same_v<Value, model::CapacityUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(CapacityUpdatedChange{});
                            } else if constexpr (std::is_same_v<Value, model::DiagnosticsUpdatedOccurrence>) {
                                publicUpdate.changes.emplace_back(DiagnosticUpdatedChange{value.diagnostic.received});
                            } else if constexpr (std::is_same_v<Value, core::CompatibilityExtensionChange>) {
                                publicUpdate.changes.emplace_back(CompatibilityExtensionChange{value.type});
                            }
                        },
                        change);
                }
                callbacks.onStateUpdated(publicUpdate);
            };
            result.onCursorAdvanced = [this](model::FrontendSequence sequence) {
                if (callbacks.onCursorAdvanced) {
                    callbacks.onCursorAdvanced(sequence.protocolValue());
                }
            };
            result.onSynchronized = [this](const core::SynchronizationInfo& info) {
                if (callbacks.onSynchronized) {
                    callbacks.onSynchronized(SynchronizationInfo{
                        info.mode, info.synchronizedThrough.protocolValue(), currentState, info.generation > 1, info.snapshotFallback});
                }
            };
            result.onProtocolMessage = [this](const frontend::ServerMessage& message) {
                if (callbacks.onProtocolMessage) {
                    callbacks.onProtocolMessage(message);
                }
            };
            result.onDiagnostic = [this](const core::Diagnostic& diagnostic) {
                if (callbacks.onDiagnostic) {
                    callbacks.onDiagnostic(
                        Diagnostic{publicSeverity(diagnostic.severity),
                                   diagnostic.message,
                                   diagnostic.error ? std::optional<Error>{publicError(*diagnostic.error)} : std::nullopt});
                }
            };
            return result;
        }

        void accountErasedTransientBytes(std::size_t bytes) noexcept {
            addSaturated(erasedTransientBytes, bytes);
        }

        void eraseTransientString(std::string& value, bool movedFrom = false) noexcept {
            bool completeStorageWasZeroed = false;
            accountErasedTransientBytes(securelyErase(value, &completeStorageWasZeroed));
            if (movedFrom && completeStorageWasZeroed && verifiedMovedFromStringScrubs != std::numeric_limits<std::size_t>::max()) {
                ++verifiedMovedFromStringScrubs;
            }
        }

        void eraseOwnedOutbound(OutboundMessage& message, bool movedFrom = false) noexcept {
            eraseTransientString(message.compactJson, movedFrom);
            message.serializedBytes = 0;
        }

        core::SendResult send(const std::weak_ptr<Connection::Control>& weak, core::OutboundMessage message) noexcept {
            auto messageGuard = ScopeExit([this, &message]() noexcept {
                if (message.sensitive) {
                    accountErasedTransientBytes(securelyErase(message));
                }
            });
            const std::shared_ptr<Connection::Control> control = weak.lock();
            if (!control || !control->open || control->owner != owner) {
                return {core::SendStatus::Closed, core::TransportError{"frontend connection is closed", false}};
            }
            try {
                frontend::Json encoded;
                auto encodedGuard = ScopeExit([this, &encoded, &message]() noexcept {
                    if (message.sensitive) {
                        accountErasedTransientBytes(securelyErase(encoded));
                    }
                });
                OutboundKind kind = OutboundKind::Command;
                if (const auto* hello = std::get_if<frontend::Hello>(&message.value)) {
                    frontend::ClientMessage wireMessage{*hello};
                    auto wireMessageGuard = ScopeExit([this, &wireMessage, &message]() noexcept {
                        if (message.sensitive) {
                            accountErasedTransientBytes(securelyErase(wireMessage));
                        }
                    });
                    auto wire = frontend::Codec::encodeClient(wireMessage);
                    wireMessageGuard.run();
                    if (!wire) {
                        return {core::SendStatus::Failed, core::TransportError{"failed to encode frontend Hello", false}};
                    }
                    encoded = std::move(wire).value();
                    kind = OutboundKind::Hello;
                } else {
                    auto wire = frontend::Codec::encodeDefinedCommand(std::get<frontend::generated::DefinedCommand>(message.value));
                    if (!wire) {
                        return {core::SendStatus::Failed, core::TransportError{"failed to encode frontend command", false}};
                    }
                    encoded = std::move(wire).value();
                }
                std::string compact = encoded.dump();
                encodedGuard.run();
                const std::size_t serializedBytes = compact.size();
                OutboundMessage outbound{kind, std::move(compact), serializedBytes, message.sensitive};
                if (message.sensitive) {
                    eraseTransientString(compact, true);
                }
                SendResult sent;
                try {
                    sent = control->transport.send(std::move(outbound));
                } catch (...) {
                    if (message.sensitive) {
                        eraseOwnedOutbound(outbound, true);
                    }
                    throw;
                }
                if (message.sensitive) {
                    eraseOwnedOutbound(outbound, true);
                }
                core::SendResult result{static_cast<core::SendStatus>(sent.status), std::nullopt};
                if (sent.error.has_value()) {
                    result.error = core::TransportError{sent.error->message, sent.error->retryable};
                }
                return result;
            } catch (...) {
                return {core::SendStatus::Failed, core::TransportError{"frontend transport send callback failed", true}};
            }
        }

        void closeTransport(const std::weak_ptr<Connection::Control>& weak, std::string_view reason) {
            const std::shared_ptr<Connection::Control> control = weak.lock();
            if (!control || !control->open || control->owner != owner) {
                return;
            }
            try {
                control->transport.close(std::string(reason));
            } catch (...) {
                control->open = false;
                control->connected = false;
                control->owner = nullptr;
                throw;
            }
            control->open = false;
            control->connected = false;
            control->owner = nullptr;
        }

        Submission submitGenerated(frontend::generated::CompleteCommandParameters parameters, GeneratedCompletionHandler handler) {
            return submitCore(std::move(parameters), [handler = std::move(handler)](const core::OperationResult& operation) {
                if (!handler) {
                    return;
                }
                handler(GeneratedOperationResult{RequestId{operation.requestId},
                                                 operation.value,
                                                 operation.error ? std::optional<Error>{publicError(*operation.error)} : std::nullopt});
            });
        }

        Submission submitBound(frontend::generated::CompleteCommandParameters parameters, detail::BoundOperationCompletion completion) {
            return submitCore(
                std::move(parameters), [this, completion = std::move(completion)](const core::OperationResult& operation) mutable {
                    const RequestId requestId{operation.requestId};
                    if (operation.error.has_value()) {
                        if (completion.fail) {
                            completion.fail(requestId, publicError(*operation.error));
                        }
                        return;
                    }
                    if (!operation.value.has_value()) {
                        const Error error = resultError("frontend operation completed without a generated result");
                        auto rejectionGuard = ScopeExit([this, generation = operation.generation, &error]() noexcept {
                            rejectResult(generation, error.message);
                        });
                        if (completion.fail) {
                            completion.fail(requestId, error);
                        }
                        return;
                    }
                    std::any decoded;
                    std::string decodingError;
                    bool valid = false;
                    try {
                        valid = completion.decode && completion.decode(*operation.value, decoded, decodingError);
                    } catch (...) {
                        decodingError = "frontend typed result decoder threw";
                    }
                    if (!valid) {
                        const Error error =
                            resultError(decodingError.empty() ? "frontend typed result is invalid" : std::move(decodingError));
                        auto rejectionGuard = ScopeExit([this, generation = operation.generation, &error]() noexcept {
                            rejectResult(generation, error.message);
                        });
                        if (completion.fail) {
                            completion.fail(requestId, error);
                        }
                        return;
                    }
                    if (completion.succeed) {
                        completion.succeed(requestId, std::move(decoded));
                    }
                });
        }

        void completeSynchronizationAdapter(const core::OperationResult& operation,
                                            frontend::SyncMode requestedMode,
                                            const CompletionHandler<SynchronizationResult>& handler) {
            const RequestId requestId{operation.requestId};
            if (operation.error.has_value()) {
                if (handler) {
                    handler(OperationResult<SynchronizationResult>{requestId, std::nullopt, publicError(*operation.error)});
                }
                return;
            }
            if (!operation.synchronization.has_value()) {
                const Error error = resultError("explicit synchronization completed without synchronization context");
                auto rejectionGuard = ScopeExit([this, generation = operation.generation, &error]() noexcept {
                    rejectResult(generation, error.message);
                });
                if (handler) {
                    handler(OperationResult<SynchronizationResult>{requestId, std::nullopt, error});
                }
                return;
            }
            const core::SynchronizationInfo& info = *operation.synchronization;
            const bool consistent =
                requestedMode == frontend::SyncMode::Snapshot
                    ? info.mode == frontend::SyncMode::Snapshot
                    : (info.mode == frontend::SyncMode::Replay || (info.mode == frontend::SyncMode::Snapshot && info.snapshotFallback));
            if (!consistent) {
                const Error error = resultError("explicit synchronization completed with an inconsistent stream mode");
                auto rejectionGuard = ScopeExit([this, generation = operation.generation, &error]() noexcept {
                    rejectResult(generation, error.message);
                });
                if (handler) {
                    handler(OperationResult<SynchronizationResult>{requestId, std::nullopt, error});
                }
                return;
            }
            const std::size_t received = info.appliedOccurrences > std::numeric_limits<std::size_t>::max() - info.ignoredOccurrences
                                             ? std::numeric_limits<std::size_t>::max()
                                             : info.appliedOccurrences + info.ignoredOccurrences;
            if (handler) {
                handler(OperationResult<SynchronizationResult>{requestId,
                                                               SynchronizationResult{info.mode,
                                                                                     info.synchronizedThrough.protocolValue(),
                                                                                     currentState,
                                                                                     received,
                                                                                     info.appliedOccurrences,
                                                                                     info.ignoredOccurrences,
                                                                                     info.snapshotFallback},
                                                               std::nullopt});
            }
        }

        template <typename Completion>
        Submission submitCore(frontend::generated::CompleteCommandParameters parameters, Completion completion) {
            const frontend::generated::MethodId method = frontend::generated::commandMethod(parameters);
            const bool sensitive = frontend::client::generated::bindingIsSensitive(method);
            auto parameterGuard = ScopeExit([this, &parameters, sensitive]() noexcept {
                if (sensitive) {
                    accountErasedTransientBytes(securelyErase(parameters));
                }
            });
            core::Submission submitted = coreClient->submit(
                std::move(parameters), [completion = std::move(completion)](const core::OperationResult& operation) mutable {
                    completion(operation);
                });
            parameterGuard.run();
            Submission result;
            if (submitted.requestId.has_value()) {
                result.requestId = RequestId{*submitted.requestId};
            }
            if (submitted.error.has_value()) {
                result.error = publicError(*submitted.error);
            }
            return result;
        }

        void rejectResult(core::PhysicalGeneration generation, std::string_view message) noexcept {
            coreClient->rejectAdapterResult(generation, message);
        }

        Client* owner;
        ClientOptions options;
        ClientCallbacks callbacks;
        std::function<State(std::shared_ptr<const detail::StateStorage>)> stateFactory;
        State currentState;
        std::optional<State> preparedState;
        std::shared_ptr<Connection::Control> active;
        std::unique_ptr<core::ClientCore> coreClient;
        bool failNextHelloConstruction = false;
        std::size_t erasedTransientBytes = 0;
        std::size_t verifiedMovedFromStringScrubs = 0;

        std::unique_ptr<Controller> controller;
        std::unique_ptr<Provider> provider;
        std::unique_ptr<Synchronization> synchronization;
        std::unique_ptr<Accounts> accounts;
        std::unique_ptr<Apps> apps;
        std::unique_ptr<Commands> commands;
        std::unique_ptr<Configuration> configuration;
        std::unique_ptr<ExternalAgents> externalAgents;
        std::unique_ptr<Feedback> feedback;
        std::unique_ptr<Filesystem> filesystem;
        std::unique_ptr<Hooks> hooks;
        std::unique_ptr<Marketplace> marketplace;
        std::unique_ptr<Mcp> mcp;
        std::unique_ptr<Models> models;
        std::unique_ptr<PermissionProfiles> permissionProfiles;
        std::unique_ptr<Plugins> plugins;
        std::unique_ptr<Requests> requests;
        std::unique_ptr<Reviews> reviews;
        std::unique_ptr<Skills> skills;
        std::unique_ptr<Threads> threads;
        std::unique_ptr<Turns> turns;
        std::unique_ptr<WindowsSandbox> windowsSandbox;
    };

    Client::Client(ClientOptions options, ClientCallbacks callbacks)
        : impl(std::make_unique<Impl>(
              *this, std::move(options), std::move(callbacks), [](std::shared_ptr<const detail::StateStorage> storage) noexcept {
                  return State{std::move(storage)};
              })) {
        impl->controller = std::unique_ptr<Controller>(new Controller(*this));
        impl->provider = std::unique_ptr<Provider>(new Provider(*this));
        impl->synchronization = std::unique_ptr<Synchronization>(new Synchronization(*this));
        impl->accounts = std::unique_ptr<Accounts>(new Accounts(*this));
        impl->apps = std::unique_ptr<Apps>(new Apps(*this));
        impl->commands = std::unique_ptr<Commands>(new Commands(*this));
        impl->configuration = std::unique_ptr<Configuration>(new Configuration(*this));
        impl->externalAgents = std::unique_ptr<ExternalAgents>(new ExternalAgents(*this));
        impl->feedback = std::unique_ptr<Feedback>(new Feedback(*this));
        impl->filesystem = std::unique_ptr<Filesystem>(new Filesystem(*this));
        impl->hooks = std::unique_ptr<Hooks>(new Hooks(*this));
        impl->marketplace = std::unique_ptr<Marketplace>(new Marketplace(*this));
        impl->mcp = std::unique_ptr<Mcp>(new Mcp(*this));
        impl->models = std::unique_ptr<Models>(new Models(*this));
        impl->permissionProfiles = std::unique_ptr<PermissionProfiles>(new PermissionProfiles(*this));
        impl->plugins = std::unique_ptr<Plugins>(new Plugins(*this));
        impl->requests = std::unique_ptr<Requests>(new Requests(*this));
        impl->reviews = std::unique_ptr<Reviews>(new Reviews(*this));
        impl->skills = std::unique_ptr<Skills>(new Skills(*this));
        impl->threads = std::unique_ptr<Threads>(new Threads(*this));
        impl->turns = std::unique_ptr<Turns>(new Turns(*this));
        impl->windowsSandbox = std::unique_ptr<WindowsSandbox>(new WindowsSandbox(*this));
    }

    Client::~Client() {
        close("frontend client destroyed");
    }

    Connection Client::openConnection(TransportCallbacks callbacks) {
        if (!callbacks.send || !callbacks.close || impl->active ||
            impl->coreClient->connectionState() != core::ConnectionState::Disconnected) {
            return {};
        }
        auto control = std::make_shared<Connection::Control>();
        control->owner = this;
        control->transport = std::move(callbacks);
        const std::weak_ptr<Connection::Control> weak = control;
        const std::optional<core::PhysicalGeneration> generation =
            impl->coreClient->attach(core::TransportCallbacks{[implementation = impl.get(), weak](core::OutboundMessage message) {
                                                                  return implementation->send(weak, std::move(message));
                                                              },
                                                              [implementation = impl.get(), weak](std::string_view reason) {
                                                                  implementation->closeTransport(weak, reason);
                                                              }});
        if (!generation.has_value()) {
            control->owner = nullptr;
            control->open = false;
            return {};
        }
        control->generation = *generation;
        impl->active = control;
        return Connection{std::move(control)};
    }

    void Client::setCallbacks(ClientCallbacks callbacks) {
        impl->callbacks = std::move(callbacks);
    }

    void Client::close(std::string reason) noexcept {
        try {
            impl->coreClient->close(reason);
        } catch (...) {
        }
        if (impl->active) {
            impl->active->open = false;
            impl->active->connected = false;
            impl->active->owner = nullptr;
            impl->active.reset();
        }
    }

    bool Client::isOpen() const noexcept {
        return impl->coreClient->connectionState() != core::ConnectionState::Closed;
    }

    bool Client::hasActiveConnection() const noexcept {
        return impl->coreClient->activeGeneration().has_value();
    }

    bool Client::isReady() const noexcept {
        return impl->coreClient->ready();
    }

    ConnectionState Client::connectionState() const noexcept {
        return publicConnectionState(impl->coreClient->connectionState());
    }

    State Client::state() const noexcept {
        return impl->currentState;
    }

    std::optional<SessionInfo> Client::session() const {
        const std::optional<core::SessionInfo> session = impl->coreClient->sessionInfo();
        return session ? std::optional<SessionInfo>{publicSessionInfo(*session)} : std::nullopt;
    }

    std::optional<frontend::SequenceNumber> Client::visibleSequence() const {
        return impl->currentState.visibleSequence();
    }

    std::optional<frontend::SequenceNumber> Client::synchronizedThrough() const {
        return impl->currentState.synchronizedThrough();
    }

    std::size_t Client::pendingOperationCount() const noexcept {
        return impl->coreClient->pendingOperationCount();
    }

    MethodStatus Client::methodStatus(frontend::generated::MethodId method) const {
        const core::MethodStatus source = impl->coreClient->methodStatus(method);
        return MethodStatus{source.method,
                            publicAvailability(source.available),
                            publicAvailability(source.permitted),
                            source.controllerRequired,
                            source.providerReadyRequired,
                            source.defaultEnabled,
                            source.requiredScopes};
    }

    CapabilityStatus Client::capabilityStatus(frontend::FrontendCapability capability) const {
        const core::CapabilityStatus source = impl->coreClient->capabilityStatus(capability);
        return CapabilityStatus{source.capability,
                                publicAvailability(source.defined),
                                publicAvailability(source.implemented),
                                publicAvailability(source.permitted)};
    }

    Submission Client::submit(frontend::generated::CompleteCommandParameters parameters, GeneratedCompletionHandler handler) {
        return impl->submitGenerated(std::move(parameters), std::move(handler));
    }

    Submission Client::submitBound(frontend::generated::CompleteCommandParameters parameters, detail::BoundOperationCompletion completion) {
        return impl->submitBound(std::move(parameters), std::move(completion));
    }

    Submission Client::submitReverseBound(const PendingRequestId&,
                                          frontend::generated::CompleteCommandParameters parameters,
                                          detail::BoundOperationCompletion completion) {
        return impl->submitBound(std::move(parameters), std::move(completion));
    }

    Submission Client::beginSynchronization(frontend::SyncMode mode,
                                            std::optional<frontend::SequenceNumber> after,
                                            CompletionHandler<SynchronizationResult> handler) {
        frontend::generated::CompleteCommandParameters parameters =
            mode == frontend::SyncMode::Snapshot
                ? frontend::generated::makeParameters(frontend::generated::MethodId::SnapshotGet, frontend::Json::object())
                : frontend::generated::makeParameters(frontend::generated::MethodId::EventsReplay,
                                                      frontend::Json{{"after", after.value_or(frontend::SequenceNumber{}).value()}});
        return impl->submitCore(
            std::move(parameters),
            [implementation = impl.get(), requestedMode = mode, handler = std::move(handler)](const core::OperationResult& operation) {
                implementation->completeSynchronizationAdapter(operation, requestedMode, handler);
            });
    }

    Submission Synchronization::snapshot(CompletionHandler<SynchronizationResult> handler) {
        return client->beginSynchronization(frontend::SyncMode::Snapshot, std::nullopt, std::move(handler));
    }

    Submission Synchronization::replay(frontend::SequenceNumber after, CompletionHandler<SynchronizationResult> handler) {
        return client->beginSynchronization(frontend::SyncMode::Replay, after, std::move(handler));
    }

    namespace {
        template <frontend::generated::MethodId Method>
        std::optional<frontend::generated::CompleteCommandParameters> unitParameters(std::string& error) noexcept {
            std::optional<frontend::Json> encoded = detail::encodeUnitParams(typed::Unit{}, error);
            if (!encoded) {
                return std::nullopt;
            }
            return frontend::generated::CompleteCommandParameters{frontend::generated::MethodParameters<Method>{std::move(*encoded)}};
        }

        template <frontend::generated::MethodId Method>
        detail::BoundOperationCompletion unitCompletion(DoneHandler handler) {
            return detail::bindCompletion<typed::Unit>(
                std::move(handler), [](const frontend::generated::CompleteCommandResult& result, std::string& error) noexcept {
                    const auto* generated = std::get_if<frontend::generated::MethodResult<Method>>(&result);
                    if (generated == nullptr) {
                        error = "frontend unit result did not match the submitted generated MethodId";
                        return std::optional<typed::Unit>{};
                    }
                    return detail::decodeUnitResult(generated->value, error);
                });
        }

        template <frontend::generated::MethodId Method>
        detail::BoundOperationCompletion controllerCompletion(Client& client, CompletionHandler<ControllerResult> handler) {
            return detail::bindCompletion<ControllerResult>(
                std::move(handler), [&client](const frontend::generated::CompleteCommandResult& result, std::string& error) noexcept {
                    const auto* generated = std::get_if<frontend::generated::MethodResult<Method>>(&result);
                    if (generated == nullptr) {
                        error = "frontend controller result did not match the submitted generated MethodId";
                        return std::optional<ControllerResult>{};
                    }
                    const std::optional<SessionInfo> session = client.session();
                    const std::optional<std::string_view> sessionId =
                        session ? std::optional<std::string_view>(session->sessionId) : std::nullopt;
                    return detail::decodeControllerResult(generated->value, sessionId, error);
                });
        }
    } // namespace

    Submission Controller::acquire(CompletionHandler<ControllerResult> handler) {
        std::string error;
        auto parameters = unitParameters<frontend::generated::MethodId::ControllerAcquire>(error);
        if (!parameters) {
            return {std::nullopt,
                    localError(ClientErrorCode::SerializationFailed,
                               error.empty() ? "controller.acquire parameters could not be encoded" : std::move(error))};
        }
        return client->submitBound(std::move(*parameters),
                                   controllerCompletion<frontend::generated::MethodId::ControllerAcquire>(*client, std::move(handler)));
    }

    Submission Controller::release(CompletionHandler<ControllerResult> handler) {
        std::string error;
        auto parameters = unitParameters<frontend::generated::MethodId::ControllerRelease>(error);
        if (!parameters) {
            return {std::nullopt,
                    localError(ClientErrorCode::SerializationFailed,
                               error.empty() ? "controller.release parameters could not be encoded" : std::move(error))};
        }
        return client->submitBound(std::move(*parameters),
                                   controllerCompletion<frontend::generated::MethodId::ControllerRelease>(*client, std::move(handler)));
    }

    bool Controller::ownedByThisClient() const noexcept {
        const State state = client->state();
        return state.controller().value && state.controller().value->ownedByThisClient;
    }

    Submission Provider::start(DoneHandler handler) {
        std::string error;
        auto parameters = unitParameters<frontend::generated::MethodId::ProviderStart>(error);
        if (!parameters) {
            return {std::nullopt, localError(ClientErrorCode::SerializationFailed, std::move(error))};
        }
        return client->submitBound(std::move(*parameters),
                                   unitCompletion<frontend::generated::MethodId::ProviderStart>(std::move(handler)));
    }

    Submission Provider::stop(DoneHandler handler) {
        std::string error;
        auto parameters = unitParameters<frontend::generated::MethodId::ProviderStop>(error);
        if (!parameters) {
            return {std::nullopt, localError(ClientErrorCode::SerializationFailed, std::move(error))};
        }
        return client->submitBound(std::move(*parameters), unitCompletion<frontend::generated::MethodId::ProviderStop>(std::move(handler)));
    }

    Submission Provider::restart(DoneHandler handler) {
        std::string error;
        auto parameters = unitParameters<frontend::generated::MethodId::ProviderRestart>(error);
        if (!parameters) {
            return {std::nullopt, localError(ClientErrorCode::SerializationFailed, std::move(error))};
        }
        return client->submitBound(std::move(*parameters),
                                   unitCompletion<frontend::generated::MethodId::ProviderRestart>(std::move(handler)));
    }

#define AISUITE_CLIENT_FACADE_GETTER(Type, name)                                                                                           \
    Type& Client::name() noexcept {                                                                                                        \
        return *impl->name;                                                                                                                \
    }
    AISUITE_CLIENT_FACADE_GETTER(Controller, controller)
    AISUITE_CLIENT_FACADE_GETTER(Provider, provider)
    AISUITE_CLIENT_FACADE_GETTER(Synchronization, synchronization)
    AISUITE_CLIENT_FACADE_GETTER(Accounts, accounts)
    AISUITE_CLIENT_FACADE_GETTER(Apps, apps)
    AISUITE_CLIENT_FACADE_GETTER(Commands, commands)
    AISUITE_CLIENT_FACADE_GETTER(Configuration, configuration)
    AISUITE_CLIENT_FACADE_GETTER(ExternalAgents, externalAgents)
    AISUITE_CLIENT_FACADE_GETTER(Feedback, feedback)
    AISUITE_CLIENT_FACADE_GETTER(Filesystem, filesystem)
    AISUITE_CLIENT_FACADE_GETTER(Hooks, hooks)
    AISUITE_CLIENT_FACADE_GETTER(Marketplace, marketplace)
    AISUITE_CLIENT_FACADE_GETTER(Mcp, mcp)
    AISUITE_CLIENT_FACADE_GETTER(Models, models)
    AISUITE_CLIENT_FACADE_GETTER(PermissionProfiles, permissionProfiles)
    AISUITE_CLIENT_FACADE_GETTER(Plugins, plugins)
    AISUITE_CLIENT_FACADE_GETTER(Requests, requests)
    AISUITE_CLIENT_FACADE_GETTER(Reviews, reviews)
    AISUITE_CLIENT_FACADE_GETTER(Skills, skills)
    AISUITE_CLIENT_FACADE_GETTER(Threads, threads)
    AISUITE_CLIENT_FACADE_GETTER(Turns, turns)
    AISUITE_CLIENT_FACADE_GETTER(WindowsSandbox, windowsSandbox)
#undef AISUITE_CLIENT_FACADE_GETTER

    Connection::Connection() noexcept = default;

    Connection::Connection(std::shared_ptr<Control> connectionControl) noexcept
        : control(std::move(connectionControl)) {
    }

    Connection::Connection(Connection&&) noexcept = default;

    Connection& Connection::operator=(Connection&& other) noexcept {
        if (this != &other) {
            close("frontend transport attachment replaced");
            control = std::move(other.control);
        }
        return *this;
    }

    Connection::~Connection() {
        close("frontend transport attachment destroyed");
    }

    void Connection::transportConnected() noexcept {
        if (!control || !control->owner || !control->open || control->connected) {
            return;
        }
        control->connected = true;
        try {
            control->owner->impl->coreClient->transportConnected(control->generation);
        } catch (...) {
        }
    }

    ReceiveResult Connection::receive(std::string_view compactJson) noexcept {
        if (!control || !control->owner || !control->open || !control->connected) {
            return {false, TransportError{"frontend connection is closed", false}};
        }
        try {
            const bool accepted = control->owner->impl->coreClient->receiveEncoded(control->generation, compactJson);
            return accepted ? ReceiveResult{true, std::nullopt}
                            : ReceiveResult{false, TransportError{"frontend server message was rejected", false}};
        } catch (...) {
            return {false, TransportError{"frontend server message dispatch failed", false}};
        }
    }

    ReceiveResult Connection::receive(const frontend::Json& message) noexcept {
        try {
            const std::string compact = message.dump();
            return receive(std::string_view(compact));
        } catch (...) {
            return {false, TransportError{"frontend server message encoding failed", false}};
        }
    }

    ReceiveResult Connection::receive(const frontend::ServerMessage& message) noexcept {
        if (!control || !control->owner || !control->open || !control->connected) {
            return {false, TransportError{"frontend connection is closed", false}};
        }
        try {
            const bool accepted = control->owner->impl->coreClient->receive(control->generation, message);
            return accepted ? ReceiveResult{true, std::nullopt}
                            : ReceiveResult{false, TransportError{"frontend server message was rejected", false}};
        } catch (...) {
            return {false, TransportError{"frontend server message dispatch failed", false}};
        }
    }

    void Connection::transportDisconnected(std::optional<TransportError> error) noexcept {
        if (!control || !control->owner || !control->open) {
            return;
        }
        const std::shared_ptr<Control> retired = control;
        Client* owner = retired->owner;
        const core::PhysicalGeneration generation = retired->generation;
        retired->open = false;
        retired->connected = false;
        retired->owner = nullptr;
        if (owner->impl->active == retired) {
            owner->impl->active.reset();
        }
        try {
            if (error.has_value()) {
                owner->impl->coreClient->transportDisconnected(generation,
                                                               core::TransportError{std::move(error->message), error->retryable});
            } else {
                owner->impl->coreClient->transportDisconnected(generation);
            }
        } catch (...) {
        }
    }

    void Connection::close(std::string reason) noexcept {
        if (!control || !control->owner || !control->open) {
            return;
        }
        Client* owner = control->owner;
        const core::PhysicalGeneration generation = control->generation;
        try {
            owner->impl->coreClient->detach(generation, reason);
        } catch (...) {
        }
        const std::optional<core::PhysicalGeneration> retained = owner->impl->coreClient->activeGeneration();
        if (control && control->owner == owner && (!retained.has_value() || *retained != generation)) {
            control->open = false;
            control->connected = false;
            control->owner = nullptr;
        }
    }

    bool Connection::isOpen() const noexcept {
        return control && control->open;
    }

    bool Connection::isTransportConnected() const noexcept {
        return control && control->connected;
    }

    std::uint64_t Connection::generation() const noexcept {
        return control ? control->generation : 0;
    }

    void detail::ClientTestAccess::setNextRequest(Client& client, std::uint64_t next) noexcept {
        core::ClientCoreTestAccess::setNextRequestId(*client.impl->coreClient, next);
    }

    void detail::ClientTestAccess::setNextConnectionGeneration(Client& client, std::uint64_t next) noexcept {
        core::ClientCoreTestAccess::setGenerationCounter(*client.impl->coreClient, next);
    }

    void detail::ClientTestAccess::setSynchronizationCounts(Client& client,
                                                            std::size_t received,
                                                            std::size_t applied,
                                                            std::size_t ignored) noexcept {
        core::ClientCoreTestAccess::setSynchronizationCounts(*client.impl->coreClient, received, applied, ignored);
    }

    bool detail::ClientTestAccess::tryAccumulateSynchronizationCounts(Client& client,
                                                                      std::size_t received,
                                                                      std::size_t applied,
                                                                      std::size_t ignored) noexcept {
        return core::ClientCoreTestAccess::tryAccumulateSynchronizationCounts(*client.impl->coreClient, received, applied, ignored);
    }

    std::array<std::size_t, 3> detail::ClientTestAccess::synchronizationCounts(const Client& client) noexcept {
        return core::ClientCoreTestAccess::synchronizationCounts(*client.impl->coreClient);
    }

    void detail::ClientTestAccess::failNextHelloConstruction(Client& client) noexcept {
        client.impl->failNextHelloConstruction = true;
    }

    void detail::ClientTestAccess::failAfterNextDispatch(Client& client) noexcept {
        core::ClientCoreTestAccess::failAfterNextDispatch(*client.impl->coreClient);
    }

    bool detail::ClientTestAccess::rejectInvalidSynchronizationAdapterResultWithThrowingCallback(Client& client) noexcept {
        const std::optional<core::PhysicalGeneration> generation = client.impl->coreClient->activeGeneration();
        if (!generation.has_value()) {
            return false;
        }
        bool callbackInvoked = false;
        try {
            core::OperationResult invalid;
            invalid.requestId = "adapter-test";
            invalid.method = frontend::generated::MethodId::SnapshotGet;
            invalid.generation = *generation;
            client.impl->completeSynchronizationAdapter(
                invalid, frontend::SyncMode::Snapshot, [&callbackInvoked](const OperationResult<SynchronizationResult>&) {
                    callbackInvoked = true;
                    throw std::runtime_error("invalid synchronization adapter callback sentinel");
                });
        } catch (...) {
        }
        return callbackInvoked;
    }

    std::size_t detail::ClientTestAccess::erasedTransientBytes(const Client& client) noexcept {
        std::size_t result = client.impl->erasedTransientBytes;
        addSaturated(result, core::ClientCoreTestAccess::erasedTransientBytes(*client.impl->coreClient));
        return result;
    }

    std::size_t detail::ClientTestAccess::verifiedMovedFromStringScrubs(const Client& client) noexcept {
        return client.impl->verifiedMovedFromStringScrubs;
    }

    bool detail::ClientTestAccess::shortStringStorageScrubbed() noexcept {
        std::string source = "SSO_SECRET_12";
        const std::size_t sentinelBytes = source.size();
        std::string destination(std::move(source));
        bool sourceZeroed = false;
        bool destinationZeroed = false;
        const std::size_t sourceScrubbed = securelyErase(source, &sourceZeroed);
        const std::size_t destinationScrubbed = securelyErase(destination, &destinationZeroed);
        return sourceZeroed && destinationZeroed && sourceScrubbed != 0 && destinationScrubbed >= sentinelBytes;
    }

    State detail::ClientTestAccess::adoptStateStorage(Client& client, std::shared_ptr<const StateStorage> storage) noexcept {
        return client.impl->stateFactory(std::move(storage));
    }

} // namespace ai::openai::codex::frontend::client
