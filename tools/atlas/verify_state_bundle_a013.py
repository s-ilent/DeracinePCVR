#!/usr/bin/env python3
"""Verify A-013 against original private PS4 SELF. Never distribute input bytes."""
import argparse
import hashlib
from pathlib import Path

SELF_SHA = "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
WINDOWS = [
    (0x03090310, 0x03090450, "e35d804f8350bb045e6911cf54accca9133c234138e19e5d6c20fc9f025316cc"),
    (0x0308FE70, 0x0308FF20, "631bef171c277bfeb77eae95f819f3b1d498bea300bb3e260472b2c6f0cebc17"),
    (0x03090090, 0x03090130, "40b3256cb1c5bc06086f9b78caef27476f843009914f0d1f6dd6957ed7610ae3"),
    (0x0CC28DF, 0x0CC28EE, "7a51ea6d2972e9171ede0c45572f0043b6a2db221758ef1bf47e320dcd7faf87"),
]
# Explicit instruction bytes and boundaries, checked on the original image.
ANCHORS = [
    (0x03090310, bytes.fromhex("55 48 89 e5 41 56 53")),
    (0x03090325, bytes.fromhex("e8 46 fb ff ff")),
    (0x03090336, bytes.fromhex("e8 55 fd ff ff")),
    (0x03090368, bytes.fromhex("41 ff 56 18")),
    (0x03090380, bytes.fromhex("e8 eb fa ff ff")),
    (0x0309038C, bytes.fromhex("e8 ff fc ff ff")),
    (0x0309039D, bytes.fromhex("48 b9 00 69 08 c0 10 00 00 00")),
    (0x030903A7, bytes.fromhex("48 ba 00 69 01 c0 0f 00 00 00")),
    (0x0309040B, bytes.fromhex("49 89 4e 10")),
    (0x03090422, bytes.fromhex("49 8b 46 10")),
    (0x0309044E, bytes.fromhex("c3")),
    (0x0CC28E9, bytes.fromhex("e8 22 da 3c 02")),
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("self_path", type=Path)
    args = ap.parse_args()
    with args.self_path.open("rb") as f:
        assert f.seek(0, 2) == 81506767, "unexpected size"
        f.seek(0)
        assert hashlib.file_digest(f, "sha256").hexdigest() == SELF_SHA, "SELF digest mismatch"
        for start, end, expected in WINDOWS:
            f.seek(start + 0x1FD30)
            assert hashlib.sha256(f.read(end-start)).hexdigest() == expected, hex(start)
        for va, expected in ANCHORS:
            f.seek(va + 0x1FD30)
            assert f.read(len(expected)) == expected, hex(va)
    print("A-013: original SELF digest, four windows, and twelve instruction anchors verified")

if __name__ == "__main__":
    main()
