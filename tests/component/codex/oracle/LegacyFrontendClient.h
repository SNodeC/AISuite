/*
 * Test-only P3 legacy oracle declaration.
 *
 * Client sources are compiled in a renamed private namespace.  The namespace
 * alias deliberately exists only in old-vs-new differential translation
 * units, which never include the cut-over public client declarations.
 */

#ifndef TESTS_COMPONENT_CODEX_ORACLE_LEGACYFRONTENDCLIENT_H
#define TESTS_COMPONENT_CODEX_ORACLE_LEGACYFRONTENDCLIENT_H

#define client legacy_client
#include "ai/openai/codex/frontend/client/Client.h"
#include "ai/openai/codex/frontend/client/GeneratedBindings.h"
#include "ai/openai/codex/frontend/client/detail/StateReducer.h"
#undef client

namespace ai::openai::codex::frontend {
    namespace client = legacy_client;
} // namespace ai::openai::codex::frontend

#endif // TESTS_COMPONENT_CODEX_ORACLE_LEGACYFRONTENDCLIENT_H
