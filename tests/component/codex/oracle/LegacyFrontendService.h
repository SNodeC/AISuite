/*
 * Test-only P3 legacy oracle declaration.
 *
 * The frozen implementation is compiled with the same private renaming so it
 * can coexist with the production FrontendService cutover.  No declaration in
 * this file is installed or used by a production target.
 */

#ifndef TESTS_COMPONENT_CODEX_ORACLE_LEGACYFRONTENDSERVICE_H
#define TESTS_COMPONENT_CODEX_ORACLE_LEGACYFRONTENDSERVICE_H

#define FrontendConnection LegacyFrontendConnection
#define FrontendService LegacyFrontendService
#define FrontendServiceTestAccess LegacyFrontendServiceTestAccess
#include "ai/openai/codex/frontend/FrontendService.h"
#include "ai/openai/codex/frontend/detail/FrontendServiceTestAccess.h"
#undef FrontendServiceTestAccess
#undef FrontendService
#undef FrontendConnection

namespace ai::openai::codex::frontend::oracle {
    using namespace ai::openai::codex::frontend;
    using FrontendConnection = LegacyFrontendConnection;
    using FrontendService = LegacyFrontendService;
    using FrontendServiceTestAccess = LegacyFrontendServiceTestAccess;
} // namespace ai::openai::codex::frontend::oracle

#endif // TESTS_COMPONENT_CODEX_ORACLE_LEGACYFRONTENDSERVICE_H
