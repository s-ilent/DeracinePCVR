#!/usr/bin/env python3
"""Independent validator for a Deracine draw package (schema 2).

Deliberately a separate implementation from the C++ exporter: it re-derives every content hash
from the blob and re-checks the geometry arithmetic, so a bug in the exporter cannot certify its
own output.

It has no dependency on shadPS4, the game, or the capture process.

Usage: python validate_draw_package.py <package-directory>
"""

import json
import math
import os
import struct
import sys

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1
SCHEMA_VERSION = 2

# Vulkan formats the capture can produce for vertex input, with their component count and byte
# width. A format outside this table is rejected rather than guessed at.
FORMATS = {
    106: (3, 4, "R32G32B32_SFLOAT"),
    109: (4, 4, "R32G32B32A32_SFLOAT"),
    103: (2, 4, "R32G32_SFLOAT"),
    37: (4, 1, "R8G8B8A8_UNORM"),
    97: (4, 2, "R16G16B16A16_SFLOAT"),
    83: (2, 2, "R16G16_SFLOAT"),
}


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
    geometry = package.get("geometry", {})

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

    # Attribute layout: format, width, offset and stride must agree.
    for vertex in vertex_bindings:
        fmt = vertex.get("vk_format", 0)
        if fmt not in FORMATS:
            return fail(f"binding {vertex.get('binding')} has unknown format {fmt}")
        components, width, name = FORMATS[fmt]
        if vertex.get("element_count", 0) != components:
            return fail(
                f"binding {vertex.get('binding')} declares {vertex.get('element_count')} "
                f"components but {name} has {components}")
        stride = vertex.get("stride", 0)
        offset = vertex.get("attr_offset", 0)
        if stride == 0:
            return fail(f"binding {vertex.get('binding')} has zero stride")
        if offset + components * width > stride:
            return fail(
                f"binding {vertex.get('binding')} attribute ({offset}+{components * width}) "
                f"exceeds its stride {stride}")
        ref = vertex.get("resource_ref", 0)
        if ref:
            size = by_id[ref].get("clamped_size", 0)
            if size % stride != 0:
                return fail(
                    f"binding {vertex.get('binding')} range {size} is not a multiple of "
                    f"stride {stride}")

    # Geometry arithmetic.
    if readiness.get("geometry_ready"):
        if not vertex_bindings:
            return fail("geometry readiness claimed with no vertex bindings")
        for vertex in vertex_bindings:
            ref = vertex.get("resource_ref", 0)
            if ref == 0:
                return fail("geometry-ready vertex binding has no resource")
            if by_id[ref].get("status") not in ("host_content_verified", "host_content_captured"):
                return fail(f"vertex binding to resource {ref} is not host-verified")

        if package.get("pipeline", {}).get("is_indexed"):
            if not index_binding:
                return fail("indexed draw claims geometry readiness with no index binding")
            ref = index_binding.get("resource_ref", 0)
            if ref == 0 or ref not in by_id:
                return fail("index binding has no resolvable resource")
            if by_id[ref].get("status") not in ("host_content_verified", "host_content_captured"):
                return fail("index resource is not host-verified")
            width = index_binding.get("index_size", 0)
            if width not in (2, 4):
                return fail(f"index width {width} is neither 2 nor 4")
            count = index_binding.get("index_count", 0)
            if count * width > by_id[ref].get("clamped_size", 0):
                return fail("index count exceeds the captured index bytes")

            # Decode the indices and check they address captured vertex data.
            offset = by_id[ref].get("blob_offset", 0)
            data = blob[offset:offset + count * width]
            fmt = "<%d%s" % (count, "H" if width == 2 else "I")
            indices = struct.unpack(fmt, data)
            if indices:
                highest = max(indices)
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

    # The geometry interpretation must be internally consistent and must not overclaim.
    if geometry:
        position_binding = geometry.get("position_binding", 0)
        match = [v for v in vertex_bindings if v.get("binding") == position_binding]
        if not match:
            return fail(f"position binding {position_binding} does not exist")
        if geometry.get("position_components", 0) != match[0].get("element_count", 0):
            return fail("position component count does not match its binding")

        lo = geometry.get("bounds_min", [])
        hi = geometry.get("bounds_max", [])
        if len(lo) != 3 or len(hi) != 3:
            return fail("geometry bounds are not three-dimensional")
        for a, b in zip(lo, hi):
            if not (math.isfinite(a) and math.isfinite(b)):
                return fail("geometry bounds are not finite")
            if b < a:
                return fail("geometry bounds are inverted")

        # A three-dimensional claim needs real extent on all three axes.
        spans = [b - a for a, b in zip(lo, hi)]
        if geometry.get("is_three_dimensional"):
            if min(spans) <= 0.0:
                return fail("three-dimensional claim with a degenerate axis")
            if geometry.get("triangle_count", 0) == 0:
                return fail("three-dimensional claim with no triangles")
            if geometry.get("degenerate_triangles", 0) >= geometry.get("triangle_count", 1):
                return fail("every triangle is degenerate")

        # Recompute the triangle statistics from the captured indices.
        if index_binding and index_binding.get("resource_ref"):
            ref = index_binding["resource_ref"]
            width = index_binding.get("index_size", 0)
            count = index_binding.get("index_count", 0)
            offset = by_id[ref].get("blob_offset", 0)
            data = blob[offset:offset + count * width]
            fmt = "<%d%s" % (count, "H" if width == 2 else "I")
            indices = struct.unpack(fmt, data)
            triangles = count // 3
            if geometry.get("triangle_count", 0) != triangles:
                return fail(
                    f"declared {geometry.get('triangle_count')} triangles but the indices give "
                    f"{triangles}")
            degenerate = sum(
                1 for t in range(0, count - 2, 3)
                if indices[t] == indices[t + 1] or indices[t + 1] == indices[t + 2]
                or indices[t] == indices[t + 2])
            if geometry.get("degenerate_triangles", 0) != degenerate:
                return fail(
                    f"declared {geometry.get('degenerate_triangles')} degenerate triangles but "
                    f"the indices give {degenerate}")
            unique = len(set(indices))
            if geometry.get("unique_referenced_vertices", 0) != unique:
                return fail(
                    f"declared {geometry.get('unique_referenced_vertices')} unique vertices but "
                    f"the indices reference {unique}")

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
        f"bytes={total} three_d={geometry.get('is_three_dimensional')} "
        f"triangles={geometry.get('triangle_count')} "
        f"diagnostic_ready={readiness.get('diagnostic_geometry_replay_ready')}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
