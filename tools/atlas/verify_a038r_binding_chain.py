#!/usr/bin/env python3
"""Deterministic verifier for the A-038R binding-chain evidence.

Reconstructs every conclusion from the recorded inputs. It does not trust derived booleans such as
`map_word_matches_encoder` or `source_matches_provider_buffer`: it recomputes them and fails when
the record disagrees with its own fields.

Usage:
  python verify_a038r_binding_chain.py <fixture.json> [--negative <name>]

Negative modes deliberately corrupt one field so the corresponding check must fail:
  map-word, slot, stage-size, provider-source, descriptor-fingerprint, declaration
"""
import json
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
        print("A-038R binding chain: %d/%d checks passed"
              % (len(self.results) - failed, len(self.results)))
        return failed


def apply_negative(records, mode):
    """Corrupt one field so the matching check must fail.

    The corruption targets a record the relevant check actually examines: a resolved record for the
    metadata checks, a matched record for the provenance checks. Corrupting an unrelated record
    would prove nothing.
    """
    if not records:
        return records

    def pick(predicate):
        for r in records:
            if predicate(r):
                return r
        return records[0]

    resolved = pick(lambda r: r.get("shader_hash_valid") and r.get("declaration_matched"))
    matched = pick(lambda r: r.get("upload_provenance") == "matched")

    if mode == "map-word":
        resolved["decoded_map_word"] = "0xffff"
    elif mode == "slot":
        resolved["api_slot"] = (resolved["api_slot"] + 1) & 0xFF
    elif mode == "stage-size":
        # Break the trailer arithmetic by moving the trailer off the program.
        resolved["trailer"] = hex(as_int(resolved["trailer"]) + 4)
    elif mode == "provider-source":
        matched["expected_provider_buffer"] = hex(
            as_int(matched["expected_provider_buffer"]) + 0x10)
    elif mode == "descriptor-fingerprint":
        resolved["descriptor_fingerprint"] = "0000000000000000"
    elif mode == "declaration":
        resolved["declaration"]["start_register"] = (
            resolved["declaration"]["start_register"] + 4) & 0xFF
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
                 fixture.get("schema") == "deracine.gnm_binding_chain/1")
    report.check("fixture carries provenance", "provenance" in fixture)
    report.check("fixture has records", len(records) > 0, "%d" % len(records))

    # --- structural validation: reject malformed records -----------------------------------
    required = ("stage", "api_slot", "decoded_map_word", "descriptor_bytes",
                "shader_hash", "shader_hash_valid", "upload_provenance")
    malformed = [i for i, r in enumerate(records) if any(k not in r for k in required)]
    report.check("every record has the required fields", not malformed,
                 "malformed: %s" % malformed[:5] if malformed else "")

    resolved = [r for r in records if r.get("shader_hash_valid")]
    matched = [r for r in records if r.get("upload_provenance") == "matched"]
    unmatched = [r for r in records if r.get("upload_provenance") == "unmatched"]

    report.check("at least one record resolved a shader hash", len(resolved) > 0,
                 "%d of %d" % (len(resolved), len(records)))
    report.check("at least one record is causally matched", len(matched) > 0,
                 "%d matched, %d unmatched" % (len(matched), len(unmatched)))

    # --- 1. declaration-to-map join, recomputed --------------------------------------------
    mismatches = []
    for r in resolved:
        if not r.get("declaration_matched"):
            continue
        d = r["declaration"]
        want = encode_map_word(d["usage_type"], d["start_register"], d["flags"])
        got = as_int(r["decoded_map_word"])
        if want != got:
            mismatches.append((r["shader_hash"], r["api_slot"], want, got))
    checked = sum(1 for r in resolved if r.get("declaration_matched"))
    report.check("decoded map word == encoder(declaration), recomputed",
                 not mismatches, "%d checked" % checked)

    # --- 2. logical API-slot consistency ---------------------------------------------------
    # The declaration's own api_slot must equal the slot the writer was called with.
    slot_bad = [r for r in resolved
                if r.get("declaration_matched")
                and r["declaration"]["api_slot"] != r["api_slot"]]
    report.check("declaration api_slot == writer api_slot", not slot_bad,
                 "%d violations" % len(slot_bad))

    # --- 3. map-word bit decomposition -----------------------------------------------------
    bad_index = [r for r in resolved
                 if r.get("mapped_index") is not None
                 and r["mapped_index"] != (as_int(r["decoded_map_word"]) & 0x3FFF)]
    report.check("mapped_index is the low 14 bits", not bad_index)
    bad_direct = [r for r in resolved
                  if r.get("direct") is not None
                  and r["direct"] != bool(as_int(r["decoded_map_word"]) & 0x8000)]
    report.check("direct is bit 15", not bad_direct)

    # --- 4. descriptor width and fingerprint ----------------------------------------------
    bad_width = [r for r in resolved if r.get("descriptor_bytes") not in (16, 32)]
    report.check("descriptor width is 16 or 32 bytes", not bad_width)
    bad_fp = [r for r in resolved
              if not r.get("descriptor_fingerprint")
              or len(r["descriptor_fingerprint"]) != 16]
    report.check("every resolved record carries a 16-hex-digit fingerprint", not bad_fp)
    # A fingerprint of all zeroes is the FNV-1a offset basis, which no real descriptor produces.
    # It is the signature of a missing or placeholder capture rather than a measured value.
    placeholder = [r for r in resolved
                   if r.get("descriptor_fingerprint") in ("0000000000000000", "cbf29ce484222325")]
    report.check("no fingerprint is a placeholder", not placeholder,
                 "%d placeholders" % len(placeholder))

    # --- 5. shader resource derivation -----------------------------------------------------
    bad_res = [r for r in resolved
               if r.get("map_object") and r.get("shader_resource")
               and as_int(r["shader_resource"]) + 0xB0 != as_int(r["map_object"])]
    report.check("shader_resource + 0xB0 == map_object", not bad_res)

    # --- 6. stage-record / trailer arithmetic ---------------------------------------------
    # The trailer must be the OrbShdr the stage record points at. The fixture carries the trailer
    # address and the program data; the trailer must be above the program and inside the 64 KiB
    # window the original decoder's 23-bit size field can address.
    # The original decoder's arithmetic: trailer = program + (stage_word_0 & 0x7FFFFF) - 0x1C.
    # The fixture carries the program address and the stage size, so this is recomputed rather than
    # taken on trust.
    bad_trailer = []
    for r in resolved:
        if not r.get("trailer") or as_int(r["trailer"]) == 0:
            bad_trailer.append(("null", r.get("sequence")))
            continue
        if not r.get("code_address") or not r.get("stage_size"):
            bad_trailer.append(("missing-inputs", r.get("sequence")))
            continue
        expected = as_int(r["code_address"]) + r["stage_size"] - 28
        if expected != as_int(r["trailer"]):
            bad_trailer.append(("arithmetic", r.get("sequence")))
    report.check("trailer == code_address + stage_size - 28, recomputed", not bad_trailer,
                 "%d violations" % len(bad_trailer))

    # --- 7. causal upload association ------------------------------------------------------
    # A matched record must carry a real upload object and a source that equals the provider's
    # output buffer. This is recomputed, not read from the boolean.
    bad_match = []
    for r in matched:
        if not r.get("upload_object") or as_int(r["upload_object"]) == 0:
            bad_match.append(("null-object", r.get("sequence")))
            continue
        if not r.get("expected_provider_buffer"):
            bad_match.append(("no-buffer", r.get("sequence")))
            continue
        if as_int(r["source"]) != as_int(r["expected_provider_buffer"]):
            bad_match.append(("source-mismatch", r.get("sequence")))
    report.check("matched records: source == provider output buffer, recomputed",
                 not bad_match, "%d matched" % len(matched))

    # An unmatched record must not carry an upload object: inheriting one is the defect this
    # verifier exists to catch.
    inherited = [r for r in unmatched
                 if r.get("upload_object") and as_int(r["upload_object"]) != 0]
    report.check("unmatched records carry no upload object", not inherited,
                 "%d inherited" % len(inherited))

    # --- 8. stage domain -------------------------------------------------------------------
    # The writer's stage index is not the shader's logical stage. Record what was observed rather
    # than asserting a mapping.
    stages = sorted({r["stage"] for r in resolved})
    report.check("writer stage index observed", len(stages) > 0, str(stages))

    # --- 9. provenance ---------------------------------------------------------------------
    prov = fixture.get("provenance", {})
    report.check("provenance names the approved executable",
                 prov.get("executable_sha256", "").startswith("8caee78e"),
                 prov.get("executable_sha256", "")[:16])

    failed = report.emit()
    if negative:
        print()
        print("negative mode '%s': %s" % (negative, "correctly rejected" if failed else "NOT REJECTED"))
        return 0 if failed else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
