#!/usr/bin/env python3

"""Run an unrelinked P0 frontend consumer against the P2 ELF DSOs."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile


EXPECTED_OUTPUT = "p0-linked-frontend-consumer-ok\n"
NEEDED_ENTRY = re.compile(r"\(NEEDED\).*Shared library: \[([^]]+)\]")
LDD_RESOLUTION = re.compile(r"^\s*(\S+)\s+=>\s+(.+?)(?:\s+\(0x[0-9a-fA-F]+\))?\s*$")


def run(command: list[str], *, cwd: Path, env: dict[str, str] | None = None, timeout: int = 300) -> str:
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=env,
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
    return completed.stdout


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def runtime_environment(aisuite_prefix: Path, snodec_prefix: Path) -> dict[str, str]:
    environment = {name: value for name, value in os.environ.items() if not name.startswith("LD_")}
    environment["LD_LIBRARY_PATH"] = ":".join(
        [
            str(aisuite_prefix / "lib"),
            str(snodec_prefix / "lib"),
            str(snodec_prefix / "lib/snode.c/web/http"),
            str(snodec_prefix / "lib/snode.c/web/http/upgrade"),
        ]
    )
    return environment


def needed_libraries(path: Path, *, cwd: Path) -> set[str]:
    dynamic = run(["readelf", "-d", str(path)], cwd=cwd, timeout=30)
    return {match.group(1) for match in NEEDED_ENTRY.finditer(dynamic)}


def resolved_libraries(path: Path, *, cwd: Path, env: dict[str, str]) -> dict[str, Path]:
    linked = run(["ldd", str(path)], cwd=cwd, env=env, timeout=30)
    resolved: dict[str, Path] = {}
    for line in linked.splitlines():
        match = LDD_RESOLUTION.match(line)
        if match is None:
            continue
        soname, location = match.groups()
        if location == "not found":
            raise RuntimeError(f"runtime dependency is unresolved: {soname}")
        resolved[soname] = Path(location).resolve()
    return resolved


def require_loaded_from(
    resolved: dict[str, Path], soname: str, expected: Path, *, description: str
) -> None:
    actual = resolved.get(soname)
    expected = expected.resolve()
    if actual != expected:
        raise RuntimeError(f"{description} resolved to {actual}, expected {expected}")


def strong_defined_symbols(path: Path, *, dynamic: bool, cwd: Path) -> set[str]:
    command = ["nm"]
    if dynamic:
        command.append("--dynamic")
    command.extend(["--extern-only", "--defined-only", "--no-demangle", "--format=posix", str(path)])
    output = run(command, cwd=cwd, timeout=60)
    symbols: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1].isupper() and fields[1] not in {"U", "V", "W"}:
            symbols.add(fields[0])
    return symbols


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--snodec-dir", type=Path, required=True)
    parser.add_argument("--p0-base", required=True)
    parser.add_argument("--temp-root", type=Path)
    parser.add_argument("--parallel", type=int, default=4)
    arguments = parser.parse_args()

    source_dir = arguments.source_dir.resolve()
    build_dir = arguments.build_dir.resolve()
    snodec_package_dir = arguments.snodec_dir.resolve()
    snodec_prefix = snodec_package_dir.parent.parent.parent
    consumer_source = source_dir / "tests/component/codex/CodexFrontendLegacyBinaryConsumer.cpp"

    temporary_root = arguments.temp_root.resolve() if arguments.temp_root else None
    with tempfile.TemporaryDirectory(prefix="aisuite-p2-legacy-binary-", dir=temporary_root) as temporary:
        root = Path(temporary)
        compiler_temporary = root / "compiler-tmp"
        compiler_temporary.mkdir()
        os.environ["TMPDIR"] = str(compiler_temporary)
        archive = root / "p0.tar"
        p0_source = root / "p0-source"
        p0_build = root / "p0-build"
        p0_install = root / "p0-install"
        p2_install = root / "p2-install"
        consumer_project = root / "consumer-source"
        consumer_build = root / "consumer-build"

        run(
            ["git", "archive", "--format=tar", f"--output={archive}", arguments.p0_base],
            cwd=source_dir,
            timeout=60,
        )
        p0_source.mkdir()
        with tarfile.open(archive) as archive_file:
            archive_file.extractall(p0_source, filter="data")

        run(
            [
                "cmake",
                "-S",
                str(p0_source),
                "-B",
                str(p0_build),
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Debug",
                f"-DCMAKE_INSTALL_PREFIX={p0_install}",
                f"-DCMAKE_PREFIX_PATH={snodec_prefix}",
                f"-Dsnodec_DIR={snodec_package_dir}",
                "-DAISUITE_BUILD_APPS=OFF",
                "-DAISUITE_BUILD_TESTS=OFF",
                "-DAISUITE_BUILD_CODEX_FRONTEND_CLIENT=OFF",
                "-DCMAKE_JOB_POOLS=aisuite_compile=4;aisuite_link=2",
                "-DCMAKE_JOB_POOL_COMPILE=aisuite_compile",
                "-DCMAKE_JOB_POOL_LINK=aisuite_link",
            ],
            cwd=root,
        )
        run(
            ["cmake", "--build", str(p0_build), "--target", "ai-openai-codex-frontend", "--parallel", str(arguments.parallel)],
            cwd=root,
        )
        run(["cmake", "--install", str(p0_build)], cwd=root)
        frontend_soname = "libaisuite-openai-codex-frontend.so.2"
        protocol_soname = "libaisuite-openai-codex-frontend-protocol.so.2"
        p0_frontend_dso = p0_install / "lib" / frontend_soname

        consumer_project.mkdir()
        shutil.copy2(consumer_source, consumer_project / consumer_source.name)
        (consumer_project / "CMakeLists.txt").write_text(
            """cmake_minimum_required(VERSION 3.18)
project(P0LinkedFrontendConsumer LANGUAGES CXX)
find_package(AISuite CONFIG REQUIRED)
add_executable(P0LinkedFrontendConsumer CodexFrontendLegacyBinaryConsumer.cpp)
target_compile_features(P0LinkedFrontendConsumer PRIVATE cxx_std_20)
target_link_libraries(P0LinkedFrontendConsumer PRIVATE AISuite::OpenAICodexFrontend)
""",
            encoding="utf-8",
        )
        run(
            [
                "cmake",
                "-S",
                str(consumer_project),
                "-B",
                str(consumer_build),
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Debug",
                f"-DCMAKE_PREFIX_PATH={p0_install};{snodec_prefix}",
                f"-DCMAKE_EXE_LINKER_FLAGS=-Wl,-rpath-link,{snodec_prefix / 'lib'}",
            ],
            cwd=root,
        )
        run(["cmake", "--build", str(consumer_build), "--parallel", str(arguments.parallel)], cwd=root)

        executable = consumer_build / "P0LinkedFrontendConsumer"
        binary_digest = digest(executable)
        if frontend_soname not in needed_libraries(executable, cwd=root):
            raise RuntimeError(f"the preserved P0 consumer does not directly need {frontend_soname}")

        p0_environment = runtime_environment(p0_install, snodec_prefix)
        p0_resolved = resolved_libraries(executable, cwd=root, env=p0_environment)
        require_loaded_from(
            p0_resolved,
            frontend_soname,
            p0_frontend_dso,
            description="the P0 frontend DSO",
        )
        p0_output = run([str(executable)], cwd=root, env=p0_environment, timeout=30)
        if p0_output != EXPECTED_OUTPUT:
            raise RuntimeError(f"unexpected P0 consumer output: {p0_output!r}")

        run(["cmake", "--install", str(build_dir), "--prefix", str(p2_install)], cwd=root)
        frontend_dso = p2_install / "lib" / frontend_soname
        protocol_dso = p2_install / "lib" / protocol_soname
        if protocol_soname not in needed_libraries(frontend_dso, cwd=root):
            raise RuntimeError("P2 frontend DSO does not directly need the protocol DSO")

        p0_protocol_object_root = (
            p0_build
            / "src/ai/openai/codex/frontend/CMakeFiles/ai-openai-codex-frontend.dir"
        )
        p0_protocol_objects = [
            p0_protocol_object_root / "Codec.cpp.o",
            p0_protocol_object_root / "Messages.cpp.o",
            p0_protocol_object_root / "detail/GeneratedSchemaValidator.cpp.o",
        ]
        missing_objects = [str(path) for path in p0_protocol_objects if not path.is_file()]
        if missing_objects:
            raise RuntimeError(f"P0 protocol object evidence is missing: {missing_objects}")
        p0_protocol_object_symbols: set[str] = set()
        for object_path in p0_protocol_objects:
            p0_protocol_object_symbols.update(strong_defined_symbols(object_path, dynamic=False, cwd=root))
        p0_exported_protocol_symbols = p0_protocol_object_symbols & strong_defined_symbols(
            p0_frontend_dso, dynamic=True, cwd=root
        )
        if not p0_exported_protocol_symbols:
            raise RuntimeError("P0 frontend exposes no strong protocol implementation symbols")

        p2_protocol_symbols = strong_defined_symbols(protocol_dso, dynamic=True, cwd=root)
        missing_protocol_symbols = p0_exported_protocol_symbols - p2_protocol_symbols
        if missing_protocol_symbols:
            sample = ", ".join(sorted(missing_protocol_symbols)[:20])
            raise RuntimeError(
                f"P2 protocol DSO is missing {len(missing_protocol_symbols)} P0 protocol symbols: {sample}"
            )
        duplicate_frontend_symbols = p0_exported_protocol_symbols & strong_defined_symbols(
            frontend_dso, dynamic=True, cwd=root
        )
        if duplicate_frontend_symbols:
            sample = ", ".join(sorted(duplicate_frontend_symbols)[:20])
            raise RuntimeError(
                f"P2 frontend DSO still owns {len(duplicate_frontend_symbols)} protocol symbols: {sample}"
            )

        p2_environment = runtime_environment(p2_install, snodec_prefix)
        p2_resolved = resolved_libraries(executable, cwd=root, env=p2_environment)
        require_loaded_from(
            p2_resolved,
            frontend_soname,
            frontend_dso,
            description="the P2 frontend DSO",
        )
        require_loaded_from(
            p2_resolved,
            protocol_soname,
            protocol_dso,
            description="the P2 protocol DSO",
        )
        p2_install_real = p2_install.resolve()
        for soname, location in p2_resolved.items():
            if soname.startswith("libaisuite-") and not location.is_relative_to(p2_install_real):
                raise RuntimeError(f"P2 runtime resolved {soname} outside its install: {location}")

        p2_output = run([str(executable)], cwd=root, env=p2_environment, timeout=30)
        if p2_output != EXPECTED_OUTPUT:
            raise RuntimeError(f"unexpected P2 consumer output: {p2_output!r}")
        if digest(executable) != binary_digest:
            raise RuntimeError("the preserved P0 consumer binary changed or was relinked")

    print("P0-linked ELF frontend consumer passed unchanged against the P2 protocol DSO")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"legacy binary compatibility failure: {error}", file=sys.stderr)
        raise SystemExit(1)
