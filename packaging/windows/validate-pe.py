import argparse,struct,pathlib,json,hashlib
p=argparse.ArgumentParser();p.add_argument('pe');p.add_argument('--icon',required=True);p.add_argument('--amd64',action='store_true');a=p.parse_args()
b=pathlib.Path(a.pe).read_bytes();u16=lambda o:struct.unpack_from('<H',b,o)[0];u32=lambda o:struct.unpack_from('<I',b,o)[0]
pe=u32(60);assert b[pe:pe+4]==b'PE\0\0';machine=u16(pe+4);n=u16(pe+6);opt=pe+24;sects=opt+u16(pe+20)
sections=[struct.unpack_from('<8sIIIIIIHHI',b,sects+i*40) for i in range(n)]
def offset(rva):
 for _,vs,va,sz,ptr,*_ in sections:
  if va<=rva<va+max(vs,sz):return ptr+rva-va
 raise ValueError(hex(rva))
base=offset(u32(opt+(112 if u16(opt)==0x20b else 96)+16))
records=[]
def visit(rel,path):
 count=u16(base+rel+12)+u16(base+rel+14)
 for i in range(count):
  key,value=struct.unpack_from('<II',b,base+rel+16+i*8)
  if key&0x80000000:
   namepos=base+(key&0x7fffffff);key=b[namepos+2:namepos+2+u16(namepos)*2].decode('utf-16le')
  if value&0x80000000:visit(value&0x7fffffff,path+[key])
  else:
   rva,size=struct.unpack_from('<II',b,base+value);records.append((path+[key],b[offset(rva):offset(rva)+size]))
visit(0,[])
ico=pathlib.Path(a.icon).read_bytes();count=struct.unpack_from('<H',ico,4)[0];wanted=[];sizes=[]
for i in range(count):
 w,h,_,_,_,_,size,pos=struct.unpack_from('<BBBBHHII',ico,6+i*16);wanted.append(ico[pos:pos+size]);sizes.append((w or 256,h or 256))
icons=[data for path,data in records if path[0]==3]
assert all(data in icons for data in wanted),'Embedded icon payload differs from approved ICO'
assert any(path[0]==14 for path,_ in records),'Missing icon group'
if a.amd64:assert machine==0x8664,hex(machine)
print(json.dumps({'file':str(pathlib.Path(a.pe).resolve()),'machine':hex(machine),'iconSizes':sizes,'approvedIconPayloadsMatched':len(wanted),'sha256':hashlib.sha256(b).hexdigest()},indent=2))
