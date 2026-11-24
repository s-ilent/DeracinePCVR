#!/usr/bin/env python3
"""Compare two render-target dumps for capture noninterference.

The capture is supposed to observe the production rendering path, not change it. This compares the
same deterministic scene rendered with the texture instrumentation off and on, and reports whether
the colour images are identical.

It compares the largest colour attachment in each dump, which is the frame the game presents. A
difference is reported with its magnitude rather than as a pass or fail, because a small difference
still needs investigating and a large one is a regression.

Usage: python compare_capture_ab.py <off-dir> <on-dir>
"""

import hashlib
import os
import struct
import sys
import zlib


def read_png(path):
    """Minimal PNG reader for the 8-bit RGB/RGBA images the dump writes."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos = 8
    width = height = 0
    bit_depth = color_type = 0
    idat = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length
    if bit_depth != 8 or color_type not in (2, 6):
        return None
    channels = 3 if color_type == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(width * height * channels)
    prev = bytearray(stride)
    src = 0
    for y in range(height):
        filter_type = raw[src]
        src += 1
        line = bytearray(raw[src:src + stride])
        src += stride
        if filter_type == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, channels, bytes(out)


def largest_colour(directory):
    """The largest 8-bit colour image in a dump, which is the presented frame."""
    best = None
    for root, _, files in os.walk(directory):
        for name in files:
            if not name.endswith(".png"):
                continue
            if "R8G8B8A8" not in name and "B8G8R8A8" not in name:
                continue
            path = os.path.join(root, name)
            parsed = read_png(path)
            if parsed is None:
                continue
            width, height, channels, pixels = parsed
            if best is None or width * height > best[0] * best[1]:
                best = (width, height, channels, pixels, path)
    return best


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    off_dir, on_dir = argv

    off = largest_colour(off_dir)
    on = largest_colour(on_dir)
    if off is None or on is None:
        print("could not find a comparable colour image in both dumps")
        return 1

    off_w, off_h, off_c, off_px, off_path = off
    on_w, on_h, on_c, on_px, on_path = on
    print(f"off: {os.path.basename(off_path)} {off_w}x{off_h} channels={off_c}")
    print(f"on:  {os.path.basename(on_path)} {on_w}x{on_h} channels={on_c}")

    if (off_w, off_h) != (on_w, on_h):
        print("FAIL: the two dumps have different dimensions, so they are not the same frame")
        return 1

    off_hash = hashlib.sha256(off_px).hexdigest()
    on_hash = hashlib.sha256(on_px).hexdigest()
    print(f"off sha256: {off_hash}")
    print(f"on  sha256: {on_hash}")

    if off_hash == on_hash:
        print("IDENTICAL: the capture did not change the rendered frame")
        return 0

    # Report the magnitude, so a difference is characterised rather than merely detected.
    channels = min(off_c, on_c)
    total = off_w * off_h
    differing = 0
    max_delta = 0
    sum_delta = 0
    for i in range(total):
        base = i * off_c
        other = i * on_c
        pixel_differs = False
        for c in range(channels):
            delta = abs(off_px[base + c] - on_px[other + c])
            if delta:
                pixel_differs = True
                max_delta = max(max_delta, delta)
                sum_delta += delta
        if pixel_differs:
            differing += 1

    print(f"DIFFERENT: {differing} of {total} pixels differ "
          f"({100.0 * differing / total:.3f}%)")
    print(f"max channel delta: {max_delta}")
    print(f"mean channel delta over differing pixels: "
          f"{(sum_delta / max(1, differing * channels)):.4f}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
