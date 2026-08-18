/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/internal/server/BackendCoreBridge.h"

#include "ai/openai/codex/backend/BackendCommand.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/backend/BackendEvent.h"
#include "ai/openai/codex/backend/FrontendSession.h"
#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/frontend/detail/BackendCommandMapper.h"
#include "ai/openai/codex/frontend/detail/ProviderResultProjection.h"
#include "ai/openai/codex/frontend/internal/server/BackendProjection.h"

#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ai::openai::codex::frontend::internal::server {

    namespace {

        ErrorCode frontendErrorCode(backend::CommandErrorCode code) noexcept {
            switch (code) {
                case backend::CommandErrorCode::PermissionDenied:
                    return ErrorCode::PermissionDenied;
                case backend::CommandErrorCode::InvalidCommand:
                    return ErrorCode::InvalidCommand;
                case backend::CommandErrorCode::NotFound:
                    return ErrorCode::NotFound;
                case backend::CommandErrorCode::Conflict:
                    return ErrorCode::Conflict;
                case backend::CommandErrorCode::LocalSubmissionFailure:
                    return ErrorCode::LocalSubmissionFailure;
                case backend::CommandErrorCode::TypedDecodingFailure:
                    return ErrorCode::TypedDecodingFailure;
                case backend::CommandErrorCode::RemoteAppServerError:
                    return ErrorCode::RemoteAppServerError;
                case backend::CommandErrorCode::Cancelled:
                    return ErrorCode::Cancelled;
                case backend::CommandErrorCode::BackendUnavailable:
                    return ErrorCode::BackendUnavailable;
            }
            return ErrorCode::InternalError;
        }

        std::string frontendErrorMessage(backend::CommandErrorCode code) {
            switch (code) {
                case backend::CommandErrorCode::PermissionDenied:
                    return "frontend command was denied";
                case backend::CommandErrorCode::InvalidCommand:
                    return "frontend command is invalid";
                case backend::CommandErrorCode::NotFound:
                    return "frontend command target was not found";
                case backend::CommandErrorCode::Conflict:
                    return "frontend command conflicts with current state";
                case backend::CommandErrorCode::LocalSubmissionFailure:
                    return "frontend command could not be submitted";
                case backend::CommandErrorCode::TypedDecodingFailure:
                    return "frontend command result could not be decoded";
                case backend::CommandErrorCode::RemoteAppServerError:
                    return "Codex App Server rejected the command";
                case backend::CommandErrorCode::Cancelled:
                    return "frontend command was cancelled";
                case backend::CommandErrorCode::BackendUnavailable:
                    return "Codex App Server is unavailable";
            }
            return "frontend command failed";
        }

        bool isFrontendV1MetadataOnlyItem(std::string_view type) noexcept {
            return type == "collabAgentToolCall" || type == "contextCompaction" || type == "enteredReviewMode" ||
                   type == "exitedReviewMode" || type == "hookPrompt" || type == "imageGeneration" || type == "imageView" ||
                   type == "plan" || type == "sleep" || type == "subAgentActivity";
        }

        Json legacyItemSnapshotJson(const backend::ItemSnapshot& item) {
            const Json frontendData = isFrontendV1MetadataOnlyItem(item.type) ? Json::object({{"codexType", item.type}}) : item.data;
            Json encoded{{"id", item.id},
                         {"type", item.type},
                         {"status", item.status},
                         {"agentText", item.agentText},
                         {"reasoningText", item.reasoningText},
                         {"reasoningSummary", item.reasoningSummary},
                         {"commandOutput", item.commandOutput},
                         {"droppedContentBytes", item.droppedContentBytes},
                         {"contentTruncated", item.contentTruncated},
                         {"data", frontendData},
                         {"extensions", item.extensions}};
            if (item.startedAtMs) {
                encoded["startedAtMs"] = *item.startedAtMs;
            }
            if (item.completedAtMs) {
                encoded["completedAtMs"] = *item.completedAtMs;
            }
            return encoded;
        }

        Json turnSnapshotJson(const backend::TurnSnapshot& turn) {
            Json encoded{{"id", turn.id},
                         {"threadId", turn.threadId},
                         {"status", turn.status},
                         {"active", turn.active},
                         {"terminal", turn.terminal},
                         {"items", Json::array()},
                         {"extensions", turn.extensions}};
            if (turn.failure) {
                encoded["failure"] = *turn.failure;
            }
            if (turn.tokenUsage) {
                encoded["tokenUsage"] = *turn.tokenUsage;
            }
            for (const backend::ItemSnapshot& item : turn.items) {
                encoded["items"].push_back(legacyItemSnapshotJson(item));
            }
            return encoded;
        }

        Json threadSnapshotJson(const backend::ThreadSnapshot& thread) {
            Json encoded{
                {"id", thread.id}, {"fullyLoaded", thread.fullyLoaded}, {"turns", Json::array()}, {"extensions", thread.extensions}};
            if (thread.title) {
                encoded["title"] = *thread.title;
            }
            if (thread.cwd) {
                encoded["cwd"] = *thread.cwd;
            }
            if (thread.model) {
                encoded["model"] = *thread.model;
            }
            if (thread.modelProvider) {
                encoded["modelProvider"] = *thread.modelProvider;
            }
            if (thread.preview) {
                encoded["preview"] = *thread.preview;
            }
            if (thread.status) {
                encoded["status"] = *thread.status;
            }
            if (thread.createdAt) {
                encoded["createdAt"] = *thread.createdAt;
            }
            if (thread.updatedAt) {
                encoded["updatedAt"] = *thread.updatedAt;
            }
            for (const backend::TurnSnapshot& turn : thread.turns) {
                encoded["turns"].push_back(turnSnapshotJson(turn));
            }
            return encoded;
        }

        const backend::ThreadSnapshot* findThread(const backend::Snapshot& snapshot, std::string_view id) noexcept {
            for (const backend::ThreadSnapshot& thread : snapshot.threads) {
                if (thread.id == id) {
                    return &thread;
                }
            }
            return nullptr;
        }

        const backend::TurnSnapshot*
        findTurn(const backend::Snapshot& snapshot, std::string_view threadId, std::string_view turnId) noexcept {
            const backend::ThreadSnapshot* thread = findThread(snapshot, threadId);
            if (thread == nullptr) {
                return nullptr;
            }
            for (const backend::TurnSnapshot& turn : thread->turns) {
                if (turn.id == turnId) {
                    return &turn;
                }
            }
            return nullptr;
        }

        bool controllerResultValid(generated::MethodId method,
                                   backend::SessionId expectedBackendSession,
                                   const backend::ControllerResult& result) noexcept {
            if (method == generated::MethodId::ControllerAcquire) {
                return result.role == backend::SessionRole::Controller && result.controller == expectedBackendSession;
            }
            if (method == generated::MethodId::ControllerRelease) {
                return result.role == backend::SessionRole::Observer && !result.controller;
            }
            return false;
        }

    } // namespace

    class BackendCoreBridge::State : public std::enable_shared_from_this<BackendCoreBridge::State> {
    public:
        struct SessionRecord {
            FrontendSessionToken token;
            backend::SessionId backendId;
            std::shared_ptr<backend::FrontendSession> backendSession;
            std::map<std::string, CommandToken, std::less<>> pending;
        };

        struct ExternalTopology {
            std::vector<model::SessionState> sessions;
            std::optional<model::SessionIdentity> controller;
        };

        struct DeferredControllerCompletion {
            backend::SequenceNumber requiredThrough;
            std::string key;
            backend::CommandCompletion completion;
        };

        struct DeferredSessionClose {
            backend::SequenceNumber requiredThrough;
            std::string reason;
        };

        State(backend::detail::BackendCoreRuntime& runtime, std::size_t maximumResultBytes)
            : runtime(runtime)
            , maximumResultBytes(maximumResultBytes) {
        }

        void bind(ServerCore& configuredCore) noexcept {
            if (coreIdentity == nullptr || coreIdentity == &configuredCore) {
                coreIdentity = &configuredCore;
            }
        }

        void unbind(ServerCore& configuredCore) noexcept {
            if (coreIdentity == &configuredCore) {
                coreLifetime.reset();
                coreIdentity = nullptr;
            }
        }

        void bindLifetime(const std::shared_ptr<ServerCore>& configuredCore) {
            if (!configuredCore || coreIdentity != configuredCore.get()) {
                throw std::logic_error("BackendCoreBridge ServerCore lifetime binding is invalid");
            }
            coreLifetime = configuredCore;
        }

        [[nodiscard]] std::shared_ptr<ServerCore> lockCore() const noexcept {
            return coreLifetime.lock();
        }

        [[nodiscard]] bool bindingCurrent(const std::shared_ptr<ServerCore>& expected) const noexcept {
            const std::shared_ptr<ServerCore> current = lockCore();
            return current && current == expected && coreIdentity == expected.get();
        }

        void start() {
            if (!lockCore() || observer.isOpen()) {
                throw std::logic_error("BackendCoreBridge observer lifecycle is invalid");
            }
            const std::weak_ptr<State> weak = weak_from_this();
            observer =
                runtime.subscribe(backend::BackendObserverCallbacks{[weak](const std::vector<backend::SequencedBackendEvent>& events) {
                                                                        if (const std::shared_ptr<State> self = weak.lock()) {
                                                                            self->onEvents(events);
                                                                        }
                                                                    },
                                                                    [weak](const backend::Snapshot& snapshot) {
                                                                        if (const std::shared_ptr<State> self = weak.lock()) {
                                                                            self->onResynchronize(snapshot);
                                                                        }
                                                                    }});
            if (!observer.isOpen()) {
                throw std::runtime_error("BackendCore rejected the shared frontend observer");
            }
            const backend::Snapshot initial = runtime.snapshot();
            if (!reconcileTopology(initial)) {
                observer.close();
                throw std::runtime_error("BackendCore topology could not be mapped into frontend identity space");
            }
            observerProcessedThrough = initial.sequence;
        }

        void close() noexcept {
            observer.close();
            while (!sessions.empty()) {
                auto current = sessions.begin();
                const backend::SessionId backendId = current->second.backendId;
                std::shared_ptr<backend::FrontendSession> session = std::move(current->second.backendSession);
                ownedBackendSessions.erase(backendId);
                sessions.erase(current);
                if (session) {
                    session->close("frontend BackendCore bridge closed");
                }
            }
            coreLifetime.reset();
            coreIdentity = nullptr;
            ownedBackendSessions.clear();
            retiredBackendSessions.clear();
            externalIdentities.clear();
            observedBackendController.reset();
            deferredObserverEvents.clear();
            resynchronizationPendingDuringAdmission = false;
            deferredControllerCompletion.reset();
            deferredSessionCloses.clear();
        }

        [[nodiscard]] bool openSession(const FrontendSessionToken& token) {
            const std::shared_ptr<ServerCore> target = lockCore();
            if (!target || sessions.contains(token.session.value())) {
                return false;
            }
            const std::string key = token.session.value();
            const std::weak_ptr<State> weak = weak_from_this();
            backend::FrontendSession session;
            std::shared_ptr<backend::FrontendSession> retainedSession;
            std::optional<backend::SessionId> admittedBackendId;
            struct AdmissionScope {
                State& owner;
                bool active = true;

                explicit AdmissionScope(State& owner)
                    : owner(owner) {
                    ++owner.sessionAdmissionDepth;
                }

                void leave() noexcept {
                    if (active) {
                        active = false;
                        if (owner.sessionAdmissionDepth != 0) {
                            --owner.sessionAdmissionDepth;
                        }
                    }
                }

                ~AdmissionScope() {
                    leave();
                }
            } admission(*this);
            try {
                session = runtime.openSession(backend::FrontendSessionCallbacks{{},
                                                                                {},
                                                                                [weak, key](const backend::CommandCompletion& completion) {
                                                                                    if (const std::shared_ptr<State> self = weak.lock()) {
                                                                                        self->onCompletion(key, completion);
                                                                                    }
                                                                                },
                                                                                [weak, key](const std::string& reason) {
                                                                                    if (const std::shared_ptr<State> self = weak.lock()) {
                                                                                        self->onSessionClosed(key, reason);
                                                                                    }
                                                                                }});
                if (!session.isOpen()) {
                    admission.leave();
                    finishSessionAdmission(*target);
                    return false;
                }
                const backend::SessionId backendId = session.id();
                admittedBackendId = backendId;
                ownedBackendSessions.insert_or_assign(backendId, key);
                if (!bindingCurrent(target)) {
                    ownedBackendSessions.erase(backendId);
                    retiredBackendSessions.insert(backendId);
                    session.close("frontend service closed during backend session admission");
                    admission.leave();
                    deferredObserverEvents.clear();
                    resynchronizationPendingDuringAdmission = false;
                    return false;
                }
                retainedSession = std::make_shared<backend::FrontendSession>(std::move(session));
                const bool inserted = sessions.emplace(key, SessionRecord{token, backendId, retainedSession, {}}).second;
                if (!inserted) {
                    ownedBackendSessions.erase(backendId);
                    retiredBackendSessions.insert(backendId);
                    retainedSession->close("duplicate frontend backend session admission");
                    admission.leave();
                    finishSessionAdmission(*target);
                    return false;
                }
                admission.leave();
                finishSessionAdmission(*target);
                return true;
            } catch (...) {
                if (admittedBackendId) {
                    ownedBackendSessions.erase(*admittedBackendId);
                    try {
                        retiredBackendSessions.insert(*admittedBackendId);
                    } catch (...) {
                    }
                }
                if (retainedSession) {
                    retainedSession->close("frontend backend session admission failed");
                } else if (session.isOpen()) {
                    session.close("frontend backend session admission failed");
                }
                // Every event accumulated while admission was ambiguous is
                // covered by the authoritative current snapshot below. Do not
                // risk classifying a partially admitted backend ID as external.
                deferredObserverEvents.clear();
                resynchronizationPendingDuringAdmission = false;
                admission.leave();
                if (bindingCurrent(target)) {
                    recoverCurrentSnapshot(*target);
                }
                return false;
            }
        }

        void closeSession(const FrontendSessionToken& token) noexcept {
            const auto found = sessions.find(token.session.value());
            if (found == sessions.end() || found->second.token != token) {
                return;
            }
            if (deferredControllerCompletion && deferredControllerCompletion->key == token.session.value()) {
                deferredControllerCompletion.reset();
            }
            std::shared_ptr<backend::FrontendSession> session = std::move(found->second.backendSession);
            sessions.erase(found);
            if (session) {
                // Retain the already-allocated ownership entry until the
                // disconnect echo is consumed. This keeps the noexcept close
                // path allocation-free and prevents local backend IDs from
                // ever being reclassified as external.
                session->close("frontend session closed");
            }
        }

        [[nodiscard]] BackendSubmitStatus submit(BackendInvocation invocation) {
            const auto found = sessions.find(invocation.session.value());
            if (found == sessions.end() || found->second.token.connection != invocation.token.connection ||
                found->second.token.connectionGeneration != invocation.token.connectionGeneration || !found->second.backendSession ||
                !found->second.backendSession->isOpen()) {
                return BackendSubmitStatus::Unavailable;
            }

            backend::BackendCommand command;
            if (invocation.token.method == generated::MethodId::ControllerAcquire) {
                command = backend::ControllerAcquire{};
            } else if (invocation.token.method == generated::MethodId::ControllerRelease) {
                command = backend::ControllerRelease{};
            } else {
                detail::DefinedCommandMapping mapping = detail::mapDefinedCommand(invocation.command);
                backend::BackendCommand* mapped = std::get_if<backend::BackendCommand>(&mapping);
                if (mapped == nullptr) {
                    return BackendSubmitStatus::Rejected;
                }
                command = std::move(*mapped);
            }

            SessionRecord& record = found->second;
            const bool inserted = record.pending.emplace(invocation.token.requestId, invocation.token).second;
            if (!inserted) {
                return BackendSubmitStatus::Rejected;
            }
            const std::shared_ptr<backend::FrontendSession> submittingSession = record.backendSession;
            backend::CommandSubmission submission = submittingSession->submit(invocation.token.requestId, std::move(command));
            if (submission) {
                return BackendSubmitStatus::Accepted;
            }
            // A synchronous completion is authoritative and has already
            // removed this token. The callback may also have erased the whole
            // session, so revalidate instead of retaining SessionRecord&.
            const auto afterSubmit = sessions.find(invocation.session.value());
            if (afterSubmit == sessions.end() || afterSubmit->second.token.connection != invocation.token.connection ||
                afterSubmit->second.token.connectionGeneration != invocation.token.connectionGeneration ||
                !afterSubmit->second.pending.contains(invocation.token.requestId)) {
                return BackendSubmitStatus::Accepted;
            }
            afterSubmit->second.pending.erase(invocation.token.requestId);
            switch (submission.error) {
                case backend::SubmissionError::None:
                case backend::SubmissionError::Closed:
                    return BackendSubmitStatus::Unavailable;
                case backend::SubmissionError::QueueFull:
                    return BackendSubmitStatus::CapacityExceeded;
                case backend::SubmissionError::EmptyRequestId:
                case backend::SubmissionError::DuplicateRequestId:
                    return BackendSubmitStatus::Rejected;
            }
            return BackendSubmitStatus::Rejected;
        }

        void onCompletion(const std::string& key, const backend::CommandCompletion& completion) noexcept {
            const auto session = sessions.find(key);
            if (session == sessions.end()) {
                return;
            }
            const auto pending = session->second.pending.find(completion.requestId);
            if (pending == session->second.pending.end()) {
                return;
            }
            if (pending->second.method == generated::MethodId::ControllerAcquire ||
                pending->second.method == generated::MethodId::ControllerRelease) {
                // BackendCore drains observer and command callbacks
                // independently. A completion may overtake the corresponding
                // ControllerChanged batch, so wait for the one shared observer
                // to observe the typed result's controller value.
                const std::optional<backend::Snapshot> current = currentSnapshotNoThrow();
                if (!current) {
                    valueFailure(key,
                                 completion.requestId,
                                 ErrorCode::InternalError,
                                 "backend controller completion ordering could not be verified");
                    return;
                }
                if (observerProcessedThrough.value() < current->sequence.value()) {
                    if (deferredControllerCompletion) {
                        valueFailure(key,
                                     completion.requestId,
                                     ErrorCode::InternalError,
                                     "backend controller completion ordering capacity was exceeded");
                        return;
                    }
                    try {
                        deferredControllerCompletion = DeferredControllerCompletion{current->sequence, key, completion};
                    } catch (...) {
                        valueFailure(
                            key, completion.requestId, ErrorCode::InternalError, "backend controller completion could not be retained");
                    }
                    return;
                }
            }
            finishCompletion(key, completion);
        }

        void finishCompletion(const std::string& key, const backend::CommandCompletion& completion) noexcept {
            const auto found = sessions.find(key);
            if (found == sessions.end()) {
                return;
            }
            const auto pending = found->second.pending.find(completion.requestId);
            if (pending == found->second.pending.end()) {
                return;
            }
            CommandToken token = pending->second;
            found->second.pending.erase(pending);
            const std::shared_ptr<ServerCore> target = lockCore();
            if (!target) {
                return;
            }

            BackendCompletionValue value =
                BackendCommandFailure{ErrorCode::InternalError, "failed to normalize backend command completion", std::nullopt};
            try {
                if (completion.result.error) {
                    value = BackendCommandFailure{frontendErrorCode(completion.result.error->code),
                                                  frontendErrorMessage(completion.result.error->code),
                                                  std::nullopt};
                } else {
                    Json projected = resultJson(token, completion.result.value);
                    value = BackendCommandSuccess{generated::makeResult(token.method, std::move(projected))};
                }
            } catch (const std::length_error&) {
                value = BackendCommandFailure{
                    ErrorCode::CapacityExceeded, "frontend command result exceeds the configured outbound capacity", std::nullopt};
            } catch (const std::logic_error&) {
                value = BackendCommandFailure{
                    ErrorCode::TypedDecodingFailure, "backend completion violates the generated frontend result authority", std::nullopt};
            } catch (...) {
            }
            if (bindingCurrent(target)) {
                static_cast<void>(target->complete(BackendCompletion{std::move(token), std::move(value)}));
            }
        }

        void valueFailure(const std::string& key, const std::string& requestId, ErrorCode code, std::string message) noexcept {
            const auto found = sessions.find(key);
            if (found == sessions.end()) {
                return;
            }
            const auto pending = found->second.pending.find(requestId);
            if (pending == found->second.pending.end()) {
                return;
            }
            CommandToken token = pending->second;
            found->second.pending.erase(pending);
            if (const std::shared_ptr<ServerCore> target = lockCore(); bindingCurrent(target)) {
                static_cast<void>(
                    target->complete(BackendCompletion{std::move(token), BackendCommandFailure{code, std::move(message), std::nullopt}}));
            }
        }

        [[nodiscard]] Json resultJson(const CommandToken& token, const backend::CommandValue& value) const {
            const detail::ProviderResultProjection projected = detail::projectProviderResult(token.method, value, maximumResultBytes);
            switch (projected.status) {
                case detail::ProviderResultProjectionStatus::Success:
                    return projected.value;
                case detail::ProviderResultProjectionStatus::ResultTooLarge:
                    throw std::length_error("frontend command result exceeds outbound capacity");
                case detail::ProviderResultProjectionStatus::LegacyProjectionRequired:
                case detail::ProviderResultProjectionStatus::NotProviderResult:
                    break;
                case detail::ProviderResultProjectionStatus::ResultTypeMismatch:
                case detail::ProviderResultProjectionStatus::InvalidResult:
                    throw std::logic_error("backend result violates generated frontend result authority");
            }

            Json result = Json::object();
            std::optional<backend::Snapshot> capturedSnapshot;
            const auto currentSnapshot = [&]() -> const backend::Snapshot& {
                if (!capturedSnapshot) {
                    capturedSnapshot.emplace(runtime.snapshot());
                }
                return *capturedSnapshot;
            };
            if (const auto* controller = std::get_if<backend::ControllerResult>(&value)) {
                // BackendCore SessionId is deliberately never projected. The
                // wire identity is the semantic ServerCore session identity.
                const auto found = sessions.find(tokenSession(token));
                if (found == sessions.end() || !controllerResultValid(token.method, found->second.backendId, *controller)) {
                    throw std::logic_error("backend controller completion violates the controller transaction");
                }
                if (token.method == generated::MethodId::ControllerAcquire) {
                    result = Json{{"controllerSessionId", found->second.token.session.value()}, {"role", "controller"}};
                } else {
                    result = Json{{"role", "observer"}};
                }
            } else if (std::holds_alternative<std::monostate>(value) || std::holds_alternative<typed::Unit>(value)) {
                result = Json::object();
            } else if (const auto* response = std::get_if<typed::ThreadStartResponse>(&value)) {
                const backend::ThreadSnapshot* thread = findThread(currentSnapshot(), response->thread.id.value);
                result = thread ? Json{{"thread", threadSnapshotJson(*thread)}} : Json{{"threadId", response->thread.id.value}};
            } else if (const auto* response = std::get_if<typed::ThreadResumeResponse>(&value)) {
                const backend::ThreadSnapshot* thread = findThread(currentSnapshot(), response->thread.id.value);
                result = thread ? Json{{"thread", threadSnapshotJson(*thread)}} : Json{{"threadId", response->thread.id.value}};
            } else if (const auto* response = std::get_if<typed::ThreadReadResponse>(&value)) {
                const backend::ThreadSnapshot* thread = findThread(currentSnapshot(), response->thread.id.value);
                result = thread ? Json{{"thread", threadSnapshotJson(*thread)}} : Json{{"threadId", response->thread.id.value}};
            } else if (const auto* response = std::get_if<typed::ThreadListResponse>(&value)) {
                result = Json{{"threads", Json::array()}};
                for (const typed::Thread& item : response->data) {
                    const backend::ThreadSnapshot* thread = findThread(currentSnapshot(), item.id.value);
                    result["threads"].push_back(thread ? threadSnapshotJson(*thread) : Json{{"id", item.id.value}});
                }
                if (response->nextCursor) {
                    result["nextCursor"] = *response->nextCursor;
                }
                if (response->backwardsCursor) {
                    result["backwardsCursor"] = *response->backwardsCursor;
                }
            } else if (const auto* response = std::get_if<typed::TurnStartResponse>(&value)) {
                const backend::TurnSnapshot* turn = findTurn(currentSnapshot(), response->turn.threadId.value, response->turn.id.value);
                result = turn ? Json{{"turn", turnSnapshotJson(*turn)}} : Json{{"turnId", response->turn.id.value}};
            } else {
                throw std::logic_error("backend result lacks a frontend result projection");
            }
            if (result.dump().size() > maximumResultBytes) {
                throw std::length_error("frontend command result exceeds outbound capacity");
            }
            return result;
        }

        [[nodiscard]] std::string tokenSession(const CommandToken& token) const {
            for (const auto& [key, record] : sessions) {
                if (record.token.connection == token.connection && record.token.connectionGeneration == token.connectionGeneration) {
                    return key;
                }
            }
            return {};
        }

        void onSessionClosed(const std::string& key, const std::string& reason) noexcept {
            const auto found = sessions.find(key);
            if (found == sessions.end()) {
                return;
            }
            const backend::SessionId backendId = found->second.backendId;
            const std::optional<backend::Snapshot> current = currentSnapshotNoThrow();
            if (!current) {
                if (const std::shared_ptr<ServerCore> target = lockCore()) {
                    static_cast<void>(target->requireSnapshot(OccurrenceFlushUrgency::Immediate));
                }
                finishSessionClosed(key, reason);
                return;
            }
            // BackendCore publishes ControllerChanged/SessionChanged before it
            // schedules onClosed. Independently drained queues can still run
            // this callback first, so retain it through the authoritative
            // Snapshot sequence captured here. That includes a controller
            // handoff queued after SessionChanged(false), not merely the
            // disconnect echo itself.
            if (backendId && observerProcessedThrough.value() < current->sequence.value()) {
                try {
                    deferredSessionCloses.insert_or_assign(key, DeferredSessionClose{current->sequence, reason});
                } catch (...) {
                    if (const std::shared_ptr<ServerCore> target = lockCore()) {
                        static_cast<void>(target->requireSnapshot(OccurrenceFlushUrgency::Immediate));
                    }
                    finishSessionClosed(key, reason);
                }
                return;
            }
            finishSessionClosed(key, reason);
        }

        void finishSessionClosed(const std::string& key, const std::string& reason) noexcept {
            const auto found = sessions.find(key);
            if (found == sessions.end()) {
                return;
            }
            const FrontendSessionToken token = found->second.token;
            std::optional<backend::SessionId> backendId;
            if (found->second.backendId) {
                backendId = found->second.backendId;
                ownedBackendSessions.erase(*backendId);
                try {
                    retiredBackendSessions.insert(*backendId);
                } catch (...) {
                }
            }
            sessions.erase(found);
            if (deferredControllerCompletion && deferredControllerCompletion->key == key) {
                deferredControllerCompletion.reset();
            }
            deferredSessionCloses.erase(key);
            if (const std::shared_ptr<ServerCore> target = lockCore()) {
                // The tombstone is the ordering fence: any BackendCore
                // controller handoff preceding this close has already updated
                // ServerCore, so closing the semantic frontend session cannot
                // erase or relabel an unrelated external owner.
                target->closeConnection(token.connection, reason.empty() ? "backend frontend session closed" : reason);
            }
            if (backendId) {
                retiredBackendSessions.erase(*backendId);
            }
        }

        void onEvents(const std::vector<backend::SequencedBackendEvent>& events) noexcept {
            if (sessionAdmissionDepth != 0) {
                try {
                    deferredObserverEvents.insert(deferredObserverEvents.end(), events.begin(), events.end());
                } catch (...) {
                    deferredObserverEvents.clear();
                    // Admission has not classified the BackendCore SessionId
                    // yet. Recover from the authoritative post-admission
                    // snapshot instead of allowing a partial suffix to be
                    // interpreted with ambiguous ownership.
                    resynchronizationPendingDuringAdmission = true;
                }
                return;
            }
            processEvents(events);
        }

        void processEvents(const std::vector<backend::SequencedBackendEvent>& events) noexcept {
            std::shared_ptr<ServerCore> target = lockCore();
            if (!target) {
                return;
            }
            try {
                std::vector<backend::SequencedBackendEvent> projectedEvents;
                projectedEvents.reserve(events.size());
                std::optional<backend::SequenceNumber> processedThrough;
                enum class ProjectedFlushResult { Staged, SnapshotPublished, Failed };
                const auto flushProjectedEvents = [&]() -> ProjectedFlushResult {
                    if (projectedEvents.empty()) {
                        return ProjectedFlushResult::Staged;
                    }
                    std::optional<ProjectedBackendBatch> projectedBatch;
                    std::vector<backend::ItemSnapshotKey> contentKeys;
                    contentKeys.reserve(projectedEvents.size());
                    bool contentOnly = true;
                    for (const backend::SequencedBackendEvent& sequenced : projectedEvents) {
                        const auto* content = std::get_if<backend::ItemContentChanged>(&sequenced.event);
                        if (content == nullptr) {
                            contentOnly = false;
                            break;
                        }
                        contentKeys.push_back({content->threadId, content->turnId, content->itemId});
                    }
                    if (contentOnly) {
                        std::optional<backend::ItemSnapshotBatch> contentItems = runtime.itemSnapshots(contentKeys);
                        if (contentItems && !projectedEvents.empty() &&
                            contentItems->sequence.value() >= projectedEvents.back().sequence.value()) {
                            model::ModelResult<ProjectedBackendBatch> direct =
                                projection.projectItemContentOccurrences(projectedEvents, contentItems->items);
                            if (direct && !direct.value().snapshotRequired) {
                                projectedBatch = std::move(direct).value();
                            }
                        }
                    }

                    std::optional<backend::Snapshot> snapshot;
                    if (!projectedBatch) {
                        snapshot = runtime.snapshot();
                        model::ModelResult<ProjectedBackendBatch> projected =
                            projection.projectOccurrences(projectedEvents, *snapshot);
                        if (!projected) {
                            projectedEvents.clear();
                            return ProjectedFlushResult::Failed;
                        }
                        projectedBatch = std::move(projected).value();
                    }
                    projectedEvents.clear();
                    if (!bindingCurrent(target)) {
                        return ProjectedFlushResult::Failed;
                    }
                    ProjectedBackendBatch batch = std::move(*projectedBatch);
                    if (batch.snapshotRequired) {
                        if (!snapshot || !publishProjectedResynchronization(*snapshot, std::move(batch.snapshot), *target)) {
                            return ProjectedFlushResult::Failed;
                        }
                        if (snapshot->sequence.value() > observerProcessedThrough.value()) {
                            observerProcessedThrough = snapshot->sequence;
                        }
                        drainDeferredControllerCompletion();
                        drainDeferredSessionCloses();
                        return ProjectedFlushResult::SnapshotPublished;
                    }
                    std::vector<OccurrenceStageRequest> groups;
                    groups.reserve(batch.occurrences.size());
                    for (ProjectedBackendOccurrence& occurrence : batch.occurrences) {
                        groups.push_back({std::move(occurrence.key), std::move(occurrence.occurrence), occurrence.urgency});
                    }
                    return target->stageGroups(std::move(groups)).accepted() ? ProjectedFlushResult::Staged : ProjectedFlushResult::Failed;
                };
                for (const backend::SequencedBackendEvent& event : events) {
                    if (event.sequence.value() <= observerProcessedThrough.value()) {
                        continue;
                    }
                    processedThrough = event.sequence;
                    if (const auto* changed = std::get_if<backend::SessionChanged>(&event.event)) {
                        const ProjectedFlushResult flushed = flushProjectedEvents();
                        if (flushed == ProjectedFlushResult::Failed) {
                            recoverCurrentSnapshot(*target);
                            return;
                        }
                        if (flushed == ProjectedFlushResult::SnapshotPublished) {
                            return;
                        }
                        if (const auto owned = ownedBackendSessions.find(changed->id); owned != ownedBackendSessions.end()) {
                            if (!changed->connected) {
                                if (sessions.contains(owned->second)) {
                                    retiredBackendSessions.insert(changed->id);
                                } else {
                                    ownedBackendSessions.erase(owned);
                                }
                            }
                            continue;
                        }
                        if (retiredBackendSessions.contains(changed->id)) {
                            if (!changed->connected) {
                                // This path belongs to a locally initiated
                                // close: the semantic session has already been
                                // removed and this is the final backend echo.
                                retiredBackendSessions.erase(changed->id);
                            }
                            continue;
                        }
                        if (!changed->connected && !externalIdentities.contains(changed->id)) {
                            recoverCurrentSnapshot(*target);
                            return;
                        }
                        const std::optional<model::SessionIdentity> identity = externalIdentity(changed->id, *target);
                        if (!identity) {
                            recoverCurrentSnapshot(*target);
                            return;
                        }
                        model::SessionState session(*identity);
                        session.role = changed->role == backend::SessionRole::Controller ? SessionRole::Controller : SessionRole::Observer;
                        if (!target->externalSessionChanged(std::move(session), changed->connected)) {
                            recoverCurrentSnapshot(*target);
                            return;
                        }
                        if (!changed->connected) {
                            externalIdentities.erase(changed->id);
                        }
                        continue;
                    }
                    if (const auto* changed = std::get_if<backend::ControllerChanged>(&event.event)) {
                        const ProjectedFlushResult flushed = flushProjectedEvents();
                        if (flushed == ProjectedFlushResult::Failed) {
                            recoverCurrentSnapshot(*target);
                            return;
                        }
                        if (flushed == ProjectedFlushResult::SnapshotPublished) {
                            return;
                        }
                        const std::optional<backend::SessionId> previous = observedBackendController;
                        observedBackendController = changed->controller;
                        if (changed->controller && ownedBackendSessions.contains(*changed->controller)) {
                            continue;
                        }
                        if (changed->controller && retiredBackendSessions.contains(*changed->controller)) {
                            continue;
                        }
                        if (!changed->controller && previous &&
                            (ownedBackendSessions.contains(*previous) || retiredBackendSessions.contains(*previous))) {
                            continue;
                        }
                        std::optional<model::SessionIdentity> identity;
                        if (changed->controller) {
                            identity = externalIdentity(*changed->controller, *target);
                            if (!identity) {
                                recoverCurrentSnapshot(*target);
                                return;
                            }
                        }
                        if (!target->externalControllerChanged(std::move(identity))) {
                            recoverCurrentSnapshot(*target);
                            return;
                        }
                        continue;
                    }
                    projectedEvents.push_back(event);
                }
                const ProjectedFlushResult flushed = flushProjectedEvents();
                if (flushed == ProjectedFlushResult::Failed) {
                    recoverCurrentSnapshot(*target);
                    return;
                }
                if (flushed == ProjectedFlushResult::SnapshotPublished) {
                    return;
                }
                if (processedThrough && processedThrough->value() > observerProcessedThrough.value()) {
                    observerProcessedThrough = *processedThrough;
                }
                drainDeferredControllerCompletion();
                drainDeferredSessionCloses();
            } catch (...) {
                if (const std::shared_ptr<ServerCore> current = lockCore()) {
                    recoverCurrentSnapshot(*current);
                }
            }
        }

        void onResynchronize(const backend::Snapshot& backendSnapshot) noexcept {
            if (sessionAdmissionDepth != 0) {
                // A bounded observer overflow can resynchronize synchronously
                // from BackendCore::openSession(), before the returned private
                // SessionId has been classified. Defer only the fact; one
                // authoritative current Snapshot after admission covers the
                // entire suffix without retaining an allocating Snapshot here.
                resynchronizationPendingDuringAdmission = true;
                return;
            }
            const std::shared_ptr<ServerCore> target = lockCore();
            if (!target) {
                return;
            }
            if (!applyResynchronization(backendSnapshot, *target)) {
                static_cast<void>(target->requireSnapshot(OccurrenceFlushUrgency::Immediate));
                return;
            }
            if (backendSnapshot.sequence.value() > observerProcessedThrough.value()) {
                observerProcessedThrough = backendSnapshot.sequence;
            }
            drainDeferredControllerCompletion();
            drainDeferredSessionCloses();
        }

        void drainDeferredObserverEvents() noexcept {
            if (sessionAdmissionDepth != 0 || deferredObserverEvents.empty()) {
                return;
            }
            std::vector<backend::SequencedBackendEvent> deferred;
            deferred.swap(deferredObserverEvents);
            processEvents(deferred);
        }

        void finishSessionAdmission(ServerCore& target) noexcept {
            if (resynchronizationPendingDuringAdmission) {
                resynchronizationPendingDuringAdmission = false;
                deferredObserverEvents.clear();
                recoverCurrentSnapshot(target);
                return;
            }
            drainDeferredObserverEvents();
        }

        void drainDeferredControllerCompletion() noexcept {
            if (!deferredControllerCompletion || observerProcessedThrough.value() < deferredControllerCompletion->requiredThrough.value()) {
                return;
            }
            const auto session = sessions.find(deferredControllerCompletion->key);
            if (session == sessions.end() || !session->second.backendSession || !session->second.backendSession->isOpen()) {
                // BackendCore closes its session state before scheduling the
                // close callback. Detect that state directly so an already-
                // queued observer callback cannot publish a completion from
                // the terminated generation first.
                return;
            }
            if (deferredSessionCloses.contains(deferredControllerCompletion->key)) {
                // Once BackendCore has reported this command session closed,
                // every completion from that generation is stale even while
                // the observer is still advancing to the close fence. Let
                // finishSessionClosed cancel it before any transient
                // controller transition can be committed.
                return;
            }
            DeferredControllerCompletion ready = std::move(*deferredControllerCompletion);
            deferredControllerCompletion.reset();
            finishCompletion(ready.key, ready.completion);
        }

        void drainDeferredSessionCloses() noexcept {
            for (auto close = deferredSessionCloses.begin(); close != deferredSessionCloses.end();) {
                const auto session = sessions.find(close->first);
                if (session != sessions.end() && observerProcessedThrough.value() < close->second.requiredThrough.value()) {
                    ++close;
                    continue;
                }
                const std::string key = close->first;
                std::string reason = std::move(close->second.reason);
                close = deferredSessionCloses.erase(close);
                finishSessionClosed(key, reason);
            }
        }

        [[nodiscard]] std::optional<backend::Snapshot> currentSnapshotNoThrow() const noexcept {
            try {
                return runtime.snapshot();
            } catch (...) {
                return std::nullopt;
            }
        }

        [[nodiscard]] bool publishProjectedResynchronization(const backend::Snapshot& snapshot,
                                                             model::CanonicalSnapshot projected,
                                                             ServerCore& target) noexcept {
            try {
                if (!reconcileTopology(snapshot)) {
                    return false;
                }
                const std::shared_ptr<ServerCore> current = lockCore();
                if (!bindingCurrent(current) || current.get() != &target) {
                    return false;
                }
                return target.publishSnapshot(std::move(projected)).accepted;
            } catch (...) {
                return false;
            }
        }

        [[nodiscard]] bool applyResynchronization(const backend::Snapshot& snapshot, ServerCore& target) noexcept {
            try {
                if (!reconcileTopology(snapshot)) {
                    return false;
                }
                model::ModelResult<model::CanonicalSnapshot> projected = projection.projectSnapshot(snapshot);
                if (!bindingCurrent(lockCore()) || !projected) {
                    return false;
                }
                return target.publishSnapshot(std::move(projected).value()).accepted;
            } catch (...) {
                return false;
            }
        }

        void recoverCurrentSnapshot(ServerCore& target) noexcept {
            try {
                const backend::Snapshot current = runtime.snapshot();
                if (applyResynchronization(current, target)) {
                    if (current.sequence.value() > observerProcessedThrough.value()) {
                        observerProcessedThrough = current.sequence;
                    }
                    drainDeferredControllerCompletion();
                    drainDeferredSessionCloses();
                    return;
                }
            } catch (...) {
            }
            static_cast<void>(target.requireSnapshot(OccurrenceFlushUrgency::Immediate));
        }

        [[nodiscard]] std::optional<model::SessionIdentity> externalIdentity(backend::SessionId id, ServerCore& target) {
            if (const auto found = externalIdentities.find(id); found != externalIdentities.end()) {
                return found->second;
            }
            std::optional<model::SessionIdentity> identity = target.reserveExternalSessionIdentity();
            while (identity && identity->value() == std::to_string(id.value())) {
                identity = target.reserveExternalSessionIdentity();
            }
            if (!identity) {
                return std::nullopt;
            }
            externalIdentities.emplace(id, *identity);
            return identity;
        }

        [[nodiscard]] bool reconcileTopology(const backend::Snapshot& snapshot) noexcept {
            const std::shared_ptr<ServerCore> target = lockCore();
            if (!target) {
                return false;
            }
            try {
                std::set<backend::SessionId> present;
                ExternalTopology topology;
                topology.sessions.reserve(snapshot.sessions.size());
                for (const backend::SessionSnapshot& backendSession : snapshot.sessions) {
                    present.insert(backendSession.id);
                    if (ownedBackendSessions.contains(backendSession.id) || retiredBackendSessions.contains(backendSession.id)) {
                        continue;
                    }
                    const std::optional<model::SessionIdentity> identity = externalIdentity(backendSession.id, *target);
                    if (!identity) {
                        return false;
                    }
                    model::SessionState session(*identity);
                    session.role =
                        backendSession.role == backend::SessionRole::Controller ? SessionRole::Controller : SessionRole::Observer;
                    topology.sessions.push_back(std::move(session));
                }
                for (auto owned = ownedBackendSessions.begin(); owned != ownedBackendSessions.end();) {
                    const backend::SessionId backendId = owned->first;
                    if (!present.contains(backendId)) {
                        if (sessions.contains(owned->second)) {
                            retiredBackendSessions.insert(backendId);
                            ++owned;
                        } else {
                            // A locally closed session's disconnect echo may
                            // have been replaced by observer resynchronization.
                            // The authoritative Snapshot now makes it safe to
                            // retire that allocation-free ownership marker.
                            owned = ownedBackendSessions.erase(owned);
                        }
                    } else {
                        ++owned;
                    }
                }
                for (auto retired = retiredBackendSessions.begin(); retired != retiredBackendSessions.end();) {
                    if (!present.contains(*retired)) {
                        retired = retiredBackendSessions.erase(retired);
                    } else {
                        ++retired;
                    }
                }
                for (auto external = externalIdentities.begin(); external != externalIdentities.end();) {
                    if (!present.contains(external->first)) {
                        external = externalIdentities.erase(external);
                    } else {
                        ++external;
                    }
                }
                observedBackendController = snapshot.controller;
                if (snapshot.controller && !ownedBackendSessions.contains(*snapshot.controller) &&
                    !retiredBackendSessions.contains(*snapshot.controller)) {
                    topology.controller = externalIdentity(*snapshot.controller, *target);
                    if (!topology.controller) {
                        return false;
                    }
                }
                const bool bridgeControllerPresent = snapshot.controller && ownedBackendSessions.contains(*snapshot.controller) &&
                                                     !retiredBackendSessions.contains(*snapshot.controller);
                return target->replaceExternalTopology(
                    std::move(topology.sessions), std::move(topology.controller), bridgeControllerPresent);
            } catch (...) {
                return false;
            }
        }

        backend::detail::BackendCoreRuntime& runtime;
        const std::size_t maximumResultBytes;
        ServerCore* coreIdentity = nullptr;
        std::weak_ptr<ServerCore> coreLifetime;
        BackendProjection projection;
        backend::BackendObserverSubscription observer;
        std::map<std::string, SessionRecord, std::less<>> sessions;
        std::map<backend::SessionId, std::string> ownedBackendSessions;
        std::set<backend::SessionId> retiredBackendSessions;
        std::map<backend::SessionId, model::SessionIdentity> externalIdentities;
        std::optional<backend::SessionId> observedBackendController;
        backend::SequenceNumber observerProcessedThrough;
        std::size_t sessionAdmissionDepth = 0;
        bool resynchronizationPendingDuringAdmission = false;
        std::vector<backend::SequencedBackendEvent> deferredObserverEvents;
        std::optional<DeferredControllerCompletion> deferredControllerCompletion;
        std::map<std::string, DeferredSessionClose, std::less<>> deferredSessionCloses;
    };

    BackendCoreBridge::BackendCoreBridge(backend::detail::BackendCoreRuntime& backend, std::size_t maximumResultBytes)
        : state(std::make_shared<State>(backend, maximumResultBytes)) {
    }

    BackendCoreBridge::~BackendCoreBridge() {
        close();
    }

    void BackendCoreBridge::bind(ServerCore& core) noexcept {
        state->bind(core);
    }

    void BackendCoreBridge::unbind(ServerCore& core) noexcept {
        state->unbind(core);
    }

    void BackendCoreBridge::bindLifetime(const std::shared_ptr<ServerCore>& core) {
        state->bindLifetime(core);
    }

    void BackendCoreBridge::start() {
        state->start();
    }

    void BackendCoreBridge::close() noexcept {
        if (state) {
            state->close();
        }
    }

    bool BackendCoreBridge::providerReady() const noexcept {
        return state->runtime.isReady();
    }

    model::CanonicalSnapshot BackendCoreBridge::snapshot() const {
        model::ModelResult<model::CanonicalSnapshot> projected = state->projection.projectSnapshot(state->runtime.snapshot());
        if (!projected) {
            throw std::runtime_error("BackendCore snapshot violates the canonical frontend model");
        }
        return std::move(projected).value();
    }

    BackendSubmitStatus BackendCoreBridge::submit(BackendInvocation invocation) {
        return state->submit(std::move(invocation));
    }

    bool BackendCoreBridge::performProviderLifecycleAction(ProviderLifecycleAction action) {
        try {
            switch (action) {
                case ProviderLifecycleAction::Start:
                    state->runtime.start();
                    break;
                case ProviderLifecycleAction::Stop:
                    state->runtime.stop();
                    break;
                case ProviderLifecycleAction::Restart:
                    state->runtime.restart();
                    break;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    bool BackendCoreBridge::sessionOpened(const FrontendSessionToken& token, const FrontendPrincipal&) {
        return state->openSession(token);
    }

    void BackendCoreBridge::sessionClosed(const FrontendSessionToken& token) noexcept {
        state->closeSession(token);
    }

    bool BackendCoreBridgeTestAccess::controllerResultValid(generated::MethodId method,
                                                            std::uint64_t expectedBackendSession,
                                                            std::optional<std::uint64_t> reportedBackendController,
                                                            bool reportedControllerRole) noexcept {
        backend::ControllerResult result;
        if (reportedBackendController) {
            result.controller = backend::SessionId{*reportedBackendController};
        }
        result.role = reportedControllerRole ? backend::SessionRole::Controller : backend::SessionRole::Observer;
        return ::ai::openai::codex::frontend::internal::server::controllerResultValid(
            method, backend::SessionId{expectedBackendSession}, result);
    }

} // namespace ai::openai::codex::frontend::internal::server
