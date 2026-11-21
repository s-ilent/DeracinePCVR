#!/usr/bin/env python3
"""A-012: verify source SELF, bounded instruction windows and semantic anchors."""
import hashlib, pathlib, sys
SHA="8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
WINDOWS=[
 (0x0CC26AD,0x0CC2766,"965fc6bb0670d23c8b60db5b1eeacf39ae18b7650a0618c862348c73413496d5"),
 (0x0CC2766,0x0CC27FF,"5d7191a2c9b3a52523506ffb1ff204e5aa739098217b749136c632d70e257c2d"),
 (0x0CC28DF,0x0CC2923,"30df0dc52a20b273ad886bd45a78e2ddb9940879647fce017928859f0d5f9949"),
]
def main(path):
 with open(path,"rb") as f:
  assert f.seek(0,2)==81506767
  f.seek(0)
  assert hashlib.file_digest(f,"sha256").hexdigest()==SHA
  blocks=[]
  for a,b,h in WINDOWS:
   f.seek(a+0x1FD30)
   data=f.read(b-a)
   assert hashlib.sha256(data).hexdigest()==h,hex(a)
   blocks.append(data)
 for h in ("418b8d1c050000","c4c17a104f20","4189851c050000","48b9006901c00b000000","c5fa114808","4883c00c"):
  assert bytes.fromhex(h) in blocks[1],h
 assert bytes.fromhex("e822da3c02") in blocks[2]
 print("A-012 PASS: source SHA, three original-byte windows, scalar-cache and writer anchors")
if __name__=="__main__":
 if len(sys.argv)!=2:sys.exit("usage: verify_resource_scalar_a012.py /private/original-SELF")
 main(pathlib.Path(sys.argv[1]))
