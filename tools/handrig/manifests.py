#!/usr/bin/env python3
"""Build the investigation manifests from the handrig dumps.

Inputs (all produced by `handrig` into _build/handrig):
  inventory-exports.csv, inventory-references.csv, animations.csv, posefinger-*.json,
  mesh-*.json, skeleton-*.json
Outputs:
  animation-inventory.csv, pose-inventory.csv, asset-dependencies.csv, summary.json
"""
import csv
import json
import pathlib
import re

OUT = pathlib.Path(r"M:\Development\Deracine-work\_build\handrig")


def read_csv(name):
    path = OUT / name
    if not path.exists():
        return []
    with path.open(encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def main():
    exports = read_csv("inventory-exports.csv")
    references = read_csv("inventory-references.csv")
    animations = read_csv("animations.csv")

    player_skeletons = {"c0000_Skeleton", "c0100_Skeleton"}
    player_animations = [a for a in animations if a["skeleton"] in player_skeletons]
    with (OUT / "animation-inventory.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["asset", "skeleton", "frames", "compressedTracks", "fingerTracks"])
        for row in sorted(player_animations, key=lambda r: r["asset"]):
            writer.writerow([row["asset"], row["skeleton"], row["frames"],
                             row["compressedTracks"], row["fingerTracks"]])

    poses = []
    for path in sorted(OUT.glob("posefinger-*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        poses.append({
            "asset": data["asset"],
            "poses": data["poseCount"],
            "tracks": len(data["tracks"]),
            "fingerTracks": data["fingerTracks"],
            "articulatedFingers": sum(1 for v in data["fingerVariation"] if v["maxDegrees"] > 1.0),
            "maxFingerDegrees": max((v["maxDegrees"] for v in data["fingerVariation"]), default=0),
        })
    with (OUT / "pose-inventory.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(poses[0].keys()) if poses else ["asset"])
        writer.writeheader()
        writer.writerows(poses)

    # Dependency map: which packages mention a known hand-rig asset in their name table. Cooked
    # blueprints keep imported names, so this is an evidence-backed reference edge.
    known = set()
    for row in exports:
        if row["class"] in ("Skeleton", "SkeletalMesh", "AnimSequence", "AnimBlueprint", "PoseAsset"):
            known.add(row["export"])
    known |= {p["asset"].rsplit("/", 1)[-1].split(".")[0] for p in poses}
    edges = {}
    for row in references:
        if row["name"] in known:
            edges.setdefault(row["package"], set()).add(row["name"])
    with (OUT / "asset-dependencies.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["package", "references"])
        for package in sorted(edges):
            if re.search(r"c0000|c0100", package):
                writer.writerow([package, ";".join(sorted(edges[package]))])

    summary = {
        "pak_assets": len(exports),
        "skeletons": sum(1 for r in exports if r["class"] == "Skeleton"),
        "skeletal_meshes": sum(1 for r in exports if r["class"] == "SkeletalMesh"),
        "anim_sequences": sum(1 for r in exports if r["class"] == "AnimSequence"),
        "anim_blueprints": sum(1 for r in exports if r["class"] == "AnimBlueprint"),
        "skeletal_mesh_components": sum(1 for r in exports if r["class"] == "SkeletalMeshComponent"),
        "sequences_total": len(animations),
        "sequences_with_finger_tracks": sum(1 for a in animations if int(a["fingerTracks"]) > 0),
        "player_sequences": len(player_animations),
        "player_sequences_with_finger_tracks": sum(1 for a in player_animations if int(a["fingerTracks"]) > 0),
        "pose_assets": poses,
        "hand_rigs": {"c0000": "right hand", "c0100": "left hand"},
    }
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
