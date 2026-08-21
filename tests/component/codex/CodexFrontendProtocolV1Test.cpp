/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/EventJournal.h"
#include "ai/openai/codex/frontend/UpdateBatch.h"
#include "support/TestResult.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
    using namespace ai::openai::codex::frontend;

    template <typename Journal>
    concept HasLegacyEventAppend = requires(Journal& journal) { journal.append(std::string{}, Json::object(), Json::object()); };

    template <typename Journal>
    concept HasLegacyEventReplay = requires(const Journal& journal) { journal.replayAfter(SequenceNumber{}); };

    void expectClientRoundTrip(tests::support::TestResult& result, const ClientMessage& message, const std::string& description) {
        const auto encoded = Codec::encodeClient(message);
        result.expectTrue(encoded.hasValue(), description + " encodes");
        if (!encoded) {
            return;
        }
        const auto decoded = Codec::decodeClient(encoded.value());
        result.expectTrue(decoded.hasValue(), description + " decodes");
        if (!decoded) {
            return;
        }
        const auto reencoded = Codec::encodeClient(decoded.value());
        result.expectTrue(reencoded.hasValue() && reencoded.value() == encoded.value(), description + " round-trips exactly");
    }

    void expectServerRoundTrip(tests::support::TestResult& result, const ServerMessage& message, const std::string& description) {
        const auto encoded = Codec::encodeServer(message);
        result.expectTrue(encoded.hasValue(), description + " encodes");
        if (!encoded) {
            return;
        }
        const auto decoded = Codec::decodeServer(encoded.value());
        result.expectTrue(decoded.hasValue(), description + " decodes");
        if (!decoded) {
            return;
        }
        const auto reencoded = Codec::encodeServer(decoded.value());
        result.expectTrue(reencoded.hasValue() && reencoded.value() == encoded.value(), description + " round-trips exactly");
    }

    Command command(std::string requestId, CommandParameters parameters) {
        return {std::move(requestId), std::move(parameters), Json::object(), Json::object()};
    }

    // Test-only evaluator for the draft-2020-12 keywords reached by FrontendEvent -> ItemState.
    // It keeps the checked-in schema, rather than a duplicated C++ predicate, authoritative for these examples.
    bool matchesJsonType(const Json& value, std::string_view type) {
        if (type == "object") {
            return value.is_object();
        }
        if (type == "array") {
            return value.is_array();
        }
        if (type == "string") {
            return value.is_string();
        }
        if (type == "boolean") {
            return value.is_boolean();
        }
        if (type == "integer") {
            return value.is_number_integer() || value.is_number_unsigned();
        }
        if (type == "number") {
            return value.is_number();
        }
        if (type == "null") {
            return value.is_null();
        }
        return false;
    }

    bool matchesSchema(const Json& root, const Json& schema, const Json& value, std::size_t depth = 0) {
        if (depth > 64) {
            return false;
        }
        if (schema.is_boolean()) {
            return schema.get<bool>();
        }
        if (!schema.is_object()) {
            return true;
        }

        if (const auto reference = schema.find("$ref"); reference != schema.end()) {
            constexpr std::string_view definitionsPrefix = "#/$defs/";
            if (!reference->is_string()) {
                return false;
            }
            const std::string name = reference->get<std::string>();
            if (!name.starts_with(definitionsPrefix)) {
                return false;
            }
            const auto definitions = root.find("$defs");
            const auto definition =
                definitions != root.end() ? definitions->find(name.substr(definitionsPrefix.size())) : Json::const_iterator{};
            if (definitions == root.end() || definition == definitions->end() || !matchesSchema(root, *definition, value, depth + 1)) {
                return false;
            }
        }

        if (const auto type = schema.find("type"); type != schema.end()) {
            bool typeMatched = false;
            if (type->is_string()) {
                typeMatched = matchesJsonType(value, type->get<std::string>());
            } else if (type->is_array()) {
                for (const Json& candidate : *type) {
                    typeMatched = typeMatched || (candidate.is_string() && matchesJsonType(value, candidate.get<std::string>()));
                }
            }
            if (!typeMatched) {
                return false;
            }
        }
        if (const auto constant = schema.find("const"); constant != schema.end() && value != *constant) {
            return false;
        }
        if (const auto enumeration = schema.find("enum");
            enumeration != schema.end() && enumeration->is_array() &&
            std::find(enumeration->begin(), enumeration->end(), value) == enumeration->end()) {
            return false;
        }
        if (value.is_number()) {
            if (const auto minimum = schema.find("minimum"); minimum != schema.end() && value < *minimum) {
                return false;
            }
            if (const auto maximum = schema.find("maximum"); maximum != schema.end() && value > *maximum) {
                return false;
            }
        }

        if (value.is_object()) {
            if (const auto required = schema.find("required"); required != schema.end() && required->is_array()) {
                for (const Json& name : *required) {
                    if (!name.is_string() || !value.contains(name.get<std::string>())) {
                        return false;
                    }
                }
            }
            if (const auto properties = schema.find("properties"); properties != schema.end() && properties->is_object()) {
                for (const auto& [name, propertySchema] : properties->items()) {
                    if (const auto member = value.find(name);
                        member != value.end() && !matchesSchema(root, propertySchema, *member, depth + 1)) {
                        return false;
                    }
                }
            }
        }
        if (value.is_array()) {
            if (const auto items = schema.find("items"); items != schema.end()) {
                for (const Json& item : value) {
                    if (!matchesSchema(root, *items, item, depth + 1)) {
                        return false;
                    }
                }
            }
        }

        if (const auto allOf = schema.find("allOf"); allOf != schema.end() && allOf->is_array()) {
            for (const Json& memberSchema : *allOf) {
                if (!matchesSchema(root, memberSchema, value, depth + 1)) {
                    return false;
                }
            }
        }
        if (const auto condition = schema.find("if"); condition != schema.end()) {
            const bool conditionMatched = matchesSchema(root, *condition, value, depth + 1);
            const auto branch = schema.find(conditionMatched ? "then" : "else");
            if (branch != schema.end() && !matchesSchema(root, *branch, value, depth + 1)) {
                return false;
            }
        }
        return true;
    }

    void testUserMessageDataSchema(tests::support::TestResult& result) {
        const std::filesystem::path sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
        const std::filesystem::path schemaPath = sourceRoot / "docs/ai/openai/codex/frontend-protocol-v1.schema.json";
        std::ifstream input(schemaPath);
        result.expectTrue(input.good(), "the checked-in Frontend Protocol v1 schema is readable");
        if (!input) {
            return;
        }

        const Json schema = Json::parse(input, nullptr, false);
        result.expectTrue(!schema.is_discarded() && schema.contains("$defs") && schema["$defs"].contains("UserMessageData"),
                          "the schema defines normalized user-message data explicitly");
        if (schema.is_discarded() || !schema.contains("$defs") || !schema["$defs"].contains("FrontendEvent")) {
            return;
        }

        const Json content =
            Json::array({Json{{"type", "text"}, {"text", "hello"}}, Json{{"type", "future"}, {"payload", Json{{"nested", true}}}}});
        const auto contentBytes = static_cast<std::uint64_t>(content.dump().size());
        const Json data = Json{{"clientId", nullptr},
                               {"content", content},
                               {"contentTruncated", false},
                               {"originalContentBytes", contentBytes},
                               {"retainedContentBytes", contentBytes},
                               {"originalContentItems", std::uint64_t{2}},
                               {"retainedContentItems", std::uint64_t{2}}};
        const Json item = Json{{"id", "item-1"},
                               {"type", "user_message"},
                               {"status", "completed"},
                               {"agentText", ""},
                               {"reasoningText", ""},
                               {"reasoningSummary", ""},
                               {"commandOutput", ""},
                               {"droppedContentBytes", std::uint64_t{0}},
                               {"contentTruncated", false},
                               {"data", data},
                               {"extensions", Json::object()}};
        const Json event = Json{{"sequence", std::uint64_t{1}},
                                {"type", "item.updated"},
                                {"data", Json{{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"item", item}}}};
        const Json& frontendEventSchema = schema["$defs"]["FrontendEvent"];
        result.expectTrue(matchesSchema(schema, frontendEventSchema, event),
                          "the schema accepts array-valued normalized user-message content");

        Json invalidEvent = event;
        invalidEvent["data"]["item"]["data"]["content"] = Json{{"truncated", true}};
        result.expectTrue(!matchesSchema(schema, frontendEventSchema, invalidEvent),
                          "the schema rejects object-valued normalized user-message content");

        Json incompleteEvent = event;
        incompleteEvent["data"]["item"]["data"].erase("retainedContentItems");
        result.expectTrue(!matchesSchema(schema, frontendEventSchema, incompleteEvent),
                          "the schema requires user-message truncation metadata");

        Json futureItemEvent = invalidEvent;
        futureItemEvent["data"]["item"]["type"] = "future_item";
        result.expectTrue(matchesSchema(schema, frontendEventSchema, futureItemEvent),
                          "the user-message condition leaves future item data shapes unrestricted");

        const Json& definitions = schema["$defs"];
        const Json* expandedUserMessage = nullptr;
        for (const Json& alternative : definitions.at("ExpandedThreadItem").at("oneOf")) {
            const Json& properties = alternative.at("allOf").at(1).at("properties");
            if (properties.at("type").value("const", std::string{}) == "userMessage") {
                expandedUserMessage = &properties;
                break;
            }
        }
        const Json& semantic = definitions.at("UserMessageSemanticData");
        result.expectTrue(expandedUserMessage &&
                              expandedUserMessage->at("data").value("$ref", std::string{}) ==
                                  "#/$defs/UserMessageSemanticData" &&
                              definitions.at("ExpandedThreadItemBase").at("properties").at("data").empty() &&
                              semantic.at("properties").at("text").value("maxLength", 0U) == 16U * 1024U * 1024U &&
                              semantic.at("properties").at("clientId").at("anyOf").at(0).value("maxLength", 0U) ==
                                  16U * 1024U * 1024U,
                          "expanded user messages use their dedicated app-server-sized semantic schema instead of generic detail bounds");
    }

    void testDefaultReplayHeadroom(tests::support::TestResult& result) {
        constexpr std::size_t retainedSlackBytes = 32;
        constexpr std::size_t maximumPayloadBytes = 16U * 1024U;
        const std::size_t retainedTarget = DefaultJournalMaxBytes - retainedSlackBytes;
        std::vector<FrontendEvent> retainedEvents;
        std::size_t retainedBytes = 0;

        while (retainedBytes < retainedTarget && retainedEvents.size() < DefaultJournalMaxEntries) {
            const SequenceNumber next{retainedEvents.size() + 1};
            const std::size_t remaining = retainedTarget - retainedBytes;
            const FrontendEvent empty{next,
                                      "item.content.updated",
                                      Json{{"threadId", "capacity-thread"},
                                           {"turnId", "capacity-turn"},
                                           {"itemId", "capacity-item"},
                                           {"channel", "commandOutput"},
                                           {"content", ""}},
                                      Json::object()};
            const auto emptySize = Codec::serializedEventSize(empty);
            result.expectTrue(emptySize.hasValue(), "capacity-test normalized event has a stable compact base encoding");
            if (!emptySize || remaining <= emptySize.value()) {
                break;
            }

            const std::size_t payloadBytes = std::min(maximumPayloadBytes, remaining - emptySize.value());
            FrontendEvent event{next,
                                "item.content.updated",
                                Json{{"threadId", "capacity-thread"},
                                     {"turnId", "capacity-turn"},
                                     {"itemId", "capacity-item"},
                                     {"channel", "commandOutput"},
                                     {"content", std::string(payloadBytes, 'x')}},
                                Json::object()};
            const auto serializedBytes = Codec::serializedEventSize(event);
            result.expectTrue(serializedBytes.hasValue() && serializedBytes.value() <= remaining,
                              "capacity-test projected event fits without crossing the configured retention budget");
            if (!serializedBytes || serializedBytes.value() > remaining) {
                break;
            }
            retainedBytes += serializedBytes.value();
            retainedEvents.push_back(std::move(event));
        }

        result.expectTrue(retainedBytes <= DefaultJournalMaxBytes && DefaultJournalMaxBytes - retainedBytes < 512 &&
                              retainedEvents.size() <= DefaultJournalMaxEntries,
                          "a projected replay set can fill the default 8 MiB retention budget to within one small event");

        const UpdateBatchResult batches = UpdateBatchBuilder{}.build(retainedEvents);
        result.expectTrue(batches.success(), "the near-capacity projected event set remains a complete normalized replay");

        std::size_t queuedBytes = 0;
        const SequenceNumber currentSequence = retainedEvents.empty() ? SequenceNumber{} : retainedEvents.back().sequence;
        const auto welcome = Codec::serializeServer(
            ServerMessage{Welcome{"capacity-session", SessionRole::Observer, currentSequence, SyncMode::Replay, Json::object()}});
        const auto complete = Codec::serializeServer(ServerMessage{SyncComplete{currentSequence, Json::object()}});
        result.expectTrue(welcome.hasValue() && complete.hasValue(), "capacity-test replay control envelopes serialize");
        if (welcome && complete) {
            queuedBytes = welcome.value().size() + complete.value().size();
        }
        bool boundedBatches = true;
        for (const BoundedEventBatch& batch : batches.batches) {
            boundedBatches = boundedBatches && batch.serializedBytes <= DefaultBatchMaxBytes;
            queuedBytes += batch.serializedBytes;
        }
        const std::size_t queuedMessages = batches.batches.size() + 2;
        result.expectTrue(boundedBatches && queuedBytes > DefaultJournalMaxBytes && queuedBytes <= DefaultFrontendServiceMaxOutboundBytes &&
                              queuedMessages <= DefaultFrontendServiceMaxOutboundMessages,
                          "default adapter headroom accepts exact batch/control overhead for a near-8 MiB retained replay");
    }

    void testProducerExpandedEventValidation(tests::support::TestResult& result) {
        const FrontendEvent malformed{
            SequenceNumber{123},
            "item.upserted",
            Json{{"item", Json{{"id", "item-1"}, {"type", "user_message"}, {"opaque", "MUST_NOT_APPEAR_IN_ERROR"}}}},
            Json::object(),
        };
        const auto encodedMalformed = Codec::encodeEvent(malformed);
        const auto serializedMalformed =
            Codec::serializeServer(ServerMessage{EventBatch{SequenceNumber{123}, SequenceNumber{123}, {malformed}, Json::object()}});
        const UpdateBatchResult batchedMalformed = UpdateBatchBuilder{}.build({malformed});
        result.expectTrue(!encodedMalformed && !serializedMalformed && batchedMalformed.status == UpdateBatchStatus::EncodingFailure &&
                              encodedMalformed.error().message.find(
                                  "expanded event.data.item.type value 'user_message' is not a schema-defined discriminator") !=
                                  std::string::npos &&
                              encodedMalformed.error().message.find("MUST_NOT_APPEAR_IN_ERROR") == std::string::npos,
                          "generic producer serialization rejects malformed expanded events locally with a bounded discriminator error");

        const FrontendEvent valid{
            SequenceNumber{124}, "item.upserted", Json{{"item", Json{{"id", "item-1"}, {"type", "userMessage"}}}}, Json::object()};
        const auto serializedValid =
            Codec::serializeServer(ServerMessage{EventBatch{SequenceNumber{124}, SequenceNumber{124}, {valid}, Json::object()}});
        const auto decodedValid =
            serializedValid ? Codec::decodeServer(std::string_view{serializedValid.value()})
                            : CodecResult<ServerMessage>{
                                  CodecError{ErrorCode::InternalError, "serialization failed", false, {}, std::nullopt, std::nullopt}};
        result.expectTrue(serializedValid.hasValue() && decodedValid.hasValue(),
                          "a valid generic expanded event passes producer schema validation and round-trips");

        const FrontendEvent schemaNeutralAppend{
            SequenceNumber{125},
            "item.content.updated",
            Json{{"threadId", "thread-1"},
                 {"turnId", "turn-1"},
                 {"itemId", "item-1"},
                 {"channel", "agentText"},
                 {"content", ""},
                 {"contentDelta", "suffix"},
                 {"baseContentBytes", std::uint64_t{6}}},
            Json::object()};
        const auto encodedSchemaNeutralAppend = Codec::encodeEvent(schemaNeutralAppend);
        FrontendEvent appendWithoutRequiredContent = schemaNeutralAppend;
        appendWithoutRequiredContent.data.erase("content");
        const auto encodedAppendWithoutRequiredContent = Codec::encodeEvent(appendWithoutRequiredContent);
        result.expectTrue(encodedSchemaNeutralAppend.hasValue() && !encodedAppendWithoutRequiredContent.hasValue(),
                          "schema-neutral append metadata is accepted through SafeDetail additional properties only when the frozen "
                          "item-content replacement shape remains complete");

        const FrontendEvent legacy{
            SequenceNumber{125},
            "item.updated",
            Json{{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"item", Json{{"id", "item-1"}, {"type", "user_message"}}}},
            Json::object()};
        const FrontendEvent legacyDiagnostics{
            SequenceNumber{126}, "diagnostics.updated", Json{{"received", 1}, {"recent", Json::array()}}, Json::object()};
        const auto encodedLegacy = Codec::encodeEvent(legacy);
        const auto encodedLegacyDiagnostics = Codec::encodeEvent(legacyDiagnostics);
        result.expectTrue(encodedLegacy.hasValue() && encodedLegacy.value().at("data").at("item").at("type") == "user_message" &&
                              encodedLegacyDiagnostics.hasValue() && encodedLegacyDiagnostics.value().at("data").contains("diagnostic") &&
                              encodedLegacyDiagnostics.value().at("data").contains("received") &&
                              encodedLegacyDiagnostics.value().at("data").contains("recent"),
                          "legacy event validation remains compatible while the dual-name diagnostics event gains an additive safe view");
    }
} // namespace

int main() {
    using namespace ai::openai::codex::frontend;

    static_assert(std::variant_size_v<CommandParameters> == 15, "A1.6b must not expand the Frontend Protocol v1 command surface");
    static_assert(!HasLegacyEventAppend<EventJournal> && !HasLegacyEventReplay<EventJournal>,
                  "EventJournal must expose only the FrontendService canonical authority");

    tests::support::TestResult result;

    expectClientRoundTrip(result, ClientMessage{Hello{}}, "hello without replay position");
    expectClientRoundTrip(result, ClientMessage{Hello{SequenceNumber(123), Json{{"future", true}}}}, "hello with replay position");
    const auto legacyHelloBytes = Codec::serializeClient(ClientMessage{Hello{}});
    result.expectTrue(legacyHelloBytes.hasValue() &&
                          legacyHelloBytes.value() == R"({"kind":"hello","protocol":"snodec.codex-frontend","version":1})",
                      "an omitted authentication credential preserves the original Hello bytes");

    constexpr std::string_view BearerSentinel = "A17B_SYNTHETIC_BEARER_SENTINEL";
    const Hello authenticatedHello{
        SequenceNumber{123},
        Json{{"future", true}},
        std::vector<FrontendCapability>{FrontendCapability::AuthenticatedFrontend},
        AuthenticationCredential{BearerCredential{std::string(BearerSentinel)}},
    };
    expectClientRoundTrip(result, ClientMessage{authenticatedHello}, "hello with additive bearer authentication");
    const auto encodedAuthenticatedHello = Codec::encodeClient(ClientMessage{authenticatedHello});
    result.expectTrue(encodedAuthenticatedHello.hasValue() && encodedAuthenticatedHello.value().at("authentication") ==
                                                                  Json{{"scheme", "bearer"}, {"token", std::string(BearerSentinel)}},
                      "the bearer credential is serialized only in the inbound Hello authentication member");
    const auto decodedAuthenticatedHello = encodedAuthenticatedHello ? Codec::decodeClient(encodedAuthenticatedHello.value())
                                                                     : CodecResult<ClientMessage>{encodedAuthenticatedHello.error()};
    const auto* decodedHello = decodedAuthenticatedHello ? std::get_if<Hello>(&decodedAuthenticatedHello.value()) : nullptr;
    const auto* decodedCredential =
        decodedHello && decodedHello->authentication ? std::get_if<BearerCredential>(&*decodedHello->authentication) : nullptr;
    result.expectTrue(decodedCredential && decodedCredential->token == BearerSentinel,
                      "the typed Hello retains the bearer credential for transport-neutral authentication");

    const auto wrongAuthenticationScheme =
        Codec::decodeClient(Json{{"protocol", ProtocolIdentity},
                                 {"version", ProtocolVersion},
                                 {"kind", "hello"},
                                 {"authentication", Json{{"scheme", "future"}, {"token", std::string(BearerSentinel)}}}});
    result.expectTrue(!wrongAuthenticationScheme && wrongAuthenticationScheme.error().code == ErrorCode::InvalidField &&
                          wrongAuthenticationScheme.error().message.find(BearerSentinel) == std::string::npos,
                      "an unsupported Hello authentication scheme is rejected without leaking the credential");
    const auto malformedAuthentication = Codec::decodeClient(Json{{"protocol", ProtocolIdentity},
                                                                  {"version", ProtocolVersion},
                                                                  {"kind", "hello"},
                                                                  {"authentication", Json{{"scheme", "bearer"}, {"token", 7}}}});
    result.expectTrue(!malformedAuthentication && malformedAuthentication.error().code == ErrorCode::InvalidField,
                      "a malformed Hello bearer credential is rejected by the generated schema");

    bool transportKindsRoundTrip = true;
    for (const FrontendTransportKind kind : FrontendTransportKinds) {
        transportKindsRoundTrip = transportKindsRoundTrip && frontendTransportKindFromString(toString(kind)) == kind;
    }
    bool authenticationFailuresRoundTrip = true;
    for (const AuthenticationFailureCode code : AuthenticationFailureCodes) {
        authenticationFailuresRoundTrip = authenticationFailuresRoundTrip && authenticationFailureCodeFromString(toString(code)) == code;
    }
    result.expectTrue(transportKindsRoundTrip && authenticationFailuresRoundTrip,
                      "transport and structured authentication failure values have stable exhaustive spellings");

    std::vector<Command> commands;
    commands.push_back(command("controller-acquire", ControllerAcquire{}));
    commands.push_back(command("controller-release", ControllerRelease{}));
    commands.push_back(command("snapshot", SnapshotGet{}));
    commands.push_back(command("replay", ReplayAfter{SequenceNumber(12)}));
    commands.push_back(command("thread-start", ThreadStart{"/tmp/project", "gpt-5", "openai", "on-request", "workspace-write", false}));
    commands.push_back(command("thread-resume", ThreadResume{"thread-1", "/tmp/project", "gpt-5", "openai", "never", "read-only"}));
    commands.push_back(command("thread-list", ThreadList{"cursor", 25, false, "needle"}));
    commands.push_back(command("thread-read", ThreadRead{"thread-1", true}));

    TurnStart turnStart;
    turnStart.threadId = "thread-1";
    turnStart.input = {TextInput{"hello", Json::object()},
                       ImageUrlInput{"https://example.invalid/image.png", "low", Json::object()},
                       LocalImageInput{"/tmp/image.png", "high", Json::object()},
                       SkillInput{"skill", "/tmp/SKILL.md", Json::object()},
                       MentionInput{"file", "/tmp/file.cpp", Json::object()}};
    turnStart.cwd = "/tmp/project";
    turnStart.model = "gpt-5";
    turnStart.reasoningEffort = "high";
    turnStart.approvalPolicy = "on-request";
    turnStart.sandboxPolicy = SandboxPolicy{"workspaceWrite", false, {"/tmp/project"}, false, true, Json::object()};
    commands.push_back(command("turn-start", std::move(turnStart)));
    commands.push_back(command("turn-interrupt", TurnInterrupt{"thread-1", "turn-1"}));
    commands.push_back(command("approval", ApprovalRespond{"41", "decline"}));
    commands.push_back(command("user-input", UserInputRespond{"42", {{"question-1", {"answer"}}}}));
    commands.push_back(command("authentication", AuthenticationRespond{"43", "secret-token", "account", "plus"}));
    commands.push_back(command("unknown-response", UnknownRequestRespond{"44", Json{{"accepted", false}}}));
    commands.push_back(
        command("unknown-reject", UnknownRequestReject{"45", -32001, "unsupported", std::optional<Json>{Json{{"reason", "test"}}}}));

    for (const Command& value : commands) {
        expectClientRoundTrip(result, ClientMessage{value}, "command " + value.requestId);
    }

    expectServerRoundTrip(
        result, ServerMessage{Welcome{"7", SessionRole::Observer, SequenceNumber(140), SyncMode::Replay, Json::object()}}, "welcome");
    expectServerRoundTrip(result, ServerMessage{SyncComplete{SequenceNumber(140), Json::object()}}, "sync complete");
    expectServerRoundTrip(result, ServerMessage{Snapshot{SequenceNumber(140), Json{{"lifecycle", "ready"}}, Json::object()}}, "snapshot");

    const FrontendEvent extensionEvent{
        SequenceNumber(141), "codex.extension", Json{{"method", "future/event"}, {"params", Json{{"value", 1}}}}, Json::object()};
    const FrontendEvent itemEvent{
        SequenceNumber(142),
        "item.content.updated",
        Json{{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"itemId", "item-1"}, {"channel", "agentText"}, {"content", "complete"}},
        Json::object()};
    expectServerRoundTrip(result,
                          ServerMessage{EventBatch{SequenceNumber(141), SequenceNumber(142), {extensionEvent, itemEvent}, Json::object()}},
                          "event batch including unknown Codex extension");
    expectServerRoundTrip(result, ServerMessage{Response::success("ok", Json{{"threadId", "thread-1"}})}, "success response");
    expectServerRoundTrip(result,
                          ServerMessage{Response::failure(
                              "failed", CommandError{ErrorCode::PermissionDenied, "controller required", std::nullopt, Json::object()})},
                          "error response");
    expectServerRoundTrip(
        result,
        ServerMessage{ProtocolErrorMessage{
            ErrorCode::UnsupportedVersion, "unsupported protocol version", {1}, true, std::nullopt, std::nullopt, Json::object()}},
        "protocol error");

    const std::vector<ErrorCode> stableErrors = {ErrorCode::PermissionDenied,
                                                 ErrorCode::InvalidCommand,
                                                 ErrorCode::NotFound,
                                                 ErrorCode::Conflict,
                                                 ErrorCode::LocalSubmissionFailure,
                                                 ErrorCode::TypedDecodingFailure,
                                                 ErrorCode::RemoteAppServerError,
                                                 ErrorCode::Cancelled,
                                                 ErrorCode::BackendUnavailable,
                                                 ErrorCode::DuplicateRequestId,
                                                 ErrorCode::MalformedJson,
                                                 ErrorCode::WrongProtocol,
                                                 ErrorCode::UnsupportedVersion,
                                                 ErrorCode::MissingField,
                                                 ErrorCode::InvalidField,
                                                 ErrorCode::UnknownKind,
                                                 ErrorCode::UnknownMethod,
                                                 ErrorCode::FrameTooLarge,
                                                 ErrorCode::CapacityExceeded,
                                                 ErrorCode::SequenceOverflow,
                                                 ErrorCode::ReplayGap,
                                                 ErrorCode::InternalError};
    for (const ErrorCode code : stableErrors) {
        result.expectTrue(errorCodeFromString(toString(code)) == code, "stable error code round-trips through its wire spelling");
    }

    const auto malformed = Codec::decodeClient(std::string_view("{not-json"));
    result.expectTrue(!malformed && malformed.error().code == ErrorCode::MalformedJson, "malformed JSON is rejected locally");
    const auto missingProtocol = Codec::decodeClient(Json{{"version", 1}, {"kind", "hello"}});
    result.expectTrue(!missingProtocol, "a missing protocol identity is rejected");
    const auto wrongProtocol = Codec::decodeClient(Json{{"protocol", "other"}, {"version", 1}, {"kind", "hello"}});
    result.expectTrue(!wrongProtocol && wrongProtocol.error().code == ErrorCode::WrongProtocol && wrongProtocol.error().closeConnection,
                      "a wrong protocol identity is rejected and closes only the frontend");
    const auto wrongVersion = Codec::decodeClient(Json{{"protocol", ProtocolIdentity}, {"version", 2}, {"kind", "hello"}});
    result.expectTrue(!wrongVersion && wrongVersion.error().code == ErrorCode::UnsupportedVersion &&
                          wrongVersion.error().supportedVersions == std::vector<std::uint32_t>{1},
                      "an unsupported version reports the supported version");
    const auto missingKind = Codec::decodeClient(Json{{"protocol", ProtocolIdentity}, {"version", 1}});
    result.expectTrue(!missingKind, "a missing message kind is rejected");
    const auto unknownKind = Codec::decodeClient(Json{{"protocol", ProtocolIdentity}, {"version", 1}, {"kind", "future-kind"}});
    result.expectTrue(!unknownKind && unknownKind.error().code == ErrorCode::UnknownKind, "an unknown message kind is stable error");
    const auto missingRequestId = Codec::decodeClient(
        Json{{"protocol", ProtocolIdentity}, {"version", 1}, {"kind", "command"}, {"method", "snapshot.get"}, {"params", Json::object()}});
    result.expectTrue(!missingRequestId, "a missing command request ID is rejected");
    const auto emptyRequestId = Codec::decodeClient(Json{{"protocol", ProtocolIdentity},
                                                         {"version", 1},
                                                         {"kind", "command"},
                                                         {"requestId", ""},
                                                         {"method", "snapshot.get"},
                                                         {"params", Json::object()}});
    result.expectTrue(!emptyRequestId, "an empty command request ID is rejected");
    const auto unknownMethod = Codec::decodeClient(Json{{"protocol", ProtocolIdentity},
                                                        {"version", 1},
                                                        {"kind", "command"},
                                                        {"requestId", "unknown"},
                                                        {"method", "future.command"},
                                                        {"params", Json::object()}});
    result.expectTrue(!unknownMethod && unknownMethod.error().code == ErrorCode::UnknownMethod, "an unknown command method is rejected");
    const auto malformedParams = Codec::decodeClient(Json{{"protocol", ProtocolIdentity},
                                                          {"version", 1},
                                                          {"kind", "command"},
                                                          {"requestId", "bad-params"},
                                                          {"method", "turn.interrupt"},
                                                          {"params", Json{{"threadId", 1}}}});
    result.expectTrue(!malformedParams, "malformed command-specific params are rejected");
    const auto unsupportedFullAccessNetwork = Codec::decodeClient(
        Json{{"protocol", ProtocolIdentity},
             {"version", 1},
             {"kind", "command"},
             {"requestId", "unsupported-full-access-network"},
             {"method", "turn.start"},
             {"params",
              Json{{"threadId", "thread-1"},
                   {"input", Json::array({Json{{"type", "text"}, {"text", "hello"}}})},
                   {"sandboxPolicy", Json{{"type", "dangerFullAccess"}, {"networkAccess", false}}}}}});
    result.expectTrue(!unsupportedFullAccessNetwork && unsupportedFullAccessNetwork.error().code == ErrorCode::InvalidField,
                      "danger-full-access rejects an unsupported network override instead of silently dropping it");
    const auto invalidRange = Codec::decodeClient(Json{{"protocol", ProtocolIdentity},
                                                       {"version", 1},
                                                       {"kind", "command"},
                                                       {"requestId", "bad-range"},
                                                       {"method", "thread.list"},
                                                       {"params", Json{{"limit", std::uint64_t{1} << 40}}}});
    result.expectTrue(!invalidRange, "out-of-range command integers are rejected safely");
    const auto unknownField = Codec::decodeClient(
        Json{{"protocol", ProtocolIdentity}, {"version", 1}, {"kind", "hello"}, {"futureField", Json{{"nested", true}}}});
    result.expectTrue(unknownField.hasValue(), "unknown non-conflicting fields are tolerated");

    EventJournal barrierJournal({4, 4096, SequenceNumber(0)});
    const SequenceNumber beforeBarrierSequence = barrierJournal.currentSequence();
    result.expectTrue(barrierJournal.invalidateReplay() && barrierJournal.currentSequence() > beforeBarrierSequence &&
                          barrierJournal.oldestReplayableAfter() == barrierJournal.currentSequence() &&
                          barrierJournal.retainedEntryCount() == 0 && barrierJournal.retainedBytes() == 0,
                      "the public journal surface exposes diagnostics and replay invalidation but no alternate append authority");
    EventJournal exhaustedBarrier({1, 1024, SequenceNumber(std::numeric_limits<std::uint64_t>::max())});
    result.expectTrue(!exhaustedBarrier.invalidateReplay() &&
                          exhaustedBarrier.currentSequence() == SequenceNumber(std::numeric_limits<std::uint64_t>::max()) &&
                          exhaustedBarrier.oldestReplayableAfter() == exhaustedBarrier.currentSequence(),
                      "snapshot barrier exhaustion fails explicitly without wrapping");

    std::vector<FrontendEvent> batchEvents;
    for (std::uint64_t sequence = 1; sequence <= 7; ++sequence) {
        batchEvents.push_back(
            {SequenceNumber(sequence), "diagnostics.updated", Json{{"received", sequence}, {"recent", Json::array()}}, Json::object()});
    }
    UpdateBatchBuilder builder({3, 1024});
    const auto batches = builder.build(batchEvents);
    result.expectTrue(batches.success() && batches.batches.size() == 3, "event-count bounds split updates into bounded batches");
    std::uint64_t expectedSequence = 1;
    bool ordered = true;
    for (const BoundedEventBatch& batch : batches.batches) {
        ordered = ordered && batch.batch.events.size() <= 3 && batch.serializedBytes <= 1024;
        for (const FrontendEvent& event : batch.batch.events) {
            ordered = ordered && event.sequence.value() == expectedSequence++;
        }
    }
    result.expectTrue(ordered, "bounded batching preserves stable sequence order and serialized-size bounds");

    const auto noticeData = [](std::string summary) {
        return Json{{"notice",
                     {{"category", "warning"}, {"summary", std::move(summary)}, {"stamp", {{"generation", 1}, {"freshness", "current"}}}}}};
    };
    const std::vector<FrontendEvent> expandedOccurrences{
        {SequenceNumber(8), "notice.added", noticeData("configuration changed"), Json::object()},
        {SequenceNumber(8), "notice.added", noticeData("configuration details changed"), Json::object()},
        {SequenceNumber(9), "notice.added", noticeData("diagnostics ready"), Json::object()},
    };
    const UpdateBatchResult atomicBatches = UpdateBatchBuilder({2, 4096}).build(expandedOccurrences);
    result.expectTrue(atomicBatches.success() && atomicBatches.batches.size() == 2 && atomicBatches.batches[0].batch.events.size() == 2 &&
                          atomicBatches.batches[0].batch.fromSequence == SequenceNumber(8) &&
                          atomicBatches.batches[0].batch.toSequence == SequenceNumber(8) &&
                          atomicBatches.batches[1].batch.events.size() == 1,
                      "all expanded families for one canonical occurrence remain in one atomic same-sequence batch group");
    if (atomicBatches.success() && !atomicBatches.batches.empty()) {
        expectServerRoundTrip(result, ServerMessage{atomicBatches.batches.front().batch}, "same-sequence expanded occurrence batch");
    }
    const UpdateBatchResult unsplittable = UpdateBatchBuilder({1, 4096}).build(expandedOccurrences);
    result.expectTrue(unsplittable.requiresSnapshot() && unsplittable.oversizedSequence == SequenceNumber(8),
                      "an expanded occurrence that exceeds a batch bound falls back to snapshot instead of splitting");
    const UpdateBatchResult repeatedExpanded =
        UpdateBatchBuilder({4, 4096}).build({{SequenceNumber(8), "notice.added", noticeData("first entity"), Json::object()},
                                             {SequenceNumber(8), "notice.added", noticeData("second entity"), Json::object()}});
    const UpdateBatchResult duplicateLegacy =
        UpdateBatchBuilder({4, 4096}).build({{SequenceNumber(8), "codex.extension", Json::object(), Json::object()},
                                             {SequenceNumber(8), "item.updated", Json::object(), Json::object()}});
    result.expectTrue(repeatedExpanded.success() && duplicateLegacy.status == UpdateBatchStatus::InvalidSequence,
                      "equal sequences permit repeated expanded families for distinct entities but reject legacy representations");

    const std::vector<FrontendEvent> mixedRepresentations{
        {SequenceNumber(10), "notice.added", noticeData("safe notice"), Json::object()},
        {SequenceNumber(11), "codex.extension", Json{{"method", "example"}, {"params", Json::object()}}, Json::object()},
    };
    const UpdateBatchResult representationBatches = UpdateBatchBuilder({8, 4096}).build(mixedRepresentations);
    bool representationBatchesEncode = representationBatches.success() && representationBatches.batches.size() == 2;
    for (const BoundedEventBatch& batch : representationBatches.batches) {
        representationBatchesEncode = representationBatchesEncode && Codec::encodeServer(ServerMessage{batch.batch}).hasValue();
    }
    const EventBatch mixedBatch{SequenceNumber(10), SequenceNumber(11), mixedRepresentations, Json::object()};
    const bool mixedRejected = !Codec::encodeServer(ServerMessage{mixedBatch}).hasValue();
    result.expectTrue(representationBatchesEncode && mixedRejected,
                      "batching separates legacy and expanded schema branches while each resulting wire batch remains valid "
                      "(status=" +
                          std::to_string(static_cast<int>(representationBatches.status)) +
                          ", batches=" + std::to_string(representationBatches.batches.size()) + ", encoded=" +
                          std::to_string(representationBatchesEncode) + ", mixedRejected=" + std::to_string(mixedRejected) + ")");

    UpdateBatchBuilder tinyBuilder({8, 120});
    const auto oversized = tinyBuilder.build({{SequenceNumber(1), "notice.added", noticeData(std::string(1024, 'x')), Json::object()}});
    result.expectTrue(oversized.requiresSnapshot() && oversized.oversizedSequence == SequenceNumber(1),
                      "a single oversized update requests snapshot fallback");

    testDefaultReplayHeadroom(result);
    testProducerExpandedEventValidation(result);
    testUserMessageDataSchema(result);

    return result.processResult();
}
