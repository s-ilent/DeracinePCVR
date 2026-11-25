#!/usr/bin/env python3
"""C-029: read-only original PS4 SELF verifier; contains no proprietary bytes."""
import hashlib, pathlib, struct, sys
SHA="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
DELTA=0x1FD30
WINDOWS=[(0x308fe70,0x105,"449647116815bce4fcb71bc71429712c1643a3c90cd1a405d679afefc6da45c7"),(0x35af480,16,"c175f659bff12175bcddd6acf5cd0ed07b0c95d85a9b7032eaf87309d02ff504"),(0x3752fb0,32,"99b8b86019a500cd5528f571eafa811cfded3c10001ff9f565c78fb4319fa8ce")]
def run(path):
 with open(path,"rb") as f:
  assert hashlib.file_digest(f,"sha256").hexdigest()==SHA
  assert f.seek(0,2)==81506767
  def rd(a,n):
   f.seek(a+DELTA);return f.read(n)
  for a,n,digest in WINDOWS:
   assert hashlib.sha256(rd(a,n)).hexdigest()==digest
  assert struct.unpack("<4I",rd(0x35af480,16))==(0,2,0,4)
  for site,target in [(0x308fea9,0xab0),(0x308ff23,0x3450),(0x308ff35,0x3220)]:
   op=rd(site,5)
   assert op[0]==0xe8 and site+5+struct.unpack("<i",op[1:])[0]==target
  for a,h in [(0x308fe92,"448b7308"),(0x308fe96,"a803"),(0x308ff15,"81ff00120000"),(0x308ff51,"c44268f2f6"),(0x308ff5f,"4489f0")]:
   assert rd(a,len(h)//2)==bytes.fromhex(h)
 print("C-029 verified original-byte classifier and tables")
if __name__=="__main__":run(pathlib.Path(sys.argv[1]))
