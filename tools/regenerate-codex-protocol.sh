#!/usr/bin/env bash

set -euo pipefail

readonly CODEX_RELEASE="rust-v0.154.0"
readonly CODEX_REVISION="6b9826e3aa83b1a5947db50f4332cb9c65f1b340"
readonly CODEX_REMOTE="https://github.com/openai/codex.git"

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
temporary_root="$(mktemp -d)"
trap 'rm -rf -- "$temporary_root"' EXIT

codex_source="$temporary_root/codex"
git init -q "$codex_source"
git -C "$codex_source" remote add origin "$CODEX_REMOTE"
git -C "$codex_source" sparse-checkout init --cone
git -C "$codex_source" sparse-checkout set codex-rs/app-server-protocol
git -C "$codex_source" fetch -q --filter=blob:none --depth 1 origin \
  "refs/tags/$CODEX_RELEASE"
resolved_revision="$(git -C "$codex_source" rev-parse 'FETCH_HEAD^{commit}')"
if [[ "$resolved_revision" != "$CODEX_REVISION" ]]; then
  printf 'Codex release %s resolved to %s, expected %s\n' \
    "$CODEX_RELEASE" "$resolved_revision" "$CODEX_REVISION" >&2
  exit 1
fi
git -C "$codex_source" checkout -q --detach FETCH_HEAD

protocol_root="$codex_source/codex-rs/app-server-protocol"
schema_path="$protocol_root/schema/json/codex_app_server_protocol.schemas.json"
mkdir -p "$(dirname "$schema_path")"
zstd -q -d -c \
  "$protocol_root/schema/precomputed/app-server-exports-experimental.json.zst" \
  > "$temporary_root/exports.json"
python3 - "$temporary_root/exports.json" "$schema_path" <<'PY'
import json
import pathlib
import sys

exports = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
schema = exports["json_schema"]["codex_app_server_protocol.schemas.json"]
pathlib.Path(sys.argv[2]).write_text(schema, encoding="utf-8")
PY

generated_root="$temporary_root/generated"
mkdir -p "$generated_root"
export CODEX_PROTOCOL_RELEASE="$CODEX_RELEASE"
export CODEX_PROTOCOL_REVISION="$CODEX_REVISION"
export CODEX_PROTOCOL_EXPERIMENTAL=1
node "$repository_root/tools/generate-codex-protocol.mjs" \
  "$schema_path" \
  "$protocol_root/src/protocol/common.rs" \
  "$generated_root/ProtocolTypes.h" \
  "$generated_root/manifest.json" \
  "$generated_root/generated.ts"

declare -a generated=(
  "ProtocolTypes.h:src/ai/openai/codex/protocol/generated/ProtocolTypes.h"
  "manifest.json:src/ai/openai/codex/protocol/generated/manifest.json"
  "generated.ts:packages/codex-frontend/src/protocol/generated.ts"
)

if [[ "${1:-}" == "--check" ]]; then
  changed=0
  for mapping in "${generated[@]}"; do
    source="${mapping%%:*}"
    destination="${mapping#*:}"
    if ! cmp -s "$generated_root/$source" "$repository_root/$destination"; then
      printf 'generated Codex protocol drift: %s\n' "$destination" >&2
      changed=1
    fi
  done
  exit "$changed"
fi

if [[ $# -ne 0 ]]; then
  printf 'usage: %s [--check]\n' "$0" >&2
  exit 2
fi

for mapping in "${generated[@]}"; do
  source="${mapping%%:*}"
  destination="${mapping#*:}"
  install -m 0644 "$generated_root/$source" "$repository_root/$destination"
done
