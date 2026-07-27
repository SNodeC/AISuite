#!/usr/bin/env python3

"""Guard the exact bytes of the completed Codex A1.1 evidence artifacts."""

from __future__ import annotations

import argparse
import hashlib
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True


EXPECTED_SHA256 = {
    "a1-1-closure-report.json": (
        "1c26d53cc1e678c4ea8f901cae443e70f83cfd8dbca11cb292260af89cad1f23"
    ),
    "a1-1-implementation-plan.json": (
        "59fbbd1e6c1ffcd5e0064e7ff7e2a6a68673ececac5ccb19d3d46cc57eda7661"
    ),
    "a1-1-notification-production-coverage.json": (
        "191c9e9152fe6c3a2fd052f542911c510e6643e78bfaf2b293729f9bfe7e62b0"
    ),
    "a1-1-operation-production-coverage.json": (
        "2301645d02f15892987eba73f92b7962f3545aa75520eabca48b473dc55f6664"
    ),
    "a1-1-start-state.json": (
        "1614414ea6320f8ac5eb47ef928c4bc8de587c9f429f86d7f94ffeb437b6c705"
    ),
    "a1-1-type-closure.json": (
        "187db47c37d094b5f22c27cb0a6c1fc656cd0b3e34c63faf9b81ededce2f8bfc"
    ),
}


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class CodexA11ArtifactByteIdentityTest(unittest.TestCase):
    def test_exact_completed_a1_1_artifact_bytes(self) -> None:
        actual_names = {
            path.name
            for path in OPTIONS.evidence_root.glob("a1-1-*.json")
            if path.is_file()
        }
        self.assertEqual(set(EXPECTED_SHA256), actual_names)

        for name, expected in sorted(EXPECTED_SHA256.items()):
            with self.subTest(artifact=name):
                path = OPTIONS.evidence_root / name
                self.assertTrue(path.is_file(), f"missing frozen A1.1 artifact: {name}")
                self.assertEqual(
                    expected,
                    file_sha256(path),
                    f"completed A1.1 artifact bytes changed: {name}",
                )


def parse_options() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", required=True, type=Path)
    return parser.parse_args()


if __name__ == "__main__":
    OPTIONS = parse_options()
    OPTIONS.evidence_root = OPTIONS.evidence_root.resolve()
    unittest.main(argv=[sys.argv[0]])
