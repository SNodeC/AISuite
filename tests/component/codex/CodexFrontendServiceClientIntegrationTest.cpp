/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/FrontendService.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "ai/openai/codex/frontend/client/Controller.h"
#include "ai/openai/codex/frontend/client/Threads.h"
#include "ai/openai/codex/frontend/client/Turns.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace client = ai::openai::codex::frontend::client;
    namespace frontend = ai::openai::codex::frontend;
    namespace typed = ai::openai::codex::typed;

    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;
    using Json = ai::openai::codex::Json;
    using TransportCallbacks = ai::openai::codex::detail::TransportCallbacks;

    constexpr std::uint64_t TrustedTestUserId = 4242;
    constexpr std::size_t RetainedThreadCount = 35;
    constexpr std::size_t PageThreadCount = 25;
    constexpr std::size_t LargePreviewBytes = 32U * 1024U;
    constexpr std::size_t ProjectedPreviewBytes = 16U * 1024U;
    constexpr std::string_view LifecycleThreadId = "turn-lifecycle-thread";
    constexpr std::string_view LifecycleTurnId = "turn-lifecycle-turn";
    constexpr std::string_view LifecycleUserItemId = "turn-lifecycle-user-item";
    constexpr std::string_view LifecycleAgentItemId = "turn-lifecycle-agent-item";
    constexpr std::string_view LifecycleUserText = "Just answer with OK";
    constexpr std::string_view LifecycleAgentText = "OK";

    std::string threadTitle(std::string_view generation, std::size_t index) {
        return std::string(generation) + " integration thread " + std::to_string(index);
    }

    std::string threadPreview(std::string_view generation, std::size_t index, bool large) {
        if (large) {
            return std::string(generation) + ":" +
                   std::string(LargePreviewBytes - generation.size() - 1, static_cast<char>('a' + index % 26));
        }
        return std::string(generation) + " integration preview " + std::to_string(index);
    }

    std::string projectedThreadPreview(std::string_view generation, std::size_t index, bool large) {
        std::string preview = threadPreview(generation, index, large);
        preview.resize(std::min(preview.size(), ProjectedPreviewBytes));
        return preview;
    }

    Json thread(std::size_t index, bool large, std::string_view generation) {
        Json value = tests::codex::threadValue("live-thread-" + std::to_string(index));
        value["name"] = threadTitle(generation, index);
        value["preview"] = threadPreview(generation, index, large);
        value["updatedAt"] = static_cast<std::int64_t>(1000 + index);
        return value;
    }

    Json page(std::size_t count, bool large, std::string_view generation, std::optional<std::string> nextCursor) {
        Json data = Json::array();
        for (std::size_t index = 0; index < count; ++index) {
            data.push_back(thread(index, large, generation));
        }
        Json result{{"data", std::move(data)}, {"backwardsCursor", "page-backwards"}};
        result["nextCursor"] = nextCursor ? Json(*nextCursor) : Json(nullptr);
        return result;
    }

    frontend::FrontendPeerContext trustedPeer() {
        frontend::FrontendPeerContext peer;
        peer.transport = frontend::FrontendTransportKind::Unix;
        peer.loopback = true;
        peer.localPeer = true;
        peer.unixUserId = TrustedTestUserId;
        return peer;
    }

    Json lifecycleUserItem() {
        return {
            {"type", "userMessage"},
            {"id", LifecycleUserItemId},
            {"clientId", nullptr},
            {"content", Json::array({Json{{"type", "text"}, {"text", LifecycleUserText}, {"text_elements", Json::array()}}})},
        };
    }

    Json lifecycleAgentItem() {
        return {{"type", "agentMessage"}, {"id", LifecycleAgentItemId}, {"text", ""}};
    }

    enum class DeliveryOrder { Wire, StateBeforeResponse };

    class CrossLayerHarness {
    public:
        CrossLayerHarness(tests::support::TestResult& result, bool largeThreads, bool forceSnapshotFallback, bool turnLifecycle = false)
            : result(result)
            , largeThreads(largeThreads)
            , forceSnapshotFallback(forceSnapshotFallback)
            , turnLifecycle(turnLifecycle)
            , transport(std::make_shared<tests::codex::FakeTransportState>()) {
            tests::codex::installInitializingFake(transport, [this](const Json& message, const TransportCallbacks& callbacks) {
                const auto method = message.find("method");
                const auto id = message.find("id");
                if (method == message.end() || !method->is_string() || id == message.end()) {
                    return;
                }
                if (*method == "thread/list") {
                    ++providerListCalls;
                    if (providerListCalls == 1) {
                        tests::codex::inject(
                            callbacks,
                            Json{{"id", *id}, {"result", page(RetainedThreadCount, this->largeThreads, "Initial", "initial-next")}});
                    } else if (providerListCalls == 2) {
                        tests::codex::inject(callbacks,
                                             Json{{"id", *id}, {"result", page(PageThreadCount, this->largeThreads, "Page", "page-next")}});
                    } else {
                        tests::codex::inject(callbacks, Json{{"id", *id}, {"result", page(3, false, "Second", std::nullopt)}});
                    }
                    return;
                }
                if (!this->turnLifecycle) {
                    return;
                }
                if (*method == "thread/start") {
                    ++providerThreadStartCalls;
                    Json startedThread = tests::codex::threadValue(std::string(LifecycleThreadId));
                    tests::codex::inject(
                        callbacks, Json{{"id", *id}, {"result", tests::codex::threadOperationResult(std::string(LifecycleThreadId))}});
                    ++lifecycleNotificationsInjected;
                    tests::codex::inject(callbacks, Json{{"method", "thread/started"}, {"params", {{"thread", std::move(startedThread)}}}});
                    return;
                }
                if (*method == "turn/start") {
                    ++providerTurnStartCalls;
                    const Json parameters = message.value("params", Json::object());
                    const Json input = parameters.value("input", Json::array());
                    exactTurnStartRequest = parameters.value("threadId", "") == LifecycleThreadId && input.is_array() && !input.empty() &&
                                            input[0].value("type", "") == "text" && input[0].value("text", "") == LifecycleUserText;
                    tests::codex::inject(
                        callbacks,
                        Json{{"id", *id},
                             {"result", tests::codex::turnOperationResult(std::string(LifecycleThreadId), std::string(LifecycleTurnId))}});
                    lifecycleNotifications.push_back(
                        Json{{"method", "turn/started"},
                             {"params",
                              {{"threadId", LifecycleThreadId},
                               {"turn", tests::codex::turnValue(std::string(LifecycleThreadId), std::string(LifecycleTurnId))}}}});
                    lifecycleNotifications.push_back(Json{{"method", "item/started"},
                                                          {"params",
                                                           {{"threadId", LifecycleThreadId},
                                                            {"turnId", LifecycleTurnId},
                                                            {"item", lifecycleUserItem()},
                                                            {"startedAtMs", 10}}}});
                    lifecycleNotifications.push_back(Json{{"method", "item/completed"},
                                                          {"params",
                                                           {{"threadId", LifecycleThreadId},
                                                            {"turnId", LifecycleTurnId},
                                                            {"item", lifecycleUserItem()},
                                                            {"completedAtMs", 20}}}});
                    lifecycleNotifications.push_back(Json{{"method", "item/started"},
                                                          {"params",
                                                           {{"threadId", LifecycleThreadId},
                                                            {"turnId", LifecycleTurnId},
                                                            {"item", lifecycleAgentItem()},
                                                            {"startedAtMs", 30}}}});
                    lifecycleNotifications.push_back(Json{{"method", "item/agentMessage/delta"},
                                                          {"params",
                                                           {{"threadId", LifecycleThreadId},
                                                            {"turnId", LifecycleTurnId},
                                                            {"itemId", LifecycleAgentItemId},
                                                            {"delta", LifecycleAgentText}}}});
                    lifecycleNotifications.push_back(Json{{"method", "item/completed"},
                                                          {"params",
                                                           {{"threadId", LifecycleThreadId},
                                                            {"turnId", LifecycleTurnId},
                                                            {"item", lifecycleAgentItem()},
                                                            {"completedAtMs", 40}}}});
                    lifecycleNotifications.push_back(Json{
                        {"method", "turn/completed"},
                        {"params",
                         {{"threadId", LifecycleThreadId},
                          {"turn", tests::codex::turnValue(std::string(LifecycleThreadId), std::string(LifecycleTurnId), "completed")}}}});
                    scheduleLifecycleNotification(callbacks);
                }
            });

            backend::BackendCoreOptions backendOptions;
            backendOptions.initialThreadListLimit = RetainedThreadCount;
            core = std::make_unique<FakeBackendCore>(std::move(backendOptions), transport);
            core->start();
        }

        void scheduleLifecycleNotification(TransportCallbacks callbacks) {
            if (lifecycleNotificationScheduled || lifecycleNotifications.empty()) {
                return;
            }
            lifecycleNotificationScheduled = true;
            core::EventReceiver::atNextTick([this, callbacks = std::move(callbacks)]() mutable {
                lifecycleNotificationScheduled = false;
                if (lifecycleNotifications.empty()) {
                    return;
                }
                Json notification = std::move(lifecycleNotifications.front());
                lifecycleNotifications.pop_front();
                ++lifecycleNotificationsInjected;
                tests::codex::inject(callbacks, notification);

                // Leave deterministic event-loop turns for BackendCore to
                // reduce this typed notification and FrontendService to flush
                // its projected event before the next provider occurrence.
                core::EventReceiver::atNextTick([this, callbacks = std::move(callbacks)]() mutable {
                    core::EventReceiver::atNextTick([this, callbacks = std::move(callbacks)]() mutable {
                        scheduleLifecycleNotification(std::move(callbacks));
                    });
                });
            });
        }

        void attach() {
            frontend::FrontendServiceOptions serviceOptions;
            serviceOptions.trustedLocalUserId = TrustedTestUserId;
            if (forceSnapshotFallback) {
                serviceOptions.batches = {64, 1};
            }
            service = std::make_unique<frontend::FrontendService>(*core, std::move(serviceOptions));

            frontendConnection.emplace(service->openConnection(trustedPeer(),
                                                               {[this](const frontend::OutboundMessage& message) {
                                                                    serverOutbound.push_back(message);
                                                                    return true;
                                                                },
                                                                [this](const std::string& reason) {
                                                                    serviceCloseReasons.push_back(reason);
                                                                }}));

            client::ClientOptions sdkOptions;
            sdkOptions.credentialProvider = [] {
                return client::AuthenticationContext{frontend::NoCredential{}, "verified-local:4242"};
            };
            client::ClientCallbacks callbacks;
            callbacks.onConnectionStateChanged = [this](const client::ConnectionStateChange& change) {
                connectionStates.push_back(change);
            };
            callbacks.onStateUpdated = [this](const client::StateUpdate& update) {
                stateUpdates.push_back(update);
            };
            callbacks.onSynchronized = [this](const client::SynchronizationInfo& info) {
                synchronized.push_back(info);
            };
            callbacks.onCursorAdvanced = [this](frontend::SequenceNumber sequence) {
                cursors.push_back(sequence);
            };
            callbacks.onProtocolMessage = [this](const frontend::ServerMessage& message) {
                protocolMessages.push_back(message);
            };
            callbacks.onDiagnostic = [this](const client::Diagnostic& diagnostic) {
                sdkDiagnostics.push_back(diagnostic);
                if (diagnostic.error && diagnostic.error->origin == client::ErrorOrigin::Protocol) {
                    ++sdkProtocolErrors;
                }
            };
            sdk = std::make_unique<client::Client>(std::move(sdkOptions), std::move(callbacks));
            sdkConnection.emplace(sdk->openConnection({[this](client::OutboundMessage message) {
                                                           clientOutbound.push_back(std::move(message));
                                                           return client::SendResult{client::SendStatus::Accepted, std::nullopt};
                                                       },
                                                       [this](std::string reason) {
                                                           sdkCloseReasons.push_back(reason);
                                                       }}));
            sdkConnection->transportConnected();
            driveSources();
        }

        ~CrossLayerHarness() {
            if (sdk) {
                sdk->close("cross-layer integration complete");
            }
            if (frontendConnection) {
                frontendConnection->close("cross-layer integration complete");
            }
            if (replayConnection) {
                replayConnection->close("cross-layer replay complete");
            }
            if (service) {
                service->close("cross-layer integration complete");
            }
            if (core) {
                core->stop();
            }
        }

        void driveSources() {
            while (!clientOutbound.empty()) {
                client::OutboundMessage outbound = std::move(clientOutbound.front());
                clientOutbound.pop_front();
                const auto decoded = frontend::Codec::decodeClient(std::string_view(outbound.compactJson));
                result.expectTrue(decoded.hasValue(), "the SDK emits a decodable Frontend Protocol object");
                if (decoded && frontendConnection) {
                    result.expectTrue(frontendConnection->receive(decoded.value()).accepted(),
                                      "FrontendService accepts the SDK's queued in-memory message");
                }
            }
            if (service) {
                service->flush();
            }
        }

        void deliverAll(DeliveryOrder order) {
            std::size_t iterations = 0;
            while (!serverOutbound.empty()) {
                auto selected = serverOutbound.begin();
                if (order == DeliveryOrder::StateBeforeResponse) {
                    const auto stateMessage = std::find_if(serverOutbound.begin(), serverOutbound.end(), [](const auto& outbound) {
                        return std::holds_alternative<frontend::Snapshot>(outbound.message) ||
                               std::holds_alternative<frontend::EventBatch>(outbound.message);
                    });
                    const auto responseMessage = std::find_if(serverOutbound.begin(), serverOutbound.end(), [](const auto& outbound) {
                        return std::holds_alternative<frontend::Response>(outbound.message);
                    });
                    if (stateMessage != serverOutbound.end() && responseMessage != serverOutbound.end()) {
                        selected = stateMessage;
                    }
                }
                frontend::OutboundMessage outbound = std::move(*selected);
                serverOutbound.erase(selected);
                ++serializedMessagesDelivered;
                std::string messageSummary = "delivery " + std::to_string(serializedMessagesDelivered);
                if (const auto* batch = std::get_if<frontend::EventBatch>(&outbound.message)) {
                    messageSummary += " event batch";
                    for (const frontend::FrontendEvent& event : batch->events) {
                        messageSummary += " " + event.type;
                        if (const auto item = event.data.find("item"); item != event.data.end() && item->is_object()) {
                            messageSummary += "[" + item->value("id", "") + ":" + item->value("type", "") + "]";
                        }
                    }
                }
                const auto decoded = frontend::Codec::decodeServer(std::string_view(outbound.compactJson));
                if (!decoded) {
                    ++serializedMessageDecodeFailures;
                } else {
                    if (decoded.value() != outbound.message) {
                        ++serializedMessageRoundTripMismatches;
                    }
                    observeExpandedEvents(decoded.value(), outbound.compactJson);
                }
                const client::ReceiveResult received = sdkConnection->receive(std::string_view(outbound.compactJson));
                std::string receiveFailure = received.error ? ": " + received.error->message : std::string{": no transport error detail"};
                if (!received.accepted && !sdkDiagnostics.empty()) {
                    receiveFailure += "; diagnostic: " + sdkDiagnostics.back().message;
                    if (sdkDiagnostics.back().error) {
                        receiveFailure += "; detail: " + sdkDiagnostics.back().error->message;
                    }
                }
                result.expectTrue(received.accepted,
                                  "the C++ SDK accepts each schema-valid serialized server message (" + messageSummary + ")" +
                                      (received.accepted ? std::string{} : receiveFailure));
                if (++iterations > 100'000) {
                    result.expectTrue(false, "cross-layer server delivery remains finitely bounded");
                    return;
                }
            }
        }

        struct WireItemObservation {
            std::string id;
            std::string type;
            std::string threadId;
            std::string turnId;
            std::string status;
            Json item;
            std::string compactMessage;
        };

        struct WireContentObservation {
            std::string itemId;
            std::string threadId;
            std::string turnId;
            std::string channel;
            std::string content;
        };

        void observeExpandedEvents(const frontend::ServerMessage& message, const std::string& compactMessage) {
            const auto* batch = std::get_if<frontend::EventBatch>(&message);
            if (batch == nullptr) {
                return;
            }
            for (const frontend::FrontendEvent& event : batch->events) {
                if (!frontend::expandedEventTypeFromString(event.type).has_value()) {
                    continue;
                }
                ++expandedEventsEmitted;
                bool valid = false;
                if (const auto encoded = frontend::Codec::encodeEvent(event); encoded) {
                    valid = frontend::Codec::decodeExpandedEvent(encoded.value()).hasValue();
                }
                if (valid) {
                    ++expandedEventsSchemaValid;
                } else {
                    ++invalidExpandedEvents;
                }

                if (event.type == frontend::toString(frontend::ExpandedEventType::ItemUpserted)) {
                    const auto item = event.data.find("item");
                    if (item == event.data.end() || !item->is_object()) {
                        continue;
                    }
                    const auto stringMember = [&item](const char* name) {
                        const auto member = item->find(name);
                        return member != item->end() && member->is_string() ? member->get<std::string>() : std::string{};
                    };
                    wireItems.push_back({stringMember("id"),
                                         stringMember("type"),
                                         stringMember("threadId"),
                                         stringMember("turnId"),
                                         stringMember("status"),
                                         *item,
                                         compactMessage});
                } else if (event.type == frontend::toString(frontend::ExpandedEventType::ItemContentUpdated)) {
                    const auto stringMember = [&event](const char* name) {
                        const auto member = event.data.find(name);
                        return member != event.data.end() && member->is_string() ? member->get<std::string>() : std::string{};
                    };
                    wireContent.push_back({stringMember("itemId"),
                                           stringMember("threadId"),
                                           stringMember("turnId"),
                                           stringMember("channel"),
                                           stringMember("content")});
                }
            }
        }

        [[nodiscard]] const WireItemObservation* wireItemSince(std::size_t begin, std::string_view id) const noexcept {
            for (std::size_t index = wireItems.size(); index > begin; --index) {
                if (wireItems[index - 1].id == id) {
                    return &wireItems[index - 1];
                }
            }
            return nullptr;
        }

        [[nodiscard]] const WireContentObservation* wireContentSince(std::size_t begin, std::string_view itemId) const noexcept {
            for (std::size_t index = wireContent.size(); index > begin; --index) {
                if (wireContent[index - 1].itemId == itemId) {
                    return &wireContent[index - 1];
                }
            }
            return nullptr;
        }

        void beginReplay(frontend::SequenceNumber after) {
            replayConnection.emplace(
                service->openConnection(trustedPeer(),
                                        {[this](const frontend::OutboundMessage& outbound) {
                                             replayCompactMessages.push_back(outbound.compactJson);
                                             const auto decoded = frontend::Codec::decodeServer(std::string_view{outbound.compactJson});
                                             if (!decoded) {
                                                 ++replayDecodeFailures;
                                                 return true;
                                             }
                                             if (const auto* welcome = std::get_if<frontend::Welcome>(&decoded.value())) {
                                                 replayWelcome = welcome->syncMode == frontend::SyncMode::Replay;
                                             } else if (const auto* batch = std::get_if<frontend::EventBatch>(&decoded.value())) {
                                                 for (const frontend::FrontendEvent& event : batch->events) {
                                                     if (frontend::expandedEventTypeFromString(event.type).has_value()) {
                                                         ++replayExpandedEvents;
                                                         const auto encoded = frontend::Codec::encodeEvent(event);
                                                         if (encoded && frontend::Codec::decodeExpandedEvent(encoded.value())) {
                                                             ++replayValidExpandedEvents;
                                                         }
                                                     }
                                                     replayEvents.push_back(event);
                                                 }
                                             } else if (std::holds_alternative<frontend::SyncComplete>(decoded.value())) {
                                                 replayComplete = true;
                                             }
                                             return true;
                                         },
                                         [this](const std::string& reason) {
                                             replayCloseReasons.push_back(reason);
                                         }}));
            const std::vector capabilities{
                frontend::FrontendCapability::DedicatedNotificationEvents,
                frontend::FrontendCapability::CompleteThreadItems,
                frontend::FrontendCapability::ScopeProjectedState,
            };
            const frontend::ConnectionReceiveResult received = replayConnection->receive(
                frontend::ClientMessage{frontend::Hello{after, frontend::Json::object(), capabilities, std::nullopt}});
            result.expectTrue(received.accepted(), "a second frontend connection requests exact journal replay after the pre-turn cursor");
        }

        [[nodiscard]] const frontend::FrontendEvent* replayItem(std::string_view itemId) const noexcept {
            for (auto iterator = replayEvents.rbegin(); iterator != replayEvents.rend(); ++iterator) {
                if (iterator->type == frontend::toString(frontend::ExpandedEventType::ItemUpserted) && iterator->data.contains("item") &&
                    iterator->data.at("item").value("id", "") == itemId) {
                    return &*iterator;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const frontend::FrontendEvent* replayContent(std::string_view itemId) const noexcept {
            for (auto iterator = replayEvents.rbegin(); iterator != replayEvents.rend(); ++iterator) {
                if (iterator->type == frontend::toString(frontend::ExpandedEventType::ItemContentUpdated) &&
                    iterator->data.value("itemId", "") == itemId) {
                    return &*iterator;
                }
            }
            return nullptr;
        }

        void settle(DeliveryOrder order = DeliveryOrder::Wire) {
            driveSources();
            deliverAll(order);
            driveSources();
            deliverAll(order);
        }

        [[nodiscard]] bool queuedResponse() const {
            return std::any_of(serverOutbound.begin(), serverOutbound.end(), [](const auto& outbound) {
                return std::holds_alternative<frontend::Response>(outbound.message);
            });
        }

        [[nodiscard]] bool queuedSnapshot() const {
            return std::any_of(serverOutbound.begin(), serverOutbound.end(), [](const auto& outbound) {
                return std::holds_alternative<frontend::Snapshot>(outbound.message);
            });
        }

        [[nodiscard]] std::size_t queuedExpandedEvents(frontend::ExpandedEventType type) const {
            std::size_t count = 0;
            for (const auto& outbound : serverOutbound) {
                if (const auto* batch = std::get_if<frontend::EventBatch>(&outbound.message)) {
                    count += static_cast<std::size_t>(std::count_if(batch->events.begin(), batch->events.end(), [type](const auto& event) {
                        return event.type == frontend::toString(type);
                    }));
                }
            }
            return count;
        }

        [[nodiscard]] bool queuesEmpty() const noexcept {
            return clientOutbound.empty() && serverOutbound.empty();
        }

        [[nodiscard]] std::size_t liveSnapshotsSince(std::size_t begin) const {
            std::size_t count = 0;
            for (std::size_t index = begin; index < protocolMessages.size(); ++index) {
                if (std::holds_alternative<frontend::Snapshot>(protocolMessages[index])) {
                    ++count;
                }
            }
            return count;
        }

        [[nodiscard]] std::size_t expandedEventsSince(std::size_t begin, frontend::ExpandedEventType type) const {
            std::size_t count = 0;
            for (std::size_t index = begin; index < protocolMessages.size(); ++index) {
                if (const auto* batch = std::get_if<frontend::EventBatch>(&protocolMessages[index])) {
                    count += static_cast<std::size_t>(std::count_if(batch->events.begin(), batch->events.end(), [type](const auto& event) {
                        return event.type == frontend::toString(type);
                    }));
                }
            }
            return count;
        }

        struct ProjectedThread {
            std::string title;
            std::string preview;
        };

        [[nodiscard]] std::pair<std::map<std::string, ProjectedThread>, std::size_t> projectedThreadsSince(std::size_t begin) const {
            std::map<std::string, ProjectedThread> projected;
            std::size_t occurrences = 0;
            for (std::size_t index = begin; index < protocolMessages.size(); ++index) {
                const auto* batch = std::get_if<frontend::EventBatch>(&protocolMessages[index]);
                if (batch == nullptr) {
                    continue;
                }
                for (const frontend::FrontendEvent& event : batch->events) {
                    if (event.type != frontend::toString(frontend::ExpandedEventType::ThreadUpserted)) {
                        continue;
                    }
                    ++occurrences;
                    const auto value = event.data.find("thread");
                    if (value == event.data.end() || !value->is_object()) {
                        continue;
                    }
                    const auto id = value->find("id");
                    const auto title = value->find("title");
                    const auto preview = value->find("preview");
                    if (id == value->end() || !id->is_string() || title == value->end() || !title->is_string() || preview == value->end() ||
                        !preview->is_string()) {
                        continue;
                    }
                    projected.emplace(id->get<std::string>(), ProjectedThread{title->get<std::string>(), preview->get<std::string>()});
                }
            }
            return {std::move(projected), occurrences};
        }

        tests::support::TestResult& result;
        bool largeThreads = false;
        bool forceSnapshotFallback = false;
        bool turnLifecycle = false;
        std::shared_ptr<tests::codex::FakeTransportState> transport;
        std::unique_ptr<FakeBackendCore> core;
        std::unique_ptr<frontend::FrontendService> service;
        std::unique_ptr<client::Client> sdk;
        std::optional<frontend::FrontendConnection> frontendConnection;
        std::optional<frontend::FrontendConnection> replayConnection;
        std::optional<client::Connection> sdkConnection;
        std::deque<client::OutboundMessage> clientOutbound;
        std::deque<frontend::OutboundMessage> serverOutbound;
        std::deque<Json> lifecycleNotifications;
        std::vector<client::ConnectionStateChange> connectionStates;
        std::vector<client::StateUpdate> stateUpdates;
        std::vector<client::SynchronizationInfo> synchronized;
        std::vector<frontend::SequenceNumber> cursors;
        std::vector<frontend::ServerMessage> protocolMessages;
        std::vector<client::Diagnostic> sdkDiagnostics;
        std::vector<WireItemObservation> wireItems;
        std::vector<WireContentObservation> wireContent;
        std::vector<std::string> sdkCloseReasons;
        std::vector<std::string> serviceCloseReasons;
        std::vector<std::string> replayCloseReasons;
        std::vector<std::string> replayCompactMessages;
        std::vector<frontend::FrontendEvent> replayEvents;
        std::size_t providerListCalls = 0;
        std::size_t providerThreadStartCalls = 0;
        std::size_t providerTurnStartCalls = 0;
        std::size_t lifecycleNotificationsInjected = 0;
        std::size_t serializedMessagesDelivered = 0;
        std::size_t serializedMessageDecodeFailures = 0;
        std::size_t serializedMessageRoundTripMismatches = 0;
        std::size_t expandedEventsEmitted = 0;
        std::size_t expandedEventsSchemaValid = 0;
        std::size_t invalidExpandedEvents = 0;
        std::size_t sdkProtocolErrors = 0;
        std::size_t replayDecodeFailures = 0;
        std::size_t replayExpandedEvents = 0;
        std::size_t replayValidExpandedEvents = 0;
        bool exactTurnStartRequest = false;
        bool lifecycleNotificationScheduled = false;
        bool replayWelcome = false;
        bool replayComplete = false;
    };

    class IntegrationRunner {
    public:
        explicit IntegrationRunner(tests::support::TestResult& result)
            : result(result) {
        }

        void start() {
            startCompact();
        }

        [[nodiscard]] bool isFinished() const noexcept {
            return finished;
        }

        [[nodiscard]] const std::string& waitingStage() const noexcept {
            return waitingDescription;
        }

    private:
        static void defer(std::function<void()> callback) {
            core::EventReceiver::atNextTick(std::move(callback));
        }

        void
        waitUntil(std::string description, std::function<bool()> predicate, std::function<void()> next, std::size_t remaining = 50'000) {
            waitingDescription = description;
            defer([this,
                   description = std::move(description),
                   predicate = std::move(predicate),
                   next = std::move(next),
                   remaining]() mutable {
                if (finished) {
                    return;
                }
                try {
                    if (predicate()) {
                        waitingDescription = "advancing after: " + description;
                        next();
                    } else if (remaining == 0) {
                        result.expectTrue(false, description);
                        finish();
                    } else {
                        waitUntil(std::move(description), std::move(predicate), std::move(next), remaining - 1);
                    }
                } catch (...) {
                    result.expectTrue(false, "cross-layer callback exception at stage: " + description);
                    finish();
                }
            });
        }

        void afterTicks(std::size_t count, std::function<void()> next) {
            if (count == 0) {
                next();
                return;
            }
            defer([this, count, next = std::move(next)]() mutable {
                if (harness) {
                    harness->settle();
                }
                afterTicks(count - 1, std::move(next));
            });
        }

        void expectInitialState() {
            const client::State state = harness->sdk->state();
            std::set<std::string> retainedIds;
            std::set<std::string> actualRetainedIds;
            for (const client::ThreadState& retained : state.threads()) {
                actualRetainedIds.insert(retained.id.value);
            }
            bool exactInitialContent = true;
            for (std::size_t index = 0; index < RetainedThreadCount; ++index) {
                const std::string id = "live-thread-" + std::to_string(index);
                retainedIds.insert(id);
                const client::ThreadState* retained = state.thread(id);
                exactInitialContent =
                    exactInitialContent && retained != nullptr &&
                    retained->title == std::optional<std::string>{threadTitle("Initial", index)} &&
                    retained->preview == std::optional<std::string>{projectedThreadPreview("Initial", index, harness->largeThreads)};
            }
            result.expectTrue(harness->sdk->isReady() && harness->sdkConnection->isOpen() && state.threadList().value.has_value() &&
                                  state.threadList().value->hasLoadedPage && !state.threadList().value->complete &&
                                  state.threadList().value->pagesLoaded == 1 &&
                                  state.threadList().value->nextCursor == std::optional<std::string>{"initial-next"} &&
                                  state.threadList().value->backwardsCursor == std::optional<std::string>{"page-backwards"} &&
                                  state.threads().size() == RetainedThreadCount && actualRetainedIds == retainedIds &&
                                  exactInitialContent && harness->synchronized.size() == 1 && harness->sdkCloseReasons.empty(),
                              "the real FrontendService expanded snapshot gives the SDK a typed 35-thread list and reaches Ready once");
        }

        void startCompact() {
            harness = std::make_unique<CrossLayerHarness>(result, true, false);
            waitUntil(
                "the large fake App Server initializes BackendCore with 35 retained threads",
                [this]() {
                    return harness->core->isReady() && harness->providerListCalls == 1 &&
                           harness->core->snapshot().threads.size() == RetainedThreadCount;
                },
                [this]() {
                    result.expectTrue(true, "the fake App Server initializes BackendCore before frontend attachment");
                    harness->attach();
                    waitUntil(
                        "the SDK reaches Ready through the real FrontendService",
                        [this]() {
                            harness->settle();
                            return harness->sdk->isReady() && harness->queuesEmpty();
                        },
                        [this]() {
                            afterTicks(4, [this]() {
                                expectInitialState();
                                beginCompactList();
                            });
                        });
                });
        }

        void beginCompactList() {
            protocolBaseline = harness->protocolMessages.size();
            updateBaseline = harness->stateUpdates.size();
            synchronizedBaseline = harness->synchronized.size();
            cursorBefore = harness->sdk->synchronizedThrough();
            completions = 0;
            completed.reset();
            const client::Submission submission = harness->sdk->threads().list(
                typed::ThreadListParams{}, [this](const client::OperationResult<client::ThreadListResult>& operation) {
                    ++completions;
                    if (operation.value) {
                        completed = *operation.value;
                    }
                });
            result.expectTrue(submission.accepted(), "typed thread.list is accepted across the in-memory SDK transport");
            waitUntil(
                "the real service queues the typed response and compact expanded thread-list occurrence",
                [this]() {
                    harness->driveSources();
                    return harness->queuedResponse() &&
                           harness->queuedExpandedEvents(frontend::ExpandedEventType::ThreadUpserted) == PageThreadCount &&
                           harness->queuedExpandedEvents(frontend::ExpandedEventType::ThreadListUpdated) == 1;
                },
                [this]() {
                    harness->deliverAll(DeliveryOrder::Wire);
                    verifyCompactList();
                });
        }

        void verifyCompactList() {
            const auto responsePosition = std::find_if(harness->protocolMessages.begin() + static_cast<std::ptrdiff_t>(protocolBaseline),
                                                       harness->protocolMessages.end(),
                                                       [](const frontend::ServerMessage& message) {
                                                           return std::holds_alternative<frontend::Response>(message);
                                                       });
            const auto statePosition = std::find_if(harness->protocolMessages.begin() + static_cast<std::ptrdiff_t>(protocolBaseline),
                                                    harness->protocolMessages.end(),
                                                    [](const frontend::ServerMessage& message) {
                                                        return std::holds_alternative<frontend::EventBatch>(message);
                                                    });
            const client::State state = harness->sdk->state();
            const auto [projectedThreads, projectedOccurrences] = harness->projectedThreadsSince(protocolBaseline);
            std::set<std::string> expectedPageIds;
            bool exactProjectedContent = true;
            bool exactResponseContent = completed.has_value();
            bool exactStateContent = true;
            for (std::size_t index = 0; index < PageThreadCount; ++index) {
                const std::string id = "live-thread-" + std::to_string(index);
                expectedPageIds.insert(id);
                const auto projected = projectedThreads.find(id);
                exactProjectedContent = exactProjectedContent && projected != projectedThreads.end() &&
                                        projected->second.title == threadTitle("Page", index) &&
                                        projected->second.preview == projectedThreadPreview("Page", index, true);
                if (completed) {
                    const auto responseThread =
                        std::find_if(completed->threads.begin(), completed->threads.end(), [&id](const auto& value) {
                            return value.state.id.value == id;
                        });
                    exactResponseContent = exactResponseContent && responseThread != completed->threads.end() &&
                                           responseThread->state.title == std::optional<std::string>{threadTitle("Page", index)} &&
                                           responseThread->state.preview == std::optional<std::string>{threadPreview("Page", index, true)};
                }
                const client::ThreadState* current = state.thread(id);
                exactStateContent = exactStateContent && current != nullptr &&
                                    current->title == std::optional<std::string>{threadTitle("Page", index)} &&
                                    current->preview == std::optional<std::string>{projectedThreadPreview("Page", index, true)};
            }
            for (std::size_t index = PageThreadCount; index < RetainedThreadCount; ++index) {
                const client::ThreadState* unrelated = state.thread("live-thread-" + std::to_string(index));
                exactStateContent = exactStateContent && unrelated != nullptr &&
                                    unrelated->title == std::optional<std::string>{threadTitle("Initial", index)} &&
                                    unrelated->preview == std::optional<std::string>{projectedThreadPreview("Initial", index, true)};
            }
            std::set<std::string> actualPageIds;
            for (const auto& [id, projected] : projectedThreads) {
                static_cast<void>(projected);
                actualPageIds.insert(id);
            }
            result.expectTrue(completions == 1 && completed && completed->threads.size() == PageThreadCount &&
                                  harness->providerListCalls == 2 && harness->liveSnapshotsSince(protocolBaseline) == 0 &&
                                  harness->expandedEventsSince(protocolBaseline, frontend::ExpandedEventType::ThreadUpserted) ==
                                      PageThreadCount &&
                                  harness->expandedEventsSince(protocolBaseline, frontend::ExpandedEventType::ThreadListUpdated) == 1 &&
                                  responsePosition != harness->protocolMessages.end() && statePosition != harness->protocolMessages.end() &&
                                  responsePosition < statePosition && harness->sdk->isReady() && harness->sdkConnection->isOpen() &&
                                  harness->sdk->pendingOperationCount() == 0 && harness->sdkCloseReasons.empty(),
                              "a realistic 25-thread page over 35 retained large threads returns once and emits only 25 page upserts "
                              "plus one compact threadList.updated without snapshot fallback");
            result.expectTrue(projectedOccurrences == PageThreadCount && projectedThreads.size() == PageThreadCount &&
                                  actualPageIds == expectedPageIds,
                              "the 25 projected page occurrences carry the exact 25 unique response-page IDs without substitution");
            result.expectTrue(exactProjectedContent,
                              "each projected page event carries the title and bounded preview belonging to its exact thread ID");
            result.expectTrue(exactResponseContent,
                              "the typed response preserves every exact page ID with its matching title and provider preview");
            result.expectTrue(exactStateContent,
                              "the reducer updates the exact 25 page threads and leaves the ten unrelated retained threads unchanged");
            result.expectTrue(
                state.threads().size() == RetainedThreadCount && state.threadList().value && state.threadList().value->pagesLoaded == 2 &&
                    state.threadList().value->nextCursor == std::optional<std::string>{"page-next"} &&
                    harness->stateUpdates.size() > updateBaseline && harness->synchronized.size() == synchronizedBaseline && cursorBefore &&
                    harness->sdk->synchronizedThrough() && *harness->sdk->synchronizedThrough() > *cursorBefore,
                "compact thread-list metadata preserves unrelated retained threads, advances the cursor, and fabricates "
                "no synchronization callback");

            secondCompletions = 0;
            const client::Submission second = harness->sdk->threads().list(
                typed::ThreadListParams{}, [this](const client::OperationResult<client::ThreadListResult>& operation) {
                    if (operation) {
                        ++secondCompletions;
                    }
                });
            result.expectTrue(second.accepted(), "a second typed command is accepted after the compact update");
            waitUntil(
                "the second typed command completes after compact thread-list delivery",
                [this]() {
                    harness->settle();
                    return secondCompletions == 1;
                },
                [this]() {
                    result.expectTrue(harness->sdk->isReady() && harness->sdkConnection->isOpen() &&
                                          harness->sdk->pendingOperationCount() == 0,
                                      "the same cross-layer connection remains Ready after the second typed command");
                    harness.reset();
                    afterTicks(8, [this]() {
                        startForcedFallback();
                    });
                });
        }

        void startForcedFallback() {
            harness = std::make_unique<CrossLayerHarness>(result, false, true);
            waitUntil(
                "the forced-fallback fake App Server initializes BackendCore",
                [this]() {
                    return harness->core->isReady() && harness->providerListCalls == 1 &&
                           harness->core->snapshot().threads.size() == RetainedThreadCount;
                },
                [this]() {
                    harness->attach();
                    waitUntil(
                        "the forced-fallback SDK reaches Ready through initial synchronization",
                        [this]() {
                            harness->settle();
                            return harness->sdk->isReady() && harness->queuesEmpty();
                        },
                        [this]() {
                            afterTicks(8, [this]() {
                                expectInitialState();
                                beginForcedList();
                            });
                        });
                });
        }

        void beginForcedList() {
            protocolBaseline = harness->protocolMessages.size();
            updateBaseline = harness->stateUpdates.size();
            synchronizedBaseline = harness->synchronized.size();
            cursorCallbackBaseline = harness->cursors.size();
            cursorBefore = harness->sdk->synchronizedThrough();
            completions = 0;
            const client::Submission submission = harness->sdk->threads().list(
                typed::ThreadListParams{}, [this](const client::OperationResult<client::ThreadListResult>& operation) {
                    if (operation) {
                        ++completions;
                    }
                });
            result.expectTrue(submission && harness->sdk->pendingOperationCount() == 1,
                              "the forced-fallback scenario retains one correlated typed command before delivery");
            waitUntil(
                "the injected one-byte batch bound queues both response and live Snapshot",
                [this]() {
                    harness->driveSources();
                    return harness->queuedResponse() && harness->queuedSnapshot();
                },
                [this]() {
                    harness->deliverAll(DeliveryOrder::StateBeforeResponse);
                    verifyForcedList();
                });
        }

        void verifyForcedList() {
            const auto responsePosition = std::find_if(harness->protocolMessages.begin() + static_cast<std::ptrdiff_t>(protocolBaseline),
                                                       harness->protocolMessages.end(),
                                                       [](const frontend::ServerMessage& message) {
                                                           return std::holds_alternative<frontend::Response>(message);
                                                       });
            const auto snapshotPosition = std::find_if(harness->protocolMessages.begin() + static_cast<std::ptrdiff_t>(protocolBaseline),
                                                       harness->protocolMessages.end(),
                                                       [](const frontend::ServerMessage& message) {
                                                           return std::holds_alternative<frontend::Snapshot>(message);
                                                       });
            const bool snapshotUpdate = harness->stateUpdates.size() == updateBaseline + 1 &&
                                        harness->stateUpdates.back().cause == client::UpdateCause::SnapshotFallback &&
                                        harness->stateUpdates.back().changes.size() == 2 &&
                                        std::holds_alternative<client::StateReplacedChange>(harness->stateUpdates.back().changes[0]) &&
                                        std::holds_alternative<client::CursorAdvancedChange>(harness->stateUpdates.back().changes[1]);
            const client::State state = harness->sdk->state();
            result.expectTrue(harness->liveSnapshotsSince(protocolBaseline) == 1 && snapshotPosition != harness->protocolMessages.end() &&
                                  responsePosition != harness->protocolMessages.end() && snapshotPosition < responsePosition &&
                                  snapshotUpdate && completions == 1 && harness->sdk->pendingOperationCount() == 0 &&
                                  harness->sdk->isReady() && harness->sdkConnection->isOpen() && harness->sdkCloseReasons.empty(),
                              "an injected one-byte event-batch bound delivers a live Snapshot before the response without losing "
                              "correlation or closing the Ready SDK connection");
            result.expectTrue(
                state.threadList().value && state.threadList().value->pagesLoaded == 2 && state.threads().size() == RetainedThreadCount &&
                    harness->synchronized.size() == synchronizedBaseline && cursorBefore && harness->sdk->synchronizedThrough() &&
                    *harness->sdk->synchronizedThrough() > *cursorBefore && harness->cursors.size() == cursorCallbackBaseline + 1,
                "the live barrier replaces typed State once, advances the durable cursor once, and invokes no fabricated "
                "onSynchronized callback");

            secondCompletions = 0;
            const client::Submission second = harness->sdk->threads().list(
                typed::ThreadListParams{}, [this](const client::OperationResult<client::ThreadListResult>& operation) {
                    if (operation) {
                        ++secondCompletions;
                    }
                });
            result.expectTrue(second.accepted(), "a second typed command is accepted after the live Snapshot barrier");
            waitUntil(
                "the second typed command completes after the live Snapshot barrier",
                [this]() {
                    harness->settle();
                    return secondCompletions == 1;
                },
                [this]() {
                    result.expectTrue(harness->sdk->isReady() && harness->sdkConnection->isOpen(),
                                      "a second typed thread.list succeeds after the deterministic live Snapshot barrier");
                    harness.reset();
                    afterTicks(8, [this]() {
                        startTurnLifecycle();
                    });
                });
        }

        void startTurnLifecycle() {
            harness = std::make_unique<CrossLayerHarness>(result, false, false, true);
            waitUntil(
                "the turn-lifecycle fake App Server initializes BackendCore",
                [this]() {
                    return harness->core->isReady() && harness->providerListCalls == 1 &&
                           harness->core->snapshot().threads.size() == RetainedThreadCount;
                },
                [this]() {
                    harness->attach();
                    waitUntil(
                        "the turn-lifecycle SDK reaches Ready through initial synchronization",
                        [this]() {
                            harness->settle();
                            return harness->sdk->isReady() && harness->queuesEmpty();
                        },
                        [this]() {
                            afterTicks(4, [this]() {
                                expectInitialState();
                                beginLifecycleAcquire();
                            });
                        });
                });
        }

        void beginLifecycleAcquire() {
            lifecycleExpandedBaseline = harness->expandedEventsEmitted;
            lifecycleValidBaseline = harness->expandedEventsSchemaValid;
            lifecycleInvalidBaseline = harness->invalidExpandedEvents;
            lifecycleSerializedBaseline = harness->serializedMessagesDelivered;
            lifecycleWireItemBaseline = harness->wireItems.size();
            lifecycleWireContentBaseline = harness->wireContent.size();
            controllerCompletions = 0;
            controllerResult.reset();
            const client::Submission submission =
                harness->sdk->controller().acquire([this](const client::OperationResult<client::ControllerResult>& operation) {
                    ++controllerCompletions;
                    if (operation.value) {
                        controllerResult = *operation.value;
                    }
                });
            result.expectTrue(submission.accepted(), "the Ready SDK accepts controller.acquire before the realistic turn lifecycle");
            waitUntil(
                "controller.acquire completes through the real FrontendService",
                [this]() {
                    harness->settle();
                    return controllerCompletions == 1;
                },
                [this]() {
                    result.expectTrue(controllerResult && controllerResult->ownedByThisClient &&
                                          harness->sdk->controller().ownedByThisClient(),
                                      "the realistic turn lifecycle owns the controller before provider commands");
                    beginLifecycleThreadStart();
                });
        }

        void beginLifecycleThreadStart() {
            threadStartCompletions = 0;
            threadStartResult.reset();
            const client::Submission submission = harness->sdk->threads().start(
                typed::ThreadStartParams{}, [this](const client::OperationResult<client::ThreadStartResult>& operation) {
                    ++threadStartCompletions;
                    if (operation.value) {
                        threadStartResult = *operation.value;
                    }
                });
            result.expectTrue(submission.accepted(), "typed thread.start is accepted after controller acquisition");
            waitUntil(
                "typed thread.start returns its exact projected thread",
                [this]() {
                    harness->settle();
                    return threadStartCompletions == 1;
                },
                [this]() {
                    result.expectTrue(harness->providerThreadStartCalls == 1 && threadStartResult &&
                                          threadStartResult->threadId == typed::ThreadId{std::string(LifecycleThreadId)} &&
                                          threadStartResult->thread &&
                                          threadStartResult->thread->state.id == typed::ThreadId{std::string(LifecycleThreadId)},
                                      "typed thread.start preserves the fake App Server thread identity across every layer");
                    beginLifecycleTurnStart();
                });
        }

        void beginLifecycleTurnStart() {
            lifecycleReplayAfter = harness->service->currentSequence();
            typed::TurnStartParams parameters;
            parameters.threadId = typed::ThreadId{std::string(LifecycleThreadId)};
            typed::TextInput input;
            input.text = LifecycleUserText;
            parameters.input.emplace_back(std::move(input));
            turnStartCompletions = 0;
            turnStartResult.reset();
            const client::Submission submission = harness->sdk->turns().start(
                std::move(parameters), [this](const client::OperationResult<client::TurnStartResult>& operation) {
                    ++turnStartCompletions;
                    if (operation.value) {
                        turnStartResult = *operation.value;
                    }
                });
            result.expectTrue(submission.accepted(), "typed turn.start is accepted for the exact started thread");
            waitUntil(
                "the typed user item, agent item, content delta, and completed turn reach SDK State",
                [this]() {
                    harness->settle();
                    const client::State state = harness->sdk->state();
                    const client::TurnState* turn = state.turn(LifecycleTurnId);
                    const client::ItemState* agent = state.item(LifecycleAgentItemId);
                    return turnStartCompletions == 1 && turn != nullptr && turn->terminal && agent != nullptr &&
                           agent->agentText == std::optional<std::string>{std::string(LifecycleAgentText)} && harness->queuesEmpty();
                },
                [this]() {
                    verifyTurnLifecycle();
                });
        }

        void verifyTurnLifecycle() {
            const client::State state = harness->sdk->state();
            const client::ThreadState* thread = state.thread(LifecycleThreadId);
            const client::TurnState* turn = state.turn(LifecycleTurnId);
            const client::ItemState* userItem = state.item(LifecycleUserItemId);
            const client::ItemState* agentItem = state.item(LifecycleAgentItemId);
            const CrossLayerHarness::WireItemObservation* wireUser = harness->wireItemSince(lifecycleWireItemBaseline, LifecycleUserItemId);
            const CrossLayerHarness::WireItemObservation* wireAgent =
                harness->wireItemSince(lifecycleWireItemBaseline, LifecycleAgentItemId);
            const CrossLayerHarness::WireContentObservation* wireAgentContent =
                harness->wireContentSince(lifecycleWireContentBaseline, LifecycleAgentItemId);
            const std::size_t emitted = harness->expandedEventsEmitted - lifecycleExpandedBaseline;
            const std::size_t valid = harness->expandedEventsSchemaValid - lifecycleValidBaseline;
            const std::size_t invalid = harness->invalidExpandedEvents - lifecycleInvalidBaseline;
            const bool noErrorDiagnostics =
                std::none_of(harness->sdkDiagnostics.begin(), harness->sdkDiagnostics.end(), [](const client::Diagnostic& diagnostic) {
                    return diagnostic.severity == client::Diagnostic::Severity::Error;
                });

            result.expectTrue(harness->providerTurnStartCalls == 1 && harness->exactTurnStartRequest &&
                                  harness->lifecycleNotificationsInjected == 8 && turnStartResult &&
                                  turnStartResult->turnId == typed::TurnId{std::string(LifecycleTurnId)} && turnStartResult->turn &&
                                  turnStartResult->turn->state.threadId == typed::ThreadId{std::string(LifecycleThreadId)},
                              "turn.start and all typed lifecycle notifications traverse the actual fake App Server path once");
            result.expectTrue(wireUser != nullptr && wireUser->type == "userMessage" && wireUser->id == LifecycleUserItemId &&
                                  wireUser->threadId == LifecycleThreadId && wireUser->turnId == LifecycleTurnId &&
                                  wireUser->status == "completed" && wireUser->item.value("type", "") == "userMessage" &&
                                  wireUser->item.value("startedAtMs", std::int64_t{-1}) == 10 &&
                                  wireUser->item.value("completedAtMs", std::int64_t{-1}) == 20 &&
                                  wireUser->compactMessage.find("\"type\":\"userMessage\"") != std::string::npos,
                              "serialized item.upserted carries userMessage with its exact thread, turn, and item identity");
            result.expectTrue(wireAgent != nullptr && wireAgent->type == "agentMessage" && wireAgent->id == LifecycleAgentItemId &&
                                  wireAgent->threadId == LifecycleThreadId && wireAgent->turnId == LifecycleTurnId &&
                                  wireAgent->status == "completed" && wireAgent->item.value("type", "") == "agentMessage" &&
                                  wireAgent->item.value("startedAtMs", std::int64_t{-1}) == 30 &&
                                  wireAgent->item.value("completedAtMs", std::int64_t{-1}) == 40 &&
                                  wireAgent->compactMessage.find("\"type\":\"agentMessage\"") != std::string::npos,
                              "serialized item.upserted carries agentMessage with its exact thread, turn, and item identity");
            result.expectTrue(wireAgentContent != nullptr && wireAgentContent->itemId == LifecycleAgentItemId &&
                                  wireAgentContent->threadId == LifecycleThreadId && wireAgentContent->turnId == LifecycleTurnId &&
                                  wireAgentContent->channel == "agentText" && wireAgentContent->content == LifecycleAgentText,
                              "serialized item.content.updated carries the exact agentText delta and parent identities");
            result.expectTrue(
                thread != nullptr &&
                    std::find(thread->orderedTurns.begin(), thread->orderedTurns.end(), typed::TurnId{std::string(LifecycleTurnId)}) !=
                        thread->orderedTurns.end() &&
                    turn != nullptr && turn->threadId == typed::ThreadId{std::string(LifecycleThreadId)} &&
                    turn->status == typed::TurnStatus::completed() && turn->terminal && !turn->active &&
                    turn->orderedItems ==
                        std::vector{typed::ItemId{std::string(LifecycleUserItemId)}, typed::ItemId{std::string(LifecycleAgentItemId)}},
                "SDK State retains the exact completed thread/turn hierarchy and ordered item identities");
            result.expectTrue(userItem != nullptr && userItem->kind.is(frontend::ThreadItemKind::UserMessage) &&
                                  userItem->threadId == typed::ThreadId{std::string(LifecycleThreadId)} &&
                                  userItem->turnId == typed::TurnId{std::string(LifecycleTurnId)} &&
                                  userItem->status == std::optional<std::string>{"completed"} && agentItem != nullptr &&
                                  userItem->startedAtMs == std::optional<std::int64_t>{10} &&
                                  userItem->completedAtMs == std::optional<std::int64_t>{20} &&
                                  agentItem->kind.is(frontend::ThreadItemKind::AgentMessage) &&
                                  agentItem->threadId == typed::ThreadId{std::string(LifecycleThreadId)} &&
                                  agentItem->turnId == typed::TurnId{std::string(LifecycleTurnId)} &&
                                  agentItem->status == std::optional<std::string>{"completed"} &&
                                  agentItem->startedAtMs == std::optional<std::int64_t>{30} &&
                                  agentItem->completedAtMs == std::optional<std::int64_t>{40} &&
                                  agentItem->agentText == std::optional<std::string>{std::string(LifecycleAgentText)},
                              "SDK State exposes completed userMessage and agentMessage items with accumulated final agent text");
            result.expectTrue(emitted > 0 && emitted == valid && invalid == 0 &&
                                  harness->serializedMessagesDelivered > lifecycleSerializedBaseline &&
                                  harness->serializedMessageDecodeFailures == 0 && harness->serializedMessageRoundTripMismatches == 0,
                              "every emitted expanded lifecycle event validates and every compact server message round-trips");
            result.expectTrue(harness->sdk->isReady() && harness->sdkConnection->isOpen() &&
                                  harness->sdkConnection->isTransportConnected() && harness->frontendConnection->isOpen() &&
                                  harness->sdk->pendingOperationCount() == 0 && harness->sdkCloseReasons.empty() &&
                                  harness->serviceCloseReasons.empty() && harness->sdkProtocolErrors == 0 && noErrorDiagnostics,
                              "the complete turn leaves the physical transport open and the SDK Ready without protocol diagnostics");

            harness->beginReplay(lifecycleReplayAfter);
            waitUntil(
                "the exact projected turn lifecycle replays from the service journal",
                [this]() {
                    return harness->replayComplete || !harness->replayCloseReasons.empty();
                },
                [this]() {
                    verifyLifecycleReplayAndBeginSecondCommand();
                });
        }

        void verifyLifecycleReplayAndBeginSecondCommand() {
            const CrossLayerHarness::WireItemObservation* liveUser = harness->wireItemSince(lifecycleWireItemBaseline, LifecycleUserItemId);
            const CrossLayerHarness::WireItemObservation* liveAgent =
                harness->wireItemSince(lifecycleWireItemBaseline, LifecycleAgentItemId);
            const CrossLayerHarness::WireContentObservation* liveContent =
                harness->wireContentSince(lifecycleWireContentBaseline, LifecycleAgentItemId);
            const frontend::FrontendEvent* replayUser = harness->replayItem(LifecycleUserItemId);
            const frontend::FrontendEvent* replayAgent = harness->replayItem(LifecycleAgentItemId);
            const frontend::FrontendEvent* replayContent = harness->replayContent(LifecycleAgentItemId);
            const bool exactReplayItems =
                liveUser != nullptr && liveAgent != nullptr && replayUser != nullptr && replayAgent != nullptr &&
                replayUser->data.at("item") == liveUser->item && replayAgent->data.at("item") == liveAgent->item &&
                replayUser->data.at("item").at("type") == "userMessage" && replayAgent->data.at("item").at("type") == "agentMessage";
            const bool exactReplayContent = liveContent != nullptr && replayContent != nullptr &&
                                            replayContent->data.value("threadId", "") == liveContent->threadId &&
                                            replayContent->data.value("turnId", "") == liveContent->turnId &&
                                            replayContent->data.value("itemId", "") == liveContent->itemId &&
                                            replayContent->data.value("channel", "") == liveContent->channel &&
                                            replayContent->data.value("content", "") == liveContent->content;
            const bool replayWireNormalized =
                std::all_of(harness->replayCompactMessages.begin(), harness->replayCompactMessages.end(), [](const std::string& message) {
                    return message.find("\"type\":\"user_message\"") == std::string::npos &&
                           message.find("\"type\":\"agent_message\"") == std::string::npos;
                });
            result.expectTrue(harness->replayWelcome && harness->replayComplete && harness->replayConnection->isOpen() &&
                                  harness->replayCloseReasons.empty() && harness->replayDecodeFailures == 0 &&
                                  harness->replayExpandedEvents > 0 &&
                                  harness->replayExpandedEvents == harness->replayValidExpandedEvents && exactReplayItems &&
                                  exactReplayContent && replayWireNormalized,
                              "journal replay preserves the same stable item discriminators, fields, content, and exact identities");

            lifecycleSecondCompletions = 0;
            const client::Submission second = harness->sdk->threads().list(
                typed::ThreadListParams{}, [this](const client::OperationResult<client::ThreadListResult>& operation) {
                    if (operation) {
                        ++lifecycleSecondCompletions;
                    }
                });
            result.expectTrue(second.accepted(), "a second typed command is accepted after the completed realistic turn");
            waitUntil(
                "the second typed command completes after the realistic turn",
                [this]() {
                    harness->settle();
                    return lifecycleSecondCompletions == 1;
                },
                [this]() {
                    const client::State finalState = harness->sdk->state();
                    const client::ItemState* finalAgent = finalState.item(LifecycleAgentItemId);
                    result.expectTrue(harness->providerListCalls == 2 && harness->sdk->isReady() && harness->sdkConnection->isOpen() &&
                                          harness->sdkConnection->isTransportConnected() && harness->frontendConnection->isOpen() &&
                                          harness->sdk->pendingOperationCount() == 0 && harness->sdkCloseReasons.empty() &&
                                          harness->serviceCloseReasons.empty() && harness->sdkProtocolErrors == 0 &&
                                          harness->invalidExpandedEvents == 0 &&
                                          harness->expandedEventsEmitted == harness->expandedEventsSchemaValid &&
                                          harness->serializedMessageDecodeFailures == 0 && finalAgent != nullptr &&
                                          finalAgent->agentText == std::optional<std::string>{std::string(LifecycleAgentText)},
                                      "the same Ready connection accepts a second typed command and retains completed turn state");
                    finish();
                });
        }

        void finish() {
            if (finished || finishing) {
                return;
            }
            finishing = true;
            harness.reset();
            afterTicks(8, [this]() {
                finishing = false;
                finished = true;
                waitingDescription = "complete";
                core::SNodeC::stop();
            });
        }

        tests::support::TestResult& result;
        std::unique_ptr<CrossLayerHarness> harness;
        std::size_t protocolBaseline = 0;
        std::size_t updateBaseline = 0;
        std::size_t synchronizedBaseline = 0;
        std::size_t cursorCallbackBaseline = 0;
        std::optional<frontend::SequenceNumber> cursorBefore;
        std::size_t completions = 0;
        std::size_t secondCompletions = 0;
        std::size_t controllerCompletions = 0;
        std::size_t threadStartCompletions = 0;
        std::size_t turnStartCompletions = 0;
        std::size_t lifecycleSecondCompletions = 0;
        std::size_t lifecycleExpandedBaseline = 0;
        std::size_t lifecycleValidBaseline = 0;
        std::size_t lifecycleInvalidBaseline = 0;
        std::size_t lifecycleSerializedBaseline = 0;
        std::size_t lifecycleWireItemBaseline = 0;
        std::size_t lifecycleWireContentBaseline = 0;
        frontend::SequenceNumber lifecycleReplayAfter{};
        std::optional<client::ThreadListResult> completed;
        std::optional<client::ControllerResult> controllerResult;
        std::optional<client::ThreadStartResult> threadStartResult;
        std::optional<client::TurnStartResult> turnStartResult;
        bool finishing = false;
        bool finished = false;
        std::string waitingDescription = "not started";
    };

} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    core::SNodeC::init(argc, argv);

    IntegrationRunner runner(result);
    runner.start();
    bool timedOut = false;
    [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
        [&timedOut]() {
            timedOut = true;
            core::SNodeC::stop();
        },
        utils::Timeval({15, 0}));
    const int eventLoopResult = core::SNodeC::start(utils::Timeval({17, 0}));
    result.expectTrue(!timedOut,
                      "cross-layer FrontendService-to-SDK regression completes before the watchdog (last stage: " + runner.waitingStage() +
                          ")");
    result.expectTrue(runner.isFinished(),
                      "compact-event, live-Snapshot, and complete-turn cross-layer scenarios finish deterministically");
    result.expectEqual(0, eventLoopResult, "the cross-layer in-memory SNode.C event loop exits cleanly");

    core::SNodeC::free();
    return result.processResult();
}
