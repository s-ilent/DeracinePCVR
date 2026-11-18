#!/usr/bin/env python3
"""Tests for the RGB (colour) correctness gate.

No emulator and no GPU: every fixture is a synthetic PNG written into a temporary directory. Each
negative case is a way a frame can still be "the same scene" while being colour-wrong: an R/B swap
that leaves the scene-signature grayscale structure untouched, a brightness/colour shift, a black
frame, a missing stereo eye, a size change, or a localised region error. Positive controls are the
identical image and a shift exactly on the tolerance boundary.
"""

from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

import numpy as np
from PIL import Image

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import image_validation as iv  # noqa: E402

WIDTH = 64
HEIGHT = 48


def noise(seed: int = 1, low: int = 40, high: int = 200) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return rng.integers(low, high, size=(HEIGHT, WIDTH, 3), dtype=np.uint8)


def offset(image: np.ndarray, delta: int, channel: int | None = None) -> np.ndarray:
    shifted = image.astype(np.int16)
    if channel is None:
        shifted += delta
    else:
        shifted[:, :, channel] += delta
    return np.clip(shifted, 0, 255).astype(np.uint8)


class ImageTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.counter = 0

    def write(self, name: str, array: np.ndarray) -> Path:
        path = self.directory / name
        Image.fromarray(np.asarray(array, dtype=np.uint8)).save(path, format="PNG")
        return path

    def compare_images(self, reference: np.ndarray, candidate: np.ndarray, tolerances=None,
                       provenance=None) -> dict:
        self.counter += 1
        self.reference_path = self.write(f"reference-{self.counter}.png", reference)
        self.candidate_path = self.write(f"candidate-{self.counter}.png", candidate)
        return iv.compare(self.reference_path, self.candidate_path, tolerances,
                          provenance=provenance)


class IdenticalImageTests(ImageTestCase):
    def test_identical_image_passes(self) -> None:
        image = noise()
        result = self.compare_images(image, image.copy())
        self.assertEqual(result["verdict"], "pass")
        self.assertTrue(result["valid"])
        self.assertEqual(result["exit_code"], 0)
        self.assertEqual(result["reasons"], [])
        self.assertTrue(result["scene_identity_untouched"])
        self.assertEqual(result["comparison"]["overall"]["mae"], 0.0)
        self.assertEqual(result["comparison"]["overall"]["bad_pixel_fraction"], 0.0)
        self.assertEqual(result["regions"]["worst"]["mae"], 0.0)
        self.assertFalse(result["channel_swap"]["suspected"])
        self.assertIsNone(result["stereo"]["missing_eye"])
        self.assertEqual(result["dimensions"]["match"], True)

    def test_module_does_not_import_the_scene_signature(self) -> None:
        source = (TOOLS / "image_validation.py").read_text(encoding="utf-8")
        imports = [
            line.strip()
            for line in source.splitlines()
            if line.strip().startswith(("import ", "from "))
        ]
        self.assertFalse(any("scene_signature" in line for line in imports))
        result = self.compare_images(noise(), noise())
        self.assertTrue(result["scene_identity_untouched"])


class ChannelSwapTests(ImageTestCase):
    def test_rb_swap_fails_even_though_the_grayscale_structure_is_identical(self) -> None:
        base = np.random.default_rng(3).integers(60, 160, size=(HEIGHT, WIDTH), dtype=np.uint8)
        reference = np.stack([base + 40, base, base], axis=2).astype(np.uint8)
        candidate = np.stack([base, base, base + 40], axis=2).astype(np.uint8)

        # scene_signature reduces RGB to the plain channel mean, so this construction gives it a
        # bit-identical grayscale image (and therefore similarity 1.0) while R and B are swapped.
        # It is deliberately not called here: this test proves the colour gate is independent.
        self.assertTrue(
            np.array_equal(
                reference.astype(np.float64).mean(axis=2),
                candidate.astype(np.float64).mean(axis=2),
            )
        )

        result = self.compare_images(reference, candidate)
        self.assertEqual(result["verdict"], "fail")
        self.assertTrue(result["channel_swap"]["suspected"])
        self.assertEqual(result["channel_swap"]["swapped_mae"], 0.0)
        self.assertGreater(result["channel_swap"]["normal_mae"], result["tolerances"]["max_mae"])
        self.assertTrue(
            any(reason.startswith("channel_swap_suspected") for reason in result["reasons"])
        )


class ShiftTests(ImageTestCase):
    def test_brightness_shift_beyond_tolerance_fails(self) -> None:
        reference = noise()
        result = self.compare_images(reference, offset(reference, 10))
        self.assertEqual(result["verdict"], "fail")
        self.assertFalse(result["channel_swap"]["suspected"])
        self.assertTrue(any("max_mae" in reason for reason in result["reasons"]))

    def test_single_channel_cast_fails_on_that_channel(self) -> None:
        reference = noise()
        result = self.compare_images(reference, offset(reference, 12, channel=2))
        self.assertEqual(result["verdict"], "fail")
        self.assertTrue(any("channel B" in reason for reason in result["reasons"]))


class BlackTests(ImageTestCase):
    def test_black_candidate_is_inconclusive_never_a_pass(self) -> None:
        reference = noise()
        black = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
        result = self.compare_images(reference, black)
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertFalse(result["valid"])
        self.assertEqual(result["exit_code"], 3)
        self.assertTrue(any("black_image" in reason for reason in result["reasons"]))

    def test_black_reference_is_inconclusive(self) -> None:
        black = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
        result = self.compare_images(black, black.copy())
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertTrue(any("reference_black" in reason for reason in result["reasons"]))

    def test_half_black_missing_eye_fails(self) -> None:
        reference = noise()
        for side in ("left", "right"):
            candidate = reference.copy()
            if side == "right":
                candidate[:, WIDTH // 2:] = 0
            else:
                candidate[:, :WIDTH // 2] = 0
            with self.subTest(side=side):
                result = self.compare_images(reference, candidate)
                self.assertEqual(result["verdict"], "fail")
                self.assertEqual(result["stereo"]["missing_eye"], side)
                self.assertLess(result["stereo"][f"{side}_energy"], 0.5)
                self.assertTrue(
                    any(reason.startswith("missing_eye") for reason in result["reasons"])
                )

    def test_duplicated_eye_is_detected(self) -> None:
        reference = noise()
        half = reference[:, :WIDTH // 2]
        candidate = np.concatenate([half, half], axis=1)
        result = self.compare_images(reference, candidate)
        self.assertEqual(result["verdict"], "fail")
        self.assertTrue(result["stereo"]["identical_halves"])
        self.assertEqual(result["stereo"]["missing_eye"], "duplicate")

    def test_uniform_candidate_is_inconclusive(self) -> None:
        reference = noise()
        uniform = np.full((HEIGHT, WIDTH, 3), 128, dtype=np.uint8)
        result = self.compare_images(reference, uniform)
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertTrue(any("uniform_candidate" in reason for reason in result["reasons"]))


class GeometryTests(ImageTestCase):
    def test_dimension_mismatch_fails(self) -> None:
        reference = noise()
        candidate = np.zeros((HEIGHT, WIDTH + 1, 3), dtype=np.uint8)
        result = self.compare_images(reference, candidate)
        self.assertEqual(result["verdict"], "fail")
        self.assertFalse(result["dimensions"]["match"])
        self.assertIsNone(result["comparison"])
        self.assertTrue(any("dimension_mismatch" in reason for reason in result["reasons"]))

    def test_unreadable_candidate_is_inconclusive(self) -> None:
        reference = noise()
        reference_path = self.write("reference.png", reference)
        result = iv.compare(reference_path, self.directory / "missing.png", None)
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertFalse(result["valid"])
        self.assertTrue(any("unreadable_candidate" in reason for reason in result["reasons"]))

    def test_grayscale_candidate_cannot_claim_colour(self) -> None:
        reference = noise()
        grayscale = reference[:, :, 0]
        result = self.compare_images(reference, grayscale)
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertTrue(any("no_colour_channels" in reason for reason in result["reasons"]))


class RegionTests(ImageTestCase):
    def test_region_error_is_located_and_fails(self) -> None:
        reference = noise()
        candidate = reference.copy()
        candidate[12:24, 32:48] = (
            np.clip(candidate[12:24, 32:48].astype(np.int16) + 20, 0, 255).astype(np.uint8)
        )
        result = self.compare_images(reference, candidate)
        self.assertEqual(result["verdict"], "fail")
        worst = result["regions"]["worst"]
        self.assertEqual((worst["column"], worst["row"]), (2, 1))
        self.assertGreater(worst["mae"], result["tolerances"]["max_region_mae"])
        # The whole-image mean alone would have passed; the region gate is what catches this.
        self.assertLess(result["comparison"]["overall"]["mae"], result["tolerances"]["max_mae"])
        self.assertTrue(any(reason.startswith("region_mae") for reason in result["reasons"]))

    def test_region_grid_is_configurable(self) -> None:
        reference = noise()
        result = self.compare_images(reference, reference.copy(), {"region_grid": [2, 2]})
        self.assertEqual(result["regions"]["grid"], [2, 2])
        self.assertEqual(len(result["regions"]["regions"]), 4)


class ToleranceTests(ImageTestCase):
    def test_boundary_inside_passes_outside_fails(self) -> None:
        reference = noise()
        inside = self.compare_images(reference, offset(reference, 2))
        self.assertEqual(inside["verdict"], "pass")
        self.assertAlmostEqual(inside["comparison"]["overall"]["mae"], 2.0, places=6)

        outside = self.compare_images(reference, offset(reference, 3))
        self.assertEqual(outside["verdict"], "fail")
        self.assertAlmostEqual(outside["comparison"]["overall"]["mae"], 3.0, places=6)
        self.assertTrue(any("max_mae" in reason for reason in outside["reasons"]))

    def test_custom_tolerance_changes_the_verdict(self) -> None:
        reference = noise()
        result = self.compare_images(reference, offset(reference, 3), {"max_mae": 5.0})
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual(result["tolerances"]["max_mae"], 5.0)

    def test_committed_tolerance_file_matches_module_defaults(self) -> None:
        path = TOOLS / "reference" / "image-tolerances.json"
        payload = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(iv.normalize_tolerances(payload), iv.normalize_tolerances(None))
        self.assertEqual(set(payload["documentation"]), set(iv.DEFAULT_TOLERANCES))
        self.assertEqual(iv.load_tolerances(path), iv.normalize_tolerances(None))

    def test_unknown_tolerance_key_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            iv.normalize_tolerances({"max_mea": 1.0})

    def test_fraction_tolerance_above_one_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            iv.normalize_tolerances({"max_bad_fraction": 1.5})


class ProvenanceTests(ImageTestCase):
    def provenance(self, reference_path: Path) -> dict:
        return {
            "schema_version": 1,
            "reference": {"sha256": iv.sha256_file(reference_path)},
            "source": {"commit": "a6946bd", "branch": "fix/rgb-validation"},
            "executable": {"sha256": "ab" * 32},
            "capture": {"dimensions": [WIDTH, HEIGHT], "conditions": "unittest synthetic frame"},
        }

    def test_matching_provenance_passes(self) -> None:
        image = noise()
        reference_path = self.write("reference.png", image)
        candidate_path = self.write("candidate.png", image.copy())
        provenance = self.provenance(reference_path)
        result = iv.compare(reference_path, candidate_path, None, provenance=provenance)
        self.assertEqual(result["verdict"], "pass")
        self.assertTrue(result["provenance"]["ok"])
        self.assertEqual(result["provenance"]["recorded"]["source_commit"], "a6946bd")
        self.assertEqual(result["provenance"]["mismatches"], [])

    def test_reference_hash_mismatch_fails(self) -> None:
        image = noise()
        reference_path = self.write("reference.png", image)
        candidate_path = self.write("candidate.png", image.copy())
        provenance = self.provenance(reference_path)
        provenance["reference"]["sha256"] = "0" * 64
        result = iv.compare(reference_path, candidate_path, None, provenance=provenance)
        self.assertEqual(result["verdict"], "fail")
        self.assertFalse(result["provenance"]["ok"])
        self.assertTrue(
            any(reason.startswith("provenance_mismatch") for reason in result["reasons"])
        )

    def test_provenance_without_reference_hash_is_inconclusive(self) -> None:
        image = noise()
        reference_path = self.write("reference.png", image)
        candidate_path = self.write("candidate.png", image.copy())
        result = iv.compare(reference_path, candidate_path, None,
                            provenance={"source": {"commit": "a6946bd"}})
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertIn("reference.sha256", result["provenance"]["missing"])

    def test_capture_dimensions_mismatch_fails(self) -> None:
        image = noise()
        reference_path = self.write("reference.png", image)
        candidate_path = self.write("candidate.png", image.copy())
        provenance = self.provenance(reference_path)
        provenance["capture"]["dimensions"] = [WIDTH + 1, HEIGHT]
        result = iv.compare(reference_path, candidate_path, None, provenance=provenance)
        self.assertEqual(result["verdict"], "fail")
        self.assertTrue(
            any("capture dimensions" in reason for reason in result["reasons"])
        )

    def test_provenance_is_not_ok_when_the_reference_cannot_be_read(self) -> None:
        image = noise()
        candidate_path = self.write("candidate.png", image)
        result = iv.compare(
            self.directory / "missing.png", candidate_path, None,
            provenance={"reference": {"sha256": "0" * 64}},
        )
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertFalse(result["provenance"]["ok"])


class AlphaTests(ImageTestCase):
    def test_alpha_is_reported_but_is_not_a_colour_failure(self) -> None:
        reference_rgb = noise()
        alpha = np.full((HEIGHT, WIDTH, 1), 255, dtype=np.uint8)
        reference = np.concatenate([reference_rgb, alpha], axis=2)
        candidate = np.concatenate(
            [reference_rgb, np.zeros((HEIGHT, WIDTH, 1), dtype=np.uint8)], axis=2
        )
        reference_path = self.write("rgba-reference.png", reference)
        candidate_path = self.write("rgba-candidate.png", candidate)
        result = iv.compare(reference_path, candidate_path, None)
        self.assertEqual(result["verdict"], "pass")
        self.assertIn("A", result["comparison"]["channels"])
        self.assertFalse(result["comparison"]["per_channel"]["A"]["is_colour"])
        self.assertEqual(result["comparison"]["overall"]["mae"], 0.0)


class CliTests(ImageTestCase):
    def run_cli(self, argv: list[str]) -> tuple[int, str, str]:
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = iv.main(argv)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_pass_exits_zero_and_prints_json(self) -> None:
        image = noise()
        reference = self.write("reference.png", image)
        candidate = self.write("candidate.png", image.copy())
        code, stdout, _ = self.run_cli(
            ["compare", "--reference", str(reference), "--candidate", str(candidate), "--json"]
        )
        self.assertEqual(code, 0)
        payload = json.loads(stdout)
        self.assertEqual(payload["verdict"], "pass")
        self.assertTrue(payload["valid"])

    def test_text_output_has_a_verdict_line(self) -> None:
        image = noise()
        reference = self.write("reference.png", image)
        candidate = self.write("candidate.png", image.copy())
        code, stdout, _ = self.run_cli(
            ["compare", "--reference", str(reference), "--candidate", str(candidate)]
        )
        self.assertEqual(code, 0)
        self.assertIn("PASS", stdout.splitlines()[0])

    def test_failure_exits_two(self) -> None:
        reference = noise()
        reference_path = self.write("reference.png", reference)
        candidate_path = self.write("candidate.png", offset(reference, 10))
        code, _, _ = self.run_cli(
            ["compare", "--reference", str(reference_path), "--candidate", str(candidate_path)]
        )
        self.assertEqual(code, 2)

    def test_black_image_exits_three(self) -> None:
        reference_path = self.write("reference.png", noise())
        black_path = self.write("black.png", np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8))
        code, _, _ = self.run_cli(
            ["compare", "--reference", str(reference_path), "--candidate", str(black_path)]
        )
        self.assertEqual(code, 3)

    def test_usage_errors_exit_three(self) -> None:
        code, _, stderr = self.run_cli([])
        self.assertEqual(code, 3)
        self.assertIn("usage error", stderr)

        reference_path = self.write("reference.png", noise())
        code, _, _ = self.run_cli(["compare", "--reference", str(reference_path)])
        self.assertEqual(code, 3)

    def test_committed_tolerance_file_is_accepted(self) -> None:
        image = noise()
        reference = self.write("reference.png", image)
        candidate = self.write("candidate.png", image.copy())
        tolerances = TOOLS / "reference" / "image-tolerances.json"
        code, stdout, _ = self.run_cli([
            "compare", "--reference", str(reference), "--candidate", str(candidate),
            "--tolerances", str(tolerances), "--json",
        ])
        self.assertEqual(code, 0)
        payload = json.loads(stdout)
        self.assertEqual(payload["tolerances"], iv.normalize_tolerances(None))

    def test_provenance_mismatch_from_file_exits_two(self) -> None:
        image = noise()
        reference = self.write("reference.png", image)
        candidate = self.write("candidate.png", image.copy())
        provenance = {
            "reference": {"sha256": "0" * 64},
            "source": {"commit": "a6946bd"},
            "executable": {"sha256": "ab" * 32},
            "capture": {"conditions": "unittest"},
        }
        provenance_path = self.directory / "provenance.json"
        provenance_path.write_text(json.dumps(provenance), encoding="utf-8")
        code, stdout, _ = self.run_cli([
            "compare", "--reference", str(reference), "--candidate", str(candidate),
            "--provenance", str(provenance_path), "--json",
        ])
        self.assertEqual(code, 2)
        payload = json.loads(stdout)
        self.assertEqual(payload["verdict"], "fail")
        self.assertTrue(any("provenance_mismatch" in reason for reason in payload["reasons"]))

    def test_malformed_provenance_file_exits_three(self) -> None:
        image = noise()
        reference = self.write("reference.png", image)
        candidate = self.write("candidate.png", image.copy())
        provenance_path = self.directory / "provenance.json"
        provenance_path.write_text("{not json", encoding="utf-8")
        code, _, stderr = self.run_cli([
            "compare", "--reference", str(reference), "--candidate", str(candidate),
            "--provenance", str(provenance_path),
        ])
        self.assertEqual(code, 3)
        self.assertIn("not valid JSON", stderr)

    def test_missing_tolerances_file_exits_three(self) -> None:
        image = noise()
        reference = self.write("reference.png", image)
        candidate = self.write("candidate.png", image.copy())
        code, _, stderr = self.run_cli([
            "compare", "--reference", str(reference), "--candidate", str(candidate),
            "--tolerances", str(self.directory / "absent.json"),
        ])
        self.assertEqual(code, 3)
        self.assertIn("cannot read tolerances", stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
