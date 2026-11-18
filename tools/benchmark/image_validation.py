#!/usr/bin/env python3
"""Colour (RGB) correctness validation for benchmark screenshots.

``scene_signature.py`` answers one question: *is this the expected scene?* It reduces the image to
a 64x36 grayscale image and correlates normalised luminance, which is deliberately insensitive to
hue. A channel swap, a colour-processing regression or a missing stereo eye can still pass it. This
module answers a different question: *does this image still carry the expected colour data?* It
compares reference and candidate pixel by pixel, per channel and per region, and turns black
frames, missing stereo eyes and R/B channel swaps into explicit verdicts instead of one similarity
number. It never imports or computes the scene signature: scene identity and colour correctness
stay separate gates, and ``scene_identity_untouched`` is reported in every result.

Defaults live in ``reference/image-tolerances.json`` (documented copy); ``DEFAULT_TOLERANCES`` below
is the same set used when no file is given. All comparisons are in 0-255 per-channel units.

CLI::

    python image_validation.py compare --reference ref.png --candidate cand.png \\
        [--tolerances reference/image-tolerances.json] [--provenance provenance.json] [--json]

Exit codes: 0 pass, 2 fail, 3 inconclusive or usage error.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path

import numpy as np

# Effective defaults; reference/image-tolerances.json carries the same values plus prose. A caller
# may pass a partial mapping to compare(); it is merged over these values.
DEFAULT_TOLERANCES = {
    # Overall mean absolute error over the R/G/B channels, 0-255.
    "max_mae": 2.0,
    # Per-channel mean absolute error, 0-255.
    "max_channel_mae": 3.0,
    # Per-channel 99th percentile of the absolute difference, 0-255.
    "max_p99": 12.0,
    # Fraction of pixels with any colour channel differing by more than pixel_diff_threshold.
    "max_bad_fraction": 0.01,
    "pixel_diff_threshold": 8.0,
    # Worst region of the configurable grid, mean absolute error 0-255.
    "max_region_mae": 6.0,
    # Grid as an int (square) or [columns, rows].
    "region_grid": 4,
    # A channel value <= this is "near zero" for black-frame detection.
    "black_luma_threshold": 4.0,
    # Black when every colour channel is >= this fraction near zero (whole image).
    "black_fraction_max": 0.98,
    # Stereo side-by-side checks (missing eye / duplicated eye).
    "stereo_side_by_side": True,
    # A half whose non-black pixel fraction is below this has no usable eye content.
    "min_eye_energy": 0.5,
    # Halves whose mean absolute difference is <= this are treated as identical.
    "eye_identical_mae": 1.0,
    # Swapped R/B counts as material when swapped MAE <= this fraction of the direct MAE.
    "channel_swap_improvement": 0.5,
    # An image whose colour-channel standard deviation is below this is uniform: no usable data.
    "min_std": 1.0,
}

EXIT_PASS = 0
EXIT_FAIL = 2
EXIT_INCONCLUSIVE = 3
EXIT_CODES = {"pass": EXIT_PASS, "fail": EXIT_FAIL, "inconclusive": EXIT_INCONCLUSIVE}

# PIL modes that cannot carry RGB data. P is indexed and may be colour, so it is not listed.
_NO_COLOUR_MODES = frozenset({"1", "L", "LA", "I", "I;16", "I;16B", "I;16L", "F"})
_WRAPPER_KEYS = frozenset({"schema_version", "description", "documentation", "tolerances"})
_FRACTION_KEYS = frozenset({"max_bad_fraction", "min_eye_energy", "black_fraction_max"})
_UNIT_LIMIT_KEYS = frozenset({"black_luma_threshold", "pixel_diff_threshold"})


class UsageError(Exception):
    """Raised for CLI argument errors so usage exits with 3, not argparse's 2 (a fail)."""


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message):  # noqa: D401 - argparse hook
        raise UsageError(message)


def sha256_file(path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _grid(value, source: str) -> list[int]:
    if isinstance(value, bool):
        raise ValueError(f"{source}: region_grid must be an int or [columns, rows]")
    if isinstance(value, int):
        if value < 1:
            raise ValueError(f"{source}: region_grid must be >= 1")
        return [value, value]
    if isinstance(value, (list, tuple)) and len(value) == 2:
        columns, rows = value
        for part in (columns, rows):
            if isinstance(part, bool) or not isinstance(part, int) or part < 1:
                raise ValueError(f"{source}: region_grid entries must be integers >= 1")
        return [columns, rows]
    raise ValueError(f"{source}: region_grid must be an int or [columns, rows]")


def normalize_tolerances(raw, source: str = "<mapping>") -> dict:
    """Merge a partial tolerance mapping over the defaults and validate every value.

    Unknown keys are rejected: a typo such as ``max_mea`` must not silently keep the default.
    ``reference/image-tolerances.json`` wraps the values in a ``tolerances`` object; a bare mapping
    is accepted too.
    """
    if raw is None:
        merged = dict(DEFAULT_TOLERANCES)
    else:
        if not isinstance(raw, dict):
            raise ValueError(f"{source}: tolerances must be a JSON object")
        if "tolerances" in raw:
            extra = sorted(set(raw) - _WRAPPER_KEYS)
            if extra:
                raise ValueError(f"{source}: unknown top-level key(s) {extra}")
            payload = raw["tolerances"]
            if not isinstance(payload, dict):
                raise ValueError(f"{source}: 'tolerances' must be a JSON object")
        else:
            payload = raw
        unknown = sorted(set(payload) - set(DEFAULT_TOLERANCES))
        if unknown:
            raise ValueError(
                f"{source}: unknown tolerance(s) {unknown}; "
                f"known keys: {sorted(DEFAULT_TOLERANCES)}"
            )
        merged = dict(DEFAULT_TOLERANCES)
        merged.update(payload)

    if isinstance(merged["region_grid"], (list, tuple)):
        merged["region_grid"] = _grid(merged["region_grid"], source)

    for key, value in merged.items():
        if key == "stereo_side_by_side":
            if not isinstance(value, bool):
                raise ValueError(f"{source}: stereo_side_by_side must be true or false")
            merged[key] = value
            continue
        if key == "region_grid":
            merged[key] = _grid(value, source)
            continue
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError(f"{source}: {key} must be a number")
        number = float(value)
        if not math.isfinite(number) or number < 0.0:
            raise ValueError(f"{source}: {key} must be a finite number >= 0")
        if key in _FRACTION_KEYS and number > 1.0:
            raise ValueError(f"{source}: {key} is a fraction and must be <= 1")
        if key in _UNIT_LIMIT_KEYS and number > 255.0:
            raise ValueError(f"{source}: {key} is a 0-255 value and must be <= 255")
        merged[key] = number
    return merged


def load_tolerances(path) -> dict:
    """Load and normalise a tolerance JSON file."""
    path = Path(path)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ValueError(f"cannot read tolerances {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"tolerances {path} is not valid JSON: {exc}") from exc
    return normalize_tolerances(data, source=str(path))


def _read_json(path: Path, label: str) -> dict:
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except OSError as exc:
        raise ValueError(f"cannot read {label} {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"{label} {path} is not valid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise ValueError(f"{label} {path} must contain a JSON object")
    return data


def _image_metadata(path) -> dict:
    from PIL import Image

    try:
        with Image.open(path) as image:
            return {
                "path": str(path),
                "format": image.format or "unknown",
                "mode": image.mode,
                "dimensions": [int(image.width), int(image.height)],
                "has_alpha": "A" in image.getbands(),
                "has_colour": image.mode not in _NO_COLOUR_MODES,
                "sha256": sha256_file(path),
            }
    except Exception as exc:  # unreadable/unidentified image: reported, never a pass
        raise ValueError(f"{path}: {type(exc).__name__}: {exc}") from exc


def _image_array(path, mode: str) -> np.ndarray:
    from PIL import Image

    with Image.open(path) as image:
        return np.asarray(image.convert(mode), dtype=np.uint8)


def _percentile(hist: np.ndarray, quantile: float) -> float:
    """Nearest-rank percentile from an integer-value histogram (0-255)."""
    total = int(hist.sum())
    if total <= 0:
        return 0.0
    cumulative = np.cumsum(hist)
    index = int(np.searchsorted(cumulative, quantile * total, side="left"))
    return float(min(index, 255))


def _stats(hist: np.ndarray) -> dict:
    total = int(hist.sum())
    if total <= 0:
        return {"mae": 0.0, "max": 0, "p95": 0.0, "p99": 0.0}
    values = np.arange(256, dtype=np.float64)
    mae = float((hist.astype(np.float64) * values).sum() / total)
    nonzero = np.nonzero(hist)[0]
    return {
        "mae": round(mae, 6),
        "max": int(nonzero[-1]) if nonzero.size else 0,
        "p95": round(_percentile(hist, 0.95), 6),
        "p99": round(_percentile(hist, 0.99), 6),
    }


def _bad_fraction(hist: np.ndarray, threshold: float) -> float:
    """Fraction of samples with a difference strictly greater than ``threshold``."""
    total = int(hist.sum())
    if total <= 0:
        return 0.0
    first_bad = int(math.floor(threshold)) + 1
    if first_bad > 255:
        return 0.0
    return round(float(hist[first_bad:].sum()) / total, 6)


def _diff_histogram(left: np.ndarray, right: np.ndarray) -> np.ndarray:
    diff = np.abs(left.astype(np.int16) - right.astype(np.int16))
    return np.bincount(diff.ravel(), minlength=256).astype(np.int64)


def _channel_std(array: np.ndarray, colour_indices: list[int]) -> float:
    total = 0
    total_sum = 0
    total_square = 0
    for index in colour_indices:
        channel = array[:, :, index]
        total += int(channel.size)
        total_sum += int(channel.sum(dtype=np.int64))
        total_square += int(np.square(channel.astype(np.int64)).sum())
    if total == 0:
        return 0.0
    mean = total_sum / total
    variance = total_square / total - mean * mean
    return math.sqrt(max(0.0, variance))


def _halves_analysis(array: np.ndarray, channel_names: list[str], colour_indices: list[int],
                     threshold: float) -> dict:
    """Near-zero fraction and energy per stereo half, and the mean difference between halves."""
    height, width = array.shape[:2]
    split = width // 2
    if split < 1:
        return {"applicable": False, "split_column": 0, "left": None, "right": None,
                "halves_mae": None}
    sides = {"left": array[:, :split, :], "right": array[:, width - split:, :]}
    result: dict = {"applicable": True, "split_column": split}
    for name, part in sides.items():
        near = {
            channel_names[index]: round(float((part[:, :, index] <= threshold).mean()), 6)
            for index in range(len(channel_names))
        }
        dark = (part[:, :, colour_indices] <= threshold).all(axis=2)
        result[name] = {
            "near_zero_fraction": near,
            "energy": round(float(1.0 - dark.mean()), 6),
        }
    left = sides["left"].astype(np.int16)
    right = sides["right"].astype(np.int16)
    total = 0.0
    for index in colour_indices:
        total += float(np.abs(left[:, :, index] - right[:, :, index]).mean())
    result["halves_mae"] = round(total / len(colour_indices), 6)
    return result


def _frame_analysis(array: np.ndarray, channel_names: list[str], colour_indices: list[int],
                    tolerances: dict) -> dict:
    """Black / uniform / stereo-half statistics for one frame."""
    threshold = tolerances["black_luma_threshold"]
    near = {
        channel_names[index]: round(float((array[:, :, index] <= threshold).mean()), 6)
        for index in range(len(channel_names))
    }
    dark = (array[:, :, colour_indices] <= threshold).all(axis=2)
    std = _channel_std(array, colour_indices)
    is_black = all(
        near[channel_names[index]] >= tolerances["black_fraction_max"]
        for index in colour_indices
    )
    return {
        "near_zero_fraction": near,
        "energy": round(float(1.0 - dark.mean()), 6),
        "std": round(std, 6),
        "is_black": bool(is_black),
        "is_uniform": bool(std < tolerances["min_std"]),
        "halves": _halves_analysis(array, channel_names, colour_indices, threshold),
    }


def _analyse(ref: np.ndarray, cand: np.ndarray, channel_names: list[str],
             tolerances: dict) -> dict:
    """All pixel statistics: overall/per-channel, regions, channel swap, black and stereo."""
    height, width = ref.shape[:2]
    colour_names = [name for name in channel_names if name != "A"]
    colour_indices = [channel_names.index(name) for name in colour_names]
    threshold = tolerances["pixel_diff_threshold"]

    columns, rows = tolerances["region_grid"]
    columns = max(1, min(int(columns), width))
    rows = max(1, min(int(rows), height))
    x_edges = [int(round(column * width / columns)) for column in range(columns + 1)]
    y_edges = [int(round(row * height / rows)) for row in range(rows + 1)]
    x_edges[-1] = width
    y_edges[-1] = height
    region_sums = np.zeros((rows, columns), dtype=np.int64)
    region_counts = np.zeros((rows, columns), dtype=np.int64)

    hists: dict[str, np.ndarray] = {}
    overall_hist = np.zeros(256, dtype=np.int64)
    bad_any = np.zeros((height, width), dtype=bool)
    for index, name in enumerate(channel_names):
        left = ref[:, :, index]
        right = cand[:, :, index]
        diff = np.abs(left.astype(np.int16) - right.astype(np.int16))
        hist = np.bincount(diff.ravel(), minlength=256).astype(np.int64)
        hists[name] = hist
        is_colour = name in colour_names
        if is_colour:
            overall_hist += hist
            bad_any |= diff > threshold
            for row in range(rows):
                y0, y1 = y_edges[row], y_edges[row + 1]
                for column in range(columns):
                    x0, x1 = x_edges[column], x_edges[column + 1]
                    patch = diff[y0:y1, x0:x1]
                    region_sums[row, column] += int(patch.sum(dtype=np.int64))
                    region_counts[row, column] += int(patch.size)

    per_channel = {}
    for name in channel_names:
        stats = _stats(hists[name])
        stats["bad_fraction"] = _bad_fraction(hists[name], threshold)
        stats["is_colour"] = name in colour_names
        per_channel[name] = stats

    overall = _stats(overall_hist)
    overall["bad_pixel_fraction"] = round(float(bad_any.mean()), 6)

    region_list = []
    worst = None
    for row in range(rows):
        for column in range(columns):
            count = int(region_counts[row, column])
            mae = round(float(region_sums[row, column]) / count, 6) if count else None
            entry = {
                "index": row * columns + column,
                "row": row,
                "column": column,
                "box": [x_edges[column], y_edges[row], x_edges[column + 1], y_edges[row + 1]],
                "pixels": count // max(1, len(colour_names)),
                "mae": mae,
            }
            region_list.append(entry)
            if mae is not None and (worst is None or mae > worst["mae"]):
                worst = entry

    channel_swap = {"suspected": False, "normal_mae": overall["mae"], "swapped_mae": None,
                    "improvement": None,
                    "threshold": tolerances["channel_swap_improvement"]}
    if "R" in colour_names and "B" in colour_names:
        index_red = channel_names.index("R")
        index_blue = channel_names.index("B")
        # Swapping the reference's R and B is the hypothesis "the candidate's channels are
        # ordered R<->B": compare reference_R with candidate_B and reference_B with candidate_R.
        swapped_hist = np.zeros(256, dtype=np.int64)
        for name in colour_names:
            if name == "R":
                swapped_hist += _diff_histogram(ref[:, :, index_red], cand[:, :, index_blue])
            elif name == "B":
                swapped_hist += _diff_histogram(ref[:, :, index_blue], cand[:, :, index_red])
            else:
                swapped_hist += hists[name]
        swapped_mae = _stats(swapped_hist)["mae"]
        normal_mae = overall["mae"]
        improvement = round(swapped_mae / normal_mae, 6) if normal_mae > 0 else None
        channel_swap.update({
            "swapped_mae": swapped_mae,
            "improvement": improvement,
            "suspected": bool(
                normal_mae > tolerances["max_mae"]
                and swapped_mae <= normal_mae * tolerances["channel_swap_improvement"]
            ),
        })

    candidate_frame = _frame_analysis(cand, channel_names, colour_indices, tolerances)
    reference_frame = _frame_analysis(ref, channel_names, colour_indices, tolerances)
    candidate_halves = candidate_frame["halves"]
    reference_halves = reference_frame["halves"]

    stereo = {
        "expected": bool(tolerances["stereo_side_by_side"]),
        "applicable": bool(candidate_halves["applicable"]),
        "left_energy": None,
        "right_energy": None,
        "identical_halves": False,
        "reference_identical_halves": False,
        "missing_eye": None,
    }
    if candidate_halves["applicable"]:
        stereo["left_energy"] = candidate_halves["left"]["energy"]
        stereo["right_energy"] = candidate_halves["right"]["energy"]
        stereo["identical_halves"] = bool(
            candidate_halves["halves_mae"] is not None
            and candidate_halves["halves_mae"] <= tolerances["eye_identical_mae"]
        )
        stereo["reference_identical_halves"] = bool(
            reference_halves["applicable"]
            and reference_halves["halves_mae"] is not None
            and reference_halves["halves_mae"] <= tolerances["eye_identical_mae"]
        )
        if stereo["expected"]:
            left_energy = stereo["left_energy"]
            right_energy = stereo["right_energy"]
            minimum = tolerances["min_eye_energy"]
            if left_energy < minimum and right_energy < minimum:
                stereo["missing_eye"] = "both"
            elif left_energy < minimum:
                stereo["missing_eye"] = "left"
            elif right_energy < minimum:
                stereo["missing_eye"] = "right"
            elif stereo["identical_halves"] and not stereo["reference_identical_halves"]:
                # One eye's picture duplicated into the other: no real stereo pair.
                stereo["missing_eye"] = "duplicate"

    return {
        "comparison": {
            "mode": None,  # filled by compare() with the common mode actually decoded
            "channels": list(channel_names),
            "colour_channels": colour_names,
            "pixel_count": height * width,
            "pixel_diff_threshold": threshold,
            "overall": overall,
            "per_channel": per_channel,
        },
        "regions": {
            "grid": [columns, rows],
            "regions": region_list,
            "worst": worst,
            "tolerance_max_region_mae": tolerances["max_region_mae"],
        },
        "channel_swap": channel_swap,
        "black": {
            "luma_threshold": tolerances["black_luma_threshold"],
            "black_fraction_max": tolerances["black_fraction_max"],
            "min_std": tolerances["min_std"],
            "reference": reference_frame,
            "candidate": candidate_frame,
            "reference_is_black": reference_frame["is_black"],
            "image_is_black": candidate_frame["is_black"],
            "reference_is_uniform": reference_frame["is_uniform"],
            "candidate_is_uniform": candidate_frame["is_uniform"],
        },
        "stereo": stereo,
    }


def _lookup(data: dict, *paths):
    for path in paths:
        node = data
        found = True
        for part in path:
            if isinstance(node, dict) and part in node:
                node = node[part]
            else:
                found = False
                break
        if found and node not in (None, "", []):
            return node
    return None


def _as_dimensions(value):
    if isinstance(value, (list, tuple)) and len(value) == 2:
        try:
            return [int(value[0]), int(value[1])]
        except (TypeError, ValueError):
            return None
    return None


def _apply_provenance(result: dict, provenance) -> tuple[str | None, str | None]:
    """Check a provenance record; return (verdict effect, reason) without ignoring mismatches."""
    if provenance is None:
        result["provenance"] = {"provided": False, "ok": None}
        return None, None
    data = provenance
    if isinstance(data, (str, Path)):
        data = _read_json(Path(data), "provenance")
    if not isinstance(data, dict):
        raise ValueError("provenance must be a JSON object")

    mismatches: list[str] = []
    missing: list[str] = []
    checked: dict = {}
    reference_problem = False

    actual_reference = result.get("reference", {}).get("sha256")
    expected_reference = _lookup(data, ("reference", "sha256"), ("reference_sha256",))
    if expected_reference is None:
        missing.append("reference.sha256")
        reference_problem = True
    elif actual_reference is None:
        missing.append("reference.sha256 (image unreadable, cannot verify)")
        reference_problem = True
    elif str(expected_reference).lower() != str(actual_reference).lower():
        mismatches.append(
            f"reference sha256 recorded {expected_reference} != actual {actual_reference}"
        )

    actual_candidate = result.get("candidate", {}).get("sha256")
    expected_candidate = _lookup(
        data, ("candidate", "sha256"), ("candidate_sha256",), ("capture", "candidate_sha256")
    )
    if expected_candidate is not None:
        if actual_candidate is None:
            missing.append("candidate.sha256 (image unreadable, cannot verify)")
        elif str(expected_candidate).lower() != str(actual_candidate).lower():
            mismatches.append(
                f"candidate sha256 recorded {expected_candidate} != actual {actual_candidate}"
            )

    expected_reference_dimensions = _as_dimensions(
        _lookup(data, ("reference", "dimensions"), ("reference", "size"))
    )
    actual_reference_dimensions = result.get("dimensions", {}).get("reference")
    if expected_reference_dimensions is not None and actual_reference_dimensions is not None:
        if expected_reference_dimensions != list(actual_reference_dimensions):
            mismatches.append(
                f"reference dimensions recorded {expected_reference_dimensions} != actual "
                f"{actual_reference_dimensions}"
            )

    expected_capture_dimensions = _as_dimensions(
        _lookup(data, ("capture", "dimensions"), ("candidate", "dimensions"), ("capture", "size"))
    )
    actual_candidate_dimensions = result.get("dimensions", {}).get("candidate")
    if expected_capture_dimensions is not None and actual_candidate_dimensions is not None:
        if expected_capture_dimensions != list(actual_candidate_dimensions):
            mismatches.append(
                f"capture dimensions recorded {expected_capture_dimensions} != actual "
                f"{actual_candidate_dimensions}"
            )

    if expected_reference is not None and actual_reference is not None:
        checked["reference_sha256"] = {
            "ok": not any("reference sha256" in item for item in mismatches),
            "recorded": expected_reference,
            "actual": actual_reference,
        }

    recorded = {
        "reference_path": _lookup(data, ("reference", "path")),
        "source_commit": _lookup(data, ("source", "commit"), ("source_commit",), ("commit",)),
        "source_branch": _lookup(data, ("source", "branch")),
        "source_worktree": _lookup(data, ("source", "worktree")),
        "executable_sha256": _lookup(
            data, ("executable", "sha256"), ("executable_sha256",)
        ),
        "executable_path": _lookup(data, ("executable", "path")),
        "capture_conditions": _lookup(
            data, ("capture", "conditions"), ("capture_conditions",)
        ),
    }
    if recorded["source_commit"] is None:
        missing.append("source.commit")
    if recorded["executable_sha256"] is None:
        missing.append("executable.sha256")
    if recorded["capture_conditions"] is None:
        missing.append("capture.conditions")

    block = {
        "provided": True,
        "ok": not mismatches and not reference_problem,
        "checked": checked,
        "mismatches": mismatches,
        "missing": missing,
        "recorded": recorded,
    }
    result["provenance"] = block
    if mismatches:
        return "fail", "provenance_mismatch: " + "; ".join(mismatches)
    if reference_problem:
        return "inconclusive", (
            "provenance_incomplete: the provenance record does not identify the reference "
            "image, so the reference cannot be verified"
        )
    return None, None


def _finish(result: dict, verdict: str, reasons: list[str], provenance) -> dict:
    effect, reason = _apply_provenance(result, provenance)
    if effect is not None:
        reasons.append(reason)
        if effect == "fail":
            verdict = "fail"
        elif verdict == "pass":
            verdict = "inconclusive"
    result["reasons"] = reasons
    result["verdict"] = verdict
    result["valid"] = verdict == "pass"
    result["exit_code"] = EXIT_CODES[verdict]
    return result


def compare(reference_path, candidate_path, tolerances=None, provenance=None) -> dict:
    """Compare a candidate screenshot with the reviewed reference.

    ``tolerances`` is a partial/complete mapping, a tolerance file path, or None for the defaults.
    ``provenance`` is an optional provenance mapping or path; a mismatch fails the verdict and is
    never silently ignored. ``scene_identity_untouched`` is always True: scene identity is a
    separate gate (``scene_signature.py``) and is not computed here.
    """
    if isinstance(tolerances, (str, Path)):
        effective = load_tolerances(tolerances)
    else:
        effective = normalize_tolerances(tolerances)

    result = {
        "schema_version": 1,
        "kind": "deracine-rgb-validation",
        "scene_identity_untouched": True,
        "valid": False,
        "verdict": "inconclusive",
        "exit_code": EXIT_INCONCLUSIVE,
        "reasons": [],
        "dimensions": {"reference": None, "candidate": None, "match": None},
        "mode": {"reference": None, "candidate": None},
        "format": {"reference": None, "candidate": None},
        "reference": {},
        "candidate": {},
        "comparison": None,
        "regions": None,
        "black": None,
        "stereo": None,
        "channel_swap": None,
        "tolerances": effective,
        "checks": [],
        "provenance": {"provided": False, "ok": None},
    }
    reasons: list[str] = result["reasons"]
    checks: list[dict] = result["checks"]

    def check(name: str, ok: bool, detail: str = "") -> bool:
        checks.append({"name": name, "ok": bool(ok), "detail": detail})
        return bool(ok)

    unreadable = False
    for label, path in (("reference", reference_path), ("candidate", candidate_path)):
        try:
            metadata = _image_metadata(path)
        except Exception as exc:
            result[label] = {"path": str(path), "error": str(exc)}
            check(f"{label}_readable", False, str(exc))
            reasons.append(f"unreadable_{label}: {exc}")
            unreadable = True
            continue
        result[label] = metadata
        result["dimensions"][label] = metadata["dimensions"]
        result["mode"][label] = metadata["mode"]
        result["format"][label] = metadata["format"]
        check(
            f"{label}_readable",
            True,
            f"{metadata['mode']} {metadata['dimensions'][0]}x{metadata['dimensions'][1]} "
            f"{metadata['format']}",
        )
    if unreadable:
        return _finish(result, "inconclusive", reasons, provenance)

    if result["dimensions"]["reference"] != result["dimensions"]["candidate"]:
        result["dimensions"]["match"] = False
        check(
            "dimensions",
            False,
            f"reference {result['dimensions']['reference']} != "
            f"candidate {result['dimensions']['candidate']}",
        )
        reasons.append(
            f"dimension_mismatch: reference {result['dimensions']['reference']} != "
            f"candidate {result['dimensions']['candidate']}"
        )
        return _finish(result, "fail", reasons, provenance)
    result["dimensions"]["match"] = True
    check("dimensions", True, f"{result['dimensions']['reference']}")

    if not result["reference"]["has_colour"] or not result["candidate"]["has_colour"]:
        check(
            "colour_channels",
            False,
            f"reference mode {result['reference']['mode']}, "
            f"candidate mode {result['candidate']['mode']}",
        )
        reasons.append(
            "no_colour_channels: at least one image has no RGB channels "
            f"(reference {result['reference']['mode']}, candidate {result['candidate']['mode']})"
        )
        return _finish(result, "inconclusive", reasons, provenance)
    check("colour_channels", True, "both images carry RGB data")

    common_mode = (
        "RGBA"
        if result["reference"]["has_alpha"] and result["candidate"]["has_alpha"]
        else "RGB"
    )
    check(
        "comparison_mode",
        True,
        f"{common_mode} (alpha compared only when both images carry it)",
    )
    try:
        reference_array = _image_array(reference_path, common_mode)
        candidate_array = _image_array(candidate_path, common_mode)
    except Exception as exc:
        reasons.append(f"unreadable_pixels: {exc}")
        check("readable_pixels", False, str(exc))
        return _finish(result, "inconclusive", reasons, provenance)
    if reference_array.shape != candidate_array.shape:
        result["dimensions"]["match"] = False
        check(
            "dimensions",
            False,
            f"decoded shapes differ {reference_array.shape} != {candidate_array.shape}",
        )
        reasons.append(
            f"dimension_mismatch: decoded shapes differ {reference_array.shape} != "
            f"{candidate_array.shape}"
        )
        return _finish(result, "fail", reasons, provenance)
    check("readable_pixels", True, f"{list(reference_array.shape)}")

    channel_names = list(common_mode)
    analysis = _analyse(reference_array, candidate_array, channel_names, effective)
    result["comparison"] = analysis["comparison"]
    result["comparison"]["mode"] = common_mode
    result["regions"] = analysis["regions"]
    result["channel_swap"] = analysis["channel_swap"]
    result["black"] = analysis["black"]
    result["stereo"] = analysis["stereo"]

    reference_frame = analysis["black"]["reference"]
    candidate_frame = analysis["black"]["candidate"]
    colour_names = analysis["comparison"]["colour_channels"]

    check("reference_usable", not (reference_frame["is_black"] or reference_frame["is_uniform"]),
          f"std {reference_frame['std']}, energy {reference_frame['energy']}")
    check("candidate_usable", not (candidate_frame["is_black"] or candidate_frame["is_uniform"]),
          f"std {candidate_frame['std']}, energy {candidate_frame['energy']}")

    if reference_frame["is_black"]:
        reasons.append("reference_black: the reference image is effectively black")
        return _finish(result, "inconclusive", reasons, provenance)
    if reference_frame["is_uniform"]:
        reasons.append(
            f"reference_uniform: the reference image has no structure (std "
            f"{reference_frame['std']} < {effective['min_std']})"
        )
        return _finish(result, "inconclusive", reasons, provenance)
    if candidate_frame["is_black"]:
        reasons.append(
            "black_image: the candidate is effectively black (near-zero fraction "
            f">= {effective['black_fraction_max']} in every colour channel); "
            "no colour claim is possible"
        )
        return _finish(result, "inconclusive", reasons, provenance)
    if candidate_frame["is_uniform"]:
        reasons.append(
            f"uniform_candidate: the candidate has no structure (std "
            f"{candidate_frame['std']} < {effective['min_std']}); no colour claim is possible"
        )
        return _finish(result, "inconclusive", reasons, provenance)

    stereo = analysis["stereo"]
    if stereo["expected"] and stereo["missing_eye"] is not None:
        if stereo["missing_eye"] == "duplicate":
            detail = ("both stereo halves are identical (halves mae "
                      f"{analysis['black']['candidate']['halves']['halves_mae']} <= "
                      f"{effective['eye_identical_mae']}) while the reference halves differ")
        else:
            detail = (
                f"{stereo['missing_eye']} half energy below {effective['min_eye_energy']} "
                f"(left {stereo['left_energy']}, right {stereo['right_energy']})"
            )
        reasons.append(f"missing_eye: {detail}")
        check("missing_eye", False, detail)

    channel_swap = analysis["channel_swap"]
    if channel_swap["suspected"]:
        reasons.append(
            "channel_swap_suspected: swapped R/B MAE "
            f"{channel_swap['swapped_mae']:.3f} is materially better than the direct MAE "
            f"{channel_swap['normal_mae']:.3f} "
            f"(<= {channel_swap['threshold']:.2f} x direct)"
        )
        check("channel_swap", False, f"direct {channel_swap['normal_mae']}, "
                                     f"swapped {channel_swap['swapped_mae']}")

    overall = analysis["comparison"]["overall"]
    per_channel = analysis["comparison"]["per_channel"]
    if overall["mae"] > effective["max_mae"]:
        reasons.append(
            f"max_mae: overall mean absolute error {overall['mae']:.3f} exceeds "
            f"{effective['max_mae']:.3f}"
        )
        check("max_mae", False, f"{overall['mae']} > {effective['max_mae']}")
    if overall["p99"] > effective["max_p99"]:
        reasons.append(
            f"max_p99: overall p99 absolute difference {overall['p99']:.3f} exceeds "
            f"{effective['max_p99']:.3f}"
        )
        check("max_p99", False, f"{overall['p99']} > {effective['max_p99']}")
    if overall["bad_pixel_fraction"] > effective["max_bad_fraction"]:
        reasons.append(
            f"max_bad_fraction: {overall['bad_pixel_fraction']:.4f} of pixels differ by more "
            f"than {effective['pixel_diff_threshold']:.1f} "
            f"(limit {effective['max_bad_fraction']:.4f})"
        )
        check("max_bad_fraction", False, f"{overall['bad_pixel_fraction']}")

    for name in colour_names:
        channel = per_channel[name]
        if channel["mae"] > effective["max_channel_mae"]:
            reasons.append(
                f"max_channel_mae: channel {name} mean absolute error {channel['mae']:.3f} "
                f"exceeds {effective['max_channel_mae']:.3f}"
            )
            check(f"max_channel_mae_{name}", False, f"{channel['mae']}")
        if channel["p99"] > effective["max_p99"]:
            reasons.append(
                f"max_p99_{name}: channel {name} p99 {channel['p99']:.3f} exceeds "
                f"{effective['max_p99']:.3f}"
            )
            check(f"max_p99_{name}", False, f"{channel['p99']}")

    worst = analysis["regions"]["worst"]
    if (
        worst is not None
        and worst["mae"] is not None
        and worst["mae"] > effective["max_region_mae"]
    ):
        reasons.append(
            f"region_mae: worst {analysis['regions']['grid'][0]}x"
            f"{analysis['regions']['grid'][1]} region column {worst['column']} row "
            f"{worst['row']} has MAE {worst['mae']:.3f}, above "
            f"{effective['max_region_mae']:.3f}"
        )
        check("region_mae", False,
              f"column {worst['column']} row {worst['row']} MAE {worst['mae']}")

    verdict = "fail" if reasons else "pass"
    return _finish(result, verdict, reasons, provenance)


def _format_text(result: dict) -> str:
    lines = [f"{result['verdict'].upper()}: RGB image validation"]
    for label in ("reference", "candidate"):
        image = result.get(label, {})
        if image.get("sha256"):
            lines.append(
                f"  {label}: {image['path']} ({image['dimensions'][0]}x{image['dimensions'][1]} "
                f"{image['mode']} {image['format']}) sha256 {image['sha256']}"
            )
        else:
            lines.append(f"  {label}: {image.get('path')} ({image.get('error', 'unreadable')})")
    comparison = result.get("comparison")
    if comparison:
        overall = comparison["overall"]
        per_channel = comparison["per_channel"]
        channel_text = " | ".join(
            f"{name} mae {stats['mae']:.3f} p99 {stats['p99']:.2f}"
            for name, stats in per_channel.items()
        )
        lines.append(
            f"  overall MAE {overall['mae']:.3f}, max {overall['max']}, p99 {overall['p99']:.2f}, "
            f"bad pixels {overall['bad_pixel_fraction']:.4f}"
        )
        lines.append(f"  channels: {channel_text}")
    regions = result.get("regions")
    if regions:
        worst = regions["worst"]
        grid_text = f"{regions['grid'][0]}x{regions['grid'][1]}"
        if worst is None or worst["mae"] is None:
            lines.append(f"  regions {grid_text}: no region could be measured")
        else:
            lines.append(
                f"  regions {grid_text}: worst column {worst['column']} row {worst['row']} "
                f"MAE {worst['mae']:.3f} (limit {regions['tolerance_max_region_mae']:.3f})"
            )
    stereo = result.get("stereo")
    if stereo:
        if stereo["applicable"]:
            lines.append(
                f"  stereo: left energy {stereo['left_energy']:.3f}, right energy "
                f"{stereo['right_energy']:.3f}, missing eye {stereo['missing_eye'] or 'none'}"
            )
        else:
            lines.append("  stereo: not applicable (image narrower than 2 px)")
    swap = result.get("channel_swap")
    if swap:
        lines.append(
            f"  channel swap: {'suspected' if swap['suspected'] else 'not suspected'} "
            f"(direct MAE {swap['normal_mae']:.3f}, swapped MAE {swap['swapped_mae']:.3f})"
        )
    provenance = result.get("provenance", {})
    if provenance.get("provided"):
        lines.append(
            f"  provenance: checked (ok {provenance['ok']})"
            + (f"; mismatches: {'; '.join(provenance['mismatches'])}"
               if provenance.get("mismatches") else "")
        )
    else:
        lines.append("  provenance: not provided")
    if result.get("reasons"):
        lines.append("  reasons:")
        for reason in result["reasons"]:
            lines.append(f"    - {reason}")
    lines.append(f"  verdict {result['verdict']} (exit {result['exit_code']})")
    return "\n".join(lines)


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    parser = _ArgumentParser(
        prog="image_validation.py",
        description="Colour (RGB) correctness validation for benchmark screenshots; separate from "
                    "the scene-identity gate.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command", required=True)
    compare_parser = sub.add_parser(
        "compare", help="compare a candidate image with the reviewed reference image"
    )
    compare_parser.add_argument("--reference", required=True, help="reviewed reference image")
    compare_parser.add_argument("--candidate", required=True, help="image under test")
    compare_parser.add_argument(
        "--tolerances", help="tolerance JSON (default: reference/image-tolerances.json values)"
    )
    compare_parser.add_argument(
        "--provenance",
        help="optional provenance JSON; mismatches fail the verdict and are never ignored",
    )
    compare_parser.add_argument("--json", action="store_true", help="print the result as JSON")

    try:
        args = parser.parse_args(argv)
    except UsageError as exc:
        print(f"usage error: {exc}", file=sys.stderr)
        return EXIT_INCONCLUSIVE

    try:
        tolerances = load_tolerances(args.tolerances) if args.tolerances else None
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_INCONCLUSIVE

    provenance = None
    if args.provenance:
        try:
            provenance = _read_json(Path(args.provenance), "provenance")
        except ValueError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return EXIT_INCONCLUSIVE

    result = compare(args.reference, args.candidate, tolerances, provenance=provenance)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(_format_text(result))
    return result["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
