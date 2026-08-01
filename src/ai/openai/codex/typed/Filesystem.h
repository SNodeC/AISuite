/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_FILESYSTEM_H
#define AI_OPENAI_CODEX_TYPED_FILESYSTEM_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/Results.h"
#include "ai/openai/codex/typed/Types.h"

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ai::openai::codex::typed {

    struct FsWatchId {
        std::string value;

        auto operator<=>(const FsWatchId&) const = default;
    };

    struct FuzzyFileSearchMatchType {
        std::string value;

        static FuzzyFileSearchMatchType file() {
            return {"file"};
        }

        static FuzzyFileSearchMatchType directory() {
            return {"directory"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "file" || value == "directory";
        }

        auto operator<=>(const FuzzyFileSearchMatchType&) const = default;
    };

    struct FsCopyParams {
        AbsolutePath destinationPath;
        std::optional<bool> recursive;
        AbsolutePath sourcePath;

        bool operator==(const FsCopyParams&) const = default;
    };

    struct FsCreateDirectoryParams {
        AbsolutePath path;
        OptionalNullable<bool> recursive;

        bool operator==(const FsCreateDirectoryParams&) const = default;
    };

    struct FsGetMetadataParams {
        AbsolutePath path;

        auto operator<=>(const FsGetMetadataParams&) const = default;
    };

    struct FsGetMetadataResponse {
        std::int64_t createdAtMs = 0;
        bool isDirectory = false;
        bool isFile = false;
        bool isSymlink = false;
        std::int64_t modifiedAtMs = 0;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const FsGetMetadataResponse&) const = default;
    };

    struct FsReadDirectoryParams {
        AbsolutePath path;

        auto operator<=>(const FsReadDirectoryParams&) const = default;
    };

    struct FsReadDirectoryEntry {
        std::string fileName;
        bool isDirectory = false;
        bool isFile = false;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const FsReadDirectoryEntry&) const = default;
    };

    struct FsReadDirectoryResponse {
        std::vector<FsReadDirectoryEntry> entries;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const FsReadDirectoryResponse&) const = default;
    };

    struct FsReadFileParams {
        AbsolutePath path;

        auto operator<=>(const FsReadFileParams&) const = default;
    };

    struct FsReadFileResponse {
        std::string dataBase64;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const FsReadFileResponse&) const = default;
    };

    struct FsRemoveParams {
        OptionalNullable<bool> force;
        AbsolutePath path;
        OptionalNullable<bool> recursive;

        bool operator==(const FsRemoveParams&) const = default;
    };

    struct FsUnwatchParams {
        FsWatchId watchId;

        auto operator<=>(const FsUnwatchParams&) const = default;
    };

    struct FsWatchParams {
        AbsolutePath path;
        FsWatchId watchId;

        auto operator<=>(const FsWatchParams&) const = default;
    };

    struct FsWatchResponse {
        AbsolutePath path;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const FsWatchResponse&) const = default;
    };

    struct FsWriteFileParams {
        std::string dataBase64;
        AbsolutePath path;

        auto operator<=>(const FsWriteFileParams&) const = default;
    };

    struct FuzzyFileSearchParams {
        OptionalNullable<std::string> cancellationToken;
        std::string query;
        std::vector<std::string> roots;

        bool operator==(const FuzzyFileSearchParams&) const = default;
    };

    struct FuzzyFileSearchResult {
        std::string fileName;
        OptionalNullable<std::vector<std::uint32_t>> indices;
        FuzzyFileSearchMatchType matchType;
        std::string path;
        std::string root;
        std::uint32_t score = 0;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const FuzzyFileSearchResult&) const = default;
    };

    struct FuzzyFileSearchResponse {
        std::vector<FuzzyFileSearchResult> files;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const FuzzyFileSearchResponse&) const = default;
    };

    struct FsChangedNotification {
        std::vector<AbsolutePath> changedPaths;
        FsWatchId watchId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const FsChangedNotification&) const = default;
    };

    struct FuzzyFileSearchSessionCompletedNotification {
        std::string sessionId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const FuzzyFileSearchSessionCompletedNotification&) const = default;
    };

    struct FuzzyFileSearchSessionUpdatedNotification {
        std::vector<FuzzyFileSearchResult> files;
        std::string query;
        std::string sessionId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const FuzzyFileSearchSessionUpdatedNotification&) const = default;
    };

    class Filesystem {
    public:
        Filesystem(const Filesystem&) = delete;
        Filesystem(Filesystem&&) = delete;
        Filesystem& operator=(const Filesystem&) = delete;
        Filesystem& operator=(Filesystem&&) = delete;

        Submission copy(FsCopyParams params, DoneHandler handler);
        Submission createDirectory(FsCreateDirectoryParams params, DoneHandler handler);
        Submission getMetadata(FsGetMetadataParams params, CompletionHandler<FsGetMetadataResponse> handler);
        Submission readDirectory(FsReadDirectoryParams params, CompletionHandler<FsReadDirectoryResponse> handler);
        Submission readFile(FsReadFileParams params, CompletionHandler<FsReadFileResponse> handler);
        Submission remove(FsRemoveParams params, DoneHandler handler);
        Submission watch(FsWatchParams params, CompletionHandler<FsWatchResponse> handler);
        Submission unwatch(FsUnwatchParams params, DoneHandler handler);
        Submission writeFile(FsWriteFileParams params, DoneHandler handler);
        Submission fuzzyFileSearch(FuzzyFileSearchParams params, CompletionHandler<FuzzyFileSearchResponse> handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Filesystem(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_FILESYSTEM_H
