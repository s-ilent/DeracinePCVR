#!/usr/bin/env python3
"""Deterministic verifier for the A-040 provider-to-descriptor evidence.

Reconstructs every relationship from recorded primitive values. It does not trust stored booleans
or status strings: it recomputes the chain and fails when a record disagrees with its own fields.

Usage:
  python verify_a040_provider_descriptor.py <fixture.json> [--negative <mode>]

Negative modes corrupt one field of a record the relevant check examines:
  object, provider-target, provider-result, binder-link, adapter-slot, descriptor-source,
  descriptor-width, descriptor-fingerprint, writer-target, thread
"""
import json
import sys

# The two provider methods, from the original machine code.
#   0x0CF9E20 returns object+0x60 directly.
#   0x0CFFEC0 loads object+0x58 and tail-jumps through that object's own +0x38.
INLINE_PROVIDER_RVA = 0x0CF9E20
DELEGATE_PROVIDER_RVA = 0x0CFFEC0
INLINE_RESULT_OFFSET = 0x60
DELEGATE_OBJECT_OFFSET = 0x58
PROVIDER_RESULT_DESCRIPTOR = 0x18
ADAPTER_RVA = 0x0CC13B0
WRITER32_RVA = 0x02F62A40
MODULE_BASE = 0x800000000


def as_int(value):
    if isinstance(value, int):
        return value
    return int(value, 16)


def lowest_set_bit_group(pending):
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
        print("A-040 provider descriptor: %d/%d checks passed"
              % (len(self.results) - failed, len(self.results)))
        return failed


def apply_negative(chains, grouped, mode):
    """Corrupt one field of a record the relevant check examines."""
    if not chains:
        return chains
    c = chains[0]
    if mode == "object":
        c["selected_object"] = hex(as_int(c["selected_object"]) + 0x10)
    elif mode == "provider-target":
        c["provider_target"] = hex(as_int(c["provider_target"]) + 0x10)
    elif mode == "provider-result":
        c["provider_result"] = hex(as_int(c["provider_result"]) + 0x10)
    elif mode == "binder-link":
        c["binder_sequence"] = 999999
    elif mode == "adapter-slot":
        c["adapter_api_slot"] = (c["adapter_api_slot"] + 1) & 0xFF
    elif mode == "descriptor-source":
        c["descriptor_source"] = "0x0"
    elif mode == "descriptor-width":
        c["descriptor_bytes"] = 16
    elif mode == "descriptor-fingerprint":
        c["descriptor_fingerprint"] = "0000000000000000"
    elif mode == "writer-target":
        c["writer_target"] = hex(MODULE_BASE + 0x02F62740)
    elif mode == "thread":
        c["guest_thread"] = c["guest_thread"] + 1
    return chains


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    fixture = json.load(open(argv[1]))
    negative = None
    if "--negative" in argv:
        negative = argv[argv.index("--negative") + 1]

    report = Report()
    chains = fixture.get("provider_chains", [])
    grouped = fixture.get("grouped_records", [])
    if negative:
        chains = apply_negative(chains, grouped, negative)

    report.check("fixture declares its schema",
                 fixture.get("schema") == "deracine.gnm_provider_descriptor/1")
    report.check("fixture carries provenance", "provenance" in fixture)
    report.check("fixture has provider chains", len(chains) > 0, "%d" % len(chains))

    required = ("binder_sequence", "selected_object", "object_vptr", "provider_target",
                "provider_result", "descriptor_source", "adapter_address", "adapter_api_slot",
                "writer_target", "descriptor_bytes", "descriptor_fingerprint", "shader_hash")
    malformed = [i for i, c in enumerate(chains) if any(k not in c for k in required)]
    report.check("every chain has the required fields", not malformed,
                 "malformed: %s" % malformed[:5] if malformed else "")

    by_seq = {g["sequence"]: g for g in grouped}

    # --- 1. every chain links to a grouped binder record -----------------------------------
    unlinked = [c for c in chains if c["binder_sequence"] not in by_seq]
    report.check("every chain links to a grouped binder record", not unlinked,
                 "%d unlinked" % len(unlinked))

    # --- 2. the binder and the chain agree on the selected object --------------------------
    bad_object = []
    for c in chains:
        g = by_seq.get(c["binder_sequence"])
        if g is None:
            continue
        if as_int(g["selected_object"]) != as_int(c["selected_object"]):
            bad_object.append(c.get("sequence"))
        elif as_int(g["object_vptr"]) != as_int(c["object_vptr"]):
            bad_object.append(c.get("sequence"))
        elif as_int(g["provider_target"]) != as_int(c["provider_target"]):
            bad_object.append(c.get("sequence"))
    report.check("binder and chain agree on object, vptr and provider target", not bad_object,
                 "%d violations" % len(bad_object))

    # --- 3. the provider result is the one the resolved method produces --------------------
    # The inline method returns object+0x60. The delegating method returns its inner object's
    # inline result, and its inner object is read from object+0x58. A chain must satisfy exactly
    # one of these, matching the target it recorded.
    bad_result = []
    for c in chains:
        target_rva = as_int(c["provider_target"]) - MODULE_BASE
        obj = as_int(c["selected_object"])
        result = as_int(c["provider_result"])
        if target_rva == INLINE_PROVIDER_RVA:
            if result != obj + INLINE_RESULT_OFFSET:
                bad_result.append(("inline", c.get("sequence")))
        elif target_rva == DELEGATE_PROVIDER_RVA:
            # The inner object is not in the fixture, so the check is that the result is NOT the
            # selected object's own inline result: the delegating method cannot return that.
            if result == obj + INLINE_RESULT_OFFSET:
                bad_result.append(("delegate-returned-inline", c.get("sequence")))
        else:
            bad_result.append(("unknown-target", c.get("sequence")))
    report.check("provider result matches the resolved method's own return rule", not bad_result,
                 "%d violations" % len(bad_result))

    # --- 4. the adapter and writer are the verified addresses ------------------------------
    bad_adapter = [c for c in chains
                   if as_int(c["adapter_address"]) - MODULE_BASE != ADAPTER_RVA]
    report.check("every chain names the verified adapter", not bad_adapter,
                 "%d violations" % len(bad_adapter))
    bad_writer = [c for c in chains
                  if as_int(c["writer_target"]) - MODULE_BASE != WRITER32_RVA]
    report.check("every chain names the 32-byte resource writer", not bad_writer,
                 "%d violations" % len(bad_writer))

    # --- 5. descriptor source and width ---------------------------------------------------
    bad_source = [c for c in chains if as_int(c["descriptor_source"]) == 0]
    report.check("every chain has a non-null descriptor source", not bad_source)
    bad_width = [c for c in chains if c["descriptor_bytes"] != 32]
    report.check("every descriptor is 32 bytes", not bad_width,
                 "%d violations" % len(bad_width))
    bad_fp = [c for c in chains
              if not c.get("descriptor_fingerprint")
              or len(c["descriptor_fingerprint"]) != 16
              or c["descriptor_fingerprint"] in ("0000000000000000", "cbf29ce484222325")]
    report.check("every chain carries a real 16-hex-digit fingerprint", not bad_fp,
                 "%d violations" % len(bad_fp))

    # --- 6. logical API slot agreement ----------------------------------------------------
    bad_slot = []
    for c in chains:
        g = by_seq.get(c["binder_sequence"])
        if g is None:
            continue
        if g["packed_api_slot"] != c["adapter_api_slot"]:
            bad_slot.append(c.get("sequence"))
    report.check("adapter api slot == packed entry api slot", not bad_slot,
                 "%d violations" % len(bad_slot))

    # --- 7. thread association ------------------------------------------------------------
    bad_thread = []
    for c in chains:
        g = by_seq.get(c["binder_sequence"])
        if g is None:
            continue
        if g.get("guest_thread") is not None and g["guest_thread"] != c["guest_thread"]:
            bad_thread.append(c.get("sequence"))
    report.check("binder and chain agree on the guest thread", not bad_thread,
                 "%d violations" % len(bad_thread))

    # --- 8. grouped-record integrity ------------------------------------------------------
    bad_pending = [g for g in grouped
                   if as_int(g["pending"]) != (as_int(g["dirty_mask"]) & as_int(g["group_mask"]))]
    report.check("pending == dirty_mask & group_mask, recomputed", not bad_pending,
                 "%d violations" % len(bad_pending))
    bad_group = []
    for g in grouped:
        want = lowest_set_bit_group(as_int(g["pending"]))
        if want is None or want != g["group"]:
            bad_group.append(g.get("sequence"))
    report.check("group == lowest set bit of pending, recomputed", not bad_group,
                 "%d violations" % len(bad_group))
    bad_decode = []
    for g in grouped:
        pe = as_int(g["packed_entry"])
        if (pe & 0xFF) != g["packed_api_slot"] or ((pe >> 8) & 0xFFFF) != g["packed_object_index"] \
                or (pe >> 24) != g["packed_group"]:
            bad_decode.append(g.get("sequence"))
    report.check("packed entry decodes to its recorded fields, recomputed", not bad_decode,
                 "%d violations" % len(bad_decode))

    # --- 9. shader identity ---------------------------------------------------------------
    resolved = [c for c in chains if c.get("shader_hash_valid")]
    report.check("at least one chain resolved a shader hash", len(resolved) > 0,
                 "%d of %d" % (len(resolved), len(chains)))
    bad_hash = [c for c in resolved if as_int(c["shader_hash"]) == 0]
    report.check("every resolved chain has a non-zero shader hash", not bad_hash)

    # --- 10. one vptr, one provider target ------------------------------------------------
    vptr_to_target = {}
    conflict = []
    for c in chains:
        v = as_int(c["object_vptr"])
        t = as_int(c["provider_target"])
        if v in vptr_to_target and vptr_to_target[v] != t:
            conflict.append(hex(v))
        vptr_to_target[v] = t
    report.check("each vptr resolves to exactly one provider target", not conflict,
                 "%d classes" % len(vptr_to_target))

    # --- 11. provenance -------------------------------------------------------------------
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
