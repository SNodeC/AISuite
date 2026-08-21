/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/internal/server/ServerCore.h"
#include "support/TestResult.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace model = ai::openai::codex::frontend::internal::model;
    namespace server = ai::openai::codex::frontend::internal::server;

    class Backend final : public server::BackendPort {
    public:
        [[nodiscard]] bool providerReady() const noexcept override {
            return true;
        }

        [[nodiscard]] model::CanonicalSnapshot snapshot() const override {
            return state;
        }

        [[nodiscard]] server::BackendSubmitStatus submit(server::BackendInvocation) override {
            return server::BackendSubmitStatus::Accepted;
        }

        [[nodiscard]] bool performProviderLifecycleAction(server::ProviderLifecycleAction) override {
            return true;
        }

        model::CanonicalSnapshot state;
    };

    frontend::AuthenticationResult authenticate(const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential&) {
        frontend::FrontendPrincipal principal;
        principal.id = "backpressure-principal";
        principal.profile = "test";
        principal.scopes = {frontend::FrontendScope::Observe};
        return frontend::AuthenticationSuccess{std::move(principal)};
    }

    model::OccurrenceDraft providerOccurrence(std::string source = "1",
                                              model::ProviderLifecycle lifecycle = model::ProviderLifecycle::Ready) {
        model::ProviderState provider;
        provider.lifecycle = lifecycle;
        return {model::SourceStamp{"backend-event:" + source}, model::ProviderUpdatedOccurrence{std::move(provider)}};
    }

    model::OccurrenceDraft controllerOccurrence(std::string source) {
        return {model::SourceStamp{"backend-event:" + source},
                model::ControllerUpdatedOccurrence{model::ControllerState{}}};
    }

    bool publishResultContractHolds(const server::PublishResult& value) {
        return (!value.accepted || !value.error.has_value()) && (!value.error.has_value() || !value.accepted) &&
               (!value.accepted || value.deliveryMode != server::PublishDeliveryMode::None) &&
               (value.accepted || value.deliveryMode == server::PublishDeliveryMode::None);
    }

    void drainScheduled(std::vector<std::function<void()>>& scheduled) {
        std::size_t turns = 0;
        while (!scheduled.empty()) {
            std::function<void()> callback = std::move(scheduled.front());
            scheduled.erase(scheduled.begin());
            callback();
            if (++turns > 64) {
                throw std::runtime_error("publish-result scheduler did not quiesce");
            }
        }
    }

    void testQueueAndTransportBackpressure(tests::support::TestResult& result) {
        Backend boundedBackend;
        server::ServerCoreOptions boundedOptions;
        boundedOptions.authenticator = authenticate;
        boundedOptions.maxOutboundMessagesPerConnection = 2;
        std::vector<std::function<void()>> boundedScheduled;
        boundedOptions.scheduler = [&boundedScheduled](std::function<void()> callback) {
            boundedScheduled.push_back(std::move(callback));
        };
        server::ServerCore bounded(boundedBackend, std::move(boundedOptions));
        bounded.start();
        std::vector<frontend::ServerMessage> boundedMessages;
        std::vector<server::ConnectionClose> boundedCloses;
        const auto boundedConnection = bounded.openConnection({},
                                                              {[&boundedMessages](server::SerializedServerMessage outbound) {
                                                                   boundedMessages.push_back(std::move(outbound.message));
                                                                   return true;
                                                               },
                                                               [&boundedCloses](const server::ConnectionClose& close) {
                                                                   boundedCloses.push_back(close);
                                                               }});
        const auto boundedHello = bounded.receive(*boundedConnection, frontend::ClientMessage{frontend::Hello{}});
        drainScheduled(boundedScheduled);
        const auto* boundedError = boundedMessages.size() == 2
                                       ? std::get_if<frontend::ProtocolErrorMessage>(&boundedMessages.back())
                                       : nullptr;
        result.expectTrue(boundedHello.status == server::ReceiveStatus::Closing && bounded.connectionCount() == 0 &&
                              boundedMessages.size() == 2 && std::holds_alternative<frontend::Welcome>(boundedMessages.front()) &&
                              boundedError && boundedError->code == frontend::ErrorCode::CapacityExceeded &&
                              boundedError->message == "frontend outbound backpressure limit exceeded" &&
                              boundedError->closeConnection &&
                              boundedCloses.size() == 1 && boundedCloses.front().protocolCode == frontend::ErrorCode::CapacityExceeded &&
                              boundedCloses.front().reason == "frontend outbound backpressure limit exceeded",
                          "a synchronization queue boundary preserves Welcome, reports a typed terminal error, and then closes");

        Backend receiveOverflowBackend;
        std::vector<std::function<void()>> receiveOverflowScheduled;
        server::ServerCoreOptions receiveOverflowOptions;
        receiveOverflowOptions.authenticator = authenticate;
        receiveOverflowOptions.maxOutboundMessagesPerConnection = 8;
        receiveOverflowOptions.scheduler = [&receiveOverflowScheduled](std::function<void()> callback) {
            receiveOverflowScheduled.push_back(std::move(callback));
        };
        server::ServerCore receiveOverflow(receiveOverflowBackend, std::move(receiveOverflowOptions));
        receiveOverflow.start();
        std::vector<frontend::ServerMessage> receiveOverflowMessages;
        std::vector<server::ConnectionClose> receiveOverflowCloses;
        const auto receiveOverflowConnection =
            receiveOverflow.openConnection({},
                                               {[&receiveOverflowMessages](server::SerializedServerMessage outbound) {
                                                    receiveOverflowMessages.push_back(std::move(outbound.message));
                                                    return true;
                                                },
                                                [&receiveOverflowCloses](const server::ConnectionClose& close) {
                                                    receiveOverflowCloses.push_back(close);
                                                }});
        const bool receiveOverflowSynchronized =
            receiveOverflowConnection &&
            receiveOverflow.receive(*receiveOverflowConnection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        while (receiveOverflowConnection && receiveOverflow.queuedMessages(*receiveOverflowConnection) < 8) {
            const server::SnapshotPublishResult published = receiveOverflow.publishSnapshot(receiveOverflowBackend.state);
            if (!published.accepted || published.recipientCount != 1) {
                break;
            }
        }
        const server::ReceiveResult receiveOverflowResult =
            receiveOverflow.receiveError(*receiveOverflowConnection,
                                         frontend::CodecError{frontend::ErrorCode::MalformedJson,
                                                              "malformed frame",
                                                              true,
                                                              {},
                                                              std::nullopt,
                                                              std::nullopt});
        drainScheduled(receiveOverflowScheduled);
        const auto* receiveOverflowError = !receiveOverflowMessages.empty()
                                               ? std::get_if<frontend::ProtocolErrorMessage>(&receiveOverflowMessages.back())
                                               : nullptr;
        result.expectTrue(receiveOverflowSynchronized && receiveOverflowResult.status == server::ReceiveStatus::Closing &&
                              receiveOverflow.connectionCount() == 0 && receiveOverflowError &&
                              receiveOverflowError->code == frontend::ErrorCode::CapacityExceeded &&
                              receiveOverflowError->message == "frontend outbound backpressure limit exceeded" &&
                              receiveOverflowCloses.size() == 1 &&
                              receiveOverflowCloses.front().protocolCode == frontend::ErrorCode::CapacityExceeded,
                          "a receive-side queue overflow reports Closing and drains its replacement terminal error");

        Backend snapshotOverflowBackend;
        std::vector<std::function<void()>> snapshotOverflowScheduled;
        server::ServerCoreOptions snapshotOverflowOptions;
        snapshotOverflowOptions.authenticator = authenticate;
        snapshotOverflowOptions.maxOutboundMessagesPerConnection = 8;
        snapshotOverflowOptions.scheduler = [&snapshotOverflowScheduled](std::function<void()> callback) {
            snapshotOverflowScheduled.push_back(std::move(callback));
        };
        server::ServerCore snapshotOverflow(snapshotOverflowBackend, std::move(snapshotOverflowOptions));
        snapshotOverflow.start();
        std::vector<frontend::ServerMessage> snapshotOverflowMessages;
        std::vector<server::ConnectionClose> snapshotOverflowCloses;
        const auto snapshotOverflowConnection =
            snapshotOverflow.openConnection({},
                                                {[&snapshotOverflowMessages](server::SerializedServerMessage outbound) {
                                                     snapshotOverflowMessages.push_back(std::move(outbound.message));
                                                     return true;
                                                 },
                                                 [&snapshotOverflowCloses](const server::ConnectionClose& close) {
                                                     snapshotOverflowCloses.push_back(close);
                                                 }});
        const bool snapshotOverflowSynchronized =
            snapshotOverflowConnection &&
            snapshotOverflow.receive(*snapshotOverflowConnection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        drainScheduled(snapshotOverflowScheduled);
        snapshotOverflowMessages.clear();
        for (std::size_t index = 0; index < 8; ++index) {
            static_cast<void>(snapshotOverflow.publishSnapshot(snapshotOverflowBackend.state));
        }
        const server::SnapshotPublishResult overflowingSnapshot =
            snapshotOverflow.publishSnapshot(snapshotOverflowBackend.state);
        drainScheduled(snapshotOverflowScheduled);
        const auto* snapshotOverflowError = !snapshotOverflowMessages.empty()
                                                ? std::get_if<frontend::ProtocolErrorMessage>(&snapshotOverflowMessages.back())
                                                : nullptr;
        result.expectTrue(snapshotOverflowSynchronized && overflowingSnapshot.accepted &&
                              overflowingSnapshot.recipientCount == 0 && snapshotOverflow.connectionCount() == 0 &&
                              snapshotOverflowError && snapshotOverflowError->code == frontend::ErrorCode::CapacityExceeded &&
                              snapshotOverflowError->message == "frontend outbound backpressure limit exceeded" &&
                              snapshotOverflowCloses.size() == 1 &&
                              snapshotOverflowCloses.front().protocolCode == frontend::ErrorCode::CapacityExceeded &&
                              snapshotOverflowCloses.front().reason == "frontend outbound backpressure limit exceeded",
                          "a live Snapshot overflow preserves the first capacity failure through wire delivery and close");

        Backend transportBackend;
        std::vector<std::function<void()>> scheduled;
        server::ServerCoreOptions transportOptions;
        transportOptions.authenticator = authenticate;
        transportOptions.scheduler = [&scheduled](std::function<void()> callback) {
            scheduled.push_back(std::move(callback));
        };
        server::ServerCore transport(transportBackend, std::move(transportOptions));
        transport.start();
        std::vector<server::ConnectionClose> transportCloses;
        const auto transportConnection = transport.openConnection({},
                                                                  {[](server::SerializedServerMessage) {
                                                                       return false;
                                                                   },
                                                                   [&transportCloses](const server::ConnectionClose& close) {
                                                                       transportCloses.push_back(close);
                                                                       throw std::runtime_error("callback containment probe");
                                                                   }});
        result.expectTrue(transport.receive(*transportConnection, frontend::ClientMessage{frontend::Hello{}}).accepted() &&
                              !scheduled.empty(),
                          "deferred delivery retains typed outbound messages without invoking transport callbacks inline");
        scheduled.front()();
        result.expectTrue(transport.connectionCount() == 0 && transportCloses.size() == 1 &&
                              !transport.connectionOpen(*transportConnection),
                          "a terminal transport close removes only that connection and contains close-callback exceptions");

        Backend throwingBackend;
        std::vector<std::function<void()>> throwingScheduled;
        server::ServerCoreOptions throwingOptions;
        throwingOptions.authenticator = authenticate;
        throwingOptions.scheduler = [&throwingScheduled](std::function<void()> callback) {
            throwingScheduled.push_back(std::move(callback));
        };
        server::ServerCore throwing(throwingBackend, std::move(throwingOptions));
        throwing.start();
        std::vector<server::ConnectionClose> throwingCloses;
        const auto throwingConnection = throwing.openConnection({},
                                                                {[](server::SerializedServerMessage) -> bool {
                                                                     throw std::runtime_error("send callback probe");
                                                                 },
                                                                 [&throwingCloses](const server::ConnectionClose& close) {
                                                                     throwingCloses.push_back(close);
                                                                 }});
        const bool throwingHello = throwingConnection &&
                                   throwing.receive(*throwingConnection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        throwingScheduled.front()();
        result.expectTrue(throwingHello && throwing.connectionCount() == 0 && throwingCloses.size() == 1 &&
                              throwingCloses.front().reason == "frontend outbound callback threw",
                          "a throwing send callback has the oracle close reason distinct from transport refusal");
    }

    void testLosslessTransportBackpressure(tests::support::TestResult& result) {
        Backend backend;
        std::vector<std::function<void()>> scheduled;
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.maxMessagesPerDelivery = 2;
        options.scheduler = [&scheduled](std::function<void()> callback) {
            scheduled.push_back(std::move(callback));
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        std::vector<std::string> attemptedJson;
        std::vector<std::string> acceptedJson;
        std::vector<server::ConnectionClose> closes;
        bool backpressureOnce = false;
        const auto connection = core.openConnection({},
                                                    {{},
                                                     [&closes](const server::ConnectionClose& close) {
                                                         closes.push_back(close);
                                                     },
                                                     [&](server::SerializedServerMessage& outbound) {
                                                         attemptedJson.push_back(outbound.compactJson);
                                                         if (backpressureOnce) {
                                                             backpressureOnce = false;
                                                             return frontend::OutboundDeliveryStatus::Backpressured;
                                                         }
                                                         acceptedJson.push_back(std::move(outbound.compactJson));
                                                         return frontend::OutboundDeliveryStatus::Accepted;
                                                     }});
        const bool helloAccepted =
            connection && core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        drainScheduled(scheduled);
        attemptedJson.clear();
        acceptedJson.clear();
        backpressureOnce = true;

        model::CanonicalSnapshot firstSnapshot = backend.state;
        firstSnapshot.provider.lifecycle = model::ProviderLifecycle::Ready;
        model::CanonicalSnapshot secondSnapshot = backend.state;
        secondSnapshot.provider.lifecycle = model::ProviderLifecycle::Failed;
        model::CanonicalSnapshot thirdSnapshot = backend.state;
        thirdSnapshot.provider.lifecycle = model::ProviderLifecycle::Starting;
        const server::SnapshotPublishResult firstPublished = core.publishSnapshot(firstSnapshot);
        const server::SnapshotPublishResult secondPublished = core.publishSnapshot(secondSnapshot);
        const server::SnapshotPublishResult thirdPublished = core.publishSnapshot(thirdSnapshot);
        const std::size_t queuedMessagesBefore = connection ? core.queuedMessages(*connection) : 0;
        const std::size_t queuedBytesBefore = connection ? core.queuedBytes(*connection) : 0;

        std::function<void()> firstDelivery;
        if (!scheduled.empty()) {
            firstDelivery = std::move(scheduled.front());
            scheduled.erase(scheduled.begin());
        }
        if (firstDelivery) {
            firstDelivery();
        }

        result.expectTrue(helloAccepted && firstPublished.accepted && firstPublished.recipientCount == 1 &&
                              secondPublished.accepted && secondPublished.recipientCount == 1 &&
                              thirdPublished.accepted && thirdPublished.recipientCount == 1 &&
                              queuedMessagesBefore == 3 && queuedBytesBefore > 0,
                          "backpressure probe starts with three exact serialized Snapshots in a stable outbound queue");
        result.expectTrue(attemptedJson.size() == 1 && acceptedJson.empty(),
                          "the first delivery attempt reports backpressure without acceptance");
        result.expectTrue(connection && core.connectionOpen(*connection) && closes.empty(),
                          "temporary transport backpressure leaves the connection open");
        result.expectTrue(connection && core.queuedMessages(*connection) == queuedMessagesBefore &&
                              core.queuedBytes(*connection) == queuedBytesBefore,
                          "temporary transport backpressure preserves exact queue accounting");

        if (connection) {
            core.flushConnection(*connection);
        }
        const bool retriedInOrder = attemptedJson.size() == 3 && acceptedJson.size() == 2 &&
                                    attemptedJson[0] == attemptedJson[1] && attemptedJson[1] == acceptedJson[0] &&
                                    attemptedJson[2] == acceptedJson[1] && attemptedJson[1] != attemptedJson[2];
        const std::size_t acceptedBytes = acceptedJson.size() == 2
                                              ? acceptedJson[0].size() + acceptedJson[1].size()
                                              : 0;
        result.expectTrue(retriedInOrder, "an explicit resume retries the identical JSON before the following queued message");
        result.expectTrue(connection && core.connectionOpen(*connection) && closes.empty(),
                          "successful resumed delivery keeps the frontend connection open");
        result.expectTrue(connection && core.queuedMessages(*connection) == queuedMessagesBefore - 2 &&
                              core.queuedBytes(*connection) == queuedBytesBefore - acceptedBytes,
                          "successful resumed delivery debits only the two accepted messages");

        drainScheduled(scheduled);
        result.expectTrue(attemptedJson.size() == 4 && acceptedJson.size() == 3 && attemptedJson[3] == acceptedJson[2],
                          "a resumed delivery schedules the remainder after reaching its per-turn fairness bound");
        result.expectTrue(connection && core.queuedMessages(*connection) == 0 && core.queuedBytes(*connection) == 0,
                          "the scheduled remainder drains the retained connection queue completely");

        Backend closedBackend;
        std::vector<std::function<void()>> closedScheduled;
        server::ServerCoreOptions closedOptions;
        closedOptions.authenticator = authenticate;
        closedOptions.scheduler = [&closedScheduled](std::function<void()> callback) {
            closedScheduled.push_back(std::move(callback));
        };
        server::ServerCore closedCore(closedBackend, std::move(closedOptions));
        closedCore.start();
        std::size_t closedSendCalls = 0;
        std::vector<server::ConnectionClose> terminalCloses;
        const auto closedConnection = closedCore.openConnection({},
                                                                {{},
                                                                 [&terminalCloses](const server::ConnectionClose& close) {
                                                                     terminalCloses.push_back(close);
                                                                 },
                                                                 [&closedSendCalls](server::SerializedServerMessage&) {
                                                                     ++closedSendCalls;
                                                                     return frontend::OutboundDeliveryStatus::Closed;
                                                                 }});
        const bool closedHelloAccepted =
            closedConnection && closedCore.receive(*closedConnection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        std::function<void()> closedDelivery;
        if (!closedScheduled.empty()) {
            closedDelivery = std::move(closedScheduled.front());
            closedScheduled.erase(closedScheduled.begin());
        }
        if (closedDelivery) {
            closedDelivery();
        }
        if (closedConnection) {
            closedCore.flushConnection(*closedConnection);
        }
        result.expectTrue(closedHelloAccepted && closedSendCalls == 1 && terminalCloses.size() == 1 &&
                              closedCore.connectionCount() == 0 && !closedCore.connectionOpen(*closedConnection),
                          "a terminal transport close is never retained or retried");
    }

    void testDeliveryFairness(tests::support::TestResult& result) {
        Backend backend;
        std::vector<std::function<void()>> scheduled;
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.maxMessagesPerDelivery = 1;
        options.scheduler = [&scheduled](std::function<void()> callback) {
            scheduled.push_back(std::move(callback));
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        std::size_t firstDeliveries = 0;
        std::size_t secondDeliveries = 0;
        const auto first = core.openConnection({},
                                               {[&firstDeliveries](server::SerializedServerMessage) {
                                                    ++firstDeliveries;
                                                    return true;
                                                },
                                                [](const server::ConnectionClose&) {
                                                }});
        const auto second = core.openConnection({},
                                                {[&secondDeliveries](server::SerializedServerMessage) {
                                                     ++secondDeliveries;
                                                     return true;
                                                 },
                                                 [](const server::ConnectionClose&) {
                                                 }});
        (void) core.receive(*first, frontend::ClientMessage{frontend::Hello{}});
        (void) core.receive(*second, frontend::ClientMessage{frontend::Hello{}});

        std::function<void()> firstDelivery = std::move(scheduled.front());
        scheduled.erase(scheduled.begin());
        firstDelivery();
        result.expectTrue(firstDeliveries == 1 && secondDeliveries == 1 && core.queuedMessages(*first) == 3 &&
                              core.queuedMessages(*second) == 3,
                          "one delivery turn services every connection once while retaining the joined-session event batch");

        std::size_t turns = 0;
        while ((!scheduled.empty() || core.queuedMessages(*first) != 0 || core.queuedMessages(*second) != 0) && turns < 8) {
            if (scheduled.empty()) {
                break;
            }
            std::function<void()> delivery = std::move(scheduled.front());
            scheduled.erase(scheduled.begin());
            delivery();
            ++turns;
        }
        result.expectTrue(firstDeliveries == 4 && secondDeliveries == 4 && core.queuedMessages(*first) == 0 &&
                              core.queuedMessages(*second) == 0,
                          "bounded delivery turns reschedule fairly until both synchronization queues drain");
    }

    void testSerializedOutboundFrames(tests::support::TestResult& result) {
        Backend backend;
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.scheduler = [](std::function<void()> callback) {
            callback();
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        std::vector<server::SerializedServerMessage> frames;
        const auto connection = core.openConnection({}, {[&frames](server::SerializedServerMessage outbound) {
                                                               frames.push_back(std::move(outbound));
                                                               return true;
                                                           },
                                                           [](const server::ConnectionClose&) {
                                                           }});
        const bool synchronized = connection && core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        frames.clear();

        const server::PublishResult first = core.publishGroup(providerOccurrence("501"));
        const server::PublishResult second = core.publishGroup(providerOccurrence("502"));

        const bool exactFrames = std::all_of(frames.begin(), frames.end(), [](const server::SerializedServerMessage& frame) {
            const auto decoded = frontend::Codec::decodeServer(std::string_view(frame.compactJson));
            const auto encoded = frontend::Codec::serializeServer(frame.message);
            return decoded && decoded.value() == frame.message && encoded && encoded.value() == frame.compactJson;
        });
        const std::size_t eventCount = static_cast<std::size_t>(
            std::count_if(frames.begin(), frames.end(), [](const server::SerializedServerMessage& frame) {
                const auto* batch = std::get_if<frontend::EventBatch>(&frame.message);
                return batch && batch->events.size() == 1;
            }));
        result.expectTrue(synchronized && first.accepted && second.accepted && exactFrames && frames.size() == 2 && eventCount == 2 &&
                              core.queuedMessages(*connection) == 0 && core.queuedBytes(*connection) == 0,
                          "compact serializations follow multiple typed event batches through queue accounting and delivery");
    }

    void testCapacityAndSequenceExhaustion(tests::support::TestResult& result) {
        Backend capacityBackend;
        std::vector<std::function<void()>> capacityCallbacks;
        server::ServerCoreOptions capacityOptions;
        capacityOptions.authenticator = authenticate;
        capacityOptions.maxDirtyEntities = 1;
        capacityOptions.scheduler = [&capacityCallbacks](std::function<void()> callback) {
            capacityCallbacks.push_back(std::move(callback));
        };
        server::ServerCore capacity(capacityBackend, std::move(capacityOptions));
        capacity.start();
        server::OccurrenceCoalescingKey providerKey;
        server::OccurrenceCoalescingKey controllerKey;
        controllerKey.kind = server::OccurrenceEntityKind::Controller;
        const server::OccurrenceStageResult first =
            capacity.stageGroup(providerKey, providerOccurrence("301"), server::OccurrenceFlushUrgency::Deferred);
        const server::OccurrenceStageResult replacement =
            capacity.stageGroup(providerKey, providerOccurrence("302"), server::OccurrenceFlushUrgency::Deferred);
        const server::OccurrenceStageResult stageOverflow =
            capacity.stageGroup(controllerKey, providerOccurrence("303"), server::OccurrenceFlushUrgency::Deferred);
        result.expectTrue(first.accepted() && first.scheduleRequired && replacement.accepted() && !replacement.scheduleRequired &&
                              stageOverflow.status == server::OccurrenceStageStatus::SnapshotRequired && !capacityCallbacks.empty(),
                          "maxDirtyEntities counts pending semantic keys, while replacement of one key consumes no new slot");
        std::function<void()> capacityFlush = std::move(capacityCallbacks.front());
        capacityCallbacks.erase(capacityCallbacks.begin());
        capacityFlush();
        result.expectTrue(capacity.currentSequence() == model::FrontendSequence{1},
                          "dirty-key overflow discards the pending semantic suffix and advances one snapshot replay barrier");

        Backend exhaustedBackend;
        server::ServerCoreOptions exhaustedOptions;
        exhaustedOptions.authenticator = authenticate;
        exhaustedOptions.journalInitialSequence =
            model::FrontendSequence{model::FrontendSequence::maximum().value() - 1};
        server::ServerCore exhausted(exhaustedBackend, std::move(exhaustedOptions));
        exhausted.start();
        std::vector<frontend::ServerMessage> messages;
        std::vector<server::ConnectionClose> closes;
        const auto connection = exhausted.openConnection({},
                                                         {[&messages](server::SerializedServerMessage outbound) {
                                                              messages.push_back(std::move(outbound.message));
                                                              return true;
                                                          },
                                                          [&closes](const server::ConnectionClose& close) {
                                                              closes.push_back(close);
                                                          }});
        (void) exhausted.receive(*connection, frontend::ClientMessage{frontend::Hello{}});
        messages.clear();
        const server::PublishResult overflow = exhausted.publishGroup(providerOccurrence("304"));
        const auto* protocolError = messages.size() > 0 ? std::get_if<frontend::ProtocolErrorMessage>(&messages[0]) : nullptr;
        const auto* snapshot = messages.size() > 1 ? std::get_if<frontend::Snapshot>(&messages[1]) : nullptr;
        const bool exhaustionVisible = snapshot && snapshot->state.dump().find("frontendSequenceExhausted") != std::string::npos;
        result.expectTrue(
            publishResultContractHolds(overflow) && !overflow.accepted && overflow.deliveryMode == server::PublishDeliveryMode::None &&
                overflow.error == frontend::ErrorCode::SequenceOverflow && protocolError &&
                protocolError->code == frontend::ErrorCode::SequenceOverflow && !protocolError->closeConnection && exhaustionVisible &&
                exhausted.isOpen() && exhausted.connectionCount() == 1 && closes.empty(),
            "sequence exhaustion is non-closing, emits protocol.error, and falls back to a live Snapshot carrying exhaustion");
    }

    void testPublishResultDeliveryContract(tests::support::TestResult& result) {
        Backend normalBackend;
        server::ServerCoreOptions normalOptions;
        normalOptions.authenticator = authenticate;
        server::ServerCore normal(normalBackend, std::move(normalOptions));
        normal.start();

        const model::FrontendSequence normalBefore = normal.currentSequence();
        const server::PublishResult committed = normal.publishGroup(providerOccurrence("publish-normal"));
        result.expectTrue(publishResultContractHolds(committed) && committed.accepted && !committed.error.has_value() &&
                              committed.deliveryMode == server::PublishDeliveryMode::Occurrences &&
                              committed.sequence == model::FrontendSequence{normalBefore.value() + 1} &&
                              normal.currentSequence() == committed.sequence,
                          "a normal publication commits exactly once and reports occurrence delivery without an error");

        model::OccurrenceDraft invalidDraft = providerOccurrence("publish-invalid");
        invalidDraft.expandedPayloads.clear();
        const model::FrontendSequence invalidBefore = normal.currentSequence();
        const server::PublishResult invalid = normal.publishGroup(std::move(invalidDraft));
        result.expectTrue(publishResultContractHolds(invalid) && !invalid.accepted && invalid.error == frontend::ErrorCode::InvalidField &&
                              invalid.deliveryMode == server::PublishDeliveryMode::None && normal.currentSequence() == invalidBefore,
                          "an invalid pre-commit publication is rejected with no delivery mode and cannot advance the journal");

        normal.close();
        const model::FrontendSequence closedBefore = normal.currentSequence();
        const server::PublishResult closed = normal.publishGroup(providerOccurrence("publish-closed"));
        result.expectTrue(publishResultContractHolds(closed) && !closed.accepted &&
                              closed.error == frontend::ErrorCode::BackendUnavailable &&
                              closed.deliveryMode == server::PublishDeliveryMode::None && normal.currentSequence() == closedBefore,
                          "a closed core rejects before commit with no delivery mode and an unchanged journal");

        Backend fallbackBackend;
        fallbackBackend.state.provider.lifecycle = model::ProviderLifecycle::Ready;
        std::vector<std::function<void()>> scheduled;
        server::ServerCoreOptions fallbackOptions;
        fallbackOptions.authenticator = authenticate;
        fallbackOptions.maxPendingDeliveryGroups = 0;
        fallbackOptions.scheduler = [&scheduled](std::function<void()> callback) {
            scheduled.push_back(std::move(callback));
        };
        server::ServerCore fallback(fallbackBackend, std::move(fallbackOptions));
        fallback.start();

        std::vector<frontend::ServerMessage> messages;
        const auto connection = fallback.openConnection({},
                                                        {[&messages](server::SerializedServerMessage outbound) {
                                                             messages.push_back(std::move(outbound.message));
                                                             return true;
                                                         },
                                                         [](const server::ConnectionClose&) {
                                                         }});
        const bool synchronized = connection && fallback.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        drainScheduled(scheduled);
        messages.clear();

        const model::FrontendSequence fallbackBefore = fallback.currentSequence();
        const server::PublishResult firstFallback = fallback.publishGroup(providerOccurrence("publish-fallback-one"));
        const bool firstCommittedOnce =
            fallback.currentSequence() == model::FrontendSequence{fallbackBefore.value() + 1} &&
            firstFallback.sequence == model::FrontendSequence{fallbackBefore.value() + 1};
        const server::PublishResult pendingFallback = fallback.publishGroup(providerOccurrence("publish-fallback-two"));
        const bool committedTwice = firstCommittedOnce &&
                                    fallback.currentSequence() == model::FrontendSequence{fallbackBefore.value() + 2} &&
                                    pendingFallback.sequence == model::FrontendSequence{fallbackBefore.value() + 2};
        result.expectTrue(synchronized && publishResultContractHolds(firstFallback) && firstFallback.accepted &&
                              !firstFallback.error.has_value() &&
                              firstFallback.deliveryMode == server::PublishDeliveryMode::SnapshotFallback && committedTwice,
                          "a zero pending-group bound keeps the committed group accepted and reports authoritative Snapshot fallback");
        result.expectTrue(publishResultContractHolds(pendingFallback) && pendingFallback.accepted && !pendingFallback.error.has_value() &&
                              pendingFallback.deliveryMode == server::PublishDeliveryMode::SnapshotFallback && committedTwice &&
                              scheduled.size() == 1,
                          "another commit while Snapshot fallback is pending remains accepted and cannot be misclassified as occurrences");

        drainScheduled(scheduled);
        const auto deliveredSnapshot = std::find_if(messages.begin(), messages.end(), [](const frontend::ServerMessage& message) {
            return std::holds_alternative<frontend::Snapshot>(message);
        });
        const bool authoritativeFallback =
            deliveredSnapshot != messages.end() &&
            std::get<frontend::Snapshot>(*deliveredSnapshot).sequence == pendingFallback.sequence.protocolValue() &&
            std::none_of(messages.begin(),
                         messages.end(),
                         [](const frontend::ServerMessage& message) {
                             return std::holds_alternative<frontend::EventBatch>(message);
                         }) &&
            fallback.currentSequence() == pendingFallback.sequence;

        std::vector<frontend::ServerMessage> replayMessages;
        const auto replay = fallback.openConnection({},
                                                    {[&replayMessages](server::SerializedServerMessage outbound) {
                                                         replayMessages.push_back(std::move(outbound.message));
                                                         return true;
                                                     },
                                                     [](const server::ConnectionClose&) {
                                                     }});
        frontend::Hello replayHello;
        replayHello.resumeAfter = pendingFallback.sequence.protocolValue();
        const bool replayAccepted = replay && fallback.receive(*replay, frontend::ClientMessage{std::move(replayHello)}).accepted();
        drainScheduled(scheduled);
        const auto* replayWelcome = !replayMessages.empty() ? std::get_if<frontend::Welcome>(&replayMessages.front()) : nullptr;
        const bool replaySnapshot = std::any_of(replayMessages.begin(), replayMessages.end(), [](const frontend::ServerMessage& message) {
            return std::holds_alternative<frontend::Snapshot>(message);
        });
        result.expectTrue(
            authoritativeFallback && replayAccepted && replayWelcome && replayWelcome->syncMode == frontend::SyncMode::Snapshot &&
                replaySnapshot,
            "global delivery fallback emits the authoritative barrier Snapshot and invalidates replay of the discarded suffix");
    }

    void testMixedSnapshotFallbackAdvancesSequence(tests::support::TestResult& result) {
        Backend backend;
        backend.state.provider.lifecycle = model::ProviderLifecycle::Ready;
        std::vector<std::function<void()>> scheduled;
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.maxDirtyEntities = 1;
        options.maxPendingDeliveryGroups = 0;
        options.scheduler = [&scheduled](std::function<void()> callback) {
            scheduled.push_back(std::move(callback));
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        std::vector<frontend::ServerMessage> messages;
        const auto connection = core.openConnection({},
                                                    {[&messages](server::SerializedServerMessage outbound) {
                                                         messages.push_back(std::move(outbound.message));
                                                         return true;
                                                     },
                                                     [](const server::ConnectionClose&) {
                                                     }});
        const bool synchronized = connection && core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        drainScheduled(scheduled);
        messages.clear();

        const model::FrontendSequence beforeA = core.currentSequence();
        const server::PublishResult committedA = core.publishGroup(providerOccurrence("mixed-a"));
        const model::FrontendSequence sequenceN{beforeA.value() + 1};

        backend.state.provider.lifecycle = model::ProviderLifecycle::Failed;
        server::OccurrenceCoalescingKey providerKey;
        const server::OccurrenceStageResult stagedB =
            core.stageGroup(providerKey,
                            providerOccurrence("mixed-b", model::ProviderLifecycle::Failed),
                            server::OccurrenceFlushUrgency::Deferred);
        server::OccurrenceCoalescingKey controllerKey;
        controllerKey.kind = server::OccurrenceEntityKind::Controller;
        controllerKey.entityId = "controller";
        const server::OccurrenceStageResult dirtyOverflow =
            core.stageGroup(controllerKey, controllerOccurrence("mixed-overflow"), server::OccurrenceFlushUrgency::Deferred);

        result.expectTrue(synchronized && publishResultContractHolds(committedA) && committedA.accepted &&
                              !committedA.error.has_value() &&
                              committedA.deliveryMode == server::PublishDeliveryMode::SnapshotFallback &&
                              committedA.sequence == sequenceN && core.currentSequence() == sequenceN && stagedB.accepted() &&
                              dirtyOverflow.status == server::OccurrenceStageStatus::SnapshotRequired && scheduled.size() == 1,
                          "the mixed fallback fixture commits A exactly once while B remains an unsequenced dirty suffix");

        drainScheduled(scheduled);
        const model::FrontendSequence sequenceNPlusOne{sequenceN.value() + 1};
        const auto deliveredSnapshot = std::find_if(messages.begin(), messages.end(), [](const frontend::ServerMessage& message) {
            return std::holds_alternative<frontend::Snapshot>(message);
        });
        const std::size_t snapshotCount =
            static_cast<std::size_t>(std::count_if(messages.begin(), messages.end(), [](const frontend::ServerMessage& message) {
                return std::holds_alternative<frontend::Snapshot>(message);
            }));
        const bool occurrenceDelivered =
            std::any_of(messages.begin(), messages.end(), [](const frontend::ServerMessage& message) {
                return std::holds_alternative<frontend::EventBatch>(message);
            });
        const bool finalBackendState = deliveredSnapshot != messages.end() &&
                                       std::get<frontend::Snapshot>(*deliveredSnapshot).state.value("lifecycle", "") == "failed";
        result.expectTrue(deliveredSnapshot != messages.end() && snapshotCount == 1 && !occurrenceDelivered && finalBackendState,
                          "the mixed fallback sends one authoritative Snapshot containing B and never duplicates A as an occurrence");
        result.expectTrue(deliveredSnapshot != messages.end() &&
                              std::get<frontend::Snapshot>(*deliveredSnapshot).sequence == sequenceNPlusOne.protocolValue() &&
                              core.currentSequence() == sequenceNPlusOne,
                          "unsequenced semantic loss upgrades committed fallback to one advancing authoritative Snapshot barrier");

        const auto replayProbe = [&core](model::FrontendSequence after, std::vector<frontend::ServerMessage>& received) {
            const auto replayConnection = core.openConnection({},
                                                              {[&received](server::SerializedServerMessage outbound) {
                                                                   received.push_back(std::move(outbound.message));
                                                                   return true;
                                                               },
                                                               [](const server::ConnectionClose&) {
                                                               }});
            frontend::Hello hello;
            hello.resumeAfter = after.protocolValue();
            const bool accepted = replayConnection &&
                                  core.receive(*replayConnection, frontend::ClientMessage{std::move(hello)}).accepted();
            if (replayConnection) {
                core.flushConnection(*replayConnection);
            }
            return accepted;
        };

        std::vector<frontend::ServerMessage> replayAfterN;
        const bool replayAfterNAccepted = replayProbe(sequenceN, replayAfterN);
        const auto replayAfterNWelcome =
            std::find_if(replayAfterN.begin(), replayAfterN.end(), [](const frontend::ServerMessage& message) {
                return std::holds_alternative<frontend::Welcome>(message);
            });
        const auto replayAfterNSnapshot =
            std::find_if(replayAfterN.begin(), replayAfterN.end(), [](const frontend::ServerMessage& message) {
                return std::holds_alternative<frontend::Snapshot>(message);
            });
        result.expectTrue(replayAfterNAccepted && replayAfterNWelcome != replayAfterN.end() &&
                              std::get<frontend::Welcome>(*replayAfterNWelcome).syncMode == frontend::SyncMode::Snapshot &&
                              replayAfterNSnapshot != replayAfterN.end() &&
                              std::get<frontend::Snapshot>(*replayAfterNSnapshot).sequence == sequenceNPlusOne.protocolValue(),
                          "replay after N observes a Gap and selects Snapshot synchronization");

        std::vector<frontend::ServerMessage> replayAfterNPlusOne;
        const bool replayAfterNPlusOneAccepted = replayProbe(sequenceNPlusOne, replayAfterNPlusOne);
        const auto replayAfterNPlusOneWelcome =
            std::find_if(replayAfterNPlusOne.begin(), replayAfterNPlusOne.end(), [](const frontend::ServerMessage& message) {
                return std::holds_alternative<frontend::Welcome>(message);
            });
        const auto replayAfterNPlusOneComplete =
            std::find_if(replayAfterNPlusOne.begin(), replayAfterNPlusOne.end(), [](const frontend::ServerMessage& message) {
                return std::holds_alternative<frontend::SyncComplete>(message);
            });
        const bool replayAfterNPlusOneIsEmpty =
            std::none_of(replayAfterNPlusOne.begin(), replayAfterNPlusOne.end(), [](const frontend::ServerMessage& message) {
                return std::holds_alternative<frontend::Snapshot>(message) || std::holds_alternative<frontend::EventBatch>(message);
            });
        result.expectTrue(replayAfterNPlusOneAccepted && replayAfterNPlusOneWelcome != replayAfterNPlusOne.end() &&
                              std::get<frontend::Welcome>(*replayAfterNPlusOneWelcome).syncMode == frontend::SyncMode::Replay &&
                              replayAfterNPlusOneComplete != replayAfterNPlusOne.end() &&
                              std::get<frontend::SyncComplete>(*replayAfterNPlusOneComplete).sequence ==
                                  sequenceNPlusOne.protocolValue() &&
                              replayAfterNPlusOneIsEmpty,
                          "replay after N + 1 is Available and empty after the single advancing barrier");
    }

    void testNestedFlushCoalescing(tests::support::TestResult& result) {
        Backend backend;
        server::ServerCoreOptions options;
        options.authenticator = authenticate;
        options.scheduler = [](std::function<void()> callback) {
            callback();
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        std::vector<frontend::ServerMessage> messages;
        std::size_t callbackDepth = 0;
        std::size_t maximumCallbackDepth = 0;
        std::size_t closeCount = 0;
        bool triggerNested = false;
        bool nestedTriggered = false;
        std::optional<server::PublishResult> nestedResult;
        const auto connection = core.openConnection({}, {[&](server::SerializedServerMessage outbound) {
                                                               ++callbackDepth;
                                                               maximumCallbackDepth = std::max(maximumCallbackDepth, callbackDepth);
                                                               const frontend::ServerMessage& message = outbound.message;
                                                               messages.push_back(message);
                                                               if (triggerNested && !nestedTriggered &&
                                                                   std::holds_alternative<frontend::EventBatch>(message)) {
                                                                   nestedTriggered = true;
                                                                   nestedResult = core.publishGroup(providerOccurrence("402"));
                                                               }
                                                               --callbackDepth;
                                                               return true;
                                                           },
                                                           [&](const server::ConnectionClose&) {
                                                               ++closeCount;
                                                           }});
        const bool synchronized = connection && core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        messages.clear();
        maximumCallbackDepth = 0;
        triggerNested = true;
        const server::PublishResult outer = core.publishGroup(providerOccurrence("401"));

        std::vector<frontend::SequenceNumber> deliveredSequences;
        for (const frontend::ServerMessage& message : messages) {
            if (const auto* batch = std::get_if<frontend::EventBatch>(&message); batch && batch->events.size() == 1) {
                deliveredSequences.push_back(batch->events.front().sequence);
            }
        }
        result.expectTrue(synchronized && outer.accepted && nestedTriggered && nestedResult && nestedResult->accepted &&
                              maximumCallbackDepth == 1 && callbackDepth == 0 && deliveredSequences.size() == 2 &&
                              deliveredSequences[0] == outer.sequence.protocolValue() &&
                              deliveredSequences[1] == nestedResult->sequence.protocolValue() && closeCount == 0 &&
                              core.connectionOpen(*connection),
                          "a publish requested by Send is coalesced after the active flush without recursively invoking Send");
    }

    frontend::Hello largeSnapshotHello() {
        frontend::Hello hello;
        hello.capabilities = {frontend::FrontendCapability::CompleteThreadItems};
        hello.extensions = {{"projection", {{"itemContentUpdateModes", {"append-v2", "append-v1"}}}}};
        return hello;
    }

    void testDerivedLargeSnapshotMessageBoundary(tests::support::TestResult& result) {
        constexpr std::size_t FormerAggregateCeiling = 26U * 1024U * 1024U;
        constexpr std::size_t ItemCount = 1'700;
        constexpr std::size_t ItemTextBytes = 16U * 1024U;

        Backend backend;
        backend.state.items.reserve(ItemCount);
        backend.state.itemsPresent = true;
        const std::string content(ItemTextBytes, 's');
        for (std::size_t index = 0; index < ItemCount; ++index) {
            model::ItemData item{model::ItemIdentity{"large-snapshot-item-" + std::to_string(index)}};
            item.agentText = content;
            backend.state.items.push_back(model::AgentMessageItem{std::move(item)});
        }

        const auto measureSnapshot = [&](std::size_t individualLimit,
                                         std::vector<std::size_t>& snapshotSizes,
                                         std::vector<server::ConnectionClose>& closes) {
            std::vector<std::function<void()>> scheduled;
            server::ServerCoreOptions options;
            options.authenticator = authenticate;
            options.maxOutboundMessageBytes = individualLimit;
            options.scheduler = [&scheduled](std::function<void()> callback) {
                scheduled.push_back(std::move(callback));
            };
            server::ServerCore core(backend, std::move(options));
            core.start();
            const auto connection = core.openConnection(
                {},
                {{},
                 [&closes](const server::ConnectionClose& close) {
                     closes.push_back(close);
                 },
                 [&snapshotSizes](server::SerializedServerMessage& outbound) {
                     if (std::holds_alternative<frontend::Snapshot>(outbound.message)) {
                         snapshotSizes.push_back(outbound.compactJson.size());
                     }
                     return frontend::OutboundDeliveryStatus::Accepted;
                 }});
            const server::ReceiveResult received =
                connection ? core.receive(*connection, frontend::ClientMessage{largeSnapshotHello()})
                           : server::ReceiveResult{};
            drainScheduled(scheduled);
            return received;
        };

        std::vector<std::size_t> measuredSizes;
        std::vector<server::ConnectionClose> measuredCloses;
        const server::ReceiveResult measured =
            measureSnapshot(frontend::DefaultFrontendMaximumServerMessageBytes, measuredSizes, measuredCloses);
        const std::size_t exactBytes = measuredSizes.empty() ? 0 : measuredSizes.front();
        const auto onlyCleanShutdown = [](const std::vector<server::ConnectionClose>& closes) {
            return std::all_of(closes.begin(), closes.end(), [](const server::ConnectionClose& close) {
                return close.clean && !close.protocolCode.has_value();
            });
        };
        result.expectTrue(measured.accepted() && onlyCleanShutdown(measuredCloses) && measuredSizes.size() == 1 &&
                              exactBytes > FormerAggregateCeiling &&
                              exactBytes <= frontend::DefaultFrontendMaximumProjectedSnapshotBytes,
                          "the derived default carries one unchunked projected Snapshot beyond the former ~26 MiB ceiling");

        std::vector<std::size_t> exactSizes;
        std::vector<server::ConnectionClose> exactCloses;
        const server::ReceiveResult atLimit = measureSnapshot(exactBytes, exactSizes, exactCloses);
        result.expectTrue(exactBytes != 0 && atLimit.accepted() && onlyCleanShutdown(exactCloses) && exactSizes.size() == 1 &&
                              exactSizes.front() == exactBytes,
                          "an individual projected Snapshot exactly at the configured message limit is accepted");

        std::vector<std::size_t> rejectedSizes;
        std::vector<server::ConnectionClose> rejectedCloses;
        const server::ReceiveResult aboveLimit = measureSnapshot(exactBytes - 1, rejectedSizes, rejectedCloses);
        result.expectTrue(exactBytes != 0 && aboveLimit.status == server::ReceiveStatus::Closing && rejectedSizes.empty() &&
                              rejectedCloses.size() == 1 &&
                              rejectedCloses.front().protocolCode == frontend::ErrorCode::CapacityExceeded,
                          "an individual projected Snapshot one byte above the configured message limit fails deterministically");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testQueueAndTransportBackpressure(result);
    testLosslessTransportBackpressure(result);
    testDeliveryFairness(result);
    testSerializedOutboundFrames(result);
    testCapacityAndSequenceExhaustion(result);
    testPublishResultDeliveryContract(result);
    testMixedSnapshotFallbackAdvancesSequence(result);
    testNestedFlushCoalescing(result);
    testDerivedLargeSnapshotMessageBoundary(result);
    return result.processResult();
}
