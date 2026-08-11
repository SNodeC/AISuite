                result.cwd = typed::AbsolutePath{*cwd};
            if (const auto model = stringMember(value, "model"))
                result.model = typed::ModelId{*model};
            result.modelProvider = stringMember(value, "modelProvider");
            result.status = stringMember(value, "status");
            result.fullyLoaded = optionalBool(value, "fullyLoaded").value_or(false);
            if (const auto realtime = value.find("realtime"); realtime != value.end()) {
                ThreadRealtimeState decoded;
                if (!decodeThreadRealtime(*realtime, decoded, error))
                    return false;
                result.realtime = std::move(decoded);
            }
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                SourceStamp decoded;
                if (!decodeStamp(*stamp, decoded, error))
                    return false;
                result.stamp = decoded;
            }
            result.createdAtMs = optionalInteger(value, "createdAtMs");
            if (!result.createdAtMs)
                result.createdAtMs = optionalInteger(value, "createdAt");
            result.updatedAtMs = optionalInteger(value, "updatedAtMs");
            if (!result.updatedAtMs)
                result.updatedAtMs = optionalInteger(value, "updatedAt");
            if (const auto turns = value.find("turns"); turns != value.end()) {
                if (!turns->is_array()) {
                    error = "thread turns must be an array";
                    return false;
                }
                for (const frontend::Json& turn : *turns) {
                    const auto turnId = stringMember(turn, "id");
                    if (!turnId || turnId->empty()) {
                        error = "thread turn lacks a nonempty id";
                        return false;
                    }
                    const typed::TurnId idValue{*turnId};
                    if (std::find(result.orderedTurns.begin(), result.orderedTurns.end(), idValue) != result.orderedTurns.end()) {
                        error = "thread turns contain a duplicate id";
                        return false;
                    }
                    result.orderedTurns.push_back(idValue);
                }
            }
            return decodeExtensions(value,
                                    {"id",
                                     "title",
                                     "name",
                                     "preview",
                                     "cwd",
                                     "model",
                                     "modelProvider",
                                     "status",
                                     "fullyLoaded",
                                     "realtime",
                                     "stamp",
                                     "createdAt",
                                     "updatedAt",
                                     "createdAtMs",
                                     "updatedAtMs",
                                     "turns",
                                     "extensions"},
                                    result.extensions,
                                    "thread",
                                    error);
        }

        bool decodeTurn(const frontend::Json& value,
                        TurnState& result,
                        std::string& error,
                        std::optional<typed::ThreadId> fallbackThread = std::nullopt,
                        bool strict = true) {
            if (!requireObject(value, "turn", error))
                return false;
            const auto id = stringMember(value, "id");
            const auto threadId = stringMember(value, "threadId");
            const auto status = stringMember(value, "status");
            const auto active = optionalBool(value, "active");
            const auto terminal = optionalBool(value, "terminal");
            if ((!id || id->empty() || (!threadId && !fallbackThread) || (strict && (!status || !active || !terminal)))) {
                error = "turn state lacks required stable fields";
                return false;
            }
            result.id = typed::TurnId{*id};
            result.threadId = threadId ? typed::ThreadId{*threadId} : *fallbackThread;
            result.status = typed::TurnStatus{status.value_or("unknown")};
            result.active = active.value_or(false);
            result.terminal = terminal.value_or(false);
            result.connectionInvalidated = optionalBool(value, "connectionInvalidated").value_or(false);
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                SourceStamp decoded;
                if (!decodeStamp(*stamp, decoded, error))
                    return false;
                result.stamp = decoded;
            }
            if (const auto items = value.find("items"); items != value.end()) {
                if (!items->is_array()) {
                    error = "turn items must be an array";
                    return false;
                }
                for (const frontend::Json& item : *items) {
                    const auto itemId = stringMember(item, "id");
                    if (!itemId || itemId->empty()) {
                        error = "turn item lacks a nonempty id";
                        return false;
                    }
                    const typed::ItemId idValue{*itemId};
                    if (std::find(result.orderedItems.begin(), result.orderedItems.end(), idValue) != result.orderedItems.end()) {
                        error = "turn items contain a duplicate id";
                        return false;
                    }
                    result.orderedItems.push_back(idValue);
                }
            }
            if (const auto failure = value.find("failure"); failure != value.end())
                result.failure = *failure;
            if (const auto usage = value.find("tokenUsage"); usage != value.end())
                result.tokenUsage = *usage;
            return decodeExtensions(value,
                                    {"id",
                                     "threadId",
                                     "status",
                                     "active",
                                     "terminal",
                                     "connectionInvalidated",
                                     "stamp",
                                     "items",
                                     "failure",
                                     "tokenUsage",
                                     "extensions"},
                                    result.extensions,
                                    "turn",
                                    error);
        }

        ItemState decodeItem(const frontend::ExpandedThreadItem& value) {
            ItemState result;
            result.id = typed::ItemId{value.id};
            if (value.threadId)
                result.threadId = typed::ThreadId{*value.threadId};
            if (value.turnId)
                result.turnId = typed::TurnId{*value.turnId};
            result.kind = value.type;
            result.status = value.status;
            result.summary = value.summary;
            result.location = value.location;
            result.agentText = value.agentText;
            result.reasoningText = value.reasoningText;
            result.reasoningSummary = value.reasoningSummary;
            result.commandOutput = value.commandOutput;
            result.droppedContentBytes = value.droppedContentBytes;
            result.contentTruncated = value.contentTruncated.value_or(value.truncated);
            result.startedAtMs = value.startedAtMs;
            result.completedAtMs = value.completedAtMs;
            result.data = value.data;
            result.truncated = value.truncated;
            result.omittedFields = value.omittedFields;
            result.connectionInvalidated = value.connectionInvalidated;
            if (value.generation && value.freshness)
                result.stamp = SourceStamp{*value.generation, *value.freshness};
            result.extensions = value.extensions;
            return result;
        }

        std::optional<frontend::ThreadItemKind> legacyItemKind(const frontend::Json& value) noexcept {
            try {
                auto typeName = stringMember(value, "type");
                if (!typeName)
                    typeName = stringMember(value, "kind");
                if (!typeName)
                    return std::nullopt;
                if (const auto direct = frontend::threadItemKindFromString(*typeName))
                    return direct;
                if (*typeName == "agent_message")
                    return frontend::ThreadItemKind::AgentMessage;
                if (*typeName == "user_message")
                    return frontend::ThreadItemKind::UserMessage;
                if (*typeName == "command_execution")
                    return frontend::ThreadItemKind::CommandExecution;
                if (*typeName == "file_change")
                    return frontend::ThreadItemKind::FileChange;
                if (*typeName == "web_search")
                    return frontend::ThreadItemKind::WebSearch;
                if (*typeName == "tool_call") {
                    const auto data = value.find("data");
                    return data != value.end() && data->is_object() && data->contains("server") ? frontend::ThreadItemKind::McpToolCall
                                                                                                : frontend::ThreadItemKind::DynamicToolCall;
                }
                const auto data = value.find("data");
                if (data != value.end() && data->is_object()) {
                    const auto codexType = stringMember(*data, "codexType");
                    if (codexType)
                        return frontend::threadItemKindFromString(*codexType);
                }
            } catch (...) {
            }
            return std::nullopt;
        }

        bool decodeLegacyItem(const frontend::Json& value,
                              ItemState& result,
                              std::string& error,
                              std::optional<typed::ThreadId> fallbackThread = std::nullopt,
                              std::optional<typed::TurnId> fallbackTurn = std::nullopt) {
            if (!requireObject(value, "legacy item", error))
                return false;
            const auto id = stringMember(value, "id");
            const auto typeName = stringMember(value, "type");
            const auto knownKind = legacyItemKind(value);
            if (!id || id->empty() || !typeName || typeName->empty()) {
                error = "legacy item lacks a valid id or type";
                return false;
            }
            result.id = typed::ItemId{*id};
            result.kind = ItemKind{knownKind ? std::string(frontend::toString(*knownKind)) : *typeName, knownKind};
            if (const auto threadId = stringMember(value, "threadId"))
                result.threadId = typed::ThreadId{*threadId};
            else
                result.threadId = std::move(fallbackThread);
            if (const auto turnId = stringMember(value, "turnId"))
                result.turnId = typed::TurnId{*turnId};
            else
                result.turnId = std::move(fallbackTurn);
            result.status = stringMember(value, "status");
            result.summary = stringMember(value, "summary");
            if (const auto location = value.find("location"); location != value.end())
                result.location = *location;
            result.agentText = stringMember(value, "agentText");
            result.reasoningText = stringMember(value, "reasoningText");
            result.reasoningSummary = stringMember(value, "reasoningSummary");
            result.commandOutput = stringMember(value, "commandOutput");
            result.droppedContentBytes = optionalUnsigned(value, "droppedContentBytes");
            result.contentTruncated = optionalBool(value, "contentTruncated").value_or(false);
            result.startedAtMs = optionalInteger(value, "startedAtMs");
            result.completedAtMs = optionalInteger(value, "completedAtMs");
            if (const auto data = value.find("data"); data != value.end())
                result.data = *data;
            result.truncated = optionalBool(value, "truncated").value_or(false);
            if (const auto omitted = value.find("omittedFields"); omitted != value.end()) {
                if (!omitted->is_array()) {
                    error = "legacy item omittedFields must be an array";
                    return false;
                }
                for (const frontend::Json& field : *omitted) {
                    if (!field.is_string()) {
                        error = "legacy item omittedFields must contain strings";
                        return false;
                    }
                    result.omittedFields.push_back(field.get<std::string>());
                }
            }
            result.connectionInvalidated = optionalBool(value, "connectionInvalidated").value_or(false);
            if (const auto stamp = value.find("stamp"); stamp != value.end()) {
                SourceStamp decoded;
                if (!decodeStamp(*stamp, decoded, error))
                    return false;
                result.stamp = decoded;
            } else {
                const auto generation = optionalUnsigned(value, "generation");
                const auto freshnessName = stringMember(value, "freshness");
                const auto freshness = freshnessName ? frontend::stateFreshnessFromString(*freshnessName) : std::nullopt;
                if (generation && freshness)
                    result.stamp = SourceStamp{*generation, *freshness};
            }
            if (const auto extensions = value.find("extensions"); extensions != value.end()) {
                if (!extensions->is_object()) {
                    error = "legacy item extensions must be an object";
                    return false;
                }
                result.extensions = *extensions;
            }
            const frontend::Json unknown = extensionsOf(value,
                                                        {"id",
                                                         "type",
                                                         "kind",
                                                         "threadId",
                                                         "turnId",
                                                         "status",
                                                         "summary",
                                                         "location",
                                                         "agentText",
                                                         "reasoningText",
                                                         "reasoningSummary",
                                                         "commandOutput",
                                                         "droppedContentBytes",
                                                         "contentTruncated",
                                                         "startedAtMs",
                                                         "completedAtMs",
                                                         "data",
                                                         "truncated",
                                                         "omittedFields",
                                                         "connectionInvalidated",
                                                         "stamp",
                                                         "generation",
                                                         "freshness",
                                                         "extensions"});
            for (auto member = unknown.begin(); member != unknown.end(); ++member)
                result.extensions[member.key()] = member.value();
            return true;
        }

        bool decodeExpandedItem(const frontend::Json& value, ItemState& result, std::string& error) {
            frontend::ExpandedFrontendEvent synthetic{
                frontend::SequenceNumber{1}, frontend::ExpandedEventType::ItemUpserted, frontend::Json::object(), frontend::Json::object()};
            synthetic.data["item"] = value;
            const auto encoded = frontend::Codec::encodeExpandedEvent(synthetic);
            if (!encoded) {
                error = "invalid expanded item: " + encoded.error().message;
                return false;
            }
            const auto decoded = frontend::Codec::decodeExpandedEvent(encoded.value());
            if (!decoded) {
                error = "invalid expanded item: " + decoded.error().message;
                return false;
            }
            const auto itemObject = decoded.value().data.find("item");
            if (itemObject == decoded.value().data.end()) {
                error = "expanded item wrapper is missing item";
                return false;
            }
            // Codec's snapshot path exposes typed items; decode this validated
            // object directly without retaining the wire object as state.
            const auto kindName = stringMember(*itemObject, "type");
            const auto kind = kindName ? frontend::threadItemKindFromString(*kindName) : std::nullopt;
            const auto id = stringMember(*itemObject, "id");
            if (!kind || !id || id->empty()) {
                error = "expanded item lacks a valid id or type";
                return false;
            }
            result.id = typed::ItemId{*id};
            result.kind = *kind;
            if (const auto threadId = stringMember(*itemObject, "threadId"))
                result.threadId = typed::ThreadId{*threadId};
            if (const auto turnId = stringMember(*itemObject, "turnId"))
                result.turnId = typed::TurnId{*turnId};
            result.status = stringMember(*itemObject, "status");
            result.summary = stringMember(*itemObject, "summary");
            if (const auto location = itemObject->find("location"); location != itemObject->end())
                result.location = *location;
            result.agentText = stringMember(*itemObject, "agentText");
            result.reasoningText = stringMember(*itemObject, "reasoningText");
            result.reasoningSummary = stringMember(*itemObject, "reasoningSummary");
            result.commandOutput = stringMember(*itemObject, "commandOutput");
            result.droppedContentBytes = optionalUnsigned(*itemObject, "droppedContentBytes");
            result.contentTruncated = optionalBool(*itemObject, "contentTruncated").value_or(false);
            result.startedAtMs = optionalInteger(*itemObject, "startedAtMs");
            result.completedAtMs = optionalInteger(*itemObject, "completedAtMs");
            if (const auto data = itemObject->find("data"); data != itemObject->end())
                result.data = *data;
            result.truncated = optionalBool(*itemObject, "truncated").value_or(false);
            if (const auto omitted = itemObject->find("omittedFields"); omitted != itemObject->end()) {
                for (const frontend::Json& member : *omitted)
                    result.omittedFields.push_back(member.get<std::string>());
            }
            result.connectionInvalidated = optionalBool(*itemObject, "connectionInvalidated").value_or(false);
            const auto generation = optionalUnsigned(*itemObject, "generation");
            const auto freshnessName = stringMember(*itemObject, "freshness");
            const auto freshness = freshnessName ? frontend::stateFreshnessFromString(*freshnessName) : std::nullopt;
            if (generation && freshness)
                result.stamp = SourceStamp{*generation, *freshness};
            result.extensions = extensionsOf(*itemObject,
                                             {"id",
                                              "type",
                                              "threadId",
                                              "turnId",
                                              "status",
                                              "summary",
                                              "location",
                                              "agentText",
                                              "reasoningText",
                                              "reasoningSummary",
                                              "commandOutput",
                                              "droppedContentBytes",
                                              "contentTruncated",
                                              "startedAtMs",
                                              "completedAtMs",
                                              "data",
                                              "truncated",
                                              "omittedFields",
                                              "connectionInvalidated",
                                              "generation",
                                              "freshness"});
            return true;
        }

        PendingRequestState decodePendingRequest(const frontend::ExpandedPendingRequest& value) {
            PendingRequestState result;
            result.id = PendingRequestId{value.pendingRequestId};
            result.kind = value.kind;
            if (value.threadId)
                result.threadId = typed::ThreadId{*value.threadId};
            if (value.turnId)
                result.turnId = typed::TurnId{*value.turnId};
            if (value.itemId)
                result.itemId = typed::ItemId{*value.itemId};
            result.summary = value.summary;
            result.opaqueDetails = value.details;
            if (value.questions) {
                result.questions.emplace();
                for (const frontend::ExpandedPendingRequestQuestion& question : *value.questions) {
                    PendingRequestQuestionState decoded;
                    decoded.id = question.id;
                    decoded.header = question.header;
                    decoded.prompt = question.prompt;
                    decoded.allowsFreeText = question.allowsFreeText;
                    decoded.isSecret = question.isSecret;
                    decoded.extensions = question.extensions;
                    for (const frontend::ExpandedPendingRequestOption& option : question.options) {
                        decoded.options.push_back({option.label, option.description, option.extensions});
                    }
                    result.questions->push_back(std::move(decoded));
                }
            }
            result.autoResolutionMs = value.autoResolutionMs;
            result.truncated = value.truncated;
            if (const auto omitted = value.extensions.find("omittedFields"); omitted != value.extensions.end() && omitted->is_array()) {
                for (const frontend::Json& field : *omitted)
                    result.omittedFields.push_back(field.get<std::string>());
            }
            result.extensions = extensionsOf(value.extensions, {"omittedFields"});
            return result;
        }

        std::optional<frontend::PendingRequestKind> legacyPendingKind(std::string_view value) noexcept {
            if (const auto direct = frontend::pendingRequestKindFromString(value))
                return direct;
            if (value == "command_approval")
                return frontend::PendingRequestKind::CommandExecutionApproval;
            if (value == "file_change_approval")
                return frontend::PendingRequestKind::FileChangeApproval;
            if (value == "apply_patch_approval")
                return frontend::PendingRequestKind::ApplyPatchApproval;
            if (value == "exec_command_approval")
                return frontend::PendingRequestKind::ExecCommandApproval;
            if (value == "permissions_approval")
                return frontend::PendingRequestKind::PermissionsApproval;
            if (value == "dynamic_tool_call")
                return frontend::PendingRequestKind::DynamicToolCall;
            if (value == "mcp_elicitation")
                return frontend::PendingRequestKind::McpElicitation;
            return std::nullopt;
        }

        std::optional<frontend::Json> sanitizeLegacyPendingDetails(const frontend::Json& value, std::string& error) {
            frontend::detail::CanonicalEventRecord record;
            record.sequence = frontend::SequenceNumber{1};
            record.legacyType = "request.pending";
            record.legacyData.value = value;
            record = frontend::detail::canonicalizeEvent(std::move(record));
            if (record.sanitization.failed || !record.legacyData.value.is_object()) {
                error = "legacy pending request details could not be bounded and sanitized";
                return std::nullopt;
            }
            return std::optional<frontend::Json>{std::move(record.legacyData.value)};
        }

        bool decodePendingRequestJson(const frontend::Json& value, PendingRequestState& result, std::string& error, bool strictExpanded) {
            if (!requireObject(value, "pending request", error))
                return false;
            auto id = stringMember(value, "pendingRequestId");
            if (!id)
                id = stringMember(value, "id");
            auto kindName = stringMember(value, "kind");
            if (!kindName)
                kindName = stringMember(value, "type");
            const auto kind = kindName ? legacyPendingKind(*kindName) : std::nullopt;
            if (!id || id->empty() || !kind) {
                error = "pending request lacks a valid id or kind";
                return false;
            }
            result.id = PendingRequestId{*id};
            result.kind = *kind;
            if (const auto threadId = stringMember(value, "threadId"))
                result.threadId = typed::ThreadId{*threadId};
            if (const auto turnId = stringMember(value, "turnId"))
                result.turnId = typed::TurnId{*turnId};
            if (const auto itemId = stringMember(value, "itemId"))
                result.itemId = typed::ItemId{*itemId};
            result.summary = stringMember(value, "summary");
            if (const auto details = value.find("details"); details != value.end()) {
                if (strictExpanded) {
                    result.opaqueDetails = *details;
                } else {
                    auto sanitized = sanitizeLegacyPendingDetails(*details, error);
                    if (!sanitized)
                        return false;
                    result.opaqueDetails = std::move(*sanitized);
                }
            }
            result.autoResolutionMs = optionalUnsigned(value, "autoResolutionMs");
            result.truncated = optionalBool(value, "truncated").value_or(false);
            result.connectionInvalidated = optionalBool(value, "connectionInvalidated").value_or(false);
            if (const auto omitted = value.find("omittedFields"); omitted != value.end() && omitted->is_array()) {
                for (const frontend::Json& field : *omitted) {
                    if (!field.is_string()) {
                        error = "pending request omittedFields must contain strings";
                        return false;
                    }
                    result.omittedFields.push_back(field.get<std::string>());
                }
            }
            if (const auto questions = value.find("questions"); questions != value.end()) {
                if (!questions->is_array()) {
                    error = "pending request questions must be an array";
                    return false;
                }
                result.questions.emplace();
                for (const frontend::Json& question : *questions) {
                    PendingRequestQuestionState decoded;
                    const auto questionId = stringMember(question, "id");
                    const auto header = stringMember(question, "header");
                    const auto prompt = stringMember(question, "prompt");
                    const auto allows = optionalBool(question, "allowsFreeText");
                    auto secret = optionalBool(question, "isSecret");
                    if (!secret)
                        secret = optionalBool(question, "secret");
                    const auto options = question.find("options");
                    if (!questionId || !header || !prompt || !allows || !secret || options == question.end() || !options->is_array()) {
                        error = "pending request question lacks required presentation fields";
                        return false;
                    }
                    decoded.id = *questionId;
                    decoded.header = *header;
                    decoded.prompt = *prompt;
                    decoded.allowsFreeText = *allows;
                    decoded.isSecret = *secret;
                    decoded.extensions =
                        extensionsOf(question, {"id", "header", "prompt", "allowsFreeText", "isSecret", "secret", "options"});
                    for (const frontend::Json& option : *options) {
                        const auto label = stringMember(option, "label");
                        const auto description = stringMember(option, "description");
                        if (!label || !description) {
                            error = "pending request option lacks label or description";
                            return false;
                        }
                        decoded.options.push_back({*label, *description, extensionsOf(option, {"label", "description"})});
                    }
                    result.questions->push_back(std::move(decoded));
                }
            }
            if (strictExpanded && result.kind == frontend::PendingRequestKind::UserInput && !result.questions) {
                error = "user-input pending request lacks questions";
                return false;
            }
            result.extensions = extensionsOf(value,
                                             {"pendingRequestId",
                                              "id",
                                              "kind",
                                              "type",
                                              "threadId",
                                              "turnId",
                                              "itemId",
                                              "summary",
                                              "details",
                                              "questions",
                                              "autoResolutionMs",
                                              "truncated",
                                              "omittedFields",
                                              "connectionInvalidated"});
            return true;
        }

        bool validateOptionalString(const frontend::Json& value, std::string_view key, std::string& error) {
            return optionalMemberHasType(value, key, frontend::Json::value_t::string, error);
        }

        bool validateOptionalInteger(const frontend::Json& value, std::string_view key, std::string& error) {
            const auto found = value.find(std::string(key));
            if (found == value.end())
                return true;
            if (found->is_number_integer())
                return true;
            if (found->is_number_unsigned() &&
                found->get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                return true;
            error = "field '" + std::string(key) + "' must be a signed 64-bit integer";
            return false;
        }

        bool validateLegacyUserMessageData(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy user-message data", error))
                return false;
            const auto clientId = value.find("clientId");
            if (clientId == value.end() || (!clientId->is_string() && !clientId->is_null())) {
                error = "legacy user-message data clientId must be a string or null";
                return false;
            }
            const auto content = value.find("content");
            if (content == value.end() || !content->is_array()) {
                error = "legacy user-message data content must be an array";
                return false;
            }
            return requireBoolMember(value, "contentTruncated", error) && requireUnsignedMember(value, "originalContentBytes", error) &&
                   requireUnsignedMember(value, "retainedContentBytes", error) &&
                   requireUnsignedMember(value, "originalContentItems", error) &&
                   requireUnsignedMember(value, "retainedContentItems", error);
        }

        bool validateLegacyItemSchema(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy item", error) || !requireStringMember(value, "id", true, error) ||
                !requireStringMember(value, "type", true, error) || !requireStringMember(value, "status", false, error) ||
                !requireStringMember(value, "agentText", false, error) || !requireStringMember(value, "reasoningText", false, error) ||
                !requireStringMember(value, "reasoningSummary", false, error) ||
                !requireStringMember(value, "commandOutput", false, error) || !requireUnsignedMember(value, "droppedContentBytes", error) ||
                !requireBoolMember(value, "contentTruncated", error))
                return false;

            const auto type = stringMember(value, "type");
            const auto status = stringMember(value, "status");
            if (!status || (*status != "unknown" && *status != "started" && *status != "completed" && *status != "failed")) {
                error = "legacy item status is not a defined stable value";
                return false;
            }
            const auto data = value.find("data");
            const auto extensions = value.find("extensions");
            if (data == value.end() || !data->is_object() || extensions == value.end() || !extensions->is_object()) {
                error = "legacy item data and extensions must be objects";
                return false;
            }
            if (*type == "user_message" && !validateLegacyUserMessageData(*data, error))
                return false;
            return validateOptionalInteger(value, "startedAtMs", error) && validateOptionalInteger(value, "completedAtMs", error);
        }

        bool validateLegacyTurnSchema(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy turn", error) || !requireStringMember(value, "id", true, error) ||
                !requireStringMember(value, "threadId", true, error) || !requireStringMember(value, "status", false, error) ||
                !requireBoolMember(value, "active", error) || !requireBoolMember(value, "terminal", error))
                return false;
            const auto items = value.find("items");
            const auto extensions = value.find("extensions");
            if (items == value.end() || !items->is_array() || extensions == value.end() || !extensions->is_object()) {
                error = "legacy turn items must be an array and extensions must be an object";
                return false;
            }
            for (const frontend::Json& item : *items) {
                if (!validateLegacyItemSchema(item, error))
                    return false;
            }
            return true;
        }

        bool validateLegacyThreadSchema(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy thread", error) || !requireStringMember(value, "id", true, error) ||
                !requireBoolMember(value, "fullyLoaded", error))
                return false;
            const auto turns = value.find("turns");
            const auto extensions = value.find("extensions");
            if (turns == value.end() || !turns->is_array() || extensions == value.end() || !extensions->is_object()) {
                error = "legacy thread turns must be an array and extensions must be an object";
                return false;
            }
            for (const frontend::Json& turn : *turns) {
                if (!validateLegacyTurnSchema(turn, error))
                    return false;
            }
            for (const std::string_view key : {"title", "cwd", "model", "modelProvider", "preview", "status"}) {
                if (!validateOptionalString(value, key, error))
                    return false;
            }
            return validateOptionalInteger(value, "createdAt", error) && validateOptionalInteger(value, "updatedAt", error);
        }

        bool validateLegacyPendingRequestSchema(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy pending request", error) || !requireDecimalIdMember(value, "id", error) ||
                !requireStringMember(value, "type", true, error))
                return false;
            const auto type = stringMember(value, "type");
            if (!type || (*type != "command_approval" && *type != "file_change_approval" && *type != "user_input" &&
                          *type != "authentication" && *type != "unknown")) {
                error = "legacy pending request type is not a defined stable value";
                return false;
            }
            const auto details = value.find("details");
            if (details == value.end() || !details->is_object()) {
                error = "legacy pending request details must be an object";
                return false;
            }
            return validateOptionalString(value, "threadId", error) && validateOptionalString(value, "turnId", error) &&
                   validateOptionalString(value, "itemId", error);
        }

        bool validateLegacyErrorSnapshot(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy error snapshot", error) || !requireStringMember(value, "category", false, error) ||
                !requireStringMember(value, "message", false, error) || !validateOptionalInteger(value, "code", error) ||
                value.find("code") == value.end())
                return false;
            const auto category = stringMember(value, "category");
            constexpr std::array<std::string_view, 9> categories{
                "launch", "transport", "protocol", "initialization", "process", "invalid_state", "capacity", "cancelled", "enqueue"};
            if (!category || std::find(categories.begin(), categories.end(), *category) == categories.end()) {
                error = "legacy error snapshot category is not a defined stable value";
                return false;
            }
            return true;
        }

        bool validateLegacyExtensionTruncation(const frontend::Json& value, std::string& error) {
            if (!requireObject(value, "legacy extension truncation", error))
                return false;
            for (auto member = value.begin(); member != value.end(); ++member) {
                if (member.key() != "originalBytes" && member.key() != "retainedBytes") {
                    error = "legacy extension truncation contains an unknown field";
                    return false;
                }
                if (!optionalUnsigned(value, member.key())) {
                    error = "legacy extension truncation byte counts must be unsigned integers";
                    return false;
                }
            }
            return true;
        }

        bool validateLegacyEvent(const frontend::FrontendEvent& event, std::string& error) {
            const frontend::Json& data = event.data;
            if (!requireObject(data, "legacy event data", error))
                return false;
            if (event.type == "backend.lifecycle.changed") {
                const auto lifecycle = stringMember(data, "lifecycle");
                constexpr std::array<std::string_view, 6> values{"stopped", "starting", "initializing", "ready", "stopping", "failed"};
                if (!lifecycle || std::find(values.begin(), values.end(), *lifecycle) == values.end()) {
                    error = "legacy provider lifecycle is not a defined stable value";
                    return false;
                }
                const auto eventError = data.find("error");
                return eventError == data.end() || validateLegacyErrorSnapshot(*eventError, error);
            }
            if (event.type == "diagnostics.updated") {
                if (!requireUnsignedMember(data, "received", error))
                    return false;
                const auto recent = data.find("recent");
                if (recent == data.end() || !recent->is_array()) {
                    error = "legacy diagnostics update recent must be an array";
                    return false;
                }
                for (const frontend::Json& message : *recent) {
                    if (!message.is_string()) {
                        error = "legacy diagnostics update recent must contain strings";
                        return false;
                    }
                }
                return true;
            }
            if (event.type == "thread.updated") {
                const frontend::Json* thread = nullptr;
                return requireObjectMember(data, "thread", thread, error) && validateLegacyThreadSchema(*thread, error);
            }
            if (event.type == "thread.list.updated") {
                if (!requireBoolMember(data, "hasLoadedPage", error) || !requireBoolMember(data, "complete", error) ||
                    !requireUnsignedMember(data, "pagesLoaded", error))
                    return false;
                return validateOptionalString(data, "nextCursor", error) && validateOptionalString(data, "backwardsCursor", error);
            }
            if (event.type == "turn.updated") {
                const frontend::Json* turn = nullptr;
                return requireObjectMember(data, "turn", turn, error) && validateLegacyTurnSchema(*turn, error);
            }
            if (event.type == "item.updated") {
                const frontend::Json* item = nullptr;
                return requireStringMember(data, "threadId", true, error) && requireStringMember(data, "turnId", true, error) &&
                       requireObjectMember(data, "item", item, error) && validateLegacyItemSchema(*item, error);
            }
            if (event.type == "item.content.updated") {
                if (!requireStringMember(data, "threadId", true, error) || !requireStringMember(data, "turnId", true, error) ||
                    !requireStringMember(data, "itemId", true, error) || !requireStringMember(data, "channel", true, error) ||
                    !requireStringMember(data, "content", false, error) || !requireBoolMember(data, "contentTruncated", error) ||
                    !requireUnsignedMember(data, "droppedContentBytes", error))
                    return false;
                const auto channel = stringMember(data, "channel");
                if (*channel != "agentText" && *channel != "reasoningText" && *channel != "reasoningSummary" &&
                    *channel != "commandOutput") {
                    error = "legacy item content channel is not a defined stable value";
                    return false;
                }
                return true;
            }
            if (event.type == "request.pending") {
                const frontend::Json* request = nullptr;
                return requireObjectMember(data, "request", request, error) && validateLegacyPendingRequestSchema(*request, error);
            }
            if (event.type == "request.resolved")
                return requireDecimalIdMember(data, "pendingRequestId", error) && requireStringMember(data, "reason", false, error);
            if (event.type == "controller.changed") {
                const auto id = data.find("controllerSessionId");
                return id == data.end() || requireDecimalIdMember(data, "controllerSessionId", error);
            }
            if (event.type == "session.changed") {
                if (!requireDecimalIdMember(data, "sessionId", error) || !requireBoolMember(data, "connected", error) ||
                    !requireStringMember(data, "role", true, error))
                    return false;
                const auto role = stringMember(data, "role");
                if (*role != "observer" && *role != "controller") {
                    error = "legacy session role is not a defined stable value";
                    return false;
                }
                return true;
            }
            if (event.type == "codex.extension") {
                if (!requireStringMember(data, "method", true, error) || data.find("params") == data.end() ||
                    !validateOptionalString(data, "decodingError", error))
                    return false;
                const auto redacted = data.find("sensitiveFieldsRedacted");
                if (redacted != data.end() && !redacted->is_boolean()) {
                    error = "legacy extension sensitiveFieldsRedacted must be a boolean";
                    return false;
                }
                const auto truncation = data.find("truncation");
                if (truncation == data.end())
                    return true;
                if (!truncation->is_object()) {
                    error = "legacy extension truncation must be an object";
                    return false;
                }
                for (auto member = truncation->begin(); member != truncation->end(); ++member) {
                    if (member.key() != "method" && member.key() != "params" && member.key() != "decodingError") {
                        error = "legacy extension truncation contains an unknown channel";
                        return false;
                    }
                    if (!validateLegacyExtensionTruncation(member.value(), error))
                        return false;
                }
                return true;
            }
            error = "unknown legacy frontend event type '" + event.type + "'";
            return false;
        }

        bool validateLegacySnapshotSchema(const frontend::Json& state, std::string& error) {
            if (!requireObject(state, "legacy snapshot state", error) || !requireUnsignedMember(state, "backendRevision", error) ||
                !requireStringMember(state, "lifecycle", true, error) || !requireUnsignedMember(state, "omittedCodexExtensions", error) ||
                !requireBoolMember(state, "sequenceExhausted", error))
                return false;

            const auto lifecycle = stringMember(state, "lifecycle");
            constexpr std::array<std::string_view, 6> lifecycleValues{"stopped", "starting", "initializing", "ready", "stopping", "failed"};
            if (!lifecycle || std::find(lifecycleValues.begin(), lifecycleValues.end(), *lifecycle) == lifecycleValues.end()) {
                error = "legacy snapshot lifecycle is not a defined stable value";
                return false;
            }
            if (const auto exhausted = state.find("frontendSequenceExhausted"); exhausted != state.end() && !exhausted->is_boolean()) {
                error = "legacy snapshot frontendSequenceExhausted must be a boolean";
                return false;
            }
            if (const auto controller = state.find("controllerSessionId");
                controller != state.end() && !requireDecimalIdMember(state, "controllerSessionId", error))
                return false;
            if (const auto controller = state.find("controller"); controller != state.end()) {
                if (controller->is_string()) {
                    if (controller->get_ref<const std::string&>().empty()) {
                        error = "legacy snapshot controller string must not be empty";
                        return false;
                    }
                } else if (!controller->is_number_unsigned() && !controller->is_object()) {
                    error = "legacy snapshot controller must be a string, unsigned integer, or object";
                    return false;
                }
                if (controller->is_object()) {
                    if (controller->contains("controllerSessionId") &&
                        !requireStringMember(*controller, "controllerSessionId", true, error))
                        return false;
                    if (controller->contains("present") && !requireBoolMember(*controller, "present", error))
                        return false;
                }
            }
            if (const auto lastError = state.find("lastLifecycleError");
                lastError != state.end() && !validateLegacyErrorSnapshot(*lastError, error))
                return false;

            const auto diagnostics = state.find("diagnostics");
            if (diagnostics == state.end() || !diagnostics->is_object() ||
                !validateLegacyEvent(frontend::FrontendEvent{frontend::SequenceNumber{1}, "diagnostics.updated", *diagnostics}, error))
                return false;

            const auto threads = state.find("threads");
            if (threads == state.end() || !threads->is_array()) {
                error = "legacy snapshot threads must be an array";
                return false;
            }
            for (const frontend::Json& thread : *threads) {
                if (!validateLegacyThreadSchema(thread, error))
                    return false;
            }

            if (const auto items = state.find("items"); items != state.end() && !items->is_array()) {
                error = "legacy snapshot complete-thread-items projection must be an array";
                return false;
            }

            const auto requests = state.find("pendingRequests");
            if (requests == state.end() || !requests->is_array()) {
                error = "legacy snapshot pendingRequests must be an array";
                return false;
            }
            for (const frontend::Json& request : *requests) {
                if (!validateLegacyPendingRequestSchema(request, error))
                    return false;
            }

            const auto sessions = state.find("sessions");
            if (sessions == state.end() || !sessions->is_array()) {
                error = "legacy snapshot sessions must be an array";
