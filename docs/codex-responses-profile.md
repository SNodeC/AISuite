# Codex Responses compatibility profile: 0.154.0

Target: upstream `openai/codex` release **0.154.0**, commit
`6b9826e3aa83b1a5947db50f4332cb9c65f1b340`, also pinned by AISuite's generated
app-server protocol. A newer Codex release must be rechecked, not assumed compatible.

Primary source inspected:

- [request construction](https://github.com/openai/codex/blob/6b9826e3aa83b1a5947db50f4332cb9c65f1b340/codex-rs/core/src/client.rs)
- [request structs](https://github.com/openai/codex/blob/6b9826e3aa83b1a5947db50f4332cb9c65f1b340/codex-rs/codex-api/src/common.rs)
- [SSE parser](https://github.com/openai/codex/blob/6b9826e3aa83b1a5947db50f4332cb9c65f1b340/codex-rs/codex-api/src/sse/responses.rs)
- [output/history items](https://github.com/openai/codex/blob/6b9826e3aa83b1a5947db50f4332cb9c65f1b340/codex-rs/protocol/src/models.rs)
- [model-provider configuration](https://github.com/openai/codex/blob/6b9826e3aa83b1a5947db50f4332cb9c65f1b340/codex-rs/model-provider-info/src/lib.rs)
- [tool serialization](https://github.com/openai/codex/blob/6b9826e3aa83b1a5947db50f4332cb9c65f1b340/codex-rs/tools/src/responses_api.rs)
- [model catalog](https://github.com/openai/codex/blob/6b9826e3aa83b1a5947db50f4332cb9c65f1b340/codex-rs/protocol/src/openai_models.rs)

## Request endpoint

Authenticated `POST /responses`, JSON, `stream:true`, `store:false`, complete input
history. The child is configured with `wire_api="responses"`, local `base_url`,
`env_key="AISUITE_MODEL_TOKEN"`, `requires_openai_auth=false` and
`supports_websockets=false`. No OpenAI login or upstream patch is necessary.

Implemented semantic fields:

- `model`, `instructions`, `input` array of text messages;
- leading system/developer messages folded into neutral instructions;
- function/custom calls and corresponding outputs with unchanged `call_id`;
- text tool outputs, including arrays of input/output text blocks;
- function/custom tool declarations and namespace groups;
- auto/none/required tool choice, parallel tool calls, maximum output tokens.

Codex always sends `include:["reasoning.encrypted_content"]`, even with reasoning
disabled. This optional enrichment request is accepted; there are no reasoning
items to enrich. Null/empty reasoning controls and `effort/summary:"none"` are
accepted. Other reasoning controls/items are rejected. Null/empty `text` and
`stream_options` are accepted. Default/auto service tier is accepted. Codex's
`prompt_cache_key` and `client_metadata` are advisory identifiers and are not
forwarded to the provider: they promise neither caching nor provider telemetry.
They do not carry continuation state. Unknown top-level semantic fields fail.

Images, non-text outputs, server tools, deferred tool loading, forced named tools,
strict tool constrained decoding, structured-output/verbosity settings, nondefault service tiers, stateful/stored
response references and Responses Lite items fail explicitly. `POST /responses/compact`
and all other endpoints return 404, not a fabricated compacted conversation.

## Output events

The service emits SSE with event/type agreement and monotonic `sequence_number`:

1. `response.created` with local response ID;
2. `response.output_item.added` for assistant messages/function/custom calls;
3. text content-part start and `response.output_text.delta`, or
   `response.function_call_arguments.delta` / `response.custom_tool_call_input.delta`;
4. `response.output_text.done` for text, then `response.output_item.done` containing
   the complete authoritative item;
5. `response.completed` with ID, output, model, usage and `end_turn` (false for tools).

Assistant message items carry an explicit `phase`. Text streams immediately as
`commentary`; the trailing text item's `output_item.done` waits until another
block begins (commentary) or successful inference completion establishes that no
tools are pending (`final_answer`). Tool-continuation text remains commentary,
including text after a tool call. Failed/truncated inference never promotes text
to a final answer. Both the done item and completed output retain the same phase.
This is essential for CodexUI's existing final-answer/update visibility policy;
omitting phase makes a valid response disappear when updates are hidden.

Codex 0.154's SSE parser consumes text/custom-input deltas and added/done items;
it **does not consume function-argument deltas**. Therefore complete function
arguments on `output_item.done` are essential. Function deltas are nevertheless
provided for coherent Responses streaming. Namespaces, names, IDs and custom raw
input survive the adapter's neutral representation. Custom input is incrementally
unescaped, including fragmented Unicode surrogate pairs; JSON wrappers never
leak into Codex's freeform `apply_patch` input.

Usage is input+cache-read+cache-write, output, total and cached-token details. No
Anthropic thinking tokens are fabricated as OpenAI reasoning details. Native
output-limit/pause stops fail as incomplete rather than fake successful turns.

Errors terminate once via `response.failed`: nonretryable invalid provider/request
semantics use Codex's recognized `invalid_prompt` error; retryable rate-limit and
server failures use `rate_limit_exceeded` and `server_is_overloaded`. The native
error code/message remains in the diagnostic. HTTP validation/authentication
errors occur before streaming with 400/401/etc. A disconnect cancels native inference;
a truncated Anthropic stream does not emit `response.completed`.

## Native reference semantics

The Anthropic implementation follows the native
[Messages streaming lifecycle](https://platform.claude.com/docs/en/api/messages-streaming),
[tool-use contract](https://platform.claude.com/docs/en/agents-and-tools/tool-use/define-tools)
and [thinking continuation semantics](https://platform.claude.com/docs/en/build-with-claude/extended-thinking).
The neutral interface is not based on these wire objects or Responses objects.
