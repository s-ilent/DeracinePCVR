#!/usr/bin/env python3
"""C-024 original SELF verifier; never emits proprietary bytes."""
import hashlib, pathlib, sys
p=pathlib.Path(sys.argv[1])
assert p.stat().st_size == 81506767
with p.open("rb") as f:
    assert hashlib.file_digest(f,"sha256").hexdigest() == "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
    f.seek(0xCD7E00)
    data=f.read(0x1BB)
assert hashlib.sha256(data).hexdigest() == "b61b2c4cc5e2461401b002b86327dc4530a530fb62e33f91549f3806aeab60e9"
print("C-024 original SELF interval PASS; ELF 0xCB80D0..0xCB828B")
