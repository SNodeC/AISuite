#!/usr/bin/env python3

"""Build the shared frontend protocol in the supported minimal P2 configuration."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


CONFIGURE_TIMEOUT_SECONDS = 180
BUILD_TIMEOUT_SECONDS = 600
BUILD_PARALLELISM = 4


def run(
    command: list[str], *, cwd: Path, environment: dict[str, str], timeout: int
) -> None:
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )


def require_cache_value(cache: str, name: str, expected: str) -> None:
    entry = f"{name}:BOOL={expected}"
    if entry not in cache.splitlines():
        raise RuntimeError(f"minimal configuration cache does not contain {entry}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--snodec-dir", type=Path, required=True)
    arguments = parser.parse_args()

    source_dir = arguments.source_dir.resolve()
    parent_build_dir = arguments.build_dir.resolve()
    snodec_package_dir = arguments.snodec_dir.resolve()
    snodec_prefix = snodec_package_dir.parent.parent.parent
    parent_build_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(
        prefix="aisuite-p2-protocol-minimal-", dir=parent_build_dir
    ) as temporary:
        root = Path(temporary)
        nested_build_dir = root / "build"
        compiler_temporary = root / "compiler-tmp"
        compiler_temporary.mkdir()

        environment = os.environ.copy()
        for name in ("TMPDIR", "TMP", "TEMP"):
            environment[name] = str(compiler_temporary)

        run(
            [
                "cmake",
                "-S",
                str(source_dir),
                "-B",
                str(nested_build_dir),
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Debug",
                f"-DCMAKE_PREFIX_PATH={snodec_prefix}",
                f"-Dsnodec_DIR={snodec_package_dir}",
                "-DAISUITE_BUILD_CODEX_FRONTEND_CLIENT=OFF",
                "-DAISUITE_BUILD_APPS=OFF",
                "-DAISUITE_BUILD_TESTS=OFF",
                "-DCMAKE_JOB_POOLS=aisuite_compile=4;aisuite_link=2",
                "-DCMAKE_JOB_POOL_COMPILE=aisuite_compile",
                "-DCMAKE_JOB_POOL_LINK=aisuite_link",
            ],
            cwd=root,
            environment=environment,
            timeout=CONFIGURE_TIMEOUT_SECONDS,
        )

        cache = (nested_build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
        require_cache_value(cache, "AISUITE_BUILD_CODEX_FRONTEND_CLIENT", "OFF")
        require_cache_value(cache, "AISUITE_BUILD_APPS", "OFF")
        require_cache_value(cache, "AISUITE_BUILD_TESTS", "OFF")

        run(
            [
                "cmake",
                "--build",
                str(nested_build_dir),
                "--target",
                "ai-openai-codex-frontend-protocol",
                "--parallel",
                str(BUILD_PARALLELISM),
            ],
            cwd=root,
            environment=environment,
            timeout=BUILD_TIMEOUT_SECONDS,
        )

        protocol_dso = (
            nested_build_dir
            / "src/ai/openai/codex/frontend"
            / "libaisuite-openai-codex-frontend-protocol.so"
        )
        if not protocol_dso.is_file():
            raise RuntimeError(f"minimal protocol DSO was not produced: {protocol_dso}")

    print(
        "Codex frontend protocol minimal configuration passed: "
        "client=OFF, apps=OFF, tests=OFF, target-only parallel=4"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
