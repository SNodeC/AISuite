/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/model/Model.h"
#include "ai/openai/codex/frontend/Codec.h"
#include "support/TestResult.h"

#include <array>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace model = frontend::internal::model;

    model::CanonicalSnapshot representativeSnapshot() {
        model::CanonicalSnapshot snapshot;
        snapshot.sequence = model::FrontendSequence{9};
        snapshot.provider.lifecycle = model::ProviderLifecycle::Ready;
        snapshot.backendCursor.backendRevision = 7;
        snapshot.backendCursor.oldestReplayableAfter = model::FrontendSequence{3};
        snapshot.backendCursor.currentSequence = snapshot.sequence;
        auto threadId = model::ThreadIdentity::parse("thread-1");
        auto turnId = model::TurnIdentity::parse("turn-1");
        auto itemId = model::ItemIdentity::parse("item-1");
        auto pendingId = model::PendingRequestIdentity::parse("1");
        snapshot.threads.emplace_back(*threadId);
        snapshot.turns.emplace_back(*turnId, *threadId);
        model::ItemData item{*itemId, *threadId, *turnId};
        item.agentText = "hello";
        snapshot.items.push_back(model::PlanItem{std::move(item)});
        snapshot.pendingRequests.push_back(model::CommandExecutionApprovalRequest{model::PendingRequestData{*pendingId}});
        return snapshot;
    }

    void testLegacyShapeAndRoundTrip(tests::support::TestResult& result) {
        const model::CanonicalSnapshot snapshot = representativeSnapshot();
        const auto legacy = model::encodeLegacySnapshot(snapshot);
        const auto decoded = legacy ? model::decodeLegacySnapshot(legacy.value())
                                    : model::ModelResult<model::CanonicalSnapshot>{legacy.error()};
        const bool exactShape = legacy && legacy.value().state.value("backendRevision", 0) == 7 &&
                                legacy.value().state.value("lifecycle", "") == "ready" &&
                                !legacy.value().state.contains("provider") && legacy.value().state.contains("journal") &&
                                legacy.value().state.contains("codexExtensions") &&
                                legacy.value().state.at("threads").front().at("turns").size() == 1;
        result.expectTrue(exactShape && decoded && decoded.value().sequence == snapshot.sequence &&
                              decoded.value().provider.lifecycle == model::ProviderLifecycle::Ready &&
                              decoded.value().items.size() == 1 && decoded.value().pendingRequests.size() == 1,
                          "legacy v1 uses its frozen nested shape and decodes into the shared typed snapshot authority");
    }

    void testIndependentRepresentationSelection(tests::support::TestResult& result) {
        const model::CanonicalSnapshot snapshot = representativeSnapshot();
        bool allCombinations = true;
        std::string failure;
        for (unsigned int bits = 0; bits < 8; ++bits) {
            const model::SnapshotRepresentationSelection selection{
                (bits & 1U) != 0, (bits & 2U) != 0, (bits & 4U) != 0, true};
            const auto encoded = model::encodeProjectedSnapshot(snapshot, selection);
            const auto decoded = encoded ? model::decodeProjectedSnapshot(encoded.value(), selection)
                                         : model::ModelResult<model::CanonicalSnapshot>{encoded.error()};
            const bool expandedDomains = (bits & 1U) != 0;
            const bool expandedItems = (bits & 2U) != 0;
            const bool expandedPending = (bits & 4U) != 0;
            const std::size_t expectedPending = 1;
            bool exactWireShape = encoded && encoded.value().extensions.contains("scopeProjection");
            if (encoded && expandedDomains) {
                exactWireShape = exactWireShape && encoded.value().state.contains("threads") &&
                                 encoded.value().state.contains("turns") && encoded.value().state.contains("items") &&
                                 encoded.value().state.at("items").size() == 1 &&
                                 encoded.value().state.at("items").front().at("type") == "plan";
                if (expandedItems) {
                    exactWireShape = exactWireShape &&
                                     !encoded.value().state.at("items").front().contains("codexType") &&
                                     encoded.value().state.at("items").front().contains("status");
                } else {
                    const frontend::Json expected{{"id", "item-1"}, {"type", "plan"}, {"codexType", "plan"}};
                    exactWireShape = exactWireShape && encoded.value().state.at("items").front() == expected;
                }
                exactWireShape = exactWireShape && encoded.value().state.contains("pendingRequests") &&
                                 (expandedPending ? encoded.value().state.at("pendingRequests").front().contains("kind")
                                                  : encoded.value().state.at("pendingRequests").front().contains("type"));
            }
            allCombinations = allCombinations && encoded && decoded && exactWireShape && decoded.value().items.size() == 1 &&
                              decoded.value().pendingRequests.size() == expectedPending;
            if (!encoded) {
                failure += " bits=" + std::to_string(bits) + " encode=" + encoded.error().message;
            } else if (!decoded) {
                failure += " bits=" + std::to_string(bits) + " decode=" + decoded.error().message;
            } else if (!exactWireShape) {
                failure += " bits=" + std::to_string(bits) + " wire=" + encoded.value().state.dump();
            } else if (decoded.value().items.size() != 1 || decoded.value().pendingRequests.size() != expectedPending) {
                failure += " bits=" + std::to_string(bits) + " counts=" + std::to_string(decoded.value().items.size()) + "/" +
                           std::to_string(decoded.value().pendingRequests.size());
            }
        }
        result.expectTrue(allCombinations,
                          "domain, item, and pending-request representations compose independently for all eight negotiated combinations" +
                              failure);
    }

    void testDiagnosticsUseFrozenSnapshotShape(tests::support::TestResult& result) {
        model::CanonicalSnapshot snapshot = representativeSnapshot();
        snapshot.diagnostics.state = model::DomainState::present();
        snapshot.diagnostics.received = 3;
        for (std::string message : {"first", "second", "third"}) {
            model::DiagnosticRecord diagnostic;
            diagnostic.message = std::move(message);
            diagnostic.detailsOmitted = true;
            snapshot.diagnostics.entries.push_back(std::move(diagnostic));
        }
        const auto encoded = model::encodeLegacySnapshot(snapshot);
        const bool exactWireShape =
            encoded && encoded.value().state.at("diagnostics").at("received") == 3 &&
            encoded.value().state.at("diagnostics").at("recent") == frontend::Json::array({"first", "second", "third"});
        const bool protocolValid = encoded && frontend::Codec::encodeServer(frontend::ServerMessage{encoded.value()}).hasValue();
        const auto decoded =
            encoded ? model::decodeLegacySnapshot(encoded.value()) : model::ModelResult<model::CanonicalSnapshot>{encoded.error()};
        result.expectTrue(exactWireShape && protocolValid && decoded && decoded.value().diagnostics.received == 3 &&
                              decoded.value().diagnostics.entries.size() == 3 &&
                              decoded.value().diagnostics.entries.back().message == std::optional<std::string>{"third"},
                          "typed diagnostics round-trip through the frozen received/recent snapshot shape");
    }
}

int main() {
    tests::support::TestResult result;
    testLegacyShapeAndRoundTrip(result);
    testIndependentRepresentationSelection(result);
    testDiagnosticsUseFrozenSnapshotShape(result);
    return result.processResult();
}
