/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_RESULTS_H
#define AI_OPENAI_CODEX_TYPED_RESULTS_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/CodexErrorInfo.h"

#include <functional>
#include <optional>

namespace ai::openai::codex::typed {

    // The single successful-result type used by every pinned empty-object
    // operation contract. Its decoder accepts only the exact empty object.
    struct Unit {
        bool operator==(const Unit&) const = default;
    };

    template <typename T>
    struct OperationResult {
        enum class Kind { Success, RemoteError, Cancelled, LocalError };

        Kind kind = Kind::Success;
        std::optional<T> value;
        std::optional<ProtocolError> remoteError;
        std::optional<Error> localError;
        std::optional<ClientRequestId> requestId;
        Json raw = nullptr;
        std::optional<CodexErrorInfo> codexErrorInfo;
        std::optional<DecodeDiagnostic> codexErrorDiagnostic;

        explicit operator bool() const noexcept {
            return kind == Kind::Success && value.has_value();
        }

        bool isSuccess() const noexcept {
            return static_cast<bool>(*this);
        }

        bool isRemoteError() const noexcept {
            return kind == Kind::RemoteError;
        }

        bool isCancelled() const noexcept {
            return kind == Kind::Cancelled;
        }

        bool isLocalError() const noexcept {
            return kind == Kind::LocalError;
        }

        T& operator*() & {
            return *value;
        }

        const T& operator*() const& {
            return *value;
        }

        T* operator->() {
            return value.operator->();
        }

        const T* operator->() const {
            return value.operator->();
        }
    };

    template <typename T>
    using CompletionHandler = std::function<void(const OperationResult<T>&)>;

    using DoneHandler = CompletionHandler<Unit>;

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_RESULTS_H
