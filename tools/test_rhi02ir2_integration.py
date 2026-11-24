#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""End-to-end integration tests for the RHI-02I-R2 provenance gate.

These invoke the actual replay executable against independently authored artifact sets. They do not
call the matcher directly: the point is to prove the production path is fail-closed, so every case
runs the real binary and checks its exit status.

The fixtures are authored independently. The tape carries an original UE4 object pointer, the
package a guest address, and the bridge the resolved relationship. No fixture copies one side into
another, so a match cannot be obtained by construction.

Usage: python tools/test_rhi02ir2_integration.py <replay-exe>
"""

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# The independently authored identities. Each is a distinct value so a copy between fixtures is
# visible as a match that should not have happened.
SESSION = 0x1111222233334444
OTHER_SESSION = 0x5555666677778888
EXECUTION = 0xAAAABBBBCCCCDDDD
PROVENANCE = 42
ORIGINAL_OBJECT = 0x101355BCE8
OTHER_OBJECT = 0x101355C000
GUEST_BASE = 0x2015E40000
OTHER_GUEST_BASE = 0x2015E50000
GUEST_SIZE = 6072
INDEX_COUNT = 3036


def fnv1a(data):
    h = 0xcbf29ce484222325
    for x in data:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def write_package(directory, *, session=SESSION, provenance=PROVENANCE,
                  guest_base=GUEST_BASE, index_count=INDEX_COUNT, schema=3):
    """A minimal but valid package: three vertices, two triangles, one index buffer."""
    verts = bytes(range(36))
    idx = struct.pack("<6H", 0, 1, 2, 2, 1, 0)
    blob = verts + idx
    (directory / "resources.bin").write_bytes(blob)
    package = {
        "schema_version": schema,
        "scene": "integration",
        "provenance_id": provenance,
        "session_id": session,
        "pipeline": {"pipeline_hash": 1, "vs_hash": 2, "ps_hash": 3,
                     "primitive_type": 4, "is_indexed": True,
                     "num_indices": index_count, "num_instances": 1},
        "resources": [
            {"id": 1, "requested_base": 0, "requested_size": 36, "clamped_base": 0,
             "clamped_size": 36, "content_hash": fnv1a(verts),
             "status": "host_content_verified", "byte_length": 36, "blob_offset": 0},
            {"id": 2, "requested_base": 0, "requested_size": 12, "clamped_base": 0,
             "clamped_size": 12, "content_hash": fnv1a(idx),
             "status": "host_content_verified", "byte_length": 12, "blob_offset": 36},
        ],
        "shader_buffers": [{"stage": 0, "resource_index": 0, "sharp_idx": 0, "vk_binding": 0,
                            "vk_offset": 0, "vk_range": 36, "buffer_id": 1,
                            "resource_ref": 1, "status": "host_content_verified"}],
        "vertex_bindings": [{"binding": 0, "host_buffer": 1, "host_offset": 0,
                             "guest_base": 0, "guest_size": 36, "stride": 12,
                             "input_rate": 0, "divisor": 0, "upload_sequence": 1,
                             "content_hash": 0, "resource_ref": 1, "semantic": 0,
                             "vk_format": 106, "attr_offset": 0, "element_count": 3,
                             "vs_hash": 2, "ps_hash": 3,
                             "status": "host_content_verified"}],
        "index_binding": {"host_buffer": 1, "host_offset": 0, "guest_base": guest_base,
                          "guest_size": 12, "index_size": 2, "index_count": index_count,
                          "first_index": 0, "base_vertex": 0, "instance_count": 1,
                          "first_instance": 0, "primitive_type": 4, "index_type": 0,
                          "swap_mode": 0, "upload_sequence": 1, "content_hash": 0,
                          "resource_ref": 2, "status": "host_content_verified"},
        "geometry": {"position_binding": 0, "position_components": 3,
                     "position_evidence": "integration", "bounds_min": [-1, -1, -1],
                     "bounds_max": [1, 1, 1], "is_three_dimensional": True,
                     "triangle_count": 2, "degenerate_triangles": 0,
                     "unique_referenced_vertices": 3, "nonfinite_vertices": 0},
        "readiness": {"shader_buffers_ready": True, "geometry_ready": True,
                      "texture_binding_ready": False, "texture_contents_ready": False,
                      "sampler_ready": False, "pipeline_state_ready": False,
                      "diagnostic_geometry_replay_ready": True,
                      "diagnostic_textured_replay_ready": False,
                      "original_material_ready": False,
                      "limitation": "integration fixture"},
    }
    (directory / "package.json").write_text(json.dumps(package, indent=2))


def write_tape(directory, *, session=SESSION, provenance=PROVENANCE,
               original_object=ORIGINAL_OBJECT, schema=2, index_domain="ue4_object"):
    """A tape whose index resource is the original UE4 object pointer."""
    tape = {
        "schema_version": schema,
        "scene": "integration",
        "provenance_id": provenance,
        "session_id": session,
        "image_sha256": "0" * 64,
        "module_base": 0x800000000,
        "submission_id": 1,
        "packet_offset": 0,
        "packet_offset_known": True,
        "resources": [{"id": 1, "kind": "index_buffer", "provenance": index_domain,
                       "package_resource_ref": 2, "guest_base": original_object,
                       "guest_size": 0, "element_size": 2}],
        "operations": [
            {"kind": "SetViewport", "evidence": "runtime_confirmed", "sequence": 1,
             "command_capture_id": 0, "command_list": 1, "rhi_context": 0,
             "viewport": {"min_x": 0.0, "min_y": 0.0, "min_z": 0.0,
                          "max_x": 640.0, "max_y": 640.0, "max_z": 1.0}},
            {"kind": "SetRenderTargets", "evidence": "verified_target_binary", "sequence": 2,
             "command_capture_id": 0, "command_list": 1, "rhi_context": 0,
             "color_target_count": 1, "color_targets": [2], "depth_target": 0},
            {"kind": "DrawIndexed", "evidence": "verified_target_binary", "sequence": 3,
             "command_capture_id": 3, "command_list": 1, "rhi_context": 0,
             "draw": {"index_buffer_resource": 1, "primitive_type": 0,
                      "base_vertex_index": 0, "first_instance": 0, "num_vertices": 3,
                      "start_index": 0, "num_primitives": 2, "num_instances": 1},
             "derived": {"valid": True, "index_count": 6, "first_index": 0,
                         "vertex_offset": 0, "instance_count": 1,
                         "index_byte_offset": 0}},
        ],
        "unresolved": [],
    }
    # The render target resource the tape references.
    tape["resources"].append({"id": 2, "kind": "render_target", "provenance": "ue4_object",
                              "package_resource_ref": 0, "guest_base": 0x1000000000,
                              "guest_size": 0, "element_size": 0})
    (directory / "tape.json").write_text(json.dumps(tape, indent=2))


def write_bridge(directory, *, session=SESSION, provenance=PROVENANCE,
                 original_object=ORIGINAL_OBJECT, selected=GUEST_BASE,
                 candidate_a=GUEST_BASE, candidate_b=OTHER_GUEST_BASE,
                 slot="primary_0x20", allocation_size=GUEST_SIZE,
                 allocation_offset=0, generation=None):
    """A bridge that resolves the original object to a guest allocation."""
    record = {
        "provenance_id": provenance,
        "session_id": session,
        "has_session_id": session != 0,
        "execution_id": EXECUTION,
        "has_execution_id": True,
        "original_rhi_object": original_object,
        "has_original_rhi_object": True,
        "resource_base": original_object - 0x68,
        "has_resource_base": True,
        "address_candidate_a": candidate_a,
        "has_address_candidate_a": True,
        "address_candidate_b": candidate_b,
        "has_address_candidate_b": True,
        "selected_slot": slot,
        "selected_guest_address": selected,
        "has_selected_guest_address": selected != 0,
        "index_element_width": 2,
        "has_index_element_width": True,
        "allocation_size": allocation_size,
        "has_allocation_size": allocation_size != 0,
        "allocation_offset": allocation_offset,
        "has_allocation_offset": True,
        "resource_generation": generation or 0,
        "has_resource_generation": generation is not None,
        "predicate_determinable": True,
        "address_evidence": "runtime_observed",
    }
    bridge = {"schema_version": 1, "records": [record]}
    (directory / "index_bridge.json").write_text(json.dumps(bridge, indent=2))


def write_manifest(directory, *, session=SESSION):
    manifest = {"schema_version": 1, "session_id": session,
                "note": "integration fixture"}
    (directory / "session.json").write_text(json.dumps(manifest, indent=2))


def build(directory, **overrides):
    """Writes a complete four-artifact set, applying per-artifact overrides."""
    write_package(directory, **overrides.get("package", {}))
    write_tape(directory, **overrides.get("tape", {}))
    write_bridge(directory, **overrides.get("bridge", {}))
    write_manifest(directory, **overrides.get("manifest", {}))


def run(replay, directory, *extra):
    """Runs the replay and returns (exit_code, combined_output)."""
    result = subprocess.run([replay, str(directory), "out", *extra],
                            capture_output=True, text=True, timeout=120)
    return result.returncode, (result.stdout or "") + (result.stderr or "")


class Runner:
    def __init__(self, replay):
        self.replay = replay
        self.passed = 0
        self.failed = 0
        self.failures = []

    def case(self, name, expect_success, **overrides):
        directory = Path(tempfile.mkdtemp(prefix="rhi02ir2_"))
        try:
            build(directory, **overrides)
            code, output = run(self.replay, directory)
            ok = (code == 0) if expect_success else (code != 0)
            if ok:
                self.passed += 1
                print(f"  ok    {name} (exit {code})")
            else:
                self.failed += 1
                self.failures.append(name)
                print(f"  FAIL  {name}: expected "
                      f"{'success' if expect_success else 'rejection'}, got exit {code}")
                for line in output.splitlines()[:6]:
                    print(f"          {line}")
        finally:
            shutil.rmtree(directory, ignore_errors=True)


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    replay = argv[0]
    if not os.path.exists(replay):
        print(f"replay executable not found: {replay}")
        return 2

    runner = Runner(replay)

    # A. A complete, consistent set executes under the synthetic label.
    runner.case("valid four-artifact set", True)

    # B. The bridge names a different original object.
    runner.case("bridge names a different original object", False,
                bridge={"original_object": OTHER_OBJECT})

    # C. Matching guest address but wrong original object.
    runner.case("matching address, wrong original object", False,
                bridge={"original_object": OTHER_OBJECT, "selected": GUEST_BASE})

    # D. Missing session identity on each artifact in turn.
    runner.case("missing tape session", False, tape={"session": 0})
    runner.case("missing package session", False, package={"session": 0})
    runner.case("missing bridge session", False, bridge={"session": 0})
    runner.case("missing manifest session", False, manifest={"session": 0})

    # E. Different session identities.
    runner.case("different tape session", False, tape={"session": OTHER_SESSION})
    runner.case("different package session", False, package={"session": OTHER_SESSION})
    runner.case("different bridge session", False, bridge={"session": OTHER_SESSION})

    # F. Same provenance reused across two sessions.
    runner.case("provenance reused across sessions", False,
                tape={"session": OTHER_SESSION}, bridge={"session": OTHER_SESSION})

    # G. The bridge is absent.
    runner.case("bridge absent", False, bridge={"selected": 0})

    # H. The predicate selected B while A is nonzero.
    runner.case("predicate selected B", True,
                bridge={"selected": OTHER_GUEST_BASE, "slot": "secondary_0x40"},
                package={"guest_base": OTHER_GUEST_BASE})

    # I. The package address disagrees with the resolved allocation.
    runner.case("incompatible resource address", False,
                package={"guest_base": OTHER_GUEST_BASE})

    # J. The draw reads outside the allocation.
    runner.case("invalid index bounds", False,
                bridge={"allocation_offset": GUEST_SIZE - 16})

    # K. A legacy tape without session metadata is not accepted as original.
    runner.case("legacy tape without session", False, tape={"schema": 1, "session": 0})

    # L. A legacy package without session metadata is not accepted as original.
    runner.case("legacy package without session", False, package={"schema": 2, "session": 0})

    # M. A generation the package cannot confirm is evidence-limited, not a match.
    runner.case("unconfirmed generation", False, bridge={"generation": 5})

    # N. The tape carries a guest address instead of an original object.
    runner.case("tape carries a guest address", False,
                tape={"original_object": GUEST_BASE, "index_domain": "guest_address"})

    print()
    print(f"{runner.passed}/{runner.passed + runner.failed} integration cases passed")
    if runner.failures:
        print("failed: " + ", ".join(runner.failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
