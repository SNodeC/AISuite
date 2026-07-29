#!/usr/bin/env python3
"""Capture and verify Codex A1.4 user-integration API/ABI evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

sys.dont_write_bytecode = True


FORMAT_VERSION = 1
CODEX_VERSION = "codex-cli 0.144.6"
UPSTREAM_TAG = "rust-v0.144.6"
BASE_SHA = "10d3829958a6a17e7437326b6c42c51f3a8de4ec"
BASE_TREE = "7b5e6500780f1c633fe18af5fba6164bd222a3ba"
IMPLEMENTATION_SHA = "352d9004a8c3beb3af8876b2a22c7ca4c42d47ec"
IMPLEMENTATION_TREE = "267fdffee9ea7fc6d50b8b5b354497decf516d71"
IMPLEMENTATION_SUBJECT = "Complete Codex plugin source and catalog operations"
EXPECTED_VARIANTS = {
    "CanonicalServerNotification": 57,
    "Event": 59,
    "PluginSource": 5,
}
HEADER_PATHS = (
    "src/ai/openai/codex/AppServerClient.h",
    "src/ai/openai/codex/typed/Apps.h",
    "src/ai/openai/codex/typed/Client.h",
    "src/ai/openai/codex/typed/Events.h",
    "src/ai/openai/codex/typed/ExternalAgents.h",
    "src/ai/openai/codex/typed/Feedback.h",
    "src/ai/openai/codex/typed/Hooks.h",
    "src/ai/openai/codex/typed/Marketplace.h",
    "src/ai/openai/codex/typed/Plugins.h",
    "src/ai/openai/codex/typed/Skills.h",
)
PROBE_PATH = (
    "tests/installed/codex/"
    "CodexA14UserIntegrationsAbiLayoutProbe.cpp"
)


class AbiEvidenceError(RuntimeError):
    """The generated API/ABI evidence is stale or malformed."""


def fail(message: str) -> None:
    raise AbiEvidenceError(
        f"UserIntegrationAbiEvidenceMismatch: {message}"
    )


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def render_json(value: dict[str, Any]) -> str:
    return (
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    )


def run(
    arguments: list[str], *, cwd: Path | None = None
) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        arguments,
        cwd=cwd,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        fail(
            f"command failed ({completed.returncode}): "
            f"{' '.join(arguments)}; stderr={completed.stderr.strip()}"
        )
    return completed


def compiler_path(value: str) -> Path:
    resolved = shutil.which(value)
    if resolved is None:
        fail(f"compiler is unavailable: {value}")
    return Path(resolved).resolve()


def capture_layout(
    repo_root: Path, compiler: Path
) -> tuple[list[str], str]:
    probe = repo_root / PROBE_PATH
    with tempfile.TemporaryDirectory(
        prefix="aisuite-a14-user-integrations-abi-"
    ) as temporary:
        binary = Path(temporary) / "layout-probe"
        run(
            [
                str(compiler),
                "-std=c++20",
                f"-I{repo_root / 'src'}",
                str(probe),
                "-o",
                str(binary),
            ],
            cwd=repo_root,
        )
        completed = run([str(binary)], cwd=repo_root)
        lines = completed.stdout.splitlines()
        binary_sha256 = sha256_file(binary)

    if not lines or len(lines) != len(set(lines)):
        fail("layout probe output is empty or contains duplicate records")
    observed_variants: dict[str, int] = {}
    for line in lines:
        if "|alternatives=" not in line:
            continue
        name, raw = line.split("|alternatives=", 1)
        try:
            observed_variants[name] = int(raw)
        except ValueError:
            fail(f"malformed variant probe line: {line}")
    if observed_variants != EXPECTED_VARIANTS:
        fail(
            "variant probe changed: "
            f"expected {EXPECTED_VARIANTS}, got {observed_variants}"
        )
    return lines, binary_sha256


def strong_dynamic_symbols(library: Path) -> list[str]:
    nm = shutil.which("nm")
    if nm is None:
        fail("nm is unavailable")
    completed = run(
        [nm, "-D", "--defined-only", "--format=posix", str(library)]
    )
    symbols: set[str] = set()
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] in {"B", "D", "R", "T"}:
            symbols.add(fields[0])
    if not symbols:
        fail(f"no strong dynamic symbols found in {library}")
    return sorted(symbols)


def symbols_text(symbols: list[str]) -> str:
    return "".join(f"{symbol}\n" for symbol in symbols)


def header_hashes(repo_root: Path) -> dict[str, str]:
    return {
        relative: sha256_file(repo_root / relative)
        for relative in HEADER_PATHS
    }


def build_report(
    repo_root: Path,
    compiler: Path,
    library: Path,
    symbol_output: Path,
) -> tuple[dict[str, Any], str]:
    layout_lines, binary_sha256 = capture_layout(repo_root, compiler)
    symbols = strong_dynamic_symbols(library)
    symbol_content = symbols_text(symbols)
    compiler_identity = run([str(compiler), "--version"]).stdout.splitlines()
    if not compiler_identity:
        fail("compiler identity is empty")
    report: dict[str, Any] = {
        "authority": {
            "base_sha": BASE_SHA,
            "base_tree": BASE_TREE,
            "codex_version": CODEX_VERSION,
            "implementation_sha": IMPLEMENTATION_SHA,
            "implementation_subject": IMPLEMENTATION_SUBJECT,
            "implementation_tree": IMPLEMENTATION_TREE,
            "upstream_tag": UPSTREAM_TAG,
        },
        "compiler": {
            "binary": str(compiler),
            "binary_sha256": sha256_file(compiler),
            "identity": compiler_identity[0],
            "language": "C++20",
        },
        "conclusion": {
            "binary_compatible": False,
            "installed_consumers_must_rebuild": True,
            "reason": (
                "Public aggregates and std::variant layouts changed; "
                "unchanged SOVERSION and symbol retention are not "
                "binary-compatibility proof."
            ),
            "soversion": 1,
        },
        "format_version": FORMAT_VERSION,
        "generated_notice": (
            "Generated by tools/codex/"
            "app_server_a1_4_user_integrations_abi.py; do not edit."
        ),
        "layout_probe": {
            "binary_sha256": binary_sha256,
            "compile_command": (
                "{compiler} -std=c++20 -I{repo}/src "
                f"{{repo}}/{PROBE_PATH} -o {{probe_binary}}"
            ),
            "header_sha256": header_hashes(repo_root),
            "source": PROBE_PATH,
            "source_sha256": sha256_file(repo_root / PROBE_PATH),
            "stdout_lines": layout_lines,
            "stdout_sha256": sha256_bytes(
                ("".join(f"{line}\n" for line in layout_lines)).encode()
            ),
            "variant_alternatives": EXPECTED_VARIANTS,
        },
        "shared_library_symbols": {
            "extract_policy": (
                "nm -D --defined-only --format=posix; retain strong "
                "B/D/R/T symbols; LC_ALL=C lexical unique sort"
            ),
            "library_basename": library.name,
            "library_sha256": sha256_file(library),
            "symbol_count": len(symbols),
            "symbol_list": symbol_output.relative_to(repo_root).as_posix(),
            "symbol_list_sha256": sha256_bytes(
                symbol_content.encode("utf-8")
            ),
        },
    }
    return report, symbol_content


def validate_report(
    repo_root: Path,
    compiler: Path,
    library: Path | None,
    report: dict[str, Any],
    symbol_output: Path,
) -> None:
    authority = report.get("authority")
    if authority != {
        "base_sha": BASE_SHA,
        "base_tree": BASE_TREE,
        "codex_version": CODEX_VERSION,
        "implementation_sha": IMPLEMENTATION_SHA,
        "implementation_subject": IMPLEMENTATION_SUBJECT,
        "implementation_tree": IMPLEMENTATION_TREE,
        "upstream_tag": UPSTREAM_TAG,
    }:
        fail("authority changed")
    if report.get("format_version") != FORMAT_VERSION:
        fail("format version changed")
    conclusion = report.get("conclusion")
    if not isinstance(conclusion, dict) or conclusion.get("soversion") != 1:
        fail("SOVERSION conclusion changed")
    if conclusion.get("binary_compatible") is not False:
        fail("evidence incorrectly claims binary compatibility")

    probe = report.get("layout_probe")
    if not isinstance(probe, dict):
        fail("layout probe record is missing")
    if probe.get("source") != PROBE_PATH:
        fail("layout probe source changed")
    if probe.get("source_sha256") != sha256_file(repo_root / PROBE_PATH):
        fail("layout probe source hash is stale")
    if probe.get("header_sha256") != header_hashes(repo_root):
        fail("public header hashes are stale")
    live_lines, _ = capture_layout(repo_root, compiler)
    if probe.get("stdout_lines") != live_lines:
        fail("captured public layouts are stale")
    live_stdout_hash = sha256_bytes(
        ("".join(f"{line}\n" for line in live_lines)).encode()
    )
    if probe.get("stdout_sha256") != live_stdout_hash:
        fail("layout stdout hash is stale")
    if probe.get("variant_alternatives") != EXPECTED_VARIANTS:
        fail("captured public variant sizes changed")

    symbols_record = report.get("shared_library_symbols")
    if not isinstance(symbols_record, dict) or not symbol_output.is_file():
        fail("symbol evidence is missing")
    stored_symbols = symbol_output.read_text(encoding="utf-8")
    if (
        symbols_record.get("symbol_list_sha256")
        != sha256_bytes(stored_symbols.encode("utf-8"))
        or symbols_record.get("symbol_count")
        != len(stored_symbols.splitlines())
    ):
        fail("stored symbol manifest is stale")
    if library is not None:
        live_symbols = symbols_text(strong_dynamic_symbols(library))
        if stored_symbols != live_symbols:
            fail("current shared-library symbols differ from evidence")


def parser() -> argparse.ArgumentParser:
    repo = Path(__file__).resolve().parents[2]
    evidence = repo / "tools/codex/app-server-evidence/0.144.6"
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("command", choices=("generate", "check"))
    result.add_argument("--repo-root", type=Path, default=repo)
    result.add_argument("--compiler", default="g++")
    result.add_argument("--library", type=Path)
    result.add_argument(
        "--output",
        type=Path,
        default=evidence / "a1-4-user-integrations-api-abi-evidence.json",
    )
    result.add_argument(
        "--symbols-output",
        type=Path,
        default=evidence / "a1-4-user-integrations-symbols.txt",
    )
    return result


def main() -> int:
    arguments = parser().parse_args()
    repo_root = arguments.repo_root.resolve()
    output = arguments.output.resolve()
    symbol_output = arguments.symbols_output.resolve()
    compiler = compiler_path(arguments.compiler)
    library = arguments.library.resolve() if arguments.library else None

    try:
        if arguments.command == "generate":
            if library is None or not library.is_file():
                fail("generate requires an existing --library")
            report, symbol_content = build_report(
                repo_root, compiler, library, symbol_output
            )
            output.parent.mkdir(parents=True, exist_ok=True)
            symbol_output.parent.mkdir(parents=True, exist_ok=True)
            symbol_output.write_text(symbol_content, encoding="utf-8")
            output.write_text(render_json(report), encoding="utf-8")
        else:
            if not output.is_file():
                fail(f"evidence is missing: {output}")
            loaded = json.loads(output.read_text(encoding="utf-8"))
            if not isinstance(loaded, dict):
                fail("evidence root is not an object")
            validate_report(
                repo_root, compiler, library, loaded, symbol_output
            )
    except (AbiEvidenceError, OSError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
