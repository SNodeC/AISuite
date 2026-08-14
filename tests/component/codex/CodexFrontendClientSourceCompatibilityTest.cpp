/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/Client.h"
#include "ai/openai/codex/typed/Accounts.h"
#include "ai/openai/codex/typed/Configuration.h"
#include "ai/openai/codex/typed/Models.h"
#include "ai/openai/codex/typed/Types.h"

#include <concepts>
#include <optional>

namespace typed = ai::openai::codex::typed;
namespace client = ai::openai::codex::frontend::client;

static_assert(requires(const client::ItemState& item) {
    { client::userMessageSemanticView(item) } -> std::same_as<std::optional<client::UserMessageSemanticView>>;
});

int main() {
    const typed::AuthMode authentication = typed::AuthMode::chatgpt();
    const typed::AutoCompactTokenLimitScope compactScope =
        typed::AutoCompactTokenLimitScope::total();
    const typed::InputModality modality = typed::InputModality::text();
    const typed::ReasoningEffort effort = typed::ReasoningEffort::high();
    const typed::TurnStatus turn = typed::TurnStatus::completed();

    return authentication.value == "chatgpt" && authentication.isKnown() &&
                   compactScope.value == "total" && compactScope.isKnown() &&
                   modality.value == "text" && modality.isKnown() &&
                   effort.value == "high" && effort.isKnown() &&
                   turn.value == "completed" && turn.isKnown()
               ? 0
               : 1;
}
