# Headless gameplay scene gate and unique-FPS validator.
#
# Pure functions over the emulator's frame-stats lines: nothing here starts an emulator or touches
# a GPU. The measured signature and the band come from the M2 validation record
# (_build/headless/validation/results/RESULTS.md, 2026-11-18) and the user's handover:
#
#   menu / front end :  465-500 draws, ~480 Mpx, ~65 passes   (~58-60 fps, paced)
#   loading dip      :   42-  55 draws, low
#   gameplay, candidate renderer : 1866-1900 draws, 550-720 Mpx, 38-46 passes (30-35 fps)
#   gameplay, unified renderer   : 1866 draws, 860 Mpx, 55.7 passes (35.7 fps)
#
# Mpx and passes depend on the renderer state at the same scene, so the band bounds draws
# tightly (1500-2300) and Mpx/passes loosely (500-1000, 25-80). Re-derive the band only from a
# live cell, never from an estimate.
#
# The gate (draws >= 800) cannot be met by the menu; the band catches a window that
# is not actually gameplay even though it passed the gate. unique_fps is always
# total frames / total measured time, never a mean of per-window rates.
#
# Exit codes: 0 valid, 2 invalid, 3 no usable data / usage error.
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from pathlib import Path

# Same line the runner/validator parse; frames is required for a unique-FPS series.
FRAME_STATS_RE = re.compile(
    r"frame stats at\s+(?P<at>\d+)\s+s(?:\s+\[(?P<ab>[A-Z+]+)\])?:\s+"
    r"(?P<fps>[0-9.]+)\s+fps,\s+per frame\s+(?P<passes>[0-9.]+)\s+passes\s+"
    r"\((?P<loading>[0-9.]+)\s+loading their targets,\s+(?P<mpx>[0-9.]+)\s+Mpx\),\s+"
    r"(?P<draws>[0-9.]+)\s+draws,\s+(?P<dispatches>[0-9.]+)\s+dispatches,\s+"
    r"(?P<submits>[0-9.]+)\s+submits"
    r"(?:,\s+GPU busy\s+(?P<busy>[0-9]+)%(?:\s+at\s+(?P<mhz>[0-9]+)\s+MHz)?)?"
    r"(?:,\s+(?P<frames>[0-9]+)\s+frames)?"
)


def parse_sample(line: str) -> dict | None:
    """One frame-stats line -> a sample, or None. A line without a frame count is not a sample."""
    match = FRAME_STATS_RE.search(line)
    if match is None or match.group("frames") is None:
        return None
    return {
        "at_s": float(match.group("at")),
        "fps": float(match.group("fps")),
        "frames": int(match.group("frames")),
        "passes": float(match.group("passes")),
        "loading": float(match.group("loading")),
        "mpx": float(match.group("mpx")),
        "draws": float(match.group("draws")),
        "dispatches": float(match.group("dispatches")),
        "submits": float(match.group("submits")),
        "gpu_busy": float(match.group("busy")) if match.group("busy") else -1.0,
    }


def parse_log(text: str) -> tuple[list[dict], int]:
    """All usable samples and the count of truncated stats lines."""
    samples: list[dict] = []
    incomplete = 0
    for line in text.splitlines():
        sample = parse_sample(line)
        if sample is not None:
            samples.append(sample)
        elif "frame stats at" in line:
            incomplete += 1
    return samples, incomplete


def aggregate_fps(samples: list[dict]) -> float:
    """Total frames / total measured time, exactly as tools/benchmark/validator.py defines it."""
    total_frames = sum(int(s["frames"]) for s in samples)
    total_time = sum(int(s["frames"]) / float(s["fps"]) for s in samples)
    return total_frames / total_time if total_time > 0 else 0.0


def in_band(sample: dict, band: dict) -> bool:
    return (
        band["min_draws"] <= sample["draws"] <= band["max_draws"]
        and band["min_mpx"] <= sample["mpx"] <= band["max_mpx"]
        and band["min_passes"] <= sample["passes"] <= band["max_passes"]
    )


def select_anchor(samples: list[dict], gate: dict, band: dict) -> int:
    """First index of `consecutive_samples` consecutive samples inside the gameplay band.

    The band, not just the coarse gate, selects the anchor: a loading-to-gameplay transition can
    pass the 800-draw gate with a huge target area and pass count (a live unified run measured
    2208 draws / 5676 Mpx / 308 passes at 64 s) and must not become the anchor of an otherwise
    valid gameplay window. The gate still has to pass as well.
    """
    run = 0
    for index, sample in enumerate(samples):
        if (
            sample["draws"] >= gate["min_draws"]
            and sample["mpx"] >= gate["min_mpx"]
            and sample["fps"] > 1.0
            and in_band(sample, band)
        ):
            run += 1
            if run >= gate["consecutive_samples"]:
                return index - gate["consecutive_samples"] + 1
        else:
            run = 0
    return -1


def evaluate(samples: list[dict], profile: dict) -> dict:
    """The gameplay verdict. `profile` is reference/scene-gameplay.json."""
    gate = profile["scene_gate"]
    band = profile["band"]
    reasons: list[str] = []

    if not samples:
        return {"valid": False, "phase": "unknown", "reasons": ["no frame-stats samples"]}

    anchor = select_anchor(samples, gate, band)
    if anchor < 0:
        # Distinguish "the menu was reached" from "nothing heavy was reached": both invalid,
        # but different follow-ups. The menu's own upper bound is in the profile.
        menu_max = float(profile["menu_disambiguation"]["max_draws"])
        if any(s["draws"] <= menu_max and s["draws"] >= 300 for s in samples):
            return {
                "valid": False,
                "phase": "menu",
                "reasons": [
                    f"the run stayed in the menu/front end: no window reached "
                    f"{gate['min_draws']} draws (peak {max(s['draws'] for s in samples):.0f})"
                ],
            }
        return {
            "valid": False,
            "phase": "none",
            "reasons": [
                f"no {gate['consecutive_samples']} consecutive windows were inside the gameplay "
                f"band (draws {band['min_draws']}-{band['max_draws']}, "
                f"Mpx {band['min_mpx']}-{band['max_mpx']}, "
                f"passes {band['min_passes']}-{band['max_passes']})"
            ],
        }

    window = samples[anchor:]
    in_band_count = sum(1 for s in window if in_band(s, band))
    ratio = in_band_count / len(window) if window else 0.0
    if not in_band(window[0], band):
        reasons.append(
            f"anchor window {window[0]['at_s']:.0f} s is outside the gameplay band "
            f"({window[0]['draws']:.0f} draws, {window[0]['mpx']:.0f} Mpx, "
            f"{window[0]['passes']:.1f} passes)"
        )
    if ratio < float(band.get("min_in_band_ratio", 0.9)):
        reasons.append(
            f"only {in_band_count}/{len(window)} measured windows are inside the gameplay band"
        )
    if len(window) < int(gate.get("min_window_samples", 5)):
        reasons.append(
            f"the measured gameplay window has {len(window)} sample(s); "
            f"at least {gate.get('min_window_samples', 5)} are needed"
        )
    unique_fps = aggregate_fps(window)
    if unique_fps <= 1.0:
        reasons.append("unique FPS is not meaningful")

    return {
        "valid": not reasons,
        "phase": "gameplay",
        "anchor_at_s": window[0]["at_s"],
        "anchor_index": anchor,
        "window_samples": len(window),
        "in_band_samples": in_band_count,
        "in_band_ratio": round(ratio, 4),
        "unique_fps": round(unique_fps, 3),
        "draws_mean": round(sum(s["draws"] for s in window) / len(window), 2),
        "draws_min": round(min(s["draws"] for s in window), 2),
        "draws_max": round(max(s["draws"] for s in window), 2),
        "mpx_mean": round(sum(s["mpx"] for s in window) / len(window), 2),
        "mpx_min": round(min(s["mpx"] for s in window), 2),
        "mpx_max": round(max(s["mpx"] for s in window), 2),
        "passes_mean": round(sum(s["passes"] for s in window) / len(window), 2),
        "gpu_busy_mean": round(sum(s["gpu_busy"] for s in window) / len(window), 2)
        if window[0]["gpu_busy"] >= 0
        else None,
        "reasons": reasons,
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_save(save_dir: Path, manifest: dict) -> list[str]:
    """Errors for a save directory that is not the pinned M2 slot 1 save."""
    errors: list[str] = []
    for name, expected in manifest["files"].items():
        path = save_dir / name
        if not path.is_file():
            errors.append(f"missing save file: {name}")
            continue
        actual = sha256_file(path)
        if actual.upper() != expected.upper():
            errors.append(f"save file {name} has sha256 {actual}, expected {expected}")
    return errors


def load_profile(path: Path) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", help="shad_log.txt of a headless gameplay run")
    parser.add_argument("--profile", required=True, help="reference/scene-gameplay.json")
    parser.add_argument("--save-dir", help="savedata directory to verify against the profile manifest")
    parser.add_argument("--save-manifest", help="save-slot MANIFEST.json")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    profile = load_profile(Path(args.profile))
    samples, incomplete = parse_log(Path(args.log).read_text(encoding="utf-8", errors="replace"))
    verdict = evaluate(samples, profile)
    verdict["incomplete_stats_lines"] = incomplete

    if args.save_dir:
        manifest = load_profile(Path(args.save_manifest or profile["save"]["manifest"]))
        save_errors = verify_save(Path(args.save_dir), manifest)
        verdict["save_ok"] = not save_errors
        verdict["save_errors"] = save_errors
        if save_errors:
            verdict["valid"] = False
            verdict["reasons"] = list(verdict.get("reasons", [])) + save_errors

    if args.json:
        print(json.dumps(verdict, indent=2))
    else:
        state = "VALID" if verdict["valid"] else "INVALID"
        print(f"{state}: phase={verdict['phase']}")
        if "unique_fps" in verdict:
            print(
                f"  unique rendered FPS {verdict['unique_fps']:.2f} over "
                f"{verdict['window_samples']} windows ({verdict['in_band_samples']} in band)"
            )
            print(
                f"  draws {verdict['draws_min']:.0f}-{verdict['draws_max']:.0f}, "
                f"Mpx {verdict['mpx_min']:.0f}-{verdict['mpx_max']:.0f}, "
                f"passes {verdict['passes_mean']:.1f}"
            )
        for reason in verdict.get("reasons", []):
            print(f"  - {reason}")

    if verdict["valid"]:
        return 0
    return 2 if samples else 3


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
