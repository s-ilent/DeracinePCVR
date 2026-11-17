#!/usr/bin/env python3
"""Deterministic scene-identity signature for benchmark screenshots.

A screenshot is reduced to a 64x36 grayscale image, normalised to zero mean and unit
standard deviation, and compared by Pearson correlation. Downscaling and normalisation make the
comparison insensitive to animation, HUD text, resize and absolute brightness, while the
classroom composition (chalkboard, windows, shelves, menu box) dominates the correlation.

The reference signature is reviewed once (see README); the validator recomputes the comparison
from the image file, so a run cannot pass by writing a high similarity number into its metadata.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys

WIDTH = 64
HEIGHT = 36
DEFAULT_THRESHOLD = 0.85


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def signature(path: str) -> list[float]:
    from PIL import Image
    import numpy as np

    with Image.open(path) as image:
        gray = image.convert("RGB").resize((WIDTH, HEIGHT), Image.LANCZOS)
    values = np.asarray(gray, dtype=np.float64).mean(axis=2)
    deviation = values.std()
    if not np.isfinite(deviation) or deviation < 1e-3:
        raise ValueError(f"{path}: image is uniform (std={deviation:.6f}); cannot identify a scene")
    values = (values - values.mean()) / deviation
    return [round(float(v), 6) for v in values.ravel()]


def similarity(left: list[float], right: list[float]) -> float:
    if len(left) != len(right):
        raise ValueError(f"signature sizes differ: {len(left)} != {len(right)}")
    n = len(left)
    mean_left = sum(left) / n
    mean_right = sum(right) / n
    cov = sum((a - mean_left) * (b - mean_right) for a, b in zip(left, right))
    var_left = sum((a - mean_left) ** 2 for a in left)
    var_right = sum((b - mean_right) ** 2 for b in right)
    if var_left <= 0.0 or var_right <= 0.0:
        raise ValueError("signature has no variance")
    return cov / (var_left * var_right) ** 0.5


def image_size(path: str) -> tuple[int, int]:
    from PIL import Image

    with Image.open(path) as image:
        return image.size


def save_preview(image_path: str, out_path: str, width: int = 320) -> None:
    from PIL import Image

    with Image.open(image_path) as image:
        image = image.convert("RGB")
        height = max(1, round(width * image.height / image.width))
        image.resize((width, height), Image.LANCZOS).save(out_path)


def build_reference(image_path: str, source_note: str) -> dict:
    width, height = image_size(image_path)
    return {
        "schema_version": 1,
        "width": WIDTH,
        "height": HEIGHT,
        "source": {
            "image": os.path.basename(image_path),
            "sha256": sha256_file(image_path),
            "size": [width, height],
            "note": source_note,
        },
        "signature": signature(image_path),
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    extract = sub.add_parser("extract", help="write a reference signature JSON for an image")
    extract.add_argument("image")
    extract.add_argument("--out", required=True)
    extract.add_argument("--source-note", default="")

    preview = sub.add_parser("preview", help="write a small reviewable preview of an image")
    preview.add_argument("image")
    preview.add_argument("--out", required=True)
    preview.add_argument("--width", type=int, default=320)

    size = sub.add_parser("size", help="print an image's pixel dimensions as JSON")
    size.add_argument("image")

    compare = sub.add_parser("compare", help="compare an image with a reference signature")
    compare.add_argument("image")
    compare.add_argument("--reference", required=True)
    compare.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)

    args = parser.parse_args(argv)

    if args.command == "extract":
        reference = build_reference(args.image, args.source_note)
        with open(args.out, "w", encoding="utf-8") as handle:
            json.dump(reference, handle, indent=1)
            handle.write("\n")
        return 0

    if args.command == "preview":
        save_preview(args.image, args.out, args.width)
        return 0

    if args.command == "size":
        width, height = image_size(args.image)
        print(json.dumps({"width": width, "height": height}))
        return 0

    with open(args.reference, encoding="utf-8") as handle:
        reference = json.load(handle)
    score = similarity(signature(args.image), reference["signature"])
    print(json.dumps({"similarity": round(score, 6), "threshold": args.threshold,
                      "pass": score >= args.threshold}))
    return 0 if score >= args.threshold else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
