#!/usr/bin/env python3
"""C-014: verify original SELF dispatch modes and selected byte windows. No proprietary output."""
import hashlib,struct,sys
EXPECTED="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
REGIONS={
 0x035C3980:(24,"dce14d01fa5d1678886570717801253ea6725498d65b1c6c8e012705b6dc0a5f"),
 0x0CB51BF:(35,"85540f40f1bdf2ec66f3cbd1dd37818a2ce2d8d6c56a694e73afc124595672bd"),
 0x0CB82B4:(176,"27064074e76e22eb09c14945d1069d5ef9a7ed2d4364440394696fd5661d3885"),
 0x0CB846C:(144,"48808451d7bf02114396d47b8bf486c08e21075193f3d694d1f6526f1567a5df"),
 0x0CB8637:(34,"f4fdfa861b94f94d741be617b03f4e4bc65e11012315f5011e324900463b0cb2"),
}
with open(sys.argv[1],"rb") as f:
 assert hashlib.file_digest(f,"sha256").hexdigest()==EXPECTED
 for va,(size,digest) in REGIONS.items():
  f.seek(va+0x1FD30)
  assert hashlib.sha256(f.read(size)).hexdigest()==digest,hex(va)
 f.seek(0x035C3980+0x1FD30)
 targets=[0x035C3980+x for x in struct.unpack("<6i",f.read(24))]
 assert targets==[0x0CB51E0,0x0CB82B4,0x0CB51E0,0x0CB8637,0x0CB51E0,0x0CB846C]
 print("PASS C-014: original SHA, five windows, six relative dispatch targets")
