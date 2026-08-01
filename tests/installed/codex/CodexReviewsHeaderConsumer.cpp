// Compile the installed public header twice in one otherwise isolated
// translation unit to prove conventional include-guard behavior.
// clang-format off
#include <ai/openai/codex/typed/Reviews.h>
#include <ai/openai/codex/typed/Reviews.h>
// clang-format on

#include <string>
#include <type_traits>
#include <variant>

int main() {
    namespace codex = ai::openai::codex;
    namespace typed = ai::openai::codex::typed;

    using Start = codex::Submission (typed::Reviews::*)(typed::ReviewStartParams, typed::CompletionHandler<typed::ReviewStartResponse>);
    static_assert(std::is_same_v<decltype(&typed::Reviews::start), Start>);
    static_assert(std::variant_size_v<typed::ReviewTarget> == 5);
    static_assert(std::variant_size_v<typed::GuardianApprovalReviewAction> == 7);

    const typed::ReviewStartParams params{
        .threadId = {"thread-installed-review"},
        .target =
            typed::CommitReviewTarget{
                .sha = "0123456789abcdef",
                .title = typed::OptionalNullable<std::string>::withValue("Synthetic installed review"),
            },
        .delivery = typed::OptionalNullable<typed::ReviewDelivery>::withValue(typed::ReviewDelivery::detached()),
    };
    const typed::GuardianApprovalReviewAction action = typed::ApplyPatchGuardianApprovalReviewAction{
        .cwd = {"/synthetic/installed-review"},
        .files = {{"/synthetic/installed-review/a.cpp"}},
    };
    return params.delivery.value && *params.delivery.value == typed::ReviewDelivery::detached() &&
                   std::holds_alternative<typed::ApplyPatchGuardianApprovalReviewAction>(action)
               ? 0
               : 1;
}
