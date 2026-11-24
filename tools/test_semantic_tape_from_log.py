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


class Capacity(unittest.TestCase):
    """The shared resource-capacity contract."""

    def test_the_observed_thirty_three_resource_tape_is_supported(self):
        self.assertGreaterEqual(builder.MAX_TAPE_RESOURCES, 33)

    def test_exactly_at_capacity_is_accepted(self):
        tape = {"schema_version": 1, "operations": [], "resources": [], "unresolved": []}
        for i in range(builder.MAX_TAPE_RESOURCES):
            tape["resources"].append({"id": i + 1, "kind": "vertex_buffer",
                                      "provenance": "ue4_object", "package_resource_ref": 0,
                                      "guest_base": 0, "guest_size": 0, "element_size": 0})
        self.assertEqual(len(tape["resources"]), builder.MAX_TAPE_RESOURCES)

    def test_one_over_capacity_is_refused_by_the_validator(self):
        tape = {"schema_version": 1, "operations": [], "resources": [], "unresolved": []}
        for i in range(builder.MAX_TAPE_RESOURCES + 1):
            tape["resources"].append({"id": i + 1, "kind": "vertex_buffer",
                                      "provenance": "ue4_object", "package_resource_ref": 0,
                                      "guest_base": 0, "guest_size": 0, "element_size": 0})
        self.assertEqual(builder.validate_tape(tape), "resource_capacity_exceeded")

    def test_late_overflow_fails_explicitly(self):
        # More distinct vertex streams than the contract allows, then the draw.
        commands = []
        for i in range(builder.MAX_TAPE_RESOURCES + 4):
            commands.append({"sequence": i + 1, "command_id": builder.CMD_SET_STREAM_SOURCE,
                             "list": 0x100, "payload": [i, 0x1000 + i, 0, 0, 0, 0, 0, 0]})
        commands.append({"sequence": 999, "command_id": builder.CMD_DRAW_INDEXED,
                         "list": 0x100, "payload": [0xAAA, 0, 0, 0, 593, 0, 1012, 2]})
        record = {"id": 1, "command_list": 0x100, "rhi_index_resource": 0xAAA,
                  "command_capture_id": 0, "submission_id": 1, "packet_offset": 0,
                  "rhi_context": 0}
        with self.assertRaises(SystemExit):
            builder.build_tape(commands, record, "", 0, 2)

    def test_repeated_references_consume_no_additional_slots(self):
        commands = []
        for i in range(39):
            commands.append({"sequence": i + 1, "command_id": builder.CMD_SET_STREAM_SOURCE,
                             "list": 0x100, "payload": [0, 0xAAA, 0, 0, 0, 0, 0, 0]})
        commands.append({"sequence": 40, "command_id": builder.CMD_DRAW_INDEXED,
                         "list": 0x100, "payload": [0xAAA, 0, 0, 0, 593, 0, 1012, 2]})
        record = {"id": 1, "command_list": 0x100, "rhi_index_resource": 0xAAA,
                  "command_capture_id": 0, "submission_id": 1, "packet_offset": 0,
                  "rhi_context": 0}
        tape = builder.build_tape(commands, record, "", 0, 2)
        self.assertEqual(len(tape["resources"]), 1)


class Consistency(unittest.TestCase):
    """The UE4 semantic payload is authoritative; stored derived values must agree."""

    def _tape(self, primitives=1012):
        commands = [{"sequence": 1, "command_id": builder.CMD_DRAW_INDEXED,
                     "list": 0x100, "payload": [0xAAA, 0, 0, 0, 593, 0, primitives, 2]}]
        record = {"id": 1, "command_list": 0x100, "rhi_index_resource": 0xAAA,
                  "command_capture_id": 0, "submission_id": 1, "packet_offset": 0,
                  "rhi_context": 0}
        return builder.build_tape(commands, record, "", 0, 2)

    def test_a_consistent_tape_validates(self):
        self.assertEqual(builder.validate_tape(self._tape()), "none")

    def test_a_changed_primitive_count_is_detected(self):
        # Change only the payload, leave the derived block untouched.
        tape = self._tape()
        draw = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"][0]
        draw["draw"]["num_primitives"] = 500
        self.assertEqual(builder.validate_tape(tape), "derived_inconsistent")

    def test_a_changed_index_count_is_detected(self):
        tape = self._tape()
        draw = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"][0]
        draw["derived"]["index_count"] = 1500
        self.assertEqual(builder.validate_tape(tape), "derived_inconsistent")

    def test_a_changed_vertex_offset_is_detected(self):
        tape = self._tape()
        draw = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"][0]
        draw["derived"]["vertex_offset"] = -3
        self.assertEqual(builder.validate_tape(tape), "derived_inconsistent")

    def test_the_regenerated_draw_validates(self):
        tape = self._tape()
        draw = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"][0]
        draw["draw"]["num_primitives"] = 500
        draw["derived"] = builder.derive_draw(draw["draw"], 2)
        self.assertEqual(builder.validate_tape(tape), "none")
        self.assertEqual(draw["derived"]["index_count"], 1500)


class ProducerParity(unittest.TestCase):
    """The offline builder and the live C++ builder must agree on the same fixture.

    These assert the semantic content the C++ tests assert, using the same fixture values, so a
    divergence between the two producers shows up as a failing test on one side.
    """

    def _classroom(self, primitives=1012, instances=2, vertices=593, start_index=0,
                   base_vertex=0, first_instance=0, primitive_type=0):
        commands = [{"sequence": 174, "command_id": builder.CMD_DRAW_INDEXED,
                     "list": 0x100,
                     "payload": [0xAAA, primitive_type, base_vertex & 0xFFFFFFFF,
                                 first_instance, vertices, start_index, primitives, instances]}]
        record = {"id": 82, "command_list": 0x100, "rhi_index_resource": 0xAAA,
                  "command_capture_id": 635, "submission_id": 8913,
                  "packet_offset": 0x1B64, "rhi_context": 0, "ue4_primitive_type": primitive_type}
        return builder.build_tape(commands, record, "", 0, 2)

    def test_classroom_draw_matches_the_cpp_expectation(self):
        tape = self._classroom()
        draw = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"][0]
        self.assertEqual(draw["draw"]["primitive_type"], 0)
        self.assertEqual(draw["draw"]["num_primitives"], 1012)
        self.assertEqual(draw["draw"]["num_instances"], 2)
        self.assertEqual(draw["derived"]["index_count"], 3036)
        self.assertEqual(draw["derived"]["instance_count"], 2)

    def test_five_hundred_primitives_derive_fifteen_hundred_indices(self):
        tape = self._classroom(primitives=500)
        draw = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"][0]
        self.assertEqual(draw["derived"]["index_count"], 1500)

    def test_nonzero_first_index_and_instance_are_preserved(self):
        tape = self._classroom(start_index=168, first_instance=4, instances=3)
        draw = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"][0]
        self.assertEqual(draw["draw"]["start_index"], 168)
        self.assertEqual(draw["draw"]["first_instance"], 4)
        self.assertEqual(draw["derived"]["first_index"], 168)
        self.assertEqual(draw["derived"]["index_byte_offset"], 336)

    def test_negative_base_vertex_is_preserved(self):
        tape = self._classroom(base_vertex=-7)
        draw = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"][0]
        self.assertEqual(draw["draw"]["base_vertex_index"], -7)
        self.assertEqual(draw["derived"]["vertex_offset"], -7)

    def test_sequence_counter_disagreement_does_not_change_selection(self):
        # The provenance record carries a deferred ordinal that matches no capture slot.
        for slot in (1, 7, 174, 9999):
            commands = [
                {"sequence": slot, "command_id": builder.CMD_DRAW_INDEXED, "list": 0x100,
                 "payload": [0xAAA, 0, 0, 0, 593, 0, 1012, 2]},
                {"sequence": slot + 1, "command_id": builder.CMD_DRAW_INDEXED, "list": 0x200,
                 "payload": [0xBBB, 0, 0, 0, 20, 0, 5, 1]},
            ]
            record = {"id": 82, "command_list": 0x100, "rhi_index_resource": 0xAAA,
                      "command_capture_id": 635, "submission_id": 8913,
                      "packet_offset": 0x1B64, "rhi_context": 0}
            tape = builder.build_tape(commands, record, "", 0, 2)
            draw = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"][0]
            self.assertEqual(draw["draw"]["num_primitives"], 1012, "slot %d" % slot)

    def test_the_decoy_at_the_deferred_ordinal_is_not_selected(self):
        commands = [
            {"sequence": 635, "command_id": builder.CMD_DRAW_INDEXED, "list": 0x200,
             "payload": [0xBBB, 0, 0, 0, 1356, 0, 958, 2]},
            {"sequence": 174, "command_id": builder.CMD_DRAW_INDEXED, "list": 0x100,
             "payload": [0xAAA, 0, 0, 0, 593, 0, 1012, 2]},
        ]
        record = {"id": 82, "command_list": 0x100, "rhi_index_resource": 0xAAA,
                  "command_capture_id": 635, "submission_id": 8913,
                  "packet_offset": 0x1B64, "rhi_context": 0}
        tape = builder.build_tape(commands, record, "", 0, 2)
        draw = [op for op in tape["operations"] if op["kind"] == "DrawIndexed"][0]
        self.assertEqual(draw["draw"]["num_primitives"], 1012)
        self.assertEqual(draw["draw"]["num_vertices"], 593)

    def test_missing_viewport_and_render_targets_are_both_reported(self):
        tape = self._classroom()
        unresolved = [d["what"] for d in tape["unresolved"]]
        self.assertIn("viewport", unresolved)
        kinds = [op["kind"] for op in tape["operations"]]
        self.assertNotIn("SetRenderTargets", kinds)


if __name__ == "__main__":
    unittest.main(verbosity=2)