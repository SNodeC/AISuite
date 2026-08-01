/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/Api.h"
#include "ai/openai/codex/backend/Reducer.h"
#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/detail/EventDecoder.h"
#include "ai/openai/codex/detail/HookCodec.h"
#include "ai/openai/codex/detail/MarketplaceCodec.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "ai/openai/codex/detail/SkillCodec.h"
#include "ai/openai/codex/detail/ThreadCodec.h"
#include "support/TestResult.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
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

    codex::Json hookMetadata() {
        return {
            {"command", nullptr},
            {"currentHash", "synthetic-hash"},
            {"displayOrder", std::numeric_limits<std::int64_t>::min()},
            {"enabled", true},
            {"eventName", "preToolUse"},
            {"handlerType", "command"},
            {"isManaged", false},
            {"key", "synthetic-hook"},
            {"matcher", nullptr},
            {"pluginId", "synthetic-plugin"},
            {"source", "project"},
            {"sourcePath", "/synthetic/hooks/pre-tool"},
            {"statusMessage", nullptr},
            {"timeoutSec", std::numeric_limits<std::uint64_t>::max()},
            {"trustStatus", "trusted"},
            {"futureHookField", {{"safe", true}}},
        };
    }

    codex::Json hookRun(bool includeDefaultBearingSource) {
        codex::Json run{
            {"completedAt", nullptr},
            {"displayOrder", -1},
            {"durationMs", 25},
            {"entries", codex::Json::array({{{"kind", "feedback"}, {"text", "Synthetic hook output"}, {"futureOutputField", true}}})},
            {"eventName", "preToolUse"},
            {"executionMode", "sync"},
            {"handlerType", "command"},
            {"id", "synthetic-hook-run"},
            {"scope", "thread"},
            {"sourcePath", "/synthetic/hooks/pre-tool"},
            {"startedAt", std::numeric_limits<std::int64_t>::max()},
            {"status", "running"},
            {"statusMessage", nullptr},
            {"futureRunField", true},
        };
        if (includeDefaultBearingSource) {
            run["source"] = "unknown";
        }
        return run;
    }

    codex::Json hooksResult() {
        return {
            {"data",
             codex::Json::array(
                 {{{"cwd", "/synthetic/workspace"},
                   {"errors",
                    codex::Json::array(
                        {{{"message", "Synthetic hook warning"}, {"path", "/synthetic/hooks"}, {"futureErrorField", true}}})},
                   {"hooks", codex::Json::array({hookMetadata()})},
                   {"warnings", codex::Json::array({"Synthetic warning"})},
                   {"futureEntryField", true}}})},
            {"futureResponseField", true},
        };
    }

    codex::Json skillMetadata() {
        return {
            {"dependencies",
             {{"tools",
               codex::Json::array({{{"command", nullptr},
                                    {"description", "Synthetic dependency"},
                                    {"transport", nullptr},
                                    {"type", "mcp"},
                                    {"url", "https://example.invalid/tool"},
                                    {"value", "synthetic-tool"},
                                    {"futureDependencyField", true}}})},
              {"futureDependenciesField", true}}},
            {"description", "Synthetic skill"},
            {"enabled", true},
            {"interface",
             {{"brandColor", "#000000"},
              {"defaultPrompt", nullptr},
              {"displayName", "Synthetic Skill"},
              {"iconLarge", "/synthetic/skill/icon-large.png"},
              {"iconSmall", nullptr},
              {"shortDescription", "Synthetic"},
              {"futureInterfaceField", true}}},
            {"name", "synthetic-skill"},
            {"path", "/synthetic/skills/synthetic-skill"},
            {"scope", "future-scope"},
            {"shortDescription", nullptr},
            {"futureSkillField", true},
        };
    }

    codex::Json skillsResult() {
        return {
            {"data",
             codex::Json::array(
                 {{{"cwd", "/synthetic/workspace"},
                   {"errors",
                    codex::Json::array(
                        {{{"message", "Synthetic skill warning"}, {"path", "/synthetic/skills"}, {"futureErrorField", true}}})},
                   {"skills", codex::Json::array({skillMetadata()})},
                   {"futureEntryField", true}}})},
            {"futureResponseField", true},
        };
    }

    bool malformedEventAt(const typed::Event& event, const codex::Notification& wire, std::string_view path) {
        const auto* unknown = std::get_if<typed::UnknownEvent>(&event);
        return unknown != nullptr && unknown->method == wire.method && unknown->params == wire.params && unknown->raw == wire.raw &&
               unknown->diagnostic.has_value() && unknown->diagnostic->kind == typed::DecodeIssueKind::MalformedKnownPayload &&
               unknown->diagnostic->severity == typed::DecodeIssueSeverity::ProtocolWarning &&
               unknown->diagnostic->surface == wire.method && unknown->diagnostic->fieldPath == path;
    }

    void testRequestEncoding(tests::support::TestResult& result) {
        std::string error = "stale";

        const auto omittedHooks = detail::encodeHooksListParams({}, error);
        result.expectTrue(omittedHooks == codex::Json::object() && error.empty(),
                          "hooks/list omits its absent optional working-directory list");
        typed::HooksListParams hooks{};
        hooks.cwds = std::vector<std::string>{};
        hooks.raw = {{"futureHooksField", true}};
        const auto emptyHooks = detail::encodeHooksListParams(hooks, error);
        result.expectTrue(emptyHooks == codex::Json{{"cwds", codex::Json::array()}, {"futureHooksField", true}} && error.empty(),
                          "hooks/list distinguishes an explicit empty list and preserves open-object future fields");

        typed::MarketplaceAddParams add{};
        add.source = "https://example.invalid/synthetic-marketplace.git";
        add.refName = typed::OptionalNullable<std::string>::explicitNull();
        add.sparsePaths = typed::OptionalNullable<std::vector<std::string>>::withValue({"plugins", "skills"});
        add.raw = {{"futureMarketplaceAddField", true}};
        const auto encodedAdd = detail::encodeMarketplaceAddParams(add, error);
        result.expectTrue(encodedAdd ==
                                  codex::Json{
                                      {"futureMarketplaceAddField", true},
                                      {"refName", nullptr},
                                      {"source", "https://example.invalid/synthetic-marketplace.git"},
                                      {"sparsePaths", codex::Json::array({"plugins", "skills"})},
                                  } &&
                              error.empty(),
                          "marketplace/add preserves required source, explicit null, and ordered sparse paths");

        typed::MarketplaceRemoveParams remove{};
        remove.marketplaceName = "synthetic-marketplace";
        remove.raw = {{"futureMarketplaceRemoveField", true}};
        const auto encodedRemove = detail::encodeMarketplaceRemoveParams(remove, error);
        result.expectTrue(encodedRemove ==
                                  codex::Json{
                                      {"futureMarketplaceRemoveField", true},
                                      {"marketplaceName", "synthetic-marketplace"},
                                  } &&
                              error.empty(),
                          "marketplace/remove encodes its exact required name and preserves future fields");

        const auto omittedUpgrade = detail::encodeMarketplaceUpgradeParams({}, error);
        result.expectTrue(omittedUpgrade == codex::Json::object() && error.empty(),
                          "marketplace/upgrade omits an absent marketplace selector");
        typed::MarketplaceUpgradeParams upgrade{};
        upgrade.marketplaceName = typed::OptionalNullable<std::string>::explicitNull();
        upgrade.raw = {{"futureMarketplaceUpgradeField", true}};
        const auto nullUpgrade = detail::encodeMarketplaceUpgradeParams(upgrade, error);
        result.expectTrue(nullUpgrade ==
                                  codex::Json{
                                      {"futureMarketplaceUpgradeField", true},
                                      {"marketplaceName", nullptr},
                                  } &&
                              error.empty(),
                          "marketplace/upgrade distinguishes explicit null from omission and preserves future fields");

        typed::SkillsConfigWriteParams write{};
        write.enabled = false;
        write.name = typed::OptionalNullable<std::string>::explicitNull();
        write.path = typed::OptionalNullable<typed::AbsolutePath>::withValue(typed::AbsolutePath{"/synthetic/skills/synthetic-skill"});
        write.raw = {{"futureSkillsWriteField", true}};
        const auto encodedWrite = detail::encodeSkillsConfigWriteParams(write, error);
        result.expectTrue(encodedWrite ==
                                  codex::Json{
                                      {"enabled", false},
                                      {"futureSkillsWriteField", true},
                                      {"name", nullptr},
                                      {"path", "/synthetic/skills/synthetic-skill"},
                                  } &&
                              error.empty(),
                          "skills/config/write preserves false, explicit null, and a concrete path");

        typed::SkillsExtraRootsSetParams roots{};
        roots.extraRoots = {
            typed::AbsolutePath{"/synthetic/skills/one"},
            typed::AbsolutePath{"/synthetic/skills/two"},
        };
        roots.raw = {{"futureSkillsRootsField", true}};
        const auto encodedRoots = detail::encodeSkillsExtraRootsSetParams(roots, error);
        result.expectTrue(encodedRoots ==
                                  codex::Json{
                                      {"extraRoots", codex::Json::array({"/synthetic/skills/one", "/synthetic/skills/two"})},
                                      {"futureSkillsRootsField", true},
                                  } &&
                              error.empty(),
                          "skills/extraRoots/set preserves path ordering and future fields");

        typed::SkillsListParams skills{};
        skills.cwds = std::vector<std::string>{};
        skills.forceReload = false;
        skills.raw = {{"futureSkillsListField", true}};
        const auto encodedSkills = detail::encodeSkillsListParams(skills, error);
        result.expectTrue(encodedSkills ==
                                  codex::Json{
                                      {"cwds", codex::Json::array()},
                                      {"forceReload", false},
                                      {"futureSkillsListField", true},
                                  } &&
                              error.empty(),
                          "skills/list distinguishes explicit empty and false values and preserves future fields");
        const auto omittedSkills = detail::encodeSkillsListParams({}, error);
        result.expectTrue(omittedSkills == codex::Json::object() && error.empty(), "skills/list omits both absent optional fields");

        typed::HooksListParams invalidRaw{};
        invalidRaw.raw = false;
        result.expectTrue(!detail::encodeHooksListParams(invalidRaw, error) && error.find("$.raw") != std::string::npos,
                          "open-object encoders reject a non-object raw preservation carrier synchronously");
    }

    void testResultDecoding(tests::support::TestResult& result) {
        std::string error;

        const codex::Json hooksWire = hooksResult();
        const auto hooks = detail::decodeHooksListResponse(hooksWire, error);
        const bool hooksComplete = hooks && hooks->data.size() == 1 && hooks->data.front().hooks.size() == 1 &&
                                   hooks->data.front().hooks.front().displayOrder == std::numeric_limits<std::int64_t>::min() &&
                                   hooks->data.front().hooks.front().timeoutSec == std::numeric_limits<std::uint64_t>::max() &&
                                   hooks->data.front().hooks.front().command.present && !hooks->data.front().hooks.front().command.value &&
                                   hooks->data.front().hooks.front().raw == hooksWire.at("data").at(0).at("hooks").at(0) &&
                                   hooks->data.front().raw == hooksWire.at("data").at(0) && hooks->raw == hooksWire && error.empty();
        result.expectTrue(hooksComplete,
                          "hooks/list decodes every stable nested field, integer boundaries, nullability, and raw future fields");

        const codex::Json addWire{
            {"alreadyAdded", false},
            {"installedRoot", "/synthetic/marketplaces/synthetic-marketplace"},
            {"marketplaceName", "synthetic-marketplace"},
            {"futureResponseField", true},
        };
        const auto added = detail::decodeMarketplaceAddResponse(addWire, error);
        result.expectTrue(added && !added->alreadyAdded && added->installedRoot.value == "/synthetic/marketplaces/synthetic-marketplace" &&
                              added->marketplaceName == "synthetic-marketplace" && added->raw == addWire && error.empty(),
                          "marketplace/add decodes every stable result field and retains future fields");

        const codex::Json removeOmittedWire{
            {"marketplaceName", "synthetic-marketplace"},
            {"futureResponseField", true},
        };
        const auto removedOmitted = detail::decodeMarketplaceRemoveResponse(removeOmittedWire, error);
        result.expectTrue(removedOmitted && !removedOmitted->installedRoot.present && !removedOmitted->installedRoot.value.has_value() &&
                              removedOmitted->raw == removeOmittedWire && error.empty(),
                          "marketplace/remove preserves an omitted optional installedRoot");
        const codex::Json removeNullWire{
            {"installedRoot", nullptr},
            {"marketplaceName", "synthetic-marketplace"},
        };
        const auto removedNull = detail::decodeMarketplaceRemoveResponse(removeNullWire, error);
        result.expectTrue(removedNull && removedNull->installedRoot.present && !removedNull->installedRoot.value.has_value() &&
                              removedNull->raw == removeNullWire && error.empty(),
                          "marketplace/remove distinguishes explicit-null installedRoot from omission");
        const codex::Json removeValueWire{
            {"installedRoot", "/synthetic/marketplaces/synthetic-marketplace"},
            {"marketplaceName", "synthetic-marketplace"},
        };
        const auto removedValue = detail::decodeMarketplaceRemoveResponse(removeValueWire, error);
        result.expectTrue(removedValue && removedValue->installedRoot.present && removedValue->installedRoot.value.has_value() &&
                              removedValue->installedRoot.value->value == "/synthetic/marketplaces/synthetic-marketplace" && error.empty(),
                          "marketplace/remove decodes a concrete optional installedRoot");

        const codex::Json upgradeWire{
            {"errors",
             codex::Json::array(
                 {{{"marketplaceName", "synthetic-marketplace"}, {"message", "Synthetic upgrade warning"}, {"futureErrorField", true}}})},
            {"selectedMarketplaces", codex::Json::array({"synthetic-marketplace"})},
            {"upgradedRoots", codex::Json::array({"/synthetic/marketplaces/synthetic-marketplace"})},
            {"futureResponseField", true},
        };
        const auto upgraded = detail::decodeMarketplaceUpgradeResponse(upgradeWire, error);
        result.expectTrue(upgraded && upgraded->errors.size() == 1 && upgraded->errors.front().marketplaceName == "synthetic-marketplace" &&
                              upgraded->errors.front().raw == upgradeWire.at("errors").at(0) &&
                              upgraded->selectedMarketplaces == std::vector<std::string>{"synthetic-marketplace"} &&
                              upgraded->upgradedRoots.size() == 1 &&
                              upgraded->upgradedRoots.front().value == "/synthetic/marketplaces/synthetic-marketplace" &&
                              upgraded->raw == upgradeWire && error.empty(),
                          "marketplace/upgrade decodes arrays, nested errors, paths, and raw future fields");

        const codex::Json writeWire{
            {"effectiveEnabled", false},
            {"futureResponseField", true},
        };
        const auto written = detail::decodeSkillsConfigWriteResponse(writeWire, error);
        result.expectTrue(written && !written->effectiveEnabled && written->raw == writeWire && error.empty(),
                          "skills/config/write preserves a false concrete result and future fields");

        const codex::Json skillsWire = skillsResult();
        const auto skills = detail::decodeSkillsListResponse(skillsWire, error);
        const bool skillsComplete =
            skills && skills->data.size() == 1 && skills->data.front().skills.size() == 1 &&
            skills->data.front().skills.front().scope.value == "future-scope" && !skills->data.front().skills.front().scope.isKnown() &&
            skills->data.front().skills.front().dependencies.present &&
            skills->data.front().skills.front().dependencies.value.has_value() &&
            skills->data.front().skills.front().dependencies.value->tools.size() == 1 &&
            skills->data.front().skills.front().interface.present && skills->data.front().skills.front().interface.value.has_value() &&
            skills->data.front().skills.front().interface.value->defaultPrompt.present &&
            !skills->data.front().skills.front().interface.value->defaultPrompt.value.has_value() &&
            skills->data.front().skills.front().raw == skillsWire.at("data").at(0).at("skills").at(0) && skills->raw == skillsWire &&
            !skills->diagnostics.empty() && skills->diagnostics.front().kind == typed::DecodeIssueKind::UnknownEnumValue &&
            skills->diagnostics.front().severity == typed::DecodeIssueSeverity::ForwardCompatibility && error.empty();
        result.expectTrue(skillsComplete,
                          "skills/list decodes its transitive closure, retains future fields, and preserves an unknown open enum");

        const auto missingHooks = detail::decodeHooksListResponse(codex::Json::object(), error);
        result.expectTrue(!missingHooks && error.find("$.data") != std::string::npos, "hooks/list rejects a missing required result field");
        codex::Json negativeTimeout = hooksWire;
        negativeTimeout["data"][0]["hooks"][0]["timeoutSec"] = -1;
        const auto invalidTimeout = detail::decodeHooksListResponse(negativeTimeout, error);
        result.expectTrue(!invalidTimeout && error.find("$.data[0].hooks[0].timeoutSec") != std::string::npos,
                          "hooks/list rejects a uint64 underflow at its exact structural path");
        codex::Json overflowDisplayOrder = hooksWire;
        overflowDisplayOrder["data"][0]["hooks"][0]["displayOrder"] = std::numeric_limits<std::uint64_t>::max();
        const auto invalidDisplayOrder = detail::decodeHooksListResponse(overflowDisplayOrder, error);
        result.expectTrue(!invalidDisplayOrder && error.find("$.data[0].hooks[0].displayOrder") != std::string::npos,
                          "hooks/list rejects an int64 overflow at its exact structural path");
        const auto invalidAdd =
            detail::decodeMarketplaceAddResponse({{"alreadyAdded", false}, {"installedRoot", "/synthetic/root"}}, error);
        result.expectTrue(!invalidAdd && error.find("$.marketplaceName") != std::string::npos,
                          "marketplace/add rejects a missing required name");
        const auto invalidRemove =
            detail::decodeMarketplaceRemoveResponse({{"installedRoot", false}, {"marketplaceName", "synthetic-marketplace"}}, error);
        result.expectTrue(!invalidRemove && error.find("$.installedRoot") != std::string::npos,
                          "marketplace/remove rejects a wrong-typed present nullable path");
        const auto invalidUpgrade = detail::decodeMarketplaceUpgradeResponse(
            {{"errors", codex::Json::array()}, {"selectedMarketplaces", codex::Json::array()}, {"upgradedRoots", false}}, error);
        result.expectTrue(!invalidUpgrade && error.find("$.upgradedRoots") != std::string::npos,
                          "marketplace/upgrade rejects a wrong-typed path array");
        const auto invalidWrite = detail::decodeSkillsConfigWriteResponse({{"effectiveEnabled", "false"}}, error);
        result.expectTrue(!invalidWrite && error.find("$.effectiveEnabled") != std::string::npos,
                          "skills/config/write rejects a wrong-typed Boolean");
        codex::Json invalidSkills = skillsWire;
        invalidSkills["data"][0]["skills"][0]["dependencies"]["tools"][0]["url"] = false;
        const auto wrongNestedSkill = detail::decodeSkillsListResponse(invalidSkills, error);
        result.expectTrue(!wrongNestedSkill && error.find("$.data[0].skills[0].dependencies.tools[0].url") != std::string::npos,
                          "skills/list rejects a wrong-typed nested nullable field");

        result.expectTrue(detail::decodeUnitResult(codex::Json::object(), error).has_value() && error.empty(),
                          "skills/extraRoots/set accepts the exact empty-object Unit result");
        result.expectTrue(!detail::decodeUnitResult(nullptr, error) && !error.empty(),
                          "skills/extraRoots/set rejects JSON null as a Unit result");
        result.expectTrue(!detail::decodeUnitResult({{"unexpected", true}}, error) && !error.empty(),
                          "skills/extraRoots/set rejects a non-empty object as a Unit result");
    }

    void testNotifications(tests::support::TestResult& result) {
        std::string error;
        const codex::Notification started = notification("hook/started",
                                                         {
                                                             {"run", hookRun(false)},
                                                             {"threadId", "synthetic-thread"},
                                                             {"turnId", nullptr},
                                                             {"futureParam", true},
                                                         });
        const auto decodedStarted = detail::decodeHookStartedNotification(started, error);
        const typed::Event startedEvent = detail::decodeEvent(started);
        const auto* startedAlternative = std::get_if<typed::HookStartedNotification>(&startedEvent);
        result.expectTrue(decodedStarted && decodedStarted->run.id == "synthetic-hook-run" &&
                              decodedStarted->run.startedAt == std::numeric_limits<std::int64_t>::max() &&
                              !decodedStarted->run.source.has_value() && decodedStarted->turnId.present &&
                              !decodedStarted->turnId.value.has_value() && decodedStarted->run.raw == started.params.at("run") &&
                              decodedStarted->raw == started.raw && error.empty() && startedAlternative != nullptr &&
                              startedAlternative->raw == started.raw,
                          "hook/started preserves null, raw fields, and omission of a default-bearing source");

        const codex::Notification completed = notification("hook/completed",
                                                           {
                                                               {"run", hookRun(true)},
                                                               {"threadId", "synthetic-thread"},
                                                               {"futureParam", true},
                                                           });
        const auto decodedCompleted = detail::decodeHookCompletedNotification(completed, error);
        const typed::Event completedEvent = detail::decodeEvent(completed);
        const auto* completedAlternative = std::get_if<typed::HookCompletedNotification>(&completedEvent);
        result.expectTrue(decodedCompleted && decodedCompleted->run.source.has_value() &&
                              decodedCompleted->run.source->value == "unknown" && !decodedCompleted->turnId.present &&
                              !decodedCompleted->turnId.value.has_value() && decodedCompleted->raw == completed.raw && error.empty() &&
                              completedAlternative != nullptr && completedAlternative->raw == completed.raw,
                          "hook/completed distinguishes an explicit schema-default value from omission and retains raw fields");

        const codex::Notification changed = notification(
            "skills/changed", {{"futureSkillPath", "/synthetic/private/skill"}, {"futureSkillName", "synthetic-private-skill"}});
        const auto decodedChanged = detail::decodeSkillsChangedNotification(changed, error);
        const typed::Event changedEvent = detail::decodeEvent(changed);
        const auto* changedAlternative = std::get_if<typed::SkillsChangedNotification>(&changedEvent);
        result.expectTrue(decodedChanged && decodedChanged->raw == changed.raw && error.empty() && changedAlternative != nullptr &&
                              changedAlternative->raw == changed.raw,
                          "skills/changed retains its open future payload in the canonical typed event");

        codex::Notification malformedStarted = started;
        malformedStarted.params["run"]["startedAt"] = std::numeric_limits<std::uint64_t>::max();
        malformedStarted.raw["params"]["run"]["startedAt"] = std::numeric_limits<std::uint64_t>::max();
        result.expectTrue(malformedEventAt(detail::decodeEvent(malformedStarted), malformedStarted, "$.params.run.startedAt"),
                          "malformed known hook/started degrades nonfatally at the exact overflow path");

        codex::Notification malformedCompleted = completed;
        malformedCompleted.params.erase("threadId");
        malformedCompleted.raw["params"].erase("threadId");
        result.expectTrue(malformedEventAt(detail::decodeEvent(malformedCompleted), malformedCompleted, "$.params.threadId"),
                          "malformed known hook/completed degrades nonfatally at the missing required path");

        codex::Notification malformedChanged = changed;
        malformedChanged.params = false;
        malformedChanged.raw["params"] = false;
        result.expectTrue(malformedEventAt(detail::decodeEvent(malformedChanged), malformedChanged, "$.params"),
                          "malformed known skills/changed degrades nonfatally when params is not an object");
    }

    void testRegistryBijection(tests::support::TestResult& result) {
        const std::array<std::string_view, 7> requests{{
            "hooks/list",
            "marketplace/add",
            "marketplace/remove",
            "marketplace/upgrade",
            "skills/config/write",
            "skills/extraRoots/set",
            "skills/list",
        }};
        const std::array<std::string_view, 3> notifications{{
            "hook/completed",
            "hook/started",
            "skills/changed",
        }};

        for (std::string_view identity : requests) {
            const detail::ProtocolSurfaceEntry* row =
                detail::findSurface(detail::SurfaceCategory::ClientRequest, "ClientRequest", "method", identity);
            bool descriptorFound = false;
            for (const detail::ClientOperationCodecDescriptor& descriptor : detail::clientOperationCodecDescriptors()) {
                descriptorFound = descriptorFound || descriptor.key.name == identity;
            }
            result.expectTrue(row != nullptr && row->typedSchemaStatus == detail::TypedSchemaStatus::Complete &&
                                  row->runtimeDisposition == detail::RuntimeDisposition::Typed &&
                                  std::holds_alternative<detail::ClientRequestTarget>(row->runtimeTarget) && descriptorFound,
                              std::string(identity) + " has a Complete typed registry row, runtime target, and descriptor");
        }
        for (std::string_view identity : notifications) {
            const detail::ProtocolSurfaceEntry* row =
                detail::findSurface(detail::SurfaceCategory::ServerNotification, "ServerNotification", "method", identity);
            bool descriptorFound = false;
            for (const detail::ServerNotificationCodecDescriptor& descriptor : detail::serverNotificationCodecDescriptors()) {
                descriptorFound = descriptorFound || descriptor.key.name == identity;
            }
            result.expectTrue(row != nullptr && row->typedSchemaStatus == detail::TypedSchemaStatus::Complete &&
                                  row->runtimeDisposition == detail::RuntimeDisposition::Typed &&
                                  std::holds_alternative<detail::ServerNotificationTarget>(row->runtimeTarget) && descriptorFound,
                              std::string(identity) + " has a Complete typed registry row, runtime target, and descriptor");
        }
    }

    void testBackendBoundary(tests::support::TestResult& result) {
        const std::string privateRun = "synthetic-private-hook-run";
        const std::string privateThread = "synthetic-private-thread";
        const std::string privateTurn = "synthetic-private-turn";
        const std::string privateSkillPath = "/synthetic/private/skill";
        const std::string privateSkillName = "synthetic-private-skill";

        codex::Json startedRun = hookRun(false);
        startedRun["id"] = privateRun;
        const std::array<codex::Notification, 3> wires{{
            notification("hook/started",
                         {{"run", startedRun}, {"threadId", privateThread}, {"turnId", privateTurn}, {"futureSafeField", "started-safe"}}),
            notification(
                "hook/completed",
                {{"run", startedRun}, {"threadId", privateThread}, {"turnId", privateTurn}, {"futureSafeField", "completed-safe"}}),
            notification(
                "skills/changed",
                {{"futureSkillPath", privateSkillPath}, {"futureSkillName", privateSkillName}, {"futureSafeField", "changed-safe"}}),
        }};

        backend::Reducer reducer;
        backend::BackendState state;
        const backend::Snapshot before = backend::makeSnapshot(state);
        for (const codex::Notification& wire : wires) {
            const typed::Event event = detail::decodeEvent(wire);
            const std::vector<backend::BackendEvent> translated = reducer.translate(event);
            const auto* extension = translated.size() == 1 ? std::get_if<backend::CodexExtensionReceived>(&translated.front()) : nullptr;
            result.expectTrue(extension != nullptr && extension->method == wire.method && extension->payload == wire.params,
                              wire.method + " uses the existing exact canonical extension boundary");
            if (extension != nullptr) {
                reducer.apply(state, *extension);
            }
        }

        const backend::Snapshot snapshot = backend::makeSnapshot(state);
        result.expectEqual(wires.size(),
                           snapshot.recentExtensions.size(),
                           "all three hooks/marketplace/skills notifications preserve extension delivery order");
        codex::Json frontendPayloads = codex::Json::array();
        for (const backend::ExtensionSnapshot& extension : snapshot.recentExtensions) {
            frontendPayloads.push_back(extension.payload);
        }
        const std::string frontendBytes = frontendPayloads.dump();
        result.expectTrue(
            snapshot.recentExtensions.size() == wires.size() && snapshot.recentExtensions[0].sensitiveFieldsRedacted &&
                snapshot.recentExtensions[1].sensitiveFieldsRedacted && snapshot.recentExtensions[2].sensitiveFieldsRedacted &&
                frontendBytes.find(privateRun) == std::string::npos && frontendBytes.find(privateThread) == std::string::npos &&
                frontendBytes.find(privateTurn) == std::string::npos && frontendBytes.find(privateSkillPath) == std::string::npos &&
                frontendBytes.find(privateSkillName) == std::string::npos,
            "frontend-compatible snapshots redact hook and open skills payload values");

        backend::Snapshot withoutExtensions = snapshot;
        withoutExtensions.recentExtensions.clear();
        withoutExtensions.omittedRecentExtensions = 0;
        result.expectTrue(withoutExtensions == before && state.threads.empty() && state.threadOrder.empty() &&
                              state.pendingRequests.empty(),
                          "hooks/marketplace/skills notifications add no backend product state");

        backend::ExtensionRecord unrelated{};
        unrelated.method = "future/extension";
        unrelated.payload = {
            {"run", privateRun},
            {"threadId", privateThread},
            {"turnId", privateTurn},
            {"futureSkillPath", privateSkillPath},
            {"futureSkillName", privateSkillName},
        };
        const backend::ExtensionSnapshot unrelatedSnapshot = backend::makeExtensionSnapshot(unrelated);
        result.expectTrue(!unrelatedSnapshot.sensitiveFieldsRedacted && unrelatedSnapshot.payload == unrelated.payload,
                          "hooks/marketplace/skills redaction is scoped to the exact notification methods");
    }
} // namespace

int main() {
    using HooksAccessor = typed::Hooks& (codex::AppServerClient::*) () noexcept;
    using MarketplaceAccessor = typed::Marketplace& (codex::AppServerClient::*) () noexcept;
    using SkillsAccessor = typed::Skills& (codex::AppServerClient::*) () noexcept;

    static_assert(std::variant_size_v<typed::CanonicalServerNotification> == 68);
    static_assert(std::variant_size_v<typed::Event> == 69);
    static_assert(std::is_same_v<std::variant_alternative_t<54, typed::CanonicalServerNotification>, typed::HookCompletedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<55, typed::CanonicalServerNotification>, typed::HookStartedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<56, typed::CanonicalServerNotification>, typed::SkillsChangedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<56, typed::Event>, typed::HookCompletedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<57, typed::Event>, typed::HookStartedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<58, typed::Event>, typed::SkillsChangedNotification>);

    using HooksListMember = codex::Submission (typed::Hooks::*)(typed::HooksListParams, typed::CompletionHandler<typed::HooksListResponse>);
    static_assert(std::is_same_v<decltype(static_cast<HooksListMember>(&typed::Hooks::list)), HooksListMember>);
    static_assert(std::is_same_v<decltype(&typed::Marketplace::add),
                                 codex::Submission (typed::Marketplace::*)(typed::MarketplaceAddParams,
                                                                           typed::CompletionHandler<typed::MarketplaceAddResponse>)>);
    static_assert(std::is_same_v<decltype(&typed::Marketplace::remove),
                                 codex::Submission (typed::Marketplace::*)(typed::MarketplaceRemoveParams,
                                                                           typed::CompletionHandler<typed::MarketplaceRemoveResponse>)>);
    using MarketplaceUpgradeMember = codex::Submission (typed::Marketplace::*)(typed::MarketplaceUpgradeParams,
                                                                               typed::CompletionHandler<typed::MarketplaceUpgradeResponse>);
    static_assert(std::is_same_v<decltype(static_cast<MarketplaceUpgradeMember>(&typed::Marketplace::upgrade)), MarketplaceUpgradeMember>);
    static_assert(std::is_same_v<decltype(&typed::Skills::writeConfig),
                                 codex::Submission (typed::Skills::*)(typed::SkillsConfigWriteParams,
                                                                      typed::CompletionHandler<typed::SkillsConfigWriteResponse>)>);
    static_assert(std::is_same_v<decltype(&typed::Skills::setExtraRoots),
                                 codex::Submission (typed::Skills::*)(typed::SkillsExtraRootsSetParams, typed::DoneHandler)>);
    using SkillsListMember =
        codex::Submission (typed::Skills::*)(typed::SkillsListParams, typed::CompletionHandler<typed::SkillsListResponse>);
    static_assert(std::is_same_v<decltype(static_cast<SkillsListMember>(&typed::Skills::list)), SkillsListMember>);
    static_assert(std::is_same_v<decltype(static_cast<HooksAccessor>(&codex::AppServerClient::hooks)), HooksAccessor>);
    static_assert(std::is_same_v<decltype(static_cast<MarketplaceAccessor>(&codex::AppServerClient::marketplace)), MarketplaceAccessor>);
    static_assert(std::is_same_v<decltype(static_cast<SkillsAccessor>(&codex::AppServerClient::skills)), SkillsAccessor>);

    tests::support::TestResult result;
    testRequestEncoding(result);
    testResultDecoding(result);
    testNotifications(result);
    testRegistryBijection(result);
    testBackendBoundary(result);
    return result.processResult();
}
