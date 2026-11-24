#!/usr/bin/env python3
"""Mutation matrix for the schema-4 effective-shader fixture.

Each case changes one thing and records what the run did: the executed draw arguments, the
effective shader identities, the output hash, the reference verdict and the validation count.

The fixture is synthetic. Every case is labelled that way, and none of it is original-game
evidence.

Usage: python run_schema4_mutations.py <replay-exe> <fixture-dir> <work-dir>
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
REF_RE = re.compile(r"reference_status: (\w+)")
EXACT_RE = re.compile(r"exact_match: (\w+)")
VALIDATION_RE = re.compile(r"validation: errors=(\d+)")
STAGE_RE = re.compile(r"stage: logical=(\d+) program_hash=0x([0-9a-f]+) perm_idx=(\d+)")
EXEC_RE = re.compile(r"execution_state: (\w+)")
SUBST_RE = re.compile(r"pipeline_state_substituted: (.*)")


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
    }


def prepare(work, base, mutate_package=None, mutate_tape=None):
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
    return work


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    replay, base, work_root = argv
    os.makedirs(work_root, exist_ok=True)
    results = []

    def record(name, code, parsed, note=""):
        results.append({"name": name, "exit": code, **parsed, "note": note})
        print(f"\n=== {name} ===")
        print(f"  exit={code} indices={parsed['index_count']} first={parsed['first_index']} "
              f"voffset={parsed['vertex_offset']} inst={parsed['instance_count']} "
              f"first_inst={parsed['first_instance']}")
        print(f"  hash={parsed['hash']} covered={parsed['covered']} "
              f"ref={parsed['reference']} exact={parsed['exact']} "
              f"validation={parsed['validation']} exec={parsed['execution_state']}")
        if note:
            print(f"  note: {note}")

    # A: baseline.
    work = prepare(os.path.join(work_root, "a-baseline"), base)
    code, out = run(replay, work)
    record("A baseline", code, parse(out))

    # B: primitive count.
    def prim(package):
        pass
    def prim_tape(tape):
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["num_primitives"] = 1
                op["derived"]["index_count"] = 3
    work = prepare(os.path.join(work_root, "b-prim"), base, mutate_tape=prim_tape)
    code, out = run(replay, work)
    record("B NumPrimitives 2 -> 1", code, parse(out), "index count must become 3")

    # C: instance count.
    def inst_tape(tape):
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["num_instances"] = 3
                op["derived"]["instance_count"] = 3
    work = prepare(os.path.join(work_root, "c-inst"), base, mutate_tape=inst_tape)
    code, out = run(replay, work)
    record("C NumInstances 1 -> 3", code, parse(out))

    # D: start index.
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
    record("D StartIndex 0 -> 3", code, parse(out))

    # E: uniform contents.
    def uniform(package):
        # The uniform block is resource 1, an identity MVP and a tint. Change the tint.
        blob = bytearray(open(os.path.join(work_root, "e-uniform", "resources.bin"), "rb").read())
        import struct
        struct.pack_into("<4f", blob, 64, 0.25, 1.0, 0.5, 1.0)
        open(os.path.join(work_root, "e-uniform", "resources.bin"), "wb").write(bytes(blob))
    work = prepare(os.path.join(work_root, "e-uniform"), base)
    uniform(None)
    code, out = run(replay, work)
    record("E uniform tint changed", code, parse(out), "draw arguments must be unchanged")

    # F: push constants.
    def push(package):
        import struct
        b = bytearray(package["pipeline_state"]["push_constant_bytes"])
        struct.pack_into("<f", b, 0, 0.5)
        package["pipeline_state"]["push_constant_bytes"] = list(b)
    work = prepare(os.path.join(work_root, "f-push"), base, mutate_package=push)
    code, out = run(replay, work)
    record("F push offset.x 0 -> 0.5", code, parse(out))

    # G: cull mode.
    def cull(package):
        package["pipeline_state"]["cull_mode"] = 2
    work = prepare(os.path.join(work_root, "g-cull"), base, mutate_package=cull)
    code, out = run(replay, work)
    record("G cull_mode 0 -> back", code, parse(out))

    # H: write mask.
    def mask(package):
        package["pipeline_state"]["write_mask"] = [0x1]
    work = prepare(os.path.join(work_root, "h-mask"), base, mutate_package=mask)
    code, out = run(replay, work)
    record("H write_mask 0xF -> 0x1", code, parse(out))

    # I: permutation hash mismatch.
    def perm(package):
        package["shader_stages"][0]["permutation_hash"] ^= 0xFFFF
    work = prepare(os.path.join(work_root, "i-perm"), base, mutate_package=perm)
    code, out = run(replay, work)
    record("I permutation hash corrupted", code, parse(out), "must be refused")

    # J: missing binding.
    def binding(package):
        package["bindings"] = []
    work = prepare(os.path.join(work_root, "j-binding"), base, mutate_package=binding)
    code, out = run(replay, work)
    record("J captured bindings removed", code, parse(out), "must be refused")

    # K: missing resource.
    def resource(package):
        package["resources"] = [r for r in package["resources"] if r["id"] != 1]
    work = prepare(os.path.join(work_root, "k-resource"), base, mutate_package=resource)
    code, out = run(replay, work)
    record("K uniform resource removed", code, parse(out), "must be refused")

    # L: out-of-range geometry.
    def range_tape(tape):
        for op in tape["operations"]:
            if op["kind"] == "DrawIndexed":
                op["draw"]["num_primitives"] = 100
                op["derived"]["index_count"] = 300
    work = prepare(os.path.join(work_root, "l-range"), base, mutate_tape=range_tape)
    code, out = run(replay, work)
    record("L index count 300 from a 6-element buffer", code, parse(out),
           "must be refused before GPU")

    # M: wrong reference.
    def ref(package):
        package["reference_output"]["post_sha256"] = "0" * 64
    work = prepare(os.path.join(work_root, "m-ref"), base, mutate_package=ref)
    code, out = run(replay, work)
    record("M reference hash corrupted", code, parse(out), "must fail the comparison")

    # --- Summary -------------------------------------------------------------------------------
    print("\n\n=== summary ===")
    print(f"{'case':<40} {'exit':>4} {'indices':>7} {'inst':>4} {'covered':>7} "
          f"{'ref':>22} {'valid':>5}")
    for r in results:
        print(f"{r['name'][:40]:<40} {r['exit']:>4} {str(r['index_count']):>7} "
              f"{str(r['instance_count']):>4} {str(r['covered']):>7} "
              f"{str(r['reference']):>22} {str(r['validation']):>5}")

    with open(os.path.join(work_root, "mutations.json"), "w", encoding="utf-8") as handle:
        json.dump(results, handle, indent=1)
    print(f"\nwrote {os.path.join(work_root, 'mutations.json')}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
