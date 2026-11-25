#!/usr/bin/env python3
"""C-019 read-only original SELF verifier; contains no proprietary binary bytes."""
import hashlib, struct, sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
BASE=0x1FD30
with open(sys.argv[1],"rb") as f:
    assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED
    def get(va,n):
        f.seek(va+BASE)
        return f.read(n)
    table=get(0x037530B0,14)
    assert hashlib.sha256(table).hexdigest()=="36ea4b08ba4748fc9b77c7cf90e0a638da6f818f907f5e98615bbbb6620cd75b"
    assert struct.unpack("<7H",table)==(0x2E40,0x2C0C,0x2C4C,0x2C8C,0x2CCC,0x2D0C,0x2D4C)
    for va,hexbytes in ((0x02F62B38,"488d3571057f00"),(0x02F62B43,"0fb71456"),(0x02F62B57,"8d8c1100d4ffff"),(0x02F62B61,"89480c"),(0x02F62CC8,"488d35e1037f00")):
        assert get(va,len(bytes.fromhex(hexbytes)))==bytes.fromhex(hexbytes),hex(va)
print("PASS C-019: original SELF digest, table, two consumer instruction paths")
