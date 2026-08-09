/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/model/Model.h"
#include "support/TestResult.h"

#include <string>
#include <variant>

namespace {
    namespace model = ai::openai::codex::frontend::internal::model;
    using ai::openai::codex::frontend::Json;

    void testStrongIdentitiesAndSafeDetail(tests::support::TestResult& result) {
        result.expectTrue(model::ThreadIdentity::parse("thread-1").has_value() &&
                              !model::ThreadIdentity::parse("").has_value() &&
                              !model::ThreadIdentity::parse(std::string(1'025, 'x')).has_value(),
                          "strong frontend identities reject empty and oversized values");

        model::SafeDetailError error = model::SafeDetailError::None;
        const auto safe = model::SafeDetail::fromJson(Json{{"tokenUsage", 7}, {"isSecret", false}}, &error);
        const auto secret = model::SafeDetail::fromJson(Json{{"nested", {{"private_key", "not-retained"}}}}, &error);
        const auto raw = model::SafeDetail::fromJson(Json{{"rawProviderEnvelope", Json::object()}}, &error);
        result.expectTrue(safe.has_value() && !secret.has_value() && !raw.has_value(),
                          "safe detail retains accounting metadata while recursively rejecting secret and raw-provider keys");
    }

    void testTypedVariantsAndInvalidation(tests::support::TestResult& result) {
        static_assert(std::variant_size_v<model::ThreadItem> == 18);
        static_assert(std::variant_size_v<model::PendingRequest> == 10);
        auto pendingId = model::PendingRequestIdentity::parse("1");
        model::PendingRequestData data{*pendingId};
        data.connectionInvalidated = true;
        model::CanonicalSnapshot snapshot;
        snapshot.pendingRequests.push_back(model::CommandExecutionApprovalRequest{std::move(data)});
        const auto encoded = model::encodeSnapshot(snapshot);
        const auto decoded = encoded ? model::decodeSnapshot(encoded.value()) : model::ModelResult<model::CanonicalSnapshot>{encoded.error()};
        result.expectTrue(decoded && decoded.value().pendingRequests.size() == 1 &&
                              model::pendingRequestData(decoded.value().pendingRequests.front()).connectionInvalidated,
                          "pending requests retain the old-connection invalidation marker through the canonical wire boundary");
    }
}

int main() {
    tests::support::TestResult result;
    testStrongIdentitiesAndSafeDetail(result);
    testTypedVariantsAndInvalidation(result);
    return result.processResult();
}
