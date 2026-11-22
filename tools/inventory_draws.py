#!/usr/bin/env python3
"""Build a candidate inventory of draws from a Deracine capture log.

Classifies every sampled draw by geometry shape so a genuine 3D scene mesh can be selected on
evidence rather than on vertex count alone.

Usage: python inventory_draws.py <shad_log.txt> [--json out.json]
"""

import argparse
import json
import re
import sys

DRAW_RE = re.compile(
    r"DERACINE_VERTEX_INPUT_DRAW provenance_id=(\d+) vs=(0x[0-9a-f]+) ps=(0x[0-9a-f]+) "
    r"pipeline=(0x[0-9a-f]+) prim=(\d+) indexed=(\w+) num_indices=(\d+) instances=(\d+) "
    r".*index_type=(\d+) index_swap=(\d+) index_base=(0x[0-9a-f]+) "
    r"index_address=(0x[0-9a-f]+) index_size=(\d+) index_bytes=(\d+) "
    r".*attributes=(\d+)")

ATTR_RE = re.compile(
    r"DERACINE_VERTEX_ATTRIBUTE provenance_id=(\d+) semantic=(\d+) vgpr=(\d+) elements=(\d+) "
    r".*base=(0x[0-9a-f]+) stride=(\d+) records=(\d+) "
    r".*dst_sel=(\d+) .*binding=(\d+) attr_offset=(\d+) vk_format=(\d+) input_rate=(\d+) "
    r"divisor=(\d+)")


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--json")
    args = parser.parse_args(argv)

    draws = {}
    with open(args.log, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = DRAW_RE.search(line)
            if match:
                pid = int(match.group(1))
                draws[pid] = {
                    "provenance_id": pid,
                    "vs": match.group(2),
                    "ps": match.group(3),
                    "pipeline": match.group(4),
                    "primitive_type": int(match.group(5)),
                    "indexed": match.group(6) == "true",
                    "num_indices": int(match.group(7)),
                    "instances": int(match.group(8)),
                    "index_type": int(match.group(9)),
                    "index_swap": int(match.group(10)),
                    "index_base": match.group(11),
                    "index_address": match.group(12),
                    "index_size": int(match.group(13)),
                    "index_bytes": int(match.group(14)),
                    "attribute_count": int(match.group(15)),
                    "attributes": [],
                }
                continue
            match = ATTR_RE.search(line)
            if match:
                pid = int(match.group(1))
                if pid not in draws:
                    continue
                draws[pid]["attributes"].append({
                    "semantic": int(match.group(2)),
                    "vgpr": int(match.group(3)),
                    "elements": int(match.group(4)),
                    "base": match.group(5),
                    "stride": int(match.group(6)),
                    "records": int(match.group(7)),
                    "dst_sel": match.group(8),
                    "binding": int(match.group(9)),
                    "attr_offset": int(match.group(10)),
                    "vk_format": int(match.group(11)),
                    "input_rate": int(match.group(12)),
                    "divisor": int(match.group(13)),
                })

    for draw in draws.values():
        attrs = draw["attributes"]
        draw["streams"] = len({a["binding"] for a in attrs})
        draw["strides"] = sorted({a["stride"] for a in attrs})
        draw["formats"] = sorted({a["vk_format"] for a in attrs})
        draw["elements"] = sorted({a["elements"] for a in attrs})
        # A screen-space element is typically a small vertex count with a 2- or 4-component
        # position and no depth-relevant third axis. This is a heuristic label, not a claim.
        draw["vertex_count"] = max((a["records"] for a in attrs), default=0)
        draw["likely_screen_space"] = (
            draw["vertex_count"] <= 1024 and draw["streams"] <= 1
        )

    ordered = sorted(draws.values(), key=lambda d: -d["num_indices"])
    print(f"draws: {len(ordered)}")
    print()
    header = f"{'id':>4} {'idx':>7} {'verts':>7} {'attrs':>5} {'strm':>4} {'strides':>10} " \
             f"{'formats':>14} {'inst':>4} {'prim':>4} {'screen?':>7}"
    print(header)
    print("-" * len(header))
    for draw in ordered:
        print(f"{draw['provenance_id']:>4} {draw['num_indices']:>7} {draw['vertex_count']:>7} "
              f"{draw['attribute_count']:>5} {draw['streams']:>4} "
              f"{','.join(map(str, draw['strides'])):>10} "
              f"{','.join(map(str, draw['formats'])):>14} "
              f"{draw['instances']:>4} {draw['primitive_type']:>4} "
              f"{'yes' if draw['likely_screen_space'] else 'no':>7}")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(ordered, handle, indent=1)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
