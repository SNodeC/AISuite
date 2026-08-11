/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef TESTS_COMPONENT_CODEX_CODEXFRONTENDDIFFERENTIALEXECUTIONLEDGER_H
#define TESTS_COMPONENT_CODEX_CODEXFRONTENDDIFFERENTIALEXECUTIONLEDGER_H

#include "ai/openai/codex/frontend/Protocol.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace tests::codex {

    // Differential tests call matched() at the exact typed comparison border.
    // A failed comparison is never entered into the ledger, so the subsequent
    // authority guard cannot confuse a visited loop member with a proven case.
    class FrontendDifferentialExecutionLedger {
    public:
        explicit FrontendDifferentialExecutionLedger(std::string suiteIdentity)
            : suite(std::move(suiteIdentity)) {
            if (suite != "server" && suite != "client") {
                throw std::invalid_argument("frontend differential ledger suite must be server or client");
            }
        }

        [[nodiscard]] bool matched(std::string caseIdentity, bool comparisonMatched) {
            validateIdentity(caseIdentity);
            if (comparisonMatched) {
                entries.insert(std::move(caseIdentity));
            }
            return comparisonMatched;
        }

        void cover(std::string caseIdentity) {
            validateIdentity(caseIdentity);
            entries.insert(std::move(caseIdentity));
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return entries.size();
        }

        void write(const std::filesystem::path& destination) const {
            if (destination.empty()) {
                throw std::invalid_argument("frontend differential ledger destination is empty");
            }
            std::filesystem::create_directories(destination.parent_path());
            const std::filesystem::path temporary = destination.string() + ".tmp";
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("cannot create frontend differential execution ledger");
            }
            ai::openai::codex::frontend::Json document{
                {"formatVersion", 1},
                {"suite", suite},
                {"status", "passed"},
                {"cases", ai::openai::codex::frontend::Json::array()},
            };
            for (const std::string& identity : entries) {
                document["cases"].push_back(identity);
            }
            output << document.dump(2) << '\n';
            output.close();
            if (!output) {
                throw std::runtime_error("cannot finish frontend differential execution ledger");
            }
            std::filesystem::rename(temporary, destination);
        }

    private:
        static void validateIdentity(std::string_view identity) {
            if (identity.empty() || identity.size() > 512 || identity.find('\0') != std::string_view::npos ||
                identity.find('\n') != std::string_view::npos || identity.find('\r') != std::string_view::npos) {
                throw std::invalid_argument("frontend differential coverage identity is invalid");
            }
        }

        std::string suite;
        std::set<std::string, std::less<>> entries;
    };

} // namespace tests::codex

#endif // TESTS_COMPONENT_CODEX_CODEXFRONTENDDIFFERENTIALEXECUTIONLEDGER_H
