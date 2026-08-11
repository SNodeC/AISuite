#!/usr/bin/env python3
"""Prove the P3 legacy oracles resolve frozen detail headers at build time.

It deliberately consumes compiler dependency data rather than source text or
declared include directories:

* Ninja builds are inspected through ``ninja -t deps``.
* Makefile builds are inspected through ``compiler_depend.make`` or ``*.o.d``.

An unavailable, stale, incomplete, or empty dependency population is a hard
failure.  Every object belonging to each oracle ownership target is checked.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
from typing import Iterable


PROJECTION_TARGET = "ai-openai-codex-frontend-legacy-projection-oracle"
SERVER_TARGET = "ai-openai-codex-frontend-legacy-server-oracle"
CLIENT_TARGET = "ai-openai-codex-frontend-legacy-client-oracle"
TARGETS = (PROJECTION_TARGET, SERVER_TARGET, CLIENT_TARGET)

ORACLE_INCLUDE_ROOT = Path("tests/component/codex/oracle/include")
DETAIL_SUFFIX = Path("ai/openai/codex/frontend/detail")
DETAIL_HEADERS = (
    "BackendProjectionBuilder.h",
    "FrontendCapabilities.h",
    "FrontendProjection.h",
)

# These source-to-header relationships make the dependency proof positive and
# non-vacuous.  They also identify the five translation units whose evidence is
# required explicitly by the P3 cutover contract.
KNOWN_TRANSLATION_UNITS: dict[str, dict[str, tuple[str, ...]]] = {
    PROJECTION_TARGET: {
        "BackendProjectionBuilder.cpp": (
            "BackendProjectionBuilder.h",
            "FrontendProjection.h",
        ),
        "FrontendCapabilities.cpp": ("FrontendCapabilities.h",),
        "FrontendProjection.cpp": ("FrontendProjection.h",),
    },
    SERVER_TARGET: {
        "LegacyFrontendService.cpp": DETAIL_HEADERS,
    },
    CLIENT_TARGET: {
        "State.cpp": ("FrontendProjection.h",),
    },
}


class ProofFailure(AssertionError):
    """A deterministic failure of the dependency-resolution proof."""


@dataclass(frozen=True)
class DependencyRecord:
    target: str
    object_name: str
    dependencies: frozenset[Path]


def fail(message: str) -> None:
    raise ProofFailure(message)


def run(command: list[str], *, cwd: Path) -> str:
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        details = completed.stderr.strip() or completed.stdout.strip() or "no diagnostic"
        fail(
            f"dependency-data command failed ({completed.returncode}): "
            f"{shlex.join(command)}: {details}"
        )
    return completed.stdout


def normalized_path(value: str | Path, *, relative_to: Path) -> Path:
    text = str(value).strip()
    if not text:
        fail("dependency data contained an empty path")
    path = Path(text)
    if not path.is_absolute():
        path = relative_to / path
    return path.resolve(strict=False)


def normalized_repo_source(value: object, *, repo_root: Path) -> Path:
    if not isinstance(value, str) or not value.strip():
        fail(f"invalid source-authority path: {value!r}")
    source = normalized_path(value, relative_to=repo_root)
    try:
        source.relative_to(repo_root)
    except ValueError:
        fail(f"source-authority path escapes the repository: {value}")
    return source


def add_authority_entry(
    result: dict[str, list[Path]],
    target: object,
    source: object,
    *,
    repo_root: Path,
) -> None:
    if not isinstance(target, str) or target not in TARGETS:
        fail(f"source authority names an unknown oracle target: {target!r}")
    normalized = normalized_repo_source(source, repo_root=repo_root)
    if normalized in result[target]:
        fail(f"duplicate source-authority entry for {target}: {source}")
    result[target].append(normalized)


def parse_json_authority(data: object, *, repo_root: Path) -> dict[str, list[Path]]:
    result = {target: [] for target in TARGETS}
    aliases = {
        "projection": PROJECTION_TARGET,
        "shared": PROJECTION_TARGET,
        "server": SERVER_TARGET,
        "client": CLIENT_TARGET,
    }

    if not isinstance(data, dict):
        fail("JSON source authority must be an object")

    mapping: object | None = None
    for key in ("targets", "targetSources", "oracleTargets"):
        candidate = data.get(key)
        if candidate is not None:
            mapping = candidate
            break

    if mapping is not None:
        if not isinstance(mapping, dict):
            fail("JSON source-authority target mapping must be an object")
        for raw_name, raw_value in mapping.items():
            target = aliases.get(raw_name, raw_name)
            sources: object
            if isinstance(raw_value, list):
                sources = raw_value
            elif isinstance(raw_value, dict):
                target = raw_value.get("target", target)
                sources = raw_value.get("sources", raw_value.get("implementationSources"))
            else:
                fail(f"invalid source-authority entry for {raw_name!r}")
            if target not in TARGETS:
                # Additional non-oracle metadata/targets may share the authority
                # file; only explicitly source-bearing unknown entries are bad.
                if sources:
                    fail(f"source authority names an unknown oracle target: {target!r}")
                continue
            if not isinstance(sources, list):
                fail(f"source-authority sources for {target} must be a list")
            for source in sources:
                if isinstance(source, dict):
                    source = source.get("path", source.get("source"))
                add_authority_entry(result, target, source, repo_root=repo_root)

    flat_sources = data.get("sources")
    if isinstance(flat_sources, list) and not any(result.values()):
        for entry in flat_sources:
            if not isinstance(entry, dict):
                fail("flat JSON source-authority entries must be objects")
            add_authority_entry(
                result,
                entry.get("target"),
                entry.get("path", entry.get("source")),
                repo_root=repo_root,
            )

    return result


def parse_source_authority(path: Path, *, repo_root: Path) -> dict[str, list[Path]]:
    if not path.is_file():
        fail(f"source authority is missing: {path}")
    text = path.read_text(encoding="utf-8")
    stripped = text.lstrip()
    if stripped.startswith("{") or stripped.startswith("["):
        try:
            result = parse_json_authority(json.loads(text), repo_root=repo_root)
        except json.JSONDecodeError as error:
            fail(f"invalid JSON source authority: {error}")
    else:
        result = {target: [] for target in TARGETS}
        for line_number, raw_line in enumerate(text.splitlines(), start=1):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            fields = [field.strip() for field in line.split("|")]
            if len(fields) != 2 or not all(fields):
                fail(
                    f"invalid source-authority line {line_number}; expected "
                    "target|repository-relative-source"
                )
            add_authority_entry(result, fields[0], fields[1], repo_root=repo_root)

    for target in TARGETS:
        if not result[target]:
            fail(f"source authority contains no implementation sources for {target}")
        for source in result[target]:
            if not source.is_file():
                fail(f"authoritative oracle source is missing: {source}")
    return result


def parse_ninja_dependency_output(
    output: str,
    *,
    target: str,
    expected_object: str,
    build_dir: Path,
) -> DependencyRecord:
    lines = output.splitlines()
    if not lines:
        fail(f"Ninja returned empty dependency data for {expected_object}")
    header = lines[0]
    match = re.fullmatch(
        r"(.+): #deps ([0-9]+), deps mtime .+ \((VALID|STALE)\)",
        header,
    )
    if match is None:
        fail(f"Ninja dependency data unavailable for {expected_object}: {header}")
    reported_object, raw_count, state = match.groups()
    if reported_object.replace("\\", "/") != expected_object.replace("\\", "/"):
        fail(
            f"Ninja returned dependency data for the wrong object: "
            f"expected {expected_object}, got {reported_object}"
        )
    if state != "VALID":
        fail(f"Ninja dependency data is stale for {expected_object}")
    dependencies = frozenset(
        normalized_path(line.strip(), relative_to=build_dir)
        for line in lines[1:]
        if line.strip()
    )
    expected_count = int(raw_count)
    if expected_count == 0 or not dependencies:
        fail(f"Ninja dependency record is empty for {expected_object}")
    if len(dependencies) != expected_count:
        fail(
            f"Ninja dependency record count mismatch for {expected_object}: "
            f"reported {expected_count}, parsed {len(dependencies)}"
        )
    return DependencyRecord(target, reported_object, dependencies)


def collect_ninja_records(
    *,
    make_program: str,
    build_dir: Path,
) -> tuple[dict[str, list[DependencyRecord]], str]:
    targets_output = run(
        [make_program, "-C", str(build_dir), "-t", "targets", "all"],
        cwd=build_dir,
    )
    object_names = {target: [] for target in TARGETS}
    for raw_line in targets_output.splitlines():
        if ": " not in raw_line:
            continue
        output_name = raw_line.split(": ", 1)[0]
        normalized_name = output_name.replace("\\", "/")
        for target in TARGETS:
            marker = f"CMakeFiles/{target}.dir/"
            if marker in normalized_name and normalized_name.endswith((".o", ".obj")):
                object_names[target].append(output_name)

    records = {target: [] for target in TARGETS}
    for target in TARGETS:
        names = sorted(set(object_names[target]))
        if not names:
            fail(f"Ninja target graph contains no compiled objects for {target}")
        for object_name in names:
            dependency_output = run(
                [make_program, "-C", str(build_dir), "-t", "deps", object_name],
                cwd=build_dir,
            )
            records[target].append(
                parse_ninja_dependency_output(
                    dependency_output,
                    target=target,
                    expected_object=object_name,
                    build_dir=build_dir,
                )
            )
    return records, "ninja -t deps"


def parse_make_rules(text: str, *, relative_to: Path, target: str) -> list[DependencyRecord]:
    # Compiler depfiles use escaped newlines for one make rule per object.
    logical_text = re.sub(r"\\\r?\n", " ", text)
    records: list[DependencyRecord] = []
    for line in logical_text.splitlines():
        if ":" not in line:
            continue
        raw_object, raw_dependencies = line.split(":", 1)
        raw_object = raw_object.strip()
        if not raw_object.replace("\\", "/").endswith((".o", ".obj")):
            continue
        try:
            dependency_fields = shlex.split(raw_dependencies, posix=True)
        except ValueError as error:
            fail(f"cannot parse Make dependency rule for {raw_object}: {error}")
        dependencies = frozenset(
            normalized_path(field, relative_to=relative_to) for field in dependency_fields
        )
        if not dependencies:
            fail(f"Make dependency record is empty for {raw_object}")
        records.append(DependencyRecord(target, raw_object, dependencies))
    return records


def collect_make_records(*, build_dir: Path) -> tuple[dict[str, list[DependencyRecord]], str]:
    records = {target: [] for target in TARGETS}
    mechanisms: set[str] = set()
    for target in TARGETS:
        target_dirs = sorted(
            path
            for path in build_dir.rglob(f"{target}.dir")
            if path.is_dir() and path.parent.name == "CMakeFiles"
        )
        if not target_dirs:
            fail(f"Make build has no CMake target directory for {target}")

        for target_dir in target_dirs:
            compiler_records = sorted(target_dir.rglob("compiler_depend.make"))
            parsed: list[DependencyRecord] = []
            if compiler_records:
                for dependency_file in compiler_records:
                    parsed.extend(
                        parse_make_rules(
                            dependency_file.read_text(encoding="utf-8", errors="strict"),
                            relative_to=build_dir,
                            target=target,
                        )
                    )
                if parsed:
                    mechanisms.add("compiler_depend.make")
            if not parsed:
                depfiles = sorted(target_dir.rglob("*.o.d")) + sorted(target_dir.rglob("*.obj.d"))
                if not depfiles:
                    fail(f"Make dependency data is unavailable for {target}")
                for dependency_file in depfiles:
                    file_records = parse_make_rules(
                        dependency_file.read_text(encoding="utf-8", errors="strict"),
                        relative_to=build_dir,
                        target=target,
                    )
                    if len(file_records) != 1:
                        fail(
                            f"expected one object rule in {dependency_file}, "
                            f"found {len(file_records)}"
                        )
                    parsed.extend(file_records)
                mechanisms.add("compiler-generated *.o.d")
            records[target].extend(parsed)

        if not records[target]:
            fail(f"Make dependency population is empty for {target}")

        # A compiler_depend.make may repeat a logical object rule.  Merge only
        # exact object identities while retaining the union of all dependencies.
        merged: dict[str, set[Path]] = {}
        for record in records[target]:
            merged.setdefault(record.object_name, set()).update(record.dependencies)
        records[target] = [
            DependencyRecord(target, object_name, frozenset(dependencies))
            for object_name, dependencies in sorted(merged.items())
        ]

    mechanism = "Make " + " + ".join(sorted(mechanisms))
    return records, mechanism


def object_for_source(
    records: Iterable[DependencyRecord],
    source: Path,
    *,
    target: str,
) -> DependencyRecord:
    matches = [record for record in records if source in record.dependencies]
    if len(matches) != 1:
        fail(
            f"expected exactly one parsed object for authoritative source "
            f"{source} in {target}, found {len(matches)}"
        )
    return matches[0]


def verify_records(
    records: dict[str, list[DependencyRecord]],
    authority: dict[str, list[Path]],
    *,
    repo_root: Path,
) -> tuple[dict[str, list[str]], dict[str, int]]:
    evidence: dict[str, list[str]] = {target: [] for target in TARGETS}
    frozen_counts = {header: 0 for header in DETAIL_HEADERS}
    frozen_paths = {
        header: (repo_root / ORACLE_INCLUDE_ROOT / DETAIL_SUFFIX / header).resolve(strict=False)
        for header in DETAIL_HEADERS
    }
    live_paths = {
        header: (
            repo_root / "src/ai/openai/codex/frontend/detail" / header
        ).resolve(strict=False)
        for header in DETAIL_HEADERS
    }

    all_dependencies: set[Path] = set()
    for target in TARGETS:
        target_records = records.get(target, [])
        if not target_records:
            fail(f"no parsed dependency records for {target}")
        if len(target_records) != len(authority[target]):
            fail(
                f"object/source closure mismatch for {target}: parsed "
                f"{len(target_records)} objects, authority lists "
                f"{len(authority[target])} sources"
            )

        seen_objects: set[str] = set()
        for record in target_records:
            normalized_object = record.object_name.replace("\\", "/")
            if normalized_object in seen_objects:
                fail(f"duplicate dependency record for {target}: {record.object_name}")
            seen_objects.add(normalized_object)
            if not record.dependencies:
                fail(f"empty dependency record for {target}: {record.object_name}")
            all_dependencies.update(record.dependencies)

        for source in authority[target]:
            object_for_source(target_records, source, target=target)

        for source_name, expected_headers in KNOWN_TRANSLATION_UNITS[target].items():
            source_matches = [source for source in authority[target] if source.name == source_name]
            if len(source_matches) != 1:
                fail(
                    f"known translation unit {source_name} is not uniquely present "
                    f"in {target}'s source authority"
                )
            record = object_for_source(target_records, source_matches[0], target=target)
            normalized_object = record.object_name.replace("\\", "/")
            if not normalized_object.endswith((f"/{source_name}.o", f"/{source_name}.obj")):
                fail(
                    f"known translation unit {source_name} resolved to unexpected "
                    f"object identity: {record.object_name}"
                )
            for header in expected_headers:
                expected_path = frozen_paths[header]
                if expected_path not in record.dependencies:
                    fail(
                        f"{target} object {record.object_name} did not resolve "
                        f"{header} beneath the frozen oracle include root"
                    )
            evidence[target].append(f"{source_name} -> {record.object_name}")

    for header in DETAIL_HEADERS:
        frozen_counts[header] = sum(
            frozen_paths[header] in record.dependencies
            for target in TARGETS
            for record in records[target]
        )
        if frozen_counts[header] == 0:
            fail(
                f"frozen header was never observed in compiler dependency data: "
                f"{frozen_paths[header]}"
            )
        if live_paths[header] in all_dependencies:
            offenders = [
                f"{target}:{record.object_name}"
                for target in TARGETS
                for record in records[target]
                if live_paths[header] in record.dependencies
            ]
            fail(
                f"live production detail header leaked into oracle dependency closure: "
                f"{live_paths[header]} ({', '.join(offenders)})"
            )

    return evidence, frozen_counts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--make-program", required=True)
    parser.add_argument("--source-authority", type=Path, required=True)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    build_dir = args.build_dir.resolve()
    source_authority = args.source_authority
    if not source_authority.is_absolute():
        source_authority = (repo_root / source_authority).resolve()
    if not build_dir.is_dir():
        fail(f"build directory is missing: {build_dir}")

    authority = parse_source_authority(source_authority, repo_root=repo_root)
    generator = args.generator.casefold()
    if "ninja" in generator:
        records, mechanism = collect_ninja_records(
            make_program=args.make_program,
            build_dir=build_dir,
        )
    elif "makefiles" in generator or generator.endswith("make"):
        records, mechanism = collect_make_records(build_dir=build_dir)
    else:
        fail(
            f"unsupported CMake generator {args.generator!r}; cannot locate and "
            "parse compiler dependency data"
        )

    evidence, frozen_counts = verify_records(records, authority, repo_root=repo_root)

    print("P3 oracle dependency resolution:")
    print(f"  generator: {args.generator}")
    print(f"  dependency-data mechanism: {mechanism}")
    for target in TARGETS:
        print(f"  {target}: {len(records[target])} parsed object(s)")
        for item in evidence[target]:
            print(f"    known TU: {item}")
    for header in DETAIL_HEADERS:
        frozen = repo_root / ORACLE_INCLUDE_ROOT / DETAIL_SUFFIX / header
        live = repo_root / "src/ai/openai/codex/frontend/detail" / header
        print(f"  frozen dependency: {frozen.resolve()} ({frozen_counts[header]} object(s))")
        print(f"  live dependency: {live.resolve()} (0 object(s))")
    print("P3 oracle dependency resolution proof passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ProofFailure, UnicodeError) as error:
        print(f"P3 oracle dependency resolution failure: {error}", file=sys.stderr)
        raise SystemExit(1)
