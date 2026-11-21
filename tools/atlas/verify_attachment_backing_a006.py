#!/usr/bin/env python3
"""Read-only byte-level A-006 verifier. Never emits copyrighted bytes."""
import hashlib, sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
def main(path):
    with open(path,"rb") as f:
        assert f.seek(0,2)==81506767
        f.seek(0)
        assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED
        # Inner ELF VA -> original SELF offset for executable region
        windows={
            0xCC7E33:"4d 85 ed 48 8b 00",
            0xCC7E43:"49 8b 45 00 4c 89 ef ff 50 38 49 89 c6",
            0xCC7E50:"4c 89 e7 41 8b 76 3c e8 e4 13 00 00",
            0xCC7E97:"31 c0 45 31 f6",
            0xCC924B:"41 89 f6 49 89 ff 45 89 f4 41 83 e4 07",
            0xCC92BE:"48 b9 00 69 01 c0 92 02 00 00",
            0xCC92C8:"48 89 08 c7 40 08 03 00 00 00",
            0xCC932C:"48 b9 00 69 01 c0 92 02 00 00",
            0xCC9336:"48 89 08 c7 40 08 02 00 00 00",
            0xCC938B:"48 b9 00 69 01 c0 01 02 00 00",
            0xCC9395:"41 81 e4 77 77 3f 0f",
            0xCC93AB:"49 8b bf 60 04 00 00 44 89 f6 e8 36 3d 29 02"
        }
        for va,hexbytes in windows.items():
            expected=bytes.fromhex(hexbytes)
            f.seek(va+0x1FD30)
            assert f.read(len(expected))==expected, f"mismatch ELF VA {va:#x}"
    print("A-006 PASS: SELF digest and original-byte instruction windows")
if __name__=="__main__":
    if len(sys.argv)!=2:raise SystemExit("usage: verify_attachment_backing_a006.py <private SELF>")
    main(sys.argv[1])
