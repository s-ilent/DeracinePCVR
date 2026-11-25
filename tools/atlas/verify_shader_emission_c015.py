#!/usr/bin/env python3
"""Read-only original SELF byte verifier for C-015. No proprietary bytes embedded."""
import hashlib, pathlib, sys
EXPECTED = "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
BASE = 0x1FD30
WINDOWS = [
    (0x0CB82B4, 0x0CB846C, "1437fa3650508a99054da139adf7165aef7f20e9f23b7070de736d9f176107dc"),
    (0x0CB846C, 0x0CB8637, "160b6070efe7ce8d8eb426602578e51f056e97f85a7db740257d6319e4e1f220"),
]
def main(path):
    with open(path, "rb") as f:
        assert hashlib.file_digest(f, "sha256").hexdigest() == EXPECTED, "wrong original SELF"
        windows = []
        for start, end, digest in WINDOWS:
            f.seek(BASE + start)
            data = f.read(end-start)
            assert len(data) == end-start
            assert hashlib.sha256(data).hexdigest() == digest, hex(start)
            windows.append(data)
    mode1, mode5 = windows
    for data in windows:
        assert bytes.fromhex("00307568") in data
        assert bytes.fromhex("003c05c0") in data
        assert bytes.fromhex("01000000ffffffff") in data
    assert bytes.fromhex("004704c028850300") in mode1
    assert bytes.fromhex("004704c004850300") in mode5
    print("C-015 original SELF digest, windows and packet constants: PASS")
if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]))
