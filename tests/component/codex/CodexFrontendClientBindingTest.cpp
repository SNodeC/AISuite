/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/Accounts.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "ai/openai/codex/frontend/client/Connection.h"
#include "ai/openai/codex/frontend/client/Controller.h"
#include "ai/openai/codex/frontend/client/GeneratedBindings.h"
#include "ai/openai/codex/frontend/client/Requests.h"
#include "ai/openai/codex/frontend/client/State.h"
#include "support/TestResult.h"

#include <concepts>
#include <set>
#include <string_view>
#include <type_traits>

namespace {
    namespace client = ai::openai::codex::frontend::client;

    void testBindingAuthority(tests::support::TestResult& result) {
        std::set<ai::openai::codex::frontend::generated::MethodId> methods;
        std::size_t native = 0;
        std::size_t provider = 0;
        std::size_t reverse = 0;
        for (const client::generated::BindingMetadata& binding : client::generated::AllBindings) {
            methods.insert(binding.method);
            native += binding.facade == "Controller" || binding.facade == "Provider" || binding.facade == "Synchronization" ? 1U : 0U;
            reverse += binding.facade == "Requests" ? 1U : 0U;
            provider += binding.facade != "Controller" && binding.facade != "Provider" && binding.facade != "Synchronization" &&
                                binding.facade != "Requests"
                            ? 1U
                            : 0U;
        }
        result.expectTrue(
            methods.size() == 105 && native == 7 && provider == 86 && reverse == 12,
            "the reviewed C++ authority binds every MethodId exactly once as 7 native, 86 provider, and 12 Requests operations");
    }

    void testFundamentalTypeRules(tests::support::TestResult& result) {
        constexpr bool rules = !std::copy_constructible<client::Client> && !std::move_constructible<client::Client> &&
                               !std::copy_constructible<client::Connection> && std::move_constructible<client::Connection> &&
                               std::copy_constructible<client::State> && std::move_constructible<client::State> &&
                               !std::copy_constructible<client::Controller> && !std::move_constructible<client::Controller> &&
                               !std::copy_constructible<client::Requests> && !std::move_constructible<client::Requests>;
        result.expectTrue(
            rules, "Client is fixed-address, Connection is move-only, State is cheap-copy capable, and facade references are stable");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    static_assert(client::generated::NativeBindingCount == 7);
    static_assert(client::generated::ProviderBindingCount == 86);
    static_assert(client::generated::ReverseBindingCount == 12);
    testBindingAuthority(result);
    testFundamentalTypeRules(result);
    return result.processResult();
}
