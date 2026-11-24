#!/usr/bin/env python3
"""Mutation harness for the RHI-02 semantic replay.

Runs the integrated replay over a set of tape mutations and records what each one did to the
executed Vulkan draw and to the rendered image. The point is to show that the semantic payload
controls the draw, and that the resource package controls the sampling, independently.

Every mutation is labelled synthetic: these are fixtures derived from a real resource package, not
original game evidence.

Usage: python run_mutations.py <replay-exe> <package-dir> <work-dir>
"""

import json
import os
import re
import shutil
import subprocess
import sys

DRAW_RE = re.compile(
    r"draw_from_tape: index_count=(\d+) first_index=(\d+) vertex_offset=(-?\d+) "
    r"instance_count=(\d+) first_instance=(\d+)")
IMAGE_RE = re.compile(r"image: hash=(\d+) distinct=(\d+) covered=(\d+)")
MODE_RE = re.compile(r"mode: (\w+)")


def load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def save(path, data):
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=1)


def run(replay, directory, extra=()):
    result = subprocess.run([replay, directory, *extra], capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def parse(output):
    draw = DRAW_RE.search(output)
    images = IMAGE_RE.findall(output)
    mode = MODE_RE.search(output)
    return {
        "index_count": int(draw.group(1)) if draw else None,
        "first_index": int(draw.group(2)) if draw else None,
        "vertex_offset": int(draw.group(3)) if draw else None,
        "instance_count": int(draw.group(4)) if draw else None,
        "first_instance": int(draw.group(5)) if draw else None,
        "images": [(int(h), int(d), int(c)) for h, d, c in images],
        "mode": mode.group(1) if mode else None,
    }


def prepare(work, base_tape, base_package, mutate_tape=None, mutate_package=None):
    """Copies the fixture and applies a mutation, returning the directory."""
    if os.path.exists(work):
        shutil.rmtree(work)
    shutil.copytree(base_package, work)
    tape = load(os.path.join(base_tape, "tape.json"))
    if mutate_tape is not None:
        mutate_tape(tape)
    save(os.path.join(work, "tape.json"), tape)
    if mutate_package is not None:
        package = load(os.path.join(work, "package.json"))
        mutate_package(package)
        save(os.path.join(work, "package.json"), package)
    return work


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    replay, package_dir, work_root = argv
    base_tape = package_dir
    os.makedirs(work_root, exist_ok=True)

    results = []

    def record(name, code, parsed, note=""):
        results.append({"name": name, "exit": code, **parsed, "note": note})
        print(f"\n=== {name} ===")
        print(f"  exit={code} mode={parsed['mode']} "
              f"index_count={parsed['index_count']} first_index={parsed['first_index']} "
              f"vertex_offset={parsed['vertex_offset']} "
              f"instance_count={parsed['instance_count']} "
              f"first_instance={parsed['first_instance']}")
        if parsed["images"]:
            h, d, c = parsed["images"][0]
            print(f"  image: hash={h} distinct={d} covered={c}")
        if note:
            print(f"  note: {note}")

    # --- A: baseline ---------------------------------------------------------------------------
    work = prepare(os.path.join(work_root, "a-baseline"), base_tape, package_dir)
    code, out = run(replay, work, ["--substitute-render-targets"])
    record("A baseline (367 primitives, 1101 indices, 2 instances)", code, parse(out))

    # The same baseline through the diagnostic instance view, so the instance-range mutations are
    # compared against a like-for-like image rather than against the untinted one.
    work = prepare(os.path.join(work_root, "a-baseline-instid"), base_tape, package_dir)
    code, out = run(replay, work, ["--substitute-render-targets", "--instance-id"])
    record("A2 baseline through the instance-id view", code, parse(out),
           "reference image for the instance-range mutations")

    # --- B: primitive-count mutation -----------------------------------------------------------
    def mutate_primitives(tape):
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["num_primitives"] = 200
                # The derived block is recomputed by the production derivation path, which is what
                # the replay validates against. Leaving it stale would be test C.
                op["derived"]["index_count"] = 600

    work = prepare(os.path.join(work_root, "b-primitives"), base_tape, package_dir,
                   mutate_tape=mutate_primitives)
    code, out = run(replay, work, ["--substitute-render-targets"])
    record("B NumPrimitives 367 -> 200 (derived 600)", code, parse(out),
           "synthetic mutation of the source payload")

    # --- C: derived-field corruption -----------------------------------------------------------
    def corrupt_derived(tape):
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["derived"]["index_count"] = 600

    work = prepare(os.path.join(work_root, "c-corrupt"), base_tape, package_dir,
                   mutate_tape=corrupt_derived)
    code, out = run(replay, work, ["--substitute-render-targets"])
    record("C derived index_count corrupted to 600, payload unchanged", code, parse(out),
           "must be refused as derived_inconsistent")

    # --- D: instance-count mutation ------------------------------------------------------------
    def mutate_instances(tape):
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["num_instances"] = 1
                op["derived"]["instance_count"] = 1

    work = prepare(os.path.join(work_root, "d-instances"), base_tape, package_dir,
                   mutate_tape=mutate_instances)
    code, out = run(replay, work, ["--substitute-render-targets", "--instance-id"])
    record("D NumInstances 2 -> 1 (instance-id view)", code, parse(out),
           "the two instances overlap exactly, so the diagnostic instance tint is what shows it")

    # --- E: first instance ---------------------------------------------------------------------
    def mutate_first_instance(tape):
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["first_instance"] = 3

    work = prepare(os.path.join(work_root, "e-first-instance"), base_tape, package_dir,
                   mutate_tape=mutate_first_instance)
    code, out = run(replay, work, ["--substitute-render-targets", "--instance-id"])
    record("E FirstInstance 0 -> 3 (instance-id view)", code, parse(out),
           "the diagnostic instance tint is what shows the shifted instance range")

    # --- F: start index ------------------------------------------------------------------------
    def mutate_start_index(tape):
        # The index buffer holds 1101 elements, so a nonzero start must shorten the draw to stay
        # inside it. 168 + 600 = 768, which is within the buffer, and every index in that window is
        # at least 7 so the base-vertex case below can also be exercised.
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["start_index"] = 168
                op["draw"]["num_primitives"] = 200
                op["derived"]["first_index"] = 168
                op["derived"]["index_count"] = 600
                op["derived"]["index_byte_offset"] = 168 * 2

    work = prepare(os.path.join(work_root, "f-start-index"), base_tape, package_dir,
                   mutate_tape=mutate_start_index)
    code, out = run(replay, work, ["--substitute-render-targets"])
    record("F StartIndex 0 -> 168, 600 indices", code, parse(out),
           "168 + 600 = 768, inside the 1101-element index buffer")

    # --- G: negative base vertex ---------------------------------------------------------------
    def mutate_base_vertex(tape):
        # A negative base vertex is only valid when every fetched index is at least its magnitude.
        # The window starting at 7 has a minimum index of 7, so -7 addresses vertex 0 and stays
        # inside the 807-vertex stream.
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["start_index"] = 7
                op["draw"]["num_primitives"] = 200
                op["draw"]["base_vertex_index"] = -7
                op["derived"]["first_index"] = 7
                op["derived"]["index_count"] = 600
                op["derived"]["index_byte_offset"] = 7 * 2
                op["derived"]["vertex_offset"] = -7

    work = prepare(os.path.join(work_root, "g-base-vertex"), base_tape, package_dir,
                   mutate_tape=mutate_base_vertex)
    code, out = run(replay, work, ["--substitute-render-targets"])
    record("G BaseVertexIndex 0 -> -7 from StartIndex 7", code, parse(out),
           "the window's minimum index is 7, so -7 addresses vertex 0")

    # --- H: unsupported topology ---------------------------------------------------------------
    def mutate_topology(tape):
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["primitive_type"] = 2  # PT_PointList

    work = prepare(os.path.join(work_root, "h-topology"), base_tape, package_dir,
                   mutate_tape=mutate_topology)
    code, out = run(replay, work, ["--substitute-render-targets"])
    record("H PrimitiveType 0 -> 2 (point list, unsupported)", code, parse(out),
           "must be refused by the derivation")

    # --- I: resource mutation, untextured ------------------------------------------------------
    work = prepare(os.path.join(work_root, "i-untextured"), base_tape, package_dir)
    code, out = run(replay, work, ["--substitute-render-targets", "--untextured"])
    record("I resource mutation: texturing disabled, tape unchanged", code, parse(out),
           "draw arguments must be identical to A")

    # --- J: identity rejection, different index resource ---------------------------------------
    def mutate_index_identity(package):
        package["index_binding"]["guest_base"] = 0x2015E80000

    work = prepare(os.path.join(work_root, "j-identity"), base_tape, package_dir,
                   mutate_package=mutate_index_identity)
    code, out = run(replay, work, ["--substitute-render-targets"])
    record("J package index identity changed, tape unchanged", code, parse(out),
           "must be rejected before GPU execution")

    # --- Summary -------------------------------------------------------------------------------
    print("\n\n=== summary ===")
    print(f"{'case':<58} {'exit':>4} {'mode':>12} {'indices':>8} {'inst':>5} {'hash':>21}")
    for r in results:
        h = r["images"][0][0] if r["images"] else "-"
        print(f"{r['name'][:58]:<58} {r['exit']:>4} {str(r['mode']):>12} "
              f"{str(r['index_count']):>8} {str(r['instance_count']):>5} {str(h):>21}")

    with open(os.path.join(work_root, "mutations.json"), "w", encoding="utf-8") as handle:
        json.dump(results, handle, indent=1)
    print(f"\nwrote {os.path.join(work_root, 'mutations.json')}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
