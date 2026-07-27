#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

PATHS = [
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
]


def run(args: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None,
        input_bytes: bytes | None = None, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        args,
        cwd=cwd,
        env=env,
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=check,
    )


def git(src: Path, *args: str, input_bytes: bytes | None = None) -> bytes:
    return run(["git", "-C", str(src), *args], input_bytes=input_bytes).stdout


def parse_identity(raw: str, kind: str) -> tuple[str, str, str]:
    match = re.match(rf"^{kind} (.*) <([^>]*)> (\d+ [+-]\d{{4}})$", raw)
    if not match:
        raise RuntimeError(f"unable to parse {kind}: {raw!r}")
    return match.group(1), match.group(2), match.group(3)


def commit_metadata(src: Path, commit: str) -> tuple[dict[str, str], bytes]:
    raw = git(src, "cat-file", "commit", commit)
    header, message = raw.split(b"\n\n", 1)
    author_line = None
    committer_line = None
    for line in header.decode("utf-8", "surrogateescape").splitlines():
        if line.startswith("author "):
            author_line = line
        elif line.startswith("committer "):
            committer_line = line
    if author_line is None or committer_line is None:
        raise RuntimeError(f"missing identity metadata in {commit}")
    an, ae, ad = parse_identity(author_line, "author")
    cn, ce, cd = parse_identity(committer_line, "committer")
    env = os.environ.copy()
    env.update({
        "GIT_AUTHOR_NAME": an,
        "GIT_AUTHOR_EMAIL": ae,
        "GIT_AUTHOR_DATE": ad,
        "GIT_COMMITTER_NAME": cn,
        "GIT_COMMITTER_EMAIL": ce,
        "GIT_COMMITTER_DATE": cd,
    })
    return env, message


def reduce_tips(src: Path, tips: list[str]) -> list[str]:
    ordered: list[str] = []
    for tip in tips:
        if tip not in ordered:
            ordered.append(tip)
    result: list[str] = []
    for candidate in ordered:
        redundant = False
        for other in ordered:
            if candidate == other:
                continue
            proc = run(
                ["git", "-C", str(src), "merge-base", "--is-ancestor", candidate, other],
                check=False,
            )
            if proc.returncode == 0:
                redundant = True
                break
        if not redundant:
            result.append(candidate)
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--map", dest="map_path", type=Path, required=True)
    args = parser.parse_args()

    src = args.source.resolve()
    out = args.output.resolve()
    if out.exists():
        shutil.rmtree(out)
    run(["git", "init", "--bare", str(out)])

    source_objects = (src / ".git" / "objects").resolve()
    dest_objects = (out / "objects").resolve()
    base_env = os.environ.copy()
    base_env.update({
        "GIT_DIR": str(out),
        "GIT_OBJECT_DIRECTORY": str(dest_objects),
        "GIT_ALTERNATE_OBJECT_DIRECTORIES": str(source_objects),
    })

    relevant_raw = git(
        src,
        "log",
        "--full-history",
        "--topo-order",
        "--reverse",
        "--format=%H",
        args.source_commit,
        "--",
        *PATHS,
    ).decode().splitlines()
    relevant = set(relevant_raw)
    all_commits = git(
        src, "rev-list", "--topo-order", "--reverse", args.source_commit
    ).decode().splitlines()

    nearest: dict[str, list[str]] = {}
    dest_map: dict[str, str] = {}
    commit_records: list[dict[str, object]] = []

    with tempfile.TemporaryDirectory(prefix="aisuite-filter-index-") as tmp:
        index = Path(tmp) / "index"
        for ordinal, source_commit in enumerate(all_commits, start=1):
            parent_line = git(src, "rev-list", "--parents", "-n", "1", source_commit).decode().strip().split()
            source_parents = parent_line[1:]
            inherited: list[str] = []
            for parent in source_parents:
                inherited.extend(nearest.get(parent, []))
            inherited = reduce_tips(src, inherited)

            if source_commit not in relevant:
                nearest[source_commit] = inherited
                continue

            env = base_env.copy()
            env["GIT_INDEX_FILE"] = str(index)
            if index.exists():
                index.unlink()
            run(["git", "read-tree", "--empty"], env=env)
            tree_entries = git(
                src, "ls-tree", "-r", "-z", source_commit, "--", *PATHS
            )
            if tree_entries:
                run(["git", "update-index", "-z", "--index-info"], env=env, input_bytes=tree_entries)
            tree = run(["git", "write-tree"], env=env).stdout.decode().strip()

            metadata_env, message = commit_metadata(src, source_commit)
            commit_env = env.copy()
            commit_env.update({k: v for k, v in metadata_env.items() if k.startswith("GIT_AUTHOR_") or k.startswith("GIT_COMMITTER_")})
            cmd = ["git", "commit-tree", tree]
            dest_parents: list[str] = []
            for parent_source in inherited:
                parent_dest = dest_map[parent_source]
                cmd.extend(["-p", parent_dest])
                dest_parents.append(parent_dest)
            dest_commit = run(cmd, env=commit_env, input_bytes=message).stdout.decode().strip()
            dest_map[source_commit] = dest_commit
            nearest[source_commit] = [source_commit]
            subject = git(src, "show", "-s", "--format=%s", source_commit).decode().strip()
            commit_records.append({
                "ordinal": len(commit_records) + 1,
                "source": source_commit,
                "filtered": dest_commit,
                "source_parents": source_parents,
                "filtered_parents": dest_parents,
                "tree": tree,
                "subject": subject,
            })
            print(f"[{len(commit_records):02d}/{len(relevant)}] {source_commit[:10]} -> {dest_commit[:10]} {subject}")

    final_source = relevant_raw[-1]
    final_dest = dest_map[final_source]
    run(["git", "update-ref", "refs/heads/master", final_dest], env=base_env)
    run(["git", "symbolic-ref", "HEAD", "refs/heads/master"], env=base_env)
    # Copy all reachable alternate objects into the destination object database.
    run(["git", "repack", "-a", "-d"], env=base_env)
    # Verify self containment without alternates.
    clean_env = os.environ.copy()
    clean_env["GIT_DIR"] = str(out)
    run(["git", "fsck", "--full", "--strict"], env=clean_env)

    report = {
        "format_version": 1,
        "source_repository": "https://github.com/SNodeC/snode.c",
        "source_commit": args.source_commit,
        "source_tree": git(src, "rev-parse", f"{args.source_commit}^{{tree}}").decode().strip(),
        "selected_paths": PATHS,
        "retained_commit_count": len(commit_records),
        "filtered_head": final_dest,
        "filtered_tree": run(["git", "--git-dir", str(out), "rev-parse", f"{final_dest}^{{tree}}"]).stdout.decode().strip(),
        "commits": commit_records,
    }
    args.map_path.parent.mkdir(parents=True, exist_ok=True)
    args.map_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({k: report[k] for k in ("source_commit", "source_tree", "retained_commit_count", "filtered_head", "filtered_tree")}, indent=2))


if __name__ == "__main__":
    main()
