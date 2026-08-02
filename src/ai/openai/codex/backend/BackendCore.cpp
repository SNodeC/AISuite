/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/backend/BackendCore.h"

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/backend/BackendCommand.h"
#include "ai/openai/codex/backend/BackendEvent.h"
#include "ai/openai/codex/backend/BackendState.h"
#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/backend/internal/RecoveryPolicy.h"
#include "ai/openai/codex/typed/Events.h"
#include "ai/openai/codex/typed/Items.h"
#include "ai/openai/codex/typed/Results.h"
#include "ai/openai/codex/typed/ServerRequests.h"
#include "ai/openai/codex/typed/Threads.h"
#include "ai/openai/codex/typed/Turns.h"
#include "ai/openai/codex/typed/Types.h"
#include "core/EventReceiver.h"
#include "core/timer/Timer.h"
#include "utils/Timeval.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ai::openai::codex::backend {

    struct FrontendSession::Control {
        SessionId sessionId;
        std::function<SessionRole()> role;
        std::function<bool()> open;
        std::function<CommandSubmission(std::string, BackendCommand)> submit;
        std::function<bool()> snapshot;
        std::function<void(std::string)> close;
    };

    struct BackendObserverSubscription::Control {
        std::function<bool()> open;
        std::function<void()> close;
    };

    namespace {
        template <typename... Visitors>
        struct Overloaded : Visitors... {
            using Visitors::operator()...;
        };

        template <typename... Visitors>
        Overloaded(Visitors...) -> Overloaded<Visitors...>;

        std::size_t jsonBytes(const Json& value) noexcept {
            try {
                return value.dump().size();
            } catch (...) {
                return std::numeric_limits<std::size_t>::max();
            }
        }

        const Json& itemRaw(const typed::ThreadItem& item) {
            return std::visit(
                [](const auto& value) -> const Json& {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, typed::UnknownItem>) {
                        return value.raw;
                    } else {
                        return value.metadata.raw;
                    }
                },
                item);
        }

        const Json& requestRaw(const typed::TypedServerRequest& request) {
            return std::visit(
                [](const auto& value) -> const Json& {
                    return value.raw;
                },
                request);
        }

        std::size_t eventBytes(const SequencedBackendEvent& sequenced) noexcept {
            constexpr std::size_t Base = 512;
            const std::size_t variable = std::visit(
                Overloaded{[](const ProviderLifecycleChanged& value) {
                               std::size_t bytes = value.provider.lastError ? value.provider.lastError->message.size() : 0;
                               if (value.provider.initialization) {
                                   const typed::InitializeResponse& initialization = *value.provider.initialization;
                                   const auto add = [&bytes](std::size_t amount) {
                                       bytes = amount > std::numeric_limits<std::size_t>::max() - bytes
                                                   ? std::numeric_limits<std::size_t>::max()
                                                   : bytes + amount;
                                   };
                                   add(initialization.codexHome.value.size());
                                   add(initialization.platformFamily.size());
                                   add(initialization.platformOs.size());
                                   add(initialization.userAgent.size());
                                   add(jsonBytes(initialization.raw));
                               }
                               return bytes;
                           },
                           [](const ProviderConnectionInvalidated& value) {
                               return value.reason.size();
                           },
                           [](const CapacityConfigured&) {
                               return std::size_t{0};
                           },
                           [](const CapacityChanged&) {
                               return std::size_t{0};
                           },
                           [](const DiagnosticReceived& value) {
                               return value.message.size();
                           },
                           [](const ThreadUpserted& value) {
                               return jsonBytes(value.thread.raw);
                           },
                           [](const ThreadListUpdated& value) {
                               return jsonBytes(value.page.raw);
                           },
                           [](const ThreadStatusUpdated& value) {
                               return jsonBytes(typed::threadStatusRaw(value.status));
                           },
                           [](const TurnUpserted& value) {
                               return jsonBytes(value.turn.raw);
                           },
                           [](const TurnCompleted& value) {
                               return jsonBytes(value.turn.raw);
                           },
                           [](const TurnFailed& value) {
                               const std::size_t turn = jsonBytes(value.turn.raw);
                               const std::size_t error = jsonBytes(value.error);
                               return turn > std::numeric_limits<std::size_t>::max() - error ? std::numeric_limits<std::size_t>::max()
                                                                                             : turn + error;
                           },
                           [](const TurnErrorUpdated& value) {
                               return jsonBytes(value.error);
                           },
                           [](const ItemUpserted& value) {
                               return jsonBytes(itemRaw(value.item));
                           },
                           [](const ItemContentChanged& value) {
                               return value.delta.size();
                           },
                           [](const FileChangeUpdated& value) {
                               return jsonBytes(value.changes);
                           },
                           [](const TokenUsageUpdated& value) {
                               return jsonBytes(value.usage);
                           },
                           [](const ModelRerouted& value) {
                               return value.reason.size();
                           },
                           [](const PendingRequestAdded& value) {
                               return jsonBytes(requestRaw(value.pending.request));
                           },
                           [](const PendingRequestRemoved& value) {
                               return value.reason.size();
                           },
                           [](const ControllerChanged&) {
                               return std::size_t{0};
                           },
                           [](const SessionChanged&) {
                               return std::size_t{0};
                           },
                           [](const CodexExtensionReceived& value) {
                               const std::size_t payload = jsonBytes(value.payload);
                               std::size_t text = value.method.size();
                               const auto add = [&text](std::size_t amount) {
                                   text = amount > std::numeric_limits<std::size_t>::max() - text
                                       ? std::numeric_limits<std::size_t>::max()
                                       : text + amount;
                               };
                               add(value.decodingError ? value.decodingError->size() : 0);
                               if (value.diagnostic) {
                                   add(value.diagnostic->surface.size());
                                   add(value.diagnostic->fieldPath.size());
                                   add(value.diagnostic->message.size());
                               }
                               return payload > std::numeric_limits<std::size_t>::max() - text ? std::numeric_limits<std::size_t>::max()
                                                                                               : payload + text;
                           }},
                sequenced.event);
            return variable > std::numeric_limits<std::size_t>::max() - Base ? std::numeric_limits<std::size_t>::max() : Base + variable;
        }

        std::size_t snapshotBytes(const Snapshot& snapshot) noexcept {
            return snapshotSizeBytes(snapshot);
        }

        std::size_t commandCompletionBytes(const CommandCompletion& completion) noexcept {
            std::size_t bytes = 512 + completion.requestId.size();
            if (completion.result.error) {
                const std::size_t errorBytes = completion.result.error->message.size() + jsonBytes(completion.result.error->details);
                if (errorBytes > std::numeric_limits<std::size_t>::max() - bytes) {
                    return std::numeric_limits<std::size_t>::max();
                }
                bytes += errorBytes;
            }
            const std::size_t valueBytes = std::visit(Overloaded{[](const std::monostate&) {
                                                                     return std::size_t{0};
                                                                 },
                                                                 [](const Snapshot& value) {
                                                                     return snapshotBytes(value);
                                                                 },
                                                                 [](const ControllerResult&) {
                                                                     return std::size_t{32};
                                                                 },
                                                                 [](const typed::Thread& value) {
                                                                     return jsonBytes(value.raw);
                                                                 },
                                                                 [](const typed::ThreadListResponse& value) {
                                                                     return jsonBytes(value.raw);
                                                                 },
                                                                 [](const typed::Turn& value) {
                                                                     return jsonBytes(value.raw);
                                                                 },
                                                                 [](const typed::Unit&) {
                                                                     return std::size_t{0};
                                                                 }},
                                                      completion.result.value);
            return valueBytes > std::numeric_limits<std::size_t>::max() - bytes ? std::numeric_limits<std::size_t>::max()
                                                                                : bytes + valueBytes;
        }

        bool requiresController(const BackendCommand& command) {
            return !std::holds_alternative<ControllerAcquire>(command) && !std::holds_alternative<ControllerRelease>(command) &&
                   !std::holds_alternative<SnapshotGet>(command) && !std::holds_alternative<ThreadList>(command) &&
                   !std::holds_alternative<ThreadRead>(command);
        }

        bool requiresReadyBackend(const BackendCommand& command) {
            return std::holds_alternative<ThreadStart>(command) || std::holds_alternative<ThreadResume>(command) ||
                   std::holds_alternative<ThreadList>(command) || std::holds_alternative<ThreadRead>(command) ||
                   std::holds_alternative<TurnStart>(command) || std::holds_alternative<TurnInterrupt>(command);
        }

        CommandResult submissionFailure(const Submission& submission) {
            const std::string message =
                submission.error ? submission.error->message : "The typed App Server operation could not be enqueued.";
            return CommandResult::failed(CommandErrorCode::LocalSubmissionFailure, message);
        }

        template <typename T>
        CommandResult operationFailure(const typed::OperationResult<T>& result) {
            using Kind = typename typed::OperationResult<T>::Kind;
            switch (result.kind) {
                case Kind::RemoteError:
                    return CommandResult::failed(CommandErrorCode::RemoteAppServerError,
                                                 result.remoteError ? result.remoteError->message
                                                                    : "The Codex App Server rejected the operation.",
                                                 result.remoteError ? std::optional<std::int64_t>{result.remoteError->code} : std::nullopt);
                case Kind::Cancelled:
                    return CommandResult::failed(CommandErrorCode::Cancelled,
                                                 result.localError ? result.localError->message : "The Codex operation was cancelled.");
                case Kind::LocalError:
                    return CommandResult::failed(result.localError && result.localError->category == Error::Category::Protocol
                                                     ? CommandErrorCode::TypedDecodingFailure
                                                     : CommandErrorCode::LocalSubmissionFailure,
                                                 result.localError ? result.localError->message
                                                                   : "The typed Codex result could not be processed.");
                case Kind::Success:
                    break;
            }
            return CommandResult::failed(CommandErrorCode::TypedDecodingFailure, "The typed Codex result omitted its value.");
        }
    } // namespace

    class detail::BackendCoreRuntime::Impl : public std::enable_shared_from_this<detail::BackendCoreRuntime::Impl> {
    public:
        struct SessionBinding {
            SessionId id;
            std::function<SessionRole()> role;
            std::function<bool()> open;
            std::function<CommandSubmission(std::string, BackendCommand)> submit;
            std::function<bool()> snapshot;
            std::function<void(std::string)> close;
        };

        struct ObserverBinding {
            std::function<bool()> open;
            std::function<void()> close;
        };

        struct RecoveryTimerGate {
            bool active = true;
        };

        Impl(AppServerClient& client, BackendCoreOptions options)
            : client(client)
            , options(std::move(options))
            , reducer(this->options.reducer) {
            if (!this->options.scheduler) {
                this->options.scheduler = [](std::function<void()> callback) {
                    core::EventReceiver::atNextTick(std::move(callback));
                };
            }
            if (this->options.maxEventsPerCallback == 0) {
                this->options.maxEventsPerCallback = 1;
            }
            this->options.recovery.multiplier = std::max<std::uint32_t>(1, this->options.recovery.multiplier);
            this->options.recovery.initialDelayMs = std::min(this->options.recovery.initialDelayMs, this->options.recovery.maximumDelayMs);
            if (!this->options.recoveryTimerScheduler) {
                this->options.recoveryTimerScheduler = [](std::uint64_t delayMs, std::function<void()> callback) {
                    auto timer = std::make_shared<std::optional<core::timer::Timer>>();
                    timer->emplace(
                        core::timer::Timer::singleshotTimer(std::move(callback), utils::Timeval(static_cast<double>(delayMs) / 1000.0)));
                    return [timer]() mutable {
                        if (timer && timer->has_value()) {
                            timer->value().cancel();
                            timer->reset();
                        }
                    };
                };
            }
        }

        ~Impl() {
            shutdown();
        }

        void initialize() {
            reducer.apply(state, CapacityConfigured{options.capacity});
            const std::weak_ptr<Impl> weak = weak_from_this();
            client.setOnStateChanged([weak](const StateChange& change) {
                if (const std::shared_ptr<Impl> self = weak.lock()) {
                    self->onStateChanged(change);
                }
            });
            client.setOnDiagnostic([weak](const Diagnostic& diagnostic) {
                if (const std::shared_ptr<Impl> self = weak.lock()) {
                    self->publish(DiagnosticReceived{diagnostic.message});
                }
            });
            client.events().setOnEvent([weak](const typed::Event& event) {
                if (const std::shared_ptr<Impl> self = weak.lock()) {
                    for (BackendEvent translated : self->reducer.translate(event)) {
                        self->publish(std::move(translated));
                    }
                }
            });
            client.requests().setOnRequest([weak](const typed::TypedServerRequest& request) {
                if (const std::shared_ptr<Impl> self = weak.lock()) {
                    self->onServerRequest(request);
                }
            });
        }

        void shutdown() noexcept {
            if (!alive) {
                return;
            }
            alive = false;
            cancelRecoveryTimer();
            ++callbackEpoch;
            sessions.clear();
            observers.clear();
            activeOperations.clear();
            try {
                client.events().setOnEvent({});
                client.requests().setOnRequest({});
                client.setOnStateChanged({});
                client.setOnDiagnostic({});
                client.stop();
            } catch (...) {
            }
        }

        void start() {
            if (!alive) {
                return;
            }
            ProviderState provider = state.provider;
            provider.desiredRunning = true;
            publish(ProviderLifecycleChanged{provider});
            if (provider.lifecycle != ProviderLifecycle::Stopped || !providerStopObserved || client.getState() != State::Stopped) {
                return;
            }
            beginProviderStart();
        }

        void stop() {
            if (!alive) {
                return;
            }
            const bool clientAlreadyStopped = client.getState() == State::Stopped && providerStopObserved;
            restartPending = false;
            restartStartGeneration.reset();
            automaticRecoveryPending = false;
            failurePendingStop = false;
            cancelRecoveryTimer();
            if (operationCallbacksEnabled) {
                operationCallbacksEnabled = false;
                ++callbackEpoch;
            }
            ProviderState provider = state.provider;
            provider.desiredRunning = false;
            provider.recovery = {};
            if (clientAlreadyStopped) {
                provider.lifecycle = ProviderLifecycle::Stopped;
            }
            publish(ProviderLifecycleChanged{provider});
            if (clientAlreadyStopped) {
                cancelActiveOperations("The backend was stopped before the operation completed.");
                return;
            }
            invalidateProvider("backend_stop_requested");
            cancelActiveOperations("The backend was stopped before the operation completed.");
            client.stop();
        }

        void restart() {
            if (!alive || restartPending) {
                return;
            }
            restartPending = true;
            restartStartGeneration.reset();
            automaticRecoveryPending = false;
            failurePendingStop = false;
            cancelRecoveryTimer();
            ProviderState provider = state.provider;
            provider.desiredRunning = true;
            provider.recovery = {};
            publish(ProviderLifecycleChanged{provider});
            if (client.getState() == State::Stopped && providerStopObserved) {
                provider.lifecycle = ProviderLifecycle::Stopped;
                publish(ProviderLifecycleChanged{provider});
                beginProviderStart();
                return;
            }
            operationCallbacksEnabled = false;
            ++callbackEpoch;
            invalidateProvider("manual_provider_restart");
            cancelActiveOperations("The provider was restarted before the operation completed.");
            client.stop();
        }

        void beginProviderStart() {
            if (!alive || !providerStopObserved || client.getState() != State::Stopped) {
                return;
            }
            ProviderState provider = state.provider;
            if (provider.generation == std::numeric_limits<std::uint64_t>::max()) {
                provider.lifecycle = ProviderLifecycle::Failed;
                provider.lastError = Error{Error::Category::Capacity, EOVERFLOW, "Provider generation exhausted."};
                provider.recovery.status = RecoveryStatus::Exhausted;
                provider.recovery.delayMs.reset();
                restartPending = false;
                restartStartGeneration.reset();
                publish(ProviderLifecycleChanged{provider});
                return;
            }
            ++provider.generation;
            provider.lifecycle = ProviderLifecycle::Starting;
            provider.initialization.reset();
            provider.recovery.delayMs.reset();
            if (!publish(ProviderLifecycleChanged{provider})) {
                restartPending = false;
                restartStartGeneration.reset();
                return;
            }
            if (restartPending) {
                restartStartGeneration = provider.generation;
            }
            providerStopObserved = false;
            ++callbackEpoch;
            operationCallbacksEnabled = true;
            initialRefreshGeneration.reset();
            invalidatedGeneration.reset();
            client.start();
        }

        void invalidateProvider(std::string reason) {
            const std::uint64_t generation = state.provider.generation;
            if (invalidatedGeneration == generation) {
                return;
            }
            invalidatedGeneration = generation;
            publish(ProviderConnectionInvalidated{generation, std::move(reason)});
        }

        bool automaticRecoveryEligible(const std::optional<Error>& error) const noexcept {
            return detail::isAutomaticRecoveryEligible(state.provider, options.recovery, error);
        }

        std::uint64_t recoveryDelay(std::uint32_t attempt) const noexcept {
            std::uint64_t delay = options.recovery.initialDelayMs;
            if (attempt <= 1 || delay == 0 || options.recovery.maximumDelayMs == 0 || options.recovery.multiplier == 1) {
                return std::min(delay, options.recovery.maximumDelayMs);
            }
            for (std::uint32_t index = 1; index < attempt; ++index) {
                if (delay >= options.recovery.maximumDelayMs || delay > options.recovery.maximumDelayMs / options.recovery.multiplier) {
                    return options.recovery.maximumDelayMs;
                }
                delay *= options.recovery.multiplier;
            }
            return std::min(delay, options.recovery.maximumDelayMs);
        }

        void cancelRecoveryTimer() noexcept {
            std::shared_ptr<RecoveryTimerGate> gate = std::move(recoveryTimerGate);
            if (gate) {
                gate->active = false;
            }
            RecoveryTimerCancellation cancellation = std::move(recoveryTimerCancellation);
            recoveryTimerCancellation = {};
            if (cancellation) {
                try {
                    cancellation();
                } catch (...) {
                }
            }
        }

        void scheduleRecovery() {
            if (!alive || !automaticRecoveryPending || !state.provider.desiredRunning || client.getState() != State::Stopped) {
                return;
            }
            automaticRecoveryPending = false;
            ProviderState provider = state.provider;
            if (options.recovery.maximumAttempts != 0 && provider.recovery.attempts >= options.recovery.maximumAttempts) {
                provider.lifecycle = ProviderLifecycle::Failed;
                provider.recovery.status = RecoveryStatus::Exhausted;
                provider.recovery.delayMs.reset();
                publish(ProviderLifecycleChanged{provider});
                return;
            }
            const std::uint32_t attempt = provider.recovery.attempts == std::numeric_limits<std::uint32_t>::max()
                                              ? provider.recovery.attempts
                                              : provider.recovery.attempts + 1;
            const std::uint64_t delay = recoveryDelay(attempt);
            provider.lifecycle = ProviderLifecycle::Recovering;
            provider.recovery.status = RecoveryStatus::Waiting;
            provider.recovery.attempts = attempt;
            provider.recovery.delayMs = delay;
            if (!publish(ProviderLifecycleChanged{provider})) {
                return;
            }

            cancelRecoveryTimer();
            auto gate = std::make_shared<RecoveryTimerGate>();
            recoveryTimerGate = gate;
            const std::weak_ptr<Impl> weak = weak_from_this();
            try {
                recoveryTimerCancellation = options.recoveryTimerScheduler(delay, [weak, weakGate = std::weak_ptr{gate}]() {
                    core::EventReceiver::atNextTick([weak, weakGate]() {
                        const std::shared_ptr<RecoveryTimerGate> gate = weakGate.lock();
                        if (const std::shared_ptr<Impl> self = weak.lock(); self && gate && gate->active) {
                            self->onRecoveryTimer(gate);
                        }
                    });
                });
                if (!gate->active) {
                    RecoveryTimerCancellation cancellation = std::move(recoveryTimerCancellation);
                    recoveryTimerCancellation = {};
                    if (cancellation) {
                        cancellation();
                    }
                }
            } catch (...) {
                gate->active = false;
                recoveryTimerGate.reset();
                ProviderState failed = state.provider;
                failed.lifecycle = ProviderLifecycle::Failed;
                failed.recovery.status = RecoveryStatus::Exhausted;
                failed.recovery.delayMs.reset();
                publish(ProviderLifecycleChanged{failed});
            }
        }

        void onRecoveryTimer(const std::shared_ptr<RecoveryTimerGate>& gate) {
            if (!alive || !gate || !gate->active || recoveryTimerGate != gate || !state.provider.desiredRunning ||
                state.provider.lifecycle != ProviderLifecycle::Recovering || state.provider.recovery.status != RecoveryStatus::Waiting ||
                client.getState() != State::Stopped) {
                return;
            }
            gate->active = false;
            recoveryTimerGate.reset();
            recoveryTimerCancellation = {};
            ProviderState provider = state.provider;
            provider.recovery.status = RecoveryStatus::Idle;
            provider.recovery.delayMs.reset();
            if (!publish(ProviderLifecycleChanged{provider})) {
                return;
            }
            beginProviderStart();
        }

        SessionBinding createSession(FrontendSessionCallbacks callbacks) {
            if (!alive || nextSessionId == 0) {
                return {};
            }
            if (sessions.size() >= options.capacity.maxSessions) {
                publish(CapacityChanged{CapacityMetric::RejectedSessions, 1});
                return {};
            }
            const SessionId id{nextSessionId};
            if (nextSessionId == std::numeric_limits<std::uint64_t>::max()) {
                nextSessionId = 0;
            } else {
                ++nextSessionId;
            }

            auto record = std::make_shared<SessionRecord>();
            record->id = id;
            record->callbacks = std::move(callbacks);
            sessions.emplace(id, record);
            publish(SessionChanged{id, true, SessionRole::Observer});

            const std::weak_ptr<Impl> weak = weak_from_this();
            return SessionBinding{id,
                                  [weak, id]() {
                                      const std::shared_ptr<Impl> self = weak.lock();
                                      return self ? self->sessionRole(id) : SessionRole::Observer;
                                  },
                                  [weak, id]() {
                                      const std::shared_ptr<Impl> self = weak.lock();
                                      return self && self->isSessionOpen(id);
                                  },
                                  [weak, id](std::string requestId, BackendCommand command) {
                                      const std::shared_ptr<Impl> self = weak.lock();
                                      return self ? self->submit(id, std::move(requestId), std::move(command))
                                                  : CommandSubmission{false, SubmissionError::Closed, "The backend no longer exists."};
                                  },
                                  [weak, id]() {
                                      const std::shared_ptr<Impl> self = weak.lock();
                                      return self && self->enqueueSnapshot(id);
                                  },
                                  [weak, id](std::string reason) {
                                      if (const std::shared_ptr<Impl> self = weak.lock()) {
                                          self->closeSession(id, std::move(reason));
                                      }
                                  }};
        }

        ObserverBinding createObserver(BackendObserverCallbacks callbacks) {
            if (!alive || nextObserverId == 0) {
                return {};
            }
            if (observers.size() >= options.capacity.maxObservers) {
                publish(CapacityChanged{CapacityMetric::RejectedObservers, 1});
                return {};
            }
            const std::uint64_t id = nextObserverId;
            if (nextObserverId == std::numeric_limits<std::uint64_t>::max()) {
                nextObserverId = 0;
            } else {
                ++nextObserverId;
            }
            auto record = std::make_shared<ObserverRecord>();
            record->id = id;
            record->callbacks = std::move(callbacks);
            observers.emplace(id, record);
            const std::weak_ptr<Impl> weak = weak_from_this();
            return ObserverBinding{[weak, id]() {
                                       const std::shared_ptr<Impl> self = weak.lock();
                                       return self && self->observers.contains(id);
                                   },
                                   [weak, id]() {
                                       if (const std::shared_ptr<Impl> self = weak.lock()) {
                                           self->closeObserver(id);
                                       }
                                   }};
        }

        BackendState copyState() const {
            return state;
        }

        Snapshot makeCurrentSnapshot() {
            const SequenceNumber sourceSequence = state.sequence;
            Snapshot snapshot = makeSnapshot(state);
            const std::uint64_t omissions = snapshot.capacity.state.snapshotOmissions > state.capacity.snapshotOmissions
                                                ? snapshot.capacity.state.snapshotOmissions - state.capacity.snapshotOmissions
                                                : 0;
            const bool alreadyAccounted = snapshotOmissionsAccountedAtSequence && *snapshotOmissionsAccountedAtSequence == sourceSequence;
            if (omissions != 0 && !alreadyAccounted && !accountingSnapshotOmissions) {
                struct AccountingGuard {
                    bool& active;

                    ~AccountingGuard() {
                        active = false;
                    }
                } guard{accountingSnapshotOmissions};
                accountingSnapshotOmissions = true;
                if (publish(CapacityChanged{CapacityMetric::SnapshotOmissions, omissions})) {
                    snapshotOmissionsAccountedAtSequence = state.sequence;
                    snapshot = makeSnapshot(state);
                    snapshot.capacity.state.snapshotOmissions = state.capacity.snapshotOmissions;
                    snapshot.sequence = state.sequence;
                }
            }
            if (omissions != 0 && (alreadyAccounted || accountingSnapshotOmissions ||
                                   snapshot.capacity.state.snapshotOmissions > state.capacity.snapshotOmissions)) {
                snapshot.capacity.state.snapshotOmissions = state.capacity.snapshotOmissions;
            }
            return snapshot;
        }

        bool ready() const noexcept {
            return state.provider.lifecycle == ProviderLifecycle::Ready;
        }

    private:
        using Outbound = std::variant<SequencedBackendEvent, Snapshot, CommandCompletion>;

        struct QueueEntry {
            Outbound value;
            std::size_t bytes = 0;
        };

        struct SessionRecord {
            SessionId id;
            FrontendSessionCallbacks callbacks;
            std::deque<QueueEntry> queue;
            std::size_t queuedBytes = 0;
            bool drainScheduled = false;
            bool closed = false;
            std::set<std::string> pendingRequestIds;
            std::set<std::string> queuedCompletions;
        };

        struct ObserverRecord {
            std::uint64_t id = 0;
            BackendObserverCallbacks callbacks;
            std::deque<SequencedBackendEvent> events;
            std::size_t queuedBytes = 0;
            bool drainScheduled = false;
            bool needsResynchronize = false;
            bool closed = false;
        };

        using OperationKey = std::pair<SessionId, std::string>;

        struct ActiveOperation {
            std::uint64_t providerGeneration = 0;
            std::uint64_t callbackEpoch = 0;
        };

        template <typename Callback>
        static void invokeBounded(Callback& callback) noexcept {
            if (!callback) {
                return;
            }
            try {
                callback();
            } catch (...) {
            }
        }

        void schedule(std::function<void()> callback) noexcept {
            const std::weak_ptr<Impl> weak = weak_from_this();
            std::function<void()> guarded = [weak, callback = std::move(callback)]() mutable {
                if (weak.lock()) {
                    try {
                        callback();
                    } catch (...) {
                    }
                }
            };
            try {
                options.scheduler(guarded);
            } catch (...) {
                // Preserve the asynchronous callback contract even when an
                // injected scheduler rejects a callback.
                try {
                    core::EventReceiver::atNextTick(std::move(guarded));
                } catch (...) {
                }
            }
        }

        bool emitReducedEvent(BackendEvent event) {
            if (!alive) {
                return false;
            }
            if (state.sequence.value() == std::numeric_limits<std::uint64_t>::max()) {
                state.sequenceExhausted = true;
                state.provider.lifecycle = ProviderLifecycle::Failed;
                state.provider.lastError = Error{Error::Category::Capacity, EOVERFLOW, "Backend sequence number exhausted."};
                client.stop();
                return false;
            }
            state.sequence = SequenceNumber{state.sequence.value() + 1};
            SequencedBackendEvent sequenced{state.sequence, std::move(event)};

            std::vector<SessionId> sessionIds;
            sessionIds.reserve(sessions.size());
            for (const auto& [id, record] : sessions) {
                // A session used only for commands/roles does not consume the
                // backend event stream. Do not mirror high-granularity events
                // into a queue that has no callback; the frontend adapter has
                // one shared observer which performs normalization/coalescing.
                if (!record->closed && record->callbacks.onEvents) {
                    sessionIds.push_back(id);
                }
            }
            for (const SessionId id : sessionIds) {
                enqueueEvent(id, sequenced);
            }

            std::vector<std::uint64_t> observerIds;
            observerIds.reserve(observers.size());
            for (const auto& [id, record] : observers) {
                if (!record->closed) {
                    observerIds.push_back(id);
                }
            }
            for (const std::uint64_t id : observerIds) {
                enqueueObserverEvent(id, sequenced);
            }
            return true;
        }

        bool publish(BackendEvent event) {
            if (!alive) {
                return false;
            }
            if (state.sequence.value() == std::numeric_limits<std::uint64_t>::max()) {
                state.sequenceExhausted = true;
                state.provider.lifecycle = ProviderLifecycle::Failed;
                state.provider.lastError = Error{Error::Category::Capacity, EOVERFLOW, "Backend sequence number exhausted."};
                client.stop();
                return false;
            }

            const Reduction reduction = reducer.apply(state, event);
            if (!reduction.changed || !emitReducedEvent(std::move(event))) {
                return false;
            }
            for (const CapacityChanged& capacityChange : reduction.capacityChanges) {
                if (!emitReducedEvent(capacityChange)) {
                    return false;
                }
            }
            for (const PendingRequestRemoved& pendingRequestRemoval : reduction.pendingRequestRemovals) {
                if (!emitReducedEvent(pendingRequestRemoval)) {
                    return false;
                }
            }
            return true;
        }

        void enqueueEvent(SessionId id, const SequencedBackendEvent& event) {
            const auto iterator = sessions.find(id);
            if (iterator == sessions.end() || iterator->second->closed) {
                return;
            }
            const std::size_t bytes = eventBytes(event);
            if (!queueFits(*iterator->second, bytes)) {
                closeSession(id, "frontend session outbound queue capacity exceeded");
                return;
            }
            iterator->second->queue.push_back({event, bytes});
            iterator->second->queuedBytes += bytes;
            scheduleSessionDrain(iterator->second);
        }

        bool enqueueSnapshot(SessionId id) {
            const auto iterator = sessions.find(id);
            if (iterator == sessions.end() || iterator->second->closed) {
                return false;
            }
            Snapshot snapshot = makeCurrentSnapshot();
            const std::size_t bytes = snapshotBytes(snapshot);
            if (!queueFits(*iterator->second, bytes)) {
                closeSession(id, "frontend session outbound queue capacity exceeded");
                return false;
            }
            iterator->second->queue.push_back({std::move(snapshot), bytes});
            iterator->second->queuedBytes += bytes;
            scheduleSessionDrain(iterator->second);
            return true;
        }

        bool queueFits(const SessionRecord& record, std::size_t bytes) const noexcept {
            return record.queue.size() < options.maxSessionQueueEntries && bytes <= options.maxSessionQueueBytes &&
                   record.queuedBytes <= options.maxSessionQueueBytes - bytes;
        }

        void scheduleSessionDrain(const std::shared_ptr<SessionRecord>& record) {
            if (record->drainScheduled || record->closed) {
                return;
            }
            record->drainScheduled = true;
            const std::weak_ptr<Impl> weak = weak_from_this();
            const SessionId id = record->id;
            schedule([weak, id]() {
                if (const std::shared_ptr<Impl> self = weak.lock()) {
                    self->drainSession(id);
                }
            });
        }

        void drainSession(SessionId id) {
            const auto iterator = sessions.find(id);
            if (iterator == sessions.end()) {
                return;
            }
            const std::shared_ptr<SessionRecord> record = iterator->second;
            record->drainScheduled = false;
            if (record->closed || record->queue.empty()) {
                return;
            }

            if (std::holds_alternative<SequencedBackendEvent>(record->queue.front().value)) {
                std::vector<SequencedBackendEvent> events;
                while (!record->queue.empty() && events.size() < options.maxEventsPerCallback &&
                       std::holds_alternative<SequencedBackendEvent>(record->queue.front().value)) {
                    record->queuedBytes -= record->queue.front().bytes;
                    events.push_back(std::get<SequencedBackendEvent>(std::move(record->queue.front().value)));
                    record->queue.pop_front();
                }
                auto callback = record->callbacks.onEvents;
                invokeBounded(callback, events);
            } else if (std::holds_alternative<Snapshot>(record->queue.front().value)) {
                Snapshot snapshot = std::get<Snapshot>(std::move(record->queue.front().value));
                record->queuedBytes -= record->queue.front().bytes;
                record->queue.pop_front();
                auto callback = record->callbacks.onSnapshot;
                invokeBounded(callback, snapshot);
            } else {
                CommandCompletion completion = std::get<CommandCompletion>(std::move(record->queue.front().value));
                record->queuedBytes -= record->queue.front().bytes;
                record->queue.pop_front();
                record->pendingRequestIds.erase(completion.requestId);
                record->queuedCompletions.erase(completion.requestId);
                auto callback = record->callbacks.onCommandCompleted;
                invokeBounded(callback, completion);
            }

            if (!record->closed && !record->queue.empty()) {
                scheduleSessionDrain(record);
            }
        }

        template <typename Callback, typename Value>
        static void invokeBounded(Callback& callback, const Value& value) noexcept {
            if (!callback) {
                return;
            }
            try {
                callback(value);
            } catch (...) {
            }
        }

        void enqueueObserverEvent(std::uint64_t id, const SequencedBackendEvent& event) {
            const auto iterator = observers.find(id);
            if (iterator == observers.end() || iterator->second->closed) {
                return;
            }
            ObserverRecord& record = *iterator->second;
            const std::size_t bytes = eventBytes(event);
            if (record.needsResynchronize || record.events.size() >= options.maxObserverQueueEntries ||
                bytes > options.maxObserverQueueBytes || record.queuedBytes > options.maxObserverQueueBytes - bytes) {
                record.events.clear();
                record.queuedBytes = 0;
                record.needsResynchronize = true;
            } else {
                record.events.push_back(event);
                record.queuedBytes += bytes;
            }
            scheduleObserverDrain(iterator->second);
        }

        void scheduleObserverDrain(const std::shared_ptr<ObserverRecord>& record) {
            if (record->drainScheduled || record->closed) {
                return;
            }
            record->drainScheduled = true;
            const std::weak_ptr<Impl> weak = weak_from_this();
            const std::uint64_t id = record->id;
            schedule([weak, id]() {
                if (const std::shared_ptr<Impl> self = weak.lock()) {
                    self->drainObserver(id);
                }
            });
        }

        void drainObserver(std::uint64_t id) {
            const auto iterator = observers.find(id);
            if (iterator == observers.end()) {
                return;
            }
            const std::shared_ptr<ObserverRecord> record = iterator->second;
            record->drainScheduled = false;
            if (record->closed) {
                return;
            }
            if (record->needsResynchronize) {
                record->needsResynchronize = false;
                auto callback = record->callbacks.onResynchronize;
                const Snapshot snapshot = makeCurrentSnapshot();
                invokeBounded(callback, snapshot);
            } else if (!record->events.empty()) {
                std::vector<SequencedBackendEvent> events;
                while (!record->events.empty() && events.size() < options.maxEventsPerCallback) {
                    record->queuedBytes -= eventBytes(record->events.front());
                    events.push_back(std::move(record->events.front()));
                    record->events.pop_front();
                }
                auto callback = record->callbacks.onEvents;
                invokeBounded(callback, events);
            }
            if (!record->closed && (record->needsResynchronize || !record->events.empty())) {
                scheduleObserverDrain(record);
            }
        }

        void closeObserver(std::uint64_t id) noexcept {
            const auto iterator = observers.find(id);
            if (iterator == observers.end()) {
                return;
            }
            iterator->second->closed = true;
            iterator->second->events.clear();
            iterator->second->queuedBytes = 0;
            observers.erase(iterator);
        }

        void closeSession(SessionId id, std::string reason) noexcept {
            const auto iterator = sessions.find(id);
            if (iterator == sessions.end()) {
                return;
            }
            const std::shared_ptr<SessionRecord> record = iterator->second;
            record->closed = true;
            record->queue.clear();
            record->queuedBytes = 0;
            record->pendingRequestIds.clear();
            record->queuedCompletions.clear();
            sessions.erase(iterator);

            try {
                if (state.controller == id) {
                    publish(ControllerChanged{std::nullopt});
                }
                publish(SessionChanged{id, false, SessionRole::Observer});
            } catch (...) {
            }

            auto callback = record->callbacks.onClosed;
            schedule([callback = std::move(callback), reason = std::move(reason)]() mutable {
                invokeBounded(callback, reason);
            });
        }

        SessionRole sessionRole(SessionId id) const noexcept {
            return state.controller == id ? SessionRole::Controller : SessionRole::Observer;
        }

        bool isSessionOpen(SessionId id) const noexcept {
            const auto iterator = sessions.find(id);
            return iterator != sessions.end() && !iterator->second->closed;
        }

        CommandSubmission submit(SessionId id, std::string requestId, BackendCommand command) {
            const auto iterator = sessions.find(id);
            if (iterator == sessions.end() || iterator->second->closed) {
                return {false, SubmissionError::Closed, "The frontend session is closed."};
            }
            if (requestId.empty()) {
                return {false, SubmissionError::EmptyRequestId, "A non-empty request id is required."};
            }
            if (iterator->second->pendingRequestIds.contains(requestId)) {
                return {false, SubmissionError::DuplicateRequestId, "The request id is already pending in this session."};
            }
            if (iterator->second->queue.size() >= options.maxSessionQueueEntries) {
                return {false, SubmissionError::QueueFull, "The frontend session outbound queue is full."};
            }
            iterator->second->pendingRequestIds.insert(requestId);

            if (requiresController(command) && state.controller != id) {
                complete(id, requestId, CommandResult::failed(CommandErrorCode::PermissionDenied, "The controller role is required."));
                return {true, SubmissionError::None, {}};
            }
            if (requiresReadyBackend(command) && state.provider.lifecycle != ProviderLifecycle::Ready) {
                complete(id, requestId, CommandResult::failed(CommandErrorCode::BackendUnavailable, "The Codex App Server is not ready."));
                return {true, SubmissionError::None, {}};
            }
            if (requiresReadyBackend(command) && activeOperations.size() >= options.capacity.maxActiveOperations) {
                publish(CapacityChanged{CapacityMetric::RejectedOperations, 1});
                schedule([weak = weak_from_this(), id, requestId]() {
                    if (const std::shared_ptr<Impl> self = weak.lock()) {
                        self->complete(id,
                                       requestId,
                                       CommandResult::failed(CommandErrorCode::LocalSubmissionFailure,
                                                             "The backend active-operation capacity is exhausted."));
                    }
                });
                return {true, SubmissionError::None, {}};
            }

            execute(id, requestId, std::move(command));
            return {true, SubmissionError::None, {}};
        }

        void execute(SessionId id, const std::string& requestId, BackendCommand command) {
            std::visit(
                Overloaded{
                    [this, id, &requestId](const ControllerAcquire&) {
                        if (!state.controller) {
                            publish(ControllerChanged{id});
                        } else if (state.controller != id) {
                            complete(id,
                                     requestId,
                                     CommandResult::failed(CommandErrorCode::Conflict, "Another frontend session is the controller."));
                            return;
                        }
                        complete(id, requestId, CommandResult::succeeded(ControllerResult{state.controller, SessionRole::Controller}));
                    },
                    [this, id, &requestId](const ControllerRelease&) {
                        if (state.controller != id) {
                            complete(id,
                                     requestId,
                                     CommandResult::failed(CommandErrorCode::PermissionDenied,
                                                           "Only the current controller can release the controller role."));
                            return;
                        }
                        publish(ControllerChanged{std::nullopt});
                        complete(id, requestId, CommandResult::succeeded(ControllerResult{std::nullopt, SessionRole::Observer}));
                    },
                    [this, id, &requestId](const SnapshotGet&) {
                        complete(id, requestId, CommandResult::succeeded(makeCurrentSnapshot()));
                    },
                    [this, id, &requestId](const ThreadStart& value) {
                        startThread(id, requestId, value);
                    },
                    [this, id, &requestId](const ThreadResume& value) {
                        resumeThread(id, requestId, value);
                    },
                    [this, id, &requestId](const ThreadList& value) {
                        listThreads(id, requestId, value);
                    },
                    [this, id, &requestId](const ThreadRead& value) {
                        readThread(id, requestId, value);
                    },
                    [this, id, &requestId](const TurnStart& value) {
                        startTurn(id, requestId, value);
                    },
                    [this, id, &requestId](const TurnInterrupt& value) {
                        interruptTurn(id, requestId, value);
                    },
                    [this, id, &requestId](const ApprovalRespond& value) {
                        respondApproval(id, requestId, value);
                    },
                    [this, id, &requestId](const UserInputRespond& value) {
                        respondUserInput(id, requestId, value);
                    },
                    [this, id, &requestId](const AuthenticationRespond& value) {
                        respondAuthentication(id, requestId, value);
                    },
                    [this, id, &requestId](const UnknownRequestRespondRaw& value) {
                        respondUnknown(id, requestId, value);
                    },
                    [this, id, &requestId](const UnknownRequestReject& value) {
                        rejectUnknown(id, requestId, value);
                    }},
                command);
        }

        void complete(SessionId id, const std::string& requestId, CommandResult result) {
            activeOperations.erase({id, requestId});
            const auto iterator = sessions.find(id);
            if (iterator == sessions.end() || iterator->second->closed || !iterator->second->pendingRequestIds.contains(requestId) ||
                iterator->second->queuedCompletions.contains(requestId)) {
                return;
            }
            CommandCompletion completion{requestId, std::move(result)};
            const std::size_t bytes = commandCompletionBytes(completion);
            if (!queueFits(*iterator->second, bytes)) {
                closeSession(id, "frontend session outbound queue capacity exceeded");
                return;
            }
            iterator->second->queuedCompletions.insert(requestId);
            iterator->second->queue.push_back({std::move(completion), bytes});
            iterator->second->queuedBytes += bytes;
            scheduleSessionDrain(iterator->second);
        }

        void markOperation(SessionId id, const std::string& requestId, std::uint64_t providerGeneration, std::uint64_t operationEpoch) {
            activeOperations.insert_or_assign({id, requestId}, ActiveOperation{providerGeneration, operationEpoch});
        }

        bool acceptsCompletion(std::uint64_t providerGeneration, std::uint64_t operationEpoch) const noexcept {
            return alive && operationCallbacksEnabled && providerGeneration == state.provider.generation && operationEpoch == callbackEpoch;
        }

        void startThread(SessionId id, const std::string& requestId, const ThreadStart& command) {
            const std::uint64_t providerGeneration = state.provider.generation;
            const std::uint64_t operationEpoch = callbackEpoch;
            markOperation(id, requestId, providerGeneration, operationEpoch);
            const std::weak_ptr<Impl> weak = weak_from_this();
            const auto submission =
                client.threads().start(command.params, [weak, id, requestId, providerGeneration, operationEpoch](const auto& result) {
                    if (const std::shared_ptr<Impl> self = weak.lock();
                        self && self->acceptsCompletion(providerGeneration, operationEpoch)) {
                        if (result && result.value) {
                            self->publish(ThreadUpserted{result.value->thread, EntityLoad::Summary});
                            self->complete(id, requestId, CommandResult::succeeded(result.value->thread));
                        } else {
                            self->complete(id, requestId, operationFailure(result));
                        }
                    }
                });
            if (!submission) {
                complete(id, requestId, submissionFailure(submission));
            }
        }

        void resumeThread(SessionId id, const std::string& requestId, const ThreadResume& command) {
            const std::uint64_t providerGeneration = state.provider.generation;
            const std::uint64_t operationEpoch = callbackEpoch;
            markOperation(id, requestId, providerGeneration, operationEpoch);
            const std::weak_ptr<Impl> weak = weak_from_this();
            const auto submission =
                client.threads().resume(command.params, [weak, id, requestId, providerGeneration, operationEpoch](const auto& result) {
                    if (const std::shared_ptr<Impl> self = weak.lock();
                        self && self->acceptsCompletion(providerGeneration, operationEpoch)) {
                        if (result && result.value) {
                            self->publish(ThreadUpserted{result.value->thread, EntityLoad::Summary});
                            self->complete(id, requestId, CommandResult::succeeded(result.value->thread));
                        } else {
                            self->complete(id, requestId, operationFailure(result));
                        }
                    }
                });
            if (!submission) {
                complete(id, requestId, submissionFailure(submission));
            }
        }

        void listThreads(SessionId id, const std::string& requestId, const ThreadList& command) {
            const std::uint64_t providerGeneration = state.provider.generation;
            const std::uint64_t operationEpoch = callbackEpoch;
            const std::optional<std::string> requestedCursor =
                command.params.cursor.hasValue() ? std::optional<std::string>{*command.params.cursor} : std::nullopt;
            markOperation(id, requestId, providerGeneration, operationEpoch);
            const std::weak_ptr<Impl> weak = weak_from_this();
            const auto submission = client.threads().list(
                command.params, [weak, id, requestId, providerGeneration, operationEpoch, requestedCursor](const auto& result) {
                    if (const std::shared_ptr<Impl> self = weak.lock();
                        self && self->acceptsCompletion(providerGeneration, operationEpoch)) {
                        if (result && result.value) {
                            self->publish(ThreadListUpdated{*result.value, requestedCursor, false});
                            self->complete(id, requestId, CommandResult::succeeded(*result.value));
                        } else {
                            self->complete(id, requestId, operationFailure(result));
                        }
                    }
                });
            if (!submission) {
                complete(id, requestId, submissionFailure(submission));
            }
        }

        void readThread(SessionId id, const std::string& requestId, const ThreadRead& command) {
            const std::uint64_t providerGeneration = state.provider.generation;
            const std::uint64_t operationEpoch = callbackEpoch;
            markOperation(id, requestId, providerGeneration, operationEpoch);
            const std::weak_ptr<Impl> weak = weak_from_this();
            const auto submission = client.threads().read(
                command.params, [weak, id, requestId, providerGeneration, operationEpoch, command](const auto& result) {
                    if (const std::shared_ptr<Impl> self = weak.lock();
                        self && self->acceptsCompletion(providerGeneration, operationEpoch)) {
                        if (result && result.value) {
                            const EntityLoad load = command.params.includeTurns.value_or(false) ? EntityLoad::Full : EntityLoad::Summary;
                            self->publish(ThreadUpserted{result.value->thread, load});
                            self->complete(id, requestId, CommandResult::succeeded(result.value->thread));
                        } else {
                            self->complete(id, requestId, operationFailure(result));
                        }
                    }
                });
            if (!submission) {
                complete(id, requestId, submissionFailure(submission));
            }
        }

        void startTurn(SessionId id, const std::string& requestId, const TurnStart& command) {
            const std::uint64_t providerGeneration = state.provider.generation;
            const std::uint64_t operationEpoch = callbackEpoch;
            markOperation(id, requestId, providerGeneration, operationEpoch);
            const std::weak_ptr<Impl> weak = weak_from_this();
            const auto submission =
                client.turns().start(command.params, [weak, id, requestId, providerGeneration, operationEpoch](const auto& result) {
                    if (const std::shared_ptr<Impl> self = weak.lock();
                        self && self->acceptsCompletion(providerGeneration, operationEpoch)) {
                        if (result && result.value) {
                            self->publish(TurnUpserted{result.value->turn});
                            self->complete(id, requestId, CommandResult::succeeded(result.value->turn));
                        } else {
                            self->complete(id, requestId, operationFailure(result));
                        }
                    }
                });
            if (!submission) {
                complete(id, requestId, submissionFailure(submission));
            }
        }

        void interruptTurn(SessionId id, const std::string& requestId, const TurnInterrupt& command) {
            const std::uint64_t providerGeneration = state.provider.generation;
            const std::uint64_t operationEpoch = callbackEpoch;
            markOperation(id, requestId, providerGeneration, operationEpoch);
            const std::weak_ptr<Impl> weak = weak_from_this();
            const auto submission =
                client.turns().interrupt(command.params, [weak, id, requestId, providerGeneration, operationEpoch](const auto& result) {
                    if (const std::shared_ptr<Impl> self = weak.lock();
                        self && self->acceptsCompletion(providerGeneration, operationEpoch)) {
                        if (result && result.value) {
                            self->complete(id, requestId, CommandResult::succeeded(*result.value));
                        } else {
                            self->complete(id, requestId, operationFailure(result));
                        }
                    }
                });
            if (!submission) {
                complete(id, requestId, submissionFailure(submission));
            }
        }

        template <typename Request, typename Respond>
        void respondTo(
            SessionId id, const std::string& commandRequestId, PendingRequestId pendingId, Respond respond, const char* expectedType) {
            const auto iterator = state.pendingRequests.find(pendingId);
            if (iterator == state.pendingRequests.end()) {
                complete(id, commandRequestId, CommandResult::failed(CommandErrorCode::NotFound, "The pending request no longer exists."));
                return;
            }
            const Request* request = std::get_if<Request>(&iterator->second.request);
            if (!request) {
                complete(id,
                         commandRequestId,
                         CommandResult::failed(CommandErrorCode::InvalidCommand,
                                               std::string("The pending request is not a ") + expectedType + " request."));
                return;
            }
            const auto send = respond(*request);
            if (!send) {
                complete(id,
                         commandRequestId,
                         CommandResult::failed(CommandErrorCode::LocalSubmissionFailure,
                                               send.error ? send.error->message : "The App Server response could not be enqueued."));
                return;
            }
            publish(PendingRequestRemoved{pendingId, "response_enqueued"});
            complete(id, commandRequestId, CommandResult::succeeded());
        }

        void respondApproval(SessionId id, const std::string& requestId, const ApprovalRespond& command) {
            const auto iterator = state.pendingRequests.find(command.requestId);
            if (iterator == state.pendingRequests.end()) {
                complete(id, requestId, CommandResult::failed(CommandErrorCode::NotFound, "The pending request no longer exists."));
            } else if (std::holds_alternative<typed::CommandApprovalRequest>(iterator->second.request)) {
                respondTo<typed::CommandApprovalRequest>(
                    id,
                    requestId,
                    command.requestId,
                    [this, &command](const auto& request) {
                        return client.requests().respond(request, command.decision);
                    },
                    "command approval");
            } else {
                respondTo<typed::FileChangeApprovalRequest>(
                    id,
                    requestId,
                    command.requestId,
                    [this, &command](const auto& request) {
                        return client.requests().respond(request, command.decision);
                    },
                    "file-change approval");
            }
        }

        void respondUserInput(SessionId id, const std::string& requestId, const UserInputRespond& command) {
            respondTo<typed::UserInputRequest>(
                id,
                requestId,
                command.requestId,
                [this, &command](const auto& request) {
                    return client.requests().respond(request, command.answers);
                },
                "user-input");
        }

        void respondAuthentication(SessionId id, const std::string& requestId, const AuthenticationRespond& command) {
            respondTo<typed::AuthenticationRequest>(
                id,
                requestId,
                command.requestId,
                [this, &command](const auto& request) {
                    return client.requests().respond(request, command.response);
                },
                "authentication");
        }

        void respondUnknown(SessionId id, const std::string& requestId, const UnknownRequestRespondRaw& command) {
            respondTo<typed::UnknownServerRequest>(
                id,
                requestId,
                command.requestId,
                [this, &command](const auto& request) {
                    return client.requests().respondRaw(request, command.result);
                },
                "unknown extension");
        }

        void rejectUnknown(SessionId id, const std::string& requestId, const UnknownRequestReject& command) {
            respondTo<typed::UnknownServerRequest>(
                id,
                requestId,
                command.requestId,
                [this, &command](const auto& request) {
                    return client.requests().reject(request, command.error);
                },
                "unknown extension");
        }

        void onStateChanged(const StateChange& change) {
            const ProviderLifecycle lifecycle = toProviderLifecycle(change.current);
            ProviderState provider = state.provider;

            if ((lifecycle == ProviderLifecycle::Starting || lifecycle == ProviderLifecycle::Initializing ||
                 lifecycle == ProviderLifecycle::Ready) &&
                (!provider.desiredRunning || (restartPending && !restartStartGeneration))) {
                return;
            }

            if (lifecycle == ProviderLifecycle::Failed) {
                if (operationCallbacksEnabled) {
                    operationCallbacksEnabled = false;
                    ++callbackEpoch;
                }
                invalidateProvider("app_server_connection_failed");
                cancelActiveOperations("The App Server connection failed before the operation completed.");
                if (change.error) {
                    provider.lastError = change.error;
                }
                if (!provider.desiredRunning) {
                    automaticRecoveryPending = false;
                    failurePendingStop = false;
                    restartPending = false;
                    restartStartGeneration.reset();
                    provider.lifecycle = providerStopObserved && client.getState() == State::Stopped ? ProviderLifecycle::Stopped
                                                                                                     : ProviderLifecycle::Stopping;
                    provider.recovery = {};
                    publish(ProviderLifecycleChanged{provider});
                    client.stop();
                    return;
                }

                const bool replacementFailed = restartStartGeneration && *restartStartGeneration == provider.generation;
                if (replacementFailed) {
                    restartPending = false;
                    restartStartGeneration.reset();
                }
                automaticRecoveryPending = !restartPending && automaticRecoveryEligible(change.error);
                const bool retryableCategory = change.error && (change.error->category == Error::Category::Transport ||
                                                                change.error->category == Error::Category::Process);
                if (!automaticRecoveryPending && provider.desiredRunning && options.recovery.enabled && retryableCategory &&
                    options.recovery.maximumAttempts != 0 && provider.recovery.attempts >= options.recovery.maximumAttempts) {
                    provider.recovery.status = RecoveryStatus::Exhausted;
                    provider.recovery.delayMs.reset();
                } else if (!automaticRecoveryPending) {
                    provider.recovery.status = RecoveryStatus::Idle;
                    provider.recovery.delayMs.reset();
                }
                provider.lifecycle = ProviderLifecycle::Failed;
                publish(ProviderLifecycleChanged{provider});
                failurePendingStop = true;
                client.stop();
                return;
            }

            if (lifecycle == ProviderLifecycle::Stopping || lifecycle == ProviderLifecycle::Stopped) {
                if (lifecycle == ProviderLifecycle::Stopped) {
                    providerStopObserved = true;
                }
                if (operationCallbacksEnabled) {
                    operationCallbacksEnabled = false;
                    ++callbackEpoch;
                }
                invalidateProvider("app_server_connection_invalidated");
                cancelActiveOperations("The App Server connection stopped before the operation completed.");
                if (lifecycle == ProviderLifecycle::Stopping) {
                    provider.lifecycle = ProviderLifecycle::Stopping;
                    publish(ProviderLifecycleChanged{provider});
                    return;
                }

                if (restartPending) {
                    provider.lifecycle = ProviderLifecycle::Stopped;
                    publish(ProviderLifecycleChanged{provider});
                    automaticRecoveryPending = false;
                    failurePendingStop = false;
                    beginProviderStart();
                    return;
                }
                if (automaticRecoveryPending) {
                    provider.lifecycle = ProviderLifecycle::Stopped;
                    publish(ProviderLifecycleChanged{provider});
                    failurePendingStop = false;
                    scheduleRecovery();
                    return;
                }
                provider.lifecycle = failurePendingStop ? ProviderLifecycle::Failed : ProviderLifecycle::Stopped;
                failurePendingStop = false;
                publish(ProviderLifecycleChanged{provider});
                return;
            }

            provider.lifecycle = lifecycle;
            if (lifecycle == ProviderLifecycle::Ready) {
                provider.initialization = client.getInitializeResponse();
                provider.lastError.reset();
                provider.recovery = {};
                automaticRecoveryPending = false;
                failurePendingStop = false;
                if (restartStartGeneration && *restartStartGeneration == provider.generation) {
                    restartPending = false;
                    restartStartGeneration.reset();
                }
            }
            publish(ProviderLifecycleChanged{provider});
            if (lifecycle == ProviderLifecycle::Ready) {
                initialRefresh();
            }
        }

        void initialRefresh() {
            const std::uint64_t providerGeneration = state.provider.generation;
            if (options.initialThreadListLimit == 0 || initialRefreshGeneration == providerGeneration) {
                return;
            }
            initialRefreshGeneration = providerGeneration;
            typed::ThreadListParams listParams;
            listParams.limit = options.initialThreadListLimit;
            const std::uint64_t operationEpoch = callbackEpoch;
            const std::weak_ptr<Impl> weak = weak_from_this();
            const auto submission =
                client.threads().list(std::move(listParams), [weak, providerGeneration, operationEpoch](const auto& result) {
                    if (const std::shared_ptr<Impl> self = weak.lock();
                        self && self->acceptsCompletion(providerGeneration, operationEpoch)) {
                        if (result && result.value) {
                            self->publish(ThreadListUpdated{*result.value, std::nullopt, true});
                        } else {
                            const CommandResult failure = operationFailure(result);
                            self->publish(
                                DiagnosticReceived{failure.error ? failure.error->message : "Initial bounded thread refresh failed."});
                        }
                    }
                });
            if (!submission) {
                publish(DiagnosticReceived{submission.error ? submission.error->message
                                                            : "Initial bounded thread refresh could not be submitted."});
            }
        }

        void onServerRequest(const typed::TypedServerRequest& request) {
            if (state.pendingRequests.size() >= options.capacity.maxPendingRequests) {
                failProviderForPendingRequestCapacity(
                    ENOBUFS, "Pending server-request capacity exhausted.", "pending_server_request_capacity_exhausted");
                return;
            }
            // A1.4b exposes these requests through the reusable typed Requests
            // component only. The backend deliberately adds no product state
            // or frontend behavior for attestation, dynamic tools, or MCP
            // elicitation.
            if (std::holds_alternative<typed::AttestationGenerateRequest>(request) ||
                std::holds_alternative<typed::DynamicToolCallRequest>(request) ||
                std::holds_alternative<typed::McpServerElicitationRequest>(request)) {
                return;
            }
            if (nextPendingRequestId == 0) {
                failProviderForPendingRequestCapacity(
                    EOVERFLOW, "Pending server-request identifier capacity exhausted.", "pending_server_request_id_exhausted");
                return;
            }
            const PendingRequestId id{nextPendingRequestId};
            if (nextPendingRequestId == std::numeric_limits<std::uint64_t>::max()) {
                nextPendingRequestId = 0;
            } else {
                ++nextPendingRequestId;
            }
            publish(PendingRequestAdded{PendingRequestState{id, request, state.provider.generation}});
        }

        void failProviderForPendingRequestCapacity(int code, std::string message, std::string reason) {
            publish(CapacityChanged{CapacityMetric::ProviderRequestOverflows, 1});
            automaticRecoveryPending = false;
            failurePendingStop = true;
            operationCallbacksEnabled = false;
            ++callbackEpoch;
            invalidateProvider(std::move(reason));
            cancelActiveOperations("The provider connection exceeded pending server-request capacity.");
            ProviderState provider = state.provider;
            provider.lifecycle = ProviderLifecycle::Failed;
            provider.lastError = Error{Error::Category::Capacity, code, std::move(message)};
            publish(ProviderLifecycleChanged{provider});
            client.stop();
        }

        void cancelActiveOperations(const std::string& reason) {
            std::vector<OperationKey> keys;
            keys.reserve(activeOperations.size());
            for (const auto& [key, operation] : activeOperations) {
                (void) operation;
                keys.push_back(key);
            }
            activeOperations.clear();
            for (const auto& [sessionId, requestId] : keys) {
                complete(sessionId, requestId, CommandResult::failed(CommandErrorCode::Cancelled, reason));
            }
        }

        AppServerClient& client;
        BackendCoreOptions options;
        Reducer reducer;
        BackendState state;
        bool alive = true;
        std::uint64_t callbackEpoch = 0;
        bool operationCallbacksEnabled = false;
        bool restartPending = false;
        std::optional<std::uint64_t> restartStartGeneration;
        bool automaticRecoveryPending = false;
        bool failurePendingStop = false;
        bool providerStopObserved = true;
        std::optional<std::uint64_t> initialRefreshGeneration;
        std::optional<std::uint64_t> invalidatedGeneration;
        std::optional<SequenceNumber> snapshotOmissionsAccountedAtSequence;
        bool accountingSnapshotOmissions = false;
        std::shared_ptr<RecoveryTimerGate> recoveryTimerGate;
        RecoveryTimerCancellation recoveryTimerCancellation;
        std::uint64_t nextSessionId = 1;
        std::uint64_t nextObserverId = 1;
        std::uint64_t nextPendingRequestId = 1;
        std::map<SessionId, std::shared_ptr<SessionRecord>> sessions;
        std::map<std::uint64_t, std::shared_ptr<ObserverRecord>> observers;
        std::map<OperationKey, ActiveOperation> activeOperations;
    };

    FrontendSession::FrontendSession() noexcept = default;

    FrontendSession::FrontendSession(std::shared_ptr<Control> control) noexcept
        : control(std::move(control)) {
    }

    FrontendSession::FrontendSession(FrontendSession&& other) noexcept
        : control(std::move(other.control)) {
    }

    FrontendSession& FrontendSession::operator=(FrontendSession&& other) noexcept {
        if (this != &other) {
            close();
            control = std::move(other.control);
        }
        return *this;
    }

    FrontendSession::~FrontendSession() {
        close();
    }

    SessionId FrontendSession::id() const noexcept {
        return control ? control->sessionId : SessionId{};
    }

    SessionRole FrontendSession::role() const noexcept {
        try {
            return control && control->role ? control->role() : SessionRole::Observer;
        } catch (...) {
            return SessionRole::Observer;
        }
    }

    bool FrontendSession::isOpen() const noexcept {
        try {
            return control && control->open && control->open();
        } catch (...) {
            return false;
        }
    }

    CommandSubmission FrontendSession::submit(std::string requestId, BackendCommand command) {
        if (!control || !control->submit) {
            return {false, SubmissionError::Closed, "The frontend session is closed."};
        }
        try {
            return control->submit(std::move(requestId), std::move(command));
        } catch (...) {
            return {false, SubmissionError::Closed, "The frontend session could not submit the command."};
        }
    }

    bool FrontendSession::requestSnapshot() {
        try {
            return control && control->snapshot && control->snapshot();
        } catch (...) {
            return false;
        }
    }

    void FrontendSession::close(std::string reason) noexcept {
        std::shared_ptr<Control> current = std::move(control);
        if (!current || !current->close) {
            return;
        }
        try {
            current->close(std::move(reason));
        } catch (...) {
        }
    }

    BackendObserverSubscription::BackendObserverSubscription() noexcept = default;

    BackendObserverSubscription::BackendObserverSubscription(std::shared_ptr<Control> control) noexcept
        : control(std::move(control)) {
    }

    BackendObserverSubscription::BackendObserverSubscription(BackendObserverSubscription&& other) noexcept
        : control(std::move(other.control)) {
    }

    BackendObserverSubscription& BackendObserverSubscription::operator=(BackendObserverSubscription&& other) noexcept {
        if (this != &other) {
            close();
            control = std::move(other.control);
        }
        return *this;
    }

    BackendObserverSubscription::~BackendObserverSubscription() {
        close();
    }

    bool BackendObserverSubscription::isOpen() const noexcept {
        try {
            return control && control->open && control->open();
        } catch (...) {
            return false;
        }
    }

    void BackendObserverSubscription::close() noexcept {
        std::shared_ptr<Control> current = std::move(control);
        if (!current || !current->close) {
            return;
        }
        try {
            current->close();
        } catch (...) {
        }
    }

    detail::BackendCoreRuntime::BackendCoreRuntime(AppServerClient& client, BackendCoreOptions options) {
        impl = std::make_shared<Impl>(client, std::move(options));
        impl->initialize();
    }

    detail::BackendCoreRuntime::~BackendCoreRuntime() {
        if (impl) {
            impl->shutdown();
        }
        impl.reset();
    }

    void detail::BackendCoreRuntime::start() {
        impl->start();
    }

    void detail::BackendCoreRuntime::stop() {
        impl->stop();
    }

    void detail::BackendCoreRuntime::restart() {
        impl->restart();
    }

    BackendState detail::BackendCoreRuntime::state() const {
        return impl->copyState();
    }

    Snapshot detail::BackendCoreRuntime::snapshot() const {
        return impl->makeCurrentSnapshot();
    }

    bool detail::BackendCoreRuntime::isReady() const noexcept {
        return impl && impl->ready();
    }

    FrontendSession detail::BackendCoreRuntime::openSession(FrontendSessionCallbacks callbacks) {
        Impl::SessionBinding binding = impl->createSession(std::move(callbacks));
        if (!binding.id) {
            return {};
        }
        auto control = std::make_shared<FrontendSession::Control>();
        control->sessionId = binding.id;
        control->role = std::move(binding.role);
        control->open = std::move(binding.open);
        control->submit = std::move(binding.submit);
        control->snapshot = std::move(binding.snapshot);
        control->close = std::move(binding.close);
        return FrontendSession{std::move(control)};
    }

    BackendObserverSubscription detail::BackendCoreRuntime::subscribe(BackendObserverCallbacks callbacks) {
        Impl::ObserverBinding binding = impl->createObserver(std::move(callbacks));
        if (!binding.open) {
            return {};
        }
        auto control = std::make_shared<BackendObserverSubscription::Control>();
        control->open = std::move(binding.open);
        control->close = std::move(binding.close);
        return BackendObserverSubscription{std::move(control)};
    }

} // namespace ai::openai::codex::backend
