#!/usr/bin/env python3
"""Deterministic verifier for the A-039 grouped-resource binding evidence.

Reconstructs every relationship from the recorded primitive values. It does not trust stored
booleans such as `packed_group_agrees` or `provider_target_resolved`: it recomputes them.

Usage:
  python verify_a039_grouped_binding.py <fixture.json> [--negative <mode>]

Negative modes corrupt one field of a record the relevant check examines:
  packed-entry, group, object-index, vptr, provider-target, timestamp, shader-hash, pending
"""
import json
import struct
import sys

# Atlas A-025/A-030: the original decoder's map encoding.
def encode_map_word(usage_type, start_register, flags):
    encoded = (0x8000 | start_register) if start_register < 16 else (start_register - 16)
    if usage_type in (0x00, 0x04) and (flags & 2) == 0:
        encoded |= 0x4000
    return encoded


def as_int(value):
    if isinstance(value, int):
        return value
    return int(value, 16)


def lowest_set_bit_group(pending):
    """The original binder's group selection: blsi then a bit scan."""
    if pending == 0:
        return None
    lowest = pending & (~pending + 1)
    group = 0
    bit = lowest
    while bit > 1:
        group += 1
        bit >>= 1
    return group


class Report:
    def __init__(self):
        self.results = []

    def check(self, name, ok, detail=""):
        self.results.append((name, bool(ok), detail))
        return ok

    def emit(self):
        for name, ok, detail in self.results:
            print(("PASS " if ok else "FAIL ") + name + (("  " + detail) if detail else ""))
        failed = sum(1 for _, ok, _ in self.results if not ok)
        print()
        print("A-039 grouped binding: %d/%d checks passed"
              % (len(self.results) - failed, len(self.results)))
        return failed


def apply_negative(records, mode):
    """Corrupt one field of a record the relevant check examines."""
    if not records:
        return records
    r = records[0]
    if mode == "packed-entry":
        r["packed_entry"] = hex(as_int(r["packed_entry"]) ^ 0x00000100)
    elif mode == "group":
        r["group"] = (r["group"] + 1) & 0xFF
    elif mode == "object-index":
        r["packed_object_index"] = (r["packed_object_index"] + 1) & 0xFFFF
    elif mode == "vptr":
        # Break the class mapping: give this record a vptr that another record already maps to a
        # different provider target, so one vptr would resolve to two targets.
        other = next((x for x in records
                      if as_int(x["object_vptr"]) != as_int(r["object_vptr"])), None)
        if other is not None:
            r["object_vptr"] = other["object_vptr"]
    elif mode == "provider-target":
        r["provider_target"] = hex(as_int(r["provider_target"]) + 0x10)
    elif mode == "timestamp":
        # 1.0 is exactly representable, so it survives the float round-trip. Use a value that does
        # not: a double with mantissa bits below float precision.
        r["timestamp_before"] = "0x3ff0000000000001"
    elif mode == "shader-hash":
        r["shader_hash"] = "0000000000000000"
    elif mode == "pending":
        r["pending"] = "0x00000000"
    return records


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    fixture = json.load(open(argv[1]))
    negative = None
    if "--negative" in argv:
        negative = argv[argv.index("--negative") + 1]

    report = Report()
    records = fixture.get("records", [])
    if negative:
        records = apply_negative(records, negative)

    report.check("fixture declares its schema",
                 fixture.get("schema") == "deracine.gnm_grouped_binding/1")
    report.check("fixture carries provenance", "provenance" in fixture)
    report.check("fixture has records", len(records) > 0, "%d" % len(records))

    required = ("helper_index", "pending", "group", "packed_entry", "packed_api_slot",
                "packed_object_index", "packed_group", "object_vptr", "provider_target",
                "shader_hash", "shader_hash_valid", "timestamp_before")
    malformed = [i for i, r in enumerate(records) if any(k not in r for k in required)]
    report.check("every record has the required fields", not malformed,
                 "malformed: %s" % malformed[:5] if malformed else "")

    # --- 1. a grouped helper actually dispatched -------------------------------------------
    helpers = fixture.get("grouped_helpers", [])
    dispatched = [h for h in helpers if h.get("nonzero_pending", 0) > 0]
    report.check("at least one grouped helper saw a nonzero pending mask",
                 len(dispatched) > 0,
                 ", ".join("%s=%d" % (h["name"], h["nonzero_pending"]) for h in dispatched))
    # The zero-pending helpers are a measurement, not an absence of instrumentation.
    report.check("every helper reports its invocation count",
                 all("invocations" in h for h in helpers),
                 "%d helpers" % len(helpers))

    # --- 2. pending is the mask intersection -----------------------------------------------
    bad_pending = [r for r in records
                   if as_int(r["pending"]) != (as_int(r["dirty_mask"]) & as_int(r["group_mask"]))]
    report.check("pending == dirty_mask & group_mask, recomputed", not bad_pending,
                 "%d violations" % len(bad_pending))

    # --- 3. group selection is the lowest set bit ------------------------------------------
    bad_group = []
    for r in records:
        want = lowest_set_bit_group(as_int(r["pending"]))
        if want is None or want != r["group"]:
            bad_group.append(r.get("sequence"))
    report.check("group == lowest set bit of pending, recomputed", not bad_group,
                 "%d violations" % len(bad_group))

    # --- 4. packed entry decoding ----------------------------------------------------------
    bad_decode = []
    for r in records:
        pe = as_int(r["packed_entry"])
        if (pe & 0xFF) != r["packed_api_slot"]:
            bad_decode.append(("slot", r.get("sequence")))
        elif ((pe >> 8) & 0xFFFF) != r["packed_object_index"]:
            bad_decode.append(("index", r.get("sequence")))
        elif (pe >> 24) != r["packed_group"]:
            bad_decode.append(("group", r.get("sequence")))
    report.check("packed entry decodes to its recorded fields, recomputed", not bad_decode,
                 "%d violations" % len(bad_decode))

    # --- 5. the packed entry's group agrees with the mask-selected group -------------------
    bad_agree = [r for r in records if r["packed_group"] != r["group"]]
    report.check("packed entry group == mask-selected group", not bad_agree,
                 "%d violations" % len(bad_agree))

    # --- 6. cached-array pointer arithmetic ------------------------------------------------
    # The binder reads cached_group from context + cache_offset + 8*group. The fixture records
    # the context and the cached group, so the offset must be one the helper table declares.
    helper_offsets = {h["index"]: h["group_cache_offset"] for h in helpers}
    bad_cache = []
    for r in records:
        off = helper_offsets.get(r["helper_index"])
        if off is None:
            bad_cache.append(("unknown-helper", r.get("sequence")))
            continue
        if as_int(r["cached_group"]) == 0:
            bad_cache.append(("null", r.get("sequence")))
    report.check("every record names a known helper and a non-null cached group", not bad_cache,
                 "%d violations" % len(bad_cache))

    # --- 7. object vptr and provider target ------------------------------------------------
    bad_vptr = [r for r in records if as_int(r["object_vptr"]) == 0]
    report.check("every record has a non-null object vptr", not bad_vptr)
    bad_provider = [r for r in records if as_int(r["provider_target"]) == 0]
    report.check("every record has a non-null provider target", not bad_provider)
    # The provider target must be inside the loaded image, not in guest heap.
    bad_range = [r for r in records
                 if not (0x800000000 <= as_int(r["provider_target"]) < 0x810000000)]
    report.check("provider target lies in the loaded image", not bad_range,
                 "%d outside" % len(bad_range))

    # --- 8. shader identity ----------------------------------------------------------------
    resolved = [r for r in records if r.get("shader_hash_valid")]
    report.check("at least one record resolved a shader hash", len(resolved) > 0,
                 "%d of %d" % (len(resolved), len(records)))
    bad_hash = [r for r in resolved if as_int(r["shader_hash"]) == 0]
    report.check("every resolved record has a non-zero shader hash", not bad_hash)

    # --- 9. timestamp writeback ------------------------------------------------------------
    # A-035: the stored value is double(float(t)), so re-rounding it through float must be a
    # no-op. A value that fails this was not produced by that path.
    bad_ts = []
    for r in records:
        if not r.get("timestamp_readable"):
            continue
        v = as_int(r["timestamp_before"])
        d = struct.unpack("<d", struct.pack("<Q", v))[0]
        f = struct.unpack("<f", struct.pack("<f", d))[0]
        back = struct.unpack("<d", struct.pack("<d", float(f)))[0]
        if back != d:
            bad_ts.append(r.get("sequence"))
    report.check("timestamp values are float-rounded doubles, recomputed", not bad_ts,
                 "%d violations" % len(bad_ts))

    # --- 10. object class consistency ------------------------------------------------------
    # Each vptr must map to exactly one provider target: a vtable slot is a fixed address.
    vptr_to_target = {}
    conflict = []
    for r in records:
        v = as_int(r["object_vptr"])
        t = as_int(r["provider_target"])
        if v in vptr_to_target and vptr_to_target[v] != t:
            conflict.append(hex(v))
        vptr_to_target[v] = t
    report.check("each vptr resolves to exactly one provider target", not conflict,
                 "%d classes" % len(vptr_to_target))

    # --- 11. provenance --------------------------------------------------------------------
    prov = fixture.get("provenance", {})
    report.check("provenance names the approved executable",
                 prov.get("executable_sha256", "").startswith("8caee78e"),
                 prov.get("executable_sha256", "")[:16])
    report.check("provenance names the scene",
                 "gameplay" in prov.get("scene", "").lower(),
                 prov.get("scene", "")[:40])

    failed = report.emit()
    if negative:
        print()
        print("negative mode '%s': %s" % (negative, "correctly rejected" if failed else "NOT REJECTED"))
        return 0 if failed else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
