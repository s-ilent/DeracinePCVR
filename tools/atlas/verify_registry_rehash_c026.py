#!/usr/bin/env python3
"""C-026: original-SELF digest and exact original-offset interval checks. No raw bytes printed."""
import hashlib, pathlib, sys
p=pathlib.Path(sys.argv[1])
assert p.stat().st_size == 81506767
with p.open("rb") as f:
    assert hashlib.file_digest(f,"sha256").hexdigest() == "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
    for va,length,expected in [
        (0xCC95B8,0x414,"a176e9789800c5fc563f352f540c71ae703dcab8defefaad5378f033757a8a84"),
        (0xCC9767,0x62,"309fc024ea0ef063490b4cc6198562ca9ba2b9a672e752cc4151c1ae1e6b6a2b"),
    ]:
        f.seek(va+0x1FD30)
        assert hashlib.sha256(f.read(length)).hexdigest()==expected
print("C-026 original SELF rehash intervals PASS")
