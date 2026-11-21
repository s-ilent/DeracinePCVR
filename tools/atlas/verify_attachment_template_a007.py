#!/usr/bin/env python3
"""Read-only original SELF verifier for A-007. Requires objdump (binutils)."""
import argparse
import hashlib
import pathlib
import re
import struct
import subprocess

EXPECTED = "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
MAPPING = 0x1FD30
WINDOWS = {
    (0x02F5D0F0, 0x1A0): "f552bc742230f14afe567a4118627ea2895916ec02c2ce696c97028962725fcd",
    (0x02F5D230, 0x5F): "83aa53ad6a8803abdf75c79284ab7afb47a5f8efef5927192adb9599de81c656",
    (0x0CC93AB, 0x0F): "f6a913df88fe95c6c40fcb7f6ef45a70447e951f5e8b6880f0d2876138713492",
}
def main():
    p = argparse.ArgumentParser()
    p.add_argument("self_path", type=pathlib.Path)
    args = p.parse_args()
    with args.self_path.open("rb") as f:
        digest = hashlib.file_digest(f, "sha256").hexdigest()
        assert digest == EXPECTED, f"SELF hash mismatch: {digest}"
        for (va, length), expected in WINDOWS.items():
            f.seek(va + MAPPING)
            block = f.read(length)
            assert len(block) == length
            assert hashlib.sha256(block).hexdigest() == expected, f"byte window mismatch {va:#x}"
    with args.self_path.open("rb") as f:
        f.seek(0x0CC93B5 + MAPPING)
        call = f.read(5)
    assert call[0] == 0xE8
    dest = 0x0CC93BA + struct.unpack("<i", call[1:])[0]
    assert dest == 0x02F5D0F0, f"call edge mismatch: {dest:#x}"
    # Independent instruction-boundary confirmation on the real target bytes.
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        with args.self_path.open("rb") as f:
            f.seek(0x02F5D0F0 + MAPPING)
            block = f.read(0x19F)
        temp = pathlib.Path(td) / "window.bin"
        temp.write_bytes(block)
        output = subprocess.check_output([
            "objdump", "-D", "-b", "binary", "-m", "i386:x86-64",
            "-M", "intel", f"--adjust-vma=0x2f5d0f0", str(temp)
        ], text=True)
        for addr, mnemonic in [
            ("2f5d101", "lea"), ("2f5d10b", "cmp"), ("2f5d127", "mov"),
            ("2f5d153", "and"), ("2f5d182", "cmp"), ("2f5d197", "call"),
            ("2f5d1aa", "movabs"), ("2f5d1be", "mov"), ("2f5d201", "mov"),
            ("2f5d210", "mov"), ("2f5d21e", "mov"), ("2f5d230", "mov"),
            ("2f5d24c", "call"), ("2f5d257", "mov"), ("2f5d268", "movabs"),
            ("2f5d272", "mov"), ("2f5d28e", "ret")
        ]:
            assert re.search(rf"^\\s*{addr}:\\s+(?:[0-9a-f]{{2}}\\s+)+{mnemonic}\\b", output, re.M), (addr, mnemonic)
    print("A-007 verified: original SELF digest, 3 original-byte windows, direct call and instruction boundaries")
if __name__ == "__main__":
    main()
