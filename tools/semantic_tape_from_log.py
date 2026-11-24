#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a semantic command tape from an existing shadPS4 capture log.

This is the offline path: it reads the command-capture records a previous run already wrote, so a
tape can be produced without launching the emulator again. The live path is
Core::SemanticTape::ExportTape in the emulator, which additionally reads the effective viewport
from the RHI context at draw entry.

The two paths emit the same schema. That is deliberate: a schema that only one implementation can
produce is not a contract.

What this tool can and cannot recover from a log
------------------------------------------------
The command capture records the deferred UE4 command payloads, the command list each was issued on,
and the provenance chain down to the submission and packet offset. It does NOT record the RHI
context for non-draw commands, and the PS4 RHI caches the viewport on the context rather than
carrying it in the command list. A draw whose viewport was set earlier therefore has no viewport
record in the log, and this tool reports that as an unresolved dependency instead of inventing one.

Usage:
    python tools/semantic_tape_from_log.py <shad_log.txt> <output-directory> [--draw N]
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path

SCHEMA_VERSION = 1

# Bounded tables. These mirror Core::SemanticTape's contract exactly: the live C++ exporter, this
# builder and the standalone reader all enforce the same limits, so a tape one producer accepts is
# one the others accept.
#
# 64 resources because the observed classroom draw needs 33 (one index buffer, one colour target,
# one depth target and thirty vertex-stream references), and the widest draw the recovered command
# family can describe needs at most 34. 64 is roughly twice that worst case.
MAX_TAPE_RESOURCES = 64
MAX_TAPE_OPERATIONS = 256
MAX_TAPE_UNRESOLVED = 16

# Command ids, matching Core::RhiCommandCapture.
CMD_DRAW_INDEXED = 1
CMD_DRAW = 2
CMD_SET_STREAM_SOURCE = 3
CMD_SET_RENDER_TARGETS = 4
CMD_SET_VIEWPORT = 5

# UE4 EPrimitiveType values this tape converts. These are the UE4 enum's own values, not the AMD
# hardware topology: UE4 begins PT_TriangleList = 0, while the GPU numbers triangle list as 4.
# SC-07R established that conflating the two is a defect; the captured classroom draw carries 0 and
# 1012 primitives x 3 = 3036 indices, exactly the downstream index count.
PRIM_TRIANGLE_LIST = 0
PRIM_TRIANGLE_STRIP = 1


def indices_per_primitive(primitive_type):
    if primitive_type == PRIM_TRIANGLE_LIST:
        return 3
    if primitive_type == PRIM_TRIANGLE_STRIP:
        return 1
    return 0


def derive_draw(payload, index_element_size):
    """Mirror of Core::SemanticTape::DeriveDraw. Kept in step with the C++ by the tests."""
    out = {
        "valid": False,
        "index_count": 0,
        "first_index": 0,
        "vertex_offset": 0,
        "instance_count": 0,
        "index_byte_offset": 0,
    }
    if index_element_size not in (2, 4):
        return out
    per = indices_per_primitive(payload["primitive_type"])
    if per == 0:
        return out
    if payload["primitive_type"] == PRIM_TRIANGLE_STRIP:
        if payload["num_primitives"] == 0:
            return out
        index_count = payload["num_primitives"] + 2
    else:
        if payload["num_primitives"] > 0xFFFFFFFF // per:
            return out
        index_count = payload["num_primitives"] * per
    out["valid"] = True
    out["index_count"] = index_count
    out["first_index"] = payload["start_index"]
    out["vertex_offset"] = payload["base_vertex_index"]
    out["instance_count"] = payload["num_instances"]
    out["index_byte_offset"] = payload["start_index"] * index_element_size
    return out


def as_float(bits):
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def as_signed(bits):
    value = bits & 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


CMD_RE = re.compile(
    r"DERACINE_CMD_CAPTURE sequence=(?P<seq>\d+) "
    r"native_thread=(?P<nt>\d+) guest_thread=(?P<gt>\d+) "
    r"list=(?P<list>0x[0-9a-fA-F]+) context=(?P<ctx>0x[0-9a-fA-F]+) "
    r"vtable_rva=(?P<vt>0x[0-9a-fA-F]+) command_id=(?P<cid>\d+) "
    r"payload_words=(?P<pw>\d+) payload=\[(?P<pl>[^\]]*)\] "
    r"rt_count=(?P<rtc>\d+) rt=\[(?P<rt>[^\]]*)\] depth=(?P<depth>0x[0-9a-fA-F]+)"
)

PROV_RE = re.compile(
    r"DERACINE_DRAW_PROVENANCE_RECORD id=(?P<id>\d+) rhi_sequence=(?P<rs>\d+) "
    r"command_id=(?P<cid>\d+) command_type=(?P<ct>\d+) "
    r"command_list=(?P<list>0x[0-9a-fA-F]+) rhi_context=(?P<ctx>0x[0-9a-fA-F]+) "
    r"thread=(?P<th>\d+) guest_thread=(?P<gt>\d+) cmdbuf=(?P<cmdbuf>0x[0-9a-fA-F]+) "
    r"packet_offset=(?P<off>0x[0-9a-fA-F]+) submission=(?P<sub>\d+) "
    r"copied_dcb=(?P<copied>0x[0-9a-fA-F]+) index_count=(?P<ic>\d+) "
    r"index_addr=(?P<ia>0x[0-9a-fA-F]+) gnm_type=(?P<gt2>\d+) "
    r"gnm_flags=(?P<gf>0x[0-9a-fA-F]+) ue4_prim=(?P<ue4>\d+) "
    r"rhi_index_resource=(?P<rir>0x[0-9a-fA-F]+) state=(?P<state>\d+) "
    r"child_draws=(?P<cd>\d+)"
)

INSTALL_RE = re.compile(
    r"DERACINE_CMD_CAPTURE_INSTALL .*image_sha256=(?P<sha>[0-9a-f]+) "
    r"module_base=(?P<base>0x[0-9a-fA-F]+)"
)


def parse_log(path):
    commands = []
    provenance = {}
    image_sha = ""
    module_base = 0
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            if "DERACINE_CMD_CAPTURE sequence=" in line:
                match = CMD_RE.search(line)
                if not match:
                    continue
                payload = [int(x, 16) for x in match.group("pl").split(",")]
                targets = [int(x, 16) for x in match.group("rt").split(",")]
                commands.append(
                    {
                        "sequence": int(match.group("seq")),
                        "native_thread": int(match.group("nt")),
                        "guest_thread": int(match.group("gt")),
                        "list": int(match.group("list"), 16),
                        "context": int(match.group("ctx"), 16),
                        "vtable_rva": int(match.group("vt"), 16),
                        "command_id": int(match.group("cid")),
                        "payload": payload,
                        "render_targets": targets,
                        "render_target_count": int(match.group("rtc")),
                        "depth_target": int(match.group("depth"), 16),
                    }
                )
            elif "DERACINE_DRAW_PROVENANCE_RECORD id=" in line:
                match = PROV_RE.search(line)
                if not match:
                    continue
                provenance[int(match.group("id"))] = {
                    "id": int(match.group("id")),
                    "rhi_sequence": int(match.group("rs")),
                    "command_capture_id": int(match.group("cid")),
                    "command_type": int(match.group("ct")),
                    "command_list": int(match.group("list"), 16),
                    "rhi_context": int(match.group("ctx"), 16),
                    "packet_offset": int(match.group("off"), 16),
                    "submission_id": int(match.group("sub")),
                    "index_count": int(match.group("ic")),
                    "index_addr": int(match.group("ia"), 16),
                    "ue4_primitive_type": int(match.group("ue4")),
                    "rhi_index_resource": int(match.group("rir"), 16),
                    "state": int(match.group("state")),
                    "child_draws": int(match.group("cd")),
                }
            elif "DERACINE_CMD_CAPTURE_INSTALL" in line:
                match = INSTALL_RE.search(line)
                if match:
                    image_sha = match.group("sha")
                    module_base = int(match.group("base"), 16)
    return commands, provenance, image_sha, module_base


def match_draw_command(commands, record):
    """Resolve a provenance record to exactly one captured draw command.

    Mirror of Core::SemanticTape::MatchDrawCommand. The live exporter and this builder call the
    same algorithm, so they select the same draw.

    The provenance record command_capture_id is the deferred-command ordinal the wrapper assigned
    on its own thread, NOT the capture slot sequence number. Those are two different counters, so
    comparing them selects the wrong command. The identity that is exact is the pair the RHI entry
    recorded: the command list the draw was issued on, and the index-buffer resource it was given,
    disambiguated by the semantic payload fields the record independently carries.

    Returns (outcome, index) where outcome is "matched", "no_candidate", "ambiguous" or
    "unusable_identity".
    """
    command_list = record.get("command_list", 0)
    index_resource = record.get("rhi_index_resource", 0)
    if command_list == 0 or index_resource == 0:
        return "unusable_identity", None

    primitive_type = record.get("ue4_primitive_type", 0)
    num_primitives = record.get("num_primitives", 0)
    num_instances = record.get("num_instances", 0)
    num_vertices = record.get("num_vertices", 0)
    start_index = record.get("start_index", 0)

    found = []
    for index, command in enumerate(commands):
        if command["command_id"] != CMD_DRAW_INDEXED:
            continue
        if command["list"] != command_list:
            continue
        payload = command["payload"]
        if payload[0] != index_resource:
            continue
        # Stage 2: the semantic fields the record knows must agree. A field the record does not
        # carry is zero and is not compared.
        if primitive_type != 0 and (payload[1] & 0xFFFFFFFF) != primitive_type:
            continue
        if num_primitives != 0 and (payload[6] & 0xFFFFFFFF) != num_primitives:
            continue
        if num_instances != 0 and (payload[7] & 0xFFFFFFFF) != num_instances:
            continue
        if num_vertices != 0 and (payload[4] & 0xFFFFFFFF) != num_vertices:
            continue
        if start_index != 0 and (payload[5] & 0xFFFFFFFF) != start_index:
            continue
        found.append(index)

    if len(found) == 1:
        return "matched", found[0]
    if len(found) > 1:
        return "ambiguous", None
    return "no_candidate", None


def validate_tape(tape):
    """Structural validation, mirroring Core::SemanticTape::ValidateTape.

    Returns a problem name, or "none". The producer calls this before writing, so a tape that fails
    here is a producer defect rather than a consumer problem.
    """
    if tape["schema_version"] != SCHEMA_VERSION:
        return "schema_mismatch"
    if len(tape["operations"]) > MAX_TAPE_OPERATIONS:
        return "operation_capacity_exceeded"
    if len(tape["resources"]) > MAX_TAPE_RESOURCES:
        return "resource_capacity_exceeded"
    if len(tape["unresolved"]) > MAX_TAPE_UNRESOLVED:
        return "resource_capacity_exceeded"

    ids = set()
    for resource in tape["resources"]:
        if resource["id"] == 0 or resource["id"] in ids:
            return "duplicate_resource_id"
        ids.add(resource["id"])

    by_id = {resource["id"]: resource for resource in tape["resources"]}
    for operation in tape["operations"]:
        kind = operation["kind"]
        if kind == "DrawIndexed":
            ref = operation["draw"]["index_buffer_resource"]
            if ref == 0:
                return "index_resource_missing"
            if ref not in by_id:
                return "dangling_resource_reference"
        if kind in ("BindIndexBuffer", "BindVertexBuffers"):
            ref = operation.get("resource", 0)
            if ref != 0 and ref not in by_id:
                return "dangling_resource_reference"
        if kind == "SetRenderTargets":
            for target in operation.get("color_targets", []):
                if target != 0 and target not in by_id:
                    return "dangling_resource_reference"
            depth = operation.get("depth_target", 0)
            if depth != 0 and depth not in by_id:
                return "dangling_resource_reference"

    draws = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"]
    if len(draws) > 1:
        return "operation_out_of_order"
    if not draws:
        return "no_draw"

    # The stored derived draw must agree with the semantic payload. The payload is authoritative.
    draw = draws[0]
    index = by_id.get(draw["draw"]["index_buffer_resource"])
    if index is not None:
        expected = derive_draw(draw["draw"], index["element_size"])
        stored = draw["derived"]
        if expected["valid"] != stored["valid"]:
            return "derived_inconsistent"
        if expected["valid"]:
            for key in ("index_count", "first_index", "vertex_offset", "instance_count",
                        "index_byte_offset"):
                if expected[key] != stored[key]:
                    return "derived_inconsistent"
    return "none"


def build_tape(commands, record, image_sha, module_base, index_element_size):
    """Build the tape for one provenance record."""
    tape = {
        "schema_version": SCHEMA_VERSION,
        "scene": "classroom-title",
        "provenance_id": record["id"],
        "image_sha256": image_sha,
        "module_base": module_base,
        "submission_id": record["submission_id"],
        "packet_offset": record["packet_offset"],
        "packet_offset_known": record["submission_id"] != 0,
        "resources": [],
        "operations": [],
        "unresolved": [],
    }

    # Locate the deferred draw command.
    #
    # The provenance record's command_capture_id is the deferred-command ordinal the wrapper
    # assigned on its own thread (Core::RhiCommandCapture::g_deferred_sequence), NOT the capture
    # slot's sequence number. The two are different counters, so matching on it selects the wrong
    # command. The identity that is exact here is the pair the RHI entry recorded: the command list
    # the draw was issued on, and the index-buffer resource the draw was given. Both come from the
    # same invocation, and a second draw sharing both would be ambiguous rather than silently
    # resolved.
    outcome, draw_index = match_draw_command(commands, record)
    if outcome != "matched":
        raise SystemExit(
            "the deferred draw command for provenance %d could not be resolved: %s"
            % (record["id"], outcome)
        )

    draw_command = commands[draw_index]
    draw_list = draw_command["list"]

    # Resource ids are assigned in first-reference order, so the tape is deterministic.
    #
    # A repeated reference to the same guest identity reuses its id and consumes no further slot.
    # Capacity exhaustion is a hard failure: returning a zero id would produce a tape that still
    # validates structurally while referencing an identity it does not carry.
    resource_by_guest = {}
    next_id = [1]

    def intern(guest, kind, element_size, provenance, package_ref=0):
        if guest == 0:
            return 0
        if guest in resource_by_guest:
            return resource_by_guest[guest]
        if len(tape["resources"]) >= MAX_TAPE_RESOURCES:
            raise SystemExit(
                "the tape exceeded its resource capacity of %d entries" % MAX_TAPE_RESOURCES
            )
        rid = next_id[0]
        next_id[0] += 1
        tape["resources"].append(
            {
                "id": rid,
                "kind": kind,
                "provenance": provenance,
                "package_resource_ref": package_ref,
                "guest_base": guest,
                "guest_size": 0,
                "element_size": element_size,
            }
        )
        resource_by_guest[guest] = rid
        return rid

    index_resource = intern(
        draw_command["payload"][0], "index_buffer", index_element_size, "ue4_object"
    )

    # State commands on the same list, in capture order, before the draw.
    for command in commands[:draw_index]:
        if command["list"] != draw_list:
            continue
        operation = {
            "kind": "Opaque",
            "evidence": "tentative",
            "sequence": command["sequence"],
            "command_capture_id": 0,
            "command_list": command["list"],
            "rhi_context": command.get("context", 0),
        }
        if command["command_id"] == CMD_SET_VIEWPORT:
            payload = command["payload"]
            operation.update(
                {
                    "kind": "SetViewport",
                    "evidence": "verified_target_binary",
                    "viewport": {
                        "min_x": float(as_signed(payload[0])),
                        "min_y": float(as_signed(payload[1])),
                        "min_z": as_float(payload[2]),
                        "max_x": float(as_signed(payload[3])),
                        "max_y": float(as_signed(payload[4])),
                        "max_z": as_float(payload[5]),
                    },
                }
            )
        elif command["command_id"] == CMD_SET_RENDER_TARGETS:
            targets = [
                intern(command["render_targets"][i], "render_target", 0, "ue4_object")
                for i in range(min(command["render_target_count"], 8))
            ]
            depth = intern(command["depth_target"], "depth_target", 0, "ue4_object")
            operation.update(
                {
                    "kind": "SetRenderTargets",
                    "evidence": "verified_target_binary",
                    "color_target_count": command["render_target_count"],
                    "color_targets": targets,
                    "depth_target": depth,
                }
            )
        elif command["command_id"] == CMD_SET_STREAM_SOURCE:
            operation.update(
                {
                    "kind": "BindVertexBuffers",
                    "evidence": "tentative",
                    "slot": command["payload"][0] & 0xFFFFFFFF,
                    "resource": intern(
                        command["payload"][1], "vertex_buffer", 0, "ue4_object"
                    ),
                    "note": "stream-source payload is an interface pointer, "
                            "not a resource base",
                }
            )
        else:
            operation["note"] = "command semantics not established"
        tape["operations"].append(operation)

    payload = draw_command["payload"]
    semantic = {
        "index_buffer_resource": index_resource,
        "primitive_type": payload[1] & 0xFFFFFFFF,
        "base_vertex_index": as_signed(payload[2]),
        "first_instance": payload[3] & 0xFFFFFFFF,
        "num_vertices": payload[4] & 0xFFFFFFFF,
        "start_index": payload[5] & 0xFFFFFFFF,
        "num_primitives": payload[6] & 0xFFFFFFFF,
        "num_instances": payload[7] & 0xFFFFFFFF,
    }
    derived = derive_draw(semantic, index_element_size)
    tape["operations"].append(
        {
            "kind": "DrawIndexed",
            "evidence": "verified_target_binary",
            "sequence": draw_command["sequence"],
            "command_capture_id": record["command_capture_id"],
            "command_list": draw_list,
            "rhi_context": record["rhi_context"],
            "draw": semantic,
            "derived": derived,
        }
    )

    # The viewport. The RHI caches it on the context and only emits a packet when it changes, and
    # the capture does not record the context for non-draw commands, so a draw whose viewport was
    # set earlier has no viewport record here. Report that rather than inventing one.
    has_viewport = any(op["kind"] == "SetViewport" for op in tape["operations"])
    if not has_viewport:
        tape["unresolved"].append(
            {
                "what": "viewport",
                "owner": "RHI context viewport cache (+0x80..+0xA8)",
                "reason": "no SetViewport command on this draw command list, and the capture "
                          "does not record the RHI context for non-draw commands; the effective "
                          "viewport must be read live at draw entry",
            }
        )

    tape["unresolved"].append(
        {
            "what": "graphics pipeline",
            "owner": "FRHICommandSetBoundShaderState",
            "reason": "the bound-shader-state command is not in the recovered command cluster",
        }
    )
    tape["unresolved"].append(
        {
            "what": "shader resources",
            "owner": "RHI shader resource bindings",
            "reason": "texture and sampler binding is SC-10 scope",
        }
    )

    # Validate the producer's own output before it is serialized. A tape that fails here is a
    # producer defect, not a consumer problem, so it is refused rather than written.
    problem = validate_tape(tape)
    if problem != "none":
        raise SystemExit("the built tape is not valid: %s" % problem)
    return tape


def main():
    parser = argparse.ArgumentParser(description="Build a semantic tape from a capture log")
    parser.add_argument("log", type=Path, help="shad_log.txt from a capture run")
    parser.add_argument("output", type=Path, help="directory to write tape.json into")
    parser.add_argument(
        "--draw",
        type=int,
        default=0,
        help="provenance record id to describe; 0 selects the last recorded draw",
    )
    parser.add_argument(
        "--index-element-size",
        type=int,
        default=2,
        choices=(2, 4),
        help="index element width, from the draw package index_binding.index_size",
    )
    args = parser.parse_args()

    if not args.log.is_file():
        raise SystemExit("log not found: %s" % args.log)
    if not args.output.is_dir():
        raise SystemExit("output directory does not exist: %s" % args.output)

    commands, provenance, image_sha, module_base = parse_log(args.log)
    if not commands:
        raise SystemExit("no command-capture records in this log")
    if not provenance:
        raise SystemExit("no provenance records in this log")

    record_id = args.draw
    if record_id == 0:
        recorded = [r for r in provenance.values() if r["state"] == 4]
        if not recorded:
            raise SystemExit("no provenance record reached the Recorded state")
        record_id = max(r["id"] for r in recorded)
    if record_id not in provenance:
        raise SystemExit("provenance record %d is not in this log" % record_id)

    tape = build_tape(
        commands, provenance[record_id], image_sha, module_base, args.index_element_size
    )

    out_path = args.output / "tape.json"
    out_path.write_text(json.dumps(tape, indent=2) + "\n", encoding="utf-8")

    typed = sum(1 for op in tape["operations"] if op["kind"] != "Opaque")
    opaque = len(tape["operations"]) - typed
    print(
        "tape: provenance=%d operations=%d typed=%d opaque=%d resources=%d unresolved=%d"
        % (
            tape["provenance_id"],
            len(tape["operations"]),
            typed,
            opaque,
            len(tape["resources"]),
            len(tape["unresolved"]),
        )
    )
    print("tape: wrote %s" % out_path)
    for dependency in tape["unresolved"]:
        print("  unresolved: %s (owner %s)" % (dependency["what"], dependency["owner"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())