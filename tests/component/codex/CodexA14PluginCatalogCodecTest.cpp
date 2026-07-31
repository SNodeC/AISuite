/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/ClientOperationCodec.h"
#include "ai/openai/codex/detail/PluginCodec.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
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

    codex::Json pluginSource(std::string_view type) {
        if (type == "git") {
            return {
                {"type", "git"},
                {"url", "https://example.invalid/synthetic-plugin.git"},
                {"path", nullptr},
                {"refName", "synthetic-ref"},
                {"sha", "synthetic-sha"},
                {"futureSourceField", true},
            };
        }
        if (type == "local") {
            return {
                {"type", "local"},
                {"path", "/synthetic/plugins/synthetic-plugin"},
                {"futureSourceField", true},
            };
        }
        if (type == "npm") {
            return {
                {"type", "npm"},
                {"package", "synthetic-plugin"},
                {"registry", nullptr},
                {"version", "^1.0.0"},
                {"futureSourceField", true},
            };
        }
        return {
            {"type", "remote"},
            {"futureSourceField", true},
        };
    }

    codex::Json sharePrincipal() {
        return {
            {"name", "Synthetic Principal"},
            {"principalId", "synthetic-principal"},
            {"principalType", "user"},
            {"role", "owner"},
            {"futurePrincipalField", true},
        };
    }

    codex::Json pluginInterface() {
        return {
            {"brandColor", "#abcdef"},
            {"capabilities", codex::Json::array({"synthetic-capability"})},
            {"category", nullptr},
            {"composerIcon", "/synthetic/icons/composer.svg"},
            {"composerIconUrl", "https://example.invalid/composer.svg"},
            {"defaultPrompt", codex::Json::array({"Synthetic prompt"})},
            {"developerName", "Synthetic Developer"},
            {"displayName", "Synthetic Plugin"},
            {"logo", "/synthetic/icons/logo.svg"},
            {"logoDark", nullptr},
            {"logoUrl", "https://example.invalid/logo.svg"},
            {"logoUrlDark", nullptr},
            {"longDescription", "Synthetic long description"},
            {"privacyPolicyUrl", "https://example.invalid/privacy"},
            {"screenshotUrls", codex::Json::array({"https://example.invalid/screenshot.png"})},
            {"screenshots", codex::Json::array({"/synthetic/screenshots/screenshot.png"})},
            {"shortDescription", "Synthetic short description"},
            {"termsOfServiceUrl", "https://example.invalid/terms"},
            {"websiteUrl", "https://example.invalid"},
            {"futureInterfaceField", true},
        };
    }

    codex::Json pluginSummary(std::string_view sourceType = "git") {
        return {
            {"authPolicy", "ON_USE"},
            {"availability", "AVAILABLE"},
            {"enabled", true},
            {"id", "synthetic-plugin"},
            {"installPolicy", "INSTALLED_BY_DEFAULT"},
            {"installPolicySource", "WORKSPACE_SETTING"},
            {"installed", true},
            {"interface", pluginInterface()},
            {"keywords", codex::Json::array({"synthetic", "plugin"})},
            {"localVersion", nullptr},
            {"name", "Synthetic Plugin"},
            {"remotePluginId", "synthetic-remote-plugin"},
            {"shareContext",
             {
                 {"creatorAccountUserId", "synthetic-user"},
                 {"creatorName", "Synthetic Creator"},
                 {"discoverability", "UNLISTED"},
                 {"remotePluginId", "synthetic-remote-plugin"},
                 {"remoteVersion", nullptr},
                 {"sharePrincipals", codex::Json::array({sharePrincipal()})},
                 {"shareUrl", "https://example.invalid/shares/synthetic-plugin"},
                 {"futureShareContextField", true},
             }},
            {"source", pluginSource(sourceType)},
            {"version", "1.0.0"},
            {"futurePluginField", true},
        };
    }

    codex::Json marketplace(std::string_view sourceType = "git") {
        return {
            {"interface", {{"displayName", "Synthetic Marketplace"}, {"futureMarketplaceInterfaceField", true}}},
            {"name", "synthetic-marketplace"},
            {"path", "/synthetic/marketplaces/synthetic-marketplace.json"},
            {"plugins", codex::Json::array({pluginSummary(sourceType)})},
            {"futureMarketplaceField", true},
        };
    }

    codex::Json installedResponse(std::string_view sourceType = "git") {
        return {
            {"marketplaceLoadErrors",
             codex::Json::array({{{"marketplacePath", "/synthetic/marketplaces/broken.json"},
                                  {"message", "Synthetic load failure"},
                                  {"futureLoadErrorField", true}}})},
            {"marketplaces", codex::Json::array({marketplace(sourceType)})},
            {"futureInstalledResponseField", true},
        };
    }

    codex::Json listResponse(std::string_view sourceType = "npm") {
        codex::Json result = installedResponse(sourceType);
        result["featuredPluginIds"] = codex::Json::array({"synthetic-plugin"});
        result["futureListResponseField"] = true;
        return result;
    }

    codex::Json appSummary() {
        return {
            {"category", nullptr},
            {"description", "Synthetic app"},
            {"id", "synthetic-app"},
            {"installUrl", "https://example.invalid/install"},
            {"name", "Synthetic App"},
            {"futureAppField", true},
        };
    }

    codex::Json appTemplateSummary() {
        return {
            {"canonicalConnectorId", nullptr},
            {"category", "synthetic"},
            {"description", "Synthetic template"},
            {"logoUrl", "https://example.invalid/template.svg"},
            {"logoUrlDark", nullptr},
            {"materializedAppIds", codex::Json::array({"synthetic-app"})},
            {"name", "Synthetic Template"},
            {"reason", "NO_ACTIVE_WORKSPACE"},
            {"templateId", "synthetic-template"},
            {"futureTemplateField", true},
        };
    }

    codex::Json pluginDetail() {
        return {
            {"appTemplates", codex::Json::array({appTemplateSummary()})},
            {"apps", codex::Json::array({appSummary()})},
            {"description", nullptr},
            {"hooks", codex::Json::array({{{"eventName", "preToolUse"}, {"key", "synthetic-hook"}, {"futureHookSummaryField", true}}})},
            {"marketplaceName", "synthetic-marketplace"},
            {"marketplacePath", "/synthetic/marketplaces/synthetic-marketplace.json"},
            {"mcpServers", codex::Json::array({"synthetic-mcp"})},
            {"shareUrl", "https://example.invalid/shares/synthetic-plugin"},
            {"skills",
             codex::Json::array({{{"description", "Synthetic skill"},
                                  {"enabled", true},
                                  {"interface",
                                   {{"brandColor", "#abcdef"},
                                    {"defaultPrompt", "Synthetic prompt"},
                                    {"displayName", "Synthetic Skill"},
                                    {"iconLarge", "/synthetic/icons/skill-large.svg"},
                                    {"iconSmall", nullptr},
                                    {"shortDescription", "Synthetic skill description"},
                                    {"futureSkillInterfaceField", true}}},
                                  {"name", "synthetic-skill"},
                                  {"path", "/synthetic/skills/synthetic-skill/SKILL.md"},
                                  {"shortDescription", nullptr},
                                  {"futureSkillSummaryField", true}}})},
            {"summary", pluginSummary("remote")},
            {"futureDetailField", true},
        };
    }

    void testRequestEncoding(tests::support::TestResult& result) {
        std::string error = "stale";

        typed::PluginInstalledParams installed{};
        installed.cwds =
            typed::OptionalNullable<std::vector<typed::AbsolutePathBuf>>::withValue({typed::AbsolutePathBuf{"/synthetic/workspace"}});
        installed.installSuggestionPluginNames = typed::OptionalNullable<std::vector<std::string>>::explicitNull();
        installed.raw = {{"futureInstalledField", true}};
        const auto encodedInstalled = detail::encodePluginInstalledParams(installed, error);
        result.expectTrue(encodedInstalled ==
                                  codex::Json{
                                      {"cwds", codex::Json::array({"/synthetic/workspace"})},
                                      {"futureInstalledField", true},
                                      {"installSuggestionPluginNames", nullptr},
                                  } &&
                              error.empty(),
                          "plugin/installed encodes concrete/null states and preserves open-object fields");

        typed::PluginInstalledParams omittedInstalled{};
        const auto encodedOmittedInstalled = detail::encodePluginInstalledParams(omittedInstalled, error);
        result.expectTrue(encodedOmittedInstalled == codex::Json::object() && error.empty(),
                          "plugin/installed distinguishes omitted fields from explicit null");

        typed::PluginListParams list{};
        list.cwds = typed::OptionalNullable<std::vector<typed::AbsolutePathBuf>>::explicitNull();
        list.marketplaceKinds = typed::OptionalNullable<std::vector<typed::PluginListMarketplaceKind>>::withValue(
            {typed::PluginListMarketplaceKind::local(), typed::PluginListMarketplaceKind::workspaceDirectory()});
        list.raw = {{"futureListField", true}};
        const auto encodedList = detail::encodePluginListParams(list, error);
        result.expectTrue(encodedList ==
                                  codex::Json{
                                      {"cwds", nullptr},
                                      {"futureListField", true},
                                      {"marketplaceKinds", codex::Json::array({"local", "workspace-directory"})},
                                  } &&
                              error.empty(),
                          "plugin/list encodes exact marketplace discriminators, null cwd, and future fields");

        typed::PluginReadParams read{};
        read.marketplacePath = typed::OptionalNullable<typed::AbsolutePathBuf>::explicitNull();
        read.pluginName = "synthetic-plugin";
        read.remoteMarketplaceName = typed::OptionalNullable<std::string>::withValue("synthetic-marketplace");
        read.raw = {{"futureReadField", true}};
        const auto encodedRead = detail::encodePluginReadParams(read, error);
        result.expectTrue(encodedRead ==
                                  codex::Json{
                                      {"futureReadField", true},
                                      {"marketplacePath", nullptr},
                                      {"pluginName", "synthetic-plugin"},
                                      {"remoteMarketplaceName", "synthetic-marketplace"},
                                  } &&
                              error.empty(),
                          "plugin/read preserves exact omission/null/value semantics and its required name");

        typed::PluginShareListParams shareList{};
        shareList.raw = {{"futureShareListField", true}};
        const auto encodedShareList = detail::encodePluginShareListParams(shareList, error);
        result.expectTrue(encodedShareList == codex::Json{{"futureShareListField", true}} && error.empty(),
                          "plugin/share/list preserves its open empty-parameter object exactly");

        typed::PluginListParams invalid{};
        invalid.cwds = {
            false,
            std::optional<std::vector<typed::AbsolutePathBuf>>{{typed::AbsolutePathBuf{"/synthetic/sensitive-path"}}},
        };
        result.expectTrue(!detail::encodePluginListParams(invalid, error) && error.find("$.cwds") != std::string::npos &&
                              error.find("/synthetic/sensitive-path") == std::string::npos,
                          "C5 encoders reject inconsistent nullable state without exposing sensitive values");
    }

    void testPluginSource(tests::support::TestResult& result) {
        static_assert(std::variant_size_v<typed::PluginSource> == 5);
        static_assert(std::is_same_v<std::variant_alternative_t<0, typed::PluginSource>, typed::GitPluginSource>);
        static_assert(std::is_same_v<std::variant_alternative_t<1, typed::PluginSource>, typed::LocalPluginSource>);
        static_assert(std::is_same_v<std::variant_alternative_t<2, typed::PluginSource>, typed::NpmPluginSource>);
        static_assert(std::is_same_v<std::variant_alternative_t<3, typed::PluginSource>, typed::RemotePluginSource>);
        static_assert(std::is_same_v<std::variant_alternative_t<4, typed::PluginSource>, typed::UnknownPluginSource>);

        std::string error;
        const auto git = detail::decodePluginSource(pluginSource("git"), error);
        const auto local = detail::decodePluginSource(pluginSource("local"), error);
        const auto npm = detail::decodePluginSource(pluginSource("npm"), error);
        const auto remote = detail::decodePluginSource(pluginSource("remote"), error);
        const bool knownComplete =
            git && git->index() == 0 && std::get<typed::GitPluginSource>(*git).url == "https://example.invalid/synthetic-plugin.git" &&
            std::get<typed::GitPluginSource>(*git).path.isNull() && std::get<typed::GitPluginSource>(*git).refName.hasValue() &&
            std::get<typed::GitPluginSource>(*git).sha.hasValue() && typed::pluginSourceRaw(*git) == pluginSource("git") && local &&
            local->index() == 1 && std::get<typed::LocalPluginSource>(*local).path.value == "/synthetic/plugins/synthetic-plugin" && npm &&
            npm->index() == 2 && std::get<typed::NpmPluginSource>(*npm).package == "synthetic-plugin" &&
            std::get<typed::NpmPluginSource>(*npm).registry.isNull() && std::get<typed::NpmPluginSource>(*npm).version.hasValue() &&
            remote && remote->index() == 3 && typed::pluginSourceDiscriminator(*remote) == "remote" && error.empty();
        result.expectTrue(knownComplete, "PluginSource decodes every known alternative in exact registry-derived ABI order");

        const std::string futureType = "synthetic-sensitive-future-source";
        const codex::Json futureWire{
            {"type", futureType},
            {"opaque", {{"synthetic", true}}},
        };
        const auto future = detail::decodePluginSource(futureWire, error);
        const auto* futureUnknown = future ? std::get_if<typed::UnknownPluginSource>(&*future) : nullptr;
        result.expectTrue(futureUnknown != nullptr && futureUnknown->type == futureType && futureUnknown->raw == futureWire &&
                              futureUnknown->diagnostic &&
                              futureUnknown->diagnostic->kind == typed::DecodeIssueKind::UnknownDiscriminator &&
                              futureUnknown->diagnostic->severity == typed::DecodeIssueSeverity::ForwardCompatibility &&
                              futureUnknown->diagnostic->fieldPath == "$.type" &&
                              futureUnknown->diagnostic->message.find(futureType) == std::string::npos && error.empty(),
                          "future PluginSource retains discriminator/raw JSON with a value-free ForwardCompatibility diagnostic");

        const std::string sensitiveVersion = "synthetic-sensitive-version";
        const codex::Json malformedWire{
            {"type", "npm"},
            {"version", sensitiveVersion},
            {"opaque", true},
        };
        const auto malformed = detail::decodePluginSource(malformedWire, error);
        const auto* malformedUnknown = malformed ? std::get_if<typed::UnknownPluginSource>(&*malformed) : nullptr;
        result.expectTrue(malformedUnknown != nullptr && malformedUnknown->type == "npm" && malformedUnknown->raw == malformedWire &&
                              malformedUnknown->diagnostic &&
                              malformedUnknown->diagnostic->kind == typed::DecodeIssueKind::MalformedKnownPayload &&
                              malformedUnknown->diagnostic->severity == typed::DecodeIssueSeverity::ProtocolWarning &&
                              malformedUnknown->diagnostic->kind != typed::DecodeIssueKind::UnknownDiscriminator &&
                              malformedUnknown->diagnostic->fieldPath == "$.package" &&
                              malformedUnknown->diagnostic->message.find(sensitiveVersion) == std::string::npos && error.empty(),
                          "malformed-known npm remains distinct from future-unknown and discloses no payload values");
    }

    void testResultDecoding(tests::support::TestResult& result) {
        std::string error;

        const codex::Json installedWire = installedResponse();
        const auto installed = detail::decodePluginInstalledResponse(installedWire, error);
        const bool installedComplete =
            installed && installed->marketplaceLoadErrors && installed->marketplaceLoadErrors->size() == 1 &&
            installed->marketplaceLoadErrors->front().marketplacePath.value == "/synthetic/marketplaces/broken.json" &&
            installed->marketplaceLoadErrors->front().message == "Synthetic load failure" && installed->marketplaces.size() == 1 &&
            installed->marketplaces.front().interface.hasValue() && installed->marketplaces.front().interface->displayName.hasValue() &&
            installed->marketplaces.front().path.hasValue() && installed->marketplaces.front().plugins.size() == 1 &&
            installed->marketplaces.front().plugins.front().source.index() == 0 && installed->raw == installedWire &&
            installed->diagnostics.empty() && error.empty();
        result.expectTrue(installedComplete,
                          "plugin/installed decodes marketplace errors, metadata, catalog entries, and git source closure");

        const typed::PluginSummary* summary =
            installed && installed->marketplaces.size() == 1 && installed->marketplaces.front().plugins.size() == 1
                ? &installed->marketplaces.front().plugins.front()
                : nullptr;
        const bool summaryComplete =
            summary != nullptr && summary->authPolicy == typed::PluginAuthPolicy::onUse() && summary->availability &&
            summary->availability->isKnown() && summary->enabled && summary->id == "synthetic-plugin" &&
            summary->installPolicy == typed::PluginInstallPolicy::installedByDefault() && summary->installPolicySource.hasValue() &&
            summary->installed && summary->interface.hasValue() && summary->interface->brandColor.hasValue() &&
            summary->interface->capabilities.size() == 1 && summary->interface->category.isNull() &&
            summary->interface->composerIcon.hasValue() && summary->interface->composerIconUrl.hasValue() &&
            summary->interface->defaultPrompt.hasValue() && summary->interface->developerName.hasValue() &&
            summary->interface->displayName.hasValue() && summary->interface->logo.hasValue() && summary->interface->logoDark.isNull() &&
            summary->interface->logoUrl.hasValue() && summary->interface->logoUrlDark.isNull() &&
            summary->interface->longDescription.hasValue() && summary->interface->privacyPolicyUrl.hasValue() &&
            summary->interface->screenshotUrls.size() == 1 && summary->interface->screenshots.size() == 1 &&
            summary->interface->shortDescription.hasValue() && summary->interface->termsOfServiceUrl.hasValue() &&
            summary->interface->websiteUrl.hasValue() && summary->keywords && summary->keywords->size() == 2 &&
            summary->localVersion.isNull() && summary->name == "Synthetic Plugin" && summary->remotePluginId.hasValue() &&
            summary->shareContext.hasValue() && summary->shareContext->creatorAccountUserId.hasValue() &&
            summary->shareContext->creatorName.hasValue() && summary->shareContext->discoverability.hasValue() &&
            summary->shareContext->remotePluginId == "synthetic-remote-plugin" && summary->shareContext->remoteVersion.isNull() &&
            summary->shareContext->sharePrincipals.hasValue() && summary->shareContext->sharePrincipals->size() == 1 &&
            summary->shareContext->shareUrl.hasValue() && summary->version.hasValue() &&
            summary->raw == installedWire.at("marketplaces").at(0).at("plugins").at(0);
        result.expectTrue(summaryComplete, "PluginSummary and PluginInterface represent every stable field and wire state");

        const codex::Json listedWire = listResponse();
        const auto listed = detail::decodePluginListResponse(listedWire, error);
        result.expectTrue(listed && listed->featuredPluginIds && listed->featuredPluginIds->size() == 1 && listed->marketplaceLoadErrors &&
                              listed->marketplaces.size() == 1 && listed->marketplaces.front().plugins.size() == 1 &&
                              listed->marketplaces.front().plugins.front().source.index() == 2 && listed->raw == listedWire &&
                              error.empty(),
                          "plugin/list preserves explicit default-bearing arrays and decodes npm catalog sources");

        codex::Json omittedDefaultsWire{{"marketplaces", codex::Json::array()}};
        const auto omittedDefaults = detail::decodePluginListResponse(omittedDefaultsWire, error);
        result.expectTrue(omittedDefaults && !omittedDefaults->featuredPluginIds && !omittedDefaults->marketplaceLoadErrors &&
                              error.empty(),
                          "plugin/list does not invent schema defaults when fields are omitted");

        const codex::Json readWire{
            {"plugin", pluginDetail()},
            {"futureReadResponseField", true},
        };
        const auto read = detail::decodePluginReadResponse(readWire, error);
        const bool detailComplete =
            read && read->plugin.appTemplates.size() == 1 && read->plugin.appTemplates.front().canonicalConnectorId.isNull() &&
            read->plugin.appTemplates.front().materializedAppIds.size() == 1 && read->plugin.appTemplates.front().reason.hasValue() &&
            read->plugin.apps.size() == 1 && read->plugin.apps.front().category.isNull() && read->plugin.description.isNull() &&
            read->plugin.hooks.size() == 1 && read->plugin.hooks.front().eventName == typed::HookEventName::preToolUse() &&
            read->plugin.hooks.front().key == "synthetic-hook" && read->plugin.marketplaceName == "synthetic-marketplace" &&
            read->plugin.marketplacePath.hasValue() && read->plugin.mcpServers.size() == 1 && read->plugin.shareUrl.hasValue() &&
            read->plugin.skills.size() == 1 && read->plugin.skills.front().enabled && read->plugin.skills.front().interface.hasValue() &&
            read->plugin.skills.front().interface->iconLarge.hasValue() && read->plugin.skills.front().interface->iconSmall.isNull() &&
            read->plugin.skills.front().path.hasValue() && read->plugin.skills.front().shortDescription.isNull() &&
            read->plugin.summary.source.index() == 3 && read->plugin.raw == readWire.at("plugin") && read->raw == readWire && error.empty();
        result.expectTrue(detailComplete, "plugin/read decodes app-template/app/hook/skill/detail closure and a remote source completely");

        const codex::Json shareListWire{
            {"data",
             codex::Json::array({{{"localPluginPath", "/synthetic/plugins/synthetic-plugin"}, {"plugin", pluginSummary("local")}}})},
            {"futureShareListResponseField", true},
        };
        const auto shared = detail::decodePluginShareListResponse(shareListWire, error);
        result.expectTrue(shared && shared->data.size() == 1 && shared->data.front().localPluginPath.hasValue() &&
                              shared->data.front().plugin.source.index() == 1 && shared->raw == shareListWire && error.empty(),
                          "plugin/share/list decodes local paths, complete summaries, and local PluginSource");

        codex::Json malformedCatalog = listResponse();
        malformedCatalog["marketplaces"][0]["plugins"][0]["source"] = codex::Json{{"type", "git"}, {"path", "/synthetic/sensitive-path"}};
        const auto malformedKnown = detail::decodePluginListResponse(malformedCatalog, error);
        const auto* preserved =
            malformedKnown && malformedKnown->marketplaces.size() == 1 && malformedKnown->marketplaces.front().plugins.size() == 1
                ? std::get_if<typed::UnknownPluginSource>(&malformedKnown->marketplaces.front().plugins.front().source)
                : nullptr;
        result.expectTrue(
            preserved != nullptr && preserved->diagnostic && preserved->diagnostic->kind == typed::DecodeIssueKind::MalformedKnownPayload &&
                malformedKnown->diagnostics.size() == 1 &&
                malformedKnown->diagnostics.front().message.find("/synthetic/sensitive-path") == std::string::npos && error.empty(),
            "malformed-known source degrades nonfatally through its enclosing catalog with a safe diagnostic");

        result.expectTrue(!detail::decodePluginInstalledResponse({{"marketplaces", false}}, error) &&
                              error.find("$.marketplaces") != std::string::npos,
                          "plugin/installed rejects a wrong-typed required catalog array");
        codex::Json wrongNested = listResponse();
        wrongNested["marketplaces"][0]["plugins"][0]["interface"]["capabilities"][0] = false;
        result.expectTrue(!detail::decodePluginListResponse(wrongNested, error) &&
                              error.find("$.marketplaces[0].plugins[0].interface.capabilities[0]") != std::string::npos,
                          "plugin/list rejects a wrong-typed nested array element at its exact structural path");
        result.expectTrue(!detail::decodePluginReadResponse({{"plugin", codex::Json::object()}}, error) &&
                              error.find("$.plugin.appTemplates") != std::string::npos,
                          "plugin/read rejects a malformed known detail");
        result.expectTrue(!detail::decodePluginShareListResponse({{"data", codex::Json::array({codex::Json::object()})}}, error) &&
                              error.find("$.data[0].plugin") != std::string::npos,
                          "plugin/share/list rejects a missing required nested plugin");

        const auto wrongAssociation = detail::decodeClientOperationResult(detail::ClientRequestTarget::PluginInstalled, readWire);
        result.expectTrue(!wrongAssociation &&
                              wrongAssociation.diagnostic.code == detail::ClientOperationDecodeCode::MalformedKnownPayload &&
                              wrongAssociation.diagnostic.message.find("synthetic-plugin") == std::string::npos,
                          "plugin/read result cannot decode under plugin/installed and its diagnostic is value-free");
    }

    void testRegistryAndFacade(tests::support::TestResult& result) {
        struct ExpectedOperation {
            std::string_view method;
            detail::ClientRequestTarget target;
            std::string_view parameterType;
            std::string_view resultType;
        };
        constexpr std::array<ExpectedOperation, 4> PluginCatalogOperations{{
            {"plugin/installed", detail::ClientRequestTarget::PluginInstalled, "PluginInstalledParams", "PluginInstalledResponse"},
            {"plugin/list", detail::ClientRequestTarget::PluginList, "PluginListParams", "PluginListResponse"},
            {"plugin/read", detail::ClientRequestTarget::PluginRead, "PluginReadParams", "PluginReadResponse"},
            {"plugin/share/list", detail::ClientRequestTarget::PluginShareList, "PluginShareListParams", "PluginShareListResponse"},
        }};

        std::size_t complete = 0;
        std::size_t partial = 0;
        std::size_t notImplemented = 0;
        std::size_t notApplicable = 0;
        std::size_t nativeComplete = 0;
        std::size_t nativePartial = 0;
        std::size_t nativeNotImplemented = 0;
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
                nativeComplete += entry.typedSchemaStatus == detail::TypedSchemaStatus::Complete ? 1U : 0U;
                nativePartial += entry.typedSchemaStatus == detail::TypedSchemaStatus::Partial ? 1U : 0U;
                nativeNotImplemented += entry.typedSchemaStatus == detail::TypedSchemaStatus::NotImplemented ? 1U : 0U;
            }
        }
        result.expectTrue(complete == 336 && partial == 3 && notImplemented == 0 && notApplicable == 48,
                          "the current global registry arithmetic is exactly 336/3/0/48");
        result.expectTrue(nativeComplete == 56 && nativePartial == 0 && nativeNotImplemented == 0,
                          "the current native A1.4 registry arithmetic is exactly 56/0/0");

        for (const ExpectedOperation& expected : PluginCatalogOperations) {
            const detail::ProtocolSurfaceEntry* row =
                detail::findSurface(detail::SurfaceCategory::ClientRequest, "ClientRequest", "method", expected.method);
            const auto* target = row == nullptr ? nullptr : std::get_if<detail::ClientRequestTarget>(&row->runtimeTarget);
            std::size_t descriptorCount = 0;
            bool descriptorExact = false;
            for (const detail::ClientOperationCodecDescriptor& descriptor : detail::clientOperationCodecDescriptors()) {
                if (descriptor.key.name == expected.method) {
                    ++descriptorCount;
                    descriptorExact = descriptor.target == expected.target && descriptor.parameterTypeIdentity == expected.parameterType &&
                                      descriptor.resultTypeIdentity == expected.resultType &&
                                      descriptor.resultKind == detail::ResultContractKind::Concrete;
                }
            }
            result.expectTrue(row != nullptr && row->typedSchemaStatus == detail::TypedSchemaStatus::Complete &&
                                  row->typedImplementation == detail::TypedImplementationStatus::Implemented &&
                                  row->runtimeDisposition == detail::RuntimeDisposition::Typed && target != nullptr &&
                                  *target == expected.target && descriptorCount == 1 && descriptorExact,
                              std::string(expected.method) + " has one exact Complete registry target and descriptor");
        }

        constexpr std::array<std::string_view, 4> PluginSourceNames{{"git", "local", "npm", "remote"}};
        constexpr std::array<detail::IntegrationsAndLongTailUnionTarget, 4> PluginSourceTargets{{
            detail::IntegrationsAndLongTailUnionTarget::PluginSourceGit,
            detail::IntegrationsAndLongTailUnionTarget::PluginSourceLocal,
            detail::IntegrationsAndLongTailUnionTarget::PluginSourceNpm,
            detail::IntegrationsAndLongTailUnionTarget::PluginSourceRemote,
        }};
        const auto pluginSourceDescriptors = detail::integrationsAndLongTailUnionCodecDescriptors();
        bool exactPluginSourceOrder = pluginSourceDescriptors.size() == 7;
        for (std::size_t index = 0; index < PluginSourceNames.size() && exactPluginSourceOrder; ++index) {
            const detail::IntegrationsAndLongTailUnionCodecDescriptor& descriptor = pluginSourceDescriptors[index];
            const detail::ProtocolSurfaceEntry* row =
                detail::findSurface(detail::SurfaceCategory::TaggedUnionDiscriminator, "PluginSource", "type", PluginSourceNames[index]);
            const auto* target = row == nullptr ? nullptr : std::get_if<detail::IntegrationsAndLongTailUnionTarget>(&row->runtimeTarget);
            exactPluginSourceOrder = descriptor.key.name == PluginSourceNames[index] && descriptor.target == PluginSourceTargets[index] &&
                                     descriptor.shape == detail::ConversationUnionCodecShape::InternallyTaggedObject &&
                                     descriptor.direction == detail::ConversationUnionCodecDirection::DecodeOnly && row != nullptr &&
                                     row->typedSchemaStatus == detail::TypedSchemaStatus::Complete && target != nullptr &&
                                     *target == PluginSourceTargets[index];
        }
        result.expectTrue(exactPluginSourceOrder,
                          "PluginSource registry descriptors remain git/local/npm/remote and correlate with public indices 0-3");
    }
} // namespace

int main() {
    static_assert(std::is_same_v<decltype(&typed::Plugins::installed),
                                 typed::Plugins::Submission (typed::Plugins::*)(typed::PluginInstalledParams,
                                                                                typed::Plugins::InstalledResultHandler)>);
    static_assert(
        std::is_same_v<decltype(&typed::Plugins::list),
                       typed::Plugins::Submission (typed::Plugins::*)(typed::PluginListParams, typed::Plugins::ListResultHandler)>);
    static_assert(
        std::is_same_v<decltype(&typed::Plugins::read),
                       typed::Plugins::Submission (typed::Plugins::*)(typed::PluginReadParams, typed::Plugins::ReadResultHandler)>);
    static_assert(std::is_same_v<decltype(&typed::Plugins::shareList),
                                 typed::Plugins::Submission (typed::Plugins::*)(typed::PluginShareListParams,
                                                                                typed::Plugins::ShareListResultHandler)>);

    tests::support::TestResult result;
    testRequestEncoding(result);
    testPluginSource(result);
    testResultDecoding(result);
    testRegistryAndFacade(result);
    return result.processResult();
}
