#!/usr/bin/env python3
"""Check that P3's temporary legacy oracle is frozen and isolated."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


PROJECTION_TARGET = "ai-openai-codex-frontend-legacy-projection-oracle"
SERVER_TARGET = "ai-openai-codex-frontend-legacy-server-oracle"
CLIENT_TARGET = "ai-openai-codex-frontend-legacy-client-oracle"
ORACLE_TARGETS = (PROJECTION_TARGET, SERVER_TARGET, CLIENT_TARGET)

ORACLE_ROOT = Path("tests/component/codex/oracle")
ORACLE_CMAKE = Path("tests/component/codex/CMakeLists.txt")
EXPECTED_INCLUDE_ROOT = ORACLE_ROOT / "include"
EXPECTED_SOURCE_AUTHORITY = ORACLE_ROOT / "source-closure.txt"
DEFAULT_MANIFEST = Path(
    "docs/ai/openai/codex/architecture-reduction/p3-frontend-cutover-manifest.json"
)
EXPECTED_BASE = "7e68847e14753553402e1d468c3af15a148eea80"
DEPENDENCY_RESOLUTION_TEST = "CodexFrontendP3OracleDependencyResolutionTest"

FROZEN_PROJECTION_BASE_PATHS = (
    "src/ai/openai/codex/frontend/detail/BackendProjectionBuilder.cpp",
    "src/ai/openai/codex/frontend/detail/BackendProjectionBuilder.h",
    "src/ai/openai/codex/frontend/detail/FrontendCapabilities.cpp",
    "src/ai/openai/codex/frontend/detail/FrontendCapabilities.h",
    "src/ai/openai/codex/frontend/detail/FrontendProjection.cpp",
    "src/ai/openai/codex/frontend/detail/FrontendProjection.h",
)
FROZEN_PROJECTION_FILES = (
    ORACLE_ROOT / "detail/BackendProjectionBuilder.cpp",
    EXPECTED_INCLUDE_ROOT
    / "ai/openai/codex/frontend/detail/BackendProjectionBuilder.h",
    ORACLE_ROOT / "detail/FrontendCapabilities.cpp",
    EXPECTED_INCLUDE_ROOT / "ai/openai/codex/frontend/detail/FrontendCapabilities.h",
    ORACLE_ROOT / "detail/FrontendProjection.cpp",
    EXPECTED_INCLUDE_ROOT / "ai/openai/codex/frontend/detail/FrontendProjection.h",
)
EXPECTED_PROJECTION_MAPPING = dict(
    zip(FROZEN_PROJECTION_FILES, FROZEN_PROJECTION_BASE_PATHS, strict=True)
)

EXPECTED_SHARED_STABLE_AUTHORITIES = {
    "src/ai/openai/codex/frontend/Messages.h",
    "src/ai/openai/codex/frontend/Security.h",
    "src/ai/openai/codex/frontend/GeneratedProtocol.h",
    "src/ai/openai/codex/backend/Snapshot.h",
}
RENAME_TOKENS = (
    "FrontendConnection",
    "FrontendService",
    "FrontendServiceTestAccess",
    "client",
)

STATE_SHIM = ORACLE_ROOT / "client/State.cpp"
STATE_PARTS = tuple(
    ORACLE_ROOT / f"client/State.part{part}.inc" for part in range(1, 9)
)
STATE_BASE_PATH = "src/ai/openai/codex/frontend/client/State.cpp"

EXPECTED_DIRECT_COPIES = {
    ORACLE_ROOT
    / "LegacyFrontendService.cpp": "src/ai/openai/codex/frontend/FrontendService.cpp",
    ORACLE_ROOT
    / "client/BindingMetadata.cpp": "src/ai/openai/codex/frontend/client/BindingMetadata.cpp",
    ORACLE_ROOT
    / "client/Client.cpp": "src/ai/openai/codex/frontend/client/Client.cpp",
    ORACLE_ROOT
    / "client/GeneratedFacades.cpp": "src/ai/openai/codex/frontend/client/GeneratedFacades.cpp",
    ORACLE_ROOT
    / "client/OperationCodecs.cpp": "src/ai/openai/codex/frontend/client/OperationCodecs.cpp",
    ORACLE_ROOT
    / "client/ProjectionFingerprint.cpp": "src/ai/openai/codex/frontend/client/ProjectionFingerprint.cpp",
    **EXPECTED_PROJECTION_MAPPING,
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def fail(message: str) -> None:
    raise AssertionError(message)


def passed(message: str) -> None:
    print(f"PASS: {message}")


def relative_text(path: Path) -> str:
    return path.as_posix()


def require_relative_path(value: object, field: str) -> Path:
    if not isinstance(value, str) or not value:
        fail(f"{field} must be a non-empty repository-relative path")
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        fail(f"{field} is not a closed repository-relative path: {value}")
    return path


def is_beneath(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
    except ValueError:
        return False
    return True


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def base_git_is_available(root: Path) -> bool:
    try:
        top = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--show-toplevel"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except OSError:
        return False
    if top.returncode != 0:
        return False
    try:
        if Path(top.stdout.strip()).resolve() != root:
            return False
    except OSError:
        return False
    try:
        present = subprocess.run(
            ["git", "-C", str(root), "cat-file", "-e", f"{EXPECTED_BASE}^{{commit}}"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return False
    return present.returncode == 0


def git_bytes(root: Path, path: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(root), "show", f"{EXPECTED_BASE}:{path}"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        fail(
            f"cannot read optional Git authority {EXPECTED_BASE}:{path}: "
            + result.stderr.decode("utf-8", errors="replace").strip()
        )
    return result.stdout


def parse_source_authority(path: Path) -> dict[str, list[Path]]:
    result: dict[str, list[Path]] = {target: [] for target in ORACLE_TARGETS}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split("|")
        if len(fields) != 2:
            fail(f"malformed source-authority row {line_number}: {raw_line}")
        target, raw_source = (field.strip() for field in fields)
        if target not in result:
            fail(f"unknown oracle target in source-authority row {line_number}: {target}")
        source = require_relative_path(raw_source, f"source-authority row {line_number}")
        if source.suffix != ".cpp":
            fail(f"oracle implementation TU is not .cpp: {source}")
        result[target].append(source)

    for target, sources in result.items():
        if not sources:
            fail(f"oracle target has empty source authority: {target}")
        if len(sources) != len(set(sources)):
            fail(f"oracle target repeats a source: {target}")
    return result


def frozen_inventory(root: Path) -> set[Path]:
    result: set[Path] = set()
    for path in (root / ORACLE_ROOT).rglob("*"):
        if not path.is_file() or path.suffix not in {".cpp", ".h", ".inc"}:
            continue
        relative = path.relative_to(root)
        if is_beneath(relative, ORACLE_ROOT / "link"):
            continue
        result.add(relative)
    return result


def manifest_digests(oracle: dict[str, Any]) -> dict[Path, dict[str, Any]]:
    entries = oracle.get("sourceDigests")
    if not isinstance(entries, list) or not entries:
        fail("temporaryOracleIdentities.sourceDigests must be a non-empty array")
    result: dict[Path, dict[str, Any]] = {}
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            fail(f"sourceDigests[{index}] is not an object")
        path = require_relative_path(entry.get("path"), f"sourceDigests[{index}].path")
        if path in result:
            fail(f"duplicate SHA-256 authority for {path}")
        digest = entry.get("sha256")
        if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
            fail(f"invalid SHA-256 authority for {path}: {digest!r}")
        if entry.get("basePath") is not None:
            require_relative_path(entry["basePath"], f"sourceDigests[{index}].basePath")
        result[path] = entry
    return result


def included_src_implementation(path: Path, root: Path, include_root: Path) -> Path | None:
    for include_name in INCLUDE_RE.findall(path.read_text(encoding="utf-8")):
        include_path = Path(include_name)
        if include_path.suffix not in {".cpp", ".inc"}:
            continue
        candidates = (
            path.parent / include_path,
            include_root / include_path,
            root / "src" / include_path,
            root / include_path,
        )
        for candidate in candidates:
            resolved = candidate.resolve()
            if resolved.exists() and is_beneath(resolved, (root / "src").resolve()):
                return resolved
    return None


def cmake_target_call(text: str, command: str, target: str) -> str | None:
    match = re.search(
        re.escape(command) + r"\s*\(\s*" + re.escape(target) + r"\b(.*?)\)",
        text,
        re.DOTALL,
    )
    return match.group(1) if match else None


def check_cmake_ownership(root: Path, authority: dict[str, list[Path]]) -> None:
    path = root / ORACLE_CMAKE
    text = path.read_text(encoding="utf-8")
    if EXPECTED_SOURCE_AUTHORITY.name not in text or "file(STRINGS" not in text:
        fail(f"CMake does not consume source authority {EXPECTED_SOURCE_AUTHORITY}")
    for target in ORACLE_TARGETS:
        if re.search(
            r"p3_legacy_oracle_target_sources\s*\([^)]*\b"
            + re.escape(target)
            + r"\b[^)]*\)",
            text,
            re.DOTALL,
        ) is None:
            fail(f"CMake target is not populated from source authority: {target}")
        includes = cmake_target_call(text, "target_include_directories", target)
        if includes is None:
            fail(f"oracle target has no private include configuration: {target}")
        if re.search(r"\bBEFORE\s+PRIVATE\b", includes) is None:
            fail(f"oracle target does not use BEFORE PRIVATE include precedence: {target}")
        if '"${P3_LEGACY_ORACLE_INCLUDE_ROOT}"' not in includes:
            fail(f"oracle target omits the mirrored frozen include root: {target}")

    expected_projection_sources = {
        path for path in FROZEN_PROJECTION_FILES if path.suffix == ".cpp"
    }
    projection_sources = set(authority[PROJECTION_TARGET])
    if projection_sources != expected_projection_sources:
        fail("shared projection target does not own exactly the three frozen projection TUs")
    if any(
        projection_sources.intersection(authority[target])
        for target in (SERVER_TARGET, CLIENT_TARGET)
    ):
        fail("frozen projection implementation is compiled into an oracle consumer")

    for consumer in (SERVER_TARGET, CLIENT_TARGET):
        links = cmake_target_call(text, "target_link_libraries", consumer)
        if links is None or PROJECTION_TARGET not in links:
            fail(f"{consumer} does not consume the shared frozen projection target")

    projection_definitions = cmake_target_call(text, "target_compile_definitions", PROJECTION_TARGET)
    if projection_definitions is not None and any(
        token in projection_definitions for token in RENAME_TOKENS
    ):
        fail("shared frozen projection target receives an oracle rename definition")
    server_definitions = cmake_target_call(text, "target_compile_definitions", SERVER_TARGET)
    if server_definitions is None or not all(
        definition in server_definitions
        for definition in (
            "FrontendConnection=LegacyFrontendConnection",
            "FrontendService=LegacyFrontendService",
            "FrontendServiceTestAccess=LegacyFrontendServiceTestAccess",
        )
    ):
        fail("server oracle compile-time rename definitions are incomplete")
    client_definitions = cmake_target_call(text, "target_compile_definitions", CLIENT_TARGET)
    if client_definitions is None or "client=legacy_client" not in client_definitions:
        fail("client oracle compile-time rename definition is missing")


def check_test_only_and_noninstalled(root: Path) -> None:
    top_cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    guarded_tests = any(
        "add_subdirectory(tests)" in match.group(1)
        for match in re.finditer(
            r"if\s*\(\s*AISUITE_BUILD_TESTS\s*\)(.*?)endif\s*\(\s*\)",
            top_cmake,
            re.DOTALL,
        )
    )
    if not guarded_tests:
        fail("tests subtree is not guarded by AISUITE_BUILD_TESTS")

    cmake_paths = [root / "CMakeLists.txt"]
    cmake_paths.extend((root / "src").rglob("CMakeLists.txt"))
    cmake_paths.extend((root / "tests").rglob("CMakeLists.txt"))
    cmake_paths.extend((root / "cmake").rglob("*.cmake"))
    install_blocks: list[str] = []
    for cmake_path in cmake_paths:
        text = cmake_path.read_text(encoding="utf-8", errors="replace")
        if not is_beneath(cmake_path.resolve(), (root / "tests").resolve()):
            for target in ORACLE_TARGETS:
                if re.search(r"add_(?:library|executable)\s*\(\s*" + re.escape(target), text):
                    fail(f"oracle target is defined outside tests: {cmake_path}: {target}")
        install_blocks.extend(match.group(0) for match in re.finditer(r"install\s*\(.*?\)", text, re.DOTALL))
    for block in install_blocks:
        for target in ORACLE_TARGETS:
            if target in block:
                fail(f"oracle target is installed: {target}")

    production_mentions: list[str] = []
    for path in (root / "src").rglob("*"):
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if any(target in text for target in ORACLE_TARGETS) or relative_text(ORACLE_ROOT) in text:
            production_mentions.append(relative_text(path.relative_to(root)))
    if production_mentions:
        fail("production source mentions the temporary oracle: " + ", ".join(production_mentions))


def check_source_package_semantics(root: Path, oracle: dict[str, Any]) -> None:
    top_cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    cpack_match = re.search(
        r"set\s*\(\s*CPACK_SOURCE_IGNORE_FILES\b(.*?)\n\s*\)",
        top_cmake,
        re.DOTALL,
    )
    if cpack_match is None:
        fail("CPACK_SOURCE_IGNORE_FILES definition is missing")
    oracle_archive_path = "/tests/component/codex/oracle/"
    for ignore_pattern in re.findall(r'"([^"]*)"', cpack_match.group(1)):
        try:
            ignored = re.search(ignore_pattern, oracle_archive_path) is not None
        except re.error as error:
            fail(f"cannot evaluate CPack source-ignore regex {ignore_pattern!r}: {error}")
        if ignored:
            fail(f"CPack source-ignore regex excludes the oracle subtree: {ignore_pattern!r}")

    required = {
        "builtOnlyWhenTestsEnabled": True,
        "installed": False,
        "sourcePackaged": True,
        "installedOrBinaryPackaged": False,
        "productionDependencyAllowed": False,
    }
    for field, expected in required.items():
        if oracle.get(field) != expected:
            fail(f"temporary oracle packaging/ownership semantic changed: {field}")
    if "packaged" in oracle:
        fail("ambiguous temporaryOracleIdentities.packaged field remains")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--source-authority", type=Path, required=True)
    args = parser.parse_args()

    root = args.repo_root.resolve()
    authority_path = args.source_authority
    if not authority_path.is_absolute():
        authority_path = (root / authority_path).resolve()
    if not is_beneath(authority_path, root):
        fail(f"source authority escaped repository: {authority_path}")
    authority_relative = authority_path.relative_to(root)
    if authority_relative != EXPECTED_SOURCE_AUTHORITY:
        fail(
            f"unexpected source authority {authority_relative}; expected "
            f"{EXPECTED_SOURCE_AUTHORITY}"
        )

    manifest = json.loads((root / DEFAULT_MANIFEST).read_text(encoding="utf-8"))
    if manifest.get("p3Base") != EXPECTED_BASE:
        fail("P3 manifest base drifted from the frozen branch base")
    if manifest.get("currentTaskScope") != {
        "allowedCommits": [1, 2, 3],
        "forbiddenToStart": [4, 5, 6, 7],
    }:
        fail("P3 Task-A scope is not closed to Commits 1-3")
    required_gates = {
        "localParallel": 24,
        "localCtestTimeoutSeconds": 60,
        "commit5HostedCheckpointRequiredBeforeDeletion": True,
        "commit5LegacyAndOracleMustExist": True,
        "commit5HostedFullClosureMustBeGreen": True,
        "sameObjectSnodecConnectProofRequiredBeforeCommit4": True,
        "threadLocalWebSocketHandoffAllowed": False,
        "webSocketHandoffModel": "file-private-owner-event-loop-raii-lifo-exact-connection",
        "finalHostedClosureRequired": True,
    }
    gates = manifest.get("gates")
    if not isinstance(gates, dict):
        fail("P3 manifest gates are missing")
    for field, expected in required_gates.items():
        if gates.get(field) != expected:
            fail(f"P3 steering gate changed: {field}")

    oracle = manifest.get("temporaryOracleIdentities")
    if not isinstance(oracle, dict):
        fail("temporaryOracleIdentities is missing")
    if oracle.get("targets") != list(ORACLE_TARGETS):
        fail("temporary oracle target identities or ordering changed")
    if oracle.get("sourceAuthority") != relative_text(EXPECTED_SOURCE_AUTHORITY):
        fail("manifest does not name the consumed source authority")
    if oracle.get("includeRoot") != relative_text(EXPECTED_INCLUDE_ROOT):
        fail("manifest frozen include root changed")
    if oracle.get("sharedProjectionTarget") != PROJECTION_TARGET:
        fail("manifest shared projection target changed")
    if oracle.get("dependencyResolutionTest") != DEPENDENCY_RESOLUTION_TEST:
        fail("manifest dependency-resolution test identity changed")
    if oracle.get("sourceLevelRenaming") is not False:
        fail("source-level oracle renaming must remain disabled")
    if oracle.get("symbolRenaming") != "compile-time-target-definitions-only":
        fail("oracle symbol renaming must remain compile-time-only")
    if "sources" in oracle:
        fail("obsolete hand-maintained temporaryOracleIdentities.sources remains")
    if set(oracle.get("sharedStableAuthorities", [])) != EXPECTED_SHARED_STABLE_AUTHORITIES:
        fail("shared stable authority list changed")
    if tuple(oracle.get("renameTokens", [])) != RENAME_TOKENS:
        fail("oracle rename-token authority changed")
    if tuple(oracle.get("frozenProjectionBasePaths", [])) != FROZEN_PROJECTION_BASE_PATHS:
        fail("six-file frozen projection closure changed")
    check_source_package_semantics(root, oracle)

    authority = parse_source_authority(authority_path)
    all_authority_sources = set().union(*(set(sources) for sources in authority.values()))
    for target, sources in authority.items():
        for source in sources:
            if not is_beneath(source, ORACLE_ROOT):
                fail(f"live implementation TU entered {target} oracle closure: {source}")
            if source.parts and source.parts[0] == "src":
                fail(f"oracle target compiles a live src implementation TU: {source}")
            if not (root / source).is_file():
                fail(f"authoritative oracle TU does not exist: {source}")
    check_cmake_ownership(root, authority)
    passed(
        f"source authority owns {len(all_authority_sources)} self-contained oracle TUs; "
        "the shared projection implementation is compiled once"
    )

    inventory = frozen_inventory(root)
    symlinks = [
        path.relative_to(root)
        for path in (root / ORACLE_ROOT).rglob("*")
        if path.is_symlink()
    ]
    if symlinks:
        fail("oracle closure contains symlinks: " + ", ".join(map(str, symlinks)))
    digests = manifest_digests(oracle)
    missing = inventory - set(digests)
    orphaned = set(digests) - inventory
    if missing:
        fail("frozen files lack SHA-256 authority: " + ", ".join(map(str, sorted(missing))))
    if orphaned:
        fail("orphan SHA-256 entries: " + ", ".join(map(str, sorted(orphaned))))
    for path, entry in digests.items():
        absolute = root / path
        if not absolute.is_file():
            fail(f"digest authority names a missing file: {path}")
        actual = sha256(absolute.read_bytes())
        if actual != entry["sha256"]:
            fail(
                f"oracle bytes do not match authoritative digest for {path}: "
                f"expected {entry['sha256']}, actual {actual}"
            )
    passed(f"all {len(inventory)} frozen files match their stored SHA-256 values")

    actual_direct_mapping = {
        path: entry.get("basePath")
        for path, entry in digests.items()
        if entry.get("basePath") is not None
    }
    if actual_direct_mapping != EXPECTED_DIRECT_COPIES:
        fail("direct-copy basePath authority differs from the frozen closure")
    if digests[STATE_SHIM].get("basePath") is not None:
        fail("State.cpp include shim must not claim a direct P3-base counterpart")
    for part in STATE_PARTS:
        if digests[part].get("basePath") is not None:
            fail(f"State reconstruction part must not claim direct-copy identity: {part}")

    reconstruction = oracle.get("stateReconstruction")
    if not isinstance(reconstruction, dict):
        fail("State reconstruction authority is missing")
    if reconstruction.get("shimPath") != relative_text(STATE_SHIM):
        fail("State include-shim identity changed")
    if reconstruction.get("partPaths") != [relative_text(path) for path in STATE_PARTS]:
        fail("State reconstruction parts or order changed")
    if reconstruction.get("basePath") != STATE_BASE_PATH:
        fail("State reconstruction base path changed")
    expected_state_digest = reconstruction.get("baseSha256")
    if not isinstance(expected_state_digest, str) or SHA256_RE.fullmatch(expected_state_digest) is None:
        fail("State reconstruction base SHA-256 is invalid")
    reconstructed = b"".join((root / path).read_bytes() for path in STATE_PARTS)
    if sha256(reconstructed) != expected_state_digest:
        fail("zero-delimiter State.part1-8 reconstruction digest differs from authority")
    passed(f"State.part1-8 reconstruct frozen State.cpp digest {expected_state_digest}")

    git_available = base_git_is_available(root)
    if git_available:
        for path, base_path in EXPECTED_DIRECT_COPIES.items():
            base = git_bytes(root, base_path)
            if (root / path).read_bytes() != base:
                fail(f"direct-copy bytes differ from {EXPECTED_BASE}:{base_path}: {path}")
            if sha256(base) != digests[path]["sha256"]:
                fail(f"P3-base digest differs from manifest authority: {path}")
        base_state = git_bytes(root, STATE_BASE_PATH)
        if base_state != reconstructed or sha256(base_state) != expected_state_digest:
            fail("State reconstruction differs from the available P3-base Git object")
        passed("optional Git comparison confirms direct copies and State reconstruction")
    else:
        passed("stored SHA-256 authority validated without requiring Git metadata")

    for path in FROZEN_PROJECTION_FILES:
        text = (root / path).read_text(encoding="utf-8")
        for token in RENAME_TOKENS:
            if token in text:
                fail(f"shared projection closure contains rename token {token}: {path}")
    passed("six-file projection closure contains no oracle rename token")

    frozen_include_root = (root / EXPECTED_INCLUDE_ROOT).resolve()
    smuggled: list[str] = []
    for path in (root / ORACLE_ROOT).rglob("*"):
        if not path.is_file() or path.suffix not in {".cpp", ".h", ".inc"}:
            continue
        resolved = included_src_implementation(path, root, frozen_include_root)
        if resolved is not None:
            smuggled.append(f"{path.relative_to(root)} -> {resolved.relative_to(root)}")
    if smuggled:
        fail("oracle includes live src .cpp/.inc implementation text: " + ", ".join(smuggled))
    passed("oracle contains no src implementation-TU/include smuggling")

    check_test_only_and_noninstalled(root)
    passed("oracle targets are test-only, non-installed, and absent from production source")
    passed("source-package semantics include the oracle while excluding it from runtime packages")
    print(
        "P3 oracle isolation and fidelity checks passed; actual frozen-header "
        f"resolution is checked separately by {DEPENDENCY_RESOLUTION_TEST}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        AssertionError,
        KeyError,
        OSError,
        subprocess.SubprocessError,
        json.JSONDecodeError,
    ) as error:
        print(f"P3 oracle isolation failure: {error}", file=sys.stderr)
        raise SystemExit(1)
