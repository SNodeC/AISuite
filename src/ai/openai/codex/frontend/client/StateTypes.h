#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_STATETYPES_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_STATETYPES_H

#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/typed/Types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ai::openai::codex::frontend::client {

    template <typename T>
    struct Projected {
        std::optional<T> value;
        bool truncated = false;
        std::vector<std::string> omittedFields;
    };

    struct ProviderState {
        std::string lifecycle;
        std::uint64_t generation = 0;
        bool ready = false;
    };

    struct ControllerState {
        std::optional<std::string> sessionId;
        bool ownedByThisClient = false;
    };

    struct ThreadState {
        std::string id;
        std::optional<std::string> title;
        std::optional<std::string> preview;
        std::optional<std::string> cwd;
        std::optional<std::string> model;
        std::optional<std::string> modelProvider;
        std::optional<std::string> status;
        bool fullyLoaded = false;
        std::vector<std::string> orderedTurns;
        frontend::Json extensions = frontend::Json::object();
    };

    struct TurnState {
        std::string id;
        std::string threadId;
        std::optional<std::string> status;
        bool active = false;
        bool terminal = false;
        bool connectionInvalidated = false;
        std::vector<std::string> orderedItems;
        frontend::Json failure = nullptr;
        frontend::Json tokenUsage = nullptr;
    };

    struct ItemState {
        std::string id;
        std::optional<std::string> turnId;
        std::string kind;
        std::optional<std::string> status;
        std::optional<std::string> agentText;
        std::optional<std::string> reasoningText;
        std::optional<std::string> reasoningSummary;
        std::optional<std::string> commandOutput;
        std::size_t truncatedBytes = 0;
        bool connectionInvalidated = false;
        frontend::Json opaque = frontend::Json::object();
    };

    struct PendingRequestState {
        std::uint64_t id = 0;
        std::string kind;
        bool connectionInvalidated = false;
        frontend::Json presentation = frontend::Json::object();
    };

    struct DomainState {
        frontend::Json value = frontend::Json::object();
    };

    struct DiagnosticState {
        std::string message;
        frontend::Json details = frontend::Json::object();
    };

} // namespace ai::openai::codex::frontend::client

#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_STATETYPES_H
