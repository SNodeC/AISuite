/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * Codex-only extraction of SNode.C
 * tests/policy/log/ParameterlessSemanticLoggerPolicyTest.cpp, blob
 * e4fddcaf69b23549eab318cb86afee6210b2aaad.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "tests/policy/support/AISuiteSourcePolicyTestRoot.h"
#include "tests/policy/support/CxxSourceScanner.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    constexpr std::string_view kReducerPath = "src/ai/openai/codex/backend/Reducer.cpp";
    constexpr std::string_view kCodexPathPrefix = "src/ai/openai/codex/";
    constexpr std::string_view kDomainClassification = "DOMAIN_OR_PROTOCOL_SCOPE";
    constexpr std::string_view kTsvHeader = "path\thelper\texpression\tclassification\trationale";
    constexpr std::size_t kAuthorityEntryCount = 4;
    constexpr std::string_view kAuthorityEnvironment = "AISUITE_CODEX_SEMANTIC_LOGGER_AUTHORITY_FILE";
    constexpr std::string_view kClassificationsEnvironment = "AISUITE_CODEX_SEMANTIC_LOGGER_CLASSIFICATIONS_FILE";

    struct Entry {
        std::string path;
        std::string helper;
        std::string expression;
        std::string classification;
        std::string rationale;

        bool operator==(const Entry&) const = default;
    };

    struct Call {
        std::string path;
        std::string helper;
        std::size_t position;
    };

    using SourceMap = std::map<std::string, std::string>;

    void report(std::string_view code, std::string_view detail) {
        std::cerr << code << ": " << detail << '\n';
    }

    std::vector<Entry> pinnedAuthority() {
        return {
            Entry{std::string(kReducerPath),
                  "lifecycleLog",
                  "turn {}: thread={} turn={}",
                  std::string(kDomainClassification),
                  "Typed turn completion is owned by Codex thread and turn identifiers."},
            Entry{std::string(kReducerPath),
                  "lifecycleLog",
                  "turn failed: thread={} turn={}",
                  std::string(kDomainClassification),
                  "Typed turn failure is owned by Codex thread and turn identifiers."},
            Entry{std::string(kReducerPath),
                  "lifecycleLog",
                  "thread created: thread={}",
                  std::string(kDomainClassification),
                  "Typed thread creation is owned by the Codex thread identifier."},
            Entry{std::string(kReducerPath),
                  "lifecycleLog",
                  "turn started: thread={} turn={}",
                  std::string(kDomainClassification),
                  "Typed turn start is owned by Codex thread and turn identifiers."},
        };
    }

    std::vector<std::string> splitTsvRow(std::string_view row) {
        std::vector<std::string> fields;
        std::size_t begin = 0;
        while (true) {
            const std::size_t end = row.find('\t', begin);
            if (end == std::string_view::npos) {
                fields.emplace_back(row.substr(begin));
                break;
            }
            fields.emplace_back(row.substr(begin, end - begin));
            begin = end + 1;
        }
        return fields;
    }

    std::optional<std::vector<Entry>> readEntries(const std::filesystem::path& path, std::string& error) {
        std::ifstream input(path);
        if (!input) {
            error = "unable to open " + path.generic_string();
            return std::nullopt;
        }

        bool sawHeader = false;
        std::vector<Entry> entries;
        std::string line;
        std::size_t lineNumber = 0;
        while (std::getline(input, line)) {
            ++lineNumber;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || line.front() == '#') {
                continue;
            }
            if (!sawHeader) {
                if (line != kTsvHeader) {
                    error = path.generic_string() + ':' + std::to_string(lineNumber) + ": unexpected TSV header";
                    return std::nullopt;
                }
                sawHeader = true;
                continue;
            }

            std::vector<std::string> fields = splitTsvRow(line);
            if (fields.size() != 5 || std::any_of(fields.begin(), fields.end(), [](const std::string& field) {
                    return field.empty();
                })) {
                error = path.generic_string() + ':' + std::to_string(lineNumber) + ": expected five nonempty TSV fields";
                return std::nullopt;
            }
            entries.push_back(
                {std::move(fields[0]), std::move(fields[1]), std::move(fields[2]), std::move(fields[3]), std::move(fields[4])});
        }
        if (!sawHeader) {
            error = path.generic_string() + ": missing TSV header";
            return std::nullopt;
        }
        return entries;
    }

    std::optional<std::filesystem::path>
    configuredDataPath(const std::filesystem::path& root, std::string_view environment, std::string_view defaultName, std::string& error) {
        const std::string environmentName(environment);
        if (const char* overrideValue = std::getenv(environmentName.c_str()); overrideValue != nullptr) {
            const std::filesystem::path overridePath(overrideValue);
            if (overridePath.empty() || !overridePath.is_absolute()) {
                error = environmentName + " must name an absolute path";
                return std::nullopt;
            }
            return overridePath.lexically_normal();
        }
        return root / "tests/policy/codex" / defaultName;
    }

    bool validatePinnedAuthority(const std::vector<Entry>& entries) {
        const std::vector<Entry> expected = pinnedAuthority();
        if (entries.size() != kAuthorityEntryCount || expected.size() != kAuthorityEntryCount) {
            report("CodexPolicySemanticLoggerAuthorityMismatch",
                   "accepted entry count is " + std::to_string(entries.size()) + ", expected exactly 4");
            return false;
        }
        for (std::size_t index = 0; index < expected.size(); ++index) {
            if (entries[index] != expected[index]) {
                report("CodexPolicySemanticLoggerAuthorityMismatch",
                       "accepted entry " + std::to_string(index + 1) +
                           " does not match the pinned SNode.C path, helper, expression, classification, and rationale");
                return false;
            }
        }
        return true;
    }

    bool validateClassificationMetadata(const std::vector<Entry>& classifications, const std::vector<Entry>& authority) {
        if (classifications.size() != kAuthorityEntryCount || classifications.size() != authority.size()) {
            report("CodexPolicySemanticLoggerClassificationMismatch",
                   "runtime classification count is " + std::to_string(classifications.size()) + ", expected exactly 4");
            return false;
        }

        for (const Entry& entry : classifications) {
            const std::filesystem::path path(entry.path);
            if (path.is_absolute() || path.lexically_normal().generic_string() != entry.path ||
                !std::string_view(entry.path).starts_with(kCodexPathPrefix)) {
                report("CodexPolicySemanticLoggerClassificationMismatch",
                       "classification path lies outside the AISuite Codex source tree: " + entry.path);
                return false;
            }
            if (entry.classification != kDomainClassification) {
                report("CodexPolicySemanticLoggerClassificationMismatch",
                       "Codex lifecycle entry must remain protocol/domain scoped rather than transport-connection scoped: " + entry.path +
                           '|' + entry.expression);
                return false;
            }
            if (entry.rationale.empty()) {
                report("CodexPolicySemanticLoggerClassificationMismatch",
                       "classification rationale is empty: " + entry.path + '|' + entry.expression);
                return false;
            }
        }

        if (classifications != authority) {
            report("CodexPolicySemanticLoggerClassificationMismatch",
                   "runtime classifications do not bijectively preserve the four pinned accepted entries");
            return false;
        }
        return true;
    }

    bool endsWithLog(std::string_view helper) {
        return helper.size() >= 3 && helper.ends_with("Log");
    }

    std::vector<Call> findParameterlessCalls(std::string_view path, std::string_view source) {
        const std::vector<aisuite::source_policy::cxx::Token> tokens = aisuite::source_policy::cxx::tokenize(source);
        std::vector<Call> calls;
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            if (index + 4 < tokens.size() && tokens[index].text == "semantic" && tokens[index + 1].text == "::" &&
                endsWithLog(tokens[index + 2].text) && tokens[index + 3].text == "(" && tokens[index + 4].text == ")") {
                calls.push_back({std::string(path), tokens[index + 2].text, tokens[index].position});
                continue;
            }

            if (path == kReducerPath && index + 3 < tokens.size() && tokens[index].text == "lifecycleLog" &&
                tokens[index + 1].text == "(" && tokens[index + 2].text == ")" && tokens[index + 3].text == ".") {
                calls.push_back({std::string(path), "lifecycleLog", tokens[index].position});
            }
        }
        return calls;
    }

    bool semanticScannerSelfTest() {
        constexpr std::string_view genericSource = R"cpp(
            semantic::alphaLog /* permitted formatting */ ( /* empty */ );
            semantic::alphaLog(value);
            semantic::alphaLog("argument");
            semantic::alphaLogger();
            // semantic::commentLog()
            const char* text = "semantic::stringLog()";
            const char* raw = R"tag(semantic::rawStringLog())tag";
        )cpp";
        const std::vector<Call> genericCalls = findParameterlessCalls("src/ai/openai/codex/example.cpp", genericSource);
        if (genericCalls.size() != 1 || genericCalls.front().helper != "alphaLog") {
            return false;
        }

        constexpr std::string_view reducerSource = R"cpp(
            lifecycleLog /* permitted formatting */ ( /* empty */ ).debug("marker");
            lifecycleLog(value).debug("argument");
            otherLifecycleLog().debug("other");
            // lifecycleLog().debug("comment")
        )cpp";
        const std::vector<Call> reducerCalls = findParameterlessCalls(kReducerPath, reducerSource);
        return reducerCalls.size() == 1 && reducerCalls.front().helper == "lifecycleLog";
    }

    bool readCodexSources(const std::filesystem::path& root, SourceMap& sources, std::string& error) {
        const std::filesystem::path codexRoot = root / "src/ai/openai/codex";
        if (!std::filesystem::is_directory(codexRoot)) {
            error = "Codex source root is missing: " + codexRoot.generic_string();
            return false;
        }

        std::vector<std::filesystem::path> sourcePaths;
        try {
            for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(codexRoot)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                const std::string extension = entry.path().extension().string();
                if (extension == ".cpp" || extension == ".h" || extension == ".hpp" || extension == ".ipp") {
                    sourcePaths.push_back(entry.path());
                }
            }
        } catch (const std::filesystem::filesystem_error& exception) {
            error = "unable to enumerate Codex sources: " + std::string(exception.what());
            return false;
        }

        std::sort(sourcePaths.begin(), sourcePaths.end());
        for (const std::filesystem::path& sourcePath : sourcePaths) {
            const std::string relative = std::filesystem::relative(sourcePath, root).lexically_normal().generic_string();
            if (!std::string_view(relative).starts_with(kCodexPathPrefix)) {
                error = "enumerated source escaped the Codex tree: " + relative;
                return false;
            }
            const std::string source = aisuite::source_policy::readFile(sourcePath);
            if (source.empty()) {
                error = "unable to read Codex source: " + sourcePath.generic_string();
                return false;
            }
            sources.emplace(relative, source);
        }
        if (sources.empty()) {
            error = "Codex source scan found no production C++ files";
            return false;
        }
        return true;
    }

    std::vector<Call> discover(const SourceMap& sources) {
        std::vector<Call> calls;
        for (const auto& [path, source] : sources) {
            std::vector<Call> fileCalls = findParameterlessCalls(path, source);
            calls.insert(calls.end(), fileCalls.begin(), fileCalls.end());
        }
        return calls;
    }

    std::size_t occurrenceCount(std::string_view source, std::string_view marker) {
        std::size_t count = 0;
        std::size_t position = 0;
        while ((position = source.find(marker, position)) != std::string_view::npos) {
            ++count;
            position += marker.size();
        }
        return count;
    }

    std::string entryIdentity(const Entry& entry) {
        return entry.path + '|' + entry.helper + '|' + entry.expression;
    }

    bool validateDiscoveredCalls(const SourceMap& sources, const std::vector<Call>& calls, const std::vector<Entry>& classifications) {
        std::vector<std::size_t> matchedBy(calls.size(), 0);
        bool structuralMismatch = false;

        for (const Entry& entry : classifications) {
            const std::string identity = entryIdentity(entry);
            const auto source = sources.find(entry.path);
            if (source == sources.end()) {
                report("CodexPolicySemanticLoggerClassificationMismatch", "stale accepted source path: " + identity);
                structuralMismatch = true;
                continue;
            }
            if (occurrenceCount(source->second, entry.expression) != 1) {
                report("CodexPolicySemanticLoggerClassificationMismatch",
                       "accepted identifying expression is missing or non-unique: " + identity);
                structuralMismatch = true;
                continue;
            }

            const std::size_t markerPosition = source->second.find(entry.expression);
            std::size_t bestCall = std::numeric_limits<std::size_t>::max();
            std::size_t bestDistance = std::numeric_limits<std::size_t>::max();
            bool tied = false;
            for (std::size_t index = 0; index < calls.size(); ++index) {
                if (calls[index].path != entry.path || calls[index].helper != entry.helper) {
                    continue;
                }
                const std::size_t distance = calls[index].position > markerPosition ? calls[index].position - markerPosition
                                                                                    : markerPosition - calls[index].position;
                if (distance < bestDistance) {
                    bestCall = index;
                    bestDistance = distance;
                    tied = false;
                } else if (distance == bestDistance) {
                    tied = true;
                }
            }
            if (bestCall == std::numeric_limits<std::size_t>::max() || tied || bestDistance > 2048) {
                report("CodexPolicySemanticLoggerClassificationMismatch", "stale accepted logger call: " + identity);
                structuralMismatch = true;
                continue;
            }
            ++matchedBy[bestCall];
        }

        if (structuralMismatch) {
            return false;
        }

        bool unclassified = false;
        for (std::size_t index = 0; index < calls.size(); ++index) {
            if (matchedBy[index] == 0) {
                report("CodexPolicySemanticLoggerUnclassified",
                       calls[index].path + '|' + calls[index].helper + '@' + std::to_string(calls[index].position));
                unclassified = true;
            }
        }
        if (unclassified) {
            return false;
        }

        bool mismatch = calls.size() != classifications.size();
        for (std::size_t index = 0; index < calls.size(); ++index) {
            if (matchedBy[index] != 1) {
                report("CodexPolicySemanticLoggerClassificationMismatch",
                       "accepted-entry multiplicity for " + calls[index].path + '|' + calls[index].helper + '@' +
                           std::to_string(calls[index].position) + " is " + std::to_string(matchedBy[index]) + ", expected 1");
                mismatch = true;
            }
        }
        if (calls.size() != classifications.size()) {
            report("CodexPolicySemanticLoggerClassificationMismatch",
                   "discovered " + std::to_string(calls.size()) + " parameterless semantic logger calls for " +
                       std::to_string(classifications.size()) + " accepted entries");
        }
        return !mismatch;
    }

} // namespace

int main(int argc, char* argv[]) {
    if (!aisuite::source_policy::cxx::scannerSelfTest() || !semanticScannerSelfTest()) {
        report("CodexPolicySemanticLoggerAuthorityMismatch", "token-aware scanner self-test failed");
        return 1;
    }

    const std::filesystem::path root = aisuite::source_policy::projectRoot(argc, argv);
    if (root.empty()) {
        report("CodexPolicySemanticLoggerAuthorityMismatch", "unable to locate the AISuite source root");
        return 1;
    }

    std::string error;
    const std::optional<std::filesystem::path> authorityPath =
        configuredDataPath(root, kAuthorityEnvironment, "CodexSemanticLoggerAuthority.tsv", error);
    if (!authorityPath) {
        report("CodexPolicySemanticLoggerAuthorityMismatch", error);
        return 1;
    }
    const std::optional<std::vector<Entry>> authority = readEntries(*authorityPath, error);
    if (!authority) {
        report("CodexPolicySemanticLoggerAuthorityMismatch", error);
        return 1;
    }
    if (!validatePinnedAuthority(*authority)) {
        return 1;
    }

    const std::optional<std::filesystem::path> classificationsPath =
        configuredDataPath(root, kClassificationsEnvironment, "CodexSemanticLoggerClassifications.tsv", error);
    if (!classificationsPath) {
        report("CodexPolicySemanticLoggerClassificationMismatch", error);
        return 1;
    }
    const std::optional<std::vector<Entry>> classifications = readEntries(*classificationsPath, error);
    if (!classifications) {
        report("CodexPolicySemanticLoggerClassificationMismatch", error);
        return 1;
    }
    if (!validateClassificationMetadata(*classifications, *authority)) {
        return 1;
    }

    SourceMap sources;
    if (!readCodexSources(root, sources, error)) {
        report("CodexPolicySemanticLoggerClassificationMismatch", error);
        return 1;
    }
    return validateDiscoveredCalls(sources, discover(sources), *classifications) ? 0 : 1;
}
