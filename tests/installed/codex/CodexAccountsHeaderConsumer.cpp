// Compile the installed public header twice in one otherwise isolated
// translation unit to prove conventional include-guard behavior.
// clang-format off
#include <ai/openai/codex/typed/Accounts.h>
#include <ai/openai/codex/typed/Accounts.h>
// clang-format on

#include <type_traits>

int main() {
    namespace codex = ai::openai::codex;
    namespace typed = ai::openai::codex::typed;

    using CancelLogin = codex::Submission (typed::Accounts::*)(typed::CancelLoginAccountParams,
                                                               typed::CompletionHandler<typed::CancelLoginAccountResponse>);
    using StartLogin =
        codex::Submission (typed::Accounts::*)(typed::LoginAccountParams, typed::CompletionHandler<typed::LoginAccountResponse>);
    using Logout = codex::Submission (typed::Accounts::*)(typed::DoneHandler);
    using ConsumeCredit = codex::Submission (typed::Accounts::*)(
        typed::ConsumeAccountRateLimitResetCreditParams, typed::CompletionHandler<typed::ConsumeAccountRateLimitResetCreditResponse>);
    using ReadRateLimits = codex::Submission (typed::Accounts::*)(typed::CompletionHandler<typed::GetAccountRateLimitsResponse>);
    using ReadAccount =
        codex::Submission (typed::Accounts::*)(typed::GetAccountParams, typed::CompletionHandler<typed::GetAccountResponse>);
    using SendNudge = codex::Submission (typed::Accounts::*)(typed::SendAddCreditsNudgeEmailParams,
                                                             typed::CompletionHandler<typed::SendAddCreditsNudgeEmailResponse>);
    using ReadUsage = codex::Submission (typed::Accounts::*)(typed::CompletionHandler<typed::GetAccountTokenUsageResponse>);
    using ReadMessages = codex::Submission (typed::Accounts::*)(typed::CompletionHandler<typed::GetWorkspaceMessagesResponse>);

    static_assert(std::is_same_v<decltype(&typed::Accounts::cancelLogin), CancelLogin>);
    static_assert(std::is_same_v<decltype(&typed::Accounts::startLogin), StartLogin>);
    static_assert(std::is_same_v<decltype(&typed::Accounts::logout), Logout>);
    static_assert(std::is_same_v<decltype(&typed::Accounts::consumeRateLimitResetCredit), ConsumeCredit>);
    static_assert(std::is_same_v<decltype(&typed::Accounts::readRateLimits), ReadRateLimits>);
    static_assert(std::is_same_v<decltype(static_cast<ReadAccount>(&typed::Accounts::read)), ReadAccount>);
    static_assert(std::is_same_v<decltype(&typed::Accounts::sendAddCreditsNudgeEmail), SendNudge>);
    static_assert(std::is_same_v<decltype(&typed::Accounts::readUsage), ReadUsage>);
    static_assert(std::is_same_v<decltype(&typed::Accounts::readWorkspaceMessages), ReadMessages>);

    const typed::ChatgptAuthTokensRefreshParams refresh{
        .previousAccountId = typed::OptionalNullable<typed::AccountId>::explicitNull(),
        .reason = typed::ChatgptAuthTokensRefreshReason::unauthorized(),
    };
    return refresh.previousAccountId.isNull() ? 0 : 1;
}
