/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_BACKEND_INTERNAL_PROVIDEROPERATIONSUPPORT_H
#define AI_OPENAI_CODEX_BACKEND_INTERNAL_PROVIDEROPERATIONSUPPORT_H

#include "ai/openai/codex/backend/BackendCommand.h"
#include "ai/openai/codex/typed/Results.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <variant>

namespace ai::openai::codex::backend::detail {

    inline std::size_t saturatingByteSum(std::size_t left, std::size_t right) noexcept {
        return right > std::numeric_limits<std::size_t>::max() - left ? std::numeric_limits<std::size_t>::max() : left + right;
    }

    inline std::size_t serializedJsonBytes(const Json& value) noexcept {
        try {
            return value.dump().size();
        } catch (...) {
            return std::numeric_limits<std::size_t>::max();
        }
    }

    template <typename T>
    std::size_t providerOperationRetainedBytes(const T& value) noexcept {
        using Value = std::remove_cvref_t<T>;
        if constexpr (std::is_same_v<Value, typed::Unit>) {
            return 0;
        } else if constexpr (requires { std::variant_size<Value>::value; }) {
            return std::visit(
                [](const auto& alternative) {
                    return providerOperationRetainedBytes(alternative);
                },
                value);
        } else if constexpr (requires { value.raw; }) {
            const std::size_t wireBytes = serializedJsonBytes(value.raw);
            if (wireBytes == std::numeric_limits<std::size_t>::max()) {
                return wireBytes;
            }

            // Successful typed results retain both their exact raw JSON and a
            // decoded typed projection. Count the variable wire payload twice
            // and add the fixed typed object so the in-process completion queue
            // never treats a duplicated large payload as a single copy.
            return saturatingByteSum(sizeof(Value), saturatingByteSum(wireBytes, wireBytes));
        } else {
            // A future result without exact raw data cannot be safely bounded
            // by the current queue-accounting model.
            return std::numeric_limits<std::size_t>::max();
        }
    }

    template <typename T>
    CommandResult providerOperationFailure(const typed::OperationResult<T>& result) {
        using Kind = typename typed::OperationResult<T>::Kind;
        switch (result.kind) {
            case Kind::RemoteError:
                return CommandResult::failed(CommandErrorCode::RemoteAppServerError,
                                             result.remoteError ? result.remoteError->message
                                                                : "The Codex App Server rejected the operation.",
                                             result.remoteError ? std::optional<std::int64_t>{result.remoteError->code} : std::nullopt);
            case Kind::Cancelled:
                return CommandResult::failed(CommandErrorCode::Cancelled,
                                             result.localError ? result.localError->message : "The Codex operation was cancelled.");
            case Kind::LocalError:
                return CommandResult::failed(result.localError && result.localError->category == Error::Category::Protocol
                                                 ? CommandErrorCode::TypedDecodingFailure
                                                 : CommandErrorCode::LocalSubmissionFailure,
                                             result.localError ? result.localError->message
                                                               : "The typed Codex result could not be processed.");
            case Kind::Success:
                break;
        }
        return CommandResult::failed(CommandErrorCode::TypedDecodingFailure, "The typed Codex result omitted its value.");
    }

} // namespace ai::openai::codex::backend::detail

#endif // AI_OPENAI_CODEX_BACKEND_INTERNAL_PROVIDEROPERATIONSUPPORT_H
