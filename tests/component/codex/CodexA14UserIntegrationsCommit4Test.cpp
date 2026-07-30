/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/ClientOperationCodec.h"
#include "ai/openai/codex/detail/PluginCodec.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "ai/openai/codex/detail/ThreadCodec.h"
#include "ai/openai/codex/typed/Client.h"
#include "support/TestResult.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace codex = ai::openai::codex;
    namespace detail = ai::openai::codex::detail;
    namespace typed = ai::openai::codex::typed;

    codex::Json appSummary() {
        return {
            {"category", nullptr},
            {"description", "Synthetic application"},
            {"id", "synthetic-app"},
            {"installUrl", "https://example.invalid/install"},
            {"name", "Synthetic App"},
            {"futureAppField", true},
        };
    }

    codex::Json installResult(std::string authPolicy = "ON_INSTALL") {
        return {
            {"appsNeedingAuth", codex::Json::array({appSummary()})},
            {"authPolicy", std::move(authPolicy)},
            {"futureResponseField", true},
        };
    }

    codex::Json checkoutResult() {
        return {
            {"marketplaceName", "synthetic-marketplace"},
            {"marketplacePath", "/synthetic/marketplaces/synthetic-marketplace"},
            {"pluginId", "synthetic-plugin"},
            {"pluginName", "Synthetic Plugin"},
            {"pluginPath", "/synthetic/plugins/synthetic-plugin"},
            {"remotePluginId", "synthetic-remote-plugin"},
            {"remoteVersion", nullptr},
            {"futureResponseField", true},
        };
    }

    codex::Json principal(std::string role = "reader") {
        return {
            {"name", "Synthetic Principal"},
            {"principalId", "synthetic-principal"},
            {"principalType", "user"},
            {"role", std::move(role)},
            {"futurePrincipalField", true},
        };
    }

    void testRequestEncoding(tests::support::TestResult& result) {
        std::string error = "stale";

        typed::PluginInstallParams install{};
        install.marketplacePath = typed::OptionalNullable<typed::AbsolutePathBuf>::explicitNull();
        install.pluginName = "synthetic-plugin";
        install.remoteMarketplaceName = typed::OptionalNullable<std::string>::withValue("synthetic-marketplace");
        install.raw = {{"futureInstallField", true}};
        const auto encodedInstall = detail::encodePluginInstallParams(install, error);
        result.expectTrue(encodedInstall ==
                                  codex::Json{
                                      {"futureInstallField", true},
                                      {"marketplacePath", nullptr},
                                      {"pluginName", "synthetic-plugin"},
                                      {"remoteMarketplaceName", "synthetic-marketplace"},
                                  } &&
                              error.empty(),
                          "plugin/install preserves explicit null, a concrete optional value, and future fields");

        typed::PluginInstallParams omittedInstall{};
        omittedInstall.pluginName = "synthetic-plugin";
        const auto encodedOmittedInstall = detail::encodePluginInstallParams(omittedInstall, error);
        result.expectTrue(encodedOmittedInstall == codex::Json{{"pluginName", "synthetic-plugin"}} && error.empty(),
                          "plugin/install distinguishes omitted optional fields from explicit null");

        typed::PluginShareCheckoutParams checkout{};
        checkout.remotePluginId = "synthetic-remote-plugin";
        checkout.raw = {{"futureCheckoutField", true}};
        const auto encodedCheckout = detail::encodePluginShareCheckoutParams(checkout, error);
        result.expectTrue(encodedCheckout ==
                                  codex::Json{
                                      {"futureCheckoutField", true},
                                      {"remotePluginId", "synthetic-remote-plugin"},
                                  } &&
                              error.empty(),
                          "plugin/share/checkout encodes its required remote ID and preserves future fields");

        typed::PluginShareDeleteParams deleteParams{};
        deleteParams.remotePluginId = "synthetic-remote-plugin";
        deleteParams.raw = {{"futureDeleteField", true}};
        const auto encodedDelete = detail::encodePluginShareDeleteParams(deleteParams, error);
        result.expectTrue(encodedDelete ==
                                  codex::Json{
                                      {"futureDeleteField", true},
                                      {"remotePluginId", "synthetic-remote-plugin"},
                                  } &&
                              error.empty(),
                          "plugin/share/delete encodes its exact required parameter");

        typed::PluginShareTarget target{};
        target.principalId = "synthetic-principal";
        target.principalType = typed::PluginSharePrincipalType::user();
        target.role = typed::PluginShareTargetRole::editor();
        target.raw = {{"futureTargetField", true}};

        typed::PluginShareSaveParams save{};
        save.discoverability =
            typed::OptionalNullable<typed::PluginShareDiscoverability>::withValue(typed::PluginShareDiscoverability::unlisted());
        save.pluginPath = typed::AbsolutePathBuf{"/synthetic/plugins/synthetic-plugin"};
        save.remotePluginId = typed::OptionalNullable<std::string>::explicitNull();
        save.shareTargets =
            typed::OptionalNullable<std::vector<typed::PluginShareTarget>>::withValue(std::vector<typed::PluginShareTarget>{target});
        save.raw = {{"futureSaveField", true}};
        const auto encodedSave = detail::encodePluginShareSaveParams(save, error);
        result.expectTrue(encodedSave ==
                                  codex::Json{
                                      {"discoverability", "UNLISTED"},
                                      {"futureSaveField", true},
                                      {"pluginPath", "/synthetic/plugins/synthetic-plugin"},
                                      {"remotePluginId", nullptr},
                                      {"shareTargets",
                                       codex::Json::array({{{"futureTargetField", true},
                                                            {"principalId", "synthetic-principal"},
                                                            {"principalType", "user"},
                                                            {"role", "editor"}}})},
                                  } &&
                              error.empty(),
                          "plugin/share/save preserves nullable states, ordered targets, open enums, and nested future fields");

        typed::PluginShareSaveParams nullTargets{};
        nullTargets.pluginPath = typed::AbsolutePathBuf{"/synthetic/plugins/synthetic-plugin"};
        nullTargets.shareTargets = typed::OptionalNullable<std::vector<typed::PluginShareTarget>>::explicitNull();
        const auto encodedNullTargets = detail::encodePluginShareSaveParams(nullTargets, error);
        result.expectTrue(encodedNullTargets ==
                                  codex::Json{
                                      {"pluginPath", "/synthetic/plugins/synthetic-plugin"},
                                      {"shareTargets", nullptr},
                                  } &&
                              error.empty(),
                          "plugin/share/save distinguishes null targets from omitted targets");

        typed::PluginShareUpdateTargetsParams update{};
        update.discoverability = typed::PluginShareUpdateDiscoverability::privateVisibility();
        update.remotePluginId = "synthetic-remote-plugin";
        update.shareTargets = {target};
        update.raw = {{"futureUpdateField", true}};
        const auto encodedUpdate = detail::encodePluginShareUpdateTargetsParams(update, error);
        result.expectTrue(encodedUpdate ==
                                  codex::Json{
                                      {"discoverability", "PRIVATE"},
                                      {"futureUpdateField", true},
                                      {"remotePluginId", "synthetic-remote-plugin"},
                                      {"shareTargets",
                                       codex::Json::array({{{"futureTargetField", true},
                                                            {"principalId", "synthetic-principal"},
                                                            {"principalType", "user"},
                                                            {"role", "editor"}}})},
                                  } &&
                              error.empty(),
                          "plugin/share/updateTargets emits its exact required fields and preserves open-object extensions");

        typed::PluginSkillReadParams readSkill{};
        readSkill.remoteMarketplaceName = "synthetic-marketplace";
        readSkill.remotePluginId = "synthetic-remote-plugin";
        readSkill.skillName = "synthetic-skill";
        readSkill.raw = {{"futureReadSkillField", true}};
        const auto encodedReadSkill = detail::encodePluginSkillReadParams(readSkill, error);
        result.expectTrue(encodedReadSkill ==
                                  codex::Json{
                                      {"futureReadSkillField", true},
                                      {"remoteMarketplaceName", "synthetic-marketplace"},
                                      {"remotePluginId", "synthetic-remote-plugin"},
                                      {"skillName", "synthetic-skill"},
                                  } &&
                              error.empty(),
                          "plugin/skill/read preserves all three required identifiers and future fields");

        typed::PluginUninstallParams uninstall{};
        uninstall.pluginId = "synthetic-plugin";
        uninstall.raw = {{"futureUninstallField", true}};
        const auto encodedUninstall = detail::encodePluginUninstallParams(uninstall, error);
        result.expectTrue(encodedUninstall ==
                                  codex::Json{
                                      {"futureUninstallField", true},
                                      {"pluginId", "synthetic-plugin"},
                                  } &&
                              error.empty(),
                          "plugin/uninstall encodes its exact required plugin ID");

        typed::PluginInstallParams invalidState{};
        invalidState.pluginName = "synthetic-plugin";
        invalidState.marketplacePath = {false, std::optional<typed::AbsolutePathBuf>{typed::AbsolutePathBuf{"/synthetic/inconsistent"}}};
        result.expectTrue(!detail::encodePluginInstallParams(invalidState, error) && error.find("$.marketplacePath") != std::string::npos &&
                              error.find("/synthetic/inconsistent") == std::string::npos,
                          "plugin/install rejects an inconsistent nullable state without disclosing its value");

        typed::PluginUninstallParams invalidRaw{};
        invalidRaw.pluginId = "synthetic-plugin";
        invalidRaw.raw = false;
        result.expectTrue(!detail::encodePluginUninstallParams(invalidRaw, error) && error.find("$.raw") != std::string::npos,
                          "plugin encoders reject a non-object raw preservation carrier synchronously");
    }

    void testResultDecoding(tests::support::TestResult& result) {
        std::string error;

        const codex::Json installWire = installResult();
        const auto installed = detail::decodePluginInstallResponse(installWire, error);
        const bool installComplete =
            installed && installed->appsNeedingAuth.size() == 1 && installed->appsNeedingAuth.front().id == "synthetic-app" &&
            installed->appsNeedingAuth.front().category.present && !installed->appsNeedingAuth.front().category.value &&
            installed->appsNeedingAuth.front().installUrl.hasValue() &&
            *installed->appsNeedingAuth.front().installUrl == "https://example.invalid/install" &&
            installed->appsNeedingAuth.front().raw == installWire.at("appsNeedingAuth").at(0) &&
            installed->authPolicy == typed::PluginAuthPolicy::onInstall() && installed->raw == installWire &&
            installed->diagnostics.empty() && error.empty();
        result.expectTrue(installComplete, "plugin/install decodes its full nested application closure and retains open-object fields");

        const std::string unknownPolicy = "SYNTHETIC_FUTURE_POLICY";
        const auto futurePolicy = detail::decodePluginInstallResponse(installResult(unknownPolicy), error);
        const bool safePolicyDiagnostic =
            futurePolicy && futurePolicy->authPolicy.value == unknownPolicy && !futurePolicy->authPolicy.isKnown() &&
            futurePolicy->diagnostics.size() == 1 && futurePolicy->diagnostics.front().kind == typed::DecodeIssueKind::UnknownEnumValue &&
            futurePolicy->diagnostics.front().severity == typed::DecodeIssueSeverity::ForwardCompatibility &&
            futurePolicy->diagnostics.front().fieldPath == "$.authPolicy" &&
            futurePolicy->diagnostics.front().message.find(unknownPolicy) == std::string::npos && error.empty();
        result.expectTrue(safePolicyDiagnostic,
                          "plugin/install retains an unknown auth policy with a value-free ForwardCompatibility diagnostic");

        const codex::Json checkoutWire = checkoutResult();
        const auto checkedOut = detail::decodePluginShareCheckoutResponse(checkoutWire, error);
        result.expectTrue(checkedOut && checkedOut->marketplaceName == "synthetic-marketplace" &&
                              checkedOut->marketplacePath.value == "/synthetic/marketplaces/synthetic-marketplace" &&
                              checkedOut->pluginId == "synthetic-plugin" && checkedOut->pluginName == "Synthetic Plugin" &&
                              checkedOut->pluginPath.value == "/synthetic/plugins/synthetic-plugin" &&
                              checkedOut->remotePluginId == "synthetic-remote-plugin" && checkedOut->remoteVersion.isNull() &&
                              checkedOut->raw == checkoutWire && error.empty(),
                          "plugin/share/checkout decodes every stable path, ID, name, nullable version, and future field");

        codex::Json omittedVersionWire = checkoutWire;
        omittedVersionWire.erase("remoteVersion");
        const auto omittedVersion = detail::decodePluginShareCheckoutResponse(omittedVersionWire, error);
        result.expectTrue(omittedVersion && omittedVersion->remoteVersion.isOmitted() && error.empty(),
                          "plugin/share/checkout distinguishes an omitted version from null");
        codex::Json concreteVersionWire = checkoutWire;
        concreteVersionWire["remoteVersion"] = "1.2.3";
        const auto concreteVersion = detail::decodePluginShareCheckoutResponse(concreteVersionWire, error);
        result.expectTrue(concreteVersion && concreteVersion->remoteVersion.hasValue() && *concreteVersion->remoteVersion == "1.2.3" &&
                              error.empty(),
                          "plugin/share/checkout decodes a concrete optional version");

        const codex::Json saveWire{
            {"remotePluginId", "synthetic-remote-plugin"},
            {"shareUrl", "https://example.invalid/shares/synthetic-plugin"},
            {"futureResponseField", true},
        };
        const auto saved = detail::decodePluginShareSaveResponse(saveWire, error);
        result.expectTrue(saved && saved->remotePluginId == "synthetic-remote-plugin" &&
                              saved->shareUrl == "https://example.invalid/shares/synthetic-plugin" && saved->raw == saveWire &&
                              error.empty(),
                          "plugin/share/save decodes both required response fields and preserves future fields");

        const codex::Json updateWire{
            {"discoverability", "LISTED"},
            {"principals", codex::Json::array({principal()})},
            {"futureResponseField", true},
        };
        const auto updated = detail::decodePluginShareUpdateTargetsResponse(updateWire, error);
        result.expectTrue(updated && updated->discoverability == typed::PluginShareDiscoverability::listed() &&
                              updated->principals.size() == 1 && updated->principals.front().name == "Synthetic Principal" &&
                              updated->principals.front().principalId == "synthetic-principal" &&
                              updated->principals.front().principalType == typed::PluginSharePrincipalType::user() &&
                              updated->principals.front().role == typed::PluginSharePrincipalRole::reader() &&
                              updated->principals.front().raw == updateWire.at("principals").at(0) && updated->raw == updateWire &&
                              updated->diagnostics.empty() && error.empty(),
                          "plugin/share/updateTargets decodes required open enums, principals, and future fields");

        const std::string unknownRole = "synthetic-future-role";
        codex::Json futureRoleWire = updateWire;
        futureRoleWire["principals"][0] = principal(unknownRole);
        const auto futureRole = detail::decodePluginShareUpdateTargetsResponse(futureRoleWire, error);
        result.expectTrue(futureRole && futureRole->principals.front().role.value == unknownRole &&
                              !futureRole->principals.front().role.isKnown() && futureRole->diagnostics.size() == 1 &&
                              futureRole->diagnostics.front().fieldPath == "$.principals[0].role" &&
                              futureRole->diagnostics.front().message.find(unknownRole) == std::string::npos && error.empty(),
                          "nested unknown principal roles remain nonfatal and diagnostics disclose no principal value");

        const codex::Json skillOmittedWire{
            {"futureResponseField", true},
        };
        const auto skillOmitted = detail::decodePluginSkillReadResponse(skillOmittedWire, error);
        result.expectTrue(skillOmitted && skillOmitted->contents.isOmitted() && skillOmitted->raw == skillOmittedWire && error.empty(),
                          "plugin/skill/read preserves an omitted contents field");
        const codex::Json skillNullWire{{"contents", nullptr}};
        const auto skillNull = detail::decodePluginSkillReadResponse(skillNullWire, error);
        result.expectTrue(skillNull && skillNull->contents.isNull() && error.empty(),
                          "plugin/skill/read distinguishes explicit-null contents");
        const codex::Json skillValueWire{{"contents", "Synthetic skill contents"}};
        const auto skillValue = detail::decodePluginSkillReadResponse(skillValueWire, error);
        result.expectTrue(skillValue && skillValue->contents.hasValue() && *skillValue->contents == "Synthetic skill contents" &&
                              error.empty(),
                          "plugin/skill/read decodes concrete contents");

        result.expectTrue(detail::decodeUnitResult(codex::Json::object(), error).has_value() && error.empty(),
                          "plugin/share/delete and plugin/uninstall accept the exact empty-object Unit result");
        result.expectTrue(!detail::decodeUnitResult(nullptr, error) && !error.empty(), "plugin Unit operations reject JSON null");
        result.expectTrue(!detail::decodeUnitResult({{"unexpected", true}}, error) && !error.empty(),
                          "plugin Unit operations reject a non-empty object");

        result.expectTrue(!detail::decodePluginInstallResponse({{"appsNeedingAuth", codex::Json::array()}}, error) &&
                              error.find("$.authPolicy") != std::string::npos,
                          "plugin/install rejects a missing required result field");
        codex::Json wrongApp = installWire;
        wrongApp["appsNeedingAuth"][0]["id"] = false;
        result.expectTrue(!detail::decodePluginInstallResponse(wrongApp, error) &&
                              error.find("$.appsNeedingAuth[0].id") != std::string::npos,
                          "plugin/install rejects a wrong-typed nested application field");
        codex::Json wrongVersion = checkoutWire;
        wrongVersion["remoteVersion"] = false;
        result.expectTrue(!detail::decodePluginShareCheckoutResponse(wrongVersion, error) &&
                              error.find("$.remoteVersion") != std::string::npos,
                          "plugin/share/checkout rejects a wrong-typed nullable version");
        result.expectTrue(!detail::decodePluginShareSaveResponse({{"remotePluginId", "synthetic"}}, error) &&
                              error.find("$.shareUrl") != std::string::npos,
                          "plugin/share/save rejects a missing required URL");
        codex::Json wrongPrincipal = updateWire;
        wrongPrincipal["principals"][0]["principalType"] = false;
        result.expectTrue(!detail::decodePluginShareUpdateTargetsResponse(wrongPrincipal, error) &&
                              error.find("$.principals[0].principalType") != std::string::npos,
                          "plugin/share/updateTargets rejects a malformed known nested principal");
        result.expectTrue(!detail::decodePluginSkillReadResponse({{"contents", false}}, error) &&
                              error.find("$.contents") != std::string::npos,
                          "plugin/skill/read rejects wrong-typed concrete contents");

        const detail::ProtocolSurfaceEntry* checkoutRow =
            detail::findSurface(detail::SurfaceCategory::ClientRequest, "ClientRequest", "method", "plugin/share/checkout");
        const detail::ClientRequestTarget* checkoutTarget =
            checkoutRow == nullptr ? nullptr : std::get_if<detail::ClientRequestTarget>(&checkoutRow->runtimeTarget);
        const auto wrongAssociation = checkoutTarget == nullptr ? detail::ClientOperationDecodeResult{}
                                                                : detail::decodeClientOperationResult(*checkoutTarget, installWire);
        result.expectTrue(checkoutTarget != nullptr && !wrongAssociation &&
                              wrongAssociation.diagnostic.code == detail::ClientOperationDecodeCode::MalformedKnownPayload &&
                              wrongAssociation.diagnostic.message.find("synthetic-app") == std::string::npos,
                          "a plugin/install result cannot decode under plugin/share/checkout and its diagnostic is value-free");
    }

    void testRegistryAndFacade(tests::support::TestResult& result) {
        constexpr std::array<std::string_view, 7> Commit4Requests{{
            "plugin/install",
            "plugin/share/checkout",
            "plugin/share/delete",
            "plugin/share/save",
            "plugin/share/updateTargets",
            "plugin/skill/read",
            "plugin/uninstall",
        }};
        constexpr std::array<std::string_view, 4> Commit5Requests{{
            "plugin/installed",
            "plugin/list",
            "plugin/read",
            "plugin/share/list",
        }};
        constexpr std::array<std::string_view, 33> ExpectedA14Complete{{
            "app/list",
            "app/list/updated",
            "externalAgentConfig/detect",
            "externalAgentConfig/import",
            "externalAgentConfig/import/completed",
            "externalAgentConfig/import/progress",
            "externalAgentConfig/import/readHistories",
            "feedback/upload",
            "hook/completed",
            "hook/started",
            "hooks/list",
            "marketplace/add",
            "marketplace/remove",
            "marketplace/upgrade",
            "plugin/install",
            "plugin/installed",
            "plugin/list",
            "plugin/read",
            "plugin/share/checkout",
            "plugin/share/delete",
            "plugin/share/list",
            "plugin/share/save",
            "plugin/share/updateTargets",
            "plugin/skill/read",
            "plugin/uninstall",
            "skills/changed",
            "skills/config/write",
            "skills/extraRoots/set",
            "skills/list",
            "git",
            "local",
            "npm",
            "remote",
        }};

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

        result.expectTrue(complete == 321 && partial == 4 && notImplemented == 14 && notApplicable == 48,
                          "A1.4b Commit 4 global registry arithmetic is exactly 321/4/14/48");
        result.expectTrue(nativeComplete == 41 && nativePartial == 1 && nativeNotImplemented == 14,
                          "A1.4b Commit 4 native A1.4 registry arithmetic is exactly 41/1/14");
        result.expectEqual(std::size_t{41}, completedA14.size(),
                           "A1.4b Commit 4 leaves exactly forty-one native A1.4 identities Complete");
        for (std::string_view identity : ExpectedA14Complete) {
            bool found = false;
            for (const std::string& actual : completedA14) {
                found = found || actual == identity;
            }
            result.expectTrue(found, std::string(identity) + " is in the exact staged Complete set");
        }

        for (std::string_view identity : Commit4Requests) {
            const detail::ProtocolSurfaceEntry* row =
                detail::findSurface(detail::SurfaceCategory::ClientRequest, "ClientRequest", "method", identity);
            bool descriptorFound = false;
            for (const detail::ClientOperationCodecDescriptor& descriptor : detail::clientOperationCodecDescriptors()) {
                descriptorFound = descriptorFound || descriptor.key.name == identity;
            }
            result.expectTrue(row != nullptr && row->typedSchemaStatus == detail::TypedSchemaStatus::Complete &&
                                  row->runtimeDisposition == detail::RuntimeDisposition::Typed &&
                                  std::holds_alternative<detail::ClientRequestTarget>(row->runtimeTarget) && descriptorFound,
                              std::string(identity) + " has one typed target and one result descriptor");
        }
        for (std::string_view identity : Commit5Requests) {
            const detail::ProtocolSurfaceEntry* row =
                detail::findSurface(detail::SurfaceCategory::ClientRequest, "ClientRequest", "method", identity);
            bool descriptorFound = false;
            for (const detail::ClientOperationCodecDescriptor& descriptor : detail::clientOperationCodecDescriptors()) {
                descriptorFound = descriptorFound || descriptor.key.name == identity;
            }
            result.expectTrue(row != nullptr && row->typedSchemaStatus == detail::TypedSchemaStatus::Complete &&
                                  row->runtimeDisposition == detail::RuntimeDisposition::Typed &&
                                  std::holds_alternative<detail::ClientRequestTarget>(row->runtimeTarget) && descriptorFound,
                              std::string(identity) + " has one typed target and one result descriptor");
        }
    }
} // namespace

int main() {
    using PluginsAccessor = typed::Plugins& (typed::Client::*) () noexcept;
    using ConstPluginsAccessor = const typed::Plugins& (typed::Client::*) () const noexcept;

    static_assert(
        std::is_same_v<decltype(&typed::Plugins::install),
                       typed::Plugins::Submission (typed::Plugins::*)(typed::PluginInstallParams, typed::Plugins::InstallResultHandler)>);
    static_assert(std::is_same_v<decltype(&typed::Plugins::shareCheckout),
                                 typed::Plugins::Submission (typed::Plugins::*)(typed::PluginShareCheckoutParams,
                                                                                typed::Plugins::ShareCheckoutResultHandler)>);
    static_assert(std::is_same_v<decltype(&typed::Plugins::shareDelete),
                                 typed::Plugins::Submission (typed::Plugins::*)(typed::PluginShareDeleteParams,
                                                                                typed::Plugins::ShareDeleteResultHandler)>);
    static_assert(std::is_same_v<decltype(&typed::Plugins::shareSave),
                                 typed::Plugins::Submission (typed::Plugins::*)(typed::PluginShareSaveParams,
                                                                                typed::Plugins::ShareSaveResultHandler)>);
    static_assert(std::is_same_v<decltype(&typed::Plugins::shareUpdateTargets),
                                 typed::Plugins::Submission (typed::Plugins::*)(typed::PluginShareUpdateTargetsParams,
                                                                                typed::Plugins::ShareUpdateTargetsResultHandler)>);
    static_assert(std::is_same_v<decltype(&typed::Plugins::readSkill),
                                 typed::Plugins::Submission (typed::Plugins::*)(typed::PluginSkillReadParams,
                                                                                typed::Plugins::ReadSkillResultHandler)>);
    static_assert(std::is_same_v<decltype(&typed::Plugins::uninstall),
                                 typed::Plugins::Submission (typed::Plugins::*)(typed::PluginUninstallParams,
                                                                                typed::Plugins::UninstallResultHandler)>);
    static_assert(std::is_same_v<decltype(static_cast<PluginsAccessor>(&typed::Client::plugins)), PluginsAccessor>);
    static_assert(std::is_same_v<decltype(static_cast<ConstPluginsAccessor>(&typed::Client::plugins)), ConstPluginsAccessor>);

    tests::support::TestResult result;
    testRequestEncoding(result);
    testResultDecoding(result);
    testRegistryAndFacade(result);
    return result.processResult();
}
