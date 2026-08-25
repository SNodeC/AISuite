/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/protocol/RuntimePaths.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace ai::openai::codex::protocol {
    namespace {
        bool privateOwnedDirectory(const std::string& path) {
            struct stat status = {};
            return ::lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
                (status.st_mode & 0077) == 0;
        }
    } // namespace

    std::string runtimeDirectory() {
        if (const char* configured = std::getenv("XDG_RUNTIME_DIR"); configured != nullptr && configured[0] == '/' &&
            privateOwnedDirectory(configured)) {
            return configured;
        }
        return "/tmp/codex-bridge-" + std::to_string(::geteuid());
    }

    bool ensurePrivateRuntimeDirectory(std::string* error) {
        const std::string path = runtimeDirectory();
        if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
            if (error != nullptr) {
                *error = "unable to create runtime directory " + path + ": " + std::strerror(errno);
            }
            return false;
        }
        if (!privateOwnedDirectory(path)) {
            if (error != nullptr) {
                *error = "runtime directory is not private and owned by the current user: " + path;
            }
            return false;
        }
        return true;
    }

    std::string defaultFrontendSocketPath() {
        return runtimeDirectory() + "/codex-bridge.sock";
    }

    std::string defaultAppServerSocketPath() {
        return runtimeDirectory() + "/codex-app-server.sock";
    }

} // namespace ai::openai::codex::protocol
