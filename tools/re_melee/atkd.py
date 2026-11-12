import struct,zlib,sys
f=open('/mnt/gaming/modlists/LoreRim/Stock Game/Data/Skyrim.esm','rb').read()
want=sys.argv[1].encode()
def recs(buf,off,end):
    while off<end:
        t=buf[off:off+4]
        if t==b'GRUP':
            sz=struct.unpack_from('<I',buf,off+4)[0]; lab=buf[off+8:off+12]; gt=struct.unpack_from('<I',buf,off+12)[0]
            if gt==0 and lab!=b'RACE': off+=sz; continue
            yield from recs(buf,off+24,off+sz); off+=sz
        else:
            sz,fl,fid=struct.unpack_from('<III',buf,off+4); data=buf[off+24:off+24+sz]
            if fl&0x40000: data=zlib.decompress(data[4:])
            yield t,fid,data; off+=24+sz
for t,fid,d in recs(f,0,len(f)):
    if t!=b'RACE': continue
    o=0; edid=None; rows=[]; cur=None
    while o<len(d):
        st=d[o:o+4]; sz=struct.unpack_from('<H',d,o+4)[0]; b=d[o+6:o+6+sz]; o+=6+sz
        if st==b'EDID': edid=b.rstrip(b'\0')
        elif st==b'ATKD': cur=b
        elif st==b'ATKE' and cur is not None:
            dm,ch,sp,fl,aa,sa,stg,at,kb,rec,stam=struct.unpack_from('<ffIIfffIfff',cur,0)
            rows.append((b.rstrip(b'\0').decode(),ch,fl,aa,sa,stam,dm)); cur=None
    if edid==want:
        for r in sorted(rows): print("%-34s chance %.2f flags 0x%02x angle %6.1f strike %5.1f stam %.2f dmg %.2f"%r)
