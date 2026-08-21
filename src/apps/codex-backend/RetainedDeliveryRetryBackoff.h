/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BACKEND_RETAINEDDELIVERYRETRYBACKOFF_H
#define APPS_CODEX_BACKEND_RETAINEDDELIVERYRETRYBACKOFF_H

#include <cstddef>
#include <limits>

namespace apps::codex_backend {

    // A retained frontend head is retried slowly enough to let the bounded
    // transport writer drain, but immediately returns to the shortest delay
    // after observable progress. The owner remains responsible for retaining
    // the exact payload and for cancelling retries through its lifetime guard.
    class RetainedDeliveryRetryBackoff {
    public:
        static constexpr std::size_t InitialDelayMilliseconds = 5;
        static constexpr std::size_t MaximumDelayMilliseconds = 80;

        [[nodiscard]] std::size_t recordBackpressure(std::size_t outstandingWriterBytes) noexcept {
            if (hasWriterObservation && outstandingWriterBytes < lastOutstandingWriterBytes) {
                resetSequence();
            }

            hasWriterObservation = true;
            lastOutstandingWriterBytes = outstandingWriterBytes;

            const std::size_t delay = nextDelayMs;
            if (nextDelayMs < MaximumDelayMilliseconds) {
                nextDelayMs *= 2;
            }
            if (consecutiveRetries < std::numeric_limits<std::size_t>::max()) {
                ++consecutiveRetries;
            }
            return delay;
        }

        void recordAccepted() noexcept {
            reset();
        }

        void reset() noexcept {
            resetSequence();
            hasWriterObservation = false;
            lastOutstandingWriterBytes = 0;
        }

        [[nodiscard]] std::size_t nextDelayMilliseconds() const noexcept {
            return nextDelayMs;
        }

        [[nodiscard]] std::size_t consecutiveRetryCount() const noexcept {
            return consecutiveRetries;
        }

    private:
        void resetSequence() noexcept {
            nextDelayMs = InitialDelayMilliseconds;
            consecutiveRetries = 0;
        }

        std::size_t nextDelayMs = InitialDelayMilliseconds;
        std::size_t consecutiveRetries = 0;
        std::size_t lastOutstandingWriterBytes = 0;
        bool hasWriterObservation = false;
    };

} // namespace apps::codex_backend

#endif // APPS_CODEX_BACKEND_RETAINEDDELIVERYRETRYBACKOFF_H
