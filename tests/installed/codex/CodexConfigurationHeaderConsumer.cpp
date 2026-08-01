// Compile the installed public header twice in one otherwise isolated
// translation unit to prove conventional include-guard behavior.
// clang-format off
#include <ai/openai/codex/typed/Configuration.h>
#include <ai/openai/codex/typed/Configuration.h>
// clang-format on

#include <type_traits>

int main() {
    namespace codex = ai::openai::codex;
    namespace typed = ai::openai::codex::typed;

    using BatchWrite =
        codex::Submission (typed::Configuration::*)(typed::ConfigBatchWriteParams, typed::CompletionHandler<typed::ConfigWriteResponse>);
    using ReloadMcp = codex::Submission (typed::Configuration::*)(typed::DoneHandler);
    using Read = codex::Submission (typed::Configuration::*)(typed::ConfigReadParams, typed::CompletionHandler<typed::ConfigReadResponse>);
    using ReadRequirements = codex::Submission (typed::Configuration::*)(typed::CompletionHandler<typed::ConfigRequirementsReadResponse>);
    using WriteValue =
        codex::Submission (typed::Configuration::*)(typed::ConfigValueWriteParams, typed::CompletionHandler<typed::ConfigWriteResponse>);
    using SetFeature = codex::Submission (typed::Configuration::*)(
        typed::ExperimentalFeatureEnablementSetParams, typed::CompletionHandler<typed::ExperimentalFeatureEnablementSetResponse>);
    using ListFeatures = codex::Submission (typed::Configuration::*)(typed::ExperimentalFeatureListParams,
                                                                     typed::CompletionHandler<typed::ExperimentalFeatureListResponse>);

    static_assert(std::is_same_v<decltype(&typed::Configuration::batchWrite), BatchWrite>);
    static_assert(std::is_same_v<decltype(&typed::Configuration::reloadMcpServers), ReloadMcp>);
    static_assert(std::is_same_v<decltype(static_cast<Read>(&typed::Configuration::read)), Read>);
    static_assert(std::is_same_v<decltype(&typed::Configuration::readRequirements), ReadRequirements>);
    static_assert(std::is_same_v<decltype(&typed::Configuration::writeValue), WriteValue>);
    static_assert(std::is_same_v<decltype(&typed::Configuration::setExperimentalFeatureEnablement), SetFeature>);
    static_assert(std::is_same_v<decltype(static_cast<ListFeatures>(&typed::Configuration::listExperimentalFeatures)), ListFeatures>);

    typed::ConfigValueWriteParams value;
    value.keyPath = typed::ConfigKeyPath{"installed.header.check"};
    value.mergeStrategy = typed::MergeStrategy::replace();
    value.value = codex::Json::array({true, nullptr, codex::Json{{"exact", 1}}});
    return value.expectedVersion.isOmitted() && value.value.has_value() ? 0 : 1;
}
