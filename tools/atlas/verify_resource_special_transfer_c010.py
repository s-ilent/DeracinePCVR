#!/usr/bin/env python3
"""C-010 original SELF verification; no proprietary bytes embedded."""
import hashlib, pathlib, sys
p=pathlib.Path(sys.argv[1])
with p.open('rb') as f:
    assert hashlib.file_digest(f,'sha256').hexdigest()=='8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0'
    blobs={}
    for va,n,digest in [
        (0x64d0,0x300,'5afc5085eb015034c6acf4e42f0afdb3bfb3a661edff52a143026a9a8307bb8f'),
        (0x7280,0x300,'66db7e3d4e63995cb069752d2a25389779d9ee4fcfe34f354c537c8603f45f4a')]:
        f.seek(va+0x1fd30); b=f.read(n)
        assert hashlib.sha256(b).hexdigest()==digest,hex(va)
        blobs[va]=b
def check(site,hexbytes):
    va=next(k for k,v in blobs.items() if k<=site<k+len(v))
    b=bytes.fromhex(hexbytes)
    assert blobs[va][site-va:site-va+len(b)]==b,hex(site)
for site,target in [(0x66df,0x5b90),(0x7482,0x5b90),(0x6669,0x2ec0),(0x740c,0x2ec0)]:
    va=next(k for k,v in blobs.items() if k<=site<k+len(v))
    b=blobs[va][site-va:site-va+5]
    assert b[0]==0xe8 and site+5+int.from_bytes(b[1:],'little',signed=True)==target
for site,seq in {
    0x6519:'8b4744',0x65cf:'8b7724',0x65d5:'837f3802',
    0x6714:'0fb60b',0x671e:'880c02',0x6723:'0fb70b',0x672d:'66890c02',
    0x6733:'8b0b',0x673c:'890c02',0x6741:'488b0b',0x674b:'48890c02',
    0x6751:'488b0b',0x675b:'48890c02',0x675f:'488b4b08',0x6763:'48894c0208',
    0x72c6:'448b6744',0x737e:'8b5724',0x7384:'837f3802',
    0x74c2:'8803',0x74d1:'668903',0x74e0:'8903',0x74ef:'488903',
    0x74fb:'488b0c02',0x74ff:'48890b',0x7502:'488b440208',0x7507:'48894308'
}.items():check(site,seq)
print('C-010 original SELF digest, two windows, four calls, direction-specific instructions: PASS')
