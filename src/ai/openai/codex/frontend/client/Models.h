#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_MODELS_H
#define AI_OPENAI_CODEX_FRONTEND_CLIENT_MODELS_H
namespace ai::openai::codex::frontend::client {
    class Client;
}
#include "ai/openai/codex/frontend/client/detail/DeclareFacade.h"
namespace ai::openai::codex::frontend::client {
    AISUITE_DECLARE_CODEX_FRONTEND_CLIENT_FACADE(Models);
}
#undef AISUITE_DECLARE_CODEX_FRONTEND_CLIENT_FACADE
#endif
