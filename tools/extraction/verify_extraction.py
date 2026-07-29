#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any

SOURCE_REPOSITORY = "https://github.com/SNodeC/snode.c"
SOURCE_COMMIT = "d18b231a1d2ec2235fd6f204786b0a761cc24ff5"
SOURCE_TREE = "88a63edc985a851b2b76b0c56df19fae74ea8069"
EXPECTED_PROTOCOL_STATUS = {
    "Complete": 313,
    "Partial": 4,
    "NotImplemented": 22,
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
GENERATION_PROOF_SELF_REFERENCE_EXCLUSIONS = (
    "tools/codex/app-server-evidence/0.144.6/"
    "a1-4-user-integrations-generation-pre.json",
    "tools/codex/app-server-evidence/0.144.6/"
    "a1-4-user-integrations-generation-pass-1.json",
    "tools/codex/app-server-evidence/0.144.6/"
    "a1-4-user-integrations-generation-pass-2.json",
)
GENERATION_PROOF_EXCLUSION_DIAGNOSTIC = (
    "UserIntegrationExtractionProofExclusionMismatch"
)


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


def validate_generation_proof_exclusions(
    root: Path,
    value: Any,
    current_files: set[str],
) -> set[str]:
    expected = set(GENERATION_PROOF_SELF_REFERENCE_EXCLUSIONS)
    if value != list(GENERATION_PROOF_SELF_REFERENCE_EXCLUSIONS):
        raise ExtractionError(
            f"{GENERATION_PROOF_EXCLUSION_DIAGNOSTIC}: expected exactly "
            f"{list(GENERATION_PROOF_SELF_REFERENCE_EXCLUSIONS)}, got {value}"
        )
    if not expected <= current_files:
        raise ExtractionError(
            f"{GENERATION_PROOF_EXCLUSION_DIAGNOSTIC}: proof paths are not "
            f"tracked/untracked repository files: {sorted(expected-current_files)}"
        )
    invalid: list[str] = []
    for relative in GENERATION_PROOF_SELF_REFERENCE_EXCLUSIONS:
        path = root / relative
        try:
            mode = path.lstat().st_mode
        except OSError:
            invalid.append(relative)
            continue
        if path.is_symlink() or not stat.S_ISREG(mode):
            invalid.append(relative)
    if invalid:
        raise ExtractionError(
            f"{GENERATION_PROOF_EXCLUSION_DIAGNOSTIC}: proof paths are not "
            f"regular non-symlink files: {invalid}"
        )
    return expected


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
    proof_exclusions = validate_generation_proof_exclusions(
        root,
        list(GENERATION_PROOF_SELF_REFERENCE_EXCLUSIONS),
        current_files,
    )
    inventoried_files = current_files - proof_exclusions
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
    for path in sorted(inventoried_files - baseline_paths):
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
            "move and is not a protocol runtime authority. Exactly three "
            "path-only generation-proof metadata files are excluded from this "
            "manifest's hash domain to break an otherwise impossible SHA-256 "
            "cycle; the specialized PR-A closure guard hashes those files and "
            "the complete live generated corpus."
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
        "generation_proof_self_reference_exclusions": list(
            GENERATION_PROOF_SELF_REFERENCE_EXCLUSIONS
        ),
        "imported_files": imported,
        "standalone_files": standalone,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(canonical_json(document), encoding="utf-8")
    return document


def check(root: Path, manifest_path: Path) -> dict[str, Any]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    current_paths = tracked_and_untracked_files(root)
    proof_exclusions = validate_generation_proof_exclusions(
        root,
        manifest.get("generation_proof_self_reference_exclusions"),
        current_paths,
    )
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

    expected_paths = {MANIFEST_PATH.as_posix(), *proof_exclusions}
    for row in manifest["imported_files"]:
        path = str(row["path"])
        if path in proof_exclusions:
            raise ExtractionError(
                f"{GENERATION_PROOF_EXCLUSION_DIAGNOSTIC}: excluded proof "
                f"path is incorrectly hashed as imported: {path}"
            )
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
        if path in proof_exclusions:
            raise ExtractionError(
                f"{GENERATION_PROOF_EXCLUSION_DIAGNOSTIC}: excluded proof "
                f"path is incorrectly hashed as standalone: {path}"
            )
        expected_paths.add(path)
        destination = root / path
        if not destination.is_file() or sha256_file(destination) != row["sha256"]:
            raise ExtractionError(f"standalone file changed after review: {path}")

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


def package_files(root: Path) -> set[str]:
    files: set[str] = set()
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ExtractionError(
                f"package contains unsupported symlink: "
                f"{path.relative_to(root).as_posix()}"
            )
        if path.is_file():
            files.add(path.relative_to(root).as_posix())
    return files


def check_package(root: Path, manifest_path: Path) -> dict[str, Any]:
    """Verify a source package using only packaged filesystem authorities."""

    raw = manifest_path.read_text(encoding="utf-8")
    manifest = json.loads(raw)
    if raw != canonical_json(manifest):
        raise ExtractionError("packaged extraction manifest is noncanonical")
    if set(manifest) != {
        "counts",
        "dependency_boundary",
        "filtered_history",
        "format_version",
        "generated_notice",
        "generation_proof_self_reference_exclusions",
        "imported_files",
        "protocol_state",
        "source",
        "standalone_files",
    } or manifest.get("format_version") != 1:
        raise ExtractionError("packaged extraction manifest structure changed")
    if manifest.get("generated_notice") != (
        "Generated extraction manifest. It verifies the additive repository "
        "move and is not a protocol runtime authority. Exactly three "
        "path-only generation-proof metadata files are excluded from this "
        "manifest's hash domain to break an otherwise impossible SHA-256 "
        "cycle; the specialized PR-A closure guard hashes those files and "
        "the complete live generated corpus."
    ):
        raise ExtractionError("packaged extraction notice changed")
    current_paths = package_files(root)
    proof_exclusions = validate_generation_proof_exclusions(
        root,
        manifest.get("generation_proof_self_reference_exclusions"),
        current_paths,
    )
    if manifest.get("source") != {
        "repository": SOURCE_REPOSITORY,
        "commit": SOURCE_COMMIT,
        "tree": SOURCE_TREE,
    }:
        raise ExtractionError("packaged source provenance changed")

    filter_map = json.loads(
        (root / FILTER_MAP_PATH).read_text(encoding="utf-8")
    )
    filtered = manifest.get("filtered_history")
    if not isinstance(filtered, dict) or filtered != {
        "head": filter_map["filtered_head"],
        "tree": filter_map["filtered_tree"],
        "retained_commit_count": int(filter_map["retained_commit_count"]),
        "selected_paths": list(filter_map["selected_paths"]),
        "commit_map_sha256": sha256_file(root / FILTER_MAP_PATH),
    }:
        raise ExtractionError("packaged filtered-history authority changed")

    imported = manifest.get("imported_files")
    standalone = manifest.get("standalone_files")
    if not isinstance(imported, list) or not isinstance(standalone, list):
        raise ExtractionError("packaged extraction inventories are malformed")
    expected_paths = {MANIFEST_PATH.as_posix(), *proof_exclusions}
    imported_paths: list[str] = []
    standalone_paths: list[str] = []
    for index, row in enumerate(imported):
        if not isinstance(row, dict):
            raise ExtractionError(
                f"packaged imported row is malformed: {index}"
            )
        path = str(row.get("path", ""))
        imported_paths.append(path)
        if path in proof_exclusions:
            raise ExtractionError(
                f"{GENERATION_PROOF_EXCLUSION_DIAGNOSTIC}: excluded proof "
                f"path is incorrectly hashed as imported: {path}"
            )
        destination = root / path
        try:
            mode = destination.lstat().st_mode
        except OSError:
            mode = 0
        if (
            not stat.S_ISREG(mode)
            or destination.is_symlink()
            or sha256_file(destination) != row.get("final_sha256")
        ):
            raise ExtractionError(
                f"packaged imported final hash changed: {path}"
            )
        if row.get("disposition") not in (
            "immutable-copy",
            "standalone-adaptation",
        ):
            raise ExtractionError(
                f"packaged imported disposition changed: {path}"
            )
        if (
            row.get("disposition") == "immutable-copy"
            and row.get("source_sha256") != row.get("final_sha256")
        ):
            raise ExtractionError(
                f"packaged immutable-copy classification is false: {path}"
            )
        if not re.fullmatch(r"[0-9a-f]{40}", str(row.get("source_blob", ""))):
            raise ExtractionError(f"packaged source blob is malformed: {path}")
        if not re.fullmatch(
            r"[0-9a-f]{64}", str(row.get("source_sha256", ""))
        ):
            raise ExtractionError(
                f"packaged source SHA-256 is malformed: {path}"
            )
        expected_paths.add(path)
    for index, row in enumerate(standalone):
        if not isinstance(row, dict):
            raise ExtractionError(
                f"packaged standalone row is malformed: {index}"
            )
        path = str(row.get("path", ""))
        standalone_paths.append(path)
        if path in proof_exclusions:
            raise ExtractionError(
                f"{GENERATION_PROOF_EXCLUSION_DIAGNOSTIC}: excluded proof "
                f"path is incorrectly hashed as standalone: {path}"
            )
        destination = root / path
        try:
            mode = destination.lstat().st_mode
        except OSError:
            mode = 0
        if (
            not stat.S_ISREG(mode)
            or destination.is_symlink()
            or sha256_file(destination) != row.get("sha256")
        ):
            raise ExtractionError(
                f"packaged standalone final hash changed: {path}"
            )
        expected_paths.add(path)

    if (
        imported_paths != sorted(imported_paths)
        or standalone_paths != sorted(standalone_paths)
        or len(set(imported_paths + standalone_paths))
        != len(imported_paths) + len(standalone_paths)
    ):
        raise ExtractionError(
            "packaged extraction paths are not sorted and unique"
        )
    if current_paths != expected_paths:
        raise ExtractionError(
            "packaged extraction file set changed: "
            f"missing={sorted(expected_paths-current_paths)}, "
            f"extra={sorted(current_paths-expected_paths)}"
        )

    counts = manifest.get("counts")
    expected_counts = {
        "imported_files": len(imported),
        "immutable_copies": sum(
            row["disposition"] == "immutable-copy" for row in imported
        ),
        "standalone_adaptations": sum(
            row["disposition"] == "standalone-adaptation"
            for row in imported
        ),
        "new_standalone_files": len(standalone),
    }
    if counts != expected_counts:
        raise ExtractionError("packaged extraction counts changed")
    registry_counts, partials = registry_status(root)
    if (
        registry_counts != EXPECTED_PROTOCOL_STATUS
        or partials != EXPECTED_PARTIAL_IDENTITIES
        or manifest.get("protocol_state")
        != {
            "counts": registry_counts,
            "partial_identities": sorted(partials),
            "codex_version": "codex-cli 0.144.6",
            "soversion": 1,
        }
    ):
        raise ExtractionError("packaged protocol state changed")
    root_cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    if "set(AISUITE_CODEX_SOVERSION 1)" not in root_cmake:
        raise ExtractionError("packaged Codex SOVERSION changed")
    boundary = dependency_boundary(root)
    if boundary["findings"] or boundary != manifest.get(
        "dependency_boundary"
    ):
        raise ExtractionError(
            f"packaged dependency boundary changed: {boundary['findings']}"
        )
    return manifest


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument(
        "command",
        choices=("generate", "check", "check-package"),
        nargs="?",
        default="check",
    )
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
        elif options.command == "check":
            document = check(root, manifest)
            print(
                "AISuite extraction guard passed: "
                f"{document['filtered_history']['retained_commit_count']} commits, "
                f"{document['counts']['imported_files']} imported files"
            )
        else:
            document = check_package(root, manifest)
            print(
                "AISuite packaged extraction guard passed: "
                f"{document['counts']['imported_files']} imported files, "
                f"{document['counts']['new_standalone_files']} standalone files"
            )
    except (ExtractionError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"aisuite-extraction: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
