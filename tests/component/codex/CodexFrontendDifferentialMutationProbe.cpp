/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "CodexFrontendDifferentialComparison.h"

#include <exception>
#include <iostream>
#include <string>

int main() {
    namespace frontend = ai::openai::codex::frontend;
    try {
        const frontend::Json input = frontend::Json::parse(std::cin);
        const auto mismatch =
            tests::codex::differential::firstMismatch(input.at("oracle"), input.at("candidate"));
        const frontend::Json output = mismatch
                                          ? frontend::Json{{"path", mismatch->path},
                                                           {"oldValue", mismatch->oldValue},
                                                           {"newValue", mismatch->newValue}}
                                          : frontend::Json(nullptr);
        std::cout << output.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CodexFrontendDifferentialMutationProbe: " << error.what() << '\n';
        return 1;
    }
}
