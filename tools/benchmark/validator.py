#!/usr/bin/env python3
"""Deterministic validity validator for Déraciné classroom benchmark runs.

This is deliberately independent of the runner's own scoring: every number is recomputed from
the raw per-sample rows and from the files on disk, so a run cannot pass by carrying a
flattering `validation` block.

Exit status: 0 = valid, 1 = invalid, 2 = usage or unreadable run directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import subprocess
import sys
from pathlib import Path

import json_schema_lite
import scene_signature

VALIDATOR_VERSION = 1
SCENARIO_ID = "deracine-classroom-menu-300-msaa2-vblank60"
SCENE_CLASSIFICATION = "classroom-main-menu-overlay"
SIGNATURE_THRESHOLD = scene_signature.DEFAULT_THRESHOLD
PERCENTILE_TOLERANCE_MS = 0.1
FPS_TOLERANCE = 0.1

HERE = Path(__file__).resolve().parent
SCHEMA = HERE / "schema" / "run.schema.json"
REFERENCE = HERE / "reference" / "scene-classroom-menu.json"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def percentile(values: list[float], fraction: float) -> float:
    """Linear interpolation between closest ranks; the runner uses the same definition."""
    ordered = sorted(values)
    if not ordered:
        raise ValueError("percentile of an empty series")
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def aggregate_fps(samples: list[dict]) -> float:
    """Total frames / total measured time: the emulator's rendered-frame rate."""
    total_frames = sum(int(s["frames"]) for s in samples)
    total_time = sum(int(s["frames"]) / float(s["fps"]) for s in samples)
    return total_frames / total_time


class Checker:
    def __init__(self, run_dir: Path, data: dict):
        self.run_dir = run_dir
        self.data = data
        self.errors: list[str] = []

    def fail(self, message: str) -> None:
        self.errors.append(message)

    def path(self, value: str) -> Path:
        candidate = Path(value)
        return candidate if candidate.is_absolute() else self.run_dir / candidate

    def check_schema(self) -> None:
        self.errors.extend(json_schema_lite.validate_file(str(self.run_dir / "run.json"),
                                                          str(SCHEMA)))

    def check_source(self) -> None:
        source = self.data["source"]
        if source["dirty_count"] != len(source["dirty_files"]):
            self.fail("source.dirty_count does not match the dirty-file list")
        worktree = Path(source["worktree"])
        if not worktree.is_absolute() or not worktree.exists():
            self.fail(f"source.worktree is not an existing absolute path: {worktree}")
        if worktree.name == "DeracinePCVR":
            self.fail("source.worktree is the active runtime repository, not an isolated worktree")

    def check_executable(self) -> None:
        executable = self.data["executable"]
        inspected = Path(executable["path"])
        if not inspected.is_absolute() or not inspected.exists():
            self.fail(f"executable.path does not exist: {inspected}")
            return
        actual = sha256_file(inspected)
        if actual != executable["sha256"]:
            self.fail("executable.sha256 does not match the inspected file")
        if inspected.stat().st_size != executable["bytes"]:
            self.fail("executable.bytes does not match the inspected file")
        lab = Path(executable["lab_path"])
        if not lab.is_absolute() or not lab.exists():
            self.fail(f"executable.lab_path does not exist: {lab}")
            return
        lab_hash = sha256_file(lab)
        if lab_hash != executable["lab_sha256"]:
            self.fail("executable.lab_sha256 does not match the file the session ran")
        if lab_hash != actual:
            self.fail("the file the session ran is not the executable this run names")
        built_from = executable.get("built_from_head")
        if built_from is not None:
            if re.fullmatch(r"[0-9a-f]{40}", str(built_from)) is None:
                self.fail("executable.built_from_head is not a commit id")
            else:
                result = subprocess.run(
                    ["git", "-C", str(Path(self.data["source"]["worktree"])), "cat-file", "-e",
                     f"{built_from}^{{commit}}"], capture_output=True, text=True)
                if result.returncode != 0:
                    self.fail("executable.built_from_head is not a commit of this worktree")

    def check_host(self) -> None:
        host = self.data["host"]
        for field in ("gpu", "driver", "xr_runtime"):
            value = str(host[field]).strip()
            if not value or value.lower() in ("unknown", "n/a", "unidentified"):
                self.fail(f"host.{field} is not identified")
        if not host.get("headset"):
            self.fail("host.headset is not recorded (may be 'not worn', but must be stated)")

    def check_configuration(self) -> None:
        scenario = self.data["scenario"]
        guest = scenario["guest"]
        if float(guest["screen_percentage"]) != 300.0 or int(guest["msaa_requested"]) != 2:
            self.fail("guest configuration is not 300% / MSAA 2")
        stereo = f"{int(guest['stereo_width'])}x{int(guest['eye_height'])}"
        if int(guest["stereo_width"]) != 2 * int(guest["eye_width"]):
            self.fail("guest stereo width is not twice the eye width")
        recorded_stereo = guest.get("stereo_actual")
        if recorded_stereo is None:
            self.fail("the actual guest stereo size was not read back from the session log")
        elif recorded_stereo != stereo:
            self.fail(f"actual guest stereo {recorded_stereo} is not the requested {stereo}")
        presentation = scenario["presentation"]
        actual = presentation.get("actual")
        if not actual:
            self.fail("the actual OpenXR presentation size was not read back from the log")
        else:
            match = re.search(r"scale:\s*([0-9.]+)x", str(actual))
            if match is None:
                self.fail(f"actual presentation text has no scale: {actual!r}")
            elif abs(float(match.group(1)) - float(presentation["vr_scale"])) > 0.01:
                self.fail(f"actual presentation scale {match.group(1)} != requested "
                          f"{presentation['vr_scale']}")

    def check_env(self) -> None:
        env = self.data["env"]
        scenario = self.data["scenario"]
        if str(env.get("SHADPS4_FRAME_STATS")) != "1":
            self.fail("env.SHADPS4_FRAME_STATS is not 1: no unique-frame series")
        if str(env.get("SHADPS4_FRAME_STATS_EVERY")) != str(scenario["stats_every_s"]):
            self.fail("env.SHADPS4_FRAME_STATS_EVERY disagrees with the scenario")
        if env.get("SHADPS4_VR_SCALE") in (None, ""):
            self.fail("env.SHADPS4_VR_SCALE is not recorded")
        autopress = str(env.get("SHADPS4_DBG_AUTOPRESS") or "")
        if autopress.strip().lower() in ("", "0", "none"):
            self.fail("SHADPS4_DBG_AUTOPRESS is missing or off; the scene cannot be reached")
        if not scenario["autopress"]["enabled"]:
            self.fail("scenario.autopress.enabled is false")
        if scenario["vblank_hz"] != scenario.get("vblank_actual_hz", scenario["vblank_hz"]):
            self.fail("requested vblank does not match the value written to the emulator config")

    def check_timing(self) -> None:
        scenario = self.data["scenario"]
        timing = self.data["timing"]
        every = float(scenario["stats_every_s"])
        expected_window = round(float(scenario["window_s"]) / every)
        expected_warmup = round(float(scenario["warmup_s"]) / every)

        if timing["source"] != "framestats":
            self.fail("timing.source is not the unique rendered-frame series")
        if "xr" in timing["source_id"].lower() or "present" in timing["source_id"].lower():
            self.fail("timing.source_id names a present/compositor cadence")
        if timing["samples_window"] != expected_window:
            self.fail(f"timing.samples_window {timing['samples_window']} != expected "
                      f"{expected_window} for {scenario['window_s']} s at {every} s")
        if timing["warmup_samples"] < expected_warmup:
            self.fail(f"timing.warmup_samples {timing['warmup_samples']} < expected "
                      f"{expected_warmup}: the scene is warmed up for less than "
                      f"{scenario['warmup_s']} s")
        if float(timing["window_start_s"]) - float(self.data["scene"]["anchor_at_s"]) < \
                float(scenario["warmup_s"]) - 1.0:
            self.fail("the measured window starts less than the warmup time after the scene anchor")
        elapsed = float(timing["window_elapsed_s"])
        if elapsed < 1.0:
            self.fail(f"the measured window lasted {elapsed:.3f} s, not {scenario['window_s']} s")
        # Each of the 80 windows closes at the next frame boundary and any hitch inside a window
        # is part of that window, so the measured span is 40 s plus quantization and stalls. The
        # gate rejects a short or wildly long window; the negative case is a claimed 40 s that
        # really lasted 0.65 s, which this bound still fails.
        allowed_upper = float(scenario["window_s"]) * 1.2 + 0.5
        if elapsed < float(scenario["window_s"]) - 1.0 or elapsed > allowed_upper:
            self.fail(f"measured window {elapsed:.3f} s is not {scenario['window_s']} s "
                      f"(allowed {float(scenario['window_s']) - 1.0:.1f}-{allowed_upper:.1f} s)")
        if not (timing["window_start_s"] < timing["window_end_s"]):
            self.fail("window boundaries are not increasing")
        if float(timing["warmup_end_s"]) > float(timing["window_start_s"]) + 1e-9:
            self.fail("the measurement window overlaps the warmup")
        if not (timing["warmup_start_s"] < timing["warmup_end_s"]):
            self.fail("warmup boundaries are not increasing")

        samples = timing["per_sample"]
        if len(samples) != timing["samples_total"]:
            self.fail("timing.per_sample does not have samples_total rows")
        indices = [int(s["sample"]) for s in samples]
        if indices != list(range(len(samples))):
            self.fail("per-sample indices are missing or out of order")
        ats = [float(s["at_s"]) for s in samples]
        if any(b < a for a, b in zip(ats, ats[1:])):
            self.fail("per-sample timestamps are nonmonotonic")
        for sample in samples:
            if float(sample["fps"]) <= 0.0:
                self.fail(f"sample {sample['sample']} has a non-positive fps")
            expected_ms = 1000.0 / float(sample["fps"])
            if abs(float(sample["frame_ms"]) - expected_ms) > 1e-6 * expected_ms + 0.01:
                self.fail(f"sample {sample['sample']} frame_ms disagrees with its fps")
        window = [s for s in samples if s["in_window"]]
        if len(window) != timing["samples_window"]:
            self.fail(f"{len(window)} in-window samples != {timing['samples_window']}")
        if samples and window and (samples[-len(window):] != window):
            self.fail("the in-window samples are not the tail of the series")
        if len(window) != len(samples) - timing["warmup_samples"]:
            self.fail("warmup_samples does not separate warmup from the measured window")
        if window:
            frame_ms = [float(s["frame_ms"]) for s in window]
            recomputed_stats = {
                "p50": percentile(frame_ms, 0.50),
                "p90": percentile(frame_ms, 0.90),
                "p95": percentile(frame_ms, 0.95),
                "p99": percentile(frame_ms, 0.99),
                "mean": sum(frame_ms) / len(frame_ms),
                "min": min(frame_ms),
                "max": max(frame_ms),
            }
            for key, value in recomputed_stats.items():
                stored = float(timing["frame_ms"][key])
                if abs(stored - value) > PERCENTILE_TOLERANCE_MS:
                    self.fail(f"timing.frame_ms.{key} {stored} != recomputed {value:.3f}")
            if not (recomputed_stats["min"] <= timing["frame_ms"]["p50"] <=
                    timing["frame_ms"]["p95"] <= timing["frame_ms"]["p99"] <=
                    timing["frame_ms"]["max"]):
                self.fail("frame-time percentiles are not ordered")
            first, last = window[0], window[-1]
            if abs(float(first["at_s"]) - float(timing["window_start_s"])) > 2.0:
                self.fail("window_start_s does not match the first measured sample")
            if abs(float(last["at_s"]) - float(timing["window_end_s"])) > 2.0:
                self.fail("window_end_s does not match the last measured sample")

            frames_available = all(isinstance(s.get("frames"), int) and int(s["frames"]) > 0
                                   for s in window)
            if not frames_available:
                self.fail("no per-window frame count: unique FPS and elapsed time cannot be "
                          "verified from the emulator's own series")
            else:
                measured = sum(int(s["frames"]) / float(s["fps"]) for s in window)
                if abs(measured - elapsed) > 0.5:
                    self.fail(f"window_elapsed_s {elapsed:.3f} != the emulator-measured "
                              f"{measured:.3f} s from per-window frames/fps")
                aggregate = aggregate_fps(window)
                if abs(aggregate - float(timing["unique_fps"])) > FPS_TOLERANCE:
                    self.fail(f"unique_fps {timing['unique_fps']} != total frames / total "
                              f"measured time {aggregate:.3f}")

        cadence = timing.get("compositor_cadence")
        if cadence:
            if cadence.get("role") != "informational" or cadence.get("is_unique_render_fps") is not False:
                self.fail("compositor cadence is presented as a frame-rate source")

    def check_scene(self) -> None:
        scenario = self.data["scenario"]
        scene = self.data["scene"]
        gate = scenario["scene_gate"]
        if scene["classification"] != SCENE_CLASSIFICATION:
            self.fail(f"scene.classification {scene['classification']!r} is not the classroom")
        if not scene["overlay_present"]:
            self.fail("the main-menu overlay is not reported present")
        if scene.get("gameplay_reached"):
            self.fail("the run claims gameplay; this scenario is the classroom front end")
        if float(scene["draws"]) < float(gate["min_draws"]):
            self.fail(f"anchor draws {scene['draws']} < gate {gate['min_draws']}")
        if float(scene["mpx"]) < float(gate["min_mpx"]):
            self.fail(f"anchor Mpx {scene['mpx']} < gate {gate['min_mpx']}")

        anchor = next((s for s in self.data["timing"]["per_sample"]
                       if int(s["sample"]) == int(scene["anchor_sample"])), None)
        if anchor is None:
            self.fail("scene.anchor_sample does not name a per-sample row")
        else:
            if abs(float(anchor["draws"]) - float(scene["draws"])) > 0.1 or \
                    abs(float(anchor["mpx"]) - float(scene["mpx"])) > 0.1:
                self.fail("scene anchor statistics disagree with the sample they name")
        # The measured window itself must stay in the heavy scene: an anchor match plus a
        # screenshot is not proof if the title drops to the light front end mid-window. The
        # overlay is animated and the title briefly dips, so the gate is a supermajority of the
        # window plus a hard floor; a drop to the ~38-draw front end fails both.
        in_window = [s for s in self.data["timing"]["per_sample"] if s["in_window"]]
        passing = sum(1 for s in in_window
                      if float(s["draws"]) >= float(gate["min_draws"]) and
                      float(s["mpx"]) >= float(gate["min_mpx"]))
        if in_window and passing < int(0.90 * len(in_window)):
            self.fail(f"only {passing}/{len(in_window)} in-window samples are in the scene gate")
        for sample in in_window:
            if float(sample["draws"]) < 250.0 or float(sample["mpx"]) < 250.0:
                self.fail(f"in-window sample {sample['sample']} dropped out of the heavy scene "
                          f"({sample['draws']:.0f} draws, {sample['mpx']:.1f} Mpx)")

        shot = scene["screenshot"]
        image_path = self.path(shot["path"])
        if not image_path.is_file():
            self.fail(f"scene screenshot is missing: {image_path}")
            return
        if sha256_file(image_path) != shot["sha256"]:
            self.fail("scene screenshot sha256 does not match the file")
        try:
            width, height = scene_signature.image_size(str(image_path))
        except Exception as error:  # noqa: BLE001 - any unreadable image invalidates the run
            self.fail(f"scene screenshot is unreadable: {error}")
            return
        if (width, height) != (int(shot["width"]), int(shot["height"])):
            self.fail(f"scene screenshot is {width}x{height}, metadata says "
                      f"{shot['width']}x{shot['height']}")
        try:
            with open(REFERENCE, encoding="utf-8") as handle:
                reference = json.load(handle)
            score = scene_signature.similarity(scene_signature.signature(str(image_path)),
                                               reference["signature"])
        except Exception as error:  # noqa: BLE001
            self.fail(f"scene screenshot cannot be compared with the reference: {error}")
            return
        if score < SIGNATURE_THRESHOLD:
            self.fail(f"scene screenshot does not match the reviewed classroom reference "
                      f"(similarity {score:.3f} < {SIGNATURE_THRESHOLD})")
        if abs(float(shot["signature_similarity"]) - score) > 0.01:
            self.fail("reported screenshot similarity disagrees with the independent comparison")

    def check_cleanup(self) -> None:
        cleanup = self.data.get("cleanup")
        if not cleanup:
            return
        root = Path(cleanup["root"])
        if not root.is_absolute():
            self.fail("cleanup.root is not absolute")
            return
        resolved_root = root.resolve()
        private = [self.run_dir.resolve(), Path(self.data["source"]["worktree"]).resolve()]
        if not any(resolved_root == p or resolved_root.is_relative_to(p) for p in private):
            self.fail(f"cleanup.root is not inside the private run/worktree root: {root}")
        if not cleanup["paths"]:
            self.fail("cleanup.paths is empty while cleanup.root was recorded")
        for entry in cleanup["paths"]:
            if not isinstance(entry, str) or not entry.strip():
                self.fail("cleanup contains an empty (path-less) destination")
                continue
            candidate = Path(entry)
            if not candidate.is_absolute():
                self.fail(f"cleanup path is relative: {entry}")
                continue
            if any(part == ".." for part in candidate.parts):
                self.fail(f"cleanup path contains '..': {entry}")
                continue
            resolved = candidate.resolve()
            if not (resolved == resolved_root or resolved.is_relative_to(resolved_root)):
                self.fail(f"cleanup path escapes its recorded root: {entry}")

    def check_provenance(self) -> None:
        provenance = self.data["provenance"]
        manifest = provenance["preflight_manifest"]
        if not manifest:
            self.fail("no preflight manifest was recorded")
        else:
            manifest_path = self.path(manifest)
            if not manifest_path.exists():
                self.fail(f"preflight manifest is missing: {manifest_path}")
        if provenance["protected_unchanged"] is not True:
            self.fail("protected paths changed during the run")
        if provenance.get("protected_drift"):
            self.fail("protected drift is recorded but the run still claims validity")
    def check_artifacts(self) -> None:
        artifacts = self.data["artifacts"]
        log = self.path(artifacts["log"])
        if not log.exists():
            self.fail(f"session log is missing: {log}")
        elif sha256_file(log) != artifacts["log_sha256"]:
            self.fail("session log sha256 does not match the file")
        frames = self.path(artifacts["frames_csv"])
        if not frames.exists():
            self.fail(f"frames CSV is missing: {frames}")
        self.check_renderdoc(artifacts)

    def check_renderdoc(self, artifacts: dict) -> None:
        rdc = artifacts.get("rdc")
        if not rdc:
            return
        capture = self.path(rdc)
        if not capture.is_file():
            self.fail(f"RenderDoc capture is missing: {capture}")
            return
        declared = artifacts.get("rdc_sha256")
        if declared and sha256_file(capture) != declared:
            self.fail("RenderDoc capture sha256 does not match the file")
        png = artifacts.get("rdc_png")
        if not png:
            self.fail("a RenderDoc capture is recorded without an extracted image")
            return
        image = self.path(png)
        if not image.is_file():
            self.fail(f"RenderDoc extracted image is missing: {image}")
            return
        try:
            with open(REFERENCE, encoding="utf-8") as handle:
                reference = json.load(handle)
            score = scene_signature.similarity(scene_signature.signature(str(image)),
                                               reference["signature"])
        except Exception as error:  # noqa: BLE001
            self.fail(f"RenderDoc extracted image is unreadable: {error}")
            return
        if score < SIGNATURE_THRESHOLD:
            self.fail(f"RenderDoc final image does not show the classroom "
                      f"(similarity {score:.3f})")
        declared_score = artifacts.get("rdc_png_similarity")
        if declared_score is not None and abs(float(declared_score) - score) > 0.01:
            self.fail("reported RenderDoc similarity disagrees with the independent comparison")

    def check_ids(self) -> None:
        if self.data["run_id"] != self.run_dir.name:
            self.fail(f"run_id {self.data['run_id']!r} does not match the directory name "
                      f"{self.run_dir.name!r}")

    def run(self) -> dict:
        if (self.run_dir / "INVALID.txt").exists():
            self.errors.append("INVALID.txt is present: this directory records a failed run")
        self.check_schema()
        checks = [self.check_source, self.check_executable, self.check_host,
                  self.check_configuration, self.check_env, self.check_timing, self.check_scene,
                  self.check_cleanup, self.check_provenance, self.check_artifacts, self.check_ids]
        for check in checks:
            try:
                check()
            except Exception as error:  # noqa: BLE001 - a malformed run must fail, not crash
                self.fail(f"{check.__name__}: malformed run data ({error!r})")
        scenario_id = None
        if isinstance(self.data.get("scenario"), dict):
            scenario_id = self.data["scenario"].get("id")
        return {
            "run": str(self.run_dir),
            "run_id": self.data.get("run_id"),
            "scenario": scenario_id,
            "valid": not self.errors,
            "validator_version": VALIDATOR_VERSION,
            "errors": self.errors,
        }


def load_run(run_dir: Path) -> tuple[dict | None, str | None]:
    path = run_dir / "run.json"
    if not path.exists():
        return None, f"run.json is missing in {run_dir}"
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle), None
    except json.JSONDecodeError as error:
        return None, f"run.json is not valid JSON: {error}"


def validate_run(run_dir: Path) -> dict:
    data, error = load_run(run_dir)
    if data is None:
        return {"run": str(run_dir), "valid": False, "errors": [error],
                "validator_version": VALIDATOR_VERSION}
    return Checker(run_dir, data).run()


def print_text(result: dict) -> None:
    state = "VALID" if result["valid"] else "INVALID"
    print(f"{state}  {result.get('run_id') or result['run']}")
    run_json = Path(result["run"]) / "run.json"
    if result["valid"] and run_json.exists():
        data = json.loads(run_json.read_text(encoding="utf-8"))
        timing = data["timing"]
        print(f"  unique rendered FPS: {timing['unique_fps']:.2f} "
              f"({timing['samples_window']} x {data['scenario']['stats_every_s']} s windows, "
              f"{timing['window_elapsed_s']:.2f} s)")
        print(f"  frame time p50/p95/p99: {timing['frame_ms']['p50']:.2f} / "
              f"{timing['frame_ms']['p95']:.2f} / {timing['frame_ms']['p99']:.2f} ms "
              f"(window means)")
        print(f"  scene: {data['scene']['classification']} "
              f"(similarity {data['scene']['screenshot']['signature_similarity']:.4f})")
    for reason in result["errors"]:
        print(f"  - {reason}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["run", "all"])
    parser.add_argument("target", help="run directory, or a directory of run directories")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    if args.command == "run":
        result = validate_run(Path(args.target))
        if args.json:
            print(json.dumps(result, indent=1))
        else:
            print_text(result)
        return 0 if result["valid"] else 1

    root = Path(args.target)
    if not root.is_dir():
        print(f"not a directory: {root}", file=sys.stderr)
        return 2
    results = [validate_run(child) for child in sorted(root.iterdir()) if child.is_dir()]
    if args.json:
        print(json.dumps(results, indent=1))
    else:
        for result in results:
            print_text(result)
    return 0 if results and all(r["valid"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
