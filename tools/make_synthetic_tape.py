#!/usr/bin/env python3
"""Builds the synthetic tape fixture from the real SC-10S resource package.

The fixture exists to exercise the integration path, not to prove provenance. It is therefore
labelled as synthetic in its own scene name, and its index identity is declared in the domain the
tape actually records: the RHI invocation argument, which is an original UE4 object pointer, not the
guest GPU address the package carries.

Copying the package's guest address into the tape would make the identity check pass for the wrong
reason. That is the circularity this script deliberately avoids.

The optional second argument selects the index-identity domain:

  none     (default) no index identity, so the join reports insufficient evidence
  guest    the package's guest address, so the matcher has a real value to compare

The `guest` variant exists only to exercise the identity matcher's rejection paths. It is still a
synthetic fixture: copying the package's address into the tape proves nothing about provenance, and
the scene name records that.

Usage: python make_synthetic_tape.py <package-dir> [none|guest]
"""

import json
import os
import sys


def main(argv):
    if len(argv) not in (1, 2):
        print(__doc__)
        return 2
    directory = argv[0]
    domain = argv[1] if len(argv) == 2 else "none"
    if domain not in ("none", "guest"):
        print(__doc__)
        return 2
    package_path = os.path.join(directory, "package.json")
    with open(package_path, encoding="utf-8") as handle:
        package = json.load(handle)

    index = package["index_binding"]
    provenance = package["provenance_id"]

    tape = {
        "schema_version": 1,
        # The scene name says what this is. A reader must not mistake it for a game capture.
        "scene": ("synthetic-fixture-from-package"
                  if domain == "none" else "synthetic-fixture-matcher-test"),
        "provenance_id": provenance,
        "image_sha256": "",
        "module_base": 0,
        "submission_id": 0,
        "packet_offset": 0,
        "packet_offset_known": False,
        "resources": [
            {
                "id": 1,
                "kind": "index_buffer",
                # The tape records the RHI invocation argument. For a synthetic fixture there is no
                # real UE4 object, so the value is zero and the domain says so. The identity check
                # then reports insufficient evidence rather than a fabricated match.
                "provenance": "ue4_object",
                "package_resource_ref": index["resource_ref"],
                "guest_base": index["guest_base"] if domain == "guest" else 0,
                "guest_size": index["guest_size"] if domain == "guest" else 0,
                "element_size": index["index_size"],
                # The matcher-test variant declares the guest-address domain so the comparison is
                # meaningful. The default variant declares the UE4-object domain, which is what a
                # real tape records and which cannot be compared against a guest address.
                "index_domain": "guest_address" if domain == "guest" else "ue4_object",
            }
        ],
        "operations": [
            {
                "kind": "SetViewport",
                "evidence": "runtime_confirmed",
                "sequence": 0,
                "command_capture_id": 0,
                "command_list": 0,
                "rhi_context": 0,
                "viewport": {
                    "min_x": 0, "min_y": 0, "min_z": 0,
                    "max_x": 640, "max_y": 640, "max_z": 1,
                },
                "color_target_count": 0,
                "color_targets": [],
                "depth_target": 0,
            },
            {
                "kind": "BindVertexBuffers",
                "evidence": "runtime_confirmed",
                "sequence": 1,
                "command_capture_id": 0,
                "command_list": 0,
                "rhi_context": 0,
                "viewport": {
                    "min_x": 0, "min_y": 0, "min_z": 0,
                    "max_x": 0, "max_y": 0, "max_z": 0,
                },
                "color_target_count": 0,
                "color_targets": [],
                "depth_target": 0,
                "vertex_resources": [0],
            },
            {
                "kind": "DrawIndexed",
                "evidence": "runtime_confirmed",
                "sequence": 2,
                "command_capture_id": 0,
                "command_list": 0,
                "rhi_context": 0,
                "viewport": {
                    "min_x": 0, "min_y": 0, "min_z": 0,
                    "max_x": 0, "max_y": 0, "max_z": 0,
                },
                "color_target_count": 1,
                "color_targets": [0],
                "depth_target": 0,
                "draw": {
                    "index_buffer_resource": 1,
                    "primitive_type": 0,
                    "base_vertex_index": index["base_vertex"],
                    "first_instance": index["first_instance"],
                    "num_vertices": 0,
                    "start_index": index["first_index"],
                    "num_primitives": index["index_count"] // 3,
                    "num_instances": index["instance_count"],
                },
                "derived": {
                    "index_count": index["index_count"],
                    "first_index": index["first_index"],
                    "vertex_offset": index["base_vertex"],
                    "instance_count": index["instance_count"],
                    "index_byte_offset": index["first_index"] * index["index_size"],
                    "valid": True,
                },
            },
        ],
        "unresolved": [],
    }

    out = os.path.join(directory, "tape.json")
    with open(out, "w", encoding="utf-8") as handle:
        json.dump(tape, handle, indent=1)
    print(f"wrote {out}")
    print("scene: synthetic-fixture-from-package")
    if domain == "guest":
        print("index identity: the package's guest address, for matcher testing only")
    else:
        print("index identity: not recorded (a synthetic fixture has no original UE4 object)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
