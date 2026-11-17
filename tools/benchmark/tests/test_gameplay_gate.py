#!/usr/bin/env python3
"""Tests for the headless gameplay scene gate.

No emulator, no GPU: the gate is fed synthetic frame-stats series that reproduce the M2 measured
signature (menu ~480 draws, loading dip ~45, gameplay ~1866-1900) and the failure modes the
validation record names. Positive control is a gameplay series; every negative case is a real way
a run can look "heavy" without being gameplay.
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import gameplay_gate as gate  # noqa: E402

PROFILE = json.loads((TOOLS / "reference" / "scene-gameplay.json").read_text(encoding="utf-8"))

LINE = (
    "[   0.5000] <Info> Render_Vulkan: frame stats at {at} s: {fps} fps, per frame "
    "{passes} passes (12.0 loading their targets, {mpx} Mpx), {draws} draws, 3.0 dispatches, "
    "4.0 submits, GPU busy 97% at 2100 MHz, {frames} frames"
)


def make(at: float, draws: float, mpx: float, passes: float, fps: float = 31.4,
         frames: int = 15) -> dict:
    return {
        "at_s": at, "fps": fps, "frames": frames, "passes": passes, "loading": 12.0,
        "mpx": mpx, "draws": draws, "dispatches": 3.0, "submits": 4.0, "gpu_busy": 97.0,
    }


def gameplay_series(count: int = 6, start: float = 60.0) -> list[dict]:
    """The measured gameplay signature: 1866-1900 draws, 620-720 Mpx, 42-46 passes."""
    return [
        make(start + i * 0.5, 1880.0, 670.0, 44.0, fps=31.0, frames=15)
        for i in range(count)
    ]


class ParseTests(unittest.TestCase):
    def test_full_line(self) -> None:
        sample = gate.parse_sample(LINE.format(at=12, fps="57.7", passes="88.0", mpx="485.3",
                                               draws="472.7", frames=30))
        assert sample is not None
        self.assertEqual(sample["at_s"], 12.0)
        self.assertEqual(sample["frames"], 30)
        self.assertEqual(sample["draws"], 472.7)
        self.assertEqual(sample["mpx"], 485.3)
        self.assertEqual(sample["passes"], 88.0)
        self.assertEqual(sample["gpu_busy"], 97.0)

    def test_truncated_line_is_not_a_sample(self) -> None:
        line = LINE.format(at=12, fps="57.7", passes="88.0", mpx="485.3", draws="472.7",
                           frames=30).replace(", 30 frames", "")
        self.assertIsNone(gate.parse_sample(line))

    def test_parse_log_counts_truncated_lines(self) -> None:
        text = "\n".join([
            LINE.format(at=12, fps="57.7", passes="88.0", mpx="485.3", draws="472.7", frames=30),
            "frame stats at 13 s: 57.7 fps, per frame 88.0 passes (12.0 loading their targets,",
            LINE.format(at=14, fps="31.0", passes="44.0", mpx="670.0", draws="1880.0", frames=15),
        ])
        samples, incomplete = gate.parse_log(text)
        self.assertEqual(len(samples), 2)
        self.assertEqual(incomplete, 1)

    def test_aggregate_fps_is_total_frames_over_total_time(self) -> None:
        samples = [make(1, 100, 100, 10, fps=60, frames=30),
                   make(3, 100, 100, 10, fps=30, frames=60)]
        # 30 frames at 60 fps (0.5 s) + 60 frames at 30 fps (2.0 s) = 90 / 2.5
        self.assertAlmostEqual(gate.aggregate_fps(samples), 36.0, places=6)


class VerdictTests(unittest.TestCase):
    def test_measured_gameplay_signature_is_valid(self) -> None:
        verdict = gate.evaluate(gameplay_series(), PROFILE)
        self.assertTrue(verdict["valid"], verdict["reasons"])
        self.assertEqual(verdict["phase"], "gameplay")
        self.assertEqual(verdict["window_samples"], 6)
        self.assertGreaterEqual(verdict["draws_min"], 1500)
        self.assertGreaterEqual(verdict["mpx_min"], 550)

    def test_menu_is_rejected_as_menu(self) -> None:
        series = [make(i * 0.5, 480.0, 485.0, 65.0, fps=58.0, frames=29) for i in range(10)]
        verdict = gate.evaluate(series, PROFILE)
        self.assertFalse(verdict["valid"])
        self.assertEqual(verdict["phase"], "menu")
        self.assertIn("menu", " ".join(verdict["reasons"]).lower())

    def test_loading_dip_alone_is_rejected(self) -> None:
        series = [make(i * 0.5, 45.0, 5.0, 3.0, fps=120.0, frames=60) for i in range(10)]
        verdict = gate.evaluate(series, PROFILE)
        self.assertFalse(verdict["valid"])
        self.assertEqual(verdict["phase"], "none")

    def test_anchor_skips_a_loading_dip(self) -> None:
        # menu window, then the loading dip, then gameplay: the anchor must start after both.
        series = (
            [make(0.0, 480.0, 485.0, 65.0, fps=58.0, frames=29),
             make(0.5, 472.0, 478.0, 63.0, fps=58.0, frames=29),
             make(1.0, 45.0, 5.0, 3.0, fps=120.0, frames=60)]
            + gameplay_series(6, start=2.0)
        )
        verdict = gate.evaluate(series, PROFILE)
        self.assertTrue(verdict["valid"], verdict["reasons"])
        # The window starts after the dip, not at sample 0.
        self.assertGreaterEqual(verdict["anchor_index"], 3)

    def test_gate_passed_but_out_of_band_is_rejected(self) -> None:
        # 1000 draws passes the 800 gate but is neither gameplay nor menu: not a valid cell.
        series = [make(i * 0.5, 1000.0, 600.0, 50.0, fps=40.0, frames=20) for i in range(8)]
        verdict = gate.evaluate(series, PROFILE)
        self.assertFalse(verdict["valid"])
        self.assertEqual(verdict["phase"], "gameplay")
        self.assertTrue(any("band" in reason for reason in verdict["reasons"]))

    def test_short_window_is_rejected(self) -> None:
        verdict = gate.evaluate(gameplay_series(3), PROFILE)
        self.assertFalse(verdict["valid"])
        self.assertTrue(any("at least 5" in reason for reason in verdict["reasons"]))

    def test_empty_series_is_invalid(self) -> None:
        verdict = gate.evaluate([], PROFILE)
        self.assertFalse(verdict["valid"])
        self.assertEqual(verdict["phase"], "unknown")

    def test_profile_band_contains_the_measured_signature(self) -> None:
        band = PROFILE["band"]
        for draws, mpx, passes in ((1866.0, 620.0, 42.0), (1900.0, 720.0, 46.0)):
            sample = make(60.0, draws, mpx, passes)
            self.assertTrue(gate.in_band(sample, band), (draws, mpx, passes))
        # The menu can never satisfy the gameplay gate.
        self.assertLess(485.0, PROFILE["scene_gate"]["min_draws"])


class SaveTests(unittest.TestCase):
    def test_matching_save_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            payload = {"MGS_GameData_1": b"slot-one", "MGS_SystemData": b"system"}
            manifest = {"files": {}}
            for name, data in payload.items():
                (root / name).write_bytes(data)
                manifest["files"][name] = gate.sha256_file(root / name)
            self.assertEqual(gate.verify_save(root, manifest), [])

    def test_mismatched_and_missing_files_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "MGS_GameData_1").write_bytes(b"tampered")
            manifest = {"files": {
                "MGS_GameData_1": "0" * 64,
                "MGS_SystemData": "1" * 64,
            }}
            errors = gate.verify_save(root, manifest)
            self.assertEqual(len(errors), 2)
            self.assertTrue(any("MGS_GameData_1" in error for error in errors))
            self.assertTrue(any("missing" in error for error in errors))


if __name__ == "__main__":
    unittest.main(verbosity=2)
