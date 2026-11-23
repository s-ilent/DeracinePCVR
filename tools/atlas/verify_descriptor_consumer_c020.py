#!/usr/bin/env python3
"""C-020: read-only verifier of original SELF bytes; no copyrighted bytes distributed."""
import hashlib,sys
P="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
with open(sys.argv[1],"rb") as f:
    assert hashlib.file_digest(f,"sha256").hexdigest()==P
    def read(va,n):
        f.seek(va+0x1FD30)
        return f.read(n)
    a,b=0x02F62D60,0x02F62EDA
    assert hashlib.sha256(read(a,b-a)).hexdigest()=="89673455122f571b5907fa474a61517d29c0b4839a67d7a33521114672a6c63b"
    anchors={
      0x02F62D60:"554889e54157415641554154",
      0x02F62DD2:"410fb78441a20000003d",
      0x02F62DF5:"c4c1781006410fb7c5c4c178110480",
      0x02F62E58:"488d3551027f00410fb7cd0fb71456",
      0x02F62E67:"48be001000c00d007568488930",
      0x02F62E9F:"4883c0204989442410",
      0x02F62EC3:"c6840750c2000001",
      0x0CC02B3:"e8a82a2a02",
    }
    for va,h in anchors.items():
        assert read(va,len(bytes.fromhex(h)))==bytes.fromhex(h),hex(va)
print("PASS C-020: original SELF digest, complete consumer body, direct caller and instruction anchors")
