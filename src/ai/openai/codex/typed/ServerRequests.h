/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_SERVERREQUESTS_H
#define AI_OPENAI_CODEX_TYPED_SERVERREQUESTS_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/Accounts.h"
#include "ai/openai/codex/typed/Conversation.h"
#include "ai/openai/codex/typed/Items.h"
#include "ai/openai/codex/typed/Types.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ai::openai::codex::typed {

    struct NetworkApprovalProtocol {
        std::string value;

        static NetworkApprovalProtocol http() {
            return {"http"};
        }

        static NetworkApprovalProtocol https() {
            return {"https"};
        }

        static NetworkApprovalProtocol socks5Tcp() {
            return {"socks5Tcp"};
        }

        static NetworkApprovalProtocol socks5Udp() {
            return {"socks5Udp"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "http" || value == "https" || value == "socks5Tcp" || value == "socks5Udp";
        }

        auto operator<=>(const NetworkApprovalProtocol&) const = default;
    };

    struct NetworkPolicyRuleAction {
        std::string value;

        static NetworkPolicyRuleAction allow() {
            return {"allow"};
        }

        static NetworkPolicyRuleAction deny() {
            return {"deny"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "allow" || value == "deny";
        }

        auto operator<=>(const NetworkPolicyRuleAction&) const = default;
    };

    struct NetworkPolicyAmendment {
        NetworkPolicyRuleAction action;
        std::string host;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct NetworkApprovalContext {
        std::string host;
        NetworkApprovalProtocol protocol;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct RootFileSystemSpecialPath {
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct MinimalFileSystemSpecialPath {
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ProjectRootsFileSystemSpecialPath {
        OptionalNullable<std::string> subpath;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct TmpdirFileSystemSpecialPath {
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct SlashTmpFileSystemSpecialPath {
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    // "unknown" is a pinned, known wire alternative. It is distinct from the
    // genuinely future alternative below.
    struct UnknownFileSystemSpecialPath {
        std::string path;
        OptionalNullable<std::string> subpath;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct UnrecognizedFileSystemSpecialPath {
        std::optional<std::string> kind;
        Json raw = Json::object();
        DecodeDiagnostic diagnostic;
    };

    using FileSystemSpecialPath = std::variant<RootFileSystemSpecialPath,
                                               MinimalFileSystemSpecialPath,
                                               ProjectRootsFileSystemSpecialPath,
                                               TmpdirFileSystemSpecialPath,
                                               SlashTmpFileSystemSpecialPath,
                                               UnknownFileSystemSpecialPath,
                                               UnrecognizedFileSystemSpecialPath>;

    struct PathFileSystemPath {
        std::string path;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct GlobPatternFileSystemPath {
        std::string pattern;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct SpecialFileSystemPath {
        FileSystemSpecialPath value;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct UnrecognizedFileSystemPath {
        std::optional<std::string> type;
        Json raw = Json::object();
        DecodeDiagnostic diagnostic;
    };

    using FileSystemPath = std::variant<PathFileSystemPath, GlobPatternFileSystemPath, SpecialFileSystemPath, UnrecognizedFileSystemPath>;

    struct FileSystemAccessMode {
        std::string value;

        static FileSystemAccessMode read() {
            return {"read"};
        }

        static FileSystemAccessMode write() {
            return {"write"};
        }

        static FileSystemAccessMode deny() {
            return {"deny"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "read" || value == "write" || value == "deny";
        }

        auto operator<=>(const FileSystemAccessMode&) const = default;
    };

    struct FileSystemSandboxEntry {
        FileSystemAccessMode access;
        FileSystemPath path;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct AdditionalFileSystemPermissions {
        OptionalNullable<std::vector<FileSystemSandboxEntry>> entries;
        OptionalNullable<std::uint64_t> globScanMaxDepth;
        OptionalNullable<std::vector<std::string>> read;
        OptionalNullable<std::vector<std::string>> write;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct AdditionalNetworkPermissions {
        OptionalNullable<bool> enabled;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct RequestPermissionProfile {
        OptionalNullable<AdditionalFileSystemPermissions> fileSystem;
        OptionalNullable<AdditionalNetworkPermissions> network;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct GrantedPermissionProfile {
        OptionalNullable<AdditionalFileSystemPermissions> fileSystem;
        OptionalNullable<AdditionalNetworkPermissions> network;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct PermissionGrantScope {
        std::string value;

        static PermissionGrantScope turn() {
            return {"turn"};
        }

        static PermissionGrantScope session() {
            return {"session"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "turn" || value == "session";
        }

        auto operator<=>(const PermissionGrantScope&) const = default;
    };

    struct AddFileChange {
        std::string content;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct DeleteFileChange {
        std::string content;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct UpdateFileChange {
        OptionalNullable<std::string> movePath;
        std::string unifiedDiff;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct UnrecognizedFileChange {
        std::optional<std::string> type;
        Json raw = Json::object();
        DecodeDiagnostic diagnostic;
    };

    using FileChange = std::variant<AddFileChange, DeleteFileChange, UpdateFileChange, UnrecognizedFileChange>;

    struct ReadParsedCommand {
        std::string command;
        std::string name;
        std::string path;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ListFilesParsedCommand {
        std::string command;
        OptionalNullable<std::string> path;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct SearchParsedCommand {
        std::string command;
        OptionalNullable<std::string> path;
        OptionalNullable<std::string> query;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    // "unknown" is a pinned, known ParsedCommand alternative.
    struct UnknownParsedCommand {
        std::string command;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct UnrecognizedParsedCommand {
        std::optional<std::string> type;
        Json raw = Json::object();
        DecodeDiagnostic diagnostic;
    };

    using ParsedCommand =
        std::variant<ReadParsedCommand, ListFilesParsedCommand, SearchParsedCommand, UnknownParsedCommand, UnrecognizedParsedCommand>;

    struct AcceptCommandExecutionApprovalDecision {};

    struct AcceptForSessionCommandExecutionApprovalDecision {};

    struct AcceptWithExecpolicyAmendmentCommandExecutionApprovalDecision {
        std::vector<std::string> execpolicyAmendment;
    };

    struct ApplyNetworkPolicyAmendmentCommandExecutionApprovalDecision {
        NetworkPolicyAmendment networkPolicyAmendment;
    };

    struct DeclineCommandExecutionApprovalDecision {};

    struct CancelCommandExecutionApprovalDecision {};

    struct UnrecognizedCommandExecutionApprovalDecision {
        std::optional<std::string> variant;
        Json raw = nullptr;
        DecodeDiagnostic diagnostic;
    };

    using CommandExecutionApprovalDecision = std::variant<AcceptCommandExecutionApprovalDecision,
                                                          AcceptForSessionCommandExecutionApprovalDecision,
                                                          AcceptWithExecpolicyAmendmentCommandExecutionApprovalDecision,
                                                          ApplyNetworkPolicyAmendmentCommandExecutionApprovalDecision,
                                                          DeclineCommandExecutionApprovalDecision,
                                                          CancelCommandExecutionApprovalDecision,
                                                          UnrecognizedCommandExecutionApprovalDecision>;

    struct ApprovedReviewDecision {};

    struct ApprovedExecpolicyAmendmentReviewDecision {
        std::vector<std::string> proposedExecpolicyAmendment;
    };

    struct ApprovedForSessionReviewDecision {};

    struct NetworkPolicyAmendmentReviewDecision {
        NetworkPolicyAmendment networkPolicyAmendment;
    };

    struct DeniedReviewDecision {};

    struct TimedOutReviewDecision {};

    struct AbortReviewDecision {};

    struct UnrecognizedReviewDecision {
        std::optional<std::string> variant;
        Json raw = nullptr;
        DecodeDiagnostic diagnostic;
    };

    using ReviewDecision = std::variant<ApprovedReviewDecision,
                                        ApprovedExecpolicyAmendmentReviewDecision,
                                        ApprovedForSessionReviewDecision,
                                        NetworkPolicyAmendmentReviewDecision,
                                        DeniedReviewDecision,
                                        TimedOutReviewDecision,
                                        AbortReviewDecision,
                                        UnrecognizedReviewDecision>;

    struct FileChangeApprovalDecision {
        std::string value;

        static FileChangeApprovalDecision accept() {
            return {"accept"};
        }

        static FileChangeApprovalDecision acceptForSession() {
            return {"acceptForSession"};
        }

        static FileChangeApprovalDecision decline() {
            return {"decline"};
        }

        static FileChangeApprovalDecision cancel() {
            return {"cancel"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "accept" || value == "acceptForSession" || value == "decline" || value == "cancel";
        }

        auto operator<=>(const FileChangeApprovalDecision&) const = default;
    };

    struct ApplyPatchApprovalParams {
        ResponseCallId callId;
        ThreadId conversationId;
        std::map<std::string, FileChange> fileChanges;
        OptionalNullable<std::string> grantRoot;
        OptionalNullable<std::string> reason;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ApplyPatchApprovalResponse {
        ReviewDecision decision;
    };

    struct ExecCommandApprovalParams {
        OptionalNullable<std::string> approvalId;
        ResponseCallId callId;
        std::vector<std::string> command;
        ThreadId conversationId;
        std::string cwd;
        std::vector<ParsedCommand> parsedCommand;
        OptionalNullable<std::string> reason;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ExecCommandApprovalResponse {
        ReviewDecision decision;
    };

    struct CommandExecutionRequestApprovalParams {
        OptionalNullable<std::string> approvalId;
        OptionalNullable<std::string> command;
        OptionalNullable<std::vector<CommandAction>> commandActions;
        OptionalNullable<PathString> cwd;
        OptionalNullable<std::string> environmentId;
        ItemId itemId;
        OptionalNullable<NetworkApprovalContext> networkApprovalContext;
        OptionalNullable<std::vector<std::string>> proposedExecpolicyAmendment;
        OptionalNullable<std::vector<NetworkPolicyAmendment>> proposedNetworkPolicyAmendments;
        OptionalNullable<std::string> reason;
        std::int64_t startedAtMs = 0;
        ThreadId threadId;
        TurnId turnId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct CommandExecutionRequestApprovalResponse {
        CommandExecutionApprovalDecision decision;
    };

    struct FileChangeRequestApprovalParams {
        OptionalNullable<std::string> grantRoot;
        ItemId itemId;
        OptionalNullable<std::string> reason;
        std::int64_t startedAtMs = 0;
        ThreadId threadId;
        TurnId turnId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct FileChangeRequestApprovalResponse {
        FileChangeApprovalDecision decision;
    };

    struct PermissionsRequestApprovalParams {
        AbsolutePath cwd;
        OptionalNullable<std::string> environmentId;
        ItemId itemId;
        RequestPermissionProfile permissions;
        OptionalNullable<std::string> reason;
        std::int64_t startedAtMs = 0;
        ThreadId threadId;
        TurnId turnId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct PermissionsRequestApprovalResponse {
        GrantedPermissionProfile permissions;
        // Omission uses the protocol's "turn" default; a present value is encoded
        // exactly and remains open to future values.
        std::optional<PermissionGrantScope> scope;
        OptionalNullable<bool> strictAutoReview;
    };

    struct ToolRequestUserInputOption {
        std::string description;
        std::string label;
        Json raw = Json::object();
    };

    struct ToolRequestUserInputQuestion {
        std::string header;
        std::string id;
        // These fields carry schema defaults. Keeping them optional preserves
        // omission separately from an explicit false value.
        std::optional<bool> isOther;
        std::optional<bool> isSecret;
        OptionalNullable<std::vector<ToolRequestUserInputOption>> options;
        std::string question;
        Json raw = Json::object();
    };

    struct ToolRequestUserInputParams {
        OptionalNullable<std::uint64_t> autoResolutionMs;
        ItemId itemId;
        std::vector<ToolRequestUserInputQuestion> questions;
        ThreadId threadId;
        TurnId turnId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ToolRequestUserInputAnswer {
        std::vector<std::string> answers;
        Json raw = Json::object();
    };

    struct ToolRequestUserInputResponse {
        std::map<std::string, ToolRequestUserInputAnswer> answers;
        Json raw = Json::object();
    };

    struct McpElicitationArrayType {
        std::string value;

        static McpElicitationArrayType array() {
            return {"array"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "array";
        }

        auto operator<=>(const McpElicitationArrayType&) const = default;
    };

    struct McpElicitationBooleanType {
        std::string value;

        static McpElicitationBooleanType boolean() {
            return {"boolean"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "boolean";
        }

        auto operator<=>(const McpElicitationBooleanType&) const = default;
    };

    struct McpElicitationNumberType {
        std::string value;

        static McpElicitationNumberType number() {
            return {"number"};
        }

        static McpElicitationNumberType integer() {
            return {"integer"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "number" || value == "integer";
        }

        auto operator<=>(const McpElicitationNumberType&) const = default;
    };

    struct McpElicitationObjectType {
        std::string value;

        static McpElicitationObjectType object() {
            return {"object"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "object";
        }

        auto operator<=>(const McpElicitationObjectType&) const = default;
    };

    struct McpElicitationStringType {
        std::string value;

        static McpElicitationStringType string() {
            return {"string"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "string";
        }

        auto operator<=>(const McpElicitationStringType&) const = default;
    };

    struct McpElicitationStringFormat {
        std::string value;

        static McpElicitationStringFormat email() {
            return {"email"};
        }

        static McpElicitationStringFormat uri() {
            return {"uri"};
        }

        static McpElicitationStringFormat date() {
            return {"date"};
        }

        static McpElicitationStringFormat dateTime() {
            return {"date-time"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "email" || value == "uri" || value == "date" || value == "date-time";
        }

        auto operator<=>(const McpElicitationStringFormat&) const = default;
    };

    struct McpElicitationConstOption {
        std::string constant;
        std::string title;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationBooleanSchema {
        OptionalNullable<bool> defaultValue;
        OptionalNullable<std::string> description;
        OptionalNullable<std::string> title;
        McpElicitationBooleanType type;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationLegacyTitledEnumSchema {
        OptionalNullable<std::string> defaultValue;
        OptionalNullable<std::string> description;
        std::vector<std::string> values;
        OptionalNullable<std::vector<std::string>> enumNames;
        OptionalNullable<std::string> title;
        McpElicitationStringType type;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationNumberSchema {
        OptionalNullable<double> defaultValue;
        OptionalNullable<std::string> description;
        OptionalNullable<double> maximum;
        OptionalNullable<double> minimum;
        OptionalNullable<std::string> title;
        McpElicitationNumberType type;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationStringSchema {
        OptionalNullable<std::string> defaultValue;
        OptionalNullable<std::string> description;
        OptionalNullable<McpElicitationStringFormat> format;
        OptionalNullable<std::uint32_t> maxLength;
        OptionalNullable<std::uint32_t> minLength;
        OptionalNullable<std::string> title;
        McpElicitationStringType type;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationTitledEnumItems {
        std::vector<McpElicitationConstOption> anyOf;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationUntitledEnumItems {
        std::vector<std::string> values;
        McpElicitationStringType type;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationTitledMultiSelectEnumSchema {
        OptionalNullable<std::vector<std::string>> defaultValue;
        OptionalNullable<std::string> description;
        McpElicitationTitledEnumItems items;
        OptionalNullable<std::uint64_t> maxItems;
        OptionalNullable<std::uint64_t> minItems;
        OptionalNullable<std::string> title;
        McpElicitationArrayType type;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationTitledSingleSelectEnumSchema {
        OptionalNullable<std::string> defaultValue;
        OptionalNullable<std::string> description;
        std::vector<McpElicitationConstOption> oneOf;
        OptionalNullable<std::string> title;
        McpElicitationStringType type;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationUntitledMultiSelectEnumSchema {
        OptionalNullable<std::vector<std::string>> defaultValue;
        OptionalNullable<std::string> description;
        McpElicitationUntitledEnumItems items;
        OptionalNullable<std::uint64_t> maxItems;
        OptionalNullable<std::uint64_t> minItems;
        OptionalNullable<std::string> title;
        McpElicitationArrayType type;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationUntitledSingleSelectEnumSchema {
        OptionalNullable<std::string> defaultValue;
        OptionalNullable<std::string> description;
        std::vector<std::string> values;
        OptionalNullable<std::string> title;
        McpElicitationStringType type;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    using McpElicitationSingleSelectEnumSchema =
        std::variant<McpElicitationUntitledSingleSelectEnumSchema, McpElicitationTitledSingleSelectEnumSchema>;
    using McpElicitationMultiSelectEnumSchema =
        std::variant<McpElicitationUntitledMultiSelectEnumSchema, McpElicitationTitledMultiSelectEnumSchema>;
    using McpElicitationEnumSchema = std::variant<McpElicitationUntitledSingleSelectEnumSchema,
                                                  McpElicitationTitledSingleSelectEnumSchema,
                                                  McpElicitationUntitledMultiSelectEnumSchema,
                                                  McpElicitationTitledMultiSelectEnumSchema,
                                                  McpElicitationLegacyTitledEnumSchema>;

    struct UnknownMcpElicitationPrimitiveSchema {
        std::optional<std::string> type;
        Json raw = Json::object();
        DecodeDiagnostic diagnostic;
    };

    using McpElicitationPrimitiveSchema = std::variant<McpElicitationEnumSchema,
                                                       McpElicitationStringSchema,
                                                       McpElicitationNumberSchema,
                                                       McpElicitationBooleanSchema,
                                                       UnknownMcpElicitationPrimitiveSchema>;

    struct McpElicitationSchema {
        OptionalNullable<std::string> schema;
        std::map<std::string, McpElicitationPrimitiveSchema> properties;
        OptionalNullable<std::vector<std::string>> required;
        McpElicitationObjectType type;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationForm {
        std::string message;
        McpElicitationSchema requestedSchema;
        OptionalNullable<Json> meta;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationOpenAiForm {
        std::string message;
        // This schema position deliberately accepts every JSON value,
        // including null.
        Json requestedSchema = nullptr;
        OptionalNullable<Json> meta;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpElicitationUrl {
        std::string elicitationId;
        std::string message;
        std::string url;
        OptionalNullable<Json> meta;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct UnknownMcpElicitation {
        std::string mode;
        Json raw = Json::object();
        DecodeDiagnostic diagnostic;
    };

    using McpElicitation = std::variant<McpElicitationForm, McpElicitationOpenAiForm, McpElicitationUrl, UnknownMcpElicitation>;

    struct McpServerElicitationRequestParams {
        std::string serverName;
        ThreadId threadId;
        OptionalNullable<TurnId> turnId;
        McpElicitation elicitation;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpServerElicitationAction {
        std::string value;

        static McpServerElicitationAction accept() {
            return {"accept"};
        }

        static McpServerElicitationAction decline() {
            return {"decline"};
        }

        static McpServerElicitationAction cancel() {
            return {"cancel"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "accept" || value == "decline" || value == "cancel";
        }

        auto operator<=>(const McpServerElicitationAction&) const = default;
    };

    struct McpServerElicitationRequestResponse {
        McpServerElicitationAction action;
        OptionalNullable<Json> content;
        OptionalNullable<Json> meta;
        Json raw = Json::object();
    };

    // Empty in the pinned schema but open to future properties.
    struct AttestationGenerateParams {
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct AttestationGenerateResponse {
        std::string token;
        Json raw = Json::object();
    };

    struct DynamicToolCallParams {
        // The protocol deliberately permits every JSON value, including null.
        Json arguments = nullptr;
        ResponseCallId callId;
        OptionalNullable<std::string> nameSpace;
        ThreadId threadId;
        std::string tool;
        TurnId turnId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct DynamicToolCallResponse {
        std::vector<DynamicToolCallOutputContentItem> contentItems;
        bool success = false;
        Json raw = Json::object();
    };

    struct CommandApprovalRequest {
        ServerRequestId requestId;
        ServerRequestToken requestToken;
        ThreadId threadId;
        TurnId turnId;
        ItemId itemId;
        std::int64_t startedAtMs = 0;
        std::optional<std::string> command;
        std::optional<std::string> cwd;
        std::optional<std::string> reason;
        Json details;
        Json raw;
        // Schema-complete canonical view. The compatibility projection above
        // preserves the established source-level access pattern.
        CommandExecutionRequestApprovalParams canonicalParams;
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct FileChangeApprovalRequest {
        ServerRequestId requestId;
        ServerRequestToken requestToken;
        ThreadId threadId;
        TurnId turnId;
        ItemId itemId;
        std::int64_t startedAtMs = 0;
        std::optional<std::string> reason;
        std::optional<std::string> grantRoot;
        Json raw;
        FileChangeRequestApprovalParams canonicalParams;
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct UserInputOption {
        std::string label;
        std::string description;
        Json raw;
    };

    struct UserInputQuestion {
        std::string id;
        std::string header;
        std::string prompt;
        std::vector<UserInputOption> options;
        bool allowsFreeText = false;
        bool secret = false;
        Json raw;
    };

    struct UserInputRequest {
        ServerRequestId requestId;
        ServerRequestToken requestToken;
        ThreadId threadId;
        TurnId turnId;
        ItemId itemId;
        std::vector<UserInputQuestion> questions;
        std::optional<std::uint64_t> autoResolutionMs;
        Json raw;
        ToolRequestUserInputParams canonicalParams;
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct AuthenticationRequest {
        ServerRequestId requestId;
        ServerRequestToken requestToken;
        std::string reason;
        std::optional<std::string> previousAccountId;
        Json raw;
        // Schema-complete canonical view. The legacy fields above remain as a
        // source-compatible projection; only canonicalParams distinguishes an
        // omitted previous account ID from an explicit null.
        ChatgptAuthTokensRefreshParams canonicalParams;
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct UnknownServerRequest {
        ServerRequestId requestId;
        ServerRequestToken requestToken;
        std::string method;
        Json params;
        Json raw;
        std::optional<DecodeDiagnostic> diagnostic;
    };

    struct ApplyPatchApprovalRequest {
        ServerRequestId requestId;
        ServerRequestToken requestToken;
        ApplyPatchApprovalParams params;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct ExecCommandApprovalRequest {
        ServerRequestId requestId;
        ServerRequestToken requestToken;
        ExecCommandApprovalParams params;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct PermissionsApprovalRequest {
        ServerRequestId requestId;
        ServerRequestToken requestToken;
        PermissionsRequestApprovalParams params;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct AttestationGenerateRequest {
        ServerRequestId requestId;
        ServerRequestToken requestToken;
        AttestationGenerateParams params;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct DynamicToolCallRequest {
        ServerRequestId requestId;
        ServerRequestToken requestToken;
        DynamicToolCallParams params;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    struct McpServerElicitationRequest {
        ServerRequestId requestId;
        ServerRequestToken requestToken;
        McpServerElicitationRequestParams params;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;
    };

    // Existing alternatives retain their indices. A1.4b request alternatives
    // append after all earlier alternatives.
    using TypedServerRequest = std::variant<CommandApprovalRequest,
                                            FileChangeApprovalRequest,
                                            UserInputRequest,
                                            AuthenticationRequest,
                                            UnknownServerRequest,
                                            ApplyPatchApprovalRequest,
                                            ExecCommandApprovalRequest,
                                            PermissionsApprovalRequest,
                                            AttestationGenerateRequest,
                                            DynamicToolCallRequest,
                                            McpServerElicitationRequest>;

    struct ApprovalDecision {
        std::string value;

        static ApprovalDecision accept();
        static ApprovalDecision acceptForSession();
        static ApprovalDecision decline();
        static ApprovalDecision cancel();

        auto operator<=>(const ApprovalDecision&) const = default;
    };

    struct UserInputAnswer {
        std::string questionId;
        std::vector<std::string> answers;
    };

    struct AuthenticationResponse {
        std::string accessToken;
        std::string chatgptAccountId;
        std::optional<std::string> chatgptPlanType;
    };

    class Requests {
    public:
        using RequestHandler = std::function<void(const TypedServerRequest&)>;

        Requests(const Requests&) = delete;
        Requests(Requests&&) = delete;
        Requests& operator=(const Requests&) = delete;
        Requests& operator=(Requests&&) = delete;

        void setOnRequest(RequestHandler handler);

        SendResult respond(const CommandApprovalRequest& request, ApprovalDecision decision);
        SendResult respond(const CommandApprovalRequest& request, CommandExecutionRequestApprovalResponse response);
        SendResult respond(const FileChangeApprovalRequest& request, ApprovalDecision decision);
        SendResult respond(const FileChangeApprovalRequest& request, FileChangeRequestApprovalResponse response);
        SendResult respond(const ApplyPatchApprovalRequest& request, ApplyPatchApprovalResponse response);
        SendResult respond(const ExecCommandApprovalRequest& request, ExecCommandApprovalResponse response);
        SendResult respond(const PermissionsApprovalRequest& request, PermissionsRequestApprovalResponse response);
        SendResult respond(const AttestationGenerateRequest& request, AttestationGenerateResponse response);
        SendResult respond(const DynamicToolCallRequest& request, DynamicToolCallResponse response);
        SendResult respond(const UserInputRequest& request, ToolRequestUserInputResponse response);
        SendResult respond(const UserInputRequest& request, std::vector<UserInputAnswer> answers);
        SendResult respond(const McpServerElicitationRequest& request, McpServerElicitationRequestResponse response);
        SendResult respond(const AuthenticationRequest& request, ChatgptAuthTokensRefreshResponse response);
        SendResult respond(const AuthenticationRequest& request, AuthenticationResponse response);
        SendResult reject(const AttestationGenerateRequest& request, ProtocolError error);
        SendResult reject(const DynamicToolCallRequest& request, ProtocolError error);
        SendResult reject(const UserInputRequest& request, ProtocolError error);
        SendResult reject(const McpServerElicitationRequest& request, ProtocolError error);
        SendResult respondRaw(const UnknownServerRequest& request, Json result);
        SendResult reject(const UnknownServerRequest& request, ProtocolError error);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Requests(AppServerClient::RawProtocol& protocol) noexcept;

        static SendResult validationFailure(std::string message);

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_SERVERREQUESTS_H
