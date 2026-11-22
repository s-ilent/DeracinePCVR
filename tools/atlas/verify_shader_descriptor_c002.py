#!/usr/bin/env python3
"""Verify C-002 on a privately held original 1.00 SELF; never redistribute bytes."""
import hashlib, pathlib, sys
p=pathlib.Path(sys.argv[1])
expected="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
with p.open("rb") as f:
    assert hashlib.file_digest(f,"sha256").hexdigest()==expected
    for va,size,digest in [
        (0x3092d20,288,"e907f47a0571fe80f893d16ba8cfd81747050db6a845a303f0b9568483494a93"),
        (0x3752fb0,244,"1bea6c958db92ad22542248fe1c2b09730e7a6e4708ca7a698d9b48dc4eefb96")]:
        f.seek(va+0x1fd30); b=f.read(size)
        assert hashlib.sha256(b).hexdigest()==digest,hex(va)
print("C-002 original SELF and both windows verified")
