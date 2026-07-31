/*
 * AISuite - Reusable AI integrations based on SNode.C
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * Adapted from SNode.C tests/policy/SourcePolicyTestRoot.h.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef TESTS_POLICY_SUPPORT_AISUITESOURCEPOLICYTESTROOT_H
#define TESTS_POLICY_SUPPORT_AISUITESOURCEPOLICYTESTROOT_H

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace aisuite::source_policy {
    inline constexpr std::string_view kProjectRootMarker = "src/ai/openai/codex/CMakeLists.txt";

    inline bool isProjectRoot(const std::filesystem::path& root) {
        return !root.empty() && std::filesystem::is_regular_file(root / kProjectRootMarker);
    }

    inline std::filesystem::path projectRoot(const int argc, char* argv[]) {
        for (int index = 1; index < argc; ++index) {
            if (std::string_view(argv[index]) != "--repo-root") {
                continue;
            }
            if (index + 1 >= argc) {
                std::cerr << "--repo-root requires a path\n";
                return {};
            }
            const std::filesystem::path root = argv[index + 1];
            if (isProjectRoot(root)) {
                return std::filesystem::path(root);
            }
            std::cerr << "--repo-root does not look like the AISuite project root: " << root << '\n';
            return {};
        }

        if (const char* sourceDir = std::getenv("AISUITE_SOURCE_DIR")) {
            const std::filesystem::path root = sourceDir;
            if (isProjectRoot(root)) {
                return std::filesystem::path(root);
            }
            std::cerr << "AISUITE_SOURCE_DIR does not look like the AISuite project root: " << root << '\n';
            return {};
        }

        std::filesystem::path current = std::filesystem::current_path();
        while (!current.empty()) {
            if (isProjectRoot(current)) {
                return std::filesystem::path(current);
            }
            const std::filesystem::path parent = current.parent_path();
            if (parent == current) {
                break;
            }
            current = parent;
        }

        std::cerr << "Unable to locate the AISuite project root from " << std::filesystem::current_path() << '\n';
        return {};
    }

    inline std::string readFile(const std::filesystem::path& path) {
        std::ifstream input(path);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    inline bool contains(const std::string_view haystack, const std::string_view needle) {
        return haystack.find(needle) != std::string_view::npos;
    }

    inline std::size_t occurrenceCount(const std::string_view source, const std::string_view needle) {
        if (needle.empty()) {
            return 0;
        }
        std::size_t count = 0;
        for (std::size_t position = source.find(needle); position != std::string_view::npos;
             position = source.find(needle, position + needle.size())) {
            ++count;
        }
        return count;
    }

    inline bool diagnostic(const std::string_view code, const std::string_view detail) {
        std::cerr << code << ": " << detail << '\n';
        return false;
    }
} // namespace aisuite::source_policy

#endif // TESTS_POLICY_SUPPORT_AISUITESOURCEPOLICYTESTROOT_H
