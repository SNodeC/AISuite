# Implementation and verification record

Date: 2026-09-19. Branch: `feature/agent-provider-anthropic`, based on clean
`master` at `24b1d16c`. No upstream Codex files or CodexUI files were changed.
Changes are staged on that branch. The attempted commit could not use the
configured GPG signer because its agent/temporary files are outside the writable
sandbox. No unsigned commit was substituted and the signing configuration was
not changed.

**Status: live Claude coding turn passed through the CodexUI-facing interface.**
After socket/network access and a user-supplied credential became available, the
production bridge and actual Codex 0.154.0 completed a coding turn against
`claude-sonnet-5` over the native Anthropic HTTPS Messages API. The test uses the
same frontend contract as CodexUI; interactive graphical CodexUI acceptance is
still outstanding. No credential was written to repository files.

### CodexUI final-answer visibility correction

The user's 14:27:18 UTC turn completed successfully and persisted 71 characters
of assistant text, but the Responses adapter omitted message `phase`. CodexUI's
`NodeGraphUiAdapter` derives final-answer identity from that field and
`ConversationItemModel::isPresented` hides non-final messages when updates are
disabled. The violated invariant was that a completed answer must retain its
final-answer identity across the Codex protocol boundary, not merely stream text.

The Codex adapter now derives phase from neutral block/stop events. It reuses its
existing output storage and defers only the trailing text item's completion until
its phase can be resolved; text deltas still stream immediately. No provider
business logic, UI policy, conversation cache, or parallel state owner was added.
Earlier/intermediate text is commentary; the last text of a successful response
without tool calls is final. The direct OpenAI path is unchanged.

The new regression failed before the correction. After rebuilding the bridge,
all eight focused tests and the real Codex catalog test passed. Coverage includes
single/multiple text blocks, text before/after tools, end-turn/stop-sequence/refusal,
output limits and pauses, with consistent done/completed phases. The process test
now requires a completed `agentMessage` with `phase: final_answer`, rather than
merely some text deltas. The environment again restricts sockets, so its rerun
was an explicit skip; the corrected graphical/live path is not yet reverified.
This follow-up adds 16/removes 1 production lines (net +15) and adds 47 test lines.
Growth resolves the absent protocol semantics at their owner without adding
new retained state; the earlier broad line-growth approval applies.

## Repository analysis and implementation path

The existing architecture was traced before production changes:

- `StdioAppServer` owns `posix_spawn`, asynchronous pipes and child lifecycle;
  `WebSocketAppServer` supports externally owned app-server connections.
- Backend/frontend `CodexBridge` implement typed JSON-RPC over the generated
  protocol and transport envelopes. App-server owns thread, turn, tool, event
  and persistence semantics. AISuite owns routing and pending request callbacks.
- The `openai` namespace contained Codex integration, not an existing native
  OpenAI HTTP/model layer. No reusable neutral LLM provider contract existed.
- HTTP client/server, transfer decoding and connection/queue/lifetime machinery
  existed in SNode.C. Its high-level HTTP client buffers bodies, while EventSource
  is a reconnecting GET abstraction, not a native streaming Messages POST client.
- nlohmann-json was already the project's JSON facility. Existing Codex auth and
  configuration belong to the app-server; new provider auth belongs to AISuite.
- CodexUI uses AISuite's frontend facade and already carries `modelProvider` and
  `model` settings. Its wire contract did not need a breaking change.
- Both the generated AISuite schema and installed Codex binary target 0.154.0.
  The exact pinned request builder, SSE parser, model-provider configuration,
  model catalog and tool serializer were inspected (links in the profile).

The implementation sequence was neutral contracts/native protocol, native streaming
transport, scoped Responses codec/service, owned-runtime startup/catalog/config,
normalized agent facade, and boundary/regression testing. Existing routing,
JSON-RPC, transport classes, child ownership and UI envelopes were reused.

Important decisions:

1. No N-by-M adapters or Responses-shaped generic model API. The Codex adapter
   knows only the neutral provider; Anthropic knows nothing about Codex.
2. No proxy for the existing OpenAI path and no new conversation-state authority.
3. Thinking is disabled on the compatibility path, not lossy-translated. Native
   callers have opaque signed continuation support and explicit stop reasons.
4. Model registration is provider-owned; a temporary catalog adapts it to Codex
   discovery. Only the local credential, not the Anthropic key, enters the child.
5. HTTP is event-loop-driven. SNode.C framing is reused; fixed-length bodies need
   a bounded incremental reader because its existing Identity decoder cannot be
   drained mid-read. Byte-level tests exercise the actual production parser.
6. The CMake transport dependencies had been incorrectly conditional on building
   apps/tests despite being linked by the always-built Codex library. This directly
   blocked the library-only provider build; discovery now follows actual target
   dependencies. No unrelated networking or upstream cleanup was performed.

## Verification performed

All builds used GCC 16.2, C++20, Debug, the installed SNode.C package, existing
warning-as-error settings and at most eight parallel build jobs.

| Check | Result |
| --- | --- |
| Full application/library build, Anthropic enabled | Passed, no compiler warnings |
| Anthropic-disabled application build | Passed |
| Library-only build, apps/tests disabled | Passed |
| Install to isolated prefix, independent CMake consumer linking new targets | Configured, built and ran |
| Focused CTest suite | **8/8 passed** |
| Native request/events, thinking replay, tool aliases/results, error/usage/cancel tests | Passed |
| Mock provider and adapter-to-native mock HTTP continuation tests | Passed |
| HTTP parser: byte-fragmented fixed-length, chunked/trailer, close-delimited, malformed/truncated bodies | Passed |
| Actual Codex stdio catalog/model discovery/Anthropic thread creation | Passed without OpenAI login or model inference |
| AddressSanitizer on five new C++ test targets | **5/5 passed**, including a full-access rerun without disabling leak detection |
| Anthropic-disabled existing focused regression suite | **4/4 passed** |
| Existing TypeScript SDK/parity tests | **3/3 passed** |
| Configuration diagnostics and failed-listener exit behavior | Checked; missing model/key and denied listen return nonzero, no key in logs |
| Full CTest suite, full-access rerun | **31 passed, 2 failed** out of 33; both existing Unix path-length failures pass when rerun with short socket paths |
| Real Codex + local mock native API coding turn | Passed: four inferences, three real tools, changed file, passing unit test and streamed final answer |
| Real Claude coding turn | Passed with `claude-sonnet-5`: three real tools, changed file, passing unit test and streamed final answer |
| Interactive CodexUI live coding turn | Not run |

The initial restricted-environment run had 11 passes, 10 skips and 12 failures,
matching the pre-change baseline's 12 failing existing transport tests. After
socket access was enabled, all but two passed. Those two Unix fixtures exceed
the existing 107-byte socket-path limit in the long build directory. Both original
test executables passed with identical test modes and short temporary socket
paths; no existing tests were weakened or changed.

Initially LeakSanitizer exited fatally under the environment's tracing
restrictions. The address checks were then run with
`ASAN_OPTIONS=detect_leaks=0`; no ASan address errors occurred. This is **not** a
claim of verified leak freedom. After full access was enabled, all five passed
again with default ASan/LSan behavior (no `ASAN_OPTIONS` override). No sanitizer
suppression was committed.

The first newly enabled process test exposed a harness-only defect: frontend
JSON-RPC requests omitted `jsonrpc: "2.0"`. The harness now sends the required
field; production request validation was not relaxed. Acceptance assertions also
verify that Codex read the original faulty source, ran `python3 -m unittest -q`
successfully through its own tool, and left the test source unchanged, in addition
to the independent post-turn unit-test run. The temporary project and isolated
Codex home are removed after each run; the credential was supplied through hidden
terminal input and the test subprocess environment, not command-line arguments.

## Accounting and remaining work

Production C++ changes: **+2685 / -102 lines (net +2583)**, including headers and
comments. Tests/fixtures/test CMake: **+1403 / -0 lines**. Build files and
documentation are accounted separately. Growth was explicitly approved by the
user: the repository had neither a native Messages provider, provider-neutral
inference model nor a Codex custom-model HTTP compatibility endpoint. These are
working connected components rather than alternative implementations of existing
routing or agent state.

Remaining acceptance is the same task in interactive CodexUI. The headless process
test now demonstrates actual read/patch/test tools, tool results, continued live
inference, streamed answer, changed file and successful turn completion over
HTTPS. Negative TLS peer-identity cases and network disconnect/backpressure under
load still merit dedicated host-level tests; a successful TLS request does not
prove all failure paths.

Deliberate limitations and usage are in [agent-providers.md](agent-providers.md)
and [codex-responses-profile.md](codex-responses-profile.md). In particular, this
is an owned-stdio, text/coding-tools profile with no compaction endpoint, deferred
or hosted tools, structured output, or Codex reasoning translation. Unsupported
semantics fail diagnostically. Future providers and native coding agents were not
implemented.
