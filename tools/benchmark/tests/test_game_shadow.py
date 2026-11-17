#!/usr/bin/env python3
"""Tests for the private game-shadow proof and the shipped protected-path denial.

The checker's job is to make ``-Game`` prove it is not the active installation or a shared copy.
These tests drive the positive control, every rejection rule the runner depends on, the junction
escape that the real shadow layout invites, and the default path-guard denial.
"""

from __future__ import annotations

import hashlib
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

import game_shadow  # noqa: E402
import path_guard  # noqa: E402

REPO = Path(game_shadow.checkout_root())
ACTIVE_INSTALL = Path(r"D:\Temp\Deracine\1.0")
ACTIVE_UPDATE = Path(r"D:\Temp\Deracine\Update")
SHARED_GAME = Path(r"D:\Temp\Deracine\CUSA11919-qual")
PRIVATE_SHADOW = Path(r"D:\Temp\Deracine\CUSA11919-guardrails-2026-11-17")
EBOOT_BYTES = b"fixture eboot payload\n" * 8


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def write_policy(path: Path, deny: list[str]) -> Path:
    path.write_text(json.dumps({"schema_version": 1, "deny": deny}), encoding="utf-8")
    return path


def make_junction(link: Path, target: Path) -> tuple[bool, str]:
    result = subprocess.run(
        ["cmd", "/c", "mklink", "/J", str(link), str(target)],
        capture_output=True,
        text=True)
    detail = (result.stderr or result.stdout).strip()
    return result.returncode == 0, detail


class ShadowFixture:
    """One temp private shadow, with an engine.ini already written by the runner preset."""

    def __init__(self, base: Path, name: str = "shadow") -> None:
        self.root = base / name
        self.root.mkdir(parents=True, exist_ok=True)
        self.eboot = self.root / "eboot.bin"
        self.eboot.write_bytes(EBOOT_BYTES)
        self.engine_ini = self.root / "gameproject" / "saved" / "config" / "ps4" / "engine.ini"
        self.engine_ini.parent.mkdir(parents=True, exist_ok=True)
        self.engine_ini.write_text(
            "[SystemSettings]\r\nr.ScreenPercentage=300\r\nr.MSAACount=2\r\n", encoding="utf-8")
        self.manifest_path = self.root / "benchmark-shadow.json"
        self.write_manifest()

    def write_manifest(self, **overrides: object) -> None:
        document = {
            "private": True,
            "eboot_sha256": sha256_file(self.eboot),
            "owner": "test: private classroom shadow",
        }
        document.update(overrides)
        self.manifest_path.write_text(json.dumps(document), encoding="utf-8")

    def check(self, **kwargs: object) -> dict:
        kwargs.setdefault("game", str(self.root))
        kwargs.setdefault("shadow_root", str(self.root))
        kwargs.setdefault("eboot", str(self.eboot))
        return game_shadow.check(**kwargs)  # type: ignore[arg-type]

    def run_cli(self, *extra: str, policy: Path | None = None) -> subprocess.CompletedProcess:
        invocation = [
            sys.executable, str(TOOLS / "game_shadow.py"), "check",
            "--game", str(self.root), "--shadow-root", str(self.root),
            "--eboot", str(self.eboot), "--json", *extra,
        ]
        if policy is not None:
            invocation += ["--policy", str(policy)]
        return subprocess.run(invocation, capture_output=True, text=True)


class ShadowTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="deracine-shadow-")
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name).resolve()
        self.fixture = ShadowFixture(self.root)

    def remove_junction(self, link: Path) -> None:
        def cleanup() -> None:
            try:
                os.rmdir(link)
            except OSError:
                pass
        self.addCleanup(cleanup)


class PositiveControlTests(ShadowTestCase):
    def test_private_shadow_with_manifest_and_matching_eboot_is_valid(self) -> None:
        verdict = self.fixture.check()
        self.assertTrue(verdict["valid"], verdict["reasons"])
        self.assertEqual(verdict["reasons"], [])
        self.assertEqual(verdict["shadow_root"], os.path.realpath(str(self.fixture.root)))
        self.assertEqual(verdict["game_realpath"], os.path.realpath(str(self.fixture.root)))
        self.assertEqual(verdict["eboot_sha256"], sha256_bytes(EBOOT_BYTES))
        self.assertEqual(verdict["manifest"]["owner"], "test: private classroom shadow")
        self.assertIs(verdict["manifest"]["private"], True)
        self.assertEqual(verdict["manifest_path"], str(self.fixture.manifest_path))
        self.assertEqual(verdict["policy"]["path"],
                         os.path.realpath(str(TOOLS / "shadow-policy.json")))

    def test_game_subdirectory_inside_the_shadow_root_is_valid(self) -> None:
        game = self.fixture.root / "content" / "CUSA11919"
        game.mkdir(parents=True)
        (game / "eboot.bin").write_bytes(EBOOT_BYTES)
        verdict = self.fixture.check(game=str(game), eboot=str(game / "eboot.bin"))
        self.assertTrue(verdict["valid"], verdict["reasons"])

    def test_check_is_read_only_for_engine_ini(self) -> None:
        before = self.fixture.engine_ini.stat().st_mtime_ns
        self.fixture.check()
        self.assertEqual(self.fixture.engine_ini.stat().st_mtime_ns, before)


class ActiveInstallTests(ShadowTestCase):
    def test_active_install_selected_is_rejected(self) -> None:
        active = self.root / "active"
        active.mkdir()
        (active / "eboot.bin").write_bytes(EBOOT_BYTES)
        self.fixture.write_manifest(eboot_sha256=sha256_bytes(EBOOT_BYTES))
        policy = write_policy(self.root / "policy.json", [str(active)])

        verdict = self.fixture.check(
            game=str(active), eboot=str(active / "eboot.bin"), policy=policy)
        self.assertFalse(verdict["valid"])
        reasons = " ".join(verdict["reasons"])
        self.assertIn("blocklisted", reasons)
        self.assertIn("protected", reasons)

    def test_shipped_policy_designates_active_and_shared_paths_but_not_the_private_shadow(self) -> None:
        deny = game_shadow.load_policy()["deny"]
        protected = [
            ACTIVE_INSTALL,
            ACTIVE_UPDATE,
            SHARED_GAME,
            Path(r"D:\Temp\Deracine\CUSA11919-1.01"),
            Path(r"D:\Temp\Deracine\CUSA11919-color"),
            REPO / "pc-vr",
            REPO / "games",
            Path(r"M:\Development\Deracine-work\_build\bench"),
            Path(r"M:\Development\Deracine-work\_build\headless\template"),
            Path(r"M:\Development\Deracine-recovery-2026-11-17"),
        ]
        for path in protected:
            with self.subTest(path=path):
                self.assertTrue(
                    any(game_shadow.matches_blocklist_prefix(str(path), entry)
                        for entry in deny),
                    f"{path} is not covered by the shipped policy")
        self.assertFalse(
            any(game_shadow.matches_blocklist_prefix(str(PRIVATE_SHADOW), entry)
                for entry in deny),
            "the private classroom shadow must not be blocklisted")

    def test_junction_to_the_active_install_is_rejected(self) -> None:
        if os.name != "nt":
            self.skipTest("junctions are a Windows feature")
        active = self.root / "active"
        active.mkdir()
        (active / "eboot.bin").write_bytes(EBOOT_BYTES)
        junction = self.fixture.root / "escape"
        created, detail = make_junction(junction, active)
        if not created:
            self.skipTest(f"the host refused to create a junction: {detail}")
        self.remove_junction(junction)

        verdict = self.fixture.check(
            game=str(junction), eboot=str(junction / "eboot.bin"))
        self.assertFalse(verdict["valid"])
        self.assertIn("escape", " ".join(verdict["reasons"]))

    def test_shadow_root_that_is_a_junction_to_the_active_install_is_rejected(self) -> None:
        if os.name != "nt":
            self.skipTest("junctions are a Windows feature")
        active = ShadowFixture(self.root, name="active")
        policy = write_policy(self.root / "policy.json", [str(active.root)])
        link = self.root / "shadow-link"
        created, detail = make_junction(link, active.root)
        if not created:
            self.skipTest(f"the host refused to create a junction: {detail}")
        self.remove_junction(link)

        verdict = game_shadow.check(
            game=str(link), shadow_root=str(link), eboot=str(link / "eboot.bin"),
            policy=policy)
        self.assertFalse(verdict["valid"])
        self.assertIn("blocklisted", " ".join(verdict["reasons"]))


class RejectionTests(ShadowTestCase):
    def test_parent_traversal_is_rejected(self) -> None:
        verdict = self.fixture.check(game=str(self.fixture.root / ".." / "shadow"))
        self.assertFalse(verdict["valid"])
        self.assertIn("'..'", " ".join(verdict["reasons"]))

    def test_game_outside_the_shadow_root_is_rejected(self) -> None:
        other = self.root / "other"
        other.mkdir()
        verdict = self.fixture.check(shadow_root=str(other))
        self.assertFalse(verdict["valid"])
        self.assertIn("path escape", " ".join(verdict["reasons"]))

    def test_missing_manifest_is_rejected(self) -> None:
        self.fixture.manifest_path.unlink()
        verdict = self.fixture.check()
        self.assertFalse(verdict["valid"])
        self.assertIn("manifest not found", " ".join(verdict["reasons"]))

    def test_manifest_not_json_is_rejected(self) -> None:
        self.fixture.manifest_path.write_text("{ this is not json", encoding="utf-8")
        verdict = self.fixture.check()
        self.assertFalse(verdict["valid"])
        self.assertIn("not valid JSON", " ".join(verdict["reasons"]))

    def test_manifest_private_must_be_true(self) -> None:
        for value in (False, "true", 1, None):
            with self.subTest(private=value):
                self.fixture.write_manifest(private=value)
                verdict = self.fixture.check()
                self.assertFalse(verdict["valid"])
                self.assertIn("private", " ".join(verdict["reasons"]))

    def test_manifest_owner_must_be_non_empty(self) -> None:
        for value in ("", "   ", None, 7):
            with self.subTest(owner=value):
                self.fixture.write_manifest(owner=value)
                verdict = self.fixture.check()
                self.assertFalse(verdict["valid"])
                self.assertIn("owner", " ".join(verdict["reasons"]))

    def test_manifest_eboot_hash_must_be_hex64(self) -> None:
        for value in ("", "xyz", "0" * 63, "0" * 65):
            with self.subTest(eboot_sha256=value):
                self.fixture.write_manifest(eboot_sha256=value)
                verdict = self.fixture.check()
                self.assertFalse(verdict["valid"])
                self.assertIn("64-character hex", " ".join(verdict["reasons"]))

    def test_wrong_eboot_hash_is_rejected(self) -> None:
        self.fixture.write_manifest(eboot_sha256="0" * 64)
        verdict = self.fixture.check()
        self.assertFalse(verdict["valid"])
        self.assertIn("mismatch", " ".join(verdict["reasons"]))

    def test_eboot_outside_the_game_is_rejected(self) -> None:
        other = self.root / "other-eboot.bin"
        other.write_bytes(EBOOT_BYTES)
        verdict = self.fixture.check(eboot=str(other))
        self.assertFalse(verdict["valid"])
        self.assertIn("not the game's declared eboot", " ".join(verdict["reasons"]))

    def test_blocklisted_shared_directory_is_rejected(self) -> None:
        shared = self.root / "shared"
        shared.mkdir()
        (shared / "eboot.bin").write_bytes(EBOOT_BYTES)
        self.fixture.write_manifest(eboot_sha256=sha256_bytes(EBOOT_BYTES))
        policy = write_policy(self.root / "policy.json", [str(shared)])

        verdict = self.fixture.check(
            game=str(shared), eboot=str(shared / "eboot.bin"), policy=policy)
        self.assertFalse(verdict["valid"])
        self.assertIn("blocklisted", " ".join(verdict["reasons"]))

    def test_shadow_root_inside_the_repository_is_rejected(self) -> None:
        scratch = Path(tempfile.mkdtemp(prefix="shadow-inside-repo-", dir=str(REPO)))
        self.addCleanup(shutil.rmtree, scratch, True)
        fixture = ShadowFixture(scratch, name="shadow")
        verdict = fixture.check()
        self.assertFalse(verdict["valid"])
        reasons = " ".join(verdict["reasons"])
        self.assertIn("inside the repository", reasons)

    def test_shadow_root_that_contains_protected_paths_is_rejected(self) -> None:
        # A whole temp root declared "private" would launder its siblings; fail closed instead.
        shared = self.fixture.root / "shared"
        shared.mkdir()
        policy = write_policy(self.root / "policy.json", [str(shared)])
        verdict = self.fixture.check(policy=policy)
        self.assertFalse(verdict["valid"])
        self.assertIn("contains a blocklisted path", " ".join(verdict["reasons"]))


class EngineIniTests(ShadowTestCase):
    def test_engine_ini_reporting_is_correct(self) -> None:
        expected = sha256_file(self.fixture.engine_ini)
        verdict = self.fixture.check()
        engine = verdict["engine_ini"]
        self.assertEqual(engine["path"], str(self.fixture.engine_ini))
        self.assertTrue(engine["exists"])
        self.assertEqual(engine["sha256"], expected)
        self.assertTrue(engine["writable"])
        self.assertTrue(verdict["valid"], verdict["reasons"])

    def test_missing_engine_ini_is_reported_not_gated(self) -> None:
        self.fixture.engine_ini.unlink()
        verdict = self.fixture.check()
        engine = verdict["engine_ini"]
        self.assertFalse(engine["exists"])
        self.assertIsNone(engine["sha256"])
        self.assertTrue(engine["writable"])
        self.assertTrue(verdict["valid"], verdict["reasons"])

    def test_engine_ini_junction_outside_the_game_is_rejected(self) -> None:
        if os.name != "nt":
            self.skipTest("junctions are a Windows feature")
        shared_saved = self.root / "shared" / "gameproject" / "saved"
        shared_config = shared_saved / "config" / "ps4"
        shared_config.mkdir(parents=True)
        (shared_config / "engine.ini").write_text("[SystemSettings]\r\n", encoding="utf-8")

        shutil.rmtree(self.fixture.root / "gameproject")
        (self.fixture.root / "gameproject").mkdir()
        junction = self.fixture.root / "gameproject" / "saved"
        created, detail = make_junction(junction, shared_saved)
        if not created:
            self.skipTest(f"the host refused to create a junction: {detail}")
        self.remove_junction(junction)

        verdict = self.fixture.check()
        self.assertFalse(verdict["valid"])
        self.assertIn("engine.ini resolves outside", " ".join(verdict["reasons"]))


class PolicyTests(ShadowTestCase):
    def test_policy_tokens_expand(self) -> None:
        policy = write_policy(self.root / "policy.json",
                              ["%REPO%\\pc-vr", "%WORKSPACE%\\_build\\bench"])
        loaded = game_shadow.load_policy(policy)
        self.assertIn(os.path.realpath(str(REPO / "pc-vr")), loaded["deny"])
        self.assertIn(
            os.path.realpath(str(Path(game_shadow.workspace_root()) / "_build" / "bench")),
            loaded["deny"])

    def test_unknown_policy_token_is_rejected(self) -> None:
        policy = write_policy(self.root / "policy.json", ["%NOT_A_TOKEN%\\games"])
        with self.assertRaises(game_shadow.UsageError):
            game_shadow.load_policy(policy)

    def test_policy_without_deny_fails_closed(self) -> None:
        policy = self.root / "policy.json"
        policy.write_text(json.dumps({"schema_version": 1}), encoding="utf-8")
        with self.assertRaises(game_shadow.UsageError):
            game_shadow.load_policy(policy)


class CliTests(ShadowTestCase):
    def test_valid_shadow_exits_zero_with_json(self) -> None:
        result = self.fixture.run_cli()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        document = json.loads(result.stdout)
        self.assertTrue(document["valid"])
        self.assertEqual(document["reasons"], [])

    def test_invalid_shadow_exits_two_with_json(self) -> None:
        self.fixture.manifest_path.unlink()
        result = self.fixture.run_cli()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        document = json.loads(result.stdout)
        self.assertFalse(document["valid"])
        self.assertIn("manifest not found", " ".join(document["reasons"]))

    def test_missing_arguments_exit_one(self) -> None:
        result = subprocess.run(
            [sys.executable, str(TOOLS / "game_shadow.py")],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("usage", result.stderr.lower())

    def test_unreadable_policy_exits_one(self) -> None:
        result = self.fixture.run_cli(policy=self.root / "absent-policy.json")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        document = json.loads(result.stdout)
        self.assertFalse(document["valid"])
        self.assertIn("policy", document["error"])


class ProtectedPathPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="deracine-policy-")
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name).resolve()

    def config(self, allow: Path) -> dict:
        return {"allow_write": [str(allow)], "allow_read": [str(allow)], "deny": []}

    def test_shipped_policy_denies_automated_writes_to_protected_paths(self) -> None:
        config = self.config(REPO)
        target = str(REPO / "games" / "CUSA11919" / "engine.ini")
        verdict = path_guard.check(target, "write", config)
        self.assertFalse(verdict["allowed"], verdict)
        self.assertIn("protected-path policy", " ".join(verdict["reasons"]))
        # Reading the active install is legitimate (preflight hashes it); writing never is.
        self.assertTrue(path_guard.check(target, "read", config)["allowed"])

    def test_configured_allow_root_cannot_override_the_policy(self) -> None:
        config = self.config(REPO)
        for op in ("write", "delete", "mkdir", "copy_dest"):
            with self.subTest(op=op):
                verdict = path_guard.check(str(REPO / "pc-vr" / "settings.txt"), op, config)
                self.assertFalse(verdict["allowed"], verdict)

    def test_missing_policy_fails_closed(self) -> None:
        original = path_guard.POLICY_PATH
        path_guard.POLICY_PATH = self.root / "absent-policy.json"
        try:
            with self.assertRaises(path_guard.GuardError):
                path_guard.check(str(self.root / "run.json"), "write", self.config(self.root))
        finally:
            path_guard.POLICY_PATH = original

    def test_alternate_policy_denies_writes_and_ignores_reads(self) -> None:
        protected = self.root / "active"
        protected.mkdir()
        policy = write_policy(self.root / "policy.json", [str(protected)])
        original = path_guard.POLICY_PATH
        path_guard.POLICY_PATH = policy
        try:
            target = str(protected / "gameproject" / "saved" / "engine.ini")
            write = path_guard.check(target, "write", self.config(self.root))
            self.assertFalse(write["allowed"], write)
            self.assertIn("protected-path policy", " ".join(write["reasons"]))
            self.assertTrue(path_guard.check(target, "read", self.config(self.root))["allowed"])
        finally:
            path_guard.POLICY_PATH = original


if __name__ == "__main__":
    unittest.main(verbosity=2)
