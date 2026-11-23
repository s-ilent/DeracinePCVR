#!/usr/bin/env python3
"""C-027 original-SELF-only digest and instruction-window checks; no proprietary bytes emitted."""
import hashlib, pathlib, sys
p=pathlib.Path(sys.argv[1])
assert p.stat().st_size==81506767
with p.open("rb") as f:
    assert hashlib.file_digest(f,"sha256").hexdigest()=="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
    f.seek(0xC7BD40+0x1FD30)
    assert hashlib.sha256(f.read(0x41C)).hexdigest()=="330b4fd659c5358c0243a5a3ffa7fd32bd15b51db23bd978103e38823972b06a"
    for callsite in [0xCC9776,0xCC978D,0xC8F00B,0xC8F02A]:
        f.seek(callsite+0x1FD30)
        b=f.read(5)
        assert b[0]==0xE8 and callsite+5+int.from_bytes(b[1:],"little",signed=True)==0xC7BD40
print("C-027 original SELF digest, helper and four callsites PASS")
