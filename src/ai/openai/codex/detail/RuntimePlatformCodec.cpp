/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/RuntimePlatformCodec.h"

#include "ai/openai/codex/detail/DecodeDiagnostic.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::openai::codex::detail {

    namespace {

        const Json* member(const Json& object, std::string_view name) noexcept {
            if (!object.is_object()) {
                return nullptr;
            }
            const auto iterator = object.find(name);
            return iterator == object.end() ? nullptr : &*iterator;
        }

        std::string childPath(std::string_view path, std::string_view field) {
            return std::string(path) + "." + std::string(field);
        }

        void expected(std::string& error, std::string_view surface, std::string_view path, std::string_view type) {
            error = std::string(surface) + " field '" + std::string(path) + "' must be " + std::string(type);
        }

        void missing(std::string& error, std::string_view surface, std::string_view path) {
            error = std::string(surface) + " is missing required field '" + std::string(path) + "'";
        }

        bool paramsObject(const Notification& notification, std::string& error, std::string_view surface) {
            if (!notification.params.is_object()) {
                expected(error, surface, "$.params", "an object");
                return false;
            }
            return true;
        }

        bool stringValue(const Json& value, std::string& output, std::string& error, std::string_view surface, std::string_view path) {
            if (!value.is_string()) {
                expected(error, surface, path, "a string");
                return false;
            }
            output = value.get_ref<const std::string&>();
            return true;
        }

        bool boolValue(const Json& value, bool& output, std::string& error, std::string_view surface, std::string_view path) {
            if (!value.is_boolean()) {
                expected(error, surface, path, "a boolean");
                return false;
            }
            output = value.get<bool>();
            return true;
        }

        bool int32Value(const Json& value, std::int32_t& output, std::string& error, std::string_view surface, std::string_view path) {
            if (value.is_number_unsigned()) {
                const auto decoded = value.get<std::uint64_t>();
                if (decoded <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
                    output = static_cast<std::int32_t>(decoded);
                    return true;
                }
            } else if (value.is_number_integer()) {
                const auto decoded = value.get<std::int64_t>();
                if (decoded >= std::numeric_limits<std::int32_t>::min() && decoded <= std::numeric_limits<std::int32_t>::max()) {
                    output = static_cast<std::int32_t>(decoded);
                    return true;
                }
            }
            expected(error, surface, path, "an int32 integer");
            return false;
        }

        bool uint64Value(const Json& value, std::uint64_t& output, std::string& error, std::string_view surface, std::string_view path) {
            if (value.is_number_unsigned()) {
                output = value.get<std::uint64_t>();
                return true;
            }
            if (value.is_number_integer()) {
                const auto decoded = value.get<std::int64_t>();
                if (decoded >= 0) {
                    output = static_cast<std::uint64_t>(decoded);
                    return true;
                }
            }
            expected(error, surface, path, "a non-negative integer");
            return false;
        }

        template <typename T, typename Decoder>
        bool
        required(const Json& object, std::string_view field, T& output, std::string& error, std::string_view surface, Decoder&& decoder) {
            const std::string path = childPath("$.params", field);
            const Json* value = member(object, field);
            if (value == nullptr) {
                missing(error, surface, path);
                return false;
            }
            return decoder(*value, output, error, surface, path);
        }

        template <typename T, typename Decoder>
        bool optionalNullable(const Json& object,
                              std::string_view field,
                              typed::OptionalNullable<T>& output,
                              std::string& error,
                              std::string_view surface,
                              Decoder&& decoder) {
            const Json* value = member(object, field);
            if (value == nullptr) {
                output = typed::OptionalNullable<T>::omitted();
                return true;
            }
            if (value->is_null()) {
                output = typed::OptionalNullable<T>::explicitNull();
                return true;
            }
            T decoded{};
            if (!decoder(*value, decoded, error, surface, childPath("$.params", field))) {
                return false;
            }
            output = typed::OptionalNullable<T>::withValue(std::move(decoded));
            return true;
        }

        bool
        threadIdValue(const Json& value, typed::ThreadId& output, std::string& error, std::string_view surface, std::string_view path) {
            return stringValue(value, output.value, error, surface, path);
        }

        bool
        requestIdValue(const Json& value, ServerRequestId& output, std::string& error, std::string_view surface, std::string_view path) {
            if (value.is_string()) {
                output = ServerRequestId(value.get_ref<const std::string&>());
                return true;
            }
            if (value.is_number_unsigned()) {
                const auto decoded = value.get<std::uint64_t>();
                if (decoded <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    output = ServerRequestId(static_cast<std::int64_t>(decoded));
                    return true;
                }
            } else if (value.is_number_integer()) {
                output = ServerRequestId(value.get<std::int64_t>());
                return true;
            }
            expected(error, surface, path, "a string or int64 integer");
            return false;
        }

        template <typename Enum>
        bool openEnumValue(const Json& value,
                           Enum& output,
                           std::vector<typed::DecodeDiagnostic>& diagnostics,
                           std::string_view enumName,
                           std::string& error,
                           std::string_view surface,
                           std::string_view path) {
            if (!stringValue(value, output.value, error, surface, path)) {
                return false;
            }
            if (!output.isKnown()) {
                diagnostics.emplace_back(unknownEnumDiagnostic(std::string(enumName), std::string(path)));
            }
            return true;
        }

        template <typename NotificationType, typename Decoder>
        std::optional<NotificationType>
        decodeNotification(const Notification& notification, std::string& error, std::string_view surface, Decoder&& decoder) noexcept {
            try {
                error.clear();
                if (!paramsObject(notification, error, surface)) {
                    return std::nullopt;
                }
                NotificationType output;
                output.raw = notification.raw;
                if (!decoder(notification.params, output)) {
                    return std::nullopt;
                }
                return output;
            } catch (...) {
                error = std::string(surface) + " could not be decoded";
                return std::nullopt;
            }
        }

    } // namespace

    std::optional<typed::DeprecationNoticeNotification> decodeDeprecationNoticeNotification(const Notification& notification,
                                                                                            std::string& error) noexcept {
        constexpr std::string_view Surface = "DeprecationNoticeNotification";
        return decodeNotification<typed::DeprecationNoticeNotification>(
            notification, error, Surface, [&](const Json& params, typed::DeprecationNoticeNotification& output) {
                return optionalNullable(params, "details", output.details, error, Surface, stringValue) &&
                       required(params, "summary", output.summary, error, Surface, stringValue);
            });
    }

    std::optional<typed::ProcessExitedNotification> decodeProcessExitedNotification(const Notification& notification,
                                                                                    std::string& error) noexcept {
        constexpr std::string_view Surface = "ProcessExitedNotification";
        return decodeNotification<typed::ProcessExitedNotification>(
            notification, error, Surface, [&](const Json& params, typed::ProcessExitedNotification& output) {
                return required(params, "exitCode", output.exitCode, error, Surface, int32Value) &&
                       required(params, "processHandle", output.processHandle, error, Surface, stringValue) &&
                       required(params, "stderr", output.stderr, error, Surface, stringValue) &&
                       required(params, "stderrCapReached", output.stderrCapReached, error, Surface, boolValue) &&
                       required(params, "stdout", output.stdout, error, Surface, stringValue) &&
                       required(params, "stdoutCapReached", output.stdoutCapReached, error, Surface, boolValue);
            });
    }

    std::optional<typed::ProcessOutputDeltaNotification> decodeProcessOutputDeltaNotification(const Notification& notification,
                                                                                              std::string& error) noexcept {
        constexpr std::string_view Surface = "ProcessOutputDeltaNotification";
        return decodeNotification<typed::ProcessOutputDeltaNotification>(
            notification, error, Surface, [&](const Json& params, typed::ProcessOutputDeltaNotification& output) {
                return required(params, "capReached", output.capReached, error, Surface, boolValue) &&
                       required(params, "deltaBase64", output.deltaBase64, error, Surface, stringValue) &&
                       required(params, "processHandle", output.processHandle, error, Surface, stringValue) &&
                       required(params,
                                "stream",
                                output.stream,
                                error,
                                Surface,
                                [&](const Json& value,
                                    typed::ProcessOutputStream& decoded,
                                    std::string& nestedError,
                                    std::string_view nestedSurface,
                                    std::string_view path) {
                                    return openEnumValue(
                                        value, decoded, output.diagnostics, "ProcessOutputStream", nestedError, nestedSurface, path);
                                });
            });
    }

    std::optional<typed::RemoteControlStatusChangedNotification>
    decodeRemoteControlStatusChangedNotification(const Notification& notification, std::string& error) noexcept {
        constexpr std::string_view Surface = "RemoteControlStatusChangedNotification";
        return decodeNotification<typed::RemoteControlStatusChangedNotification>(
            notification, error, Surface, [&](const Json& params, typed::RemoteControlStatusChangedNotification& output) {
                return optionalNullable(params, "environmentId", output.environmentId, error, Surface, stringValue) &&
                       required(params, "installationId", output.installationId, error, Surface, stringValue) &&
                       required(params, "serverName", output.serverName, error, Surface, stringValue) &&
                       required(
                           params,
                           "status",
                           output.status,
                           error,
                           Surface,
                           [&](const Json& value,
                               typed::RemoteControlConnectionStatus& decoded,
                               std::string& nestedError,
                               std::string_view nestedSurface,
                               std::string_view path) {
                               return openEnumValue(
                                   value, decoded, output.diagnostics, "RemoteControlConnectionStatus", nestedError, nestedSurface, path);
                           });
            });
    }

    std::optional<typed::ServerRequestResolvedNotification> decodeServerRequestResolvedNotification(const Notification& notification,
                                                                                                    std::string& error) noexcept {
        constexpr std::string_view Surface = "ServerRequestResolvedNotification";
        return decodeNotification<typed::ServerRequestResolvedNotification>(
            notification, error, Surface, [&](const Json& params, typed::ServerRequestResolvedNotification& output) {
                return required(params, "requestId", output.requestId, error, Surface, requestIdValue) &&
                       required(params, "threadId", output.threadId, error, Surface, threadIdValue);
            });
    }

    std::optional<typed::WarningNotification> decodeWarningNotification(const Notification& notification, std::string& error) noexcept {
        constexpr std::string_view Surface = "WarningNotification";
        return decodeNotification<typed::WarningNotification>(
            notification, error, Surface, [&](const Json& params, typed::WarningNotification& output) {
                return required(params, "message", output.message, error, Surface, stringValue) &&
                       optionalNullable(params, "threadId", output.threadId, error, Surface, threadIdValue);
            });
    }

    std::optional<typed::WindowsWorldWritableWarningNotification>
    decodeWindowsWorldWritableWarningNotification(const Notification& notification, std::string& error) noexcept {
        constexpr std::string_view Surface = "WindowsWorldWritableWarningNotification";
        return decodeNotification<typed::WindowsWorldWritableWarningNotification>(
            notification, error, Surface, [&](const Json& params, typed::WindowsWorldWritableWarningNotification& output) {
                const Json* paths = member(params, "samplePaths");
                if (!required(params, "extraCount", output.extraCount, error, Surface, uint64Value) ||
                    !required(params, "failedScan", output.failedScan, error, Surface, boolValue)) {
                    return false;
                }
                if (paths == nullptr) {
                    missing(error, Surface, "$.params.samplePaths");
                    return false;
                }
                if (!paths->is_array()) {
                    expected(error, Surface, "$.params.samplePaths", "an array");
                    return false;
                }
                output.samplePaths.clear();
                output.samplePaths.reserve(paths->size());
                for (std::size_t index = 0; index < paths->size(); ++index) {
                    std::string decoded;
                    if (!stringValue((*paths)[index], decoded, error, Surface, "$.params.samplePaths[*]")) {
                        return false;
                    }
                    output.samplePaths.emplace_back(std::move(decoded));
                }
                return true;
            });
    }

    std::optional<typed::WindowsSandboxSetupCompletedNotification>
    decodeWindowsSandboxSetupCompletedNotification(const Notification& notification, std::string& error) noexcept {
        constexpr std::string_view Surface = "WindowsSandboxSetupCompletedNotification";
        return decodeNotification<typed::WindowsSandboxSetupCompletedNotification>(
            notification, error, Surface, [&](const Json& params, typed::WindowsSandboxSetupCompletedNotification& output) {
                return optionalNullable(params, "error", output.error, error, Surface, stringValue) &&
                       required(params,
                                "mode",
                                output.mode,
                                error,
                                Surface,
                                [&](const Json& value,
                                    typed::WindowsSandboxSetupMode& decoded,
                                    std::string& nestedError,
                                    std::string_view nestedSurface,
                                    std::string_view path) {
                                    return openEnumValue(
                                        value, decoded, output.diagnostics, "WindowsSandboxSetupMode", nestedError, nestedSurface, path);
                                }) &&
                       required(params, "success", output.success, error, Surface, boolValue);
            });
    }

} // namespace ai::openai::codex::detail
