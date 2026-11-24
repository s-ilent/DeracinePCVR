#!/usr/bin/env python3
"""Seeds a schema-4 fixture's reference from a known-good render.

The fixture's reference must be the image the renderer actually produces, or the comparison is
testing the fixture rather than the renderer. This reads a PPM the replay wrote and writes it as the
package's reference sidecar, updating the declared hash and extent.

This is a fixture tool. It does not make a synthetic render into a captured reference: the package
still says it is synthetic.

Usage: python seed_reference.py <package-dir> <ppm-path>
"""

import hashlib
import json
import os
import sys


def read_ppm(path):
    with open(path, 'rb') as handle:
        data = handle.read()
    # P6\n<width> <height>\n<maxval>\n<pixels>
    parts = data.split(b'\n', 3)
    if parts[0] != b'P6':
        raise SystemExit(f'{path} is not a binary PPM')
    width, height = (int(v) for v in parts[1].split())
    maxval = int(parts[2])
    if maxval != 255:
        raise SystemExit(f'{path} has maxval {maxval}, expected 255')
    pixels = parts[3]
    expected = width * height * 3
    if len(pixels) != expected:
        raise SystemExit(f'{path} holds {len(pixels)} bytes, expected {expected}')
    # The reference is RGBA, so an opaque alpha is appended.
    rgba = bytearray(width * height * 4)
    for i in range(width * height):
        rgba[i * 4 + 0] = pixels[i * 3 + 0]
        rgba[i * 4 + 1] = pixels[i * 3 + 1]
        rgba[i * 4 + 2] = pixels[i * 3 + 2]
        rgba[i * 4 + 3] = 255
    return width, height, bytes(rgba)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    directory, ppm = argv
    width, height, rgba = read_ppm(ppm)

    with open(os.path.join(directory, 'reference_post.bin'), 'wb') as handle:
        handle.write(rgba)

    path = os.path.join(directory, 'package.json')
    with open(path, encoding='utf-8') as handle:
        package = json.load(handle)
    package['reference_output']['width'] = width
    package['reference_output']['height'] = height
    package['reference_output']['bytes_per_pixel'] = 4
    package['reference_output']['post_sha256'] = hashlib.sha256(rgba).hexdigest()
    package['reference_output']['post_file'] = 'reference_post.bin'
    with open(path, 'w', encoding='utf-8') as handle:
        json.dump(package, handle, indent=1)

    print(f'seeded {width}x{height} reference from {ppm}')
    print(f'  sha256 {hashlib.sha256(rgba).hexdigest()[:16]}...')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
