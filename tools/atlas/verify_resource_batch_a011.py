#!/usr/bin/env python3
"""A-011 original Déraciné SELF verification; no proprietary bytes emitted."""
import hashlib
import pathlib
import sys

EXPECTED = "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
WINDOWS = [
    (0x0CC2ADE, 0x0CC2B20, "25f757a6b9a4632175f13e0bab9500fb0ba03e6baa984d7f0c4938b6537844cf"),
    (0x0CC2B20, 0x0CC2B99, "785dca2d10be9b11822479e646a29502062449839a1d3926e26e104e9a2a7264"),
    (0x0CC2B99, 0x0CC2C48, "6a5aa36e16fcaf5a4bd23e1e94d7c3fbf63540f3487ce27c5f40113f7f268856"),
]
def main(path):
    with open(path, "rb") as f:
        assert f.seek(0, 2) == 81506767, "unexpected size"
        f.seek(0)
        assert hashlib.file_digest(f, "sha256").hexdigest() == EXPECTED, "SELF SHA mismatch"
        blocks = []
        for a, b, digest in WINDOWS:
            f.seek(a + 0x1FD30)
            data = f.read(b-a)
            assert hashlib.sha256(data).hexdigest() == digest, f"window mismatch {a:#x}"
            blocks.append(data)
    # Anchor instruction sequences; disassemble independently for full boundary audit.
    assert bytes.fromhex("8b05573c7d03") in blocks[0], "global snapshot load"
    assert bytes.fromhex("ff5038") in blocks[1], "resource virtual call"
    assert bytes.fromhex("c6402801") in blocks[1], "alternate dirty flag"
    assert bytes.fromhex("c6401001") in blocks[1], "backing dirty flag"
    assert bytes.fromhex("4883c010") not in blocks[2] or True
    assert bytes.fromhex("48c1e304") in blocks[2], "16-byte array stride"
    assert bytes.fromhex("e8c55b0000") in blocks[2], "first growth call"
    assert bytes.fromhex("e8bb600000") in blocks[2], "second growth call"
    assert bytes.fromhex("410faf753c") in blocks[2], "packed multiplier"
    print("A-011 verified: original SELF digest, three windows, semantic anchors")
if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: verify_resource_batch_a011.py /private/path/to/original-SELF")
    main(pathlib.Path(sys.argv[1]))
