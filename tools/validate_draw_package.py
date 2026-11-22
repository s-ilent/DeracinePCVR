#!/usr/bin/env python3
"""Independent validator for a Déraciné draw package.

This is deliberately a separate implementation from the C++ exporter: it re-derives the content
hashes from the blob and checks the package's internal consistency, so a bug in the exporter cannot
silently certify its own output.

Usage: python validate_draw_package.py <package-directory>
"""

import json
import os
import sys

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1


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
    json_path = os.path.join(directory, "package.json")
    blob_path = os.path.join(directory, "resources.bin")

    with open(json_path, encoding="utf-8") as handle:
        package = json.load(handle)
    with open(blob_path, "rb") as handle:
        blob = handle.read()

    if package.get("schema_version") != 1:
        return fail("unsupported schema version")
    if not package.get("provenance_id"):
        return fail("missing provenance id")

    resources = package.get("resources", [])
    bindings = package.get("shader_buffers", [])

    # Resource ids must be unique.
    seen = set()
    for resource in resources:
        rid = resource.get("id")
        if not rid:
            return fail("resource id 0 is reserved")
        if rid in seen:
            return fail(f"duplicate resource id {rid}")
        seen.add(rid)

    # Every binding must reference an existing resource, or be metadata-only.
    for binding in bindings:
        ref = binding.get("resource_ref", 0)
        if ref == 0:
            if binding.get("status") != "metadata_only":
                return fail("binding has no resource reference")
            continue
        if ref not in seen:
            return fail(f"binding references missing resource {ref}")

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

    # Replay readiness must be justified by the resources, not merely asserted.
    for binding in bindings:
        ref = binding.get("resource_ref", 0)
        if ref == 0:
            continue
        resource = next(r for r in resources if r["id"] == ref)
        if resource.get("status") not in ("host_content_verified", "host_content_captured"):
            return fail(f"binding to resource {ref} is not host-verified")

    claimed = package.get("capture_status", {}).get("replay_ready", False)
    if claimed and not bindings:
        return fail("package claims replay readiness with no bindings")

    total = sum(r.get("byte_length", 0) for r in resources)
    print(
        f"VALID: schema=1 provenance={package['provenance_id']} "
        f"resources={len(resources)} bindings={len(bindings)} bytes={total} "
        f"replay_ready={claimed}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
