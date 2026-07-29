/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/backend/Reducer.h"
#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/detail/AppCodec.h"
#include "ai/openai/codex/detail/ClientOperationCodec.h"
#include "ai/openai/codex/detail/EventDecoder.h"
#include "ai/openai/codex/detail/ExternalAgentCodec.h"
#include "ai/openai/codex/detail/FeedbackCodec.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "ai/openai/codex/typed/Client.h"
#include "support/TestResult.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace codex = ai::openai::codex;
    namespace detail = ai::openai::codex::detail;
    namespace typed = ai::openai::codex::typed;

    codex::Notification notification(std::string method, codex::Json params) {
        codex::Json raw{
            {"jsonrpc", "2.0"},
            {"method", method},
            {"params", params},
            {"futureEnvelopeField", true},
        };
        return {std::move(method), std::move(params), std::move(raw)};
    }

    codex::Json appInfo(std::string id = "synthetic-app") {
        return {
            {"id", std::move(id)},
            {"name", "Synthetic App"},
            {"futureAppField", {{"safe", true}}},
        };
    }

    codex::Json migrationItem() {
        return {
            {"description", "Synthetic configuration"},
            {"itemType", "CONFIG"},
            {"futureMigrationField", true},
        };
    }

    codex::Json importTypeResult() {
        return {
            {"failures", codex::Json::array()},
            {"itemType", "CONFIG"},
            {"successes", codex::Json::array()},
            {"futureResultField", true},
        };
    }

    bool malformedEventAt(const typed::Event& event, const codex::Notification& wire, std::string_view path) {
        const auto* unknown = std::get_if<typed::UnknownEvent>(&event);
        return unknown != nullptr && unknown->method == wire.method && unknown->params == wire.params && unknown->raw == wire.raw &&
               unknown->decodingError.has_value() && unknown->diagnostic.has_value() &&
               unknown->diagnostic->kind == typed::DecodeIssueKind::MalformedKnownPayload &&
               unknown->diagnostic->severity == typed::DecodeIssueSeverity::ProtocolWarning &&
               unknown->diagnostic->surface == wire.method && unknown->diagnostic->fieldPath == path;
    }

    void testRequestEncoding(tests::support::TestResult& result) {
        std::string error = "stale";

        const auto omittedApps = detail::encodeAppsListParams({}, error);
        result.expectTrue(omittedApps == codex::Json::object() && error.empty(), "app/list omits every omitted optional parameter");

        typed::AppsListParams appNulls{};
        appNulls.cursor = typed::OptionalNullable<std::string>::explicitNull();
        appNulls.limit = typed::OptionalNullable<std::uint32_t>::explicitNull();
        appNulls.threadId = typed::OptionalNullable<typed::ThreadId>::explicitNull();
        appNulls.forceRefetch = false;
        const auto encodedAppNulls = detail::encodeAppsListParams(appNulls, error);
        result.expectTrue(encodedAppNulls ==
                                  codex::Json{
                                      {"cursor", nullptr},
                                      {"forceRefetch", false},
                                      {"limit", nullptr},
                                      {"threadId", nullptr},
                                  } &&
                              error.empty(),
                          "app/list distinguishes explicit null from omission and preserves false");

        typed::AppsListParams appValues{};
        appValues.cursor = typed::OptionalNullable<std::string>::withValue("cursor-a");
        appValues.forceRefetch = true;
        appValues.limit = typed::OptionalNullable<std::uint32_t>::withValue(std::numeric_limits<std::uint32_t>::max());
        appValues.threadId = typed::OptionalNullable<typed::ThreadId>::withValue(typed::ThreadId{"synthetic-thread"});
        const auto encodedAppValues = detail::encodeAppsListParams(appValues, error);
        result.expectTrue(encodedAppValues ==
                                  codex::Json{
                                      {"cursor", "cursor-a"},
                                      {"forceRefetch", true},
                                      {"limit", std::numeric_limits<std::uint32_t>::max()},
                                      {"threadId", "synthetic-thread"},
                                  } &&
                              error.empty(),
                          "app/list encodes exact values and the uint32 boundary");

        const auto omittedDetect = detail::encodeExternalAgentConfigDetectParams({}, error);
        result.expectTrue(omittedDetect == codex::Json::object() && error.empty(),
                          "externalAgentConfig/detect omits its absent parameters");

        typed::ExternalAgentConfigDetectParams detect{};
        detect.cwds = typed::OptionalNullable<std::vector<std::string>>::explicitNull();
        detect.includeHome = false;
        const auto encodedDetect = detail::encodeExternalAgentConfigDetectParams(detect, error);
        result.expectTrue(encodedDetect == codex::Json{{"cwds", nullptr}, {"includeHome", false}} && error.empty(),
                          "externalAgentConfig/detect preserves null cwd lists and false");

        typed::ExternalAgentConfigImportParams importParams{};
        importParams.source = typed::OptionalNullable<std::string>::explicitNull();
        const auto encodedEmptyImport = detail::encodeExternalAgentConfigImportParams(importParams, error);
        result.expectTrue(encodedEmptyImport ==
                                  codex::Json{
                                      {"migrationItems", codex::Json::array()},
                                      {"source", nullptr},
                                  } &&
                              error.empty(),
                          "externalAgentConfig/import keeps its required empty array and explicit-null source");

        typed::ExternalAgentConfigMigrationItem item{};
        item.description = "Synthetic configuration";
        item.itemType = typed::ExternalAgentConfigMigrationItemType::config();
        importParams.migrationItems.push_back(std::move(item));
        importParams.source = typed::OptionalNullable<std::string>::withValue("synthetic-agent");
        const auto encodedImport = detail::encodeExternalAgentConfigImportParams(importParams, error);
        result.expectTrue(
            encodedImport ==
                    codex::Json{
                        {"migrationItems", codex::Json::array({{{"description", "Synthetic configuration"}, {"itemType", "CONFIG"}}})},
                        {"source", "synthetic-agent"},
                    } &&
                error.empty(),
            "externalAgentConfig/import encodes its exact required item fields");

        typed::FeedbackUploadParams feedback{};
        feedback.classification = "bug";
        feedback.extraLogFiles = typed::OptionalNullable<std::vector<std::string>>::explicitNull();
        feedback.includeLogs = false;
        feedback.reason = typed::OptionalNullable<std::string>::explicitNull();
        feedback.tags = typed::OptionalNullable<std::map<std::string, std::string>>::withValue({{"kind", "synthetic"}});
        feedback.threadId = typed::OptionalNullable<typed::ThreadId>::withValue(typed::ThreadId{"synthetic-thread"});
        const auto encodedFeedback = detail::encodeFeedbackUploadParams(feedback, error);
        result.expectTrue(encodedFeedback ==
                                  codex::Json{
                                      {"classification", "bug"},
                                      {"extraLogFiles", nullptr},
                                      {"includeLogs", false},
                                      {"reason", nullptr},
                                      {"tags", {{"kind", "synthetic"}}},
                                      {"threadId", "synthetic-thread"},
                                  } &&
                              error.empty(),
                          "feedback/upload preserves exact omission, null, map, and Boolean semantics");

        typed::FeedbackUploadParams minimalFeedback{};
        minimalFeedback.classification = "bug";
        const auto encodedMinimalFeedback = detail::encodeFeedbackUploadParams(minimalFeedback, error);
        result.expectTrue(encodedMinimalFeedback == codex::Json{{"classification", "bug"}} && error.empty(),
                          "feedback/upload emits only its required classification when optional fields are omitted");
    }

    void testResultDecoding(tests::support::TestResult& result) {
        std::string error;

        const codex::Json appsWire{
            {"data", codex::Json::array({appInfo()})},
            {"nextCursor", nullptr},
            {"futureResponseField", true},
        };
        const auto apps = detail::decodeAppsListResponse(appsWire, error);
        result.expectTrue(apps && apps->data.size() == 1 && apps->data.front().id == "synthetic-app" &&
                              apps->data.front().name == "Synthetic App" && apps->data.front().raw == appsWire.at("data").at(0) &&
                              apps->nextCursor.present && !apps->nextCursor.value && apps->raw == appsWire && error.empty(),
                          "app/list decodes required fields, explicit null, and future fields");

        const codex::Json detectWire{
            {"items", codex::Json::array({migrationItem()})},
            {"futureResponseField", 1},
        };
        const auto detected = detail::decodeExternalAgentConfigDetectResponse(detectWire, error);
        result.expectTrue(detected && detected->items.size() == 1 && detected->items.front().description == "Synthetic configuration" &&
                              detected->items.front().itemType.value == "CONFIG" &&
                              detected->items.front().raw == detectWire.at("items").at(0) && detected->raw == detectWire && error.empty(),
                          "externalAgentConfig/detect decodes its complete stable root and retains raw future fields");

        const codex::Json importWire{
            {"importId", "synthetic-import"},
            {"futureResponseField", true},
        };
        const auto imported = detail::decodeExternalAgentConfigImportResponse(importWire, error);
        result.expectTrue(imported && imported->importId == "synthetic-import" && imported->raw == importWire && error.empty(),
                          "externalAgentConfig/import decodes the exact import identifier");

        const codex::Json historiesWire{
            {"data", codex::Json::array()},
            {"futureResponseField", true},
        };
        const auto histories = detail::decodeExternalAgentConfigImportHistoriesReadResponse(historiesWire, error);
        result.expectTrue(histories && histories->data.empty() && histories->raw == historiesWire && error.empty(),
                          "externalAgentConfig/import/readHistories preserves its required empty data array");

        const codex::Json feedbackWire{
            {"threadId", "synthetic-thread"},
            {"futureResponseField", true},
        };
        const auto feedback = detail::decodeFeedbackUploadResponse(feedbackWire, error);
        result.expectTrue(feedback && feedback->threadId.value == "synthetic-thread" && feedback->raw == feedbackWire && error.empty(),
                          "feedback/upload decodes the exact thread identifier and retains raw future fields");

        const auto appOperation = detail::decodeClientOperationResult(detail::ClientRequestTarget::AppsList, appsWire);
        const auto detectOperation =
            detail::decodeClientOperationResult(detail::ClientRequestTarget::ExternalAgentConfigDetect, detectWire);
        const auto importOperation =
            detail::decodeClientOperationResult(detail::ClientRequestTarget::ExternalAgentConfigImport, importWire);
        const auto historiesOperation =
            detail::decodeClientOperationResult(detail::ClientRequestTarget::ExternalAgentConfigImportHistoriesRead, historiesWire);
        const auto feedbackOperation = detail::decodeClientOperationResult(detail::ClientRequestTarget::FeedbackUpload, feedbackWire);
        result.expectTrue(appOperation && std::holds_alternative<typed::AppsListResponse>(*appOperation.value) && detectOperation &&
                              std::holds_alternative<typed::ExternalAgentConfigDetectResponse>(*detectOperation.value) && importOperation &&
                              std::holds_alternative<typed::ExternalAgentConfigImportResponse>(*importOperation.value) &&
                              historiesOperation &&
                              std::holds_alternative<typed::ExternalAgentConfigImportHistoriesReadResponse>(*historiesOperation.value) &&
                              feedbackOperation && std::holds_alternative<typed::FeedbackUploadResponse>(*feedbackOperation.value),
                          "all five Commit-2 result decoders are reachable through exact registry targets");

        const auto missingApps = detail::decodeAppsListResponse(codex::Json::object(), error);
        result.expectTrue(!missingApps && error.find("$.data") != std::string::npos, "app/list rejects a missing required response field");
        const auto wrongAppId =
            detail::decodeAppsListResponse({{"data", codex::Json::array({{{"id", false}, {"name", "Synthetic App"}}})}}, error);
        result.expectTrue(!wrongAppId && error.find("$.data[0].id") != std::string::npos,
                          "app/list rejects a wrong-typed nested required field");
        const auto wrongDetect = detail::decodeExternalAgentConfigDetectResponse({{"items", codex::Json::object()}}, error);
        result.expectTrue(!wrongDetect && error.find("$.items") != std::string::npos,
                          "externalAgentConfig/detect rejects a wrong-typed required array");
        const auto missingImport = detail::decodeExternalAgentConfigImportResponse(codex::Json::object(), error);
        result.expectTrue(!missingImport && error.find("$.importId") != std::string::npos,
                          "externalAgentConfig/import rejects a missing import identifier");
        const auto wrongHistories = detail::decodeExternalAgentConfigImportHistoriesReadResponse({{"data", false}}, error);
        result.expectTrue(!wrongHistories && error.find("$.data") != std::string::npos,
                          "externalAgentConfig/import/readHistories rejects a wrong-typed data array");
        const auto wrongFeedback = detail::decodeFeedbackUploadResponse({{"threadId", 1}}, error);
        result.expectTrue(!wrongFeedback && error.find("$.threadId") != std::string::npos,
                          "feedback/upload rejects a wrong-typed thread identifier");

        const auto wrongAssociation = detail::decodeClientOperationResult(detail::ClientRequestTarget::FeedbackUpload, appsWire);
        result.expectTrue(
            !wrongAssociation && wrongAssociation.diagnostic.code == detail::ClientOperationDecodeCode::MalformedKnownPayload &&
                wrongAssociation.diagnostic.surfaceKey.name == "feedback/upload" && wrongAssociation.diagnostic.fieldPath == "$.threadId",
            "result dispatch rejects a structurally valid result associated with the wrong operation");
    }

    void testNotifications(tests::support::TestResult& result) {
        std::string error;
        const codex::Notification apps = notification("app/list/updated",
                                                      {
                                                          {"data", codex::Json::array({appInfo()})},
                                                          {"futureParam", true},
                                                      });
        const auto decodedApps = detail::decodeAppListUpdatedNotification(apps, error);
        const typed::Event appEvent = detail::decodeEvent(apps);
        const auto* appAlternative = std::get_if<typed::AppListUpdatedNotification>(&appEvent);
        result.expectTrue(decodedApps && decodedApps->data.size() == 1 && decodedApps->raw == apps.raw && error.empty() &&
                              appAlternative != nullptr && appAlternative->raw == apps.raw &&
                              appAlternative->data.front().raw == apps.params.at("data").at(0),
                          "app/list/updated decodes completely at its appended typed Event alternative");

        const codex::Notification completed = notification("externalAgentConfig/import/completed",
                                                           {
                                                               {"importId", "synthetic-import"},
                                                               {"itemTypeResults", codex::Json::array({importTypeResult()})},
                                                               {"futureParam", true},
                                                           });
        const auto decodedCompleted = detail::decodeExternalAgentConfigImportCompletedNotification(completed, error);
        const typed::Event completedEvent = detail::decodeEvent(completed);
        const auto* completedAlternative = std::get_if<typed::ExternalAgentConfigImportCompletedNotification>(&completedEvent);
        result.expectTrue(decodedCompleted && decodedCompleted->importId == "synthetic-import" &&
                              decodedCompleted->itemTypeResults.size() == 1 && decodedCompleted->raw == completed.raw && error.empty() &&
                              completedAlternative != nullptr && completedAlternative->raw == completed.raw,
                          "externalAgentConfig/import/completed retains its complete raw envelope and stable fields");

        const codex::Notification progress = notification("externalAgentConfig/import/progress",
                                                          {
                                                              {"importId", "synthetic-import"},
                                                              {"itemTypeResults", codex::Json::array()},
                                                              {"futureParam", true},
                                                          });
        const auto decodedProgress = detail::decodeExternalAgentConfigImportProgressNotification(progress, error);
        const typed::Event progressEvent = detail::decodeEvent(progress);
        const auto* progressAlternative = std::get_if<typed::ExternalAgentConfigImportProgressNotification>(&progressEvent);
        result.expectTrue(decodedProgress && decodedProgress->importId == "synthetic-import" && decodedProgress->itemTypeResults.empty() &&
                              decodedProgress->raw == progress.raw && error.empty() && progressAlternative != nullptr &&
                              progressAlternative->raw == progress.raw,
                          "externalAgentConfig/import/progress retains its complete raw envelope and stable fields");

        codex::Notification malformedApps = apps;
        malformedApps.params["data"] = false;
        malformedApps.raw["params"]["data"] = false;
        result.expectTrue(malformedEventAt(detail::decodeEvent(malformedApps), malformedApps, "$.params.data"),
                          "malformed known app/list/updated degrades nonfatally at the exact field path");

        codex::Notification malformedCompleted = completed;
        malformedCompleted.params.erase("importId");
        malformedCompleted.raw["params"].erase("importId");
        result.expectTrue(malformedEventAt(detail::decodeEvent(malformedCompleted), malformedCompleted, "$.params.importId"),
                          "malformed known import/completed degrades nonfatally at the exact field path");

        codex::Notification malformedProgress = progress;
        malformedProgress.params["itemTypeResults"] = false;
        malformedProgress.raw["params"]["itemTypeResults"] = false;
        result.expectTrue(malformedEventAt(detail::decodeEvent(malformedProgress), malformedProgress, "$.params.itemTypeResults"),
                          "malformed known import/progress degrades nonfatally at the exact field path");
    }

    void testRegistryAndFacade(tests::support::TestResult& result) {
        std::size_t complete = 0;
        std::size_t partial = 0;
        std::size_t notImplemented = 0;
        std::size_t notApplicable = 0;
        std::size_t nativeComplete = 0;
        std::size_t nativePartial = 0;
        std::size_t nativeNotImplemented = 0;
        std::vector<std::string> completedA14;

        for (const detail::ProtocolSurfaceEntry& entry : detail::protocolSurfaceRegistry()) {
            switch (entry.typedSchemaStatus) {
                case detail::TypedSchemaStatus::Complete:
                    ++complete;
                    break;
                case detail::TypedSchemaStatus::Partial:
                    ++partial;
                    break;
                case detail::TypedSchemaStatus::NotImplemented:
                    ++notImplemented;
                    break;
                case detail::TypedSchemaStatus::NotApplicable:
                    ++notApplicable;
                    break;
            }
            if (entry.a1Slice == detail::A1Slice::A1_4) {
                switch (entry.typedSchemaStatus) {
                    case detail::TypedSchemaStatus::Complete:
                        ++nativeComplete;
                        completedA14.emplace_back(entry.key.name);
                        break;
                    case detail::TypedSchemaStatus::Partial:
                        ++nativePartial;
                        break;
                    case detail::TypedSchemaStatus::NotImplemented:
                        ++nativeNotImplemented;
                        break;
                    case detail::TypedSchemaStatus::NotApplicable:
                        break;
                }
            }
        }

        result.expectTrue(complete == 313 && partial == 4 && notImplemented == 22 && notApplicable == 48,
                          "final PR-A global registry arithmetic is exactly 313/4/22/48");
        result.expectTrue(nativeComplete == 33 && nativePartial == 1 && nativeNotImplemented == 22,
                          "final PR-A native A1.4 registry arithmetic is exactly 33/1/22");

        const std::array<std::string_view, 8> expectedCompleted{{
            "app/list",
            "app/list/updated",
            "externalAgentConfig/detect",
            "externalAgentConfig/import",
            "externalAgentConfig/import/completed",
            "externalAgentConfig/import/progress",
            "externalAgentConfig/import/readHistories",
            "feedback/upload",
        }};
        for (std::string_view identity : expectedCompleted) {
            bool found = false;
            for (const std::string& actual : completedA14) {
                if (actual == identity) {
                    found = true;
                    break;
                }
            }
            result.expectTrue(found, std::string(identity) + " is one of the exact Commit-2 Complete identities");
        }
    }

    void testBackendBoundary(tests::support::TestResult& result) {
        struct Case {
            const char* method;
            codex::Json params;
            std::array<const char*, 2> redactedFields;
            std::size_t redactedFieldCount = 0;
        };

        const std::array<Case, 3> cases{{
            {
                "app/list/updated",
                {
                    {"data", codex::Json::array({appInfo("sensitive-app")})},
                    {"futureSafeField", true},
                },
                {{"data", ""}},
                1,
            },
            {
                "externalAgentConfig/import/completed",
                {
                    {"importId", "sensitive-import"},
                    {"itemTypeResults", codex::Json::array({importTypeResult()})},
                    {"futureSafeField", true},
                },
                {{"importId", "itemTypeResults"}},
                2,
            },
            {
                "externalAgentConfig/import/progress",
                {
                    {"importId", "sensitive-import"},
                    {"itemTypeResults", codex::Json::array()},
                    {"futureSafeField", true},
                },
                {{"importId", "itemTypeResults"}},
                2,
            },
        }};

        backend::Reducer reducer;
        backend::BackendState state;
        const backend::Snapshot before = backend::makeSnapshot(state);

        for (const Case& testCase : cases) {
            const typed::Event event = detail::decodeEvent(notification(testCase.method, testCase.params));
            const std::vector<backend::BackendEvent> translated = reducer.translate(event);
            const auto* extension = translated.size() == 1 ? std::get_if<backend::CodexExtensionReceived>(&translated.front()) : nullptr;
            result.expectTrue(extension != nullptr && extension->method == testCase.method && extension->payload == testCase.params,
                              std::string(testCase.method) + " uses the existing exact params-only extension boundary");
            if (extension != nullptr) {
                reducer.apply(state, *extension);
            }
        }

        const backend::Snapshot snapshot = backend::makeSnapshot(state);
        result.expectEqual(
            cases.size(), snapshot.recentExtensions.size(), "all three Commit-2 notifications retain extension delivery order");
        if (snapshot.recentExtensions.size() == cases.size()) {
            for (std::size_t index = 0; index < cases.size(); ++index) {
                const Case& testCase = cases[index];
                const backend::ExtensionSnapshot& extension = snapshot.recentExtensions[index];
                bool exact = extension.method == testCase.method && extension.sensitiveFieldsRedacted &&
                             extension.payload.value("futureSafeField", false);
                for (std::size_t fieldIndex = 0; fieldIndex < testCase.redactedFieldCount; ++fieldIndex) {
                    exact = exact && extension.payload.value(testCase.redactedFields[fieldIndex], "") == "[redacted]";
                }
                result.expectTrue(exact, std::string(testCase.method) + " redacts only its reviewed sensitive payload fields");
            }
            const std::string frontendBytes =
                codex::Json{
                    snapshot.recentExtensions[0].payload,
                    snapshot.recentExtensions[1].payload,
                    snapshot.recentExtensions[2].payload,
                }
                    .dump();
            result.expectTrue(frontendBytes.find("sensitive-app") == std::string::npos &&
                                  frontendBytes.find("sensitive-import") == std::string::npos,
                              "frontend-compatible extension bytes contain no synthetic user-integration values");
        }

        backend::Snapshot withoutExtensions = snapshot;
        withoutExtensions.recentExtensions.clear();
        withoutExtensions.omittedRecentExtensions = 0;
        result.expectTrue(withoutExtensions == before && state.threads.empty() && state.threadOrder.empty() &&
                              state.pendingRequests.empty(),
                          "user-integration notifications add no backend product state");

        backend::ExtensionRecord unrelated{};
        unrelated.method = "future/extension";
        unrelated.payload = {
            {"data", "ordinary-data"},
            {"importId", "ordinary-import"},
            {"itemTypeResults", "ordinary-results"},
        };
        const backend::ExtensionSnapshot unrelatedSnapshot = backend::makeExtensionSnapshot(unrelated);
        result.expectTrue(!unrelatedSnapshot.sensitiveFieldsRedacted && unrelatedSnapshot.payload == unrelated.payload,
                          "Commit-2 redaction is scoped to the three exact notification methods");
    }
} // namespace

int main() {
    using AppsAccessor = typed::Apps& (typed::Client::*) () noexcept;
    using ConstAppsAccessor = const typed::Apps& (typed::Client::*) () const noexcept;
    using ExternalAgentsAccessor = typed::ExternalAgents& (typed::Client::*) () noexcept;
    using ConstExternalAgentsAccessor = const typed::ExternalAgents& (typed::Client::*) () const noexcept;
    using FeedbackAccessor = typed::Feedback& (typed::Client::*) () noexcept;
    using ConstFeedbackAccessor = const typed::Feedback& (typed::Client::*) () const noexcept;

    static_assert(std::variant_size_v<typed::CanonicalServerNotification> == 57);
    static_assert(std::variant_size_v<typed::Event> == 59);
    static_assert(std::is_same_v<std::variant_alternative_t<51, typed::CanonicalServerNotification>, typed::AppListUpdatedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<52, typed::CanonicalServerNotification>,
                                 typed::ExternalAgentConfigImportCompletedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<53, typed::CanonicalServerNotification>,
                                 typed::ExternalAgentConfigImportProgressNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<53, typed::Event>, typed::AppListUpdatedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<54, typed::Event>, typed::ExternalAgentConfigImportCompletedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<55, typed::Event>, typed::ExternalAgentConfigImportProgressNotification>);

    static_assert(std::is_same_v<decltype(&typed::Apps::list),
                                 typed::Apps::Submission (typed::Apps::*)(typed::AppsListParams, typed::Apps::ListResultHandler)>);
    static_assert(std::is_same_v<decltype(&typed::ExternalAgents::detect),
                                 typed::ExternalAgents::Submission (typed::ExternalAgents::*)(typed::ExternalAgentConfigDetectParams,
                                                                                              typed::ExternalAgents::DetectResultHandler)>);
    static_assert(std::is_same_v<decltype(&typed::ExternalAgents::importConfiguration),
                                 typed::ExternalAgents::Submission (typed::ExternalAgents::*)(
                                     typed::ExternalAgentConfigImportParams, typed::ExternalAgents::ImportConfigurationResultHandler)>);
    static_assert(std::is_same_v<decltype(&typed::ExternalAgents::readImportHistories),
                                 typed::ExternalAgents::Submission (typed::ExternalAgents::*)(
                                     typed::Unit, typed::ExternalAgents::ReadImportHistoriesResultHandler)>);
    static_assert(std::is_same_v<decltype(&typed::Feedback::upload),
                                 typed::Feedback::Submission (typed::Feedback::*)(typed::FeedbackUploadParams,
                                                                                  typed::Feedback::UploadResultHandler)>);
    static_assert(std::is_same_v<decltype(static_cast<AppsAccessor>(&typed::Client::apps)), AppsAccessor>);
    static_assert(std::is_same_v<decltype(static_cast<ConstAppsAccessor>(&typed::Client::apps)), ConstAppsAccessor>);
    static_assert(std::is_same_v<decltype(static_cast<ExternalAgentsAccessor>(&typed::Client::externalAgents)), ExternalAgentsAccessor>);
    static_assert(
        std::is_same_v<decltype(static_cast<ConstExternalAgentsAccessor>(&typed::Client::externalAgents)), ConstExternalAgentsAccessor>);
    static_assert(std::is_same_v<decltype(static_cast<FeedbackAccessor>(&typed::Client::feedback)), FeedbackAccessor>);
    static_assert(std::is_same_v<decltype(static_cast<ConstFeedbackAccessor>(&typed::Client::feedback)), ConstFeedbackAccessor>);

    tests::support::TestResult result;
    testRequestEncoding(result);
    testResultDecoding(result);
    testNotifications(result);
    testRegistryAndFacade(result);
    testBackendBoundary(result);
    return result.processResult();
}
