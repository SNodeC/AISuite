// Compile the installed public header twice in one otherwise isolated
// translation unit to prove conventional include-guard behavior.
// clang-format off
#include <ai/openai/codex/typed/Models.h>
#include <ai/openai/codex/typed/Models.h>
// clang-format on

#include <type_traits>

int main() {
    namespace codex = ai::openai::codex;
    namespace typed = ai::openai::codex::typed;

    using List = codex::Submission (typed::Models::*)(typed::ModelListParams, typed::CompletionHandler<typed::ModelListResponse>);
    using ReadProviderCapabilities =
        codex::Submission (typed::Models::*)(typed::CompletionHandler<typed::ModelProviderCapabilitiesReadResponse>);

    static_assert(std::is_same_v<decltype(static_cast<List>(&typed::Models::list)), List>);
    static_assert(std::is_same_v<decltype(&typed::Models::readProviderCapabilities), ReadProviderCapabilities>);

    const typed::ModelRerouteReason futureReason{"installed-future-reroute"};
    return futureReason.isKnown() ? 1 : 0;
}
