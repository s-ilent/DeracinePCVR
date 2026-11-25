#!/usr/bin/env python3
"""C-025 verifier; never prints copyrighted bytes."""
import hashlib, pathlib, sys
p=pathlib.Path(sys.argv[1])
assert p.stat().st_size==81506767
with p.open("rb") as f:
    assert hashlib.file_digest(f,"sha256").hexdigest()=="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
    f.seek(0xCE9100)
    d=f.read(0x1E8)
assert hashlib.sha256(d).hexdigest()=="70f50e7c6fa211b9374705d3254521a2aa2b70864b034219430e20e926f8710f"
with p.open("rb") as f:\n    f.seek(0xCE8288)\n    caller=f.read(0x29)\nassert hashlib.sha256(caller).hexdigest()=="7d7d41460b940e0ef93b78da0e0c62dd09395d549ecf8537e2721f77672af7ed"\nprint("C-025 original SELF caller and insertion interval PASS")
