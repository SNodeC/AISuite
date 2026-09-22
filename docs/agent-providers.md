# Agent runtimes and model providers

## Architectural invariant

The agent owns conversations, tools, approvals and persistence. A model provider
owns inference and provider credentials. Neither is the other's identity or data
model. In particular, Codex still owns every thread and turn; AISuite does **not**
maintain a competing conversation cache, replay database, or tool executor.

This slice evolves the existing bridge rather than replacing it:

```
CodexUI / frontend SDK -> AISuite bridge -> owned Codex app-server
                                            |                |
                                      direct OpenAI    local Responses service
                                                             |
                                                  ai::model::Provider
                                                             |
                                                  native Anthropic Messages
```

The existing `ai::openai::codex` namespace and `AISuite::OpenAICodex` target are
kept for source compatibility. Its pre-existing `provider/` directory means
**app-server transport**, not model provider. No upstream Codex changes are used.

## Components and ownership

- `AISuite::Agent`: `ai::agent::Selection` separates agent/provider/model;
  `Runtime` is the nonblocking lifecycle contract used by the bridge application.
  `Agent` is the common conversation/start-turn/interrupt/event facade.
- `frontend::CodexAgent` implements that facade using the existing frontend
  `CodexBridge`. Events normalize text, tool activity and turn completion.
  Backend-specific approval, history, resume and controller APIs remain available
  through `backend()`. It stores no conversation state. The facade exclusively
  owns its normalized notification handlers; do not replace those handlers through
  `backend()` while using the facade.
- `AISuite::Model`: neutral request, messages, content, tools, tool results,
  reasoning continuation, streaming events, usage, stop reasons, errors and RAII
  cancellation. JSON is used for **tool input/schema**, not Responses wire items.
  Provider registration (`ProviderInfo`) is AISuite-owned.
- `AISuite::Anthropic`: native Messages request construction and stream codec.
  Knows nothing about Codex or Responses. Model IDs are supplied in configuration.
- `AISuite::HttpStreaming`: bounded POST streaming on SNode.C TCP/TLS, with
  SNode.C HTTP header/chunk/trailer decoders, a bounded fixed-length body
  reader and a request-scoped SSE decoder. The existing
  SNode.C high-level client buffers response bodies; its EventSource is a
  reconnecting GET client, so neither is appropriate for a billable streaming POST.
  SNode.C's fixed-length decoder owns a pre-sized body buffer and cannot be
  drained incrementally, so that path uses a bounded remaining-length reader.
  No new networking dependency or event loop is introduced.
- Codex `model/ResponsesAdapter`: strictly scoped wire translation through the
  neutral provider interface. Custom/freeform tools use a neutral object schema
  containing an `input` string; the adapter unwraps it in the outgoing stream.
- Codex `model/ResponsesService`: authenticated loopback-only HTTP service;
  SNode.C owns listening, request parsing, response chunking and bounded output.
- Codex `model/ProviderProfile`: private temporary model catalog and supported
  custom-provider launch configuration. Model discovery comes from AISuite's
  provider registration; Codex `model/list` exposes it to unchanged CodexUI clients.

The application owns HTTP client -> Anthropic provider -> catalog/service ->
app-server runtime, with corresponding reverse lifetime teardown. The service
binds **before** the child is started. Startup, stdio JSON-RPC, HTTP and SSE all
progress independently in SNode.C's event loop. There is no nested event loop,
blocking wait for a model response, or synchronous child-to-parent callback.

`Operation::cancel()` is event-loop confined, idempotent, and suppresses later
callbacks. Operation destruction cancels too. Consumer disconnect, failed SSE
admission, terminal provider events and application stop cancel upstream work.
Slow consumers fail closed instead of accumulating an unbounded model stream.

## Configuration

Build with `AISUITE_ENABLE_ANTHROPIC=ON` (default), using an installed SNode.C with
HTTP client/server, IPv4 plain/TLS transports, OpenSSL and nlohmann-json. Set it
`OFF` to retain the original runtime without the new model HTTP/TLS dependencies.

Supply an API key only through the environment, and a model ID available to your
Anthropic account. Do not put credentials in command-line arguments or config
files checked into source control:

```sh
# ANTHROPIC_API_KEY must already be exported by your secret-management workflow.
codex-bridge codex --agent codex --model-provider anthropic \
  --model "$CLAUDE_MODEL" --codex-home "$ISOLATED_CODEX_HOME"
```

`codex` is the application's configuration subcommand. Optional settings:

| Option | Default / meaning |
| --- | --- |
| `--agent` | `codex`; only implemented runtime |
| `--model-provider` | `openai`; existing directly configured Codex path |
| `--model` | existing Codex default on direct path; **required** for Anthropic |
| `--anthropic-api-key-env` | `ANTHROPIC_API_KEY` |
| `--anthropic-base-url` | `https://api.anthropic.com`; native API root, not `/v1/messages` |
| `--model-maximum-output-tokens` | `8192`, per inference |
| `--model-context-window` | `200000`; set to the selected model's supported capacity |

The initial application registry contains the configured Anthropic model. Native
library callers may register multiple models. There is no hardcoded Claude model
identifier, provider model-list HTTP call, or dependency on Codex's OpenAI catalog.

Anthropic selection requires the **owned stdio** app-server transport. Existing
external Unix/WebSocket app-server connections remain unchanged for the direct
path; AISuite cannot safely reconfigure an independently owned app-server process.

For CodexUI, connect to the same AISuite frontend endpoint as before. Select the
configured model from `model/list`, or pass `modelProvider=anthropic` and that model
when starting a thread. A thread cannot change to an unregistered model silently.
No Anthropic, SSE or compatibility-endpoint details cross the UI boundary. Existing
rich C++/TypeScript UI APIs remain supported; migrating UI consumers to the small
normalized C++ agent facade is optional, not a broad UI redesign.

Running `codex-bridge` without the new options uses the existing Codex configuration
and authentication **directly**, without a compatibility listener or generated
catalog. Setting `--model` on this path overrides only the child model. Existing
Codex-home, transport, approval and sandbox configuration is not rewritten.

## Authentication and network policy

- The compatibility endpoint binds only IPv4 loopback, with a random 256-bit bearer
  token given to the owned child via `AISUITE_MODEL_TOKEN`, not command arguments.
- The configured Anthropic key environment variable is cleared in the child's
  inherited environment. The child receives neither the provider API key nor the
  Anthropic URL. As with any same-user process, OS process-memory inspection is
  outside this in-process separation; this is not a separate-UID security boundary.
- Native HTTPS checks both the certificate chain and DNS/IP peer identity **before**
  sending the API key. Only HTTP to `127.0.0.1:<port>` is accepted for local fixtures.
  IPv6-literal provider URLs, HTTP proxies and redirects are not implemented.
- Requests/streams are bounded at 64 MiB, SSE events at 1 MiB, HTTP headers at
  64 KiB, local concurrent streams at 16 and the local write queue at 8 MiB.
  Slow consumers are disconnected/cancelled. Native HTTP has 120-second read-idle
  and 30-second write timeouts; there is no automatic inference retry in AISuite.
  Codex retains its own retry policy. There is no overall generation deadline.
- Compression and HTTP upgrade are unsupported; requests ask for identity encoding.
  Secrets and full model prompts are not logged by the new components.

## Anthropic semantics and continuation

System instructions are separate from user/assistant messages. Consecutive same-role
messages are merged. All `tool_result` blocks lead the immediately following user
message; multiple tool-use IDs and results are preserved and checked. Streamed tool
JSON is accumulated and parsed only at content-block completion. Native invalid
names (for example namespaced tools) are reversibly aliased per request, with
stable names across declaration reordering; collisions fail rather than misroute.

Usage distinguishes uncached input, cache reads, cache writes and output; cumulative
`message_delta` counts replace, rather than increment, earlier values. Stop reasons
are explicit. Native `message_stop`, not TCP EOF, determines successful completion.
API errors, malformed content and truncated streams cannot become success.

Native provider callers may enable **manual-budget thinking** and preserve each
complete `Reasoning.continuation` verbatim when replaying assistant content. Signed
thinking and redacted blocks are opaque provider-owned state, not OpenAI reasoning.
Unsigned, modified or cross-provider state is rejected. Adaptive thinking and
model-specific beta features are not implemented.

The Codex compatibility profile deliberately sets thinking **disabled**. It does
not invent an encrypted Responses item, drop signed state, or use an in-memory
cache to make a resumed Codex thread accidentally work. Select a Claude model
that supports this configuration. Reasoning controls/items outside the documented
profile fail explicitly. All necessary non-thinking tool state is in Codex's
replayed history, including across restarts; no Responses ID lookup is needed.

## Verification and opt-in coding turn

```sh
cmake -S . -B "$BUILD_DIR" -DAISUITE_BUILD_CODEX_TESTS=ON
cmake --build "$BUILD_DIR" --parallel 8
ctest --test-dir "$BUILD_DIR" -L focused --output-on-failure
ctest --test-dir "$BUILD_DIR" -R 'CodexModelCatalogTest|CodexAnthropicCodingTurnMock' --output-on-failure
```

`CodexModelCatalogTest` uses the actual installed Codex app-server over stdio to
verify catalog loading, model discovery and Anthropic thread selection without
making a model request. It needs no Anthropic credential.

`coding_turn.py` uses the **same Unix frontend contract as CodexUI**, the production
bridge application, actual Codex 0.154.0, local Responses service and native
Anthropic provider. Its default local native-API fixture requests a real file read,
a real custom `apply_patch`, a real unit-test execution and a final answer, checking
all inference continuations and the resulting file. This headless boundary test
does not claim to launch or visually test CodexUI itself.

To run the billable real Claude variant (key must already be exported):

```sh
export AISUITE_ANTHROPIC_TEST_MODEL="$CLAUDE_MODEL"
python3 tests/model/coding_turn.py \
  --bridge "$BUILD_DIR/src/apps/codex-bridge/codex-bridge" --live
```

Or configure `-DAISUITE_ANTHROPIC_LIVE_TEST=ON` and run
`ctest --test-dir "$BUILD_DIR" -R CodexAnthropicCodingTurnLive --output-on-failure`.
Missing credentials/model, unavailable Codex or denied socket capability produce
an explicit **skip (77)**, never a fake pass. All edits run in a fresh temporary
project with isolated `CODEX_HOME`, workspace-write sandbox and never-approval;
production sandbox/approval defaults are unchanged. The test checks that Codex
used tools, patched the file, streamed its final answer and completed its turn.
For manual CodexUI acceptance, use that same isolated project/task against the
configured bridge and verify the displayed tool events and final completion.

## Deliberate scope and remaining acceptance

See [the pinned compatibility profile](codex-responses-profile.md). This is a
text/coding-tools slice, not a general Responses server: no images/audio, hosted
search, deferred tool loading, stored response IDs, Responses Lite, background
requests, compaction endpoint or structured-output constraints. Custom-tool Lark
syntax is given as instructions, not enforced by constrained decoding. Codex's
actual tool validates input. Mid-conversation system/developer changes after
assistant history are rejected rather than silently moved to global instructions.

On 2026-09-19, after network access became available, both the mock-native-API
coding turn and a real Claude coding turn with `--model claude-sonnet-5` passed.
The live path used the production bridge, actual Codex 0.154.0, native Anthropic
HTTPS/SSE and three real tools to read, patch and test a temporary project before
streaming the final answer. This is a headless test of the CodexUI-facing contract,
not graphical UI acceptance. Interactive CodexUI verification, negative TLS
identity cases and network backpressure/disconnect stress tests remain follow-up
work. The Codex compatibility path still disables thinking explicitly.

Detailed build/test outcomes and line accounting: [verification record](anthropic-verification.md).
