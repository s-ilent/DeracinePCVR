#!/usr/bin/env python3
"""Independent validator for a Deracine draw package (schema 2).

Deliberately a separate implementation from the C++ exporter: it re-derives every content hash
from the blob and re-checks the geometry arithmetic, so a bug in the exporter cannot certify its
own output.

It has no dependency on shadPS4, the game, or the capture process.

Usage: python validate_draw_package.py <package-directory>
"""

import json
import os
import struct
import sys

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1
SCHEMA_VERSION = 2


def fnv1a(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def fail(msg: str) -> int:
    print(f"INVALID: {msg}")
    return 1


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__)
        return 2
    directory = argv[0]
    with open(os.path.join(directory, "package.json"), encoding="utf-8") as handle:
        package = json.load(handle)
    with open(os.path.join(directory, "resources.bin"), "rb") as handle:
        blob = handle.read()

    if package.get("schema_version") != SCHEMA_VERSION:
        return fail(f"schema version {package.get('schema_version')} is not {SCHEMA_VERSION}")
    if not package.get("provenance_id"):
        return fail("missing provenance id")

    resources = package.get("resources", [])
    bindings = package.get("shader_buffers", [])
    vertex_bindings = package.get("vertex_bindings", [])
    index_binding = package.get("index_binding")
    readiness = package.get("readiness", {})

    # Resource ids unique.
    by_id = {}
    for resource in resources:
        rid = resource.get("id")
        if not rid:
            return fail("resource id 0 is reserved")
        if rid in by_id:
            return fail(f"duplicate resource id {rid}")
        by_id[rid] = resource

    # Every reference resolves.
    for group, name in ((bindings, "shader binding"), (vertex_bindings, "vertex binding")):
        for entry in group:
            ref = entry.get("resource_ref", 0)
            if ref == 0:
                if entry.get("status") != "metadata_only":
                    return fail(f"{name} has no resource reference")
                continue
            if ref not in by_id:
                return fail(f"{name} references missing resource {ref}")
    if index_binding:
        ref = index_binding.get("resource_ref", 0)
        if ref and ref not in by_id:
            return fail(f"index binding references missing resource {ref}")

    # Re-derive every content hash from the blob.
    for resource in resources:
        length = resource.get("byte_length", 0)
        offset = resource.get("blob_offset", 0)
        if length == 0:
            if resource.get("status") in ("host_content_verified", "host_content_captured"):
                return fail(f"resource {resource['id']} claims verified content but has no bytes")
            continue
        if offset + length > len(blob):
            return fail(f"resource {resource['id']} range exceeds the blob")
        data = blob[offset:offset + length]
        if len(data) != resource.get("clamped_size"):
            return fail(f"resource {resource['id']} byte length does not match its range")
        if fnv1a(data) != resource.get("content_hash"):
            return fail(f"resource {resource['id']} content hash does not match its bytes")

    # Geometry arithmetic.
    if readiness.get("geometry_ready"):
        if not vertex_bindings:
            return fail("geometry readiness claimed with no vertex bindings")
        for vertex in vertex_bindings:
            ref = vertex.get("resource_ref", 0)
            if ref == 0:
                return fail("geometry-ready vertex binding has no resource")
            resource = by_id[ref]
            if resource.get("status") not in ("host_content_verified", "host_content_captured"):
                return fail(f"vertex binding to resource {ref} is not host-verified")
            stride = vertex.get("stride", 0)
            size = vertex.get("guest_size", 0)
            if stride > 0 and size % stride != 0:
                return fail(f"vertex range {size} is not a multiple of stride {stride}")

        if package.get("pipeline", {}).get("is_indexed"):
            if not index_binding:
                return fail("indexed draw claims geometry readiness with no index binding")
            ref = index_binding.get("resource_ref", 0)
            if ref == 0 or ref not in by_id:
                return fail("index binding has no resolvable resource")
            resource = by_id[ref]
            if resource.get("status") not in ("host_content_verified", "host_content_captured"):
                return fail("index resource is not host-verified")
            width = index_binding.get("index_size", 0)
            if width not in (2, 4):
                return fail(f"index width {width} is neither 2 nor 4")
            count = index_binding.get("index_count", 0)
            if count * width > resource.get("clamped_size", 0):
                return fail("index count exceeds the captured index bytes")

            # Decode the indices and check they address captured vertex data.
            offset = resource.get("blob_offset", 0)
            data = blob[offset:offset + count * width]
            fmt = "<%d%s" % (count, "H" if width == 2 else "I")
            indices = struct.unpack(fmt, data)
            if indices:
                highest = max(indices)
                # The vertex range must cover the highest referenced index.
                for vertex in vertex_bindings:
                    stride = vertex.get("stride", 0)
                    if stride == 0:
                        continue
                    vres = by_id[vertex.get("resource_ref")]
                    available = vres.get("clamped_size", 0) // stride
                    if highest >= available:
                        return fail(
                            f"index {highest} exceeds the {available} vertices captured for "
                            f"binding {vertex.get('binding')}")

    # Readiness claims must be justified.
    if readiness.get("diagnostic_geometry_replay_ready") and not readiness.get("geometry_ready"):
        return fail("diagnostic replay readiness claimed without geometry readiness")
    if readiness.get("full_draw_replay_ready"):
        for key in ("geometry_ready", "shader_buffers_ready", "textures_ready",
                    "pipeline_state_ready"):
            if not readiness.get(key):
                return fail(f"full draw readiness claimed without {key}")

    total = sum(r.get("byte_length", 0) for r in resources)
    print(
        f"VALID: schema={SCHEMA_VERSION} provenance={package['provenance_id']} "
        f"resources={len(resources)} shader_bindings={len(bindings)} "
        f"vertex_bindings={len(vertex_bindings)} indexed={bool(index_binding)} "
        f"bytes={total} geometry_ready={readiness.get('geometry_ready')} "
        f"diagnostic_ready={readiness.get('diagnostic_geometry_replay_ready')}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
