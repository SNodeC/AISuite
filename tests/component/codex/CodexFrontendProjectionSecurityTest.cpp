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
        data.commandOutputOverflowV2 = model::ItemContentOverflowV1{
            static_cast<std::uint64_t>(data.commandOutput->size()), " retained suffix", 0, false, false};
        data.droppedContentBytes = data.commandOutputOverflowV2->suffix.size();
        data.contentTruncated = true;
        data.truncation.truncated = true;
        data.truncation.droppedBytes = data.commandOutputOverflowV2->suffix.size();
        data.truncation.omittedPaths = {"/commandOutput"};
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

    model::CanonicalSnapshot legacyItemSnapshot() {
        model::CanonicalSnapshot snapshot;
        model::ItemData data{model::ItemIdentity{"legacy-item-1"}};
        data.commandOutput = "legacy bounded output";
        data.commandOutputOverflowV2 = model::ItemContentOverflowV1{
            static_cast<std::uint64_t>(data.commandOutput->size()), " legacy retained suffix", 0, false, false};
        data.droppedContentBytes = data.commandOutputOverflowV2->suffix.size();
        data.contentTruncated = true;
        data.truncation.truncated = true;
        data.truncation.droppedBytes = data.commandOutputOverflowV2->suffix.size();
        data.truncation.omittedPaths = {"/commandOutput"};
        snapshot.legacyItems.push_back(
            model::LegacyItemCompatibility{std::move(data), "future_item", 0, "/legacyItems/0"});
        return snapshot;
    }

    model::OccurrenceResult<model::CanonicalOccurrence>
    itemContentOccurrence(std::optional<frontend::ThreadItemKind> kind, std::uint64_t sequence) {
        model::OccurrenceIdentity identity{
            model::FrontendSequence{sequence},
            *model::OccurrenceGroupIdentity::parse("scope-item-content-" + std::to_string(sequence)),
            0,
            1,
            *model::SourceStamp::parse("backend-event:" + std::to_string(sequence))};
        model::ItemContentUpdatedOccurrence update{model::ItemIdentity{"item-" + std::to_string(sequence)}};
        update.channel = "commandOutput";
        update.itemKind = kind;
        update.content = "projected command output";
        return model::makeOccurrence(std::move(identity), std::move(update));
    }

    model::OccurrenceResult<model::CanonicalOccurrence> legacyItemOccurrence(std::uint64_t sequence) {
        model::OccurrenceIdentity identity{
            model::FrontendSequence{sequence},
            *model::OccurrenceGroupIdentity::parse("scope-legacy-item-" + std::to_string(sequence)),
            0,
            1,
            *model::SourceStamp::parse("backend-event:scope-legacy-item")};
        model::ItemData data{model::ItemIdentity{"legacy-item-" + std::to_string(sequence)}};
        data.commandOutput = "legacy occurrence output";
        data.commandOutputOverflowV2 = model::ItemContentOverflowV1{
            static_cast<std::uint64_t>(data.commandOutput->size()), " legacy occurrence suffix", 0, false, false};
        model::LegacyCompatibilityPayload legacy;
        legacy.kind = model::LegacyCompatibilityKind::LegacyItem;
        legacy.legacyItem =
            model::LegacyItemCompatibility{std::move(data), "future_item", 0, "/legacy/item"};
        return model::makeOccurrenceGroup(std::move(identity), std::move(legacy), {});
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
        const auto hiddenPathCount = hiddenFile
                                         ? std::ranges::count(hiddenFile.value().projection.omittedPaths,
                                                              "/items/0/commandOutput")
                                         : std::ptrdiff_t{0};
        result.expectTrue(projectedFile && model::itemData(projectedFile.value().items.front()).commandOutput.has_value() &&
                              model::itemData(projectedFile.value().items.front()).commandOutputOverflowV2.has_value() && hiddenFile &&
                              !model::itemData(hiddenFile.value().items.front()).commandOutput.has_value() &&
                              !model::itemData(hiddenFile.value().items.front()).commandOutputOverflowV2.has_value() &&
                              hiddenPathCount == 1 && projectedCommand &&
                              model::itemData(projectedCommand.value().items.front()).commandOutput.has_value() &&
                              model::itemData(projectedCommand.value().items.front()).commandOutputOverflowV2.has_value() && conservative &&
                              model::itemData(conservative.value().items.front()).commandOutput.has_value(),
                          "command output uses discriminator-specific command, filesystem-write, and conservative dual-scope ceilings");
    }

    void testCommandOutputOccurrenceScopeMatrix(tests::support::TestResult& result) {
        model::ProjectionAuthority authority;
        const auto command = projectionContext(
            {frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution});
        const auto filesystem = projectionContext(
            {frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite});
        const auto both = projectionContext({frontend::FrontendScope::Observe,
                                             frontend::FrontendScope::CommandExecution,
                                             frontend::FrontendScope::FilesystemWrite});

        const auto commandOccurrence = itemContentOccurrence(frontend::ThreadItemKind::CommandExecution, 10);
        const auto fileOccurrence = itemContentOccurrence(frontend::ThreadItemKind::FileChange, 11);
        const auto unknownOccurrence = itemContentOccurrence(std::nullopt, 12);
        const auto commandAllowed = commandOccurrence ? authority.projectOccurrence(commandOccurrence.value(), command)
                                                      : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                            {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};
        const auto commandDenied = commandOccurrence ? authority.projectOccurrence(commandOccurrence.value(), filesystem)
                                                     : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                           {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};
        const auto fileAllowed = fileOccurrence ? authority.projectOccurrence(fileOccurrence.value(), filesystem)
                                                : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                      {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};
        const auto fileDenied = fileOccurrence ? authority.projectOccurrence(fileOccurrence.value(), command)
                                               : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                     {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};
        const auto unknownCommand = unknownOccurrence ? authority.projectOccurrence(unknownOccurrence.value(), command)
                                                      : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                            {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};
        const auto unknownFilesystem = unknownOccurrence ? authority.projectOccurrence(unknownOccurrence.value(), filesystem)
                                                         : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                               {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};
        const auto unknownBoth = unknownOccurrence ? authority.projectOccurrence(unknownOccurrence.value(), both)
                                                   : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                         {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};

        const auto retainedCommandOutput = [](const auto& projected) {
            if (!projected || !projected.value().has_value() || projected.value()->expandedPayloads().size() != 1) {
                return false;
            }
            const auto* content =
                std::get_if<model::ItemContentUpdatedOccurrence>(&projected.value()->expandedPayloads().front());
            return content != nullptr && content->channel == std::optional<std::string>{"commandOutput"} &&
                   content->content == std::optional<std::string>{"projected command output"};
        };
        result.expectTrue(commandOccurrence && retainedCommandOutput(commandAllowed) && commandDenied &&
                              !commandDenied.value().has_value(),
                          "typed command-output occurrences require command-execution scope");
        result.expectTrue(fileOccurrence && retainedCommandOutput(fileAllowed) && fileDenied && !fileDenied.value().has_value(),
                          "typed file-change output occurrences require filesystem-write scope");
        result.expectTrue(unknownOccurrence && unknownCommand && !unknownCommand.value().has_value() && unknownFilesystem &&
                              !unknownFilesystem.value().has_value() && retainedCommandOutput(unknownBoth),
                          "typed command-output occurrences with unknown item kind conservatively require both scopes");
    }

    void testLegacyCommandOutputScopeMatrix(tests::support::TestResult& result) {
        model::ProjectionAuthority authority;
        const auto command = projectionContext(
            {frontend::FrontendScope::Observe, frontend::FrontendScope::CommandExecution});
        const auto filesystem = projectionContext(
            {frontend::FrontendScope::Observe, frontend::FrontendScope::FilesystemWrite});
        const auto both = projectionContext({frontend::FrontendScope::Observe,
                                             frontend::FrontendScope::CommandExecution,
                                             frontend::FrontendScope::FilesystemWrite});

        const auto commandSnapshot = authority.projectSnapshot(legacyItemSnapshot(), command);
        const auto filesystemSnapshot = authority.projectSnapshot(legacyItemSnapshot(), filesystem);
        const auto bothSnapshot = authority.projectSnapshot(legacyItemSnapshot(), both);
        const auto legacyOccurrence = legacyItemOccurrence(20);
        const auto commandOccurrence = legacyOccurrence ? authority.projectOccurrence(legacyOccurrence.value(), command)
                                                        : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                              {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};
        const auto filesystemOccurrence = legacyOccurrence ? authority.projectOccurrence(legacyOccurrence.value(), filesystem)
                                                           : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                                 {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};
        const auto bothOccurrence = legacyOccurrence ? authority.projectOccurrence(legacyOccurrence.value(), both)
                                                     : model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>>{
                                                           {model::ProjectionErrorCode::InvalidValue, "/", "construction failed"}};

        const auto snapshotOutputVisible = [](const auto& projected) {
            return projected && projected.value().legacyItems.size() == 1 &&
                   projected.value().legacyItems.front().value.commandOutput.has_value() &&
                   projected.value().legacyItems.front().value.commandOutputOverflowV2.has_value();
        };
        const auto snapshotOutputHidden = [](const auto& projected) {
            return projected && projected.value().legacyItems.size() == 1 &&
                   !projected.value().legacyItems.front().value.commandOutput.has_value() &&
                   !projected.value().legacyItems.front().value.commandOutputOverflowV2.has_value();
        };
        const auto occurrenceOutputVisible = [](const auto& projected) {
            return projected && projected.value().has_value() &&
                   projected.value()->legacyCompatibility().legacyItem.has_value() &&
                   projected.value()->legacyCompatibility().legacyItem->value.commandOutput.has_value() &&
                   projected.value()->legacyCompatibility().legacyItem->value.commandOutputOverflowV2.has_value();
        };
        const auto occurrenceOutputHidden = [](const auto& projected) {
            return projected && projected.value().has_value() &&
                   projected.value()->legacyCompatibility().legacyItem.has_value() &&
                   !projected.value()->legacyCompatibility().legacyItem->value.commandOutput.has_value() &&
                   !projected.value()->legacyCompatibility().legacyItem->value.commandOutputOverflowV2.has_value();
        };
        result.expectTrue(snapshotOutputHidden(commandSnapshot) && snapshotOutputHidden(filesystemSnapshot) &&
                              snapshotOutputVisible(bothSnapshot),
                          "legacy snapshot command output conservatively requires both scopes");
        result.expectTrue(legacyOccurrence && occurrenceOutputHidden(commandOccurrence) &&
                              occurrenceOutputHidden(filesystemOccurrence) && occurrenceOutputVisible(bothOccurrence),
                          "legacy-only occurrence command output conservatively requires both scopes");
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
    testCommandOutputOccurrenceScopeMatrix(result);
    testLegacyCommandOutputScopeMatrix(result);
    testGeneratedOccurrenceAuthority(result);
    testExecutionConfigurationAuthority(result);
    return result.processResult();
}
