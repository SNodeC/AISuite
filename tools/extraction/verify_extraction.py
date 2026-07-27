#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

SOURCE_REPOSITORY = "https://github.com/SNodeC/snode.c"
SOURCE_COMMIT = "d18b231a1d2ec2235fd6f204786b0a761cc24ff5"
SOURCE_TREE = "88a63edc985a851b2b76b0c56df19fae74ea8069"
EXPECTED_PROTOCOL_STATUS = {
    "Complete": 280,
    "Partial": 4,
    "NotImplemented": 55,
    "NotApplicable": 48,
}
EXPECTED_PARTIAL_IDENTITIES = {
    "initialize",
    "initialized",
    "error",
    "item/tool/requestUserInput",
}
MANIFEST_PATH = Path("docs/extraction/source-manifest.json")
FILTER_MAP_PATH = Path("docs/extraction/filter-map.json")


class ExtractionError(RuntimeError):
    pass


def run(root: Path, *arguments: str, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if check and result.returncode != 0:
        raise ExtractionError(
            result.stderr.decode("utf-8", "replace")
            or f"git {' '.join(arguments)} failed"
        )
    return result


def git_text(root: Path, *arguments: str) -> str:
    return run(root, *arguments).stdout.decode("utf-8", "surrogateescape").strip()


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def canonical_json(value: Any) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def tracked_and_untracked_files(root: Path) -> set[str]:
    raw = run(
        root,
        "ls-files",
        "-z",
        "--cached",
        "--others",
        "--exclude-standard",
    ).stdout
    return {
        item.decode("utf-8", "surrogateescape")
        for item in raw.split(b"\0")
        if item
    }


def baseline_entries(root: Path, baseline: str) -> list[tuple[str, str]]:
    raw = run(root, "ls-tree", "-r", "-z", baseline).stdout
    rows: list[tuple[str, str]] = []
    for record in raw.split(b"\0"):
        if not record:
            continue
        metadata, path = record.split(b"\t", 1)
        _mode, object_type, blob = metadata.decode().split()
        if object_type != "blob":
            raise ExtractionError(f"unexpected baseline object type: {object_type}")
        rows.append((path.decode("utf-8", "surrogateescape"), blob))
    return rows


def registry_status(root: Path) -> tuple[dict[str, int], set[str]]:
    tools = root / "tools/codex"
    sys.path.insert(0, str(tools))
    try:
        import app_server_surface as surface

        rows = surface.parse_registry_data(
            root / "src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc"
        )
    finally:
        try:
            sys.path.remove(str(tools))
        except ValueError:
            pass
    counts = {name: 0 for name in EXPECTED_PROTOCOL_STATUS}
    partials: set[str] = set()
    for row in rows:
        status = str(row["typed_schema_status"])
        counts[status] += 1
        if status == "Partial":
            partials.add(str(row["name"]))
    if sum(counts.values()) != 387:
        raise ExtractionError(f"registry parse produced {sum(counts.values())} identities")
    return counts, partials


def dependency_boundary(root: Path) -> dict[str, Any]:
    scan_paths = [root / "CMakeLists.txt", root / "cmake", root / "src"]
    forbidden_patterns = {
        "source_relative_add_subdirectory": re.compile(
            r"add_subdirectory\s*\([^\n)]*snode\.c", re.IGNORECASE
        ),
        "source_relative_include": re.compile(r"\.\./snode\.c/(?:src|include)"),
        "absolute_snodec_source": re.compile(r"/(?:[^/]+/)*snode\.c/src/"),
    }
    findings: list[str] = []
    for candidate in scan_paths:
        paths = [candidate] if candidate.is_file() else sorted(candidate.rglob("*"))
        for path in paths:
            if not path.is_file() or path.suffix not in {
                "", ".cmake", ".txt", ".h", ".cpp", ".in"
            }:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            for code, pattern in forbidden_patterns.items():
                if pattern.search(text):
                    findings.append(f"{code}:{path.relative_to(root)}")
    root_cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    package_config = (root / "cmake/AISuiteConfig.cmake.in").read_text(
        encoding="utf-8"
    )
    required = (
        "find_package(snodec CONFIG REQUIRED",
        "find_dependency(snodec CONFIG COMPONENTS",
    )
    if required[0] not in root_cmake or required[1] not in package_config:
        findings.append("installed_snodec_package_dependency_missing")
    return {
        "findings": sorted(findings),
        "installed_dependency": not findings,
    }


def generate(root: Path, output: Path) -> dict[str, Any]:
    filter_map = json.loads((root / FILTER_MAP_PATH).read_text(encoding="utf-8"))
    baseline = str(filter_map["filtered_head"])
    baseline_tree = str(filter_map["filtered_tree"])
    if git_text(root, "rev-parse", f"{baseline}^{{tree}}") != baseline_tree:
        raise ExtractionError("filtered baseline tree disagrees with filter map")
    if run(root, "merge-base", "--is-ancestor", baseline, "HEAD", check=False).returncode:
        raise ExtractionError("filtered baseline is not an ancestor of HEAD")

    current_files = tracked_and_untracked_files(root)
    current_files.discard(MANIFEST_PATH.as_posix())
    imported: list[dict[str, Any]] = []
    baseline_paths: set[str] = set()
    for path, blob in baseline_entries(root, baseline):
        baseline_paths.add(path)
        destination = root / path
        if not destination.is_file():
            raise ExtractionError(f"imported path disappeared: {path}")
        original = run(root, "show", f"{baseline}:{path}").stdout
        current_digest = sha256_file(destination)
        original_digest = sha256_bytes(original)
        disposition = "immutable-copy" if current_digest == original_digest else "standalone-adaptation"
        imported.append(
            {
                "path": path,
                "source_blob": blob,
                "source_sha256": original_digest,
                "final_sha256": current_digest,
                "disposition": disposition,
            }
        )

    standalone: list[dict[str, str]] = []
    for path in sorted(current_files - baseline_paths):
        destination = root / path
        if not destination.is_file():
            raise ExtractionError(f"unsupported non-file extraction path: {path}")
        standalone.append({"path": path, "sha256": sha256_file(destination)})

    counts, partials = registry_status(root)
    boundary = dependency_boundary(root)
    if counts != EXPECTED_PROTOCOL_STATUS or partials != EXPECTED_PARTIAL_IDENTITIES:
        raise ExtractionError(
            f"protocol state changed: counts={counts}, partials={sorted(partials)}"
        )
    if boundary["findings"]:
        raise ExtractionError(
            "invalid cross-repository dependency: " + ", ".join(boundary["findings"])
        )

    selected_paths = list(filter_map["selected_paths"])
    required_selected = {
        "src/ai/openai/codex",
        "src/apps/codex-backend",
        "src/apps/codex-backend-client",
        "docs/ai/openai/codex",
        "tools/codex",
        "tests/component/codex",
        "tests/installed/codex",
        "tests/CodexBinaryPackageTest.cmake",
        "tests/CodexSourcePackageTest.cmake",
        "tests/policy/security/CodexSyntheticSecretLeakGuardTest.py",
    }
    if set(selected_paths) != required_selected:
        raise ExtractionError("filtered selected-path ownership changed")

    document = {
        "format_version": 1,
        "generated_notice": (
            "Generated extraction manifest. It verifies the additive repository "
            "move and is not a protocol runtime authority."
        ),
        "source": {
            "repository": SOURCE_REPOSITORY,
            "commit": SOURCE_COMMIT,
            "tree": SOURCE_TREE,
        },
        "filtered_history": {
            "head": baseline,
            "tree": baseline_tree,
            "retained_commit_count": int(filter_map["retained_commit_count"]),
            "selected_paths": selected_paths,
            "commit_map_sha256": sha256_file(root / FILTER_MAP_PATH),
        },
        "counts": {
            "imported_files": len(imported),
            "immutable_copies": sum(row["disposition"] == "immutable-copy" for row in imported),
            "standalone_adaptations": sum(
                row["disposition"] == "standalone-adaptation" for row in imported
            ),
            "new_standalone_files": len(standalone),
        },
        "protocol_state": {
            "counts": counts,
            "partial_identities": sorted(partials),
            "codex_version": "codex-cli 0.144.6",
            "soversion": 1,
        },
        "dependency_boundary": boundary,
        "imported_files": imported,
        "standalone_files": standalone,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(canonical_json(document), encoding="utf-8")
    return document


def check(root: Path, manifest_path: Path) -> dict[str, Any]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    source = manifest.get("source")
    if source != {
        "repository": SOURCE_REPOSITORY,
        "commit": SOURCE_COMMIT,
        "tree": SOURCE_TREE,
    }:
        raise ExtractionError("source provenance changed")

    filtered = manifest["filtered_history"]
    baseline = str(filtered["head"])
    if git_text(root, "rev-parse", f"{baseline}^{{tree}}") != filtered["tree"]:
        raise ExtractionError("filtered history baseline changed")
    if run(root, "merge-base", "--is-ancestor", baseline, "HEAD", check=False).returncode:
        raise ExtractionError("filtered history is no longer an ancestor")
    if sha256_file(root / FILTER_MAP_PATH) != filtered["commit_map_sha256"]:
        raise ExtractionError("filtered commit map changed")

    expected_paths = {MANIFEST_PATH.as_posix()}
    for row in manifest["imported_files"]:
        path = str(row["path"])
        expected_paths.add(path)
        destination = root / path
        if not destination.is_file():
            raise ExtractionError(f"imported file missing: {path}")
        if sha256_file(destination) != row["final_sha256"]:
            raise ExtractionError(f"imported file changed after review: {path}")
        original = run(root, "show", f"{baseline}:{path}").stdout
        if sha256_bytes(original) != row["source_sha256"]:
            raise ExtractionError(f"filtered source content changed: {path}")
        if row["disposition"] == "immutable-copy" and row["source_sha256"] != row["final_sha256"]:
            raise ExtractionError(f"immutable-copy classification is false: {path}")
    for row in manifest["standalone_files"]:
        path = str(row["path"])
        expected_paths.add(path)
        destination = root / path
        if not destination.is_file() or sha256_file(destination) != row["sha256"]:
            raise ExtractionError(f"standalone file changed after review: {path}")

    current_paths = tracked_and_untracked_files(root)
    if current_paths != expected_paths:
        missing = sorted(expected_paths - current_paths)
        extra = sorted(current_paths - expected_paths)
        raise ExtractionError(f"extraction file set changed: missing={missing}, extra={extra}")

    counts, partials = registry_status(root)
    if counts != EXPECTED_PROTOCOL_STATUS:
        raise ExtractionError(f"registry status changed: {counts}")
    if partials != EXPECTED_PARTIAL_IDENTITIES:
        raise ExtractionError(f"partial identity set changed: {sorted(partials)}")
    if manifest["protocol_state"]["counts"] != counts:
        raise ExtractionError("manifest protocol counts changed")
    if manifest["protocol_state"]["partial_identities"] != sorted(partials):
        raise ExtractionError("manifest partial identity set changed")

    root_cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    if "set(AISUITE_CODEX_SOVERSION 1)" not in root_cmake:
        raise ExtractionError("Codex SOVERSION changed during extraction")
    boundary = dependency_boundary(root)
    if boundary["findings"] or boundary != manifest["dependency_boundary"]:
        raise ExtractionError(f"dependency boundary changed: {boundary['findings']}")

    return manifest


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument("command", choices=("generate", "check"), nargs="?", default="check")
    result.add_argument("--repo-root", type=Path, required=True)
    result.add_argument("--manifest", type=Path)
    return result


def main() -> int:
    options = parser().parse_args()
    root = options.repo_root.resolve()
    manifest = (
        options.manifest.resolve()
        if options.manifest is not None
        else root / MANIFEST_PATH
    )
    try:
        if options.command == "generate":
            document = generate(root, manifest)
            print(
                "AISuite extraction manifest generated: "
                f"{document['counts']['imported_files']} imported files, "
                f"{document['counts']['new_standalone_files']} standalone files"
            )
        else:
            document = check(root, manifest)
            print(
                "AISuite extraction guard passed: "
                f"{document['filtered_history']['retained_commit_count']} commits, "
                f"{document['counts']['imported_files']} imported files"
            )
    except (ExtractionError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"aisuite-extraction: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
