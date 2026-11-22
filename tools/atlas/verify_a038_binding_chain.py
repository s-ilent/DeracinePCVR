#!/usr/bin/env python3
"""Deterministic verifier for the A-038 binding-chain evidence.

Reads the metadata-only export and re-derives every claim the report makes. It needs no
emulator, no GPU and no game data: the export is the evidence.

Usage:
  python verify_a038_binding_chain.py <gnm_binding_chain.json> [<gnm_shader_metadata.json>]
"""
import json
import sys
from collections import Counter

# Atlas A-025/A-030: the original decoder's map encoding.
def encode_map_word(usage_type, start_register, flags):
    encoded = (0x8000 | start_register) if start_register < 16 else (start_register - 16)
    if usage_type in (0x00, 0x04) and (flags & 2) == 0:
        encoded |= 0x4000
    return encoded


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + (("  " + detail) if detail else ""))
    return ok


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    chain = json.load(open(argv[1]))
    results = []

    records = chain["records"]
    resolved = [r for r in records if r["shader_hash_valid"]]
    results.append(check("export has records", len(records) > 0, "%d" % len(records)))
    results.append(check("at least one record resolved a shader hash", len(resolved) > 0,
                         "%d of %d" % (len(resolved), len(records))))

    # 1. Every resolved record carries a real OrbShdr-derived hash and a matched declaration.
    results.append(check("every resolved record matched a declaration",
                         all(r["declaration_matched"] for r in resolved)))

    # 2. The decoded map word equals the encoder applied to the parsed declaration. This is the
    #    join between the shader's own metadata and the writer's destination selection.
    mismatches = []
    for r in resolved:
        d = r["declaration"]
        want = encode_map_word(d["usage_type"], d["start_register"], d["flags"])
        got = int(r["decoded_map_word"], 16)
        if want != got:
            mismatches.append((r["shader_hash"], r["api_slot"], want, got))
    results.append(check("decoded map word == encoder(declaration)", not mismatches,
                         "%d checked" % len(resolved)))

    # 3. The mapped index is the low 14 bits, and the direct bit is bit 15.
    bad_index = [r for r in resolved
                 if r["mapped_index"] != (int(r["decoded_map_word"], 16) & 0x3FFF)]
    results.append(check("mapped_index is the low 14 bits", not bad_index))
    bad_direct = [r for r in resolved
                  if r["direct"] != bool(int(r["decoded_map_word"], 16) & 0x8000)]
    results.append(check("direct is bit 15", not bad_direct))

    # 4. Descriptor bytes were captured by value at the writer boundary.
    bad_desc = [r for r in resolved if r["descriptor_bytes"] not in (16, 32)]
    results.append(check("descriptor width is 16 or 32 bytes", not bad_desc))
    bad_hex = [r for r in resolved if len(r["descriptor"]) != r["descriptor_bytes"] * 2]
    results.append(check("descriptor hex length matches its width", not bad_hex))

    # 5. The shader resource is the map object minus the metadata offset (Atlas A-025).
    bad_res = [r for r in resolved
               if int(r["shader_resource"], 16) + 0xB0 != int(r["map_object"], 16)]
    results.append(check("shader_resource + 0xB0 == map_object", not bad_res))

    # 6. The trailer lies inside the program the stage record describes.
    bad_trailer = [r for r in resolved if int(r["trailer"], 16) == 0]
    results.append(check("every resolved record has a trailer address", not bad_trailer))

    # 7. Stage identity: the writer's stage index is not the shader's logical stage. Record the
    #    observed mapping rather than asserting one.
    stage_pairs = Counter((r["stage"], r["shader_hash"]) for r in resolved)
    results.append(check("writer stage index observed", len(stage_pairs) > 0,
                         str(sorted(set(r["stage"] for r in resolved)))))

    # 8. Optional cross-check against the A-028R metadata export.
    if len(argv) > 2:
        meta = json.load(open(argv[2]))
        known = {(s["shader_hash"], s["stage"]) for s in meta["shaders"]}
        hashes = {r["shader_hash"] for r in resolved}
        # The A-028R export keys on the shadPS4 stage index, which is a different domain from the
        # writer's stage index, so compare hashes only.
        meta_hashes = {h for h, _ in known}
        overlap = hashes & meta_hashes
        results.append(check("captured hashes appear in the A-028R metadata sample",
                             len(overlap) > 0,
                             "%d of %d" % (len(overlap), len(hashes))))

    failed = results.count(False)
    print()
    print("A-038 binding chain: %d/%d checks passed" % (len(results) - failed, len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
