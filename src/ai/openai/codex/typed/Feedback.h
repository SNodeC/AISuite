/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_FEEDBACK_H
#define AI_OPENAI_CODEX_TYPED_FEEDBACK_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/Results.h"
#include "ai/openai/codex/typed/Types.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ai::openai::codex::typed {

    struct FeedbackUploadParams {
        std::string classification;
        OptionalNullable<std::vector<std::string>> extraLogFiles;
        std::optional<bool> includeLogs;
        OptionalNullable<std::string> reason;
        OptionalNullable<std::map<std::string, std::string>> tags;
        OptionalNullable<ThreadId> threadId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const FeedbackUploadParams&) const = default;
    };

    struct FeedbackUploadResponse {
        ThreadId threadId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const FeedbackUploadResponse&) const = default;
    };

    class Feedback {
    public:
        using Submission = AppServerClient::RawProtocol::Submission;
        using UploadResult = OperationResult<FeedbackUploadResponse>;
        using UploadResultHandler = std::function<void(const UploadResult&)>;

        Submission upload(FeedbackUploadParams params, UploadResultHandler handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Feedback(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_FEEDBACK_H
