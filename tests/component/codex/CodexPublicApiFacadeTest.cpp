/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *               2020, 2021, 2022, 2023, 2024, 2025, 2026
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/Api.h"
#include "ai/openai/codex/detail/Transport.h"
#include "support/TestResult.h"

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#ifndef AISUITE_TEST_PROJECT_VERSION
#error "AISUITE_TEST_PROJECT_VERSION must be supplied by CMake"
#endif

namespace {
    namespace codex = ai::openai::codex;
    namespace typed = codex::typed;

    struct GenericResultHandler {
        template <typename Result>
        void operator()(const Result&) const {
        }
    };

    template <typename Client>
    concept HasTypedAccessor = requires(Client& client) { client.typed(); };

    template <typename Client>
    concept HasConstRawAccessor = requires(const Client& client) { client.raw(); };

#define DEFINE_CONST_ACCESSOR_CONCEPT(Name, name)                                                                                          \
    template <typename Client>                                                                                                             \
    concept HasConst##Name##Accessor = requires(const Client& client) { client.name(); }

    DEFINE_CONST_ACCESSOR_CONCEPT(Accounts, accounts);
    DEFINE_CONST_ACCESSOR_CONCEPT(Apps, apps);
    DEFINE_CONST_ACCESSOR_CONCEPT(Commands, commands);
    DEFINE_CONST_ACCESSOR_CONCEPT(Configuration, configuration);
    DEFINE_CONST_ACCESSOR_CONCEPT(Events, events);
    DEFINE_CONST_ACCESSOR_CONCEPT(ExternalAgents, externalAgents);
    DEFINE_CONST_ACCESSOR_CONCEPT(Feedback, feedback);
    DEFINE_CONST_ACCESSOR_CONCEPT(Filesystem, filesystem);
    DEFINE_CONST_ACCESSOR_CONCEPT(Hooks, hooks);
    DEFINE_CONST_ACCESSOR_CONCEPT(Marketplace, marketplace);
    DEFINE_CONST_ACCESSOR_CONCEPT(Mcp, mcp);
    DEFINE_CONST_ACCESSOR_CONCEPT(Models, models);
    DEFINE_CONST_ACCESSOR_CONCEPT(PermissionProfiles, permissionProfiles);
    DEFINE_CONST_ACCESSOR_CONCEPT(Plugins, plugins);
    DEFINE_CONST_ACCESSOR_CONCEPT(Requests, requests);
    DEFINE_CONST_ACCESSOR_CONCEPT(Reviews, reviews);
    DEFINE_CONST_ACCESSOR_CONCEPT(Skills, skills);
    DEFINE_CONST_ACCESSOR_CONCEPT(Threads, threads);
    DEFINE_CONST_ACCESSOR_CONCEPT(Turns, turns);
    DEFINE_CONST_ACCESSOR_CONCEPT(WindowsSandbox, windowsSandbox);

#undef DEFINE_CONST_ACCESSOR_CONCEPT

    template <typename Client>
    concept HasRemoteControlAccessor = requires(Client& client) { client.remoteControl(); };

    template <typename Facade>
    concept HasLegacyLogoutUnit =
        requires(Facade& facade, typed::DoneHandler handler) { facade.logout(typed::Unit{}, std::move(handler)); };

    template <typename Facade>
    concept HasLegacyRateLimitsUnit = requires(Facade& facade, typed::CompletionHandler<typed::GetAccountRateLimitsResponse> handler) {
        facade.readRateLimits(typed::Unit{}, std::move(handler));
    };

    template <typename Facade>
    concept HasLegacyUsageUnit = requires(Facade& facade, typed::CompletionHandler<typed::GetAccountTokenUsageResponse> handler) {
        facade.readUsage(typed::Unit{}, std::move(handler));
    };

    template <typename Facade>
    concept HasLegacyWorkspaceMessagesUnit =
        requires(Facade& facade, typed::CompletionHandler<typed::GetWorkspaceMessagesResponse> handler) {
            facade.readWorkspaceMessages(typed::Unit{}, std::move(handler));
        };

    template <typename Facade>
    concept HasLegacyReloadMcpUnit =
        requires(Facade& facade, typed::DoneHandler handler) { facade.reloadMcpServers(typed::Unit{}, std::move(handler)); };

    template <typename Facade>
    concept HasLegacyRequirementsUnit = requires(Facade& facade, typed::CompletionHandler<typed::ConfigRequirementsReadResponse> handler) {
        facade.readRequirements(typed::Unit{}, std::move(handler));
    };

    template <typename Facade>
    concept HasLegacyImportHistoriesUnit =
        requires(Facade& facade, typed::CompletionHandler<typed::ExternalAgentConfigImportHistoriesReadResponse> handler) {
            facade.readImportHistories(typed::Unit{}, std::move(handler));
        };

    template <typename Facade>
    concept HasProviderCapabilitiesParams =
        requires(Facade& facade, typed::CompletionHandler<typed::ModelProviderCapabilitiesReadResponse> handler) {
            facade.readProviderCapabilities(typed::ModelProviderCapabilitiesReadParams{}, std::move(handler));
        };

    template <typename Facade>
    concept HasPluginShareListParams = requires(Facade& facade, typed::CompletionHandler<typed::PluginShareListResponse> handler) {
        facade.shareList(typed::PluginShareListParams{}, std::move(handler));
    };

    template <typename Facade>
    concept HasParameterlessStartLogin =
        requires(Facade& facade, typed::CompletionHandler<typed::LoginAccountResponse> handler) { facade.startLogin(std::move(handler)); };

    template <typename Facade>
    concept HasRemoteControlEnable =
        requires(Facade& facade, GenericResultHandler handler) { facade.enableRemoteControl(std::move(handler)); };

    template <typename Facade>
    concept HasRemoteControlDisable =
        requires(Facade& facade, GenericResultHandler handler) { facade.disableRemoteControl(std::move(handler)); };

    template <typename Facade>
    constexpr bool FacadeLifetimeSafe = !std::is_copy_constructible_v<Facade> && !std::is_move_constructible_v<Facade> &&
                                        !std::is_copy_assignable_v<Facade> && !std::is_move_assignable_v<Facade>;

    class InertTransport final : public codex::detail::Transport {
    public:
        void setCallbacks(codex::detail::TransportCallbacks) override {
        }

        void start() override {
        }

        bool send(std::string) override {
            return false;
        }

        void stop() override {
        }
    };

    class TestClient final : public codex::AppServerClient {
    public:
        TestClient()
            : AppServerClient(std::make_unique<InertTransport>(), codex::ClientInfo{}) {
        }
    };

    struct ProbeValue {
        int member = 0;
    };
} // namespace

int main() {
    static_assert(sizeof(codex::AppServerClient) == 2 * sizeof(void*));
    static_assert(!HasTypedAccessor<codex::AppServerClient>);
    static_assert(!HasConstRawAccessor<codex::AppServerClient>);
    static_assert(!HasRemoteControlAccessor<codex::AppServerClient>);

#define ASSERT_DIRECT_ACCESSOR(name, Facade)                                                                                               \
    static_assert(                                                                                                                         \
        std::same_as<decltype(static_cast<typed::Facade& (codex::AppServerClient::*) () noexcept>(&codex::AppServerClient::name)),         \
                     typed::Facade& (codex::AppServerClient::*) () noexcept>)

    ASSERT_DIRECT_ACCESSOR(accounts, Accounts);
    ASSERT_DIRECT_ACCESSOR(apps, Apps);
    ASSERT_DIRECT_ACCESSOR(commands, Commands);
    ASSERT_DIRECT_ACCESSOR(configuration, Configuration);
    ASSERT_DIRECT_ACCESSOR(events, Events);
    ASSERT_DIRECT_ACCESSOR(externalAgents, ExternalAgents);
    ASSERT_DIRECT_ACCESSOR(feedback, Feedback);
    ASSERT_DIRECT_ACCESSOR(filesystem, Filesystem);
    ASSERT_DIRECT_ACCESSOR(hooks, Hooks);
    ASSERT_DIRECT_ACCESSOR(marketplace, Marketplace);
    ASSERT_DIRECT_ACCESSOR(mcp, Mcp);
    ASSERT_DIRECT_ACCESSOR(models, Models);
    ASSERT_DIRECT_ACCESSOR(permissionProfiles, PermissionProfiles);
    ASSERT_DIRECT_ACCESSOR(plugins, Plugins);
    ASSERT_DIRECT_ACCESSOR(requests, Requests);
    ASSERT_DIRECT_ACCESSOR(reviews, Reviews);
    ASSERT_DIRECT_ACCESSOR(skills, Skills);
    ASSERT_DIRECT_ACCESSOR(threads, Threads);
    ASSERT_DIRECT_ACCESSOR(turns, Turns);
    ASSERT_DIRECT_ACCESSOR(windowsSandbox, WindowsSandbox);

#undef ASSERT_DIRECT_ACCESSOR

    static_assert(!HasConstAccountsAccessor<codex::AppServerClient>);
    static_assert(!HasConstAppsAccessor<codex::AppServerClient>);
    static_assert(!HasConstCommandsAccessor<codex::AppServerClient>);
    static_assert(!HasConstConfigurationAccessor<codex::AppServerClient>);
    static_assert(!HasConstEventsAccessor<codex::AppServerClient>);
    static_assert(!HasConstExternalAgentsAccessor<codex::AppServerClient>);
    static_assert(!HasConstFeedbackAccessor<codex::AppServerClient>);
    static_assert(!HasConstFilesystemAccessor<codex::AppServerClient>);
    static_assert(!HasConstHooksAccessor<codex::AppServerClient>);
    static_assert(!HasConstMarketplaceAccessor<codex::AppServerClient>);
    static_assert(!HasConstMcpAccessor<codex::AppServerClient>);
    static_assert(!HasConstModelsAccessor<codex::AppServerClient>);
    static_assert(!HasConstPermissionProfilesAccessor<codex::AppServerClient>);
    static_assert(!HasConstPluginsAccessor<codex::AppServerClient>);
    static_assert(!HasConstRequestsAccessor<codex::AppServerClient>);
    static_assert(!HasConstReviewsAccessor<codex::AppServerClient>);
    static_assert(!HasConstSkillsAccessor<codex::AppServerClient>);
    static_assert(!HasConstThreadsAccessor<codex::AppServerClient>);
    static_assert(!HasConstTurnsAccessor<codex::AppServerClient>);
    static_assert(!HasConstWindowsSandboxAccessor<codex::AppServerClient>);

    static_assert(FacadeLifetimeSafe<typed::Accounts>);
    static_assert(FacadeLifetimeSafe<typed::Apps>);
    static_assert(FacadeLifetimeSafe<typed::Commands>);
    static_assert(FacadeLifetimeSafe<typed::Configuration>);
    static_assert(FacadeLifetimeSafe<typed::Events>);
    static_assert(FacadeLifetimeSafe<typed::ExternalAgents>);
    static_assert(FacadeLifetimeSafe<typed::Feedback>);
    static_assert(FacadeLifetimeSafe<typed::Filesystem>);
    static_assert(FacadeLifetimeSafe<typed::Hooks>);
    static_assert(FacadeLifetimeSafe<typed::Marketplace>);
    static_assert(FacadeLifetimeSafe<typed::Mcp>);
    static_assert(FacadeLifetimeSafe<typed::Models>);
    static_assert(FacadeLifetimeSafe<typed::PermissionProfiles>);
    static_assert(FacadeLifetimeSafe<typed::Plugins>);
    static_assert(FacadeLifetimeSafe<typed::Requests>);
    static_assert(FacadeLifetimeSafe<typed::Reviews>);
    static_assert(FacadeLifetimeSafe<typed::Skills>);
    static_assert(FacadeLifetimeSafe<typed::Threads>);
    static_assert(FacadeLifetimeSafe<typed::Turns>);
    static_assert(FacadeLifetimeSafe<typed::WindowsSandbox>);

    static_assert(std::same_as<decltype(std::declval<codex::AppServerClient::RawProtocol&>().request(
                                   std::declval<std::string>(),
                                   std::declval<codex::Json>(),
                                   std::declval<codex::AppServerClient::RawProtocol::ResponseHandler>())),
                               codex::Submission>);
    static_assert(std::same_as<decltype(std::declval<codex::AppServerClient::RawProtocol&>().notify(std::declval<std::string>(),
                                                                                                    std::declval<codex::Json>())),
                               codex::SendResult>);
    static_assert(std::same_as<typed::CompletionHandler<typed::ThreadStartResponse>,
                               std::function<void(const typed::OperationResult<typed::ThreadStartResponse>&)>>);
    static_assert(std::same_as<typed::DoneHandler, std::function<void(const typed::OperationResult<typed::Unit>&)>>);

    static_assert(requires(typed::Accounts& accounts,
                           typed::DoneHandler done,
                           typed::CompletionHandler<typed::GetAccountRateLimitsResponse> rateLimits,
                           typed::CompletionHandler<typed::GetAccountTokenUsageResponse> usage,
                           typed::CompletionHandler<typed::GetWorkspaceMessagesResponse> messages) {
        { accounts.logout(std::move(done)) } -> std::same_as<codex::Submission>;
        { accounts.readRateLimits(std::move(rateLimits)) } -> std::same_as<codex::Submission>;
        { accounts.readUsage(std::move(usage)) } -> std::same_as<codex::Submission>;
        { accounts.readWorkspaceMessages(std::move(messages)) } -> std::same_as<codex::Submission>;
    });
    static_assert(requires(typed::Configuration& configuration,
                           typed::DoneHandler done,
                           typed::CompletionHandler<typed::ConfigRequirementsReadResponse> requirements) {
        { configuration.reloadMcpServers(std::move(done)) } -> std::same_as<codex::Submission>;
        { configuration.readRequirements(std::move(requirements)) } -> std::same_as<codex::Submission>;
    });
    static_assert(requires(typed::ExternalAgents& externalAgents,
                           typed::CompletionHandler<typed::ExternalAgentConfigImportHistoriesReadResponse> histories) {
        { externalAgents.readImportHistories(std::move(histories)) } -> std::same_as<codex::Submission>;
    });
    static_assert(!HasLegacyLogoutUnit<typed::Accounts>);
    static_assert(!HasLegacyRateLimitsUnit<typed::Accounts>);
    static_assert(!HasLegacyUsageUnit<typed::Accounts>);
    static_assert(!HasLegacyWorkspaceMessagesUnit<typed::Accounts>);
    static_assert(!HasLegacyReloadMcpUnit<typed::Configuration>);
    static_assert(!HasLegacyRequirementsUnit<typed::Configuration>);
    static_assert(!HasLegacyImportHistoriesUnit<typed::ExternalAgents>);

    static_assert(requires(typed::Models& models, typed::CompletionHandler<typed::ModelProviderCapabilitiesReadResponse> handler) {
        { models.readProviderCapabilities(std::move(handler)) } -> std::same_as<codex::Submission>;
    });
    static_assert(requires(typed::Plugins& plugins, typed::CompletionHandler<typed::PluginShareListResponse> handler) {
        { plugins.shareList(std::move(handler)) } -> std::same_as<codex::Submission>;
    });
    static_assert(!HasProviderCapabilitiesParams<typed::Models>);
    static_assert(!HasPluginShareListParams<typed::Plugins>);
    static_assert(requires(
        typed::Accounts& accounts, typed::LoginAccountParams params, typed::CompletionHandler<typed::LoginAccountResponse> handler) {
        { accounts.startLogin(std::move(params), std::move(handler)) } -> std::same_as<codex::Submission>;
    });
    static_assert(!HasParameterlessStartLogin<typed::Accounts>);
    static_assert(std::same_as<std::variant_alternative_t<8, typed::TypedServerRequest>, typed::AttestationGenerateRequest>);

    static_assert(requires(typed::Threads& threads,
                           typed::Accounts& accounts,
                           typed::Models& models,
                           typed::Configuration& configuration,
                           typed::PermissionProfiles& permissions,
                           typed::Apps& apps,
                           typed::ExternalAgents& externalAgents,
                           typed::Hooks& hooks,
                           typed::Marketplace& marketplace,
                           typed::Plugins& plugins,
                           typed::Skills& skills,
                           typed::Mcp& mcp,
                           GenericResultHandler handler) {
        { threads.start(handler) } -> std::same_as<codex::Submission>;
        { threads.list(handler) } -> std::same_as<codex::Submission>;
        { threads.listLoaded(handler) } -> std::same_as<codex::Submission>;
        { accounts.read(handler) } -> std::same_as<codex::Submission>;
        { models.list(handler) } -> std::same_as<codex::Submission>;
        { configuration.read(handler) } -> std::same_as<codex::Submission>;
        { configuration.listExperimentalFeatures(handler) } -> std::same_as<codex::Submission>;
        { permissions.list(handler) } -> std::same_as<codex::Submission>;
        { apps.list(handler) } -> std::same_as<codex::Submission>;
        { externalAgents.detect(handler) } -> std::same_as<codex::Submission>;
        { hooks.list(handler) } -> std::same_as<codex::Submission>;
        { marketplace.upgrade(handler) } -> std::same_as<codex::Submission>;
        { plugins.installed(handler) } -> std::same_as<codex::Submission>;
        { plugins.list(handler) } -> std::same_as<codex::Submission>;
        { skills.list(handler) } -> std::same_as<codex::Submission>;
        { mcp.listServers(handler) } -> std::same_as<codex::Submission>;
    });
    static_assert(!HasRemoteControlEnable<typed::Commands>);
    static_assert(!HasRemoteControlDisable<typed::Commands>);

    using ThreadStartCwd = codex::Submission (typed::Threads::*)(typed::AbsolutePath, typed::CompletionHandler<typed::ThreadStartResponse>);
    using ThreadResumeId = codex::Submission (typed::Threads::*)(typed::ThreadId, typed::CompletionHandler<typed::ThreadResumeResponse>);
    using ThreadReadId = codex::Submission (typed::Threads::*)(typed::ThreadId, typed::CompletionHandler<typed::ThreadReadResponse>);
    using TurnStartInput = codex::Submission (typed::Turns::*)(
        typed::ThreadId, std::vector<typed::TurnInput>, typed::CompletionHandler<typed::TurnStartResponse>);
    using TurnStartText =
        codex::Submission (typed::Turns::*)(typed::ThreadId, std::string, typed::CompletionHandler<typed::TurnStartResponse>);
    using TurnInterruptIds = codex::Submission (typed::Turns::*)(typed::ThreadId, typed::TurnId, typed::DoneHandler);
    static_assert(std::same_as<decltype(static_cast<ThreadStartCwd>(&typed::Threads::start)), ThreadStartCwd>);
    static_assert(std::same_as<decltype(static_cast<ThreadResumeId>(&typed::Threads::resume)), ThreadResumeId>);
    static_assert(std::same_as<decltype(static_cast<ThreadReadId>(&typed::Threads::read)), ThreadReadId>);
    static_assert(std::same_as<decltype(static_cast<TurnStartInput>(&typed::Turns::start)), TurnStartInput>);
    static_assert(std::same_as<decltype(static_cast<TurnStartText>(&typed::Turns::start)), TurnStartText>);
    static_assert(std::same_as<decltype(static_cast<TurnInterruptIds>(&typed::Turns::interrupt)), TurnInterruptIds>);

    static_assert(std::variant_size_v<typed::CanonicalServerNotification> == 68);
    static_assert(std::same_as<std::variant_alternative_t<67, typed::CanonicalServerNotification>, typed::ErrorNotification>);
    static_assert(std::variant_size_v<typed::Event> == 69);
    static_assert(std::same_as<std::variant_alternative_t<44, typed::Event>, typed::TurnErrorEvent>);
    static_assert(std::same_as<std::variant_alternative_t<45, typed::Event>, typed::UnknownEvent>);
    static_assert(std::variant_size_v<typed::TypedServerRequest> == 11);

    tests::support::TestResult result;
    TestClient client;

#define EXPECT_STABLE_FACADE(name) result.expectTrue(&client.name() == &client.name(), #name "() returns one stable client-owned façade")

    EXPECT_STABLE_FACADE(accounts);
    EXPECT_STABLE_FACADE(apps);
    EXPECT_STABLE_FACADE(commands);
    EXPECT_STABLE_FACADE(configuration);
    EXPECT_STABLE_FACADE(events);
    EXPECT_STABLE_FACADE(externalAgents);
    EXPECT_STABLE_FACADE(feedback);
    EXPECT_STABLE_FACADE(filesystem);
    EXPECT_STABLE_FACADE(hooks);
    EXPECT_STABLE_FACADE(marketplace);
    EXPECT_STABLE_FACADE(mcp);
    EXPECT_STABLE_FACADE(models);
    EXPECT_STABLE_FACADE(permissionProfiles);
    EXPECT_STABLE_FACADE(plugins);
    EXPECT_STABLE_FACADE(requests);
    EXPECT_STABLE_FACADE(reviews);
    EXPECT_STABLE_FACADE(skills);
    EXPECT_STABLE_FACADE(threads);
    EXPECT_STABLE_FACADE(turns);
    EXPECT_STABLE_FACADE(windowsSandbox);

#undef EXPECT_STABLE_FACADE

    const codex::ClientInfo identity{};
    result.expectTrue(identity.name == "aisuite" && identity.title == "AISuite" && identity.version == AISUITE_TEST_PROJECT_VERSION,
                      "default client identity uses AISuite and the configured project version");

    typed::OperationResult<ProbeValue> success;
    success.value = ProbeValue{42};
    result.expectTrue(success && success.isSuccess() && !success.isRemoteError() && !success.isCancelled() && !success.isLocalError() &&
                          (*success).member == 42 && success->member == 42,
                      "successful OperationResult supports predicates and optional-like access");
    const auto& constSuccess = success;
    result.expectTrue((*constSuccess).member == 42 && constSuccess->member == 42,
                      "const successful OperationResult supports optional-like access");

    typed::OperationResult<ProbeValue> remote;
    remote.kind = typed::OperationResult<ProbeValue>::Kind::RemoteError;
    typed::OperationResult<ProbeValue> cancelled;
    cancelled.kind = typed::OperationResult<ProbeValue>::Kind::Cancelled;
    typed::OperationResult<ProbeValue> local;
    local.kind = typed::OperationResult<ProbeValue>::Kind::LocalError;
    result.expectTrue(!remote && remote.isRemoteError() && !remote.isSuccess() && !remote.isCancelled() && !remote.isLocalError(),
                      "remote-error OperationResult predicate is exact");
    result.expectTrue(!cancelled && cancelled.isCancelled() && !cancelled.isSuccess() && !cancelled.isRemoteError() &&
                          !cancelled.isLocalError(),
                      "cancelled OperationResult predicate is exact");
    result.expectTrue(!local && local.isLocalError() && !local.isSuccess() && !local.isRemoteError() && !local.isCancelled(),
                      "local-error OperationResult predicate is exact");

    return result.processResult();
}
