#!/usr/bin/env python3
"""Standalone read-only PS4 GNM input-usage parser (Binary Atlas A-028).

Examples:
  python tools/atlas/parse_gnm_usage_a028.py --file private-shader-memory.bin --json
  python tools/atlas/parse_gnm_usage_a028.py --self private-1.00-eboot.bin
  python tools/atlas/parse_gnm_usage_a028.py --test

The --file input must include the full original OrbShdr trailer AND preceding
GnmInputUsageSlot records; shadPS4's ordinary shader .bin dumps may omit these.
No shadPS4, Vulkan, GPU emulation or game assets are needed.
"""
import argparse
from collections import Counter
import hashlib
import json
import struct
import sys
import unittest
from dataclasses import asdict, dataclass
from pathlib import Path

MAGIC = b"OrbShdr"
TRAILER_SIZE = 28
SELF_SIZE = 81_506_767
SELF_SHA = "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0"
USAGE = {
    0x00:"IMM_RESOURCE", 0x01:"IMM_SAMPLER", 0x02:"IMM_CONSTBUFFER",
    0x03:"IMM_VERTEXBUFFER", 0x04:"IMM_RWRESOURCE",
    0x05:"IMM_ALUFLOATCONST", 0x06:"IMM_ALUBOOL32CONST",
    0x07:"IMM_GDSCOUNTERRANGE", 0x08:"IMM_GDSMEMORYRANGE",
    0x09:"IMM_GWSBASE", 0x0A:"IMM_SRT", 0x0D:"IMM_LDSESGSSIZE",
    0x12:"SUBPTR_FETCHSHADER", 0x13:"PTR_RESOURCETABLE",
    0x14:"PTR_INTERNALRESOURCETABLE", 0x15:"PTR_SAMPLERTABLE",
    0x16:"PTR_CONSTBUFFERTABLE", 0x17:"PTR_VERTEXBUFFERTABLE",
    0x18:"PTR_SOBUFFERTABLE", 0x19:"PTR_RWRESOURCETABLE",
    0x1A:"PTR_INTERNALGLOBALTABLE", 0x1B:"PTR_EXTENDEDUSERDATA",
    0x1C:"PTR_INDIRECTRESOURCETABLE",
    0x1D:"PTR_INDIRECTINTERNALRESOURCETABLE",
    0x1E:"PTR_INDIRECTRWRESOURCETABLE",
    0x22:"IMM_GDS_KICK_RING_OFFSET", 0x23:"IMM_VERTEX_RING_OFFSET",
    0x24:"PTR_DISPATCH_DRAW", 0x25:"IMM_DISPATCH_DRAW_INSTANCES",
}

class BadMetadata(ValueError):
    pass

@dataclass(frozen=True)
class UsageRecord:
    kind: int
    kind_name: str
    api_slot: int
    start_register: int
    flags: int

def parse_trailer(blob: bytes, at: int) -> dict:
    if at < 0 or at + TRAILER_SIZE > len(blob) or blob[at:at+7] != MAGIC:
        raise BadMetadata("missing/incomplete OrbShdr trailer")
    packed = struct.unpack_from("<I", blob, at+8)[0]
    chunk = blob[at+12]
    count = blob[at+13]
    flags = blob[at+14]
    table_start = at - 4 * (chunk + count)
    if table_start < 0 or table_start + 4 * count > at:
        raise BadMetadata("input usage table outside supplied memory")
    records = []
    for i in range(count):
        kind, api, reg, flag = struct.unpack_from("<BBBB", blob, table_start+4*i)
        records.append(asdict(UsageRecord(kind,USAGE.get(kind,f"UNKNOWN_0x{kind:02X}"),api,reg,flag)))
    return {
        "trailer_offset":at,
        "binary_info_version":blob[at+7],
        "binary_info_code_length":packed >> 8,
        "shader_type":(packed >> 2) & 0xf,
        "shader_hash":f"{struct.unpack_from('<Q',blob,at+16)[0]:016x}",
        "chunk_offset_dwords":chunk,
        "slot_count":count,
        "is_srt":bool(flags & 1),
        "extended_usage_info":bool(flags & 4),
        "slots":records,
    }

def find_shaders(blob: bytes) -> list[dict]:
    found=[]
    offset=0
    while True:
        trailer=blob.find(MAGIC,offset)
        if trailer<0:break
        try:found.append(parse_trailer(blob,trailer))
        except BadMetadata:pass
        offset=trailer+len(MAGIC)
    return found

def summary(shaders:list[dict]) -> dict:
    kinds=Counter(r["kind_name"] for s in shaders for r in s["slots"])
    return {"shader_count":len(shaders),
            "declaration_count":sum(s["slot_count"] for s in shaders),
            "usage_type_counts":dict(sorted(kinds.items())),
            "shaders":shaders}

def check_self(path:Path) -> dict:
    b=path.read_bytes()
    if len(b)!=SELF_SIZE or hashlib.sha256(b).hexdigest()!=SELF_SHA:
        raise BadMetadata("refused: not the approved original Déraciné 1.00 SELF")
    return {"approved_self":True,"size":len(b),"orbshdr_signatures":b.count(MAGIC)}

class Tests(unittest.TestCase):
    @staticmethod
    def fixture(records=((2,0,4,0),(0x15,1,8,1),(0x22,2,7,0)),chunk=2):
        b=bytearray(512);at=384
        pos=at-4*(chunk+len(records))
        for i,r in enumerate(records):struct.pack_into("<BBBB",b,pos+4*i,*r)
        b[at:at+7]=MAGIC;b[at+7]=3
        struct.pack_into("<I",b,at+8,(0x190<<8)|(2<<2)|1)
        b[at+12]=chunk;b[at+13]=len(records);b[at+14]=5
        struct.pack_into("<Q",b,at+16,0x0123456789ABCDEF)
        return bytes(b)
    def test_normal(self):
        a=find_shaders(self.fixture())
        self.assertEqual(len(a),1)
        self.assertEqual(a[0]["shader_hash"],"0123456789abcdef")
        self.assertEqual([r["kind"] for r in a[0]["slots"]],[2,0x15,0x22])
    def test_labels(self):
        h=summary(find_shaders(self.fixture()))["usage_type_counts"]
        self.assertEqual(h["IMM_CONSTBUFFER"],1)
        self.assertEqual(h["PTR_SAMPLERTABLE"],1)
        self.assertEqual(h["IMM_GDS_KICK_RING_OFFSET"],1)
    def test_no_magic(self):self.assertEqual(find_shaders(b"\0"*1024),[])
    def test_prefix(self):
        self.assertEqual(find_shaders(b"\0"*300+self.fixture())[0]["trailer_offset"],684)
    def test_short_trailer(self):self.assertEqual(find_shaders(MAGIC+b"\0"),[])
    def test_truncated_table(self):
        b=bytearray(self.fixture());b[384+12]=255
        self.assertEqual(find_shaders(bytes(b)),[])
    def test_zero_slots(self):
        self.assertEqual(find_shaders(self.fixture(records=()))[0]["slot_count"],0)
    def test_unknown(self):
        a=find_shaders(self.fixture(records=((0xfe,5,6,7),)))[0]
        self.assertEqual(a["slots"][0]["kind_name"],"UNKNOWN_0xFE")
    def test_flags(self):
        a=find_shaders(self.fixture(records=((3,12,14,0xe1),)))[0]
        self.assertEqual(a["slots"][0]["flags"],0xe1)
    def test_two_shaders(self):
        self.assertEqual(len(find_shaders(self.fixture()+self.fixture())),2)
    def test_trailing_data(self):
        self.assertEqual(len(find_shaders(self.fixture()+b"\0"*2048)),1)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    group=p.add_mutually_exclusive_group(required=True)
    group.add_argument("--file",type=Path,help="complete guest shader-memory snapshot")
    group.add_argument("--self",type=Path,help="check approved original SELF for literal trailers")
    group.add_argument("--test",action="store_true")
    p.add_argument("--json",action="store_true")
    args=p.parse_args()
    if args.test:
        suite=unittest.defaultTestLoader.loadTestsFromTestCase(Tests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    try:
        result=check_self(args.self) if args.self else summary(find_shaders(args.file.read_bytes()))
    except (OSError,BadMetadata) as e:
        print(str(e),file=sys.stderr);return 2
    if args.json:print(json.dumps(result,indent=2,sort_keys=True))
    else:
        for k,v in result.items():
            if k!="shaders":print(f"{k}: {v}")
        for shader in result.get("shaders",[]):
            print(f"hash={shader['shader_hash']} type={shader['shader_type']} usages={shader['slot_count']}")
    return 0

if __name__=="__main__":sys.exit(main())
