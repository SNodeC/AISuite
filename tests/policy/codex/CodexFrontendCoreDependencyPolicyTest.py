#!/usr/bin/env python3

"""Enforce the permanent P2 Codex frontend target and include boundaries."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from collections.abc import Iterable


class PolicyFailure(RuntimeError):
    pass


def load_json(path: pathlib.Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PolicyFailure(f"cannot read CMake File API document {path}: {error}") from error
    if not isinstance(value, dict):
        raise PolicyFailure(f"CMake File API document is not an object: {path}")
    return value


def load_link_manifest(path: pathlib.Path) -> dict[str, dict[str, set[str]]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise PolicyFailure(f"cannot read target link manifest {path}: {error}") from error
    result: dict[str, dict[str, set[str]]] = {}
    for number, line in enumerate(lines, start=1):
        fields = line.split("|", 2)
        if len(fields) != 3 or fields[1] not in {
            "LINK_LIBRARIES",
            "INTERFACE_LINK_LIBRARIES",
        }:
            raise PolicyFailure(
                f"malformed target link manifest row {path}:{number}: {line!r}"
            )
        target, property_name, encoded = fields
        properties = result.setdefault(target, {})
        if property_name in properties:
            raise PolicyFailure(
                f"duplicate target link manifest row {target}.{property_name}"
            )
        properties[property_name] = {
            entry for entry in encoded.split(";") if entry
        }
    return result


def require_link_manifest(
    actual: dict[str, dict[str, set[str]]],
    expected: dict[str, set[str]],
) -> None:
    if set(actual) != set(expected):
        raise PolicyFailure(
            "target link manifest inventory mismatch: "
            f"expected={sorted(expected)}, actual={sorted(actual)}"
        )
    for target, expected_links in expected.items():
        properties = actual[target]
        for property_name in ("LINK_LIBRARIES", "INTERFACE_LINK_LIBRARIES"):
            links = properties.get(property_name)
            if links != expected_links:
                raise PolicyFailure(
                    f"{target}.{property_name} mismatch: "
                    f"expected={sorted(expected_links)}, "
                    f"actual={None if links is None else sorted(links)}"
                )


def file_api_targets(build_dir: pathlib.Path) -> dict[str, dict[str, object]]:
    reply_dir = build_dir / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply_dir.glob("index-*.json"))
    if not indexes:
        raise PolicyFailure(
            "CMake File API reply is missing; configure with the committed "
            "codemodel-v2 query before running this policy"
        )
    index = load_json(indexes[-1])
    reply = index.get("reply")
    if not isinstance(reply, dict):
        raise PolicyFailure("CMake File API index has no reply object")
    codemodel_ref = reply.get("codemodel-v2")
    if not isinstance(codemodel_ref, dict) or not isinstance(codemodel_ref.get("jsonFile"), str):
        raise PolicyFailure("CMake File API codemodel-v2 reply is absent")
    codemodel = load_json(reply_dir / codemodel_ref["jsonFile"])
    configurations = codemodel.get("configurations")
    if not isinstance(configurations, list) or len(configurations) != 1 or not isinstance(configurations[0], dict):
        raise PolicyFailure("dependency policy requires exactly one CMake configuration")
    references = configurations[0].get("targets")
    if not isinstance(references, list):
        raise PolicyFailure("CMake codemodel target inventory is absent")
    result: dict[str, dict[str, object]] = {}
    for reference in references:
        if not isinstance(reference, dict):
            continue
        name = reference.get("name")
        json_file = reference.get("jsonFile")
        if isinstance(name, str) and isinstance(json_file, str):
            result[name] = load_json(reply_dir / json_file)
    return result


def target_dependencies(targets: dict[str, dict[str, object]], name: str) -> set[str]:
    target = targets.get(name)
    if target is None:
        raise PolicyFailure(f"required CMake target is absent: {name}")
    ids: dict[str, str] = {}
    for candidate_name, candidate in targets.items():
        candidate_id = candidate.get("id")
        if isinstance(candidate_id, str):
            ids[candidate_id] = candidate_name
    dependencies = target.get("dependencies", [])
    if not isinstance(dependencies, list):
        raise PolicyFailure(f"target dependencies are malformed: {name}")
    return {
        ids[dependency_id]
        for dependency in dependencies
        if isinstance(dependency, dict)
        and isinstance((dependency_id := dependency.get("id")), str)
        and dependency_id in ids
    }


def dependency_closure(targets: dict[str, dict[str, object]], name: str) -> set[str]:
    if name not in targets:
        raise PolicyFailure(f"required CMake target is absent: {name}")
    closure: set[str] = set()
    pending = list(target_dependencies(targets, name))
    while pending:
        dependency = pending.pop()
        if dependency in closure:
            continue
        closure.add(dependency)
        pending.extend(target_dependencies(targets, dependency) - closure)
    return closure


def require_dependency_closure(
    targets: dict[str, dict[str, object]],
    name: str,
    required: Iterable[str],
    forbidden: Iterable[str],
) -> None:
    actual = dependency_closure(targets, name)
    missing = sorted(set(required) - actual)
    rejected = sorted(set(forbidden) & actual)
    if missing or rejected:
        raise PolicyFailure(
            f"{name} transitive dependency mismatch: "
            f"missing={missing}, forbidden-present={rejected}, actual={sorted(actual)}"
        )


def require_dependencies(
    targets: dict[str, dict[str, object]],
    name: str,
    required: Iterable[str],
    allowed: Iterable[str],
) -> None:
    actual = target_dependencies(targets, name)
    required_set = set(required)
    allowed_set = set(allowed)
    if not required_set <= allowed_set:
        raise PolicyFailure(
            f"{name} dependency policy is malformed: "
            f"required-not-allowed={sorted(required_set - allowed_set)}"
        )
    missing = sorted(required_set - actual)
    unexpected = sorted(actual - allowed_set)
    if missing or unexpected:
        raise PolicyFailure(
            f"{name} direct in-project dependency mismatch: "
            f"missing={missing}, unexpected={unexpected}, actual={sorted(actual)}, "
            f"allowed={sorted(allowed_set)}"
        )


def reject_dependency_patterns(
    targets: dict[str, dict[str, object]], name: str, forbidden_patterns: Iterable[str]
) -> None:
    actual = dependency_closure(targets, name)
    rejected = sorted(
        dependency
        for dependency in actual
        if any(re.search(pattern, dependency, re.IGNORECASE) for pattern in forbidden_patterns)
    )
    if rejected:
        raise PolicyFailure(
            f"{name} transitive dependency closure contains forbidden targets {rejected}"
        )


def target_link_fragments(targets: dict[str, dict[str, object]], name: str) -> list[str]:
    target = targets.get(name)
    if target is None:
        raise PolicyFailure(f"required CMake target is absent: {name}")
    link = target.get("link")
    if link is None:
        return []
    if not isinstance(link, dict) or not isinstance(link.get("commandFragments", []), list):
        raise PolicyFailure(f"target link command is malformed: {name}")
    result: list[str] = []
    for row in link.get("commandFragments", []):
        # Global LDFLAGS are emitted for every target with role "flags";
        # only library-role fragments describe the target's linked libraries.
        if (
            isinstance(row, dict)
            and row.get("role") == "libraries"
            and isinstance(row.get("fragment"), str)
        ):
            result.append(row["fragment"])
    return result


def reject_link_fragments(
    targets: dict[str, dict[str, object]], name: str, forbidden_patterns: Iterable[str]
) -> None:
    fragments = "\n".join(target_link_fragments(targets, name))
    rejected = sorted(pattern for pattern in forbidden_patterns if re.search(pattern, fragments, re.IGNORECASE))
    if rejected:
        raise PolicyFailure(
            f"{name} link closure contains forbidden patterns {rejected}: {fragments!r}"
        )


INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)

SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".inc"}

TRANSPORT_INCLUDE_PREFIXES = (
    "snodec/",
    "core/",
    "net/",
    "web/",
    "http/",
    "websocket/",
    "tls/",
    "rfcomm/",
    "openssl/",
    "bluetooth/",
    "Qt",
    "qt/",
    "curses.h",
    "ncurses.h",
    "ncurses/",
)


def target_source_paths(
    targets: dict[str, dict[str, object]],
    name: str,
    source_dir: pathlib.Path,
    build_dir: pathlib.Path,
) -> list[pathlib.Path]:
    target = targets.get(name)
    if target is None:
        raise PolicyFailure(f"required CMake target is absent: {name}")
    sources = target.get("sources")
    if not isinstance(sources, list):
        raise PolicyFailure(f"target source inventory is malformed: {name}")

    result: list[pathlib.Path] = []
    for row in sources:
        if not isinstance(row, dict) or not isinstance(row.get("path"), str):
            raise PolicyFailure(f"target source entry is malformed: {name}: {row!r}")
        path = pathlib.Path(row["path"])
        candidates = [path] if path.is_absolute() else [source_dir / path, build_dir / path]
        resolved = next(
            (candidate.resolve() for candidate in candidates if candidate.is_file()), None
        )
        if resolved is None:
            raise PolicyFailure(f"target source is absent: {name}: {path.as_posix()}")
        result.append(resolved)
    if not result:
        raise PolicyFailure(f"target source inventory is empty: {name}")
    return result


def source_files(source_dir: pathlib.Path, roots: Iterable[pathlib.Path]) -> list[pathlib.Path]:
    result: set[pathlib.Path] = set()
    for root in roots:
        resolved = root.resolve() if root.is_absolute() else (source_dir / root).resolve()
        if not resolved.exists():
            raise PolicyFailure(f"include-policy input is absent: {resolved}")
        paths = [resolved] if resolved.is_file() else resolved.rglob("*")
        result.update(
            path.resolve()
            for path in paths
            if path.is_file() and path.suffix in SOURCE_SUFFIXES
        )
    return sorted(result)


def source_label(source_dir: pathlib.Path, path: pathlib.Path) -> str:
    try:
        return path.relative_to(source_dir).as_posix()
    except ValueError:
        return path.as_posix()


def reject_includes(source_dir: pathlib.Path, roots: Iterable[pathlib.Path], forbidden: Iterable[str]) -> None:
    failures: list[str] = []
    prefixes = tuple(forbidden)
    for path in source_files(source_dir, roots):
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            raise PolicyFailure(f"cannot inspect include closure {path}: {error}") from error
        for included in INCLUDE.findall(text):
            if included.startswith(prefixes):
                failures.append(f"{source_label(source_dir, path)}: {included}")
    if failures:
        raise PolicyFailure("forbidden include closure:\n  " + "\n  ".join(failures))


def read_source(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise PolicyFailure(f"cannot inspect source policy input {path}: {error}") from error


def require_server_core_reentrancy_policy(source_dir: pathlib.Path) -> None:
    paths = [
        source_dir / "src/ai/openai/codex/frontend/internal/server/ServerCore.h",
        source_dir / "src/ai/openai/codex/frontend/internal/server/ServerCore.cpp",
    ]
    forbidden = ("recursive_mutex", "std::mutex", "lock_guard")
    failures = [
        f"{source_label(source_dir, path)}: {token}"
        for path in paths
        for token in forbidden
        if token in read_source(path)
    ]
    if failures:
        raise PolicyFailure(
            "ServerCore must remain owner-event-loop confined without mutex-based reentrancy:\n  "
            + "\n  ".join(failures)
        )

    implementation = read_source(paths[1])
    snapshot_calls = list(re.finditer(r"\bbackend[.]snapshot\s*\(\s*\)", implementation))
    unscoped_snapshot_calls = [
        implementation.count("\n", 0, match.start()) + 1
        for match in snapshot_calls
        if "BackendSnapshotScope snapshotScope(*this);" not in implementation[max(0, match.start() - 256) : match.start()]
    ]
    if implementation.count("class BackendSnapshotScope") != 1 or not snapshot_calls or unscoped_snapshot_calls:
        raise PolicyFailure(
            "every BackendPort snapshot callback boundary must remain under BackendSnapshotScope: "
            f"calls={len(snapshot_calls)}, unscoped-lines={unscoped_snapshot_calls}"
        )


def require_exact_entity_lookup_policy(source_dir: pathlib.Path) -> None:
    path = source_dir / "src/ai/openai/codex/frontend/internal/server/BackendProjection.cpp"
    text = read_source(path)
    begin_marker = "// BEGIN exact-entity-lookup-policy"
    end_marker = "// END exact-entity-lookup-policy"
    if text.count(begin_marker) != 1 or text.count(end_marker) != 1:
        raise PolicyFailure("BackendProjection exact-entity lookup policy markers are absent or ambiguous")
    begin = text.index(begin_marker) + len(begin_marker)
    end = text.index(end_marker, begin)
    block = text[begin:end]
    family_end = text.index("const generated::ProjectionMetadata* notificationProjection", end)
    entity_projection_block = text[begin:family_end]

    required_helpers = (
        "findThread(",
        "findTurn(",
        "findItem(",
        "findProcess(",
        "findFilesystemWatch(",
        "findFuzzySearch(",
        "findActivity(",
    )
    missing = [helper for helper in required_helpers if helper not in block]
    required_signatures = {
        "findThread strong identity": r"findThread\s*\([^)]*const\s+model::ThreadIdentity\s*&",
        "findTurn strong parent and identity":
            r"findTurn\s*\([^)]*const\s+model::ThreadIdentity\s*&[^)]*const\s+model::TurnIdentity\s*&",
        "findItem strong parents and identity":
            r"findItem\s*\([^)]*const\s+model::ThreadIdentity\s*&[^)]*const\s+model::TurnIdentity\s*&"
            r"[^)]*const\s+model::ItemIdentity\s*&",
        "findProcess strong handle": r"findProcess\s*\([^)]*const\s+model::ProcessHandle\s*&",
        "findFilesystemWatch required key": r"findFilesystemWatch\s*\([^)]*std::string_view\s+watchId",
        "findFuzzySearch required key": r"findFuzzySearch\s*\([^)]*std::string_view\s+sessionId",
        "findActivity required key": r"findActivity\s*\([^)]*std::string_view\s+key",
    }
    missing_signatures = [
        name for name, pattern in required_signatures.items() if re.search(pattern, block, flags=re.DOTALL) is None
    ]
    helper_forbidden_patterns = {
        "optional strong-identity selector": re.compile(
            r"std::optional\s*<\s*(?:model::(?:ThreadIdentity|TurnIdentity|ItemIdentity|ProcessHandle)|std::string(?:_view)?)\s*>"
        ),
        "fallback selector value": re.compile(r"\bvalue_or\s*\("),
    }
    entity_forbidden_patterns = {
        "first/last entity fallback": re.compile(r"[.](?:front|back)\s*\("),
        "first-match entity lookup": re.compile(r"\bstd::find_if\b"),
        "absent-selector wildcard predicate": re.compile(
            r"\breturn\s*\([^;]*!\s*(?:selection[.])?"
            r"(?:threadId|turnId|itemId|processHandle|filesystemWatchId|fuzzySearchId|activityKey)\s*\|\|"
        ),
    }
    rejected = [name for name, pattern in helper_forbidden_patterns.items() if pattern.search(block)]
    rejected.extend(name for name, pattern in entity_forbidden_patterns.items() if pattern.search(entity_projection_block))
    # Each of the seven entity helpers must reject a second match instead of
    # returning the first retained entity with that identity.
    unique_match_guards = block.count("if (found != nullptr)")
    if missing or missing_signatures or rejected or unique_match_guards != len(required_helpers):
        raise PolicyFailure(
            "BackendProjection exact-entity lookup policy mismatch: "
            f"missing={missing}, missing-signatures={missing_signatures}, rejected={rejected}, "
            f"unique-match-guards={unique_match_guards}/{len(required_helpers)}"
        )


def require_include_allowlist(
    source_dir: pathlib.Path,
    roots: Iterable[pathlib.Path],
    namespace: str,
    allowed_files: Iterable[str],
    allowed_prefixes: Iterable[str],
) -> None:
    exact = set(allowed_files)
    prefixes = tuple(allowed_prefixes)
    failures: list[str] = []
    for path in source_files(source_dir, roots):
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            raise PolicyFailure(f"cannot inspect include closure {path}: {error}") from error
        for included in INCLUDE.findall(text):
            if (
                included.startswith(namespace)
                and included not in exact
                and not included.startswith(prefixes)
            ):
                failures.append(f"{source_label(source_dir, path)}: {included}")
    if failures:
        raise PolicyFailure(
            f"unapproved {namespace} include closure:\n  " + "\n  ".join(failures)
        )


def require_target_type(targets: dict[str, dict[str, object]], name: str, expected: str) -> None:
    target = targets.get(name)
    if target is None or target.get("type") != expected:
        actual = None if target is None else target.get("type")
        raise PolicyFailure(f"{name} type is {actual!r}; expected {expected}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--link-manifest", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    source_dir = arguments.source_dir.resolve()
    build_dir = arguments.build_dir.resolve()
    targets = file_api_targets(build_dir)

    require_server_core_reentrancy_policy(source_dir)
    require_exact_entity_lookup_policy(source_dir)

    protocol = "ai-openai-codex-frontend-protocol"
    model = "ai-openai-codex-frontend-model"
    server = "ai-openai-codex-frontend-server-core"
    client = "ai-openai-codex-frontend-client-core"
    codex = "ai-openai-codex"
    backend = "ai-openai-codex-backend"
    old_server = "ai-openai-codex-frontend"
    old_client = "ai-openai-codex-frontend-client"

    require_link_manifest(
        load_link_manifest(arguments.link_manifest.resolve()),
        {
            protocol: {"nlohmann_json::nlohmann_json"},
            model: {"AISuite::OpenAICodexFrontendProtocol"},
            server: {
                "ai-openai-codex-frontend-model",
                "AISuite::OpenAICodexFrontendProtocol",
                "AISuite::OpenAICodexBackend",
                "AISuite::OpenAICodex",
            },
            client: {
                "ai-openai-codex-frontend-model",
                "AISuite::OpenAICodexFrontendProtocol",
                "AISuite::OpenAICodex",
            },
        },
    )

    require_target_type(targets, protocol, "SHARED_LIBRARY")
    require_target_type(targets, model, "STATIC_LIBRARY")
    require_target_type(targets, server, "STATIC_LIBRARY")
    require_target_type(targets, client, "STATIC_LIBRARY")

    # File API target references cover in-project targets.  Treat these as
    # exclusive allowlists so an unrelated AISuite production edge cannot hide
    # merely because it is not named in a forbidden list yet.
    require_dependencies(targets, protocol, [], [])
    require_dependencies(targets, model, [protocol], [protocol, codex])
    require_dependencies(
        targets,
        server,
        [codex, backend, protocol, model],
        [codex, backend, protocol, model],
    )
    require_dependencies(
        targets,
        client,
        [codex, protocol, model],
        [codex, protocol, model],
    )

    require_dependency_closure(
        targets, protocol, [], [codex, backend, old_server, old_client, model, server, client]
    )
    require_dependency_closure(
        targets, model, [protocol], [backend, old_server, old_client, server, client]
    )
    require_dependency_closure(
        targets, server, [codex, backend, protocol, model], [old_server, old_client, client]
    )
    require_dependency_closure(
        targets, client, [codex, protocol, model], [backend, old_server, old_client, server]
    )

    transport_target_patterns = [
        r"(^|[-_])snodec($|[-_])",
        r"(^|[-_])(http|websocket|tls|rfcomm|tcp|unix|ssl|crypto|bluetooth|qt|curses|ncurses)($|[-_])",
        r"(^|[-_])net-(un|in|in6|rc)($|[-_])",
    ]
    reject_dependency_patterns(targets, protocol, transport_target_patterns)
    reject_dependency_patterns(targets, model, transport_target_patterns)
    reject_dependency_patterns(targets, server, transport_target_patterns)
    reject_dependency_patterns(targets, client, transport_target_patterns)

    # P2 deliberately leaves the production application wiring on the oracle
    # implementations.  The client reaches its public SDK through the support
    # library, so prove the transitive closure rather than assuming a direct
    # executable-to-DSO edge.
    require_dependency_closure(targets, "codex-backend", [old_server], [server, client])
    require_dependency_closure(
        targets, "codex-backend-client-support", [old_client], [server, client]
    )
    require_dependency_closure(targets, "codex-backend-client", [old_client], [server, client])

    reject_link_fragments(
        targets,
        protocol,
        [
            r"aisuite-openai-codex(?:-backend|-frontend|-frontend-client)?(?:[.]so|[.]a)",
            r"(?:^|[/_-])snodec(?:[/_.-]|$)",
            r"(?:^|[/_-])(?:ssl|crypto|bluetooth)(?:[/_.-]|$)",
            r"(?:^|[/_-])(?:http|websocket|tls|rfcomm|tcp|unix|net-(?:un|in|in6|rc))(?:[/_.-]|$)",
            r"(?:^|[/_-])(?:Qt|curses|ncurses)(?:[/_.-]|$)",
        ],
    )
    reject_link_fragments(
        targets,
        server,
        [
            r"aisuite-openai-codex-frontend-client-core",
            r"aisuite-openai-codex-frontend-client(?:[.]so|[.]a)",
            r"(?:^|[/_-])(?:http|websocket|tls|rfcomm|tcp|unix|net-(?:un|in|in6|rc))(?:[/_.-]|$)",
            r"(?:^|[/_-])(?:ssl|crypto|bluetooth)(?:[/_.-]|$)",
            r"(?:^|[/_-])(?:Qt|curses|ncurses)(?:[/_.-]|$)",
        ],
    )
    reject_link_fragments(
        targets,
        client,
        [
            r"aisuite-openai-codex-backend(?:[.]so|[.]a)",
            r"aisuite-openai-codex-frontend-server-core",
            r"aisuite-openai-codex-frontend(?:[.]so|[.]a)",
            r"(?:^|[/_-])snodec(?:[/_.-]|$)",
            r"(?:^|[/_-])(?:http|websocket|tls|rfcomm|tcp|unix|net-(?:un|in|in6|rc))(?:[/_.-]|$)",
            r"(?:^|[/_-])(?:ssl|crypto|bluetooth)(?:[/_.-]|$)",
            r"(?:^|[/_-])(?:Qt|curses|ncurses)(?:[/_.-]|$)",
        ],
    )

    model_sources = [pathlib.Path("src/ai/openai/codex/frontend/internal/model")]
    client_sources = [pathlib.Path("src/ai/openai/codex/frontend/internal/client")]

    reject_includes(
        source_dir,
        model_sources,
        [
            "ai/openai/codex/backend/",
            "ai/openai/codex/AppServerClient.h",
            "ai/openai/codex/Api.h",
            "ai/openai/codex/frontend/FrontendService.h",
            "ai/openai/codex/frontend/internal/server/",
            "ai/openai/codex/frontend/internal/client/",
            *TRANSPORT_INCLUDE_PREFIXES,
        ],
    )
    reject_includes(
        source_dir,
        client_sources,
        [
            "ai/openai/codex/backend/",
            "ai/openai/codex/AppServerClient.h",
            "ai/openai/codex/Api.h",
            "ai/openai/codex/frontend/FrontendService.h",
            "ai/openai/codex/frontend/client/",
            "ai/openai/codex/frontend/internal/server/",
            *TRANSPORT_INCLUDE_PREFIXES,
        ],
    )

    # Keep the permitted Codex-facing surface explicit.  In particular, the
    # model and client currently need no ai/openai/codex/typed/* header; adding
    # any stable domain dependency therefore requires a reviewed policy change.
    require_include_allowlist(
        source_dir,
        model_sources,
        "ai/openai/codex/",
        [
            "ai/openai/codex/frontend/Codec.h",
            "ai/openai/codex/frontend/GeneratedProtocol.h",
            "ai/openai/codex/frontend/Messages.h",
            "ai/openai/codex/frontend/Protocol.h",
        ],
        ["ai/openai/codex/frontend/internal/model/"],
    )
    require_include_allowlist(
        source_dir,
        client_sources,
        "ai/openai/codex/",
        [
            "ai/openai/codex/frontend/Codec.h",
            "ai/openai/codex/frontend/Protocol.h",
            "ai/openai/codex/frontend/detail/EventRepresentation.h",
        ],
        [
            "ai/openai/codex/frontend/internal/client/",
            "ai/openai/codex/frontend/internal/model/",
        ],
    )
    reject_includes(
        source_dir,
        [pathlib.Path("src/ai/openai/codex/frontend/internal/server")],
        [
            "ai/openai/codex/AppServerClient.h",
            "ai/openai/codex/Api.h",
            "ai/openai/codex/frontend/FrontendService.h",
            "ai/openai/codex/frontend/client/",
            "ai/openai/codex/frontend/internal/client/",
            *TRANSPORT_INCLUDE_PREFIXES,
        ],
    )
    protocol_inputs = target_source_paths(targets, protocol, source_dir, build_dir)
    protocol_inputs.extend(
        source_dir / path
        for path in [
            # Installed public boundary.
            pathlib.Path("src/ai/openai/codex/frontend/Codec.h"),
            pathlib.Path("src/ai/openai/codex/frontend/GeneratedProtocol.h"),
            pathlib.Path("src/ai/openai/codex/frontend/Messages.h"),
            pathlib.Path("src/ai/openai/codex/frontend/Protocol.h"),
            pathlib.Path("src/ai/openai/codex/frontend/Security.h"),
            # Documented private schema and representation inputs.
            pathlib.Path("src/ai/openai/codex/frontend/GeneratedProtocolSchema.inc"),
            pathlib.Path("src/ai/openai/codex/frontend/detail/EventRepresentation.h"),
            pathlib.Path("src/ai/openai/codex/frontend/detail/GeneratedSchemaValidator.h"),
        ]
    )
    reject_includes(
        source_dir,
        protocol_inputs,
        [
            "ai/openai/codex/backend/",
            "ai/openai/codex/AppServerClient.h",
            "ai/openai/codex/Api.h",
            "ai/openai/codex/frontend/FrontendService.h",
            "ai/openai/codex/frontend/client/",
            "ai/openai/codex/frontend/internal/",
            *TRANSPORT_INCLUDE_PREFIXES,
        ],
    )

    print("Codex frontend P2 dependency and source-closure policy passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PolicyFailure as error:
        print(f"Codex frontend P2 dependency policy failure: {error}", file=sys.stderr)
        raise SystemExit(1) from error
