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
    candidates = [
        index
        for index, command in enumerate(commands)
        if command["command_id"] == CMD_DRAW_INDEXED
        and command["list"] == record["command_list"]
        and command["payload"][0] == record["rhi_index_resource"]
    ]
    if not candidates:
        raise SystemExit(
            "no deferred draw command matches provenance %d (list 0x%x, index resource 0x%x)"
            % (record["id"], record["command_list"], record["rhi_index_resource"])
        )
    if len(candidates) > 1:
        raise SystemExit(
            "provenance %d is ambiguous: %d deferred draw commands share list 0x%x and index "
            "resource 0x%x"
            % (record["id"], len(candidates), record["command_list"],
               record["rhi_index_resource"])
        )
    draw_index = candidates[0]

    draw_command = commands[draw_index]
    draw_list = draw_command["list"]

    # Resource ids are assigned in first-reference order, so the tape is deterministic.
    resource_by_guest = {}
    next_id = [1]

    def intern(guest, kind, element_size, provenance, package_ref=0):
        if guest == 0:
            return 0
        if guest in resource_by_guest:
            return resource_by_guest[guest]
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
