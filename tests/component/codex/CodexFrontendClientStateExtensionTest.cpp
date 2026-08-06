/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/detail/StateReducer.h"
#include "support/TestResult.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace client = ai::openai::codex::frontend::client;

    client::SessionInfo expandedSession(frontend::SequenceNumber sequence) {
        client::SessionInfo result;
        result.sessionId = "7";
        result.role = frontend::SessionRole::Observer;
        result.syncMode = frontend::SyncMode::Snapshot;
        result.serverCurrentSequence = sequence;
        result.selectedRepresentationCapabilities = {
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
        };
        return result;
    }

    frontend::Json stamp(std::string marker) {
        frontend::Json result = frontend::Json::object();
        result["generation"] = std::uint64_t{3};
        result["freshness"] = "current";
        result["vendorStamp"] = std::move(marker);
        return result;
    }

    frontend::Json truncation(std::string marker) {
        frontend::Json result = frontend::Json::object();
        result["truncated"] = false;
        result["omittedFields"] = frontend::Json::array();
        result["vendorTruncation"] = std::move(marker);
        return result;
    }

    frontend::Json collection(std::string marker) {
        frontend::Json result = frontend::Json::object();
        result["entries"] = frontend::Json::array();
        result["truncation"] = truncation(marker + "-truncation");
        result["vendorCollection"] = std::move(marker);
        return result;
    }

    frontend::Json expandedState(bool largeExtension = false) {
        frontend::Json state = frontend::Json::object();
        state["provider"] = frontend::Json::object();
        state["provider"]["lifecycle"] = "ready";
        state["provider"]["generation"] = std::uint64_t{2};
        state["provider"]["desiredRunning"] = true;
        state["provider"]["recovery"] = frontend::Json::object();
        state["provider"]["recovery"]["status"] = "waiting";
        state["provider"]["recovery"]["attempts"] = std::uint64_t{4};
        state["provider"]["recovery"]["vendorRecovery"] = "recovery-extension";
        state["provider"]["vendorProvider"] =
            largeExtension ? frontend::Json(std::string(8'192, 'x')) : frontend::Json("provider-extension");

        state["controller"] = frontend::Json::object();
        state["controller"]["controllerSessionId"] = "7";
        state["controller"]["present"] = true;
        state["controller"]["vendorController"] = "controller-extension";
        state["sessions"] = frontend::Json::array();
        state["sessions"].push_back(frontend::Json{{"sessionId", "7"}, {"role", "observer"}, {"vendorSession", "session-extension"}});
        state["threadList"] = frontend::Json{{"hasLoadedPage", true},
                                             {"complete", false},
                                             {"pagesLoaded", std::uint64_t{1}},
                                             {"stamp", stamp("thread-list-stamp-extension")},
                                             {"vendorThreadList", "thread-list-extension"}};

        state["accounts"] = frontend::Json::object();
        state["accounts"]["stamp"] = stamp("domain-stamp-extension");
        state["accounts"]["latestResults"] = frontend::Json::array();
        state["accounts"]["vendorDomain"] = "domain-extension";

        state["processes"] = collection("processes-extension");
        state["processes"]["entries"].push_back(
            frontend::Json{{"processHandle", "process-1"}, {"lifecycle", "running"}, {"stamp", stamp("process-stamp-extension")}});
        state["filesystemWatches"] = collection("watches-extension");
        state["fuzzySearches"] = collection("searches-extension");
        state["notices"] = collection("notices-extension");
        state["activities"] = collection("activities-extension");

        state["capacity"] = frontend::Json{{"sessions", std::uint64_t{1}}, {"vendorCapacity", "capacity-extension"}};
        state["truncation"] = truncation("state-truncation-extension");
        return state;
    }

    void testExtensionPreservation(tests::support::TestResult& result) {
        const frontend::SequenceNumber sequence{9};
        const client::SessionInfo session = expandedSession(sequence);
        const client::ProjectionFingerprintMetadata fingerprint{
            R"({"format":"snodec.codex-frontend.projection-fingerprint.v1","continuityKey":{"present":true,"value":"test"}})"};
        std::string error;
        const auto reduction = client::detail::StateReducer::snapshot(
            client::detail::StateReducer::initial(),
            frontend::Snapshot{sequence, expandedState()},
            session,
            1U << 20U,
            64,
            true,
            error,
            fingerprint);

        result.expectTrue(reduction.has_value() && error.empty(), "expanded state with bounded SafeDetailValue extensions is reduced");
        if (!reduction)
            return;

        const client::State& state = reduction->state;
        result.expectTrue(state.projectionFingerprintMetadata() == std::optional(fingerprint),
                          "immutable State retains the canonical non-secret projection fingerprint associated with its projection");
        const bool providerPreserved = state.provider().value &&
                                       state.provider().value->extensions.value("vendorProvider", "") == "provider-extension" &&
                                       state.provider().value->recovery.extensions.value("vendorRecovery", "") == "recovery-extension";
        result.expectTrue(providerPreserved, "provider and provider-recovery additive fields remain in typed compatibility extensions");

        const bool controllerAndSessionPreserved =
            state.controller().value && state.controller().value->extensions.value("vendorController", "") == "controller-extension" &&
            state.sessions().value && state.sessions().value->size() == 1 &&
            state.sessions().value->front().extensions.value("vendorSession", "") == "session-extension" && state.threadList().value &&
            state.threadList().value->extensions.value("vendorThreadList", "") == "thread-list-extension" &&
            state.threadList().value->stamp &&
            state.threadList().value->stamp->extensions.value("vendorStamp", "") == "thread-list-stamp-extension";
        result.expectTrue(controllerAndSessionPreserved,
                          "controller, session, and thread-list additive fields remain in their typed records");

        const bool stampsPreserved =
            state.accounts().value && state.accounts().value->projection.stamp &&
            state.accounts().value->projection.stamp->extensions.value("vendorStamp", "") == "domain-stamp-extension" &&
            state.processes().value && state.processes().value->entries.size() == 1 &&
            state.processes().value->entries.front().stamp.extensions.value("vendorStamp", "") == "process-stamp-extension";
        result.expectTrue(stampsPreserved, "SourceStamp additive fields survive domain and entity decoding");

        const bool collectionPreserved =
            state.processes().value && state.processes().value->extensions.value("vendorCollection", "") == "processes-extension" &&
            state.filesystemWatches().value &&
            state.filesystemWatches().value->extensions.value("vendorCollection", "") == "watches-extension" &&
            state.fuzzySearches().value && state.fuzzySearches().value->extensions.value("vendorCollection", "") == "searches-extension" &&
            state.notices().value && state.notices().value->extensions.value("vendorCollection", "") == "notices-extension" &&
            state.activities().value && state.activities().value->extensions.value("vendorCollection", "") == "activities-extension";
        result.expectTrue(collectionPreserved, "each expanded collection wrapper preserves its own additive fields");

        const bool truncationPreserved =
            state.truncation().value &&
            state.truncation().value->extensions.value("vendorTruncation", "") == "state-truncation-extension" && state.processes().value &&
            state.processes().value->truncation.extensions.value("vendorTruncation", "") == "processes-extension-truncation";
        result.expectTrue(truncationPreserved, "top-level and collection TruncationMetadata preserve additive fields");

        const frontend::Json serialized = client::detail::StateReducer::serializeForTesting(state);
        const bool serializationPreserved =
            serialized.at("provider").at("value").value("vendorProvider", "") == "provider-extension" &&
            serialized.at("provider").at("value").at("recovery").value("vendorRecovery", "") == "recovery-extension" &&
            serialized.at("controller").at("value").value("vendorController", "") == "controller-extension" &&
            serialized.at("sessions").at("value").at(0).value("vendorSession", "") == "session-extension" &&
            serialized.at("threadList").at("value").value("vendorThreadList", "") == "thread-list-extension" &&
            serialized.at("threadList").at("value").at("stamp").value("vendorStamp", "") == "thread-list-stamp-extension" &&
            serialized.at("accounts").at("value").at("stamp").value("vendorStamp", "") == "domain-stamp-extension" &&
            serialized.at("processes").at("value").value("vendorCollection", "") == "processes-extension" &&
            serialized.at("processes").at("value").at("truncation").value("vendorTruncation", "") == "processes-extension-truncation" &&
            serialized.at("truncation").at("value").value("vendorTruncation", "") == "state-truncation-extension" &&
            serialized.value("projectionFingerprint", "") == fingerprint.canonical;
        result.expectTrue(serializationPreserved, "the canonical test serialization includes every retained compatibility extension layer");
    }

    void testExtensionsCountTowardStateCapacity(tests::support::TestResult& result) {
        const frontend::SequenceNumber sequence{11};
        const client::SessionInfo session = expandedSession(sequence);
        std::string error;
        const auto baseline = client::detail::StateReducer::snapshot(
            client::detail::StateReducer::initial(), frontend::Snapshot{sequence, expandedState()}, session, 7'500, 64, true, error);
        result.expectTrue(baseline.has_value(), "the compact typed state fits below the focused capacity bound");

        error.clear();
        const auto oversized = client::detail::StateReducer::snapshot(
            client::detail::StateReducer::initial(), frontend::Snapshot{sequence, expandedState(true)}, session, 7'500, 64, true, error);
        result.expectTrue(!oversized && error.find("maximumDecodedStateBytes") != std::string::npos,
                          "retained SafeDetailValue extension bytes participate in transactional state-capacity accounting: " + error);

        error.clear();
        const auto oversizedFingerprint = client::detail::StateReducer::snapshot(
            client::detail::StateReducer::initial(),
            frontend::Snapshot{sequence, expandedState()},
            session,
            7'500,
            64,
            true,
            error,
            client::ProjectionFingerprintMetadata{std::string(8'192, 'f')});
        result.expectTrue(!oversizedFingerprint && error.find("maximumDecodedStateBytes") != std::string::npos,
                          "projection-fingerprint metadata participates in transactional state-capacity accounting: " + error);
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testExtensionPreservation(result);
    testExtensionsCountTowardStateCapacity(result);
    return result.processResult();
}
