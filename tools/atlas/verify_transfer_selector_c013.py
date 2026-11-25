#!/usr/bin/env python3
"""Verify C-013 on exact original PS4 SELF; no proprietary bytes embedded."""
import hashlib, pathlib, struct, sys
p=pathlib.Path(sys.argv[1])
with p.open("rb") as f:
    assert hashlib.file_digest(f,"sha256").hexdigest()=="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
    for base,digest,targets in [
        (0x33cad10,"bb1753f7f8e6086d74826edfb1b38af75f7b306bee3557a38ae6ab85bfb2c889",(0x94f1,0x94c8,0x9504,0x9511)),
        (0x33cad90,"c75bab0c8df9cddff9e8e9397b4e86a876b93505321262332dd2a92a458d69f0",(0x9861,0x9838,0x9874,0x9881))
    ]:
        f.seek(base+0x1fd30); b=f.read(64)
        assert hashlib.sha256(b).hexdigest()==digest
        got=[base+x for x in struct.unpack("<16i",b)]
        expected=[targets[0] if i==1 else targets[1] if i in (4,8) else targets[2] if i==16 else targets[3] for i in range(1,17)]
        assert got==expected,(hex(base),got,expected)
    for va,seq in [(0x94b1,"ffc983f90f7759"),(0x94bf,"48630c8b4801d9ffe1"),(0x9821,"ffc983f90f7759"),(0x982f,"48630c8b4801d9ffe1"),(0x9504,"450fafc8"),(0x9874,"450fafc8")]:
        f.seek(va+0x1fd30);assert f.read(len(bytes.fromhex(seq)))==bytes.fromhex(seq),hex(va)
print("C-013: exact SELF, 16-case tables, branch dispatch and arithmetic PASS")
