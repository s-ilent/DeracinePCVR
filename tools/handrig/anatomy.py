#!/usr/bin/env python3
"""Derive the hand-rig manifests from the dumps written by the handrig tool.

Reads _build/handrig/skeleton-*.json (reference pose in world space) and writes:
  hand-rig-manifest.json  - chains, joint reference transforms, mesh/skeleton wiring
  openxr-mapping.csv      - OpenXR hand joint -> game bone, per side
  finger-anatomy.txt      - evidence table used to decide which digit is the thumb

Nothing is read from the game here; this is pure analysis of the tool's JSON output.
"""
import json
import math
import pathlib
import sys

OUT = pathlib.Path(r"M:\Development\Deracine-work\_build\handrig")

# OpenXR XR_HAND_JOINT_* order (26 joints).
OPENXR_JOINTS = [
    "PALM", "WRIST",
    "THUMB_METACARPAL", "THUMB_PROXIMAL", "THUMB_DISTAL", "THUMB_TIP",
    "INDEX_METACARPAL", "INDEX_PROXIMAL", "INDEX_INTERMEDIATE", "INDEX_DISTAL", "INDEX_TIP",
    "MIDDLE_METACARPAL", "MIDDLE_PROXIMAL", "MIDDLE_INTERMEDIATE", "MIDDLE_DISTAL", "MIDDLE_TIP",
    "RING_METACARPAL", "RING_PROXIMAL", "RING_INTERMEDIATE", "RING_DISTAL", "RING_TIP",
    "LITTLE_METACARPAL", "LITTLE_PROXIMAL", "LITTLE_INTERMEDIATE", "LITTLE_DISTAL", "LITTLE_TIP",
]

# The game rig has 3 joints per digit and no metacarpal/tip bones. OpenXR proximal feeds the
# game's first phalanx, intermediate the second and distal the third; the tip has no bone and the
# metacarpal is folded into the palm/wrist.
GAME_DIGITS = ["THUMB", "INDEX", "MIDDLE", "RING", "LITTLE"]


def load(side_file: pathlib.Path):
    return json.loads(side_file.read_text(encoding="utf-8"))


def vec(p):
    return (p[0], p[1], p[2])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def norm(v):
    n = math.sqrt(sum(c * c for c in v))
    return (0.0, 0.0, 0.0) if n < 1e-9 else tuple(c / n for c in v)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def quat_rotate(q, v):
    x, y, z, w = q
    ix = w * v[0] + y * v[2] - z * v[1]
    iy = w * v[1] + z * v[0] - x * v[2]
    iz = w * v[2] + x * v[1] - y * v[0]
    iw = -x * v[0] - y * v[1] - z * v[2]
    return (ix * w + iw * -x + iy * -z - iz * -y,
            iy * w + iw * -y + iz * -x - ix * -z,
            iz * w + iw * -z + ix * -y - iy * -x)


def live_side(rig):
    """Each rig poses only one hand (the other side's chains collapse to near-zero length)."""
    totals = {}
    for chain in rig["fingers"]:
        totals[chain["Side"]] = totals.get(chain["Side"], 0.0) + chain["Length"]
    return max(totals, key=totals.get) if totals else "R"


def anatomy(rig):
    """Per-digit measurements; the thumb is the digit that leaves the palm plane."""
    bones = {b["Name"]: b for b in rig["bones"]}
    side = live_side(rig)
    hand = bones[side + "-Hand"]
    wrist = vec(hand["worldPosition"])
    bases = []
    for digit in range(5):
        joints = [b for b in rig["bones"] if b["Name"] in (f"{side}-Finger{digit}", f"{side}-Finger{digit}1", f"{side}-Finger{digit}2")]
        joints.sort(key=lambda b: (len(b["Name"]), b["Name"]))
        bases.append((digit, vec(joints[0]["worldPosition"]), vec(joints[-1]["worldPosition"]), joints))
    # Palm plane from the four non-thumb candidate bases (digits 1..4 by naming).
    anchor = [b for d, b, t, j in bases if d != 0]
    plane_normal = norm(cross(norm(sub(anchor[1], anchor[0])), norm(sub(anchor[2], anchor[0]))))
    rows = []
    for digit, base, tip, joints in bases:
        direction = norm(sub(tip, base))
        out_of_plane = abs(dot(direction, plane_normal))
        length = sum(math.dist(vec(joints[i - 1]["worldPosition"]), vec(joints[i]["worldPosition"])) for i in range(1, len(joints)))
        rows.append({
            "digit": digit,
            "chain": [b["Name"] for b in joints],
            "length": round(length, 3),
            "base_offset_from_wrist": round(math.dist(base, wrist), 3),
            "out_of_palm_plane": round(out_of_plane, 3),
        })
    # The thumb is the digit whose direction leaves the palm plane most strongly, with length as a
    # tie-break towards the shortest chain.
    ranked = sorted(rows, key=lambda r: (-r["out_of_palm_plane"], r["length"]))
    return rows, ranked[0]["digit"], plane_normal


def main():
    skeletons = sorted(OUT.glob("skeleton-*_Skeleton_uasset.json"))
    if not skeletons:
        sys.exit("no skeleton dumps; run: handrig skeleton <path>")

    manifest = {"source": "handrig offline dumps", "rigs": {}, "notes": []}
    mapping_rows = ["side,openxr_joint,game_bone,note"]
    anatomy_lines = ["# digit evidence per rig", ""]
    for path in skeletons:
        rig = load(path)
        if not rig["fingerBones"]:
            continue
        rows, thumb, normal = anatomy(rig)
        rig_name = path.stem
        manifest["rigs"][rig_name] = {
            "asset": rig["asset"],
            "bone_count": int(rig["boneCount"]),
            "finger_bones": int(rig["fingerBones"]),
            "helper_bones": int(rig["helperBones"]),
            "thumb_digit": thumb,
            "palm_plane_normal": [round(c, 4) for c in normal],
            "fingers": rows,
            "sockets": rig["sockets"],
            "virtual_bones": rig["virtualBones"],
        }
        anatomy_lines.append(f"## {rig['asset']}")
        for row in rows:
            anatomy_lines.append(
                f"  digit {row['digit']}: len={row['length']:7.3f} "
                f"base_from_wrist={row['base_offset_from_wrist']:7.3f} "
                f"out_of_plane={row['out_of_palm_plane']:5.3f} chain={'>'.join(row['chain'])}")
        anatomy_lines.append(f"  -> thumb = digit {thumb}")
        anatomy_lines.append("")

        side = live_side(rig)
        bones = {b["Name"]: b for b in rig["bones"]}
        manifest["rigs"][rig_name]["joints"] = {
            b["Name"]: {
                "index": b["Index"],
                "parent": b["Parent"],
                "translation": b["translation"],
                "rotation": b["rotation"],
            }
            for b in rig["bones"] if b["finger"] and b["Name"].startswith(side + "-")
        }

        # OpenXR -> game mapping for this side. The rig has 3 bones per digit and no tip bone.
        # Thumb evidence: its first bone starts at 6.8 units from the wrist while every other digit
        # starts at ~15.4, i.e. Finger0 of the thumb is the metacarpal, not a phalanx.
        for joint in OPENXR_JOINTS:
            side_prefix = side + "-"
            if joint in ("PALM", "WRIST"):
                bone = side_prefix + "Hand"
                note = "palm and wrist both drive the hand bone; the actor transform carries the wrist pose"
            elif joint == "THUMB_METACARPAL":
                bone = side_prefix + "Finger0"
                note = "thumb metacarpal: first thumb bone, base at the wrist"
            elif joint == "THUMB_TIP":
                bone = ""
                note = "no tip bone; tip is distal joint plus OpenXR tip offset"
            elif joint.startswith("THUMB_"):
                bone = side_prefix + "Finger" + {"PROXIMAL": "01", "DISTAL": "02"}[joint.split("_")[1]]
                note = "thumb phalanx (game thumb has 2 phalanges after its metacarpal)"
            elif joint.endswith("_TIP"):
                bone = ""
                note = "no tip bone; tip is distal joint plus OpenXR tip offset"
            elif joint.endswith("_METACARPAL"):
                bone = ""
                note = "no metacarpal bone; its rotation is folded into the hand bone"
            else:
                digit = {"INDEX": 1, "MIDDLE": 2, "RING": 3, "LITTLE": 4}[joint.split("_")[0]]
                stage = {"PROXIMAL": "", "INTERMEDIATE": "1", "DISTAL": "2"}[joint.split("_")[1]]
                bone = f"{side_prefix}Finger{digit}{stage}"
                note = "direct"
            mapping_rows.append(f"{side},{joint},{bone},{note}")

    (OUT / "hand-rig-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    (OUT / "openxr-mapping.csv").write_text("\n".join(mapping_rows) + "\n", encoding="utf-8")
    (OUT / "finger-anatomy.txt").write_text("\n".join(anatomy_lines), encoding="utf-8")
    print("wrote hand-rig-manifest.json, openxr-mapping.csv, finger-anatomy.txt")
    print("\n".join(anatomy_lines))


if __name__ == "__main__":
    main()
