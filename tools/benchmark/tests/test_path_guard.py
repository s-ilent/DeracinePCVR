#!/usr/bin/env python3
"""Tests for the harness path guard: allowlists, denial, traversal and reparse points."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import path_guard  # noqa: E402


class GuardTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="deracine-guard-")
        self.root = Path(self._tmp.name).resolve()
        self.private = self.root / "private"
        self.private.mkdir()
        self.danger = self.root / "protected"
        self.danger.mkdir()
        (self.danger / "shadps4.exe").write_bytes(b"protected")
        self.config = {
            "allow_write": [str(self.private)],
            "allow_read": [str(self.private)],
            "deny": [str(self.danger)],
        }

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_inside_allow_root_is_allowed(self) -> None:
        verdict = path_guard.check(str(self.private / "run" / "run.json"), "write", self.config)
        self.assertTrue(verdict["allowed"], verdict)

    def test_outside_every_root_is_denied(self) -> None:
        verdict = path_guard.check(str(self.root / "elsewhere.txt"), "write", self.config)
        self.assertFalse(verdict["allowed"])

    def test_deny_root_wins_even_if_it_were_listed(self) -> None:
        config = dict(self.config)
        config["allow_write"] = [str(self.root)]
        verdict = path_guard.check(str(self.danger / "shadps4.exe"), "delete", config)
        self.assertFalse(verdict["allowed"])
        self.assertIn("deny root", " ".join(verdict["reasons"]))

    def test_empty_path_fails_closed(self) -> None:
        with self.assertRaises(path_guard.GuardError):
            path_guard.normalise("")
        with self.assertRaises(path_guard.GuardError):
            path_guard.normalise("   ")

    def test_relative_path_fails_closed(self) -> None:
        with self.assertRaises(path_guard.GuardError):
            path_guard.normalise("run/run.json")

    def test_parent_traversal_fails_closed(self) -> None:
        with self.assertRaises(path_guard.GuardError):
            path_guard.normalise(str(self.private / ".." / "protected" / "shadps4.exe"))

    def test_junction_escape_is_denied(self) -> None:
        if os.name != "nt":
            self.skipTest("junctions are a Windows feature")
        junction = self.private / "escape"
        result = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(junction), str(self.danger)],
            capture_output=True, text=True)
        if result.returncode != 0:
            self.skipTest(f"could not create a junction: {result.stderr or result.stdout}")
        verdict = path_guard.check(str(junction / "shadps4.exe"), "delete", self.config)
        self.assertFalse(verdict["allowed"], verdict)
        self.assertIn("reparse point", " ".join(verdict["reasons"]))

    def test_cli_refuses_a_pathless_delete(self) -> None:
        config_path = self.root / "guard.json"
        config_path.write_text(json.dumps(self.config), encoding="utf-8")
        invocation = [sys.executable, str(TOOLS / "path_guard.py"),
                      "--config", str(config_path), "--op", "delete", "--path", ""]
        result = subprocess.run(invocation, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('"allowed": false', result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
