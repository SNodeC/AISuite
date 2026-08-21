/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/RetainedDeliveryRetryBackoff.h"
#include "support/TestResult.h"

#include <array>
#include <cstddef>

int main() {
    tests::support::TestResult result;
    apps::codex_backend::RetainedDeliveryRetryBackoff backoff;

    std::array<std::size_t, 7> delays{};
    for (std::size_t index = 0; index < delays.size(); ++index) {
        delays[index] = backoff.recordBackpressure(4096);
    }
    result.expectTrue(delays == std::array<std::size_t, 7>{5, 10, 20, 40, 80, 80, 80} &&
                          backoff.nextDelayMilliseconds() == 80 && backoff.consecutiveRetryCount() == delays.size(),
                      "retained delivery retries rise from 5 to 80 ms and remain deterministically capped");

    const std::size_t progressDelay = backoff.recordBackpressure(2048);
    result.expectTrue(progressDelay == 5 && backoff.nextDelayMilliseconds() == 10 &&
                          backoff.consecutiveRetryCount() == 1,
                      "observable writer progress immediately restores the shortest retry delay");

    const std::size_t unchangedAfterProgress = backoff.recordBackpressure(2048);
    result.expectTrue(unchangedAfterProgress == 10 && backoff.consecutiveRetryCount() == 2,
                      "an unchanged retained writer resumes exponential backoff after progress");

    backoff.recordAccepted();
    result.expectTrue(backoff.nextDelayMilliseconds() == 5 && backoff.consecutiveRetryCount() == 0 &&
                          backoff.recordBackpressure(8192) == 5,
                      "an accepted head resets retry state for prompt delivery of the next retained head");

    backoff.reset();
    result.expectTrue(backoff.nextDelayMilliseconds() == 5 && backoff.consecutiveRetryCount() == 0 &&
                          backoff.recordBackpressure(16384) == 5,
                      "a connection reset discards prior retry history and starts the replacement connection promptly");

    return result.processResult();
}
