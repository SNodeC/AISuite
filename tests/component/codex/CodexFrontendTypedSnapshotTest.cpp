/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/internal/model/Model.h"
#include "support/TestResult.h"

#include <array>
#include <ranges>
#include <string>

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

    void testExplicitControllerPresenceRoundTrip(tests::support::TestResult& result) {
        const auto encoded = model::encodeSnapshot(representativeSnapshot());
        if (!encoded) {
            result.expectTrue(false, "the representative typed snapshot encodes for the controller-presence fixture");
            return;
        }
        frontend::ExpandedSnapshot wire = encoded.value();
        wire.state.controller = frontend::Json{{"present", true}};
        const auto decoded = model::decodeSnapshot(wire);
        const auto roundTrip =
            decoded ? model::encodeSnapshot(decoded.value()) : model::ModelResult<frontend::ExpandedSnapshot>{decoded.error()};
        result.expectTrue(decoded && decoded.value().controller.safeDetails.json().value("present", false) && roundTrip &&
                              roundTrip.value().state.controller.value("present", false) &&
                              !roundTrip.value().state.controller.contains("controllerSessionId"),
                          "an explicit controller-presence fact without an owner survives the typed snapshot authority");
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

    void testLegacyUserInputSecretClassificationRoundTrip(tests::support::TestResult& result) {
        model::CanonicalSnapshot snapshot;
        snapshot.sequence = model::FrontendSequence{12};
        model::PendingRequestData data{model::PendingRequestIdentity{"12"}};
        data.questionsPresent = true;
        data.questions.push_back(model::PendingRequestQuestion{"question-1", "Credential", "Enter the credential", true, true, {}, {}});
        snapshot.pendingRequests.push_back(model::UserInputRequest{std::move(data)});

        const auto legacy = model::encodeLegacySnapshot(snapshot);
        const auto decoded =
            legacy ? model::decodeLegacySnapshot(legacy.value()) : model::ModelResult<model::CanonicalSnapshot>{legacy.error()};
        const model::PendingRequestData* request = decoded && decoded.value().pendingRequests.size() == 1
                                                       ? &model::pendingRequestData(decoded.value().pendingRequests.front())
                                                       : nullptr;
        const bool frozenWire =
            legacy && legacy.value().state.at("pendingRequests").at(0).at("details").at("questions").at(0).at("secret") == true;
        result.expectTrue(frozenWire && request != nullptr && request->questionsPresent && request->questions.size() == 1 &&
                              request->questions.front().secretAnswer,
                          "legacy user-input secret classification survives the compatibility scrub and typed conversion");
    }

    void testExpandedItemDetailWireBounds(tests::support::TestResult& result) {
        model::CanonicalSnapshot snapshot = representativeSnapshot();
        frontend::Json details = frontend::Json::object();
        details[""] = "empty-key-value";
        const std::string postTruncationForbiddenKey = std::string(245, '-') + "accessToken" + "benignSuffix";
        const std::string truncatedForbiddenKey = postTruncationForbiddenKey.substr(0, 256);
        details[postTruncationForbiddenKey] = "must-not-leak";
        details["array"] = frontend::Json::array();
        for (std::size_t index = 0; index < 65; ++index) {
            details["array"].push_back(index);
        }
        details["nested"] = frontend::Json::array({frontend::Json{{"not", "a scalar"}}});
        details["text"] = std::string(16'383, 'x') + "\xE2\x82\xAC";
        for (std::size_t index = 0; index < 70; ++index) {
            details["z-" + std::to_string(index)] = index;
        }
        auto safeDetails = model::SafeDetail::fromJson(std::move(details));
        if (safeDetails.has_value()) {
            std::visit(
                [&safeDetails](auto& item) {
                    item.value.safeDetails = std::move(*safeDetails);
                },
                snapshot.items.front());
        }

        const auto encoded = model::encodeProjectedSnapshot(snapshot, model::SnapshotRepresentationSelection{true, true, true, true});
        const frontend::Json projected = encoded && encoded.value().state.contains("items") && !encoded.value().state.at("items").empty()
                                             ? encoded.value().state.at("items").front().value("data", frontend::Json::object())
                                             : frontend::Json::object();
        const bool scalarLeaves = std::ranges::all_of(projected, [](const frontend::Json& value) {
            return value.is_null() || value.is_boolean() || value.is_number() || value.is_string() ||
                   (value.is_array() && std::ranges::all_of(value, [](const frontend::Json& element) {
                        return element.is_null() || element.is_boolean() || element.is_number() || element.is_string();
                    }));
        });
        const std::string expectedBoundedText = std::string(16'383, 'x') + "\xE2\x82\xAC";
        result.expectTrue(safeDetails.has_value() && encoded && projected.size() == 64 && projected.value("", "") == "empty-key-value" &&
                              !projected.contains(postTruncationForbiddenKey) && !projected.contains(truncatedForbiddenKey) &&
                              !projected.contains("nested") && projected.at("array").size() == 64 &&
                              projected.at("text").get_ref<const std::string&>() == expectedBoundedText && scalarLeaves,
                          "expanded item detail projection preserves an empty safe key, rejects a forbidden name created by key "
                          "truncation, and enforces exact property/array/UTF-8 string bounds");
    }
}

int main() {
    tests::support::TestResult result;
    testLegacyShapeAndRoundTrip(result);
    testExplicitControllerPresenceRoundTrip(result);
    testIndependentRepresentationSelection(result);
    testDiagnosticsUseFrozenSnapshotShape(result);
    testLegacyUserInputSecretClassificationRoundTrip(result);
    testExpandedItemDetailWireBounds(result);
    return result.processResult();
}
