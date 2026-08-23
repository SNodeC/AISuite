/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AISUITE_TESTS_CODEX2_TESTHARNESS_H
#define AISUITE_TESTS_CODEX2_TESTHARNESS_H

#include <iostream>
#include <string_view>

namespace tests::codex2 {

    class TestHarness {
    public:
        void expect(bool condition, std::string_view description) {
            if (condition) {
                std::cout << "PASS: " << description << '\n';
            } else {
                std::cerr << "FAIL: " << description << '\n';
                ++failures_;
            }
        }

        template <typename Actual, typename Expected>
        void expectEqual(const Actual& actual, const Expected& expected, std::string_view description) {
            expect(actual == expected, description);
        }

        int result() const noexcept {
            return failures_ == 0 ? 0 : 1;
        }

    private:
        unsigned int failures_ = 0;
    };

} // namespace tests::codex2

#endif
