#!/usr/bin/env python3
"""Human and JSON report for one benchmark run.

Rules enforced here:
  * Invalid runs never get a headline FPS; their reasons are printed instead.
  * Unique rendered FPS comes from the emulator's rendered-frame windows only.
  * Present/compositor cadence, when recorded, is shown separately and labelled informational.
  * Observed values and estimates are separate sections.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import validator


def build(run_dir: Path) -> dict:
    result = validator.validate_run(run_dir)
    report = {
        "run": str(run_dir),
        "run_id": result.get("run_id"),
        "valid": result["valid"],
        "validator_errors": result["errors"],
        "observed": None,
        "estimated": None,
        "informational": {},
        "limitations": [],
    }
    run_json = run_dir / "run.json"
    if not run_json.exists():
        report["limitations"].append("run.json is missing; nothing can be reported")
        return report
    data = json.loads(run_json.read_text(encoding="utf-8"))
    timing = data.get("timing", {})
    report["informational"]["compositor_cadence"] = timing.get("compositor_cadence")
    report["limitations"].append(
        "Frame times are means of the emulator's 0.5 s frame-stats windows, not individual "
        "frames; p95/p99 describe windows.")
    report["limitations"].append(
        "The classroom front end is not gameplay: menu selection, Move input and gameplay "
        "pacing were not exercised.")
    if not result["valid"]:
        return report
    window = [s for s in timing["per_sample"] if s["in_window"]]
    report["observed"] = {
        "unique_rendered_fps": timing["unique_fps"],
        "frame_ms": timing["frame_ms"],
        "window": {
            "samples": timing["samples_window"],
            "stats_every_s": data["scenario"]["stats_every_s"],
            "elapsed_s": timing["window_elapsed_s"],
            "source": timing["source_id"],
        },
        "scene": {
            "classification": data["scene"]["classification"],
            "overlay_present": data["scene"]["overlay_present"],
            "screenshot_similarity": data["scene"]["screenshot"]["signature_similarity"],
        },
        "content": {
            "draws_per_frame_mean": round(sum(s["draws"] for s in window) / len(window), 3),
            "mpx_per_frame_mean": round(sum(s["mpx"] for s in window) / len(window), 3),
            "gpu_busy_percent_mean": round(
                sum(s.get("gpu_busy", 0) for s in window) / len(window), 1),
        },
        "executable_sha256": data["executable"]["sha256"],
        "guest": data["scenario"]["guest"],
        "presentation": data["scenario"]["presentation"],
    }
    report["estimated"] = {
        "note": "Derived from window means; not an independent measurement.",
        "frames_in_window": round(timing["unique_fps"] * timing["window_elapsed_s"], 1),
    }
    return report


def print_text(report: dict) -> None:
    state = "VALID" if report["valid"] else "INVALID"
    print(f"RUN {report.get('run_id')}  [{state}]")
    if not report["valid"]:
        for error in report["validator_errors"]:
            print(f"  - {error}")
        print("  no FPS is reported for an invalid run")
        return
    observed = report["observed"]
    print(f"  unique rendered FPS: {observed['unique_rendered_fps']:.2f} "
          f"({observed['window']['samples']} x {observed['window']['stats_every_s']} s windows, "
          f"{observed['window']['elapsed_s']:.2f} s)")
    frame = observed["frame_ms"]
    print(f"  frame time p50/p90/p95/p99: {frame['p50']:.2f} / {frame['p90']:.2f} / "
          f"{frame['p95']:.2f} / {frame['p99']:.2f} ms (window means)")
    print(f"  scene: {observed['scene']['classification']}, overlay "
          f"{'present' if observed['scene']['overlay_present'] else 'absent'}, screenshot "
          f"similarity {observed['scene']['screenshot_similarity']:.4f}")
    print(f"  content: {observed['content']['draws_per_frame_mean']:.1f} draws, "
          f"{observed['content']['mpx_per_frame_mean']:.1f} Mpx, GPU "
          f"{observed['content']['gpu_busy_percent_mean']:.0f}% busy per frame")
    print(f"  executable: {observed['executable_sha256']}")
    cadence = report["informational"].get("compositor_cadence")
    if cadence:
        print(f"  informational only, not FPS: compositor/present cadence "
              f"{cadence.get('presented_hz')} Hz ({cadence.get('source')})")
    for limitation in report["limitations"]:
        print(f"  limit: {limitation}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--out")
    args = parser.parse_args(argv)
    report = build(Path(args.run))
    text = json.dumps(report, indent=1)
    if args.out:
        Path(args.out).write_text(text + "\n", encoding="utf-8")
    if args.json:
        print(text)
    else:
        print_text(report)
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
