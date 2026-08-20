/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/client/ClientCore.h"
#include "support/TestResult.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace core = ai::openai::codex::frontend::internal::client;
    namespace model = ai::openai::codex::frontend::internal::model;
    namespace generated = ai::openai::codex::frontend::generated;

    core::ClientOptions clientOptions() {
        core::ClientOptions options;
        options.credentialProvider = [] {
            return core::AuthenticationContext{frontend::NoCredential{}, std::nullopt};
        };
        return options;
    }

    struct Harness {
        std::vector<core::OutboundMessage> outbound;

        core::TransportCallbacks transport() {
            return {[this](core::OutboundMessage message) {
                        outbound.push_back(std::move(message));
                        return core::SendResult{};
                    },
                    [](std::string_view) {
                    }};
        }
    };

    std::vector<frontend::FrontendMethod> methods() {
        std::vector<frontend::FrontendMethod> result;
        for (const auto& metadata : generated::AllMethods) {
            result.emplace_back(metadata.method);
        }
        return result;
    }

    frontend::CapabilityAdvertisement capabilities() {
        std::vector<frontend::FrontendCapability> defined;
        for (const auto& metadata : generated::AllCapabilities) {
            if (metadata.defined) {
                defined.push_back(static_cast<frontend::FrontendCapability>(metadata.id));
            }
        }
        const std::vector<frontend::FrontendCapability> selected{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
        };
        return {defined, selected, selected, frontend::Json::object()};
    }

    model::CanonicalSnapshot populatedSnapshot() {
        model::CanonicalSnapshot snapshot;
        snapshot.sequence = model::FrontendSequence(4);
        snapshot.sessions.emplace_back(model::SessionIdentity{"1"});
        snapshot.threads.emplace_back(model::ThreadIdentity{"thread-1"});
        snapshot.turns.emplace_back(model::TurnIdentity{"turn-1"}, model::ThreadIdentity{"thread-1"});
        snapshot.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"item-1"}, model::ThreadIdentity{"thread-1"}, model::TurnIdentity{"turn-1"}}});
        snapshot.pendingRequests.emplace_back(
            model::CommandExecutionApprovalRequest{model::PendingRequestData{model::PendingRequestIdentity{"1"},
                                                                             model::ThreadIdentity{"thread-1"},
                                                                             model::TurnIdentity{"turn-1"},
                                                                             model::ItemIdentity{"item-1"}}});
        snapshot.processes.emplace_back(model::ProcessHandle{"process-1"});
        snapshot.processes.back().lifecycle = "running";

        model::FilesystemWatchRecord watch;
        watch.watchId = "watch-1";
        snapshot.filesystemWatches.state = model::DomainState::present();
        snapshot.filesystemWatches.entries.push_back(std::move(watch));
        model::FuzzySearchRecord fuzzy;
        fuzzy.sessionId = "fuzzy-1";
        snapshot.fuzzySearches.state = model::DomainState::present();
        snapshot.fuzzySearches.entries.push_back(std::move(fuzzy));
        model::ActivityRecord activity;
        activity.key = "activity-1";
        activity.subjectId = "thread-1";
        activity.kind = "turn";
        activity.lifecycle = "active";
        snapshot.activities.state = model::DomainState::present();
        snapshot.activities.entries.push_back(std::move(activity));

        snapshot.accounts.state = model::DomainState::present();
        model::DomainResultSummary account;
        account.method = "account/read";
        account.status = "ok";
        account.subjectId = "account-1";
        snapshot.accounts.state.latestResults.push_back(std::move(account));
        return snapshot;
    }

    frontend::Snapshot encodedSnapshot() {
        const auto expanded = model::encodeSnapshot(populatedSnapshot());
        if (!expanded) {
            throw std::runtime_error(expanded.error().path + ": " + expanded.error().message);
        }
        const auto encoded = frontend::Codec::encodeExpandedSnapshot(expanded.value());
        if (!encoded) {
            throw std::runtime_error(encoded.error().message);
        }
        return {frontend::SequenceNumber(4), encoded.value().at("state")};
    }

    void testImmutableIndexedState(tests::support::TestResult& result) {
        Harness harness;
        core::ClientCore client(clientOptions());
        const core::PhysicalGeneration generation = *client.attach(harness.transport());
        client.transportConnected(generation);
        frontend::Welcome welcome{"state-session",
                                  frontend::SessionRole::Observer,
                                  frontend::SequenceNumber(4),
                                  frontend::SyncMode::Snapshot,
                                  {{"permittedScopes", frontend::Json::array({"observe"})}},
                                  capabilities(),
                                  methods(),
                                  methods()};
        (void) client.receive(generation, frontend::ServerMessage{welcome});
        (void) client.receive(generation, frontend::ServerMessage{encodedSnapshot()});
        (void) client.receive(generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(4)}});

        const std::shared_ptr<const core::PublishedState> current = client.state();
        const bool indexed = current->sessionById("1") != nullptr && current->thread("thread-1") != nullptr &&
                             current->turn("turn-1") != nullptr && current->item("item-1") != nullptr &&
                             current->pendingRequest("1") != nullptr && current->process("process-1") != nullptr &&
                             current->filesystemWatch("watch-1") != nullptr && current->fuzzySearch("fuzzy-1") != nullptr &&
                             current->activity("activity-1") != nullptr && current->domain(core::SnapshotDomain::Accounts) != nullptr &&
                             current->domainResult(core::SnapshotDomain::Accounts, "account-1") != nullptr;
        result.expectTrue(indexed && current->thread("missing") == nullptr && current->pendingRequest("missing") == nullptr,
                          "published typed State provides deterministic indexed lookup across every retained entity class");

        client.transportDisconnected(generation, {"state becomes stale", true});
        const std::shared_ptr<const core::PublishedState> stale = client.state();
        result.expectTrue(stale != current && current->revision != std::numeric_limits<std::uint64_t>::max() &&
                              stale->revision == current->revision + 1 && current->freshness == core::PublishedFreshness::Current &&
                              stale->freshness == core::PublishedFreshness::Stale,
                          "disconnect publishes exactly the next immutable stale-State revision");
        result.expectTrue(current->snapshot && stale->snapshot && !current->snapshot->threads.empty() &&
                              !stale->snapshot->threads.empty() &&
                              stale->snapshot->threads.front().freshness == current->snapshot->threads.front().freshness,
                          "disconnect retains projected thread values without rewriting their source freshness");
        result.expectTrue(current->snapshot && stale->snapshot && !current->snapshot->pendingRequests.empty() &&
                              !stale->snapshot->pendingRequests.empty() &&
                              !model::pendingRequestData(current->snapshot->pendingRequests.front()).connectionInvalidated &&
                              model::pendingRequestData(stale->snapshot->pendingRequests.front()).connectionInvalidated,
                          "disconnect invalidates retained reverse requests without mutating the prior State");
    }

    void testOptionalMetadataPresence(tests::support::TestResult& result) {
        model::CanonicalSnapshot snapshot;
        snapshot.threadList.stampKnown = true;
        snapshot.accounts.state = model::DomainState::present(model::Freshness::Unknown);
        snapshot.accounts.state.stampKnown = true;
        snapshot.accounts.state.truncationKnown = true;

        core::PublishedState published;
        published.snapshot = std::make_shared<const model::CanonicalSnapshot>(snapshot);
        const frontend::Json explicitDefaults = published.serializeForTesting();

        snapshot.threadList.stampKnown = false;
        snapshot.accounts.state.stampKnown = false;
        snapshot.accounts.state.truncationKnown = false;
        published.snapshot = std::make_shared<const model::CanonicalSnapshot>(std::move(snapshot));
        const frontend::Json absentMetadata = published.serializeForTesting();

        const frontend::Json& explicitThreadList = explicitDefaults.at("threadList").at("value");
        const frontend::Json& explicitAccount = explicitDefaults.at("accounts").at("value");
        const frontend::Json& absentThreadList = absentMetadata.at("threadList").at("value");
        const frontend::Json& absentAccount = absentMetadata.at("accounts").at("value");
        result.expectTrue(explicitThreadList.contains("stamp") && explicitAccount.contains("stamp") &&
                              explicitAccount.contains("truncation") && !absentThreadList.contains("stamp") &&
                              !absentAccount.contains("stamp") && !absentAccount.contains("truncation"),
                          "published State preserves explicit default stamp/truncation presence and omits absent metadata");
    }

    void testForkTurnSerializationScoping(tests::support::TestResult& result) {
        model::CanonicalSnapshot snapshot;
        const model::ThreadIdentity firstThread{"first-thread"};
        const model::ThreadIdentity secondThread{"second-thread"};
        const model::ThreadIdentity uniqueThread{"unique-thread"};
        const model::TurnIdentity sharedTurn{"shared-turn"};
        const model::TurnIdentity uniqueTurn{"unique-turn"};
        snapshot.threads.emplace_back(firstThread);
        snapshot.threads.emplace_back(secondThread);
        snapshot.threads.emplace_back(uniqueThread);
        snapshot.turns.emplace_back(sharedTurn, firstThread);
        snapshot.turns.emplace_back(sharedTurn, secondThread);
        snapshot.turns.emplace_back(uniqueTurn, uniqueThread);
        snapshot.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"first-item"}, firstThread, sharedTurn}});
        snapshot.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"second-item"}, secondThread, sharedTurn}});
        snapshot.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"ambiguous-unscoped"}, std::nullopt, sharedTurn}});
        snapshot.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"unique-unscoped"}, std::nullopt, uniqueTurn}});

        core::PublishedState published;
        published.snapshot = std::make_shared<const model::CanonicalSnapshot>(std::move(snapshot));
        const frontend::Json serialized = published.serializeForTesting();
        const frontend::Json& turns = serialized.at("turns");
        const auto findTurn = [&turns](std::string_view threadId) -> const frontend::Json* {
            const auto found = std::find_if(turns.begin(), turns.end(), [threadId](const frontend::Json& turn) {
                return turn.value("threadId", "") == threadId;
            });
            return found == turns.end() ? nullptr : &*found;
        };
        const frontend::Json* first = findTurn("first-thread");
        const frontend::Json* second = findTurn("second-thread");
        const frontend::Json* unique = findTurn("unique-thread");
        result.expectTrue(first != nullptr && second != nullptr && unique != nullptr &&
                              first->at("orderedItems") == frontend::Json::array({"first-item"}) &&
                              second->at("orderedItems") == frontend::Json::array({"second-item"}) &&
                              unique->at("orderedItems") == frontend::Json::array({"unique-unscoped"}) &&
                              published.turn("shared-turn") == nullptr,
                          "published serialization preserves unique unscoped items, scopes fork descendants, and rejects bare ambiguity");
    }

    void testPartialForkTurnSerializationScoping(tests::support::TestResult& result) {
        model::CanonicalSnapshot snapshot;
        const model::ThreadIdentity retainedThread{"retained-thread"};
        const model::ThreadIdentity omittedThread{"omitted-thread"};
        const model::TurnIdentity sharedTurn{"partial-shared-turn"};
        snapshot.threads.emplace_back(retainedThread);
        snapshot.threads.emplace_back(omittedThread);
        snapshot.turns.emplace_back(sharedTurn, retainedThread);
        snapshot.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"other-scoped-item"}, omittedThread, sharedTurn}});
        snapshot.items.emplace_back(model::AgentMessageItem{
            model::ItemData{model::ItemIdentity{"ambiguous-unscoped-item"}, std::nullopt, sharedTurn}});

        core::PublishedState published;
        published.snapshot = std::make_shared<const model::CanonicalSnapshot>(std::move(snapshot));
        const frontend::Json serialized = published.serializeForTesting();
        const frontend::Json& turns = serialized.at("turns");
        const auto retained = std::find_if(turns.begin(), turns.end(), [](const frontend::Json& turn) {
            return turn.value("threadId", "") == "retained-thread";
        });
        result.expectTrue(retained != turns.end() && retained->at("orderedItems").empty(),
                          "published serialization rejects an unscoped item when a partial projection proves another parent");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testImmutableIndexedState(result);
    testOptionalMetadataPresence(result);
    testForkTurnSerializationScoping(result);
    testPartialForkTurnSerializationScoping(result);
    return result.processResult();
}
