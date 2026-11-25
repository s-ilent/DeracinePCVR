#!/usr/bin/env python3
"""Read-only original SELF A-008 four-profile table verifier. Requires objdump."""
import argparse, hashlib, pathlib, struct, subprocess, tempfile
EXPECTED='8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0'
BASES=(0x35af340,0x35af350,0x35af360,0x35af370,0x35af380)
QBASE=0x35c3940
ROWS=((0xc44c,0,0,0,4,0x10),(0x22eea66a,0,0,0,6,0x1032),(0x5bb137d9,0x1ff5739d,0,0,7,0x54106327),(0x5bb137d9,0x1ff5739d,0x6e8224a8,0xac640ec,8,0xa85410feb632dc97))
def main():
 p=argparse.ArgumentParser();p.add_argument('self_path',type=pathlib.Path);args=p.parse_args()
 with args.self_path.open('rb') as f:
  assert hashlib.file_digest(f,'sha256').hexdigest()==EXPECTED
  def read(va,n):
   f.seek(va+0x1fd30);b=f.read(n);assert len(b)==n;return b
  cols=[struct.unpack('<4I',read(a,16)) for a in BASES]
  qs=struct.unpack('<4Q',read(QBASE,32))
  rows=tuple(tuple(col[i] for col in cols)+(qs[i],) for i in range(4))
  assert rows==ROWS,rows
  code=read(0x2f5d0f0,0x19f)
 with tempfile.TemporaryDirectory() as td:
  path=pathlib.Path(td)/'window.bin';path.write_bytes(code)
  out=subprocess.check_output(['objdump','-D','-b','binary','-m','i386:x86-64','-M','intel','--adjust-vma=0x2f5d0f0',str(path)],text=True)
 for addr in ('2f5d101:','2f5d10b:','2f5d112:','2f5d119:','2f5d120:','2f5d127:','2f5d132:','2f5d136:','2f5d13a:','2f5d141:','2f5d148:','2f5d14b:','2f5d14f:','2f5d153:','2f5d156:','2f5d257:','2f5d260:','2f5d263:','2f5d265:'):
  assert addr in out,addr
 for va in (*BASES,QBASE):assert f'# 0x{va:x}' in out,hex(va)
 for i,row in enumerate(rows,1):
  packed=(i&7)|((i&7)<<20)|((row[4]&15)<<13)
  print(f'value={i} dwords={" ".join(f"{v:08x}" for v in row[:4])} nibble={row[4]} qword={row[5]:016x} final={packed:08x}')
 print('A-008 verified: SELF digest, six tables, scaled-index instructions, four rows and packed words')
if __name__=='__main__':main()
