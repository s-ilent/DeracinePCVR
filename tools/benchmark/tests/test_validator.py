#!/usr/bin/env python3
"""Fixtures and tests for the benchmark validity gate.

Every prior failure the contract names is fed to the validator here as a deliberate negative
case; the legitimate run is the positive control. The A/B tests use compare.py's own logic.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import compare  # noqa: E402
import scene_signature  # noqa: E402
import validator  # noqa: E402

REFERENCE_IMAGE = TOOLS / "reference" / "scene-classroom-menu.png"
SPLASH_IMAGE = TOOLS / "fixtures" / "sie-splash.png"
HEAD_SHA = "0" * 39 + "a"
EXE_BYTES = b"not a real emulator, only a fixture for hash pairing\n" * 4


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


class RunFixture:
    """Builds one synthetic run directory that the validator should accept."""

    def __init__(self, root: Path, name: str = "run-valid"):
        self.root = root
        self.run_dir = root / name
        self.run_dir.mkdir(parents=True, exist_ok=True)
        (root / "bin").mkdir(exist_ok=True)
        (root / "lab").mkdir(exist_ok=True)
        self.exe = root / "bin" / "shadps4-test.exe"
        self.lab_exe = root / "lab" / "shadps4.exe"
        self.exe.write_bytes(EXE_BYTES)
        self.lab_exe.write_bytes(EXE_BYTES)
        self.head_sha = HEAD_SHA
        self.screenshot = REFERENCE_IMAGE
        self.speeds = [38.0 + (index % 5) for index in range(120)]
        self.draws = 530.0
        self.mpx = 560.0

    def sample(self, index: int, in_window: bool) -> dict:
        fps = self.speeds[index]
        return {
            "sample": index,
            "at_s": round(index * 0.5, 3),
            "fps": fps,
            "frames": max(1, round(fps * 0.5)),
            "frame_ms": round(1000.0 / fps, 3),
            "draws": self.draws,
            "mpx": self.mpx,
            "passes": 89.0,
            "gpu_busy": 97.0,
            "in_window": in_window,
        }

    def timing(self) -> dict:
        per_sample = [self.sample(index, index >= 40) for index in range(120)]
        window = [row for row in per_sample if row["in_window"]]
        frame_ms = [row["frame_ms"] for row in window]
        measured_elapsed = sum(row["frames"] / row["fps"] for row in window)
        total_frames = sum(row["frames"] for row in window)
        aggregate_fps = total_frames / measured_elapsed
        stats = {
            "p50": validator.percentile(frame_ms, 0.50),
            "p90": validator.percentile(frame_ms, 0.90),
            "p95": validator.percentile(frame_ms, 0.95),
            "p99": validator.percentile(frame_ms, 0.99),
            "mean": sum(frame_ms) / len(frame_ms),
            "min": min(frame_ms),
            "max": max(frame_ms),
        }
        return {
            "source": "framestats",
            "source_id": "SHADPS4_FRAME_STATS windows from the draw scheduler's steady_clock",
            "samples_total": len(per_sample),
            "samples_window": len(window),
            "warmup_samples": 40,
            "window_start_s": 20.0,
            "window_end_s": 60.0,
            "window_elapsed_s": round(measured_elapsed, 3),
            "window_observed_s": 40.0,
            "warmup_start_s": 0.0,
            "warmup_end_s": 19.5,
            "unique_fps": round(aggregate_fps, 3),
            "frame_ms": {key: round(value, 3) for key, value in stats.items()},
            "compositor_cadence": {
                "presented_hz": 120.2,
                "source": "xrBeginFrame delimiters",
                "role": "informational",
                "is_unique_render_fps": False,
            },
            "per_sample": per_sample,
        }

    def scene(self) -> dict:
        from PIL import Image

        with Image.open(self.screenshot) as image:
            width, height = image.size
        with open(TOOLS / "reference" / "scene-classroom-menu.json", encoding="utf-8") as handle:
            reference = json.load(handle)
        similarity = scene_signature.similarity(scene_signature.signature(str(self.screenshot)),
                                                reference["signature"])
        shutil.copyfile(self.screenshot, self.run_dir / "scene.png")
        return {
            "classification": "classroom-main-menu-overlay",
            "overlay_present": True,
            "gameplay_reached": False,
            "anchor_sample": 20,
            "anchor_at_s": 10.0,
            "draws": self.draws,
            "mpx": self.mpx,
            "passes": 89.0,
            "screenshot": {
                "path": "scene.png",
                "sha256": validator.sha256_file(self.run_dir / "scene.png"),
                "width": width,
                "height": height,
                "taken_at_s": 11.0,
                "from_scene_anchor_s": 1.5,
                "role": "scene",
                "signature_similarity": round(similarity, 6),
                "reference": "reference/scene-classroom-menu.json",
            },
        }

    def build(self) -> Path:
        (self.run_dir / "shad_log.txt").write_text("frame stats at 0 s: 40.0 fps\n",
                                                   encoding="utf-8")
        (self.run_dir / "frames.csv").write_text("sample,at_s,fps\n0,0.0,40.0\n",
                                                 encoding="utf-8")
        (self.run_dir / "preflight.json").write_text("{}\n", encoding="utf-8")
        document = {
            "schema_version": 1,
            "run_id": self.run_dir.name,
            "label": self.run_dir.name,
            "created_utc": "2026-11-17T09:00:00Z",
            "scenario": {
                "id": validator.SCENARIO_ID,
                "game": {
                    "title_id": "CUSA11919",
                    "eboot": "D:/Temp/Deracine/CUSA11919-fixture/eboot.bin",
                    "eboot_sha256": "1" * 64,
                    "root": "D:/Temp/Deracine/CUSA11919-fixture",
                    "content_source": "fixture",
                },
                "guest": {"screen_percentage": 300, "msaa_requested": 2, "eye_width": 2880,
                          "eye_height": 3240, "stereo_width": 5760,
                          "stereo_actual": "5760x3240"},
                "presentation": {"vr_scale": 0.5, "guest_eye": "2880x3240 per eye",
                                 "actual": "2880x1632 window, per eye 1440x1632, base eye "
                                           "2880x3240, scale: 0.50x"},
                "vblank_hz": 60,
                "vblank_actual_hz": 60,
                "autopress": {"enabled": True, "value": "1", "devices": "pad,move",
                              "schedule": "8000-45000 ms, 600 ms every 3000 ms", "bursts_logged": 12},
                "warmup_s": 10.0,
                "window_s": 40.0,
                "stats_every_s": 0.5,
                "scene_gate": {"min_draws": 400, "min_mpx": 500, "consecutive_samples": 2},
            },
            "source": {
                "worktree": str(self.root),
                "branch": "feature/fixture",
                "head_sha": self.head_sha,
                "dirty_files": [],
                "dirty_count": 0,
                "status_porcelain_sha256": "2" * 64,
            },
            "executable": {
                "path": str(self.exe),
                "sha256": validator.sha256_file(self.exe),
                "bytes": self.exe.stat().st_size,
                "lab_path": str(self.lab_exe),
                "lab_sha256": validator.sha256_file(self.lab_exe),
                "built_from_head": None,
                "build_commit_source": "not recorded",
            },
            "host": {"gpu": "AMD Radeon fixture", "driver": "fixture 1.0", "os": "Windows 11",
                     "cpu": "fixture CPU", "xr_runtime": "SteamVR fixture", "headset": "not worn"},
            "env": {
                "SHADPS4_FRAME_STATS": "1",
                "SHADPS4_FRAME_STATS_EVERY": "0.5",
                "SHADPS4_VR_SCALE": "0.5",
                "SHADPS4_DBG_AUTOPRESS": "1",
                "SHADPS4_TITLE_EYE_WIDTH": "2880",
            },
            "timing": self.timing(),
            "scene": self.scene(),
            "artifacts": {
                "log": "shad_log.txt",
                "log_sha256": validator.sha256_file(self.run_dir / "shad_log.txt"),
                "frames_csv": "frames.csv",
                "console": None,
                "screenshots": ["scene.png"],
                "rdc": None,
                "rdc_png": None,
            },
            "provenance": {
                "preflight_manifest": "preflight.json",
                "protected_unchanged": True,
                "protected_drift": [],
                "protected_observed": [],
                "validator_version": validator.VALIDATOR_VERSION,
            },
        }
        self.document = document
        self.write(document)
        return self.run_dir

    def write(self, document: dict) -> None:
        (self.run_dir / "run.json").write_text(json.dumps(document, indent=1) + "\n",
                                               encoding="utf-8")

    def reload(self) -> dict:
        return json.loads((self.run_dir / "run.json").read_text(encoding="utf-8"))


class ValidatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="deracine-bench-")
        self.root = Path(self._tmp.name)
        self.fixture = RunFixture(self.root)
        self.run_dir = self.fixture.build()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def assert_invalid(self, expect: str) -> list[str]:
        result = validator.validate_run(self.run_dir)
        self.assertFalse(result["valid"], f"expected invalid; errors={result['errors']}")
        joined = "\n".join(result["errors"])
        self.assertIn(expect, joined)
        return result["errors"]

    # Positive control -----------------------------------------------------

    def test_valid_run_passes(self) -> None:
        result = validator.validate_run(self.run_dir)
        self.assertTrue(result["valid"], result["errors"])
        report = __import__("report").build(self.run_dir)
        self.assertTrue(report["valid"])
        self.assertAlmostEqual(report["observed"]["unique_rendered_fps"],
                               self.fixture.reload()["timing"]["unique_fps"], places=3)

    # 1. Present cadence mislabelled as FPS --------------------------------

    def test_present_cadence_is_not_a_frame_rate_source(self) -> None:
        document = self.fixture.reload()
        document["timing"]["source"] = "timeline"
        document["timing"]["source_id"] = "TIMELINE xr_begin_end (xrBeginFrame cadence)"
        document["timing"]["unique_fps"] = 120.2
        self.fixture.write(document)
        self.assert_invalid("expected 'framestats'")
        document["timing"]["source"] = "framestats"
        document["timing"]["source_id"] = "xrBeginFrame present cadence"
        self.fixture.write(document)
        self.assert_invalid("present/compositor cadence")

    # 2. Claimed 40 s window that really spans 0.65 s ---------------------

    def test_claimed_window_shorter_than_recorded(self) -> None:
        document = self.fixture.reload()
        document["timing"]["window_elapsed_s"] = 0.65
        document["timing"]["window_start_s"] = 10.0
        document["timing"]["window_end_s"] = 10.65
        self.fixture.write(document)
        self.assert_invalid("lasted 0.650 s")

    # 3. Light front end and SIE splash with fabricated performance -------

    def test_splash_screenshot_cannot_be_the_classroom(self) -> None:
        document = self.fixture.reload()
        shutil.copyfile(SPLASH_IMAGE, self.run_dir / "scene.png")
        from PIL import Image
        with Image.open(SPLASH_IMAGE) as image:
            width, height = image.size
        document["scene"]["draws"] = 38
        document["scene"]["mpx"] = 232
        document["scene"]["screenshot"].update({
            "sha256": validator.sha256_file(self.run_dir / "scene.png"),
            "width": width, "height": height, "signature_similarity": 0.99,
        })
        self.fixture.write(document)
        errors = self.assert_invalid("does not match the reviewed classroom reference")
        self.assertTrue(any("draws 38" in error for error in errors))

    # 4. 500+ draws but a wrong/missing screenshot -------------------------

    def test_wrong_screenshot_fails_even_with_heavy_draws(self) -> None:
        document = self.fixture.reload()
        shutil.copyfile(SPLASH_IMAGE, self.run_dir / "scene.png")
        document["scene"]["screenshot"]["sha256"] = validator.sha256_file(
            self.run_dir / "scene.png")
        self.fixture.write(document)
        self.assert_invalid("does not match the reviewed classroom reference")

        document = self.fixture.reload()
        document["scene"]["screenshot"]["path"] = "missing.png"
        self.fixture.write(document)
        self.assert_invalid("scene screenshot is missing")

    # 5. Missing autopress / vblank / resolution / GPU / executable hash --

    def test_missing_required_provenance(self) -> None:
        document = self.fixture.reload()
        document["env"].pop("SHADPS4_DBG_AUTOPRESS")
        document["scenario"].pop("vblank_hz")
        document["host"].pop("driver")
        document["executable"].pop("sha256")
        document["scenario"]["guest"].pop("eye_width")
        self.fixture.write(document)
        errors = "\n".join(self.assert_invalid("missing required property"))
        for field in ("SHADPS4_DBG_AUTOPRESS", "vblank_hz", "driver", "sha256", "eye_width"):
            self.assertIn(field, errors)

    # 6. vblank 60 vs 120 in cells claimed comparable ---------------------

    def test_different_vblank_cells_are_not_comparable(self) -> None:
        other = RunFixture(self.root, "run-vblank120")
        other.head_sha = self.fixture.head_sha
        other_dir = other.build()
        document = other.reload()
        document["scenario"]["vblank_hz"] = 120
        document["scenario"]["vblank_actual_hz"] = 120
        other.write(document)
        self.assertTrue(validator.validate_run(other_dir)["valid"])
        result, reason = compare.compare(self.run_dir, other_dir, 10.0)
        self.assertIsNotNone(reason)
        self.assertFalse(result["comparable"])
        self.assertIn("vblank", result["reason"])

    # 7. Path-less or traversal cleanup destination -----------------------

    def test_cleanup_paths_must_be_absolute_inside_the_private_root(self) -> None:
        document = self.fixture.reload()
        document["cleanup"] = {
            "root": str(self.run_dir),
            "paths": ["", str(self.run_dir / ".." / "escape"), "scene.png"],
        }
        self.fixture.write(document)
        errors = "\n".join(self.assert_invalid("path-less"))
        self.assertIn("'..'", errors)
        self.assertIn("relative", errors)

    # 8. Truncated windows, nonmonotonic timestamps, missing samples ------

    def test_timing_series_must_be_contiguous_and_monotonic(self) -> None:
        document = self.fixture.reload()
        document["timing"]["per_sample"][30]["at_s"] = 1.0
        self.fixture.write(document)
        self.assert_invalid("nonmonotonic")

        document = self.fixture.reload()
        document["timing"]["samples_window"] = 70
        self.fixture.write(document)
        self.assert_invalid("expected 80")

        document = self.fixture.reload()
        document["timing"]["per_sample"] = document["timing"]["per_sample"][:60]
        document["timing"]["samples_total"] = 60
        self.fixture.write(document)
        self.assert_invalid("in-window samples")

    # 9. Metadata names a different executable than the file --------------

    def test_executable_hash_must_match_the_inspected_file(self) -> None:
        document = self.fixture.reload()
        document["executable"]["sha256"] = "f" * 64
        self.fixture.write(document)
        self.assert_invalid("does not match the inspected file")

        document = self.fixture.reload()
        document["executable"]["sha256"] = validator.sha256_file(self.fixture.exe)
        self.fixture.write(document)
        self.fixture.exe.write_bytes(EXE_BYTES + b"tampered")
        self.assert_invalid("does not match the inspected file")

    # 10. Content drift between A and B despite similar FPS ---------------

    def test_content_drift_refuses_an_ab_claim(self) -> None:
        other = RunFixture(self.root, "run-drift")
        other.head_sha = self.fixture.head_sha
        other.draws = 460.0
        other.mpx = 505.0
        other_dir = other.build()
        self.assertTrue(validator.validate_run(other_dir)["valid"])
        result, reason = compare.compare(self.run_dir, other_dir, 10.0)
        self.assertIsNotNone(reason)
        self.assertFalse(result["comparable"])
        self.assertIn("drifted", result["reason"])

    # Positive A/B: same binary, inside noise -----------------------------

    def test_same_binary_replicate_is_inconclusive(self) -> None:
        other = RunFixture(self.root, "run-replicate")
        other.head_sha = self.fixture.head_sha
        other.speeds = [value + 0.4 for value in self.fixture.speeds]
        other_dir = other.build()
        self.assertTrue(validator.validate_run(other_dir)["valid"])
        result, reason = compare.compare(self.run_dir, other_dir, 10.0)
        self.assertIsNone(reason)
        self.assertTrue(result["comparable"])
        self.assertTrue(result["inside_noise_floor"])
        self.assertIn("inconclusive", result["conclusion"])

    def test_unique_fps_is_total_frames_over_total_time(self) -> None:
        document = self.fixture.reload()
        document["timing"]["unique_fps"] = round(document["timing"]["unique_fps"] + 1.0, 3)
        self.fixture.write(document)
        self.assert_invalid("total frames / total measured time")

    def test_in_window_workload_must_stay_in_the_scene(self) -> None:
        document = self.fixture.reload()
        row = next(s for s in document["timing"]["per_sample"] if s["in_window"])
        row["draws"] = 38.0
        row["mpx"] = 232.0
        self.fixture.write(document)
        self.assert_invalid("left the scene gate")

    def test_observed_protected_changes_do_not_fail_but_drift_does(self) -> None:
        document = self.fixture.reload()
        document["provenance"]["protected_observed"] = ["another worktree changed"]
        self.fixture.write(document)
        self.assertTrue(validator.validate_run(self.run_dir)["valid"])

        document = self.fixture.reload()
        document["provenance"]["protected_drift"] = ["content changed: pc-vr/shadps4.exe"]
        self.fixture.write(document)
        self.assert_invalid("protected drift")

    def test_failed_run_directory_is_never_valid(self) -> None:
        (self.run_dir / "INVALID.txt").write_text("INVALID RUN - timeout\n", encoding="utf-8")
        self.assert_invalid("records a failed run")
        (self.run_dir / "INVALID.txt").unlink()
        self.assertTrue(validator.validate_run(self.run_dir)["valid"])

    def test_missing_frame_counts_are_not_estimated(self) -> None:
        document = self.fixture.reload()
        row = next(s for s in document["timing"]["per_sample"] if s["in_window"])
        row.pop("frames")
        self.fixture.write(document)
        self.assert_invalid("no per-window frame count")

    def test_actual_presentation_must_match_requested(self) -> None:
        document = self.fixture.reload()
        document["scenario"]["presentation"]["actual"] = (
            "2880x1632 window, per eye 1440x1632, base eye 2880x3240, scale: 1.00x")
        self.fixture.write(document)
        self.assert_invalid("actual presentation scale")

        document = self.fixture.reload()
        document["scenario"]["guest"]["stereo_actual"] = "2880x3240"
        self.fixture.write(document)
        self.assert_invalid("actual guest stereo")


if __name__ == "__main__":
    unittest.main(verbosity=2)
