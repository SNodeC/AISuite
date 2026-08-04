/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend/ReferenceInvocationPolicy.h"
#include "support/TestResult.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

    namespace app = apps::codex_backend;
    namespace frontend = ai::openai::codex::frontend;

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            std::string value = (std::filesystem::temp_directory_path() / "aisuite-invocation-policy-XXXXXX").string();
            if (char* created = ::mkdtemp(value.data()); created != nullptr) {
                path = created;
            }
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        [[nodiscard]] const std::filesystem::path& get() const noexcept {
            return path;
        }

    private:
        std::filesystem::path path;
    };

    bool constructorRejects(const app::ReferenceInvocationPolicyOptions& options) {
        try {
            const app::ReferenceInvocationPolicy policy(options);
            (void) policy;
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    }

} // namespace

int main() {
    tests::support::TestResult result;
    TemporaryDirectory temporary;
    result.expectTrue(!temporary.get().empty(), "an isolated invocation-policy directory is created");

    const std::filesystem::path allowed = temporary.get() / "allowed";
    const std::filesystem::path outside = temporary.get() / "outside";
    const std::filesystem::path prefixSibling = temporary.get() / "allowed-sibling";
    std::error_code filesystemError;
    std::filesystem::create_directories(allowed / "nested", filesystemError);
    std::filesystem::create_directories(outside, filesystemError);
    std::filesystem::create_directories(prefixSibling, filesystemError);
    filesystemError.clear();
    std::filesystem::create_directory_symlink(outside, allowed / "escape", filesystemError);
    result.expectTrue(!filesystemError && std::filesystem::is_symlink(allowed / "escape"),
                      "the symlink-escape fixture is created");

    const app::ReferenceInvocationPolicy policy({
        .filesystemRoots = {allowed},
        .commands = {.executables = {"/usr/bin/printf"}, .shellCommands = {"printf safe"}},
    });
    result.expectTrue(policy.filesystemRoots().size() == 1 && policy.commandAllowlist().executables.size() == 1 &&
                          policy.commandAllowlist().shellCommands.size() == 1,
                      "the policy retains only explicit canonical root and command allowlists");

    result.expectTrue(policy.allowsFilesystemRead("fs.readFile", {{"path", allowed.string()}}) &&
                          policy.allowsFilesystemRead("fs.getMetadata", {{"path", (allowed / "nested" / "future").string()}}),
                      "the configured root itself and normalized descendants are admitted");
    result.expectTrue(!policy.allowsFilesystemRead("fs.readFile", {{"path", (allowed / ".." / "outside").string()}}) &&
                          !policy.allowsFilesystemRead("fs.readFile", {{"path", prefixSibling.string()}}) &&
                          !policy.allowsFilesystemRead("fs.readFile", {{"path", "relative/path"}}),
                      "lexical traversal, prefix siblings, and relative paths are rejected");
    result.expectTrue(!policy.allowsFilesystemRead("fs.readFile", {{"path", (allowed / "escape" / "secret").string()}}),
                      "canonical resolution rejects a symlink escape from an allowed root");

    result.expectTrue(policy.allowsFilesystemWrite(
                          "fs.copy", {{"sourcePath", (allowed / "nested").string()}, {"destinationPath", (allowed / "copy").string()}}) &&
                          !policy.allowsFilesystemWrite(
                              "fs.copy", {{"sourcePath", (allowed / "nested").string()}, {"destinationPath", outside.string()}}),
                      "copy policy checks both sourcePath and destinationPath");
    result.expectTrue(policy.allowsFilesystemRead(
                          "fuzzyFileSearch", {{"query", "needle"}, {"roots", frontend::Json::array({allowed.string()})}}) &&
                          !policy.allowsFilesystemRead(
                              "fuzzyFileSearch", {{"query", "needle"}, {"roots", frontend::Json::array({allowed.string(), outside.string()})}}) &&
                          !policy.allowsFilesystemRead("fuzzyFileSearch", {{"query", "needle"}, {"roots", frontend::Json::array()}}),
                      "fuzzy search requires a nonempty set of roots wholly inside the allowlist");
    result.expectTrue(policy.allowsFilesystemRead("fs.unwatch", {{"watchId", "watch-1"}}) &&
                          !policy.allowsFilesystemRead("fs.unwatch", frontend::Json::object()) &&
                          !policy.allowsFilesystemRead("fs.unknown", {{"path", allowed.string()}}),
                      "a schema-valid watch identifier passes policy while BackendCore retains authoritative watch correlation");

    const frontend::Json permittedExec{{"command", frontend::Json::array({"/usr/bin/printf", "%s", "safe"})}};
    result.expectTrue(policy.allowsCommandExecution("command.exec", permittedExec) &&
                          !policy.allowsCommandExecution("command.exec", {{"command", frontend::Json::array()}}) &&
                          !policy.allowsCommandExecution("command.exec", {{"command", frontend::Json::array({"/usr/bin/printf-extra"})}}),
                      "command.exec requires nonempty argv and an exact argv[0] allowlist match");
    result.expectTrue(policy.allowsCommandExecution(
                          "command.exec", {{"command", frontend::Json::array({"/usr/bin/printf"})}, {"cwd", allowed.string()}}) &&
                          !policy.allowsCommandExecution(
                              "command.exec", {{"command", frontend::Json::array({"/usr/bin/printf"})}, {"cwd", outside.string()}}),
                      "command working directories remain inside configured filesystem roots");
    result.expectTrue(
        policy.allowsCommandExecution("command.exec",
                                      {{"command", frontend::Json::array({"/usr/bin/printf"})},
                                       {"sandboxPolicy", {{"type", "workspaceWrite"},
                                                          {"writableRoots", frontend::Json::array({(allowed / "nested").string()})}}}}) &&
            !policy.allowsCommandExecution("command.exec",
                                           {{"command", frontend::Json::array({"/usr/bin/printf"})},
                                            {"sandboxPolicy", {{"type", "workspaceWrite"},
                                                               {"writableRoots", frontend::Json::array({outside.string()})}}}}),
        "workspace sandbox writableRoots are projected through the same canonical root policy");

    result.expectTrue(policy.allowsCommandExecution("command.exec.resize",
                                                    {{"processId", "process-1"}, {"size", {{"cols", 80}, {"rows", 24}}}}) &&
                          policy.allowsCommandExecution("command.exec.write", {{"processId", "process-1"}, {"deltaBase64", "YQ=="}}) &&
                          policy.allowsCommandExecution("command.exec.terminate", {{"processId", "process-1"}}) &&
                          !policy.allowsCommandExecution("command.exec.resize", {{"processId", "process-1"}}),
                      "schema-valid process follow-ups pass while BackendCore retains authoritative process correlation");
    result.expectTrue(policy.allowsCommandExecution("thread.shellCommand", {{"threadId", "thread-1"}, {"command", "printf safe"}}) &&
                          !policy.allowsCommandExecution("thread.shellCommand",
                                                        {{"threadId", "thread-1"}, {"command", "printf safe "}}) &&
                          !policy.allowsCommandExecution("thread.shellCommand",
                                                        {{"threadId", "thread-1"}, {"command", "printf unsafe"}}),
                      "thread shell commands require an exact full-string allowlist match");

    result.expectTrue(constructorRejects({.filesystemRoots = {std::filesystem::path{"/"}}, .commands = {}}) &&
                          constructorRejects({.filesystemRoots = {std::filesystem::path{"relative"}}, .commands = {}}) &&
                          constructorRejects(
                              {.filesystemRoots = {}, .commands = {.executables = {"*"}, .shellCommands = {}}}) &&
                          constructorRejects(
                              {.filesystemRoots = {}, .commands = {.executables = {}, .shellCommands = {""}}}),
                      "filesystem-wide, relative, wildcard, and empty allowlist entries cannot create an allow-all policy");

    const app::ReferenceInvocationPolicy denyByDefault({});
    result.expectTrue(!denyByDefault.allowsFilesystemRead("fs.readFile", {{"path", allowed.string()}}) &&
                          !denyByDefault.allowsCommandExecution("command.exec", permittedExec) &&
                          !denyByDefault.allowsCommandExecution(
                              "thread.shellCommand", {{"threadId", "thread-1"}, {"command", "printf safe"}}),
                      "empty explicit allowlists deny new filesystem and command invocations");

    return result.processResult();
}
