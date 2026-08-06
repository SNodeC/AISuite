/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_DETAIL_BOUNDOPERATION_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_DETAIL_BOUNDOPERATION_H

#include "ai/openai/codex/frontend/client/Results.h"

#include <any>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace ai::openai::codex::frontend::client::detail {

    // This private type-erasure boundary keeps the public façade signatures
    // domain-typed while letting Client own correlation and protocol-failure
    // containment for every operation uniformly.
    struct BoundOperationCompletion {
        using Decoder = std::function<bool(const frontend::generated::CompleteCommandResult&, std::any&, std::string&)>;
        using Success = std::function<void(const RequestId&, std::any&&)>;
        using Failure = std::function<void(const RequestId&, const Error&)>;

        Decoder decode;
        Success succeed;
        Failure fail;
    };

    template <typename Result, typename Decoder>
    BoundOperationCompletion bindCompletion(CompletionHandler<Result> handler, Decoder decoder) {
        BoundOperationCompletion completion;
        completion.decode = [decoder = std::move(decoder)](const frontend::generated::CompleteCommandResult& generated,
                                                           std::any& decoded,
                                                           std::string& error) mutable {
            std::optional<Result> value = decoder(generated, error);
            if (!value) {
                return false;
            }
            decoded = std::move(*value);
            return true;
        };
        completion.succeed = [handler](const RequestId& requestId, std::any&& decoded) {
            if (handler) {
                handler(OperationResult<Result>{requestId, std::any_cast<Result>(std::move(decoded)), std::nullopt});
            }
        };
        completion.fail = [handler](const RequestId& requestId, const Error& error) {
            if (handler) {
                handler(OperationResult<Result>{requestId, std::nullopt, error});
            }
        };
        return completion;
    }

} // namespace ai::openai::codex::frontend::client::detail

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_DETAIL_BOUNDOPERATION_H
