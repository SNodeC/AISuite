# `@snodec/codex-frontend`

Framework-neutral TypeScript frontend SDK for the AISuite `codex-bridge`.

The generated protocol declarations come from the same pinned Codex schema
and Rust operation bindings as AISuite's C++ views. Regenerate both outputs in
one invocation:

```sh
node tools/generate-codex-protocol.mjs \
  /path/to/codex_app_server_protocol.schemas.json \
  /path/to/app-server-protocol/src/protocol/common.rs \
  src/ai/openai/codex/protocol/generated/ProtocolTypes.h \
  src/ai/openai/codex/protocol/generated/manifest.json \
  packages/codex-frontend/src/protocol/generated.ts
```

`npm test --prefix packages/codex-frontend` builds the declarations and proves
that the checked-in C++ and TypeScript type names, operation bindings,
required-parameter flags, counts, and source hashes remain equal.
