/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/FrontendService.h"

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/backend/BackendCommand.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/backend/BackendEvent.h"
#include "ai/openai/codex/backend/BackendState.h"
#include "ai/openai/codex/backend/FrontendSession.h"
#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/detail/BackendCommandMapper.h"
#include "ai/openai/codex/frontend/detail/BackendProjectionBuilder.h"
#include "ai/openai/codex/frontend/detail/FrontendCapabilities.h"
#include "ai/openai/codex/frontend/detail/FrontendProjection.h"
#include "ai/openai/codex/frontend/detail/ProviderResultProjection.h"
#include "ai/openai/codex/typed/ServerRequests.h"
#include "ai/openai/codex/typed/Threads.h"
#include "ai/openai/codex/typed/Turns.h"
#include "ai/openai/codex/typed/Types.h"
#include "core/EventReceiver.h"
#include "core/timer/Timer.h"
#include "utils/Timeval.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace ai::openai::codex::frontend {

    namespace {

        template <typename... Visitors>
        struct Overloaded : Visitors... {
            using Visitors::operator()...;
        };

        template <typename... Visitors>
        Overloaded(Visitors...) -> Overloaded<Visitors...>;

        template <typename T, typename Variant>
        struct VariantContains;

        template <typename T, typename... Alternatives>
        struct VariantContains<T, std::variant<Alternatives...>> : std::bool_constant<(std::is_same_v<T, Alternatives> || ...)> {};

        template <typename T>
        concept ProviderOperationResult = VariantContains<std::remove_cvref_t<T>, backend::ProviderOperationValue>::value;

        bool hasScope(const FrontendPrincipal& principal, FrontendScope scope) noexcept {
            return std::find(principal.scopes.begin(), principal.scopes.end(), scope) != principal.scopes.end();
        }

        bool hasRequiredScopes(const FrontendPrincipal& principal, std::span<const FrontendScope> required) noexcept {
            return std::all_of(required.begin(), required.end(), [&](FrontendScope scope) {
                return hasScope(principal, scope);
            });
        }

        bool validPrincipal(const FrontendPrincipal& principal) noexcept {
            if (principal.id.empty() || principal.profile.empty() || principal.scopes.size() > LocalTrustedScopes.size()) {
                return false;
            }
            std::array<bool, LocalTrustedScopes.size()> seen{};
            for (FrontendScope scope : principal.scopes) {
                const std::size_t index = static_cast<std::size_t>(scope);
                if (index >= seen.size() || seen[index]) {
                    return false;
                }
                seen[index] = true;
            }
            return true;
        }

        ErrorCode frontendErrorCode(AuthenticationFailureCode code) noexcept {
            switch (code) {
                case AuthenticationFailureCode::AuthenticationRequired:
                    return ErrorCode::AuthenticationRequired;
                case AuthenticationFailureCode::AuthenticationFailed:
                    return ErrorCode::AuthenticationFailed;
                case AuthenticationFailureCode::OriginRejected:
                    return ErrorCode::OriginRejected;
                case AuthenticationFailureCode::TransportSecurityRequired:
                    return ErrorCode::TransportSecurityRequired;
                case AuthenticationFailureCode::RateLimited:
                    return ErrorCode::RateLimited;
            }
            return ErrorCode::AuthenticationFailed;
        }

        std::string authenticationErrorMessage(AuthenticationFailureCode code) {
            switch (code) {
                case AuthenticationFailureCode::AuthenticationRequired:
                    return "frontend authentication is required";
                case AuthenticationFailureCode::AuthenticationFailed:
                    return "frontend authentication failed";
                case AuthenticationFailureCode::OriginRejected:
                    return "frontend origin is not permitted";
                case AuthenticationFailureCode::TransportSecurityRequired:
                    return "frontend transport security is required";
                case AuthenticationFailureCode::RateLimited:
                    return "frontend authentication rate limit exceeded";
            }
            return "frontend authentication failed";
        }

        std::string_view addressWithoutEphemeralPort(std::string_view address) noexcept {
            if (address.starts_with('[')) {
                const std::size_t closingBracket = address.find(']');
                if (closingBracket == address.size() - 1) {
                    return address.substr(1, closingBracket - 1);
                }
                if (closingBracket != std::string_view::npos && closingBracket + 1 < address.size() && address[closingBracket + 1] == ':') {
                    const std::string_view port = address.substr(closingBracket + 2);
                    if (!port.empty() && std::all_of(port.begin(), port.end(), [](char character) {
                            return character >= '0' && character <= '9';
                        })) {
                        return address.substr(1, closingBracket - 1);
                    }
                }
                return address;
            }

            const std::size_t separator = address.rfind(':');
            if (separator != std::string_view::npos && address.find(':') == separator) {
                const std::string_view port = address.substr(separator + 1);
                if (!port.empty() && std::all_of(port.begin(), port.end(), [](char character) {
                        return character >= '0' && character <= '9';
                    })) {
                    return address.substr(0, separator);
                }
            }
            return address;
        }

        bool isHexadecimalDigit(char character) noexcept {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
                   (character >= 'A' && character <= 'F');
        }

        std::string_view rfcommAddressWithoutChannel(std::string_view address) noexcept {
            constexpr std::size_t BluetoothAddressSize = 17;
            if (address.size() < BluetoothAddressSize) {
                return address;
            }
            for (std::size_t index = 0; index < BluetoothAddressSize; ++index) {
                const bool separator = index % 3 == 2;
                if ((separator && address[index] != ':') || (!separator && !isHexadecimalDigit(address[index]))) {
                    return address;
                }
            }
            if (address.size() == BluetoothAddressSize) {
                return address;
            }
            if (address[BluetoothAddressSize] != ':') {
                return address;
            }
            const std::string_view channel = address.substr(BluetoothAddressSize + 1);
            if (channel.empty() || !std::all_of(channel.begin(), channel.end(), [](char character) {
                    return character >= '0' && character <= '9';
                })) {
                return address;
            }
            return address.substr(0, BluetoothAddressSize);
        }

        std::string peerAdmissionKey(const FrontendPeerContext& peer) {
            if (peer.remoteAddress.has_value()) {
                const bool rfcomm = peer.transport == FrontendTransportKind::Rfcomm || peer.transport == FrontendTransportKind::RfcommTls;
                const std::string_view address =
                    rfcomm ? rfcommAddressWithoutChannel(*peer.remoteAddress) : addressWithoutEphemeralPort(*peer.remoteAddress);
                return "address:" + std::string(address);
            }
            if (peer.unixUserId.has_value()) {
                return "unix:uid:" + std::to_string(*peer.unixUserId);
            }
            return std::string(toString(peer.transport)) + ":anonymous";
        }

        std::uint64_t saturatingMultiply(std::size_t left, std::uint64_t right) noexcept {
            if (left == 0 || right == 0) {
                return 0;
            }
            if (left > std::numeric_limits<std::uint64_t>::max() / right) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            return static_cast<std::uint64_t>(left) * right;
        }

        std::string backendLifecycleName(backend::ProviderLifecycle lifecycle) {
            switch (lifecycle) {
                case backend::ProviderLifecycle::Stopped:
                    return "stopped";
                case backend::ProviderLifecycle::Starting:
                    return "starting";
                case backend::ProviderLifecycle::Initializing:
                    return "initializing";
                case backend::ProviderLifecycle::Ready:
                    return "ready";
                case backend::ProviderLifecycle::Stopping:
                    return "stopping";
                case backend::ProviderLifecycle::Failed:
                    return "failed";
                case backend::ProviderLifecycle::Recovering:
                    return "starting";
            }
            return "failed";
        }

        std::string backendRoleName(backend::SessionRole role) {
            return role == backend::SessionRole::Controller ? "controller" : "observer";
        }

        bool capacityMutationRequiresSnapshot(const backend::CapacityChanged& event) noexcept {
            switch (event.metric) {
                case backend::CapacityMetric::EvictedThreads:
                case backend::CapacityMetric::EvictedTurns:
                case backend::CapacityMetric::EvictedItems:
                case backend::CapacityMetric::DroppedContentBytes:
                case backend::CapacityMetric::EvictedNotices:
                case backend::CapacityMetric::EvictedProcesses:
                case backend::CapacityMetric::DroppedProcessOutputBytes:
                case backend::CapacityMetric::EvictedFilesystemWatches:
                case backend::CapacityMetric::EvictedFuzzySearchSessions:
                case backend::CapacityMetric::EvictedActivityRecords:
                    return true;
                case backend::CapacityMetric::RejectedSessions:
                case backend::CapacityMetric::RejectedObservers:
                case backend::CapacityMetric::RejectedOperations:
                case backend::CapacityMetric::ProviderRequestOverflows:
                case backend::CapacityMetric::SnapshotOmissions:
                    return false;
            }
            return false;
        }

        Json errorSnapshotJson(const backend::ErrorSnapshot& error) {
            // Provider error text is not structured strongly enough to prove
            // that it cannot contain credential material. Keep the stable
            // category/code contract and use a deterministic safe summary.
            return Json{{"category", error.category}, {"code", error.code}, {"message", "Codex App Server reported an error"}};
        }

        bool isFrontendV1MetadataOnlyItem(std::string_view type) noexcept {
            return type == "collabAgentToolCall" || type == "contextCompaction" || type == "enteredReviewMode" ||
                   type == "exitedReviewMode" || type == "hookPrompt" || type == "imageGeneration" || type == "imageView" ||
                   type == "plan" || type == "sleep" || type == "subAgentActivity";
        }

        Json itemSnapshotJson(const backend::ItemSnapshot& item) {
            const Json frontendData = isFrontendV1MetadataOnlyItem(item.type) ? Json::object({{"codexType", item.type}}) : item.data;
            Json encoded{{"id", item.id},
                         {"type", item.type},
                         {"status", item.status},
                         {"agentText", item.agentText},
                         {"reasoningText", item.reasoningText},
                         {"reasoningSummary", item.reasoningSummary},
                         {"commandOutput", item.commandOutput},
                         {"droppedContentBytes", item.droppedContentBytes},
                         {"contentTruncated", item.contentTruncated},
                         {"data", frontendData},
                         {"extensions", item.extensions}};
            if (item.startedAtMs.has_value()) {
                encoded["startedAtMs"] = *item.startedAtMs;
            }
            if (item.completedAtMs.has_value()) {
                encoded["completedAtMs"] = *item.completedAtMs;
            }
            return encoded;
        }

        Json turnSnapshotJson(const backend::TurnSnapshot& turn) {
            Json encoded{{"id", turn.id},
                         {"threadId", turn.threadId},
                         {"status", turn.status},
                         {"active", turn.active},
                         {"terminal", turn.terminal},
                         {"items", Json::array()},
                         {"extensions", turn.extensions}};
            if (turn.failure.has_value()) {
                encoded["failure"] = *turn.failure;
            }
            if (turn.tokenUsage.has_value()) {
                encoded["tokenUsage"] = *turn.tokenUsage;
            }
            for (const backend::ItemSnapshot& item : turn.items) {
                encoded["items"].push_back(itemSnapshotJson(item));
            }
            return encoded;
        }

        Json threadSnapshotJson(const backend::ThreadSnapshot& thread) {
            Json encoded{
                {"id", thread.id}, {"fullyLoaded", thread.fullyLoaded}, {"turns", Json::array()}, {"extensions", thread.extensions}};
            if (thread.title.has_value()) {
                encoded["title"] = *thread.title;
            }
            if (thread.cwd.has_value()) {
                encoded["cwd"] = *thread.cwd;
            }
            if (thread.model.has_value()) {
                encoded["model"] = *thread.model;
            }
            if (thread.modelProvider.has_value()) {
                encoded["modelProvider"] = *thread.modelProvider;
            }
            if (thread.preview.has_value()) {
                encoded["preview"] = *thread.preview;
            }
            if (thread.status.has_value()) {
                encoded["status"] = *thread.status;
            }
            if (thread.createdAt.has_value()) {
                encoded["createdAt"] = *thread.createdAt;
            }
            if (thread.updatedAt.has_value()) {
                encoded["updatedAt"] = *thread.updatedAt;
            }
            for (const backend::TurnSnapshot& turn : thread.turns) {
                encoded["turns"].push_back(turnSnapshotJson(turn));
            }
            return encoded;
        }

        Json pendingRequestSnapshotJson(const backend::PendingRequestSnapshot& pending) {
            std::string type = pending.type;
            Json details = pending.details;

            // BackendCore now distinguishes these request families for trusted
            // in-process consumers.  Frontend Protocol v1 deliberately keeps
            // its existing generic unknown-request contract until A1.7 reviews
            // their remote exposure.  Preserve the previous generic envelope
            // for the three approval families and the previous empty details
            // for the other three request families.
            if (pending.type == "apply_patch_approval" || pending.type == "exec_command_approval" ||
                pending.type == "permissions_approval") {
                type = "unknown";
                Json legacyDetails = Json::object();
                for (const char* key : {"method",
                                        "methodTruncated",
                                        "originalMethodBytes",
                                        "params",
                                        "sensitiveFieldsRedacted",
                                        "paramsTruncated",
                                        "originalParamsBytes",
                                        "decodingError",
                                        "decodingErrorTruncated",
                                        "originalDecodingErrorBytes"}) {
                    if (details.contains(key)) {
                        legacyDetails[key] = details[key];
                    }
                }
                details = std::move(legacyDetails);
            } else if (pending.type == "attestation" || pending.type == "dynamic_tool_call" || pending.type == "mcp_elicitation") {
                type = "unknown";
                details = Json::object();
            }

            Json encoded{{"id", std::to_string(pending.id.value())}, {"type", std::move(type)}, {"details", std::move(details)}};
            if (pending.threadId.has_value()) {
                encoded["threadId"] = *pending.threadId;
            }
            if (pending.turnId.has_value()) {
                encoded["turnId"] = *pending.turnId;
            }
            if (pending.itemId.has_value()) {
                encoded["itemId"] = *pending.itemId;
            }
            return encoded;
        }

        Json extensionSnapshotJson(const backend::ExtensionSnapshot& extension) {
            Json encoded{{"method", extension.method}, {"params", extension.payload}};
            if (extension.decodingError.has_value()) {
                encoded["decodingError"] = *extension.decodingError;
            }
            if (extension.sensitiveFieldsRedacted) {
                encoded["sensitiveFieldsRedacted"] = true;
            }
            Json truncation = Json::object();
            if (extension.methodTruncated) {
                truncation["method"] = {{"originalBytes", extension.originalMethodBytes}, {"retainedBytes", extension.method.size()}};
            }
            if (extension.payloadTruncated) {
                truncation["params"] = Json::object();
                if (extension.originalPayloadBytes.has_value()) {
                    truncation["params"]["originalBytes"] = *extension.originalPayloadBytes;
                }
            }
            if (extension.decodingErrorTruncated) {
                truncation["decodingError"] = {{"originalBytes", extension.originalDecodingErrorBytes},
                                               {"retainedBytes", extension.decodingError ? extension.decodingError->size() : 0}};
            }
            if (!truncation.empty()) {
                encoded["truncation"] = std::move(truncation);
            }
            return encoded;
        }

        const backend::ThreadSnapshot* findThread(const backend::Snapshot& snapshot, std::string_view threadId) {
            for (const backend::ThreadSnapshot& thread : snapshot.threads) {
                if (thread.id == threadId) {
                    return &thread;
                }
            }
            return nullptr;
        }

        const backend::TurnSnapshot* findTurn(const backend::Snapshot& snapshot, std::string_view threadId, std::string_view turnId) {
            const backend::ThreadSnapshot* thread = findThread(snapshot, threadId);
            if (thread == nullptr) {
                return nullptr;
            }
            for (const backend::TurnSnapshot& turn : thread->turns) {
                if (turn.id == turnId) {
                    return &turn;
                }
            }
            return nullptr;
        }

        const backend::ItemSnapshot*
        findItem(const backend::Snapshot& snapshot, std::string_view threadId, std::string_view turnId, std::string_view itemId) {
            const backend::TurnSnapshot* turn = findTurn(snapshot, threadId, turnId);
            if (turn == nullptr) {
                return nullptr;
            }
            for (const backend::ItemSnapshot& item : turn->items) {
                if (item.id == itemId) {
                    return &item;
                }
            }
            return nullptr;
        }

        const backend::PendingRequestSnapshot* findPending(const backend::Snapshot& snapshot, backend::PendingRequestId id) {
            for (const backend::PendingRequestSnapshot& pending : snapshot.pendingRequests) {
                if (pending.id == id) {
                    return &pending;
                }
            }
            return nullptr;
        }

        Json backendSnapshotJson(const backend::Snapshot& snapshot,
                                 SequenceNumber frontendSequence,
                                 SequenceNumber oldestReplayableAfter,
                                 std::optional<SequenceNumber> oldestRetained,
                                 std::optional<SequenceNumber> newestRetained) {
            Json encoded{
                {"backendRevision", snapshot.sequence.value()},
                {"lifecycle", backendLifecycleName(snapshot.provider.lifecycle)},
                {"diagnostics", {{"received", snapshot.diagnostics.received}, {"recent", snapshot.diagnostics.recent}}},
                {"threads", Json::array()},
                {"pendingRequests", Json::array()},
                {"sessions", Json::array()},
                {"codexExtensions", Json::array()},
                {"omittedCodexExtensions", snapshot.omittedRecentExtensions},
                {"threadList",
                 {{"hasLoadedPage", snapshot.threadList.hasLoadedPage},
                  {"complete", snapshot.threadList.complete},
                  {"pagesLoaded", snapshot.threadList.pagesLoaded}}},
                {"journal", {{"oldestReplayableAfter", oldestReplayableAfter.value()}, {"currentSequence", frontendSequence.value()}}},
                {"sequenceExhausted", snapshot.sequenceExhausted}};
            if (snapshot.provider.lastError.has_value()) {
                encoded["lastLifecycleError"] = errorSnapshotJson(*snapshot.provider.lastError);
            }
            for (const backend::ThreadSnapshot& thread : snapshot.threads) {
                encoded["threads"].push_back(threadSnapshotJson(thread));
            }
            for (const backend::PendingRequestSnapshot& pending : snapshot.pendingRequests) {
                encoded["pendingRequests"].push_back(pendingRequestSnapshotJson(pending));
            }
            if (snapshot.controller.has_value()) {
                encoded["controllerSessionId"] = std::to_string(snapshot.controller->value());
            }
            for (const backend::SessionSnapshot& session : snapshot.sessions) {
                encoded["sessions"].push_back({{"sessionId", std::to_string(session.id.value())}, {"role", backendRoleName(session.role)}});
            }
            for (const backend::ExtensionSnapshot& extension : snapshot.recentExtensions) {
                encoded["codexExtensions"].push_back(extensionSnapshotJson(extension));
            }
            if (snapshot.threadList.nextCursor.has_value()) {
                encoded["threadList"]["nextCursor"] = *snapshot.threadList.nextCursor;
            }
            if (snapshot.threadList.backwardsCursor.has_value()) {
                encoded["threadList"]["backwardsCursor"] = *snapshot.threadList.backwardsCursor;
            }
            if (oldestRetained.has_value()) {
                encoded["journal"]["oldestRetainedSequence"] = oldestRetained->value();
            }
            if (newestRetained.has_value()) {
                encoded["journal"]["newestRetainedSequence"] = newestRetained->value();
            }
            return encoded;
        }

        ErrorCode frontendErrorCode(backend::CommandErrorCode code) {
            switch (code) {
                case backend::CommandErrorCode::PermissionDenied:
                    return ErrorCode::PermissionDenied;
                case backend::CommandErrorCode::InvalidCommand:
                    return ErrorCode::InvalidCommand;
                case backend::CommandErrorCode::NotFound:
                    return ErrorCode::NotFound;
                case backend::CommandErrorCode::Conflict:
                    return ErrorCode::Conflict;
                case backend::CommandErrorCode::LocalSubmissionFailure:
                    return ErrorCode::LocalSubmissionFailure;
                case backend::CommandErrorCode::TypedDecodingFailure:
                    return ErrorCode::TypedDecodingFailure;
                case backend::CommandErrorCode::RemoteAppServerError:
                    return ErrorCode::RemoteAppServerError;
                case backend::CommandErrorCode::Cancelled:
                    return ErrorCode::Cancelled;
                case backend::CommandErrorCode::BackendUnavailable:
                    return ErrorCode::BackendUnavailable;
            }
            return ErrorCode::InternalError;
        }

        std::string_view frontendCommandErrorMessage(backend::CommandErrorCode code) noexcept {
            switch (code) {
                case backend::CommandErrorCode::PermissionDenied:
                    return "frontend command was denied";
                case backend::CommandErrorCode::InvalidCommand:
                    return "frontend command is invalid";
                case backend::CommandErrorCode::NotFound:
                    return "frontend command target was not found";
                case backend::CommandErrorCode::Conflict:
                    return "frontend command conflicts with current state";
                case backend::CommandErrorCode::LocalSubmissionFailure:
                    return "frontend command could not be submitted";
                case backend::CommandErrorCode::TypedDecodingFailure:
                    return "frontend command result could not be decoded";
                case backend::CommandErrorCode::RemoteAppServerError:
                    return "Codex App Server rejected the command";
                case backend::CommandErrorCode::Cancelled:
                    return "frontend command was cancelled";
                case backend::CommandErrorCode::BackendUnavailable:
                    return "Codex App Server is unavailable";
            }
            return "frontend command failed";
        }

        class ResultCapacityFailure final : public std::exception {};

    } // namespace

    namespace detail {

        // EventJournal keeps canonical service records type-erased in private
        // entries. This source-private friend restores the concrete type only
        // for FrontendService's single append/replay projection path.
        class CanonicalEventJournalAccess {
        public:
            struct AppendResult {
                JournalAppendStatus status = JournalAppendStatus::EncodingFailure;
                std::optional<SequenceNumber> sequence;

                [[nodiscard]] bool accepted() const noexcept {
                    return status == JournalAppendStatus::Appended || status == JournalAppendStatus::NotRetained;
                }
            };

            struct ReplayResult {
                JournalReplayStatus status = JournalReplayStatus::Gap;
                SequenceNumber requestedAfter;
                SequenceNumber oldestReplayableAfter;
                SequenceNumber currentSequence;
                std::vector<std::shared_ptr<const CanonicalEventRecord>> records;
            };

            static AppendResult
            append(EventJournal& journal, const std::shared_ptr<CanonicalEventRecord>& record, std::size_t serializedBytes) noexcept {
                const EventJournal::OpaqueAppendResult result = journal.appendOpaque(record, serializedBytes);
                if (result.sequence.has_value()) {
                    assignCanonicalSequence(*record, *result.sequence);
                }
                return {result.status, result.sequence};
            }

            static ReplayResult replayAfter(const EventJournal& journal, SequenceNumber sequence) {
                const EventJournal::OpaqueReplayResult replay = journal.replayOpaqueAfter(sequence);
                ReplayResult result{replay.status, replay.requestedAfter, replay.oldestReplayableAfter, replay.currentSequence, {}};
                result.records.reserve(replay.records.size());
                for (const std::shared_ptr<const void>& opaque : replay.records) {
                    result.records.push_back(std::static_pointer_cast<const CanonicalEventRecord>(opaque));
                }
                return result;
            }
        };

    } // namespace detail

    struct FrontendConnection::Control {
        std::weak_ptr<FrontendService::Impl> service;
        std::uint64_t localId = 0;
        FrontendPeerContext peer;
        FrontendConnectionCallbacks callbacks;
        std::optional<backend::FrontendSession> backendSession;
        std::optional<FrontendPrincipal> principal;
        std::vector<FrontendCapability> negotiatedCapabilities;
        std::deque<OutboundMessage> outbound;
        std::unordered_map<std::string, generated::MethodId> pendingRequests;
        std::size_t outboundBytes = 0;
        std::uint64_t rateTokens = 0;
        std::uint64_t lastRateRefillMs = 0;
        FrontendTimerCancellation handshakeTimer;
        bool open = true;
        bool helloDone = false;
        bool authenticationAttempted = false;
        bool deliveryScheduled = false;
        bool closeAfterDelivery = false;
        bool closedNotified = false;
    };

    class FrontendService::Impl : public std::enable_shared_from_this<FrontendService::Impl> {
    public:
        struct FailedAuthenticationWindow {
            std::size_t failures = 0;
            std::uint64_t generation = 0;
            FrontendTimerCancellation expiration;
        };

        Impl(backend::detail::BackendCoreRuntime& backend, FrontendServiceOptions options)
            : backendCore(&backend)
            , serviceOptions(std::move(options))
            , journal(serviceOptions.journal)
            , batchBuilder(serviceOptions.batches)
            , coalescer(serviceOptions.coalescer) {
            if (!serviceOptions.scheduler) {
                serviceOptions.scheduler = [](std::function<void()> callback) {
                    core::EventReceiver::atNextTick(callback);
                };
            }
            if (!serviceOptions.monotonicClockMs) {
                serviceOptions.monotonicClockMs = [] {
                    return static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                };
            }
            if (!serviceOptions.timerScheduler) {
                serviceOptions.timerScheduler = [](std::uint64_t delayMs, std::function<void()> callback) {
                    auto timer = std::make_shared<std::optional<core::timer::Timer>>();
                    timer->emplace(
                        core::timer::Timer::singleshotTimer(std::move(callback), utils::Timeval(static_cast<double>(delayMs) / 1000.0)));
                    return [timer]() mutable {
                        if (timer && timer->has_value()) {
                            timer->value().cancel();
                            timer->reset();
                        }
                    };
                };
            }
        }

        void initialize() {
            const std::weak_ptr<Impl> weakSelf = shared_from_this();
            observer = backendCore->subscribe(
                backend::BackendObserverCallbacks{[weakSelf](const std::vector<backend::SequencedBackendEvent>& events) {
                                                      if (const std::shared_ptr<Impl> self = weakSelf.lock()) {
                                                          self->onBackendEvents(events);
                                                      }
                                                  },
                                                  [weakSelf](const backend::Snapshot& snapshot) {
                                                      if (const std::shared_ptr<Impl> self = weakSelf.lock()) {
                                                          self->onBackendResynchronize(snapshot);
                                                      }
                                                  }});
        }

        FrontendConnection openConnection(FrontendPeerContext peer, FrontendConnectionCallbacks callbacks) {
            if (!open || serviceOptions.maxConnections == 0 || connections.size() >= serviceOptions.maxConnections ||
                serviceOptions.maxUnauthenticatedConnections == 0 ||
                unauthenticatedConnections >= serviceOptions.maxUnauthenticatedConnections) {
                return FrontendConnection{};
            }
            if (nextConnectionId == std::numeric_limits<std::uint64_t>::max()) {
                return FrontendConnection{};
            }
            auto control = std::make_shared<FrontendConnection::Control>();
            control->service = shared_from_this();
            control->localId = ++nextConnectionId;
            control->peer = std::move(peer);
            control->callbacks = std::move(callbacks);
            control->lastRateRefillMs = nowMs();
            control->rateTokens = saturatingMultiply(serviceOptions.maxInboundBurst, 1000);
            connections.emplace(control->localId, control);
            if (unauthenticatedConnections < std::numeric_limits<std::size_t>::max()) {
                ++unauthenticatedConnections;
            }
            scheduleHandshakeTimeout(control);
            return FrontendConnection(std::move(control));
        }

        std::uint64_t nowMs() const noexcept {
            try {
                return serviceOptions.monotonicClockMs();
            } catch (...) {
                return 0;
            }
        }

        void cancelTimer(FrontendTimerCancellation& cancellation) noexcept {
            FrontendTimerCancellation callback = std::move(cancellation);
            cancellation = {};
            if (callback) {
                try {
                    callback();
                } catch (...) {
                }
            }
        }

        void scheduleHandshakeTimeout(const std::shared_ptr<FrontendConnection::Control>& control) noexcept {
            if (!control || serviceOptions.handshakeTimeoutMs == 0) {
                closeControl(control, "frontend handshake capacity is zero");
                return;
            }
            const std::weak_ptr<Impl> weakSelf = shared_from_this();
            const std::weak_ptr<FrontendConnection::Control> weakControl = control;
            try {
                control->handshakeTimer = serviceOptions.timerScheduler(serviceOptions.handshakeTimeoutMs, [weakSelf, weakControl] {
                    if (const std::shared_ptr<Impl> self = weakSelf.lock()) {
                        self->schedule([weakSelf, weakControl] {
                            const std::shared_ptr<Impl> lockedSelf = weakSelf.lock();
                            const std::shared_ptr<FrontendConnection::Control> lockedControl = weakControl.lock();
                            if (lockedSelf && lockedControl && lockedControl->open && !lockedControl->helloDone) {
                                lockedSelf->enqueueProtocolError(
                                    lockedControl,
                                    CodecError{ErrorCode::AuthenticationRequired,
                                               "frontend authentication did not complete before the handshake deadline",
                                               true,
                                               {},
                                               std::nullopt,
                                               std::nullopt},
                                    true);
                            }
                        });
                    }
                });
            } catch (...) {
                closeControl(control, "failed to schedule frontend handshake timeout");
            }
        }

        void schedule(std::function<void()> callback) noexcept {
            try {
                serviceOptions.scheduler(callback);
            } catch (...) {
                // Preserve ordered asynchronous delivery even when an
                // injected scheduler rejects a callback.
                try {
                    core::EventReceiver::atNextTick(std::move(callback));
                } catch (...) {
                }
            }
        }

        bool enqueue(const std::shared_ptr<FrontendConnection::Control>& control,
                     ServerMessage message,
                     bool closeAfterDelivery = false) noexcept {
            if (!open || !control || !control->open) {
                return false;
            }
            const auto serialized = Codec::serializeServer(message);
            if (!serialized) {
                closeControl(control, "frontend protocol serialization failed");
                return false;
            }
            const std::size_t size = serialized.value().size();
            if (serviceOptions.maxOutboundMessagesPerConnection == 0 ||
                control->outbound.size() >= serviceOptions.maxOutboundMessagesPerConnection ||
                size > serviceOptions.maxOutboundBytesPerConnection ||
                control->outboundBytes > serviceOptions.maxOutboundBytesPerConnection - size) {
                closeControl(control, "frontend outbound backpressure limit exceeded");
                return false;
            }

            try {
                control->outbound.push_back(OutboundMessage{std::move(message), serialized.value(), size});
                control->outboundBytes += size;
                control->closeAfterDelivery = control->closeAfterDelivery || closeAfterDelivery;
                if (!control->deliveryScheduled) {
                    control->deliveryScheduled = true;
                    scheduleDelivery(control);
                }
                return true;
            } catch (...) {
                closeControl(control, "frontend outbound queue allocation failed");
                return false;
            }
        }

        void scheduleDelivery(const std::shared_ptr<FrontendConnection::Control>& control) noexcept {
            const std::weak_ptr<Impl> weakSelf = shared_from_this();
            const std::weak_ptr<FrontendConnection::Control> weakControl = control;
            schedule([weakSelf, weakControl]() {
                const std::shared_ptr<Impl> self = weakSelf.lock();
                const std::shared_ptr<FrontendConnection::Control> locked = weakControl.lock();
                if (self && locked) {
                    self->deliver(locked);
                }
            });
        }

        void deliver(const std::shared_ptr<FrontendConnection::Control>& control) noexcept {
            if (!control->open) {
                return;
            }
            const std::size_t deliveryLimit = serviceOptions.maxMessagesPerDelivery;
            if (deliveryLimit == 0) {
                closeControl(control, "frontend delivery limit is zero");
                return;
            }

            std::size_t delivered = 0;
            while (control->open && !control->outbound.empty() && delivered < deliveryLimit) {
                OutboundMessage message = std::move(control->outbound.front());
                control->outbound.pop_front();
                control->outboundBytes -= message.serializedBytes;
                ++delivered;

                bool accepted = false;
                try {
                    accepted = control->callbacks.onMessage && control->callbacks.onMessage(message);
                } catch (...) {
                    closeControl(control, "frontend outbound callback threw");
                    return;
                }
                if (!accepted) {
                    closeControl(control, "frontend transport rejected outbound data");
                    return;
                }
            }

            if (!control->open) {
                return;
            }
            if (!control->outbound.empty()) {
                scheduleDelivery(control);
                return;
            }
            control->deliveryScheduled = false;
            if (control->closeAfterDelivery) {
                closeControl(control, "frontend protocol requested connection close");
            }
        }

        void closeControl(const std::shared_ptr<FrontendConnection::Control>& control, std::string reason) noexcept {
            if (!control || !control->open) {
                return;
            }
            control->open = false;
            control->outbound.clear();
            control->outboundBytes = 0;
            control->pendingRequests.clear();
            cancelTimer(control->handshakeTimer);
            control->deliveryScheduled = false;
            control->closeAfterDelivery = false;
            if (control->backendSession.has_value()) {
                control->backendSession->close(reason);
                control->backendSession.reset();
            }
            if (control->principal.has_value()) {
                if (authenticatedConnections != 0) {
                    --authenticatedConnections;
                }
            } else if (unauthenticatedConnections != 0) {
                --unauthenticatedConnections;
            }
            connections.erase(control->localId);

            if (!control->closedNotified && control->callbacks.onClosed) {
                control->closedNotified = true;
                auto callback = control->callbacks.onClosed;
                schedule([callback = std::move(callback), reason = std::move(reason)]() {
                    try {
                        callback(reason);
                    } catch (...) {
                    }
                });
            }
        }

        std::size_t unauthenticatedConnectionCount() const noexcept {
            return unauthenticatedConnections;
        }

        std::size_t authenticatedConnectionCount() const noexcept {
            return authenticatedConnections;
        }

        void enqueueProtocolError(const std::shared_ptr<FrontendConnection::Control>& control,
                                  CodecError error,
                                  bool closeAfterDelivery) noexcept {
            ProtocolErrorMessage message;
            message.code = error.code;
            message.message = std::move(error.message);
            message.closeConnection = closeAfterDelivery || error.closeConnection;
            message.requestId = std::move(error.requestId);
            message.details = std::move(error.details);
            if (!error.supportedVersions.empty()) {
                message.supportedVersions = std::move(error.supportedVersions);
            } else {
                message.supportedVersions.assign(SupportedProtocolVersions.begin(), SupportedProtocolVersions.end());
            }
            const bool closes = message.closeConnection;
            enqueue(control, ServerMessage{std::move(message)}, closes);
        }

        ConnectionReceiveResult receiveError(const std::shared_ptr<FrontendConnection::Control>& control, CodecError error) noexcept {
            if (!control || !control->open) {
                return {ConnectionReceiveStatus::Closed, std::move(error)};
            }
            if (control->closeAfterDelivery) {
                return {ConnectionReceiveStatus::Closing, std::move(error)};
            }

            // Before authentication, retain only envelope- and admission-level
            // diagnostics. In particular, a codec failure while decoding a
            // command must not disclose whether its method or parameters are
            // defined by the privileged protocol surface.
            if (!control->principal.has_value()) {
                switch (error.code) {
                    case ErrorCode::MalformedJson:
                    case ErrorCode::WrongProtocol:
                    case ErrorCode::UnsupportedVersion:
                    case ErrorCode::FrameTooLarge:
                    case ErrorCode::RateLimited:
                    case ErrorCode::AuthenticationRequired:
                    case ErrorCode::AuthenticationFailed:
                    case ErrorCode::OriginRejected:
                    case ErrorCode::TransportSecurityRequired:
                        break;
                    default:
                        error = CodecError{
                            ErrorCode::InvalidField, "frontend handshake message is invalid", true, {}, std::nullopt, std::nullopt};
                        break;
                }
            }
            const bool closeConnection = !control->helloDone || error.closeConnection || error.code == ErrorCode::MalformedJson ||
                                         error.code == ErrorCode::WrongProtocol || error.code == ErrorCode::UnsupportedVersion;
            CodecError returned = error;
            enqueueProtocolError(control, std::move(error), closeConnection);
            return {closeConnection ? ConnectionReceiveStatus::Closing : ConnectionReceiveStatus::Rejected, std::move(returned)};
        }

        detail::FrontendProjectionContext projectionContext(const std::shared_ptr<FrontendConnection::Control>& control) const noexcept {
            if (!control || !control->principal.has_value()) {
                return {};
            }
            return detail::makeProjectionContext(*control->principal, control->negotiatedCapabilities);
        }

        using CanonicalEventRecords = std::vector<std::shared_ptr<const detail::CanonicalEventRecord>>;

        UpdateBatchResult buildProjectedBatches(const std::shared_ptr<FrontendConnection::Control>& control,
                                                const CanonicalEventRecords& canonicalEvents,
                                                std::optional<SequenceNumber> replayAfter = std::nullopt) const noexcept {
            try {
                std::vector<FrontendEvent> projected;
                const detail::FrontendProjectionContext context = projectionContext(control);
                for (const std::shared_ptr<const detail::CanonicalEventRecord>& record : canonicalEvents) {
                    if (!record) {
                        return {UpdateBatchStatus::EncodingFailure, {}, std::nullopt};
                    }
                    detail::EventProjection occurrence = detail::projectEvent(*record, context, replayAfter);
                    projected.insert(projected.end(),
                                     std::make_move_iterator(occurrence.events.begin()),
                                     std::make_move_iterator(occurrence.events.end()));
                }
                if (projected.empty()) {
                    return {UpdateBatchStatus::Success, {}, std::nullopt};
                }
                return batchBuilder.build(projected);
            } catch (...) {
                return {UpdateBatchStatus::EncodingFailure, {}, std::nullopt};
            }
        }

        detail::CanonicalSnapshotRecord canonicalSnapshot(const backend::Snapshot& backendSnapshot) const {
            Json legacy = backendSnapshotJson(backendSnapshot,
                                              journal.currentSequence(),
                                              journal.oldestReplayableAfter(),
                                              journal.oldestRetainedSequence(),
                                              journal.newestRetainedSequence());
            legacy["frontendSequenceExhausted"] = sequenceExhausted;
            detail::CanonicalSnapshotRecord record =
                detail::makeCanonicalSnapshotRecord(std::move(legacy), backendSnapshot, journal.currentSequence());
            record.expandedState.value["frontendSequenceExhausted"] = sequenceExhausted;
            return record;
        }

        Snapshot frontendSnapshot(const std::shared_ptr<FrontendConnection::Control>& control,
                                  const backend::Snapshot* supplied = nullptr) const {
            const backend::Snapshot backendSnapshot = supplied != nullptr ? *supplied : backendCore->snapshot();
            const detail::CanonicalSnapshotRecord record = canonicalSnapshot(backendSnapshot);
            const auto projected = detail::projectSnapshot(record, projectionContext(control));
            if (projected.has_value()) {
                return projected->snapshot;
            }
            Json extensions = Json::object();
            if (control && std::find(control->negotiatedCapabilities.begin(),
                                     control->negotiatedCapabilities.end(),
                                     FrontendCapability::ScopeProjectedState) != control->negotiatedCapabilities.end()) {
                extensions["scopeProjection"] = {{"omittedFields", Json::array({"/"})}, {"redactedFields", Json::array()}};
            }
            return Snapshot{journal.currentSequence(), Json::object(), std::move(extensions)};
        }

        bool enqueueSnapshot(const std::shared_ptr<FrontendConnection::Control>& control,
                             const backend::Snapshot* supplied = nullptr) noexcept {
            try {
                return enqueue(control, ServerMessage{frontendSnapshot(control, supplied)});
            } catch (...) {
                closeControl(control, "failed to create frontend snapshot");
                return false;
            }
        }

        bool enqueueBatches(const std::shared_ptr<FrontendConnection::Control>& control,
                            const CanonicalEventRecords& canonicalEvents,
                            std::optional<SequenceNumber> replayAfter = std::nullopt) noexcept {
            const UpdateBatchResult built = buildProjectedBatches(control, canonicalEvents, replayAfter);
            if (built.requiresSnapshot()) {
                return enqueueSnapshot(control);
            }
            if (!built.success()) {
                return false;
            }
            for (const BoundedEventBatch& batch : built.batches) {
                if (!enqueue(control, ServerMessage{batch.batch})) {
                    return false;
                }
            }
            return true;
        }

        bool verifiedLocalTrust(const FrontendPeerContext& peer) const noexcept {
            return serviceOptions.allowVerifiedLocalTrust && peer.transport == FrontendTransportKind::Unix &&
                   serviceOptions.trustedLocalUserId.has_value() && peer.localPeer && peer.unixUserId.has_value() &&
                   *peer.unixUserId == *serviceOptions.trustedLocalUserId;
        }

        bool insecureLocalTrust(const FrontendPeerContext& peer) const noexcept {
            return serviceOptions.allowInsecureLocalTrust && peer.transport == FrontendTransportKind::Unix;
        }

        FrontendPrincipal localTrustedPrincipal(const FrontendPeerContext& peer, bool verified) const {
            FrontendPrincipal principal;
            principal.id =
                verified && peer.unixUserId.has_value() ? "unix-user-" + std::to_string(*peer.unixUserId) : "insecure-local-override";
            principal.scopes.assign(LocalTrustedScopes.begin(), LocalTrustedScopes.end());
            principal.profile = std::string(LocalTrustedScopeProfile.name);
            principal.localTrusted = true;
            return principal;
        }

        bool peerAuthenticationRateLimited(const std::string& key) const noexcept {
            if (serviceOptions.maximumFailedAuthenticationsPerPeer == 0 || serviceOptions.failedAuthenticationWindowMs == 0) {
                return true;
            }
            const auto found = failedAuthentications.find(key);
            return found != failedAuthentications.end() && found->second.failures >= serviceOptions.maximumFailedAuthenticationsPerPeer;
        }

        bool peerAuthenticationAccountingFull(const std::string& key) const noexcept {
            return !failedAuthentications.contains(key) && failedAuthentications.size() >= serviceOptions.maxConnections;
        }

        void expireFailedAuthentication(std::string key, std::uint64_t generation) noexcept {
            const auto found = failedAuthentications.find(key);
            if (found != failedAuthentications.end() && found->second.generation == generation) {
                found->second.expiration = {};
                failedAuthentications.erase(found);
            }
        }

        void recordFailedAuthentication(const std::string& key) noexcept {
            if (serviceOptions.maximumFailedAuthenticationsPerPeer == 0 || serviceOptions.failedAuthenticationWindowMs == 0) {
                return;
            }
            FailedAuthenticationWindow& window = failedAuthentications[key];
            if (window.failures < std::numeric_limits<std::size_t>::max()) {
                ++window.failures;
            }
            if (window.generation != 0) {
                return;
            }
            if (nextFailureWindowGeneration < std::numeric_limits<std::uint64_t>::max()) {
                ++nextFailureWindowGeneration;
            }
            window.generation = std::max<std::uint64_t>(1, nextFailureWindowGeneration);
            const std::uint64_t generation = window.generation;
            const std::weak_ptr<Impl> weakSelf = shared_from_this();
            try {
                window.expiration = serviceOptions.timerScheduler(serviceOptions.failedAuthenticationWindowMs, [weakSelf, key, generation] {
                    if (const std::shared_ptr<Impl> self = weakSelf.lock()) {
                        self->schedule([weakSelf, key, generation] {
                            if (const std::shared_ptr<Impl> locked = weakSelf.lock()) {
                                locked->expireFailedAuthentication(key, generation);
                            }
                        });
                    }
                });
            } catch (...) {
                // Failing closed is safer than silently discarding the peer
                // admission budget when the event-loop timer cannot be armed.
                window.failures = serviceOptions.maximumFailedAuthenticationsPerPeer;
            }
        }

        AuthenticationFailureCode recordPreAuthenticationFailure(const FrontendPeerContext& peer,
                                                                 AuthenticationFailureCode failure) noexcept {
            const std::string key = peerAdmissionKey(peer);
            if (peerAuthenticationRateLimited(key) || peerAuthenticationAccountingFull(key)) {
                return AuthenticationFailureCode::RateLimited;
            }
            recordFailedAuthentication(key);
            return failure;
        }

        bool authenticate(const std::shared_ptr<FrontendConnection::Control>& control, const Hello& hello) noexcept {
            if (!control || !control->open || control->authenticationAttempted) {
                if (control && control->open) {
                    enqueueProtocolError(control,
                                         CodecError{ErrorCode::AuthenticationFailed,
                                                    "frontend authentication may only be attempted once",
                                                    true,
                                                    {},
                                                    std::nullopt,
                                                    std::nullopt},
                                         true);
                }
                return false;
            }
            control->authenticationAttempted = true;

            const std::string admissionKey = peerAdmissionKey(control->peer);
            if (peerAuthenticationRateLimited(admissionKey) || peerAuthenticationAccountingFull(admissionKey)) {
                enqueueProtocolError(control,
                                     CodecError{ErrorCode::RateLimited,
                                                authenticationErrorMessage(AuthenticationFailureCode::RateLimited),
                                                true,
                                                {},
                                                std::nullopt,
                                                std::nullopt},
                                     true);
                return false;
            }

            AuthenticationResult result = AuthenticationFailure{AuthenticationFailureCode::AuthenticationRequired};
            const bool verified = verifiedLocalTrust(control->peer);
            if (verified || insecureLocalTrust(control->peer)) {
                result = AuthenticationSuccess{localTrustedPrincipal(control->peer, verified)};
            } else if (serviceOptions.authenticator) {
                try {
                    result = serviceOptions.authenticator(control->peer,
                                                          hello.authentication.value_or(AuthenticationCredential{NoCredential{}}));
                } catch (...) {
                    result = AuthenticationFailure{AuthenticationFailureCode::AuthenticationFailed};
                }
            }

            if (const auto* success = std::get_if<AuthenticationSuccess>(&result); success && validPrincipal(success->principal)) {
                control->principal = success->principal;
                if (unauthenticatedConnections != 0) {
                    --unauthenticatedConnections;
                }
                if (authenticatedConnections < std::numeric_limits<std::size_t>::max()) {
                    ++authenticatedConnections;
                }
                cancelTimer(control->handshakeTimer);
                return true;
            }

            const AuthenticationFailureCode code = std::holds_alternative<AuthenticationFailure>(result)
                                                       ? std::get<AuthenticationFailure>(result).code
                                                       : AuthenticationFailureCode::AuthenticationFailed;
            recordFailedAuthentication(admissionKey);
            enqueueProtocolError(
                control, CodecError{frontendErrorCode(code), authenticationErrorMessage(code), true, {}, std::nullopt, std::nullopt}, true);
            return false;
        }

        bool deploymentEnabled(const generated::MethodMetadata& metadata) const noexcept {
            if (metadata.defaultEnabled) {
                return true;
            }
            if (metadata.capability == "conditional_command_execution") {
                return serviceOptions.enableCommandExecutionMethods && static_cast<bool>(serviceOptions.commandExecutionPolicy);
            }
            if (metadata.capability == "conditional_filesystem") {
                return std::find(metadata.requiredScopes.begin(), metadata.requiredScopes.end(), FrontendScope::FilesystemWrite) !=
                               metadata.requiredScopes.end()
                           ? serviceOptions.enableFilesystemWriteMethods && static_cast<bool>(serviceOptions.filesystemWritePolicy)
                           : serviceOptions.enableFilesystemReadMethods && static_cast<bool>(serviceOptions.filesystemReadPolicy);
            }
            return false;
        }

        bool conditionalInvocationAllowed(const generated::MethodMetadata& metadata,
                                          const FrontendPrincipal& principal,
                                          const Json& validatedParameters) const noexcept {
            const FrontendInvocationPolicy* policy = nullptr;
            if (metadata.capability == "conditional_command_execution") {
                policy = &serviceOptions.commandExecutionPolicy;
            } else if (metadata.capability == "conditional_filesystem") {
                policy = std::find(metadata.requiredScopes.begin(), metadata.requiredScopes.end(), FrontendScope::FilesystemWrite) !=
                                 metadata.requiredScopes.end()
                             ? &serviceOptions.filesystemWritePolicy
                             : &serviceOptions.filesystemReadPolicy;
            }
            if (policy == nullptr) {
                return true;
            }
            try {
                return static_cast<bool>(*policy) && (*policy)(principal, metadata.method, validatedParameters);
            } catch (...) {
                return false;
            }
        }

        void refreshLifecycleAction(const backend::ProviderSnapshot& provider) noexcept {
            if (!lifecycleActionInFlight.has_value()) {
                return;
            }
            const bool complete =
                *lifecycleActionInFlight == detail::NativeServiceAction::ProviderStop
                    ? provider.lifecycle == backend::ProviderLifecycle::Stopped ||
                          (provider.lifecycle == backend::ProviderLifecycle::Failed && !provider.desiredRunning)
                    : provider.lifecycle == backend::ProviderLifecycle::Ready || provider.lifecycle == backend::ProviderLifecycle::Failed;
            if (complete) {
                lifecycleActionInFlight.reset();
            }
        }

        std::vector<FrontendMethod> definedMethods() const {
            std::vector<FrontendMethod> result;
            result.reserve(generated::AllMethods.size());
            for (const generated::MethodMetadata& metadata : generated::AllMethods) {
                result.emplace_back(metadata.method);
            }
            return result;
        }

        std::vector<FrontendMethod> implementedMethods() const {
            std::vector<FrontendMethod> result;
            for (const generated::MethodMetadata& metadata : generated::AllMethods) {
                if (metadata.currentlyImplemented) {
                    result.emplace_back(metadata.method);
                }
            }
            return result;
        }

        std::vector<FrontendMethod> availableMethods() const {
            std::vector<FrontendMethod> result;
            for (const generated::MethodMetadata& metadata : generated::AllMethods) {
                if (metadata.currentlyImplemented && deploymentEnabled(metadata)) {
                    result.emplace_back(metadata.method);
                }
            }
            return result;
        }

        std::vector<FrontendMethod> permittedMethods(const FrontendPrincipal& principal) const {
            std::vector<FrontendMethod> result;
            if (!validPrincipal(principal)) {
                return result;
            }
            for (const generated::MethodMetadata& metadata : generated::AllMethods) {
                if (metadata.currentlyImplemented && deploymentEnabled(metadata) && hasRequiredScopes(principal, metadata.requiredScopes)) {
                    result.emplace_back(metadata.method);
                }
            }
            return result;
        }

        std::vector<FrontendCapability> implementedCapabilities() const {
            return capabilityAdvertisement().implemented;
        }

        CapabilityAdvertisement capabilityAdvertisement() const {
            return detail::computeCapabilities(AISUITE_CODEX_CPP_CLIENT_SDK_BUILT != 0, declaredTransportCounts.size()).advertisement;
        }

        void declareTransportFamily(FrontendTransportKind transport) {
            std::size_t& declarations = declaredTransportCounts[transport];
            if (declarations != std::numeric_limits<std::size_t>::max()) {
                ++declarations;
            }
        }

        void withdrawTransportFamily(FrontendTransportKind transport) noexcept {
            const auto found = declaredTransportCounts.find(transport);
            if (found == declaredTransportCounts.end()) {
                return;
            }
            if (found->second > 1) {
                --found->second;
            } else {
                declaredTransportCounts.erase(found);
            }
        }

        std::vector<FrontendTransportKind> enabledTransportFamilies() const {
            std::vector<FrontendTransportKind> result;
            result.reserve(declaredTransportCounts.size());
            for (const auto& [transport, declarations] : declaredTransportCounts) {
                if (declarations != 0) {
                    result.push_back(transport);
                }
            }
            return result;
        }

        std::vector<FrontendCapability> negotiatedCapabilities(const Hello& hello, const CapabilityAdvertisement& advertisement) const {
            std::vector<FrontendCapability> negotiated;
            if (!hello.capabilities.has_value()) {
                return negotiated;
            }
            negotiated.reserve(hello.capabilities->size());
            for (FrontendCapability capability : *hello.capabilities) {
                if (std::find(advertisement.implemented.begin(), advertisement.implemented.end(), capability) !=
                        advertisement.implemented.end() &&
                    std::find(negotiated.begin(), negotiated.end(), capability) == negotiated.end()) {
                    negotiated.push_back(capability);
                }
            }
            return negotiated;
        }

        void synchronize(const std::shared_ptr<FrontendConnection::Control>& control, const Hello& hello) noexcept {
            if (!control->open || control->helloDone) {
                enqueueProtocolError(control,
                                     CodecError{ErrorCode::InvalidCommand,
                                                "hello has already completed for this connection",
                                                false,
                                                {},
                                                std::nullopt,
                                                std::nullopt},
                                     false);
                return;
            }

            if (!authenticate(control, hello)) {
                return;
            }

            const CapabilityAdvertisement handshakeAdvertisement = capabilityAdvertisement();
            control->negotiatedCapabilities = negotiatedCapabilities(hello, handshakeAdvertisement);

            flushNow();
            const std::weak_ptr<FrontendConnection::Control> weakControl = control;
            const std::weak_ptr<Impl> weakSelf = shared_from_this();
            try {
                control->backendSession.emplace(backendCore->openSession(
                    backend::FrontendSessionCallbacks{{},
                                                      [weakSelf, weakControl](const backend::Snapshot& snapshot) {
                                                          const auto self = weakSelf.lock();
                                                          const auto locked = weakControl.lock();
                                                          if (self && locked && locked->open) {
                                                              self->enqueueSnapshot(locked, &snapshot);
                                                          }
                                                      },
                                                      [weakSelf, weakControl](const backend::CommandCompletion& completion) {
                                                          const auto self = weakSelf.lock();
                                                          const auto locked = weakControl.lock();
                                                          if (self && locked && locked->open) {
                                                              self->onCommandCompleted(locked, completion);
                                                          }
                                                      },
                                                      [weakSelf, weakControl](const std::string& reason) {
                                                          const auto self = weakSelf.lock();
                                                          const auto locked = weakControl.lock();
                                                          if (self && locked && locked->open) {
                                                              self->closeControl(locked, reason);
                                                          }
                                                      }}));
            } catch (...) {
                enqueueProtocolError(
                    control,
                    CodecError{
                        ErrorCode::BackendUnavailable, "failed to open a backend frontend session", false, {}, std::nullopt, std::nullopt},
                    true);
                return;
            }
            if (!control->backendSession->isOpen()) {
                enqueueProtocolError(
                    control,
                    CodecError{
                        ErrorCode::BackendUnavailable, "backend frontend session is unavailable", false, {}, std::nullopt, std::nullopt},
                    true);
                return;
            }

            SyncMode syncMode = SyncMode::Snapshot;
            CanonicalEventRecords replayRecords;
            bool replayUsable = false;
            if (hello.resumeAfter.has_value()) {
                try {
                    const detail::CanonicalEventJournalAccess::ReplayResult replay =
                        detail::CanonicalEventJournalAccess::replayAfter(journal, *hello.resumeAfter);
                    if (replay.status == JournalReplayStatus::Available) {
                        const UpdateBatchResult batches = buildProjectedBatches(control, replay.records, *hello.resumeAfter);
                        if (batches.success()) {
                            syncMode = SyncMode::Replay;
                            replayRecords = replay.records;
                            replayUsable = true;
                        }
                    }
                } catch (...) {
                }
            }

            control->helloDone = true;
            const std::string id = std::to_string(control->backendSession->id().value());
            Welcome welcome{id, SessionRole::Observer, journal.currentSequence(), syncMode, Json::object()};
            if (hello.capabilities.has_value()) {
                welcome.capabilities = handshakeAdvertisement;
                welcome.availableMethods = availableMethods();
                welcome.permittedMethods = permittedMethods(*control->principal);
                Json permittedScopes = Json::array();
                for (const FrontendScope scope : control->principal->scopes) {
                    permittedScopes.push_back(std::string(toString(scope)));
                }
                welcome.extensions["permittedScopes"] = std::move(permittedScopes);
            }
            if (!enqueue(control, ServerMessage{std::move(welcome)})) {
                return;
            }
            if (replayUsable) {
                if (!enqueueBatches(control, replayRecords, hello.resumeAfter)) {
                    closeControl(control, "failed to enqueue frontend replay");
                    return;
                }
            } else {
                enqueueSnapshot(control);
            }
            enqueue(control, ServerMessage{SyncComplete{journal.currentSequence(), Json::object()}});
        }

        void enqueueFailure(const std::shared_ptr<FrontendConnection::Control>& control,
                            std::string requestId,
                            ErrorCode code,
                            std::string message,
                            std::optional<Json> details = std::nullopt) noexcept {
            enqueue(control,
                    ServerMessage{Response::failure(std::move(requestId),
                                                    CommandError{code, std::move(message), std::move(details), Json::object()})});
        }

        bool inboundFrameAdmissible(std::size_t bytes) const noexcept {
            return serviceOptions.maximumInboundMessageBytes != 0 && bytes <= serviceOptions.maximumInboundMessageBytes;
        }

        bool consumeInboundAdmission(const std::shared_ptr<FrontendConnection::Control>& control) noexcept {
            if (!control || serviceOptions.maxInboundMessagesPerSecond == 0 || serviceOptions.maxInboundBurst == 0) {
                return false;
            }
            const std::uint64_t now = nowMs();
            const std::uint64_t elapsed = now >= control->lastRateRefillMs ? now - control->lastRateRefillMs : 0;
            if (elapsed != 0) {
                const std::uint64_t rate = static_cast<std::uint64_t>(serviceOptions.maxInboundMessagesPerSecond);
                const std::uint64_t added =
                    elapsed > std::numeric_limits<std::uint64_t>::max() / rate ? std::numeric_limits<std::uint64_t>::max() : elapsed * rate;
                const std::uint64_t capacity = saturatingMultiply(serviceOptions.maxInboundBurst, 1000);
                control->rateTokens =
                    added > capacity - std::min(capacity, control->rateTokens) ? capacity : std::min(capacity, control->rateTokens) + added;
                control->lastRateRefillMs = now;
            }
            if (control->rateTokens < 1000) {
                return false;
            }
            control->rateTokens -= 1000;
            return true;
        }

        ConnectionReceiveResult
        receiveFrameError(const std::shared_ptr<FrontendConnection::Control>& control, ErrorCode code, std::string message) noexcept {
            return receiveError(control, CodecError{code, std::move(message), true, {}, std::nullopt, std::nullopt});
        }

        ConnectionReceiveResult receive(const std::shared_ptr<FrontendConnection::Control>& control,
                                        const ClientMessage& message,
                                        bool consumeAdmission = true) noexcept {
            if (!open || !control || !control->open) {
                return {ConnectionReceiveStatus::Closed, std::nullopt};
            }
            if (control->closeAfterDelivery) {
                return {ConnectionReceiveStatus::Closing, std::nullopt};
            }
            if (consumeAdmission && !consumeInboundAdmission(control)) {
                return receiveFrameError(control, ErrorCode::RateLimited, "frontend inbound message rate limit exceeded");
            }
            try {
                if (const auto* hello = std::get_if<Hello>(&message)) {
                    if (control->helloDone) {
                        CodecError error{ErrorCode::InvalidCommand, "hello may only be sent once", false, {}, std::nullopt, std::nullopt};
                        return receiveError(control, std::move(error));
                    }
                    synchronize(control, *hello);
                    return {control->open && !control->closeAfterDelivery ? ConnectionReceiveStatus::Accepted
                                                                          : ConnectionReceiveStatus::Closing,
                            std::nullopt};
                }

                const Command& command = std::get<Command>(message);
                if (!control->helloDone) {
                    CodecError error{ErrorCode::AuthenticationRequired,
                                     "frontend authentication must complete before commands are accepted",
                                     true,
                                     {},
                                     std::nullopt,
                                     std::nullopt};
                    return receiveError(control, std::move(error));
                }
                const CodecResult<Json> encoded = Codec::encodeClient(ClientMessage{command});
                if (!encoded) {
                    return receiveError(control, encoded.error());
                }
                const CodecResult<generated::DefinedCommand> decoded = Codec::decodeDefinedCommand(encoded.value());
                if (!decoded) {
                    return receiveError(control, decoded.error());
                }
                return receiveDefined(control, decoded.value(), false);
            } catch (const std::exception&) {
                CodecError error{ErrorCode::InternalError, "failed to process frontend message", false, {}, std::nullopt, std::nullopt};
                return receiveError(control, std::move(error));
            } catch (...) {
                CodecError error{ErrorCode::InternalError,
                                 "failed to process frontend message: unknown local exception",
                                 false,
                                 {},
                                 std::nullopt,
                                 std::nullopt};
                return receiveError(control, std::move(error));
            }
        }

        ConnectionReceiveResult receiveJson(const std::shared_ptr<FrontendConnection::Control>& control, const Json& message) noexcept {
            if (!open || !control || !control->open) {
                return {ConnectionReceiveStatus::Closed, std::nullopt};
            }
            if (control->closeAfterDelivery) {
                return {ConnectionReceiveStatus::Closing, std::nullopt};
            }
            if (!consumeInboundAdmission(control)) {
                return receiveFrameError(control, ErrorCode::RateLimited, "frontend inbound message rate limit exceeded");
            }

            // Perform only transport-independent envelope checks before
            // authentication. In particular, do not reveal method existence,
            // deployment policy, or parameter diagnostics to an unauthenticated
            // peer.
            if (!message.is_object()) {
                return receiveError(
                    control,
                    CodecError{ErrorCode::InvalidField, "frontend message envelope is invalid", false, {}, std::nullopt, std::nullopt});
            }
            const auto protocol = message.find("protocol");
            const auto version = message.find("version");
            const auto messageKind = message.find("kind");
            if (protocol == message.end() || !protocol->is_string() || version == message.end() ||
                !(version->is_number_unsigned() || version->is_number_integer()) || messageKind == message.end() ||
                !messageKind->is_string()) {
                return receiveError(
                    control,
                    CodecError{ErrorCode::InvalidField, "frontend message envelope is invalid", false, {}, std::nullopt, std::nullopt});
            }
            if (protocol->get_ref<const std::string&>() != ProtocolIdentity) {
                return receiveError(
                    control,
                    CodecError{ErrorCode::WrongProtocol, "unsupported frontend protocol identity", true, {}, std::nullopt, std::nullopt});
            }
            bool supportedVersion = false;
            if (version->is_number_unsigned()) {
                supportedVersion = version->get<std::uint64_t>() == ProtocolVersion;
            } else {
                const std::int64_t signedVersion = version->get<std::int64_t>();
                supportedVersion = signedVersion >= 0 && static_cast<std::uint64_t>(signedVersion) == ProtocolVersion;
            }
            if (!supportedVersion) {
                return receiveError(control,
                                    CodecError{ErrorCode::UnsupportedVersion,
                                               "unsupported frontend protocol version",
                                               true,
                                               {SupportedProtocolVersions.begin(), SupportedProtocolVersions.end()},
                                               std::nullopt,
                                               std::nullopt});
            }

            const std::string& kindName = messageKind->get_ref<const std::string&>();
            if (kindName == kind::Hello) {
                const CodecResult<ClientMessage> decoded = Codec::decodeClient(message);
                if (!decoded) {
                    return receiveError(control, decoded.error());
                }
                return receive(control, decoded.value(), false);
            }
            if (!control->helloDone || !control->principal.has_value()) {
                return receiveError(control,
                                    CodecError{ErrorCode::AuthenticationRequired,
                                               "frontend authentication must complete before commands are accepted",
                                               true,
                                               {},
                                               std::nullopt,
                                               std::nullopt});
            }
            if (kindName != kind::Command) {
                return receiveError(
                    control,
                    CodecError{
                        ErrorCode::UnknownKind, "unknown authenticated frontend message kind", false, {}, std::nullopt, std::nullopt});
            }

            const auto method = message.find("method");
            const auto requestId = message.find("requestId");
            if (method == message.end() || !method->is_string() || requestId == message.end() || !requestId->is_string() ||
                requestId->get_ref<const std::string&>().empty()) {
                return receiveError(
                    control,
                    CodecError{ErrorCode::InvalidCommand, "frontend command envelope is invalid", false, {}, std::nullopt, std::nullopt});
            }
            const std::string& requestIdValue = requestId->get_ref<const std::string&>();
            const std::optional<generated::MethodId> methodId = generated::definedMethodFromString(method->get_ref<const std::string&>());
            if (!methodId.has_value()) {
                return receiveError(
                    control,
                    CodecError{ErrorCode::UnknownMethod, "unknown frontend command method", false, {}, requestIdValue, std::nullopt});
            }
            const generated::MethodMetadata* metadata = methodMetadata(*methodId);
            if (metadata == nullptr || !metadata->currentlyImplemented || !deploymentEnabled(*metadata)) {
                return receiveError(
                    control,
                    CodecError{
                        ErrorCode::UnknownMethod, "frontend command method is unavailable", false, {}, requestIdValue, std::nullopt});
            }

            // Full method-schema validation occurs only after exact method,
            // implementation, and deployment checks.
            const CodecResult<generated::DefinedCommand> decoded = Codec::decodeDefinedCommand(message);
            if (!decoded) {
                CodecError error = decoded.error();
                error.requestId = requestIdValue;
                return receiveError(control, std::move(error));
            }
            return receiveDefined(control, decoded.value(), false);
        }

        const generated::MethodMetadata* methodMetadata(generated::MethodId method) const noexcept {
            const std::size_t index = static_cast<std::size_t>(method);
            return index < generated::AllMethods.size() ? &generated::AllMethods[index] : nullptr;
        }

        ConnectionReceiveResult receiveDefined(const std::shared_ptr<FrontendConnection::Control>& control,
                                               const generated::DefinedCommand& command,
                                               bool consumeAdmission = true) noexcept {
            if (!open || !control || !control->open) {
                return {ConnectionReceiveStatus::Closed, std::nullopt};
            }
            if (control->closeAfterDelivery) {
                return {ConnectionReceiveStatus::Closing, std::nullopt};
            }
            if (consumeAdmission && !consumeInboundAdmission(control)) {
                return receiveFrameError(control, ErrorCode::RateLimited, "frontend inbound message rate limit exceeded");
            }
            if (!control->helloDone || !control->principal.has_value()) {
                return receiveError(control,
                                    CodecError{ErrorCode::AuthenticationRequired,
                                               "frontend authentication must complete before commands are accepted",
                                               true,
                                               {},
                                               std::nullopt,
                                               std::nullopt});
            }

            try {
                const generated::MethodId method = generated::commandMethod(command.parameters);
                const generated::MethodMetadata* metadata = methodMetadata(method);
                if (metadata == nullptr || !metadata->currentlyImplemented || !deploymentEnabled(*metadata)) {
                    enqueueFailure(control, command.requestId, ErrorCode::UnknownMethod, "frontend command method is unavailable");
                    return {ConnectionReceiveStatus::Rejected, std::nullopt};
                }
                const CodecResult<Json> validatedCommand = Codec::encodeDefinedCommand(command);
                if (!validatedCommand) {
                    CodecError error = validatedCommand.error();
                    error.requestId = command.requestId;
                    return receiveError(control, std::move(error));
                }
                bool controllerRequired = metadata->controllerRequired;
                bool scopeAllowed = hasRequiredScopes(*control->principal, metadata->requiredScopes);
                if (method == generated::MethodId::AccountRead) {
                    const Json& params = validatedCommand.value().at("params");
                    const bool refreshToken = params.contains("refreshToken") && params.at("refreshToken").get<bool>();
                    if (refreshToken) {
                        scopeAllowed = hasScope(*control->principal, FrontendScope::Control) &&
                                       hasScope(*control->principal, FrontendScope::AccountManagement);
                        controllerRequired = true;
                    }
                }
                if (!conditionalInvocationAllowed(*metadata, *control->principal, validatedCommand.value().at("params"))) {
                    enqueueFailure(
                        control, command.requestId, ErrorCode::PermissionDenied, "frontend deployment policy denied the command");
                    return {ConnectionReceiveStatus::Rejected, std::nullopt};
                }
                if (!scopeAllowed) {
                    enqueueFailure(control, command.requestId, ErrorCode::PermissionDenied, "frontend principal lacks a required scope");
                    return {ConnectionReceiveStatus::Rejected, std::nullopt};
                }
                if (controllerRequired &&
                    (!control->backendSession.has_value() || control->backendSession->role() != backend::SessionRole::Controller)) {
                    enqueueFailure(control, command.requestId, ErrorCode::PermissionDenied, "the current controller is required");
                    return {ConnectionReceiveStatus::Rejected, std::nullopt};
                }
                if (metadata->providerReadyRequired && !backendCore->isReady()) {
                    enqueueFailure(control, command.requestId, ErrorCode::BackendUnavailable, "the Codex App Server is not ready");
                    return {ConnectionReceiveStatus::Rejected, std::nullopt};
                }
                if (control->pendingRequests.contains(command.requestId)) {
                    enqueueFailure(
                        control, command.requestId, ErrorCode::DuplicateRequestId, "requestId is already pending in this frontend session");
                    return {ConnectionReceiveStatus::Rejected, std::nullopt};
                }
                if (serviceOptions.maxOutstandingCommandsPerConnection == 0 ||
                    control->pendingRequests.size() >= serviceOptions.maxOutstandingCommandsPerConnection) {
                    enqueueFailure(
                        control, command.requestId, ErrorCode::CapacityExceeded, "frontend outstanding command capacity exceeded");
                    return {ConnectionReceiveStatus::Rejected, std::nullopt};
                }

                detail::DefinedCommandMapping mapping = detail::mapDefinedCommand(command);
                if (const auto* error = std::get_if<detail::BackendCommandMappingError>(&mapping)) {
                    (void) error;
                    enqueueFailure(
                        control, command.requestId, ErrorCode::InternalError, "generated frontend command mapping is inconsistent");
                    return {ConnectionReceiveStatus::Rejected, std::nullopt};
                }
                control->pendingRequests.emplace(command.requestId, method);

                const auto completeServiceAction = [this, control, requestId = command.requestId](Json result) {
                    control->pendingRequests.erase(requestId);
                    enqueue(control, ServerMessage{Response::success(requestId, std::move(result))});
                };

                if (const auto* native = std::get_if<detail::NativeCommandMapping>(&mapping)) {
                    switch (native->action) {
                        case detail::NativeServiceAction::SnapshotGet:
                            flushNow();
                            completeServiceAction(Json{{"sequence", journal.currentSequence().value()}});
                            enqueueSnapshot(control);
                            enqueue(control, ServerMessage{SyncComplete{journal.currentSequence(), Json::object()}});
                            return {ConnectionReceiveStatus::Accepted, std::nullopt};
                        case detail::NativeServiceAction::EventsReplay: {
                            flushNow();
                            const detail::CanonicalEventJournalAccess::ReplayResult replayResult =
                                detail::CanonicalEventJournalAccess::replayAfter(journal, SequenceNumber{native->replayAfter.value_or(0)});
                            if (replayResult.status == JournalReplayStatus::FutureSequence) {
                                control->pendingRequests.erase(command.requestId);
                                enqueueFailure(control,
                                               command.requestId,
                                               ErrorCode::InvalidCommand,
                                               "events.replay cannot start after the current sequence");
                                return {ConnectionReceiveStatus::Rejected, std::nullopt};
                            }
                            SyncMode mode = SyncMode::Snapshot;
                            UpdateBatchResult built;
                            if (replayResult.status == JournalReplayStatus::Available) {
                                built =
                                    buildProjectedBatches(control, replayResult.records, SequenceNumber{native->replayAfter.value_or(0)});
                                if (built.success()) {
                                    mode = SyncMode::Replay;
                                }
                            }
                            completeServiceAction(Json{{"syncMode", toString(mode)}, {"sequence", journal.currentSequence().value()}});
                            if (mode == SyncMode::Replay) {
                                for (const BoundedEventBatch& batch : built.batches) {
                                    enqueue(control, ServerMessage{batch.batch});
                                }
                            } else {
                                enqueueSnapshot(control);
                            }
                            enqueue(control, ServerMessage{SyncComplete{journal.currentSequence(), Json::object()}});
                            return {ConnectionReceiveStatus::Accepted, std::nullopt};
                        }
                        case detail::NativeServiceAction::ControllerAcquire:
                            mapping = backend::BackendCommand{backend::ControllerAcquire{}};
                            break;
                        case detail::NativeServiceAction::ControllerRelease:
                            mapping = backend::BackendCommand{backend::ControllerRelease{}};
                            break;
                        case detail::NativeServiceAction::ProviderStart:
                        case detail::NativeServiceAction::ProviderStop:
                        case detail::NativeServiceAction::ProviderRestart: {
                            const detail::NativeServiceAction action = native->action;
                            const backend::ProviderSnapshot provider = backendCore->snapshot().provider;
                            refreshLifecycleAction(provider);
                            if (lifecycleActionInFlight.has_value()) {
                                control->pendingRequests.erase(command.requestId);
                                enqueueFailure(control,
                                               command.requestId,
                                               ErrorCode::Conflict,
                                               "another provider lifecycle action is still in progress");
                                return {ConnectionReceiveStatus::Rejected, std::nullopt};
                            }
                            const bool validState =
                                action == detail::NativeServiceAction::ProviderStart
                                    ? provider.lifecycle == backend::ProviderLifecycle::Stopped && !provider.desiredRunning
                                : action == detail::NativeServiceAction::ProviderStop
                                    ? provider.lifecycle != backend::ProviderLifecycle::Stopped &&
                                          provider.lifecycle != backend::ProviderLifecycle::Stopping
                                    : provider.lifecycle != backend::ProviderLifecycle::Stopping;
                            if (!validState) {
                                control->pendingRequests.erase(command.requestId);
                                enqueueFailure(control,
                                               command.requestId,
                                               ErrorCode::Conflict,
                                               "provider lifecycle action is not valid in the current state");
                                return {ConnectionReceiveStatus::Rejected, std::nullopt};
                            }
                            lifecycleActionInFlight = action;
                            try {
                                switch (action) {
                                    case detail::NativeServiceAction::ProviderStart:
                                        backendCore->start();
                                        break;
                                    case detail::NativeServiceAction::ProviderStop:
                                        backendCore->stop();
                                        break;
                                    case detail::NativeServiceAction::ProviderRestart:
                                        backendCore->restart();
                                        break;
                                    default:
                                        break;
                                }
                            } catch (...) {
                                lifecycleActionInFlight.reset();
                                control->pendingRequests.erase(command.requestId);
                                enqueueFailure(
                                    control, command.requestId, ErrorCode::InternalError, "provider lifecycle action failed locally");
                                return {ConnectionReceiveStatus::Rejected, std::nullopt};
                            }

                            // BackendCore accepts the action synchronously so
                            // transport closure cannot cancel provider state.
                            // Only its correlated response remains callback-last.
                            const std::weak_ptr<Impl> weakSelf = shared_from_this();
                            const std::weak_ptr<FrontendConnection::Control> weakControl = control;
                            const std::string requestId = command.requestId;
                            schedule([weakSelf, weakControl, requestId] {
                                const auto self = weakSelf.lock();
                                const auto locked = weakControl.lock();
                                if (!self || !locked || !locked->open || !locked->pendingRequests.erase(requestId)) {
                                    return;
                                }
                                self->enqueue(locked, ServerMessage{Response::success(requestId, Json::object())});
                            });
                            return {ConnectionReceiveStatus::Accepted, std::nullopt};
                        }
                    }
                }

                if (!control->backendSession.has_value() || !control->backendSession->isOpen()) {
                    control->pendingRequests.erase(command.requestId);
                    enqueueFailure(control, command.requestId, ErrorCode::BackendUnavailable, "backend frontend session is closed");
                    return {ConnectionReceiveStatus::Rejected, std::nullopt};
                }
                backend::CommandSubmission submission =
                    control->backendSession->submit(command.requestId, std::get<backend::BackendCommand>(mapping));
                if (!submission) {
                    control->pendingRequests.erase(command.requestId);
                    ErrorCode code = ErrorCode::LocalSubmissionFailure;
                    switch (submission.error) {
                        case backend::SubmissionError::None:
                        case backend::SubmissionError::Closed:
                            code = ErrorCode::BackendUnavailable;
                            break;
                        case backend::SubmissionError::EmptyRequestId:
                            code = ErrorCode::InvalidCommand;
                            break;
                        case backend::SubmissionError::DuplicateRequestId:
                            code = ErrorCode::DuplicateRequestId;
                            break;
                        case backend::SubmissionError::QueueFull:
                            code = ErrorCode::CapacityExceeded;
                            break;
                    }
                    enqueueFailure(control, command.requestId, code, "frontend command submission failed");
                    return {ConnectionReceiveStatus::Rejected, std::nullopt};
                }
                return {ConnectionReceiveStatus::Accepted, std::nullopt};
            } catch (...) {
                control->pendingRequests.erase(command.requestId);
                enqueueFailure(control, command.requestId, ErrorCode::InternalError, "failed to dispatch frontend command");
                return {ConnectionReceiveStatus::Rejected, std::nullopt};
            }
        }

        Json legacyCommandValueJson(const backend::CommandValue& value) const {
            const backend::Snapshot currentSnapshot = backendCore->snapshot();
            return std::visit(
                Overloaded{[](const std::monostate&) {
                               return Json::object();
                           },
                           [this](const backend::Snapshot& snapshot) {
                               return backendSnapshotJson(snapshot,
                                                          journal.currentSequence(),
                                                          journal.oldestReplayableAfter(),
                                                          journal.oldestRetainedSequence(),
                                                          journal.newestRetainedSequence());
                           },
                           [](const backend::ControllerResult& result) {
                               Json encoded{{"role", backendRoleName(result.role)}};
                               if (result.controller.has_value()) {
                                   encoded["controllerSessionId"] = std::to_string(result.controller->value());
                               }
                               return encoded;
                           },
                           [&currentSnapshot](const typed::ThreadStartResponse& response) {
                               const typed::Thread& thread = response.thread;
                               const backend::ThreadSnapshot* found = findThread(currentSnapshot, thread.id.value);
                               return found != nullptr ? Json{{"thread", threadSnapshotJson(*found)}} : Json{{"threadId", thread.id.value}};
                           },
                           [&currentSnapshot](const typed::ThreadResumeResponse& response) {
                               const typed::Thread& thread = response.thread;
                               const backend::ThreadSnapshot* found = findThread(currentSnapshot, thread.id.value);
                               return found != nullptr ? Json{{"thread", threadSnapshotJson(*found)}} : Json{{"threadId", thread.id.value}};
                           },
                           [&currentSnapshot](const typed::ThreadReadResponse& response) {
                               const typed::Thread& thread = response.thread;
                               const backend::ThreadSnapshot* found = findThread(currentSnapshot, thread.id.value);
                               return found != nullptr ? Json{{"thread", threadSnapshotJson(*found)}} : Json{{"threadId", thread.id.value}};
                           },
                           [&currentSnapshot](const typed::ThreadListResponse& page) {
                               Json encoded{{"threads", Json::array()}};
                               for (const typed::Thread& thread : page.data) {
                                   const backend::ThreadSnapshot* found = findThread(currentSnapshot, thread.id.value);
                                   encoded["threads"].push_back(found != nullptr ? threadSnapshotJson(*found)
                                                                                 : Json{{"id", thread.id.value}});
                               }
                               if (page.nextCursor.has_value()) {
                                   encoded["nextCursor"] = *page.nextCursor;
                               }
                               if (page.backwardsCursor.has_value()) {
                                   encoded["backwardsCursor"] = *page.backwardsCursor;
                               }
                               return encoded;
                           },
                           [&currentSnapshot](const typed::TurnStartResponse& response) {
                               const typed::Turn& turn = response.turn;
                               const backend::TurnSnapshot* found = findTurn(currentSnapshot, turn.threadId.value, turn.id.value);
                               return found != nullptr ? Json{{"turn", turnSnapshotJson(*found)}} : Json{{"turnId", turn.id.value}};
                           },
                           [](const typed::Unit&) {
                               return Json::object();
                           },
                           []<ProviderOperationResult Result>(const Result&) -> Json {
                               throw std::logic_error("provider operation result is not exposed by Frontend Protocol v1");
                           }},
                value);
        }

        Json commandValueJson(generated::MethodId method, const backend::CommandValue& value) const {
            const detail::ProviderResultProjection projection =
                detail::projectProviderResult(method, value, serviceOptions.maxOutboundBytesPerConnection);
            switch (projection.status) {
                case detail::ProviderResultProjectionStatus::Success:
                    return projection.value;
                case detail::ProviderResultProjectionStatus::LegacyProjectionRequired:
                case detail::ProviderResultProjectionStatus::NotProviderResult:
                    return legacyCommandValueJson(value);
                case detail::ProviderResultProjectionStatus::ResultTypeMismatch:
                case detail::ProviderResultProjectionStatus::InvalidResult:
                    throw std::logic_error("backend result does not satisfy its generated frontend result contract");
                case detail::ProviderResultProjectionStatus::ResultTooLarge:
                    throw ResultCapacityFailure{};
            }
            throw std::logic_error("unknown provider result projection status");
        }

        void onCommandCompleted(const std::shared_ptr<FrontendConnection::Control>& control,
                                const backend::CommandCompletion& completion) noexcept {
            const auto pending = control->pendingRequests.find(completion.requestId);
            if (pending == control->pendingRequests.end()) {
                return;
            }
            const generated::MethodId method = pending->second;
            control->pendingRequests.erase(pending);
            try {
                if (completion.result.error.has_value()) {
                    const backend::CommandError& error = *completion.result.error;
                    enqueueFailure(
                        control, completion.requestId, frontendErrorCode(error.code), std::string(frontendCommandErrorMessage(error.code)));
                } else {
                    enqueue(control,
                            ServerMessage{Response::success(completion.requestId, commandValueJson(method, completion.result.value))});
                }
            } catch (const ResultCapacityFailure&) {
                enqueueFailure(control,
                               completion.requestId,
                               ErrorCode::CapacityExceeded,
                               "frontend command result exceeds the configured outbound capacity");
            } catch (...) {
                enqueueFailure(control, completion.requestId, ErrorCode::InternalError, "failed to normalize backend command completion");
            }
        }

        CoalescerMarkResult
        markNormalized(CoalescingKey key, std::string type, Json data, FlushUrgency urgency = FlushUrgency::Deferred) noexcept {
            return coalescer.mark(DirtyUpdate{std::move(key), std::move(type), std::move(data), urgency});
        }

        Json lifecycleEventData(const backend::Snapshot& snapshot) const {
            Json data{{"lifecycle", backendLifecycleName(snapshot.provider.lifecycle)}};
            if (snapshot.provider.lastError.has_value()) {
                data["error"] = errorSnapshotJson(*snapshot.provider.lastError);
            }
            return data;
        }

        Json threadListEventData(const backend::Snapshot& snapshot) const {
            Json data{{"hasLoadedPage", snapshot.threadList.hasLoadedPage},
                      {"complete", snapshot.threadList.complete},
                      {"pagesLoaded", snapshot.threadList.pagesLoaded}};
            if (snapshot.threadList.nextCursor.has_value()) {
                data["nextCursor"] = *snapshot.threadList.nextCursor;
            }
            if (snapshot.threadList.backwardsCursor.has_value()) {
                data["backwardsCursor"] = *snapshot.threadList.backwardsCursor;
            }
            return data;
        }

        CoalescerMarkResult
        markThread(const backend::Snapshot& snapshot, std::string_view threadId, FlushUrgency urgency = FlushUrgency::Deferred) noexcept {
            const backend::ThreadSnapshot* thread = findThread(snapshot, threadId);
            if (thread == nullptr) {
                return coalescer.requireSnapshot(urgency);
            }
            return markNormalized(
                CoalescingKey::thread(std::string(threadId)), "thread.updated", Json{{"thread", threadSnapshotJson(*thread)}}, urgency);
        }

        CoalescerMarkResult markTurn(const backend::Snapshot& snapshot,
                                     std::string_view threadId,
                                     std::string_view turnId,
                                     FlushUrgency urgency = FlushUrgency::Deferred) noexcept {
            const backend::TurnSnapshot* turn = findTurn(snapshot, threadId, turnId);
            if (turn == nullptr) {
                return coalescer.requireSnapshot(urgency);
            }
            return markNormalized(CoalescingKey::turn(std::string(threadId), std::string(turnId)),
                                  "turn.updated",
                                  Json{{"turn", turnSnapshotJson(*turn)}},
                                  urgency);
        }

        CoalescerMarkResult markItem(const backend::Snapshot& snapshot,
                                     std::string_view threadId,
                                     std::string_view turnId,
                                     std::string_view itemId,
                                     FlushUrgency urgency = FlushUrgency::Deferred) noexcept {
            const backend::ItemSnapshot* item = findItem(snapshot, threadId, turnId, itemId);
            if (item == nullptr) {
                return coalescer.requireSnapshot(urgency);
            }
            return markNormalized(CoalescingKey::item(std::string(threadId), std::string(turnId), std::string(itemId)),
                                  "item.updated",
                                  Json{{"threadId", threadId}, {"turnId", turnId}, {"item", itemSnapshotJson(*item)}},
                                  urgency);
        }

        CoalescerMarkResult markItemContent(const backend::Snapshot& snapshot, const backend::ItemContentChanged& content) noexcept {
            const backend::ItemSnapshot* item = findItem(snapshot, content.threadId.value, content.turnId.value, content.itemId.value);
            if (item == nullptr) {
                return coalescer.requireSnapshot();
            }
            std::string channel;
            std::string accumulated;
            switch (content.kind) {
                case backend::ItemContentChanged::Kind::AgentText:
                    channel = "agentText";
                    accumulated = item->agentText;
                    break;
                case backend::ItemContentChanged::Kind::ReasoningText:
                    channel = "reasoningText";
                    accumulated = item->reasoningText;
                    break;
                case backend::ItemContentChanged::Kind::ReasoningSummary:
                    channel = "reasoningSummary";
                    accumulated = item->reasoningSummary;
                    break;
                case backend::ItemContentChanged::Kind::CommandOutput:
                    channel = "commandOutput";
                    accumulated = item->commandOutput;
                    break;
            }
            return markNormalized(CoalescingKey::itemContent(content.threadId.value, content.turnId.value, content.itemId.value, channel),
                                  "item.content.updated",
                                  Json{{"threadId", content.threadId.value},
                                       {"turnId", content.turnId.value},
                                       {"itemId", content.itemId.value},
                                       {"channel", channel},
                                       {"content", accumulated},
                                       {"contentTruncated", item->contentTruncated},
                                       {"droppedContentBytes", item->droppedContentBytes}},
                                  FlushUrgency::Deferred);
        }

        bool normalizeEvent(const backend::SequencedBackendEvent& sequenced, const backend::Snapshot& snapshot) noexcept {
            CoalescerMarkResult result;
            std::visit(
                Overloaded{
                    [&](const backend::ProviderLifecycleChanged&) {
                        result = markNormalized(CoalescingKey{DirtyEntityKind::BackendLifecycle, {}, {}, {}, {}},
                                                "backend.lifecycle.changed",
                                                lifecycleEventData(snapshot),
                                                FlushUrgency::Immediate);
                    },
                    [&](const backend::ProviderConnectionInvalidated&) {
                    },
                    [&](const backend::CapacityConfigured&) {
                    },
                    [&](const backend::CapacityChanged& event) {
                        if (capacityMutationRequiresSnapshot(event)) {
                            result = coalescer.requireSnapshot(FlushUrgency::Immediate);
                        }
                    },
                    [&](const backend::DiagnosticReceived&) {
                        result = markNormalized(CoalescingKey{DirtyEntityKind::Diagnostic, {}, {}, {}, {}},
                                                "diagnostics.updated",
                                                Json{{"received", snapshot.diagnostics.received}, {"recent", snapshot.diagnostics.recent}});
                    },
                    [&](const backend::ProviderOperationCompleted&) {
                    },
                    [&](const backend::ProviderOperationStateChanged&) {
                    },
                    [&](const backend::ProviderResourceAdmissionRequested&) {
                    },
                    [&](const backend::ProviderResourceAdmissionReleased&) {
                    },
                    [&](const backend::ThreadUpserted& event) {
                        result = markThread(snapshot, event.thread.id.value);
                    },
                    [&](const backend::ThreadListUpdated& event) {
                        for (const typed::Thread& thread : event.page.data) {
                            const CoalescerMarkResult threadResult = markThread(snapshot, thread.id.value);
                            if (threadResult.immediateFlush) {
                                result = threadResult;
                            }
                        }
                        const CoalescerMarkResult listResult =
                            markNormalized(CoalescingKey{DirtyEntityKind::Thread, {}, {}, "thread-list", {}},
                                           "thread.list.updated",
                                           threadListEventData(snapshot));
                        if (listResult.status != CoalescerMarkStatus::Accepted) {
                            result = listResult;
                        }
                    },
                    [&](const backend::ThreadStatusUpdated& event) {
                        result = markThread(snapshot, event.threadId.value);
                    },
                    [&](const backend::TurnUpserted& event) {
                        const backend::TurnSnapshot* turn = findTurn(snapshot, event.turn.threadId.value, event.turn.id.value);
                        result = markTurn(snapshot,
                                          event.turn.threadId.value,
                                          event.turn.id.value,
                                          turn != nullptr && turn->terminal ? FlushUrgency::Immediate : FlushUrgency::Deferred);
                    },
                    [&](const backend::TurnCompleted& event) {
                        result = markTurn(snapshot, event.turn.threadId.value, event.turn.id.value, FlushUrgency::Immediate);
                    },
                    [&](const backend::TurnFailed& event) {
                        result = markTurn(snapshot, event.turn.threadId.value, event.turn.id.value, FlushUrgency::Immediate);
                    },
                    [&](const backend::TurnErrorUpdated& event) {
                        result = markTurn(snapshot,
                                          event.threadId.value,
                                          event.turnId.value,
                                          event.willRetry ? FlushUrgency::Deferred : FlushUrgency::Immediate);
                    },
                    [&](const backend::ItemUpserted& event) {
                        const auto id = backend::itemId(event.item);
                        if (!id.has_value()) {
                            result = coalescer.requireSnapshot(event.lifecycle == backend::ItemLifecycle::Completed ||
                                                                       event.lifecycle == backend::ItemLifecycle::Failed
                                                                   ? FlushUrgency::Immediate
                                                                   : FlushUrgency::Deferred);
                            return;
                        }
                        const bool terminal =
                            event.lifecycle == backend::ItemLifecycle::Completed || event.lifecycle == backend::ItemLifecycle::Failed;
                        result = markItem(snapshot,
                                          event.threadId.value,
                                          event.turnId.value,
                                          id->value,
                                          terminal ? FlushUrgency::Immediate : FlushUrgency::Deferred);
                    },
                    [&](const backend::ItemContentChanged& event) {
                        result = markItemContent(snapshot, event);
                    },
                    [&](const backend::FileChangeUpdated& event) {
                        result = markItem(snapshot, event.threadId.value, event.turnId.value, event.itemId.value);
                    },
                    [&](const backend::TokenUsageUpdated& event) {
                        result = markTurn(snapshot, event.threadId.value, event.turnId.value);
                    },
                    [&](const backend::ModelRerouted& event) {
                        result = markTurn(snapshot, event.threadId.value, event.turnId.value);
                    },
                    [&](const backend::PendingRequestAdded& event) {
                        const backend::PendingRequestSnapshot* pending = findPending(snapshot, event.pending.id);
                        if (pending == nullptr) {
                            result = coalescer.requireSnapshot(FlushUrgency::Immediate);
                            return;
                        }
                        result = markNormalized(CoalescingKey::pendingRequest(std::to_string(event.pending.id.value())),
                                                "request.pending",
                                                Json{{"request", pendingRequestSnapshotJson(*pending)}},
                                                FlushUrgency::Immediate);
                    },
                    [&](const backend::PendingRequestRemoved& event) {
                        result = markNormalized(CoalescingKey::pendingRequest(std::to_string(event.id.value())),
                                                "request.resolved",
                                                Json{{"pendingRequestId", std::to_string(event.id.value())}, {"reason", event.reason}},
                                                FlushUrgency::Immediate);
                    },
                    [&](const backend::ControllerChanged& event) {
                        Json data = Json::object();
                        if (event.controller.has_value()) {
                            data["controllerSessionId"] = std::to_string(event.controller->value());
                        }
                        result = markNormalized(CoalescingKey{DirtyEntityKind::Controller, {}, {}, {}, {}},
                                                "controller.changed",
                                                std::move(data),
                                                FlushUrgency::Immediate);
                    },
                    [&](const backend::SessionChanged& event) {
                        result = markNormalized(CoalescingKey{DirtyEntityKind::Session, {}, {}, std::to_string(event.id.value()), {}},
                                                "session.changed",
                                                Json{{"sessionId", std::to_string(event.id.value())},
                                                     {"connected", event.connected},
                                                     {"role", backendRoleName(event.role)}},
                                                FlushUrgency::Immediate);
                    },
                    [&](const backend::CodexExtensionReceived& event) {
                        try {
                            backend::ExtensionSnapshot extension;
                            if (event.safeProjection) {
                                extension.method = event.method;
                                extension.payload = event.payload;
                                extension.decodingError = event.decodingError;
                                extension.methodTruncated = event.methodTruncated;
                                extension.payloadTruncated = event.payloadTruncated;
                                extension.decodingErrorTruncated = event.decodingErrorTruncated;
                                extension.sensitiveFieldsRedacted = event.sensitiveFieldsRedacted;
                                extension.originalMethodBytes = event.originalMethodBytes;
                                extension.originalPayloadBytes = event.originalPayloadBytes;
                                extension.originalDecodingErrorBytes = event.originalDecodingErrorBytes;
                            } else {
                                extension = backend::makeExtensionSnapshot(backend::ExtensionRecord{
                                    event.method, event.payload, event.decodingError, std::nullopt, std::nullopt, std::nullopt});
                            }
                            Json data = extensionSnapshotJson(extension);
                            result = markNormalized(
                                CoalescingKey{
                                    DirtyEntityKind::CodexExtension, {}, {}, std::to_string(sequenced.sequence.value()), extension.method},
                                "codex.extension",
                                std::move(data));
                        } catch (...) {
                            // Snapshot synchronization still exposes the
                            // reducer-retained sanitized extension record.
                            result = coalescer.requireSnapshot(FlushUrgency::Immediate);
                        }
                    }},
                sequenced.event);
            return result.immediateFlush || result.status == CoalescerMarkStatus::SnapshotRequired ||
                   result.status == CoalescerMarkStatus::AllocationFailure;
        }

        void onBackendEvents(const std::vector<backend::SequencedBackendEvent>& events) noexcept {
            if (!open || events.empty()) {
                return;
            }
            const bool requiresSnapshot = std::any_of(events.begin(), events.end(), [](const auto& event) {
                if (const auto* capacity = std::get_if<backend::CapacityChanged>(&event.event)) {
                    return capacityMutationRequiresSnapshot(*capacity);
                }
                return !std::holds_alternative<backend::ProviderConnectionInvalidated>(event.event) &&
                       !std::holds_alternative<backend::CapacityConfigured>(event.event);
            });
            if (!requiresSnapshot) {
                return;
            }
            try {
                const backend::Snapshot snapshot = backendCore->snapshot();
                refreshLifecycleAction(snapshot.provider);
                bool immediate = false;
                for (const backend::SequencedBackendEvent& event : events) {
                    immediate = normalizeEvent(event, snapshot) || immediate;
                }
                if (immediate) {
                    flushNow();
                } else {
                    scheduleFlush();
                }
            } catch (...) {
                (void) coalescer.requireSnapshot(FlushUrgency::Immediate);
                flushNow();
            }
        }

        void onBackendResynchronize(const backend::Snapshot& snapshot) noexcept {
            if (!open) {
                return;
            }
            refreshLifecycleAction(snapshot.provider);
            coalescer.clear();
            if (!journal.invalidateReplay()) {
                sequenceExhausted = true;
            }
            std::vector<std::shared_ptr<FrontendConnection::Control>> recipients;
            try {
                recipients.reserve(connections.size());
                for (const auto& [id, control] : connections) {
                    (void) id;
                    if (control->open && control->helloDone) {
                        recipients.push_back(control);
                    }
                }
            } catch (...) {
                return;
            }
            for (const auto& control : recipients) {
                enqueueSnapshot(control, &snapshot);
            }
        }

        void scheduleFlush() noexcept {
            if (!open || flushCallbackPending || !coalescer.flushScheduled()) {
                return;
            }
            flushCallbackPending = true;
            const std::weak_ptr<Impl> weakSelf = shared_from_this();
            schedule([weakSelf]() {
                if (const auto self = weakSelf.lock()) {
                    self->flushCallbackPending = false;
                    self->flushNow();
                }
            });
        }

        void broadcast(const ServerMessage& message) noexcept {
            std::vector<std::shared_ptr<FrontendConnection::Control>> recipients;
            try {
                recipients.reserve(connections.size());
                for (const auto& [id, control] : connections) {
                    (void) id;
                    if (control->open && control->helloDone) {
                        recipients.push_back(control);
                    }
                }
            } catch (...) {
                return;
            }
            for (const auto& control : recipients) {
                enqueue(control, message);
            }
        }

        void broadcastCanonicalEvents(const CanonicalEventRecords& events) noexcept {
            std::vector<std::shared_ptr<FrontendConnection::Control>> recipients;
            try {
                recipients.reserve(connections.size());
                for (const auto& [id, control] : connections) {
                    (void) id;
                    if (control->open && control->helloDone) {
                        recipients.push_back(control);
                    }
                }
            } catch (...) {
                return;
            }
            for (const auto& control : recipients) {
                if (!enqueueBatches(control, events)) {
                    closeControl(control, "failed to project or enqueue canonical frontend events");
                }
            }
        }

        void broadcastSnapshot() noexcept {
            try {
                const backend::Snapshot backendSnapshot = backendCore->snapshot();
                std::vector<std::shared_ptr<FrontendConnection::Control>> recipients;
                recipients.reserve(connections.size());
                for (const auto& [id, control] : connections) {
                    (void) id;
                    if (control->open && control->helloDone) {
                        recipients.push_back(control);
                    }
                }
                for (const auto& control : recipients) {
                    enqueueSnapshot(control, &backendSnapshot);
                }
            } catch (...) {
            }
        }

        void flushNow() noexcept {
            if (!open) {
                return;
            }
            if (flushing) {
                flushAgain = true;
                return;
            }
            flushing = true;
            do {
                flushAgain = false;
                try {
                    CoalescerDrainResult dirty = coalescer.drain();
                    if (dirty.snapshotRequired || sequenceExhausted) {
                        if (!journal.invalidateReplay()) {
                            sequenceExhausted = true;
                        }
                        broadcastSnapshot();
                        continue;
                    }

                    CanonicalEventRecords events;
                    events.reserve(dirty.updates.size());
                    bool snapshotFallback = false;
                    const backend::Snapshot backendSnapshot = backendCore->snapshot();
                    for (DirtyUpdate& update : dirty.updates) {
                        auto record = std::make_shared<detail::CanonicalEventRecord>(
                            detail::makeCanonicalEventRecord(std::move(update.type), std::move(update.data), backendSnapshot));
                        const auto retainedBytes = detail::canonicalEventRetainedBytes(*record);
                        if (!retainedBytes.has_value()) {
                            if (!journal.invalidateReplay()) {
                                sequenceExhausted = true;
                            }
                            snapshotFallback = true;
                            break;
                        }
                        const detail::CanonicalEventJournalAccess::AppendResult appended =
                            detail::CanonicalEventJournalAccess::append(journal, record, *retainedBytes);
                        if (appended.status == JournalAppendStatus::SequenceOverflow) {
                            sequenceExhausted = true;
                            (void) journal.invalidateReplay();
                            ProtocolErrorMessage error;
                            error.code = ErrorCode::SequenceOverflow;
                            error.message = "frontend event sequence is exhausted";
                            error.supportedVersions.assign(SupportedProtocolVersions.begin(), SupportedProtocolVersions.end());
                            broadcast(ServerMessage{std::move(error)});
                            snapshotFallback = true;
                            break;
                        }
                        if (!appended.accepted() || !appended.sequence.has_value() || record->sequence != appended.sequence) {
                            if (!journal.invalidateReplay()) {
                                sequenceExhausted = true;
                            }
                            snapshotFallback = true;
                            break;
                        }
                        events.push_back(std::move(record));
                    }
                    if (snapshotFallback) {
                        broadcastSnapshot();
                        continue;
                    }
                    if (events.empty()) {
                        continue;
                    }

                    broadcastCanonicalEvents(events);
                } catch (...) {
                    coalescer.clear();
                    if (!journal.invalidateReplay()) {
                        sequenceExhausted = true;
                    }
                    broadcastSnapshot();
                }
            } while (flushAgain || coalescer.flushScheduled());
            flushing = false;
        }

        void shutdown(std::string reason) noexcept {
            if (!open) {
                return;
            }
            open = false;
            observer.close();
            coalescer.clear();

            std::vector<std::shared_ptr<FrontendConnection::Control>> active;
            try {
                active.reserve(connections.size());
                for (const auto& [id, control] : connections) {
                    (void) id;
                    active.push_back(control);
                }
            } catch (...) {
            }
            for (const auto& control : active) {
                closeControl(control, reason);
            }
            connections.clear();
            for (auto& [key, window] : failedAuthentications) {
                (void) key;
                cancelTimer(window.expiration);
            }
            failedAuthentications.clear();
        }

        bool isOpen() const noexcept {
            return open;
        }

        bool isFlushScheduled() const noexcept {
            return flushCallbackPending || coalescer.flushScheduled();
        }

        std::size_t connectionCount() const noexcept {
            return connections.size();
        }

        backend::detail::BackendCoreRuntime* backendCore;
        FrontendServiceOptions serviceOptions;
        EventJournal journal;
        UpdateBatchBuilder batchBuilder;
        EventCoalescer coalescer;
        backend::BackendObserverSubscription observer;
        std::map<std::uint64_t, std::shared_ptr<FrontendConnection::Control>> connections;
        std::map<std::string, FailedAuthenticationWindow> failedAuthentications;
        std::map<FrontendTransportKind, std::size_t> declaredTransportCounts;
        std::uint64_t nextConnectionId = 0;
        std::uint64_t nextFailureWindowGeneration = 0;
        std::size_t unauthenticatedConnections = 0;
        std::size_t authenticatedConnections = 0;
        bool open = true;
        bool flushCallbackPending = false;
        bool flushing = false;
        bool flushAgain = false;
        bool sequenceExhausted = false;
        std::optional<detail::NativeServiceAction> lifecycleActionInFlight;
    };

    FrontendConnection::FrontendConnection() noexcept = default;

    FrontendConnection::FrontendConnection(std::shared_ptr<Control> control) noexcept
        : control(std::move(control)) {
    }

    FrontendConnection::FrontendConnection(FrontendConnection&& other) noexcept
        : control(std::move(other.control)) {
    }

    FrontendConnection& FrontendConnection::operator=(FrontendConnection&& other) noexcept {
        if (this != &other) {
            close();
            control = std::move(other.control);
        }
        return *this;
    }

    FrontendConnection::~FrontendConnection() {
        close();
    }

    ConnectionReceiveResult FrontendConnection::receive(const ClientMessage& message) noexcept {
        if (!control || !control->open) {
            return {ConnectionReceiveStatus::Closed, std::nullopt};
        }
        if (control->closeAfterDelivery) {
            return {ConnectionReceiveStatus::Closing, std::nullopt};
        }
        const auto service = control->service.lock();
        if (!service) {
            control->open = false;
            return {ConnectionReceiveStatus::Closed, std::nullopt};
        }
        const CodecResult<Json> encoded = Codec::encodeClient(message);
        if (!encoded) {
            return receiveError(encoded.error());
        }
        try {
            if (!service->inboundFrameAdmissible(encoded.value().dump().size())) {
                return service->receiveFrameError(control, ErrorCode::FrameTooLarge, "frontend inbound message exceeds frame capacity");
            }
        } catch (...) {
            return service->receiveFrameError(control, ErrorCode::InvalidField, "frontend inbound message could not be measured");
        }
        return service->receive(control, message);
    }

    ConnectionReceiveResult FrontendConnection::receive(const Json& message) noexcept {
        if (!control || !control->open) {
            return {ConnectionReceiveStatus::Closed, std::nullopt};
        }
        if (control->closeAfterDelivery) {
            return {ConnectionReceiveStatus::Closing, std::nullopt};
        }
        const auto service = control->service.lock();
        if (!service) {
            control->open = false;
            return {ConnectionReceiveStatus::Closed, std::nullopt};
        }
        try {
            if (!service->inboundFrameAdmissible(message.dump().size())) {
                return service->receiveFrameError(control, ErrorCode::FrameTooLarge, "frontend inbound message exceeds frame capacity");
            }
        } catch (...) {
            return service->receiveFrameError(control, ErrorCode::MalformedJson, "frontend inbound JSON could not be measured");
        }
        return service->receiveJson(control, message);
    }

    ConnectionReceiveResult FrontendConnection::receive(std::string_view compactJson) noexcept {
        if (!control || !control->open) {
            return {ConnectionReceiveStatus::Closed, std::nullopt};
        }
        if (control->closeAfterDelivery) {
            return {ConnectionReceiveStatus::Closing, std::nullopt};
        }
        const auto service = control->service.lock();
        if (!service) {
            control->open = false;
            return {ConnectionReceiveStatus::Closed, std::nullopt};
        }
        if (!service->inboundFrameAdmissible(compactJson.size())) {
            return service->receiveFrameError(control, ErrorCode::FrameTooLarge, "frontend inbound message exceeds frame capacity");
        }
        const Json parsed = Json::parse(compactJson.begin(), compactJson.end(), nullptr, false);
        if (parsed.is_discarded()) {
            return receiveError(CodecError{ErrorCode::MalformedJson, "message is not valid JSON", false, {}, std::nullopt, std::nullopt});
        }
        return service->receiveJson(control, parsed);
    }

    ConnectionReceiveResult FrontendConnection::receiveError(CodecError error) noexcept {
        if (!control || !control->open) {
            return {ConnectionReceiveStatus::Closed, std::move(error)};
        }
        if (control->closeAfterDelivery) {
            return {ConnectionReceiveStatus::Closing, std::move(error)};
        }
        const auto service = control->service.lock();
        if (!service) {
            control->open = false;
            return {ConnectionReceiveStatus::Closed, std::move(error)};
        }
        return service->receiveError(control, std::move(error));
    }

    bool FrontendConnection::updatePeerContext(FrontendPeerContext peer) noexcept {
        if (!control || !control->open || control->helloDone || control->authenticationAttempted) {
            return false;
        }
        control->peer = std::move(peer);
        return true;
    }

    void FrontendConnection::close(std::string reason) noexcept {
        if (!control) {
            return;
        }
        if (const auto service = control->service.lock()) {
            service->closeControl(control, std::move(reason));
        } else {
            control->open = false;
            control->outbound.clear();
            control->outboundBytes = 0;
            control->backendSession.reset();
        }
        control.reset();
    }

    bool FrontendConnection::isOpen() const noexcept {
        return control && control->open;
    }

    bool FrontendConnection::helloComplete() const noexcept {
        return control && control->open && control->helloDone;
    }

    std::optional<std::string> FrontendConnection::sessionId() const {
        if (!control || !control->open || !control->backendSession.has_value()) {
            return std::nullopt;
        }
        return std::to_string(control->backendSession->id().value());
    }

    std::optional<FrontendPrincipal> FrontendConnection::principal() const {
        return control && control->open ? control->principal : std::nullopt;
    }

    FrontendPeerContext FrontendConnection::peer() const {
        return control ? control->peer : FrontendPeerContext{};
    }

    std::size_t FrontendConnection::queuedMessages() const noexcept {
        return control ? control->outbound.size() : 0;
    }

    std::size_t FrontendConnection::queuedBytes() const noexcept {
        return control ? control->outboundBytes : 0;
    }

    FrontendService::FrontendService(backend::detail::BackendCoreRuntime& backend, FrontendServiceOptions options)
        : impl(std::make_shared<Impl>(backend, std::move(options))) {
        impl->initialize();
    }

    FrontendService::~FrontendService() {
        close();
    }

    FrontendConnection FrontendService::openConnection(FrontendPeerContext peer, FrontendConnectionCallbacks callbacks) {
        return impl ? impl->openConnection(std::move(peer), std::move(callbacks)) : FrontendConnection{};
    }

    AuthenticationFailureCode FrontendService::recordPreAuthenticationFailure(const FrontendPeerContext& peer,
                                                                              AuthenticationFailureCode failure) noexcept {
        return impl ? impl->recordPreAuthenticationFailure(peer, failure) : AuthenticationFailureCode::RateLimited;
    }

    void FrontendService::declareTransportFamily(FrontendTransportKind transport) {
        if (impl) {
            impl->declareTransportFamily(transport);
        }
    }

    void FrontendService::withdrawTransportFamily(FrontendTransportKind transport) noexcept {
        if (impl) {
            impl->withdrawTransportFamily(transport);
        }
    }

    void FrontendService::flush() {
        if (impl) {
            impl->flushNow();
        }
    }

    void FrontendService::close(std::string reason) noexcept {
        if (impl) {
            impl->shutdown(std::move(reason));
            impl.reset();
        }
    }

    bool FrontendService::isOpen() const noexcept {
        return impl && impl->isOpen();
    }

    bool FrontendService::flushScheduled() const noexcept {
        return impl && impl->isFlushScheduled();
    }

    SequenceNumber FrontendService::currentSequence() const noexcept {
        return impl ? impl->journal.currentSequence() : SequenceNumber{};
    }

    std::size_t FrontendService::connectionCount() const noexcept {
        return impl ? impl->connectionCount() : 0;
    }

    std::size_t FrontendService::unauthenticatedConnectionCount() const noexcept {
        return impl ? impl->unauthenticatedConnectionCount() : 0;
    }

    std::size_t FrontendService::authenticatedConnectionCount() const noexcept {
        return impl ? impl->authenticatedConnectionCount() : 0;
    }

    std::optional<std::string> FrontendService::currentController() const {
        if (!impl) {
            return std::nullopt;
        }
        const std::optional<backend::SessionId> controller = impl->backendCore->snapshot().controller;
        return controller ? std::optional<std::string>{std::to_string(controller->value())} : std::nullopt;
    }

    std::vector<FrontendMethod> FrontendService::definedMethods() const {
        return impl ? impl->definedMethods() : std::vector<FrontendMethod>{};
    }

    std::vector<FrontendMethod> FrontendService::implementedMethods() const {
        return impl ? impl->implementedMethods() : std::vector<FrontendMethod>{};
    }

    std::vector<FrontendMethod> FrontendService::availableMethods() const {
        return impl ? impl->availableMethods() : std::vector<FrontendMethod>{};
    }

    std::vector<FrontendMethod> FrontendService::permittedMethods(const FrontendPrincipal& principal) const {
        return impl ? impl->permittedMethods(principal) : std::vector<FrontendMethod>{};
    }

    std::vector<FrontendTransportKind> FrontendService::enabledTransportFamilies() const {
        return impl ? impl->enabledTransportFamilies() : std::vector<FrontendTransportKind>{};
    }

    std::vector<FrontendCapability> FrontendService::implementedCapabilities() const {
        return impl ? impl->implementedCapabilities() : std::vector<FrontendCapability>{};
    }

    EventJournalConfig FrontendService::journalConfig() const noexcept {
        return impl ? impl->journal.config() : EventJournalConfig{};
    }

    UpdateBatchConfig FrontendService::batchConfig() const noexcept {
        return impl ? impl->batchBuilder.config() : UpdateBatchConfig{};
    }

} // namespace ai::openai::codex::frontend
