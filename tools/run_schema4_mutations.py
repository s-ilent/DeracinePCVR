#!/usr/bin/env python3
"""Mutation matrix for the schema-4 effective-shader fixture, as a regression gate.

Every case carries machine-checked expectations. The harness exits nonzero if any assertion fails,
so a future regression that produced wrong draw arguments, an unchanged image after a shader
mutation, or validation errors cannot report a successful run.

The fixture is synthetic. Every case is labelled that way, and none of it is original-game
evidence.

Usage: python run_schema4_mutations.py <replay-exe> <fixture-dir> <work-dir> [--self-test]
"""

import json
import os
import re
import shutil
import struct
import subprocess
import sys

DRAW_RE = re.compile(
    r"draw_from_tape: index_count=(\d+) first_index=(\d+) vertex_offset=(-?\d+) "
    r"instance_count=(\d+) first_instance=(\d+)")
IMAGE_RE = re.compile(r"image: hash=(\d+) distinct=(\d+) covered=(\d+)")
REF_RE = re.compile(r"reference_status: (\w+)")
EXACT_RE = re.compile(r"exact_match: (\w+)")
VALIDATION_RE = re.compile(r"validation: errors=(\d+)")
STAGE_RE = re.compile(r"stage: logical=(\d+) program_hash=0x([0-9a-f]+) perm_idx=(\d+)")
EXEC_RE = re.compile(r"execution_state: (\w+)")
SUBST_RE = re.compile(r"pipeline_state_substituted: (.*)")
PROBLEM_RE = re.compile(r"effective shader resolution failed: (\w+)")
BOUNDS_RE = re.compile(r"draw is out of bounds")
SIDECAR_RE = re.compile(r"hashes to|bytes but the package declares|cannot open sidecar")


def run(replay, directory):
    args = [replay, directory, "--shader-mode", "effective", "--synthetic-fixture",
            "--substitute-render-targets"]
    result = subprocess.run(args, capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def parse(output):
    draw = DRAW_RE.search(output)
    images = IMAGE_RE.findall(output)
    ref = REF_RE.search(output)
    exact = EXACT_RE.search(output)
    validation = VALIDATION_RE.search(output)
    stages = STAGE_RE.findall(output)
    exec_state = EXEC_RE.search(output)
    subst = SUBST_RE.search(output)
    problem = PROBLEM_RE.search(output)
    return {
        "index_count": int(draw.group(1)) if draw else None,
        "first_index": int(draw.group(2)) if draw else None,
        "vertex_offset": int(draw.group(3)) if draw else None,
        "instance_count": int(draw.group(4)) if draw else None,
        "first_instance": int(draw.group(5)) if draw else None,
        "hash": int(images[0][0]) if images else None,
        "covered": int(images[0][2]) if images else None,
        "reference": ref.group(1) if ref else None,
        "exact": exact.group(1) if exact else None,
        "validation": int(validation.group(1)) if validation else None,
        "stages": [(int(a), b, int(c)) for a, b, c in stages],
        "execution_state": exec_state.group(1) if exec_state else None,
        "substituted": subst.group(1).strip() if subst else None,
        "problem": problem.group(1) if problem else None,
        "bounds_refused": bool(BOUNDS_RE.search(output)),
        "sidecar_refused": bool(SIDECAR_RE.search(output)),
    }


def prepare(work, base, mutate_package=None, mutate_tape=None, mutate_blob=None):
    if os.path.exists(work):
        shutil.rmtree(work)
    shutil.copytree(base, work)
    if mutate_package is not None:
        path = os.path.join(work, "package.json")
        with open(path, encoding="utf-8") as handle:
            package = json.load(handle)
        mutate_package(package)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(package, handle, indent=1)
    if mutate_tape is not None:
        path = os.path.join(work, "tape.json")
        with open(path, encoding="utf-8") as handle:
            tape = json.load(handle)
        mutate_tape(tape)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(tape, handle, indent=1)
    if mutate_blob is not None:
        path = os.path.join(work, "resources.bin")
        with open(path, "rb") as handle:
            blob = bytearray(handle.read())
        mutate_blob(blob)
        with open(path, "wb") as handle:
            handle.write(bytes(blob))
    return work


class Gate:
    """Collects assertions and decides the exit code."""

    def __init__(self):
        self.case_results = []
        self.failure_reasons = []
        self.validation_errors = 0

    def case(self, name, code, parsed, expectations):
        """Checks one case against its expectations and records the verdict."""
        failures = []
        for key, expected in expectations.items():
            actual = parsed.get(key)
            if callable(expected):
                if not expected(actual, parsed):
                    failures.append(f"{key}: predicate failed, actual={actual!r}")
            elif actual != expected:
                failures.append(f"{key}: expected {expected!r}, actual {actual!r}")

        # An executed case must have clean validation. A refused case has no validation count.
        if code == 0 and parsed.get("validation") != 0:
            failures.append(
                f"validation: an executed case must report 0 errors, actual {parsed.get('validation')!r}")
        if parsed.get("validation"):
            self.validation_errors += parsed["validation"]

        verdict = "passed" if not failures else "failed"
        self.case_results.append({
            "name": name,
            "verdict": verdict,
            "exit": code,
            "failures": failures,
            "observed": parsed,
        })
        if failures:
            self.failure_reasons.append({"case": name, "reasons": failures})
        print(f"  {'PASS' if not failures else 'FAIL'}  {name}")
        for failure in failures:
            print(f"          {failure}")
        return not failures


def main(argv):
    self_test = "--self-test" in argv
    argv = [a for a in argv if a != "--self-test"]
    if len(argv) != 3:
        print(__doc__)
        return 2
    replay, base, work_root = argv
    os.makedirs(work_root, exist_ok=True)
    gate = Gate()

    # --- A: baseline ---------------------------------------------------------------------------
    work = prepare(os.path.join(work_root, "a-baseline"), base)
    code, out = run(replay, work)
    baseline = parse(out)
    gate.case("A baseline", code, baseline, {
        "index_count": 6,
        "first_index": 0,
        "vertex_offset": 0,
        "instance_count": 1,
        "first_instance": 0,
        "reference": "exact_match",
        "exact": "yes",
        "execution_state": "effective_captured_state",
    })

    # --- B: primitive count --------------------------------------------------------------------
    def prim_tape(tape):
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["num_primitives"] = 1
                op["derived"]["index_count"] = 3
    work = prepare(os.path.join(work_root, "b-prim"), base, mutate_tape=prim_tape)
    code, out = run(replay, work)
    parsed = parse(out)
    gate.case("B NumPrimitives 2 -> 1", code, parsed, {
        "index_count": 3,
        "covered": lambda v, p: v is not None and v != baseline["covered"],
        "reference": "pixel_mismatch",
    })

    # --- C: instance count ---------------------------------------------------------------------
    def inst_tape(tape):
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["num_instances"] = 3
                op["derived"]["instance_count"] = 3
    work = prepare(os.path.join(work_root, "c-inst"), base, mutate_tape=inst_tape)
    code, out = run(replay, work)
    parsed = parse(out)
    gate.case("C NumInstances 1 -> 3", code, parsed, {
        "instance_count": 3,
        "index_count": 6,
    })

    # --- D: start index ------------------------------------------------------------------------
    def start_tape(tape):
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["start_index"] = 3
                op["draw"]["num_primitives"] = 1
                op["derived"]["first_index"] = 3
                op["derived"]["index_count"] = 3
                op["derived"]["index_byte_offset"] = 6
    work = prepare(os.path.join(work_root, "d-start"), base, mutate_tape=start_tape)
    code, out = run(replay, work)
    parsed = parse(out)
    gate.case("D StartIndex 0 -> 3", code, parsed, {
        "first_index": 3,
        "index_count": 3,
    })

    # --- E: uniform contents -------------------------------------------------------------------
    def uniform(blob):
        struct.pack_into("<4f", blob, 64, 0.25, 1.0, 0.5, 1.0)
    work = prepare(os.path.join(work_root, "e-uniform"), base, mutate_blob=uniform)
    code, out = run(replay, work)
    parsed = parse(out)
    gate.case("E uniform tint changed", code, parsed, {
        "hash": lambda v, p: v is not None and v != baseline["hash"],
        "index_count": 6,
        "first_index": 0,
        "instance_count": 1,
    })

    # --- F: push constants ---------------------------------------------------------------------
    def push(package):
        b = bytearray(package["pipeline_state"]["push_constant_bytes"])
        struct.pack_into("<f", b, 0, 0.5)
        package["pipeline_state"]["push_constant_bytes"] = list(b)
    work = prepare(os.path.join(work_root, "f-push"), base, mutate_package=push)
    code, out = run(replay, work)
    parsed = parse(out)
    gate.case("F push offset.x 0 -> 0.5", code, parsed, {
        "hash": lambda v, p: v is not None and v != baseline["hash"],
        "covered": lambda v, p: v is not None and v != baseline["covered"],
        "index_count": 6,
        "instance_count": 1,
    })

    # --- G: cull mode --------------------------------------------------------------------------
    def cull(package):
        package["pipeline_state"]["cull_mode"] = 2
    work = prepare(os.path.join(work_root, "g-cull"), base, mutate_package=cull)
    code, out = run(replay, work)
    parsed = parse(out)
    gate.case("G cull_mode 0 -> back", code, parsed, {
        "covered": 0,
        "index_count": 6,
    })

    # --- H: write mask -------------------------------------------------------------------------
    def mask(package):
        package["pipeline_state"]["write_mask"] = [0x1]
    work = prepare(os.path.join(work_root, "h-mask"), base, mutate_package=mask)
    code, out = run(replay, work)
    parsed = parse(out)
    gate.case("H write_mask 0xF -> 0x1", code, parsed, {
        "reference": "incomplete_capture",
    })

    # --- I: permutation hash -------------------------------------------------------------------
    def perm(package):
        package["shader_stages"][0]["permutation_hash"] ^= 0xFFFF
    work = prepare(os.path.join(work_root, "i-perm"), base, mutate_package=perm)
    code, out = run(replay, work)
    parsed = parse(out)
    gate.case("I permutation hash corrupted", code, parsed, {
        "problem": "permutation_hash_mismatch",
    })
    if code == 0:
        gate.case("I must not execute", code, parsed, {"index_count": None})

    # --- J: missing bindings -------------------------------------------------------------------
    def binding(package):
        package["bindings"] = []
    work = prepare(os.path.join(work_root, "j-binding"), base, mutate_package=binding)
    code, out = run(replay, work)
    parsed = parse(out)
    gate.case("J captured bindings removed", code, parsed, {
        "problem": "missing_binding",
    })

    # --- K: missing resource -------------------------------------------------------------------
    def resource(package):
        package["resources"] = [r for r in package["resources"] if r["id"] != 1]
    work = prepare(os.path.join(work_root, "k-resource"), base, mutate_package=resource)
    code, out = run(replay, work)
    parsed = parse(out)
    gate.case("K uniform resource removed", code, parsed, {
        "problem": "missing_resource",
    })

    # --- L: out-of-range geometry --------------------------------------------------------------
    def range_tape(tape):
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["num_primitives"] = 100
                op["derived"]["index_count"] = 300
    work = prepare(os.path.join(work_root, "l-range"), base, mutate_tape=range_tape)
    code, out = run(replay, work)
    parsed = parse(out)
    gate.case("L index count 300 from a 6-element buffer", code, parsed, {
        "bounds_refused": True,
        "index_count": None,
    })

    # --- M: corrupt reference ------------------------------------------------------------------
    def ref(package):
        package["reference_output"]["post_sha256"] = "0" * 64
    work = prepare(os.path.join(work_root, "m-ref"), base, mutate_package=ref)
    code, out = run(replay, work)
    parsed = parse(out)
    gate.case("M reference hash corrupted", code, parsed, {
        "sidecar_refused": True,
    })

    # --- The negative control ------------------------------------------------------------------
    # A deliberately wrong expectation must make the gate fail. Without this, a gate that always
    # passed would look identical to a gate that worked.
    print("\n=== negative control ===")
    control = Gate()
    control.case("control: baseline asserted to have 999 indices", 0, baseline, {
        "index_count": 999,
    })
    control_failed = bool(control.failure_reasons)
    print(f"  {'PASS' if control_failed else 'FAIL'}  the gate rejects a wrong expectation")

    # --- Summary -------------------------------------------------------------------------------
    passed = sum(1 for r in gate.case_results if r["verdict"] == "passed")
    failed = len(gate.case_results) - passed
    print("\n\n=== summary ===")
    print(f"passed: {passed}")
    print(f"failed: {failed}")
    print(f"validation_errors: {gate.validation_errors}")
    if gate.failure_reasons:
        print("failure_reasons:")
        for entry in gate.failure_reasons:
            for reason in entry["reasons"]:
                print(f"  {entry['case']}: {reason}")

    report = {
        "passed": passed,
        "failed": failed,
        "case_results": gate.case_results,
        "failure_reasons": gate.failure_reasons,
        "validation_errors": gate.validation_errors,
        "negative_control_rejected_wrong_expectation": control_failed,
    }
    with open(os.path.join(work_root, "mutations.json"), "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=1)
    print(f"\nwrote {os.path.join(work_root, 'mutations.json')}")

    if failed != 0:
        print("\nGATE FAILED")
        return 1
    if not control_failed:
        print("\nGATE FAILED: the negative control did not reject a wrong expectation")
        return 1
    print("\nGATE PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
