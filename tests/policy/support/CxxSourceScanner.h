/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * Adapted for AISuite from the tokenizer and literal-masking machinery in
 * SNode.C tests/policy/log/LoggingApiSurfacePolicyTest.cpp and
 * tests/policy/log/ParameterlessSemanticLoggerPolicyTest.cpp.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef TESTS_POLICY_SUPPORT_CXXSOURCESCANNER_H
#define TESTS_POLICY_SUPPORT_CXXSOURCESCANNER_H

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aisuite::source_policy::cxx {

    struct Token {
        std::string text;
        std::size_t position;
    };

    inline bool isIdentifierStart(char character) {
        const auto value = static_cast<unsigned char>(character);
        return std::isalpha(value) != 0 || character == '_';
    }

    inline bool isIdentifierCharacter(char character) {
        const auto value = static_cast<unsigned char>(character);
        return std::isalnum(value) != 0 || character == '_';
    }

    inline void maskRange(std::string& masked, std::size_t begin, std::size_t end) {
        for (std::size_t position = begin; position < end; ++position) {
            if (masked[position] != '\n' && masked[position] != '\r') {
                masked[position] = ' ';
            }
        }
    }

    inline void maskLiteral(std::string& masked, std::size_t begin, std::size_t end) {
        maskRange(masked, begin, end);
        if (begin < end) {
            masked[begin] = '@';
        }
    }

    inline std::size_t prefixedLiteralQuote(std::string_view source, std::size_t position, char quote) {
        if (source[position] == quote) {
            return position;
        }
        for (const std::string_view prefix :
             {std::string_view{"u8"}, std::string_view{"u"}, std::string_view{"U"}, std::string_view{"L"}}) {
            if (source.substr(position).starts_with(prefix) && position + prefix.size() < source.size() &&
                source[position + prefix.size()] == quote) {
                return position + prefix.size();
            }
        }
        return std::string_view::npos;
    }

    inline std::pair<std::size_t, std::size_t> rawLiteralBounds(std::string_view source, std::size_t position) {
        for (const std::string_view prefix :
             {std::string_view{"R"}, std::string_view{"u8R"}, std::string_view{"uR"}, std::string_view{"UR"}, std::string_view{"LR"}}) {
            if (!source.substr(position).starts_with(prefix) || position + prefix.size() >= source.size() ||
                source[position + prefix.size()] != '"') {
                continue;
            }

            const std::size_t delimiterBegin = position + prefix.size() + 1;
            const std::size_t open = source.find('(', delimiterBegin);
            if (open == std::string_view::npos || open - delimiterBegin > 16) {
                return {position, source.size()};
            }
            const std::string delimiter(source.substr(delimiterBegin, open - delimiterBegin));
            const std::string close = ")" + delimiter + '"';
            const std::size_t closePosition = source.find(close, open + 1);
            return {position, closePosition == std::string_view::npos ? source.size() : closePosition + close.size()};
        }
        return {std::string_view::npos, std::string_view::npos};
    }

    inline std::string maskCommentsAndLiterals(std::string_view source) {
        std::string masked(source);
        std::size_t position = 0;
        while (position < source.size()) {
            if (source.substr(position).starts_with("//")) {
                const std::size_t end = source.find('\n', position + 2);
                const std::size_t maskedEnd = end == std::string_view::npos ? source.size() : end;
                maskRange(masked, position, maskedEnd);
                position = maskedEnd;
                continue;
            }
            if (source.substr(position).starts_with("/*")) {
                const std::size_t end = source.find("*/", position + 2);
                const std::size_t maskedEnd = end == std::string_view::npos ? source.size() : end + 2;
                maskRange(masked, position, maskedEnd);
                position = maskedEnd;
                continue;
            }

            const bool prefixBoundary = position == 0 || !isIdentifierCharacter(source[position - 1]);
            if (prefixBoundary) {
                const auto [rawBegin, rawEnd] = rawLiteralBounds(source, position);
                if (rawBegin != std::string_view::npos) {
                    maskLiteral(masked, rawBegin, rawEnd);
                    position = rawEnd;
                    continue;
                }

                std::size_t quotePosition = prefixedLiteralQuote(source, position, '"');
                if (quotePosition == std::string_view::npos) {
                    quotePosition = prefixedLiteralQuote(source, position, '\'');
                }
                if (quotePosition != std::string_view::npos) {
                    const char quote = source[quotePosition];
                    std::size_t end = quotePosition + 1;
                    while (end < source.size()) {
                        if (source[end] == '\\') {
                            end = std::min(source.size(), end + 2);
                        } else if (source[end++] == quote) {
                            break;
                        }
                    }
                    maskLiteral(masked, position, end);
                    position = end;
                    continue;
                }
            }
            ++position;
        }
        return masked;
    }

    inline std::vector<Token> tokenize(std::string_view source) {
        const std::string masked = maskCommentsAndLiterals(source);
        std::vector<Token> tokens;
        for (std::size_t position = 0; position < masked.size();) {
            if (isIdentifierStart(masked[position])) {
                const std::size_t begin = position++;
                while (position < masked.size() && isIdentifierCharacter(masked[position])) {
                    ++position;
                }
                tokens.push_back({masked.substr(begin, position - begin), begin});
            } else if (masked.substr(position).starts_with("::")) {
                tokens.push_back({"::", position});
                position += 2;
            } else if (std::isspace(static_cast<unsigned char>(masked[position])) != 0) {
                ++position;
            } else {
                tokens.push_back({std::string(1, masked[position]), position});
                ++position;
            }
        }
        return tokens;
    }

    inline std::size_t tokenCount(const std::vector<Token>& tokens, std::string_view text) {
        return static_cast<std::size_t>(std::count_if(tokens.begin(), tokens.end(), [text](const Token& token) {
            return token.text == text;
        }));
    }

    inline bool scannerSelfTest() {
        constexpr std::string_view source = R"source(
            int lifecycleStart = 0;
            int lifecycleStartSuffix = 0;
            // creationLogged lifecycleStarted lifecycleTerminalLogged
            /* creationLogged lifecycleStarted lifecycleTerminalLogged */
            const char* ordinary = "creationLogged";
            const char* prefixed = u8"lifecycleStarted";
            const char* raw = R"tag(lifecycleTerminalLogged)tag";
            const char character = 'x';
        )source";
        const std::vector<Token> tokens = tokenize(source);
        return tokenCount(tokens, "lifecycleStart") == 1 && tokenCount(tokens, "lifecycleStartSuffix") == 1 &&
               tokenCount(tokens, "creationLogged") == 0 && tokenCount(tokens, "lifecycleStarted") == 0 &&
               tokenCount(tokens, "lifecycleTerminalLogged") == 0;
    }

} // namespace aisuite::source_policy::cxx

#endif // TESTS_POLICY_SUPPORT_CXXSOURCESCANNER_H
