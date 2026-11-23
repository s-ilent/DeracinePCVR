#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for the offline semantic tape builder.

These exercise the production functions in semantic_tape_from_log.py, not a reimplementation of
their formulas. The draw conversion is checked against the real classroom payload and against the
index count the downstream GNM draw reported for the same execution.

Run: python tools/test_semantic_tape_from_log.py
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import semantic_tape_from_log as builder


def classroom_payload():
    """The real classroom draw payload, as the deferred command carried it."""
    return {
        "index_buffer_resource": 1,
        "primitive_type": builder.PRIM_TRIANGLE_LIST,
        "base_vertex_index": 0,
        "first_instance": 0,
        "num_vertices": 593,
        "start_index": 0,
        "num_primitives": 1012,
        "num_instances": 2,
    }


class DrawConversion(unittest.TestCase):
    def test_classroom_conversion_matches_the_downstream_index_count(self):
        # 1012 triangle-list primitives consume 3036 indices, which is exactly what the
        # downstream GNM draw for the same execution reported.
        derived = builder.derive_draw(classroom_payload(), 2)
        self.assertTrue(derived["valid"])
        self.assertEqual(derived["index_count"], 3036)
        self.assertEqual(derived["instance_count"], 2)

    def test_ue4_triangle_list_is_zero_not_the_gpu_topology(self):
        # UE4 begins PT_TriangleList = 0; the GPU numbers triangle list as 4. Conflating them is
        # the defect SC-07R identified.
        self.assertEqual(builder.PRIM_TRIANGLE_LIST, 0)
        self.assertEqual(builder.PRIM_TRIANGLE_STRIP, 1)
        self.assertEqual(builder.indices_per_primitive(0), 3)
        self.assertEqual(builder.indices_per_primitive(1), 1)

    def test_triangle_strip_consumes_two_more_indices_than_primitives(self):
        payload = classroom_payload()
        payload["primitive_type"] = builder.PRIM_TRIANGLE_STRIP
        payload["num_primitives"] = 10
        derived = builder.derive_draw(payload, 2)
        self.assertTrue(derived["valid"])
        self.assertEqual(derived["index_count"], 12)

    def test_empty_triangle_strip_is_refused(self):
        payload = classroom_payload()
        payload["primitive_type"] = builder.PRIM_TRIANGLE_STRIP
        payload["num_primitives"] = 0
        self.assertFalse(builder.derive_draw(payload, 2)["valid"])

    def test_start_index_becomes_an_index_offset_not_a_byte_offset(self):
        payload = classroom_payload()
        payload["start_index"] = 168
        derived = builder.derive_draw(payload, 2)
        self.assertTrue(derived["valid"])
        self.assertEqual(derived["first_index"], 168)
        self.assertEqual(derived["index_byte_offset"], 336)
        self.assertEqual(derived["index_count"], 3036)

    def test_start_index_byte_offset_scales_with_element_width(self):
        payload = classroom_payload()
        payload["start_index"] = 100
        self.assertEqual(builder.derive_draw(payload, 4)["index_byte_offset"], 400)
        self.assertEqual(builder.derive_draw(payload, 2)["index_byte_offset"], 200)

    def test_negative_base_vertex_is_preserved_as_signed(self):
        payload = classroom_payload()
        payload["base_vertex_index"] = -7
        self.assertEqual(builder.derive_draw(payload, 2)["vertex_offset"], -7)

    def test_unknown_index_width_is_refused(self):
        for width in (0, 1, 3, 8):
            self.assertFalse(builder.derive_draw(classroom_payload(), width)["valid"])

    def test_unknown_topology_is_refused(self):
        payload = classroom_payload()
        payload["primitive_type"] = 2  # PT_PointList
        self.assertFalse(builder.derive_draw(payload, 2)["valid"])

    def test_primitive_count_overflow_is_refused(self):
        payload = classroom_payload()
        payload["num_primitives"] = 0xFFFFFFFF
        self.assertFalse(builder.derive_draw(payload, 2)["valid"])


class DrawResolution(unittest.TestCase):
    """The identity rules the builder uses to find the draw command."""

    def _commands(self):
        # Two draws on the same list with different index resources, plus one on another list.
        return [
            {"sequence": 1, "command_id": builder.CMD_DRAW_INDEXED, "list": 0x100,
             "payload": [0xAAA, 0, 0, 0, 10, 0, 3, 1]},
            {"sequence": 2, "command_id": builder.CMD_DRAW_INDEXED, "list": 0x100,
             "payload": [0xBBB, 0, 0, 0, 593, 0, 1012, 2]},
            {"sequence": 3, "command_id": builder.CMD_DRAW_INDEXED, "list": 0x200,
             "payload": [0xBBB, 0, 0, 0, 20, 0, 5, 1]},
        ]

    def _record(self, list_id, resource):
        return {"id": 1, "command_list": list_id, "rhi_index_resource": resource,
                "command_capture_id": 0, "submission_id": 1, "packet_offset": 0,
                "rhi_context": 0}

    def test_resolves_by_list_and_index_resource(self):
        tape = builder.build_tape(self._commands(), self._record(0x100, 0xBBB), "", 0, 2)
        draw = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"][0]
        self.assertEqual(draw["sequence"], 2)
        self.assertEqual(draw["draw"]["num_primitives"], 1012)

    def test_a_draw_on_another_list_is_not_selected(self):
        tape = builder.build_tape(self._commands(), self._record(0x200, 0xBBB), "", 0, 2)
        draw = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"][0]
        self.assertEqual(draw["sequence"], 3)

    def test_an_absent_draw_is_refused_rather_than_guessed(self):
        with self.assertRaises(SystemExit):
            builder.build_tape(self._commands(), self._record(0x300, 0xBBB), "", 0, 2)

    def test_an_ambiguous_match_is_refused(self):
        commands = self._commands()
        commands.append({"sequence": 4, "command_id": builder.CMD_DRAW_INDEXED, "list": 0x100,
                         "payload": [0xBBB, 0, 0, 0, 5, 0, 1, 1]})
        with self.assertRaises(SystemExit):
            builder.build_tape(commands, self._record(0x100, 0xBBB), "", 0, 2)


class TapeShape(unittest.TestCase):
    def test_missing_viewport_is_reported_not_invented(self):
        commands = [{"sequence": 1, "command_id": builder.CMD_DRAW_INDEXED, "list": 0x100,
                     "payload": [0xAAA, 0, 0, 0, 593, 0, 1012, 2]}]
        record = {"id": 1, "command_list": 0x100, "rhi_index_resource": 0xAAA,
                  "command_capture_id": 0, "submission_id": 1, "packet_offset": 0,
                  "rhi_context": 0}
        tape = builder.build_tape(commands, record, "", 0, 2)
        kinds = [op["kind"] for op in tape["operations"]]
        self.assertNotIn("SetViewport", kinds)
        unresolved = [d["what"] for d in tape["unresolved"]]
        self.assertIn("viewport", unresolved)

    def test_state_from_another_list_is_excluded(self):
        commands = [
            {"sequence": 1, "command_id": builder.CMD_SET_VIEWPORT, "list": 0x999,
             "payload": [0, 0, 0, 1024, 1024, 0x3F800000], "context": 0,
             "render_targets": [0] * 8, "render_target_count": 0, "depth_target": 0},
            {"sequence": 2, "command_id": builder.CMD_DRAW_INDEXED, "list": 0x100,
             "payload": [0xAAA, 0, 0, 0, 593, 0, 1012, 2]},
        ]
        record = {"id": 1, "command_list": 0x100, "rhi_index_resource": 0xAAA,
                  "command_capture_id": 0, "submission_id": 1, "packet_offset": 0,
                  "rhi_context": 0}
        tape = builder.build_tape(commands, record, "", 0, 2)
        kinds = [op["kind"] for op in tape["operations"]]
        self.assertNotIn("SetViewport", kinds)

    def test_packet_offset_zero_is_a_known_offset(self):
        commands = [{"sequence": 1, "command_id": builder.CMD_DRAW_INDEXED, "list": 0x100,
                     "payload": [0xAAA, 0, 0, 0, 593, 0, 1012, 2]}]
        record = {"id": 1, "command_list": 0x100, "rhi_index_resource": 0xAAA,
                  "command_capture_id": 0, "submission_id": 7, "packet_offset": 0,
                  "rhi_context": 0}
        tape = builder.build_tape(commands, record, "", 0, 2)
        self.assertEqual(tape["packet_offset"], 0)
        self.assertTrue(tape["packet_offset_known"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
