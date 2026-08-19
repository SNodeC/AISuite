/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/model/Projection.h"
#include "support/TestResult.h"

#include <algorithm>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = frontend::generated;
    namespace model = frontend::internal::model;

    model::ProjectionContext projectionContext(std::vector<frontend::FrontendScope> scopes, bool controllerOwned = false) {
        model::ProjectionContext context;
        context.principal.id = "principal";
        context.principal.scopes = std::move(scopes);
        context.controllerOwned = controllerOwned;
        return context;
    }

    model::CanonicalSnapshot itemSnapshot(frontend::ThreadItemKind kind) {
        model::CanonicalSnapshot snapshot;
        auto id = model::ItemIdentity::parse("item-1");
        model::ItemData data{*id};
        data.commandOutput = "bounded output";
        switch (kind) {
            case frontend::ThreadItemKind::CommandExecution:
                snapshot.items.push_back(model::CommandExecutionItem{std::move(data)});
                break;
            case frontend::ThreadItemKind::FileChange:
                snapshot.items.push_back(model::FileChangeItem{std::move(data)});
                break;
            default:
                snapshot.items.push_back(model::AgentMessageItem{std::move(data)});
                break;
        }
        return snapshot;
    }

    void testGeneratedMethodPolicy(tests::support::TestResult& result) {
        model::ProjectionAuthority authority;
        auto permitted = projectionContext(
            {frontend::FrontendScope::Control, frontend::FrontendScope::CommandExecution}, true);
        auto noController = permitted;
        noController.controllerOwned = false;
        auto noScope = projectionContext({frontend::FrontendScope::Control}, true);
        result.expectTrue(authority.methodAllowed(permitted, generated::MethodId::CommandExec, true) &&
                              !authority.methodAllowed(noController, generated::MethodId::CommandExec, true) &&
                              !authority.methodAllowed(noScope, generated::MethodId::CommandExec, true) &&
                              !authority.methodAllowed(permitted, generated::MethodId::CommandExec, false),
                          "generated scopes, controller ownership, and provider readiness remain independent method gates");
    }

    void testItemInformationCeilings(tests::support::TestResult& result) {
        model::ProjectionAuthority authority;
        const auto filesystem = projectionContext(
            {frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite});
        const auto command = projectionContext(
            {frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution});
        const auto both = projectionContext({frontend::FrontendScope::Observe,
                                             frontend::FrontendScope::CommandExecution,
                                             frontend::FrontendScope::FilesystemWrite});
        const auto projectedFile = authority.projectSnapshot(itemSnapshot(frontend::ThreadItemKind::FileChange), filesystem);
        const auto hiddenFile = authority.projectSnapshot(itemSnapshot(frontend::ThreadItemKind::FileChange), command);
        const auto projectedCommand = authority.projectSnapshot(itemSnapshot(frontend::ThreadItemKind::CommandExecution), command);
        const auto conservative = authority.projectSnapshot(itemSnapshot(frontend::ThreadItemKind::AgentMessage), both);
        result.expectTrue(projectedFile && model::itemData(projectedFile.value().items.front()).commandOutput.has_value() && hiddenFile &&
                              !model::itemData(hiddenFile.value().items.front()).commandOutput.has_value() && projectedCommand &&
                              model::itemData(projectedCommand.value().items.front()).commandOutput.has_value() && conservative &&
                              model::itemData(conservative.value().items.front()).commandOutput.has_value(),
                          "command output uses discriminator-specific command, filesystem-write, and conservative dual-scope ceilings");
    }

    void testGeneratedOccurrenceAuthority(tests::support::TestResult& result) {
        const auto metadata = std::find_if(generated::AllNotificationProjections.begin(),
                                           generated::AllNotificationProjections.end(),
                                           [](const generated::ProjectionMetadata& value) {
                                               return value.expandedMappings.size() == 1 &&
                                                      value.expandedMappings.front() == "process.updated";
                                           });
        const std::string source = metadata == generated::AllNotificationProjections.end()
                                       ? std::string{"missing-generated-authority"}
                                       : std::string{metadata->registryKey};
        model::OccurrenceIdentity identity{model::FrontendSequence{1},
                                           *model::OccurrenceGroupIdentity::parse("projection-group"),
                                           0,
                                           1,
                                           *model::SourceStamp::parse(source)};
        model::ProcessState process{*model::ProcessHandle::parse("process-1")};
        const auto occurrence = model::makeOccurrence(std::move(identity), model::ProcessUpdatedOccurrence{std::move(process)});
        model::ProjectionAuthority authority;
        const auto observer = projectionContext({frontend::FrontendScope::Observe});
        const auto projected = occurrence ? authority.projectOccurrence(occurrence.value(), observer)
                                          : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};
        result.expectTrue(metadata != generated::AllNotificationProjections.end() && occurrence && projected &&
                              !projected.value().has_value(),
                          "generated notification identity and privileged family scope suppress the entire unauthorized occurrence");

        const auto makeProvider = [](std::string sourceStamp, std::string group) {
            model::OccurrenceIdentity valueIdentity{model::FrontendSequence{2},
                                                    *model::OccurrenceGroupIdentity::parse(std::move(group)),
                                                    0,
                                                    1,
                                                    *model::SourceStamp::parse(std::move(sourceStamp))};
            return model::makeOccurrence(std::move(valueIdentity), model::ProviderUpdatedOccurrence{model::ProviderState{}});
        };
        const auto trustedBackend = makeProvider("backend-event:42", "trusted-backend");
        const auto arbitrary = makeProvider("retained-source", "arbitrary-source");
        const auto mismatched = makeProvider(source, "mismatched-generated-source");
        const auto trustedProjected = trustedBackend ? authority.projectOccurrence(trustedBackend.value(), observer)
                                                     : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                           {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};
        const auto arbitraryProjected = arbitrary ? authority.projectOccurrence(arbitrary.value(), observer)
                                                  : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                        {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};
        const auto mismatchedProjected = mismatched ? authority.projectOccurrence(mismatched.value(), observer)
                                                    : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                          {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};
        result.expectTrue(trustedProjected && trustedProjected.value().has_value() && !arbitraryProjected &&
                              arbitraryProjected.error().code == model::ProjectionErrorCode::MissingGeneratedAuthority &&
                              !mismatchedProjected &&
                              mismatchedProjected.error().code == model::ProjectionErrorCode::MissingGeneratedAuthority,
                          "only structural backend-event identities or exact generated source/family mappings bypass authority rejection");
    }

    void testExecutionConfigurationAuthority(tests::support::TestResult& result) {
        model::CanonicalSnapshot snapshot;
        model::ThreadState thread{model::ThreadIdentity{"thread-config"}};
        thread.safeDetails = *model::SafeDetail::fromJson(frontend::Json{
            {"cwd", "/workspace/private"},
            {"executionConfiguration",
             {{"approvalPolicy", "on-request"},
              {"approvalsReviewer", "user"},
              {"collaborationMode",
               {{"mode", "plan"},
                {"settings",
                 {{"developerInstructions", "private developer instructions"}, {"model", "gpt-5.6"}}}}},
              {"cwd", "/workspace/private"},
              {"model", "gpt-5.6"},
              {"modelProvider", "openai"},
              {"sandboxPolicy", {{"type", "readOnly"}}}}}});
        snapshot.threads.push_back(std::move(thread));

        model::ProjectionAuthority authority;
        const auto controller = projectionContext({frontend::FrontendScope::Observe,
                                                   frontend::FrontendScope::Control,
                                                   frontend::FrontendScope::FilesystemRead,
                                                   frontend::FrontendScope::SensitiveResponse},
                                                  true);
        const auto observer = projectionContext({frontend::FrontendScope::Observe});
        const auto controlled = authority.projectSnapshot(snapshot, controller);
        const auto observed = authority.projectSnapshot(snapshot, observer);
        const frontend::Json controlledDetails =
            controlled ? controlled.value().threads.front().safeDetails.json() : frontend::Json::object();
        const frontend::Json observedDetails =
            observed ? observed.value().threads.front().safeDetails.json() : frontend::Json::object();
        const frontend::Json observedConfiguration =
            observedDetails.value("executionConfiguration", frontend::Json::object());
        const frontend::Json observedCollaboration =
            observedConfiguration.value("collaborationMode", frontend::Json::object());
        const frontend::Json observedCollaborationSettings =
            observedCollaboration.value("settings", frontend::Json::object());
        result.expectTrue(controlled &&
                              controlledDetails.at("executionConfiguration").value("cwd", "") == "/workspace/private" &&
                              controlledDetails.at("executionConfiguration")
                                      .at("collaborationMode")
                                      .at("settings")
                                      .value("developerInstructions", "") == "private developer instructions" &&
                              observed && !observedDetails.contains("cwd") && !observedConfiguration.contains("cwd") &&
                              !observedCollaborationSettings.contains("developerInstructions") &&
                              observedConfiguration.value("model", "") == "gpt-5.6",
                          "execution settings retain cwd and instructions only for frontends with the corresponding authority");
    }
}

int main() {
    tests::support::TestResult result;
    testGeneratedMethodPolicy(result);
    testItemInformationCeilings(result);
    testGeneratedOccurrenceAuthority(result);
    testExecutionConfigurationAuthority(result);
    return result.processResult();
}
