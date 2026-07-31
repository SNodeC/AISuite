# Codex App Server protocol tooling

AISuite's checked-in Codex protocol authority is `codex-cli 0.144.6`, release
`rust-v0.144.6`. The stable and experimental schemas, the vendored Rust
request-association source, and their protocol-version metadata live under
`tools/codex/` and are validated offline.

This protocol pin is independent of the installed SNode.C build dependency.
AISuite builds against the current installed SNode.C package and does not use
a historical SNode.C checkout for protocol or extraction validation.

## Authoritative inputs

- `app-server-schema/0.144.6/stable/` contains the stable schemas.
- `app-server-schema/0.144.6/experimental/` contains the experimental census.
- `app-server-schema/0.144.6/PROVENANCE.json` records the protocol release and
  deterministic vendored-file hashes.
- `app-server-protocol-source/0.144.6/` contains the reviewed Rust macro source
  used for client request/result association.
- `app-server-surface/0.144.6.json` is the complete generated surface census.
- `src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc` is the generated
  data for the sole production implementation-state registry.

The OpenAI Codex release attribution, source tag, and license/NOTICE hashes are
part of the vendored protocol-version record. They are not an AISuite or
SNode.C repository-history contract.

Ordinary checks use only checked-in inputs. They do not invoke Codex, access
the network, or require credentials.

## Current surface and descriptor checks

Run these commands from the repository root:

```sh
SCHEMA_ROOT=tools/codex/app-server-schema/0.144.6
PROVENANCE="$SCHEMA_ROOT/PROVENANCE.json"
SURFACE=tools/codex/app-server-surface/0.144.6.json
REGISTRY=src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc
EVIDENCE_ROOT=tools/codex/app-server-evidence/0.144.6
FIXTURE_ROOT=tools/codex/app-server-fixtures/0.144.6

python3 tools/codex/app_server_surface.py verify \
  --schema-root "$SCHEMA_ROOT" \
  --provenance "$PROVENANCE" \
  --manifest "$SURFACE"

python3 tools/codex/app_server_surface.py extract \
  --schema-root "$SCHEMA_ROOT" \
  --output /tmp/aisuite-codex-surface.json
cmp /tmp/aisuite-codex-surface.json "$SURFACE"

python3 tools/codex/app_server_surface.py registry \
  --manifest "$SURFACE" \
  --evidence-root "$EVIDENCE_ROOT" \
  --output "$REGISTRY" \
  --check

python3 tools/codex/app_server_surface.py operation-descriptors \
  --manifest "$SURFACE" \
  --evidence-root "$EVIDENCE_ROOT" \
  --output src/ai/openai/codex/detail/ClientOperationCodecDescriptors.inc \
  --check

python3 tools/codex/app_server_surface.py notification-descriptors \
  --manifest "$SURFACE" \
  --evidence-root "$EVIDENCE_ROOT" \
  --output src/ai/openai/codex/detail/ServerNotificationCodecDescriptors.inc \
  --check

python3 tools/codex/app_server_surface.py conversation-descriptors \
  --manifest "$SURFACE" \
  --schema-root "$SCHEMA_ROOT" \
  --evidence-root "$EVIDENCE_ROOT" \
  --output src/ai/openai/codex/detail/ConversationUnionCodecDescriptors.inc \
  --check

python3 tools/codex/app_server_surface.py integrations-and-long-tail-union-descriptors \
  --manifest "$SURFACE" \
  --schema-root "$SCHEMA_ROOT" \
  --evidence-root "$EVIDENCE_ROOT" \
  --output src/ai/openai/codex/detail/IntegrationsAndLongTailUnionCodecDescriptors.inc \
  --check

python3 tools/codex/app_server_surface.py item-descriptors \
  --manifest "$SURFACE" \
  --schema-root "$SCHEMA_ROOT" \
  --evidence-root "$EVIDENCE_ROOT" \
  --thread-output src/ai/openai/codex/detail/ThreadItemCodecDescriptors.inc \
  --response-output src/ai/openai/codex/detail/ResponseItemCodecDescriptors.inc \
  --check

python3 tools/codex/app_server_surface.py docs \
  --manifest "$SURFACE" \
  --registry "$REGISTRY" \
  --provenance "$PROVENANCE" \
  --coverage-output docs/ai/openai/codex/app-server-api-coverage.md \
  --security-output docs/ai/openai/codex/app-server-security-decisions.md \
  --check
```

`verify` checks the vendored protocol-version inputs and their attribution.
`extract` resolves repository-local schema references, rejects unsupported
schema layouts, and emits a deterministic sorted surface. The registry and
descriptor commands compare current generated data with current production
sources; they do not inspect Git history.

## Contracts, fixtures, and production coverage

Client request/result associations are not present in the JSON schemas. The
contract tool derives all stable client associations from the vendored Rust
`client_request_definitions!` source and all stable server-request pairs from
the schema tree:

```sh
SOURCE_ROOT=tools/codex/app-server-protocol-source/0.144.6

python3 tools/codex/app_server_contracts.py \
  --source-root "$SOURCE_ROOT" \
  --schema-root "$SCHEMA_ROOT" \
  --manifest "$SURFACE" \
  --schema-provenance "$PROVENANCE" \
  --evidence-root "$EVIDENCE_ROOT" \
  --check

python3 tools/codex/app_server_fixtures.py check \
  --schema-root "$SCHEMA_ROOT" \
  --manifest "$SURFACE" \
  --contracts "$EVIDENCE_ROOT/operation-contracts.json" \
  --fixture-root "$FIXTURE_ROOT" \
  --evidence-root "$EVIDENCE_ROOT"

python3 tools/codex/app_server_fixtures.py validate \
  --schema-root "$SCHEMA_ROOT" \
  --manifest "$SURFACE" \
  --contracts "$EVIDENCE_ROOT/operation-contracts.json" \
  --fixture-root "$FIXTURE_ROOT" \
  --evidence-root "$EVIDENCE_ROOT"

python3 tools/codex/app_server_surface.py operation-production-coverage \
  --manifest "$SURFACE" \
  --evidence-root "$EVIDENCE_ROOT" \
  --fixture-index "$FIXTURE_ROOT/index.json" \
  --repo-root . \
  --output "$EVIDENCE_ROOT/a1-1-operation-production-coverage.json" \
  --check

python3 tools/codex/app_server_surface.py notification-production-coverage \
  --manifest "$SURFACE" \
  --evidence-root "$EVIDENCE_ROOT" \
  --fixture-index "$FIXTURE_ROOT/index.json" \
  --repo-root . \
  --output "$EVIDENCE_ROOT/a1-1-notification-production-coverage.json" \
  --check
```

The fixture validator is independent of the production codecs. Production
coverage guards then bind indexed fixture cases to the same structured result
and notification dispatchers used by the typed API. `generate` replaces
`check` only after an intentional, reviewed change to a vendored protocol
input or deterministic generation rule.

## Current A1 software-state checks

The production authority is the current `ProtocolSurfaceRegistry`; generated
descriptor tables bind its typed targets to current codecs. The compact
current-state tests are:

```sh
python3 -B tests/component/codex/CodexA11A13CurrentStateTest.py \
  --repo-root .
python3 -B tests/component/codex/CodexA14RuntimePlatformCurrentStateTest.py \
  --repo-root .
```

They read current source, generated descriptors, the production registry, and
vendored schemas. Together they verify A1.1--A1.4 taxonomy, implementation
dispositions, descriptor/target agreement, schema roots, A1.4 runtime/platform
reachability and property coverage, public-header inventory, InventoryOnly
status, and SOVERSION. They do not inspect commits, pull requests, merge
topology, repository SHAs, or a historical dependency checkout, and they do
not create milestone reports.

CTest also exercises the codecs, wire framing, typed facades, notification
ordering, request occurrence lifecycle, public headers, installed consumers,
packages, and security/logging policy against the current tree.

## Regenerating the vendored protocol pin

A deliberate Codex protocol-version update uses that version's `codex`
executable to create duplicate stable and experimental schema generations and
the stable and experimental TypeScript trees. Review the generated schema
semantics and attribution before using `app_server_surface.py provenance` and
`extract` to prepare a new version directory. Do not overwrite `0.144.6` with
output from another version.

The TypeScript trees are an independent method/discriminator cross-check;
they are not production association authority and are not vendored in bulk.
The vendored Rust macro remains authoritative for client result association.

## Generated and reviewed sources

Do not hand-edit the vendored schema trees or these generated current-state
artifacts:

- `app-server-schema/0.144.6/PROVENANCE.json`;
- `app-server-surface/0.144.6.json`;
- `app-server-protocol-source/0.144.6/PROVENANCE.json`;
- `app-server-evidence/0.144.6/operation-contracts.json`;
- `app-server-evidence/0.144.6/typescript-audit.json`;
- `app-server-evidence/0.144.6/module-slice-assignment.json`;
- `app-server-evidence/0.144.6/nested-reachability.json`;
- `app-server-evidence/0.144.6/schema-completeness-evidence.json`;
- `app-server-evidence/0.144.6/fixture-coverage.json`;
- `app-server-evidence/0.144.6/schema-keywords.json`;
- `app-server-evidence/0.144.6/a1-1-type-closure.json`;
- `app-server-evidence/0.144.6/a1-1-operation-production-coverage.json`;
- `app-server-evidence/0.144.6/a1-1-notification-production-coverage.json`;
- `app-server-fixtures/0.144.6/`;
- `ClientOperationCodecDescriptors.inc`;
- `ServerNotificationCodecDescriptors.inc`;
- `ConversationUnionCodecDescriptors.inc`;
- `IntegrationsAndLongTailUnionCodecDescriptors.inc`;
- `ThreadItemCodecDescriptors.inc`;
- `ResponseItemCodecDescriptors.inc`;
- `ProtocolSurfaceRegistryData.inc`;
- `docs/ai/openai/codex/app-server-api-coverage.md`; and
- `docs/ai/openai/codex/app-server-security-decisions.md`.

`app_server_surface.py` discovers the protocol inventory and contains the
explicit reviewed mapping from discovered entries to current local runtime and
architectural dispositions. The generated `.inc` files provide the single
inventory/descriptor data sources consumed by production C++. Registration is
not implementation: deferred, raw-preserved, opaque-preserved, and
owner-decision-required entries do not count as typed support.
