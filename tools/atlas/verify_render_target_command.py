#!/usr/bin/env python3
"""Read-only original-SELF verifier for A-005; no game bytes are emitted."""
import hashlib
import struct
import sys

EXPECTED = "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
SIZE = 81506767
EXEC_DELTA = 0x1FD30
RELRO_DELTA = 0x21240

def check(f, va, expected, delta=EXEC_DELTA):
    f.seek(va + delta)
    actual = f.read(len(expected))
    assert actual == expected, f"byte mismatch at inner ELF VA 0x{va:X}"

def main(path):
    with open(path, "rb") as f:
        assert f.seek(0, 2) == SIZE, "size mismatch"
        f.seek(0)
        assert hashlib.file_digest(f, "sha256").hexdigest() == EXPECTED, "SELF digest mismatch"
        f.seek(0x3F26A28 + RELRO_DELTA)
        assert struct.unpack("<QQ", f.read(16)) == (0x13A6A30, 0x13A6A70)
        check(f, 0x1420F77, bytes.fromhex("48 8d 0d aa 5a b0 02"))
        check(f, 0x1420F7E, bytes.fromhex("48 c7 43 08 00 00 00 00 48 89 0b 44 89 63 10"))
        check(f, 0x142103E, bytes.fromhex("c5 f8 11 83 d8 00 00 00"))
        check(f, 0x13A6A38, bytes.fromhex("48 8b 46 20 8b 77 10 44 8b 87 e8 00 00 00"))
        check(f, 0x13A6A46, bytes.fromhex("48 8d 57 18 48 8d 8f d8 00 00 00"))
        check(f, 0x13A6A51, bytes.fromhex("4c 8d 8f f0 00 00 00"))
        check(f, 0x13A6A62, bytes.fromhex("e8 b9 b9 91 ff"))
        check(f, 0xCC2500, bytes.fromhex("4d 39 f4 73 4b 49 8b 17"))
        check(f, 0xCC251E, bytes.fromhex("e8 dd 58 00 00"))
        check(f, 0xCC256F, bytes.fromhex("49 83 c7 18"))
        print("A-005: original SELF digest, command vtable, producer, Execute and binding loop PASS")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_render_target_command.py <private-original-SELF>")
    main(sys.argv[1])
