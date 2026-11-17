#!/usr/bin/env python3
"""Conservative A/B comparison of two validated benchmark runs.

Refuses to compare runs that are not valid, not the same workload/configuration, or whose
content drifted too far. A difference inside the measured same-binary noise floor is reported as
inconclusive; nothing here promotes a change.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import validator

CONTENT_DRIFT_LIMIT = 0.10


def means(data: dict) -> dict:
    window = [s for s in data["timing"]["per_sample"] if s["in_window"]]
    return {
        "draws": sum(s["draws"] for s in window) / len(window),
        "mpx": sum(s["mpx"] for s in window) / len(window),
    }


def workload_signature(data: dict) -> dict:
    scenario = data["scenario"]
    return {
        "id": scenario["id"],
        "vblank_hz": scenario["vblank_hz"],
        "vblank_actual_hz": scenario.get("vblank_actual_hz", scenario["vblank_hz"]),
        "guest": scenario["guest"],
        "presentation": scenario["presentation"],
        "autopress": scenario["autopress"]["value"],
        "autopress_devices": scenario["autopress"]["devices"],
        "warmup_s": scenario["warmup_s"],
        "window_s": scenario["window_s"],
        "stats_every_s": scenario["stats_every_s"],
        "scene_gate": scenario["scene_gate"],
        "vr_scale": data["env"].get("SHADPS4_VR_SCALE"),
    }


def compare(run_a: Path, run_b: Path, noise_floor_pct: float) -> tuple[dict, str | None]:
    result_a = validator.validate_run(run_a)
    result_b = validator.validate_run(run_b)
    if not result_a["valid"] or not result_b["valid"]:
        return {
            "comparable": False,
            "reason": "one or both runs are invalid",
            "a_valid": result_a["valid"],
            "b_valid": result_b["valid"],
            "a_errors": result_a["errors"],
            "b_errors": result_b["errors"],
        }, "invalid run"

    data_a = json.loads((run_a / "run.json").read_text(encoding="utf-8"))
    data_b = json.loads((run_b / "run.json").read_text(encoding="utf-8"))

    signature_a = workload_signature(data_a)
    signature_b = workload_signature(data_b)
    if signature_a != signature_b:
        differing = [key for key in signature_a
                     if signature_a[key] != signature_b.get(key)]
        return {
            "comparable": False,
            "reason": f"workload/configuration differs: {differing}",
            "a": signature_a,
            "b": signature_b,
        }, "configuration mismatch"

    content_a = means(data_a)
    content_b = means(data_b)
    drift = {
        "draws_pct": abs(content_b["draws"] - content_a["draws"]) / content_a["draws"] * 100.0,
        "mpx_pct": abs(content_b["mpx"] - content_a["mpx"]) / content_a["mpx"] * 100.0,
    }
    if drift["draws_pct"] > CONTENT_DRIFT_LIMIT * 100 or drift["mpx_pct"] > CONTENT_DRIFT_LIMIT * 100:
        return {
            "comparable": False,
            "reason": f"scene content drifted beyond {CONTENT_DRIFT_LIMIT:.0%}",
            "content_drift_pct": drift,
        }, "content drift"

    fps_a = float(data_a["timing"]["unique_fps"])
    fps_b = float(data_b["timing"]["unique_fps"])
    p50_a = float(data_a["timing"]["frame_ms"]["p50"])
    p50_b = float(data_b["timing"]["frame_ms"]["p50"])
    delta_fps_pct = (fps_b - fps_a) / fps_a * 100.0
    delta_fps = fps_b - fps_a
    inside_noise = abs(delta_fps_pct) < noise_floor_pct

    if inside_noise:
        conclusion = (f"inconclusive: {delta_fps_pct:+.2f}% is inside the {noise_floor_pct:.1f}% "
                      f"same-binary noise floor")
    else:
        conclusion = (f"outside the {noise_floor_pct:.1f}% noise floor "
                      f"({delta_fps_pct:+.2f}%); needs repeated interleaved cells before any "
                      f"claim")
    return {
        "comparable": True,
        "a": {"run": str(run_a), "unique_fps": fps_a, "p50_ms": p50_a,
              "p95_ms": data_a["timing"]["frame_ms"]["p95"], "content": content_a},
        "b": {"run": str(run_b), "unique_fps": fps_b, "p50_ms": p50_b,
              "p95_ms": data_b["timing"]["frame_ms"]["p95"], "content": content_b},
        "delta": {"fps": round(delta_fps, 3), "fps_pct": round(delta_fps_pct, 3),
                  "p50_ms": round(p50_b - p50_a, 3)},
        "content_drift_pct": {"draws": round(drift["draws_pct"], 3),
                              "mpx": round(drift["mpx_pct"], 3)},
        "noise_floor_pct": noise_floor_pct,
        "inside_noise_floor": inside_noise,
        "conclusion": conclusion,
        "promotion": "none; comparison only",
    }, None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--noise-floor-pct", type=float, required=True,
                        help="same-binary noise floor measured from replicate runs")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    result, reason = compare(Path(args.a), Path(args.b), args.noise_floor_pct)
    if args.json:
        print(json.dumps(result, indent=1))
    else:
        if reason:
            print(f"REFUSED: {result['reason']}")
            if "a_errors" in result:
                for error in result["a_errors"]:
                    print(f"  A: {error}")
                for error in result["b_errors"]:
                    print(f"  B: {error}")
        else:
            print(f"A {result['a']['unique_fps']:.2f} fps -> "
                  f"B {result['b']['unique_fps']:.2f} fps "
                  f"({result['delta']['fps_pct']:+.2f}%)")
            print(f"content drift: draws {result['content_drift_pct']['draws']:.2f}%, "
                  f"Mpx {result['content_drift_pct']['mpx']:.2f}%")
            print(f"conclusion: {result['conclusion']}")
            print("promotion: none")
    return 0 if reason is None else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
