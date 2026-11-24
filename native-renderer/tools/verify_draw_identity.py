#!/usr/bin/env python3
"""Verify that a semantic tape and a draw package describe the same draw.

This is the Phase 1 gate. A tape and a package can come from the same scene and
still describe different draws, so identity is established from the data, not
from provenance ids or file proximity.

What is checked:
  1. scene and provenance agreement
  2. draw arguments agree between tape and package
  3. the tape's draw arguments are consistent with the package's actual index bytes
  4. every vertex stream yields the vertex count the tape declares
  5. the highest index is inside the vertex range
  6. triangle count agrees with the index count

What is deliberately reported as NOT established:
  - vertex-buffer resource linkage (the tape records interface pointers, not
    resource bases, and marks its bindings tentative)
  - shader permutation identity (the tape carries no shader identity)

Exit code 0 means identity is established for the draw arguments and geometry.
"""
import json
import struct
import sys
from pathlib import Path


def load(directory):
    d = Path(directory)
    tape = json.loads((d / "tape.json").read_text(encoding="utf-8"))
    package = json.loads((d / "package.json").read_text(encoding="utf-8"))
    blob = (d / "resources.bin").read_bytes()
    return tape, package, blob


def resource_bytes(package, blob, resource_id):
    for r in package["resources"]:
        if r["id"] == resource_id:
            off = r["blob_offset"]
            length = r["byte_length"]
            return blob[off:off + length]
    return None


def main(argv):
    if len(argv) < 2:
        print("usage: verify_draw_identity.py <package-dir>")
        return 2

    tape, package, blob = load(argv[1])
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, ok, detail))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  " + detail) if detail else ""))

    print("=== draw identity: %s ===" % argv[1])

    # 1. Scene and provenance
    check("scene agrees",
          tape["scene"] == package["scene"],
          "tape=%s package=%s" % (tape["scene"], package["scene"]))
    check("provenance agrees",
          tape["provenance_id"] == package["provenance_id"],
          "tape=%s package=%s" % (tape["provenance_id"], package["provenance_id"]))

    # 2. Draw arguments
    draws = [o for o in tape["operations"] if o["kind"] == "DrawIndexed"]
    if len(draws) != 1:
        print("  [FAIL] expected exactly one DrawIndexed, found %d" % len(draws))
        return 1
    draw = draws[0]
    payload = draw["draw"]
    derived = draw["derived"]
    ib = package["index_binding"]

    check("index count agrees",
          derived["index_count"] == ib["index_count"],
          "tape=%s package=%s" % (derived["index_count"], ib["index_count"]))
    check("first index agrees",
          derived["first_index"] == ib["first_index"],
          "tape=%s package=%s" % (derived["first_index"], ib["first_index"]))
    check("base vertex agrees",
          derived["vertex_offset"] == ib["base_vertex"],
          "tape=%s package=%s" % (derived["vertex_offset"], ib["base_vertex"]))
    check("instance count agrees",
          derived["instance_count"] == ib["instance_count"],
          "tape=%s package=%s" % (derived["instance_count"], ib["instance_count"]))
    check("first instance agrees",
          payload["first_instance"] == ib["first_instance"],
          "tape=%s package=%s" % (payload["first_instance"], ib["first_instance"]))

    # 3. Draw arguments against the actual index bytes
    index_bytes = resource_bytes(package, blob, ib["resource_ref"])
    if index_bytes is None:
        print("  [FAIL] index resource %s missing" % ib["resource_ref"])
        return 1
    width = ib["index_size"]
    count = len(index_bytes) // width
    fmt = "<%d%s" % (count, "H" if width == 2 else "I")
    indices = struct.unpack(fmt, index_bytes)

    check("index bytes match declared count",
          count == ib["index_count"],
          "bytes=%d declared=%d" % (count, ib["index_count"]))
    check("tape index count matches index bytes",
          derived["index_count"] == count,
          "tape=%d bytes=%d" % (derived["index_count"], count))

    highest = max(indices)
    check("highest index inside vertex range",
          highest < payload["num_vertices"],
          "max=%d tape.num_vertices=%d" % (highest, payload["num_vertices"]))

    check("triangle count agrees",
          count // 3 == payload["num_primitives"],
          "count/3=%d tape.num_primitives=%d" % (count // 3, payload["num_primitives"]))
    check("package triangle count agrees",
          count // 3 == package["geometry"]["triangle_count"],
          "count/3=%d package=%d" % (count // 3, package["geometry"]["triangle_count"]))

    # 4. Every vertex stream yields the declared vertex count
    for b in package["vertex_bindings"]:
        vb = resource_bytes(package, blob, b["resource_ref"])
        if vb is None:
            check("vertex stream %s present" % b["binding"], False, "resource missing")
            continue
        n = len(vb) // b["stride"]
        check("vertex stream %s yields %d vertices" % (b["binding"], n),
              n == payload["num_vertices"],
              "stride=%d bytes=%d" % (b["stride"], len(vb)))

    # 5. Report what is NOT established
    print()
    print("  [info] not established by this check:")
    tentative = sum(1 for o in tape["operations"]
                    if o["kind"] == "BindVertexBuffers" and o.get("evidence") == "tentative")
    print("    - vertex-buffer resource linkage: %d tape bindings are marked tentative"
          % tentative)
    print("      and record interface pointers, not resource bases")
    print("    - shader permutation identity: the tape carries no shader identity")

    failed = [c for c in checks if not c[1]]
    print()
    print("=== %d checks, %d failed ===" % (len(checks), len(failed)))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
