# usage: re.py <rt> dis <rva|idN> [len]   | xref <rva|idN> | vt <vtable-rva|idN> <slot>... | str <text> | rev <rva> | ptr <rva>(find abs pointers to rva)
import sys, struct, pickle, os, re, bisect
P='/mnt/gaming/modlists/Projects'
sys.path.insert(0,P+'/marth-follower-overhaul/tools/verified_addresses')
import gen_verified_addresses as g
import capstone
RT={'ae':(P+'/marth-follower-overhaul/binaries/1.6.1170/SkyrimSE.unpacked.exe','/mnt/gaming/modlists/LoreRim/mods/Address Library for SKSE Plugins/SKSE/Plugins/versionlib-1-6-1170-0.bin',2,(1,6,1170,0)),
    'se':(P+'/marth-follower-overhaul/binaries/1.5.97/SkyrimSE.unpacked.exe',P+'/_commonlib/mit-3.7-fork/tests/REL/version-1-5-97-0.bin',1,(1,5,97,0)),
    'r17':(P+'/marth-follower-overhaul/binaries/1.7.104/SkyrimSE.exe',None,0,None)}
rt=sys.argv[1]; exe,al,fmt,ver=RT[rt]
D=os.path.dirname(os.path.abspath(__file__))
m={}
if al:
    C=os.path.join(D,'al_%s.pkl'%rt)
    if os.path.exists(C): m=pickle.load(open(C,'rb'))
    else: m=g.load_addrlib(al,fmt,ver); pickle.dump(m,open(C,'wb'))
rev={v:k for k,v in m.items()}; starts=sorted(m.values())
img=open(exe,'rb').read()
pe=struct.unpack_from('<I',img,0x3c)[0]; ns=struct.unpack_from('<H',img,pe+6)[0]; so=pe+24+struct.unpack_from('<H',img,pe+20)[0]
secs=[]
for i in range(ns):
    o=so+40*i; nm=img[o:o+8].rstrip(b'\0'); vs,va,rs,rp=struct.unpack_from('<IIII',img,o+8); secs.append((va,max(vs,rs),rp,rs,nm))
B=0x140000000
def raw(r):
    for va,vs,rp,rs,nm in secs:
        if va<=r<va+vs: return rp+r-va
    raise ValueError('rva %x not mapped'%r)
def rva_of(off):
    for va,vs,rp,rs,nm in secs:
        if rp<=off<rp+rs: return va+off-rp
    return None
def A(a):
    if a.startswith('id'): return m[int(a[2:])]
    return int(a,16)
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
def fn_of(r):
    if not starts: return None
    k=bisect.bisect_right(starts,r)-1
    return starts[k] if k>=0 else None
def nm(r):
    return f" [id {rev[r]}]" if r in rev else ""
def cstr(r):
    try:
        o=raw(r); e=img.index(b'\0',o); s=img[o:e]
        if 2<len(s)<80 and all(32<=c<127 for c in s): return s.decode()
    except Exception: pass
    return None
def dis(r,n,stop=True):
    for i in md.disasm(img[raw(r):raw(r)+n],B+r):
        ex=""
        if i.mnemonic in('call','jmp') and i.op_str.startswith('0x'): ex=nm(int(i.op_str,16)-B)
        mm=re.search(r'rip ([+-]) (0x[0-9a-f]+)',i.op_str)
        if mm:
            t=i.address+i.size+(int(mm.group(2),16) if mm.group(1)=='+' else -int(mm.group(2),16))-B
            ex+=f" ; ->{t:X}"+nm(t)
            s=cstr(t)
            if s: ex+=f' "{s}"'
        print(f"{i.address-B:08X} {i.mnemonic} {i.op_str}{ex}")
        if stop and i.mnemonic=='int3': break
cmd=sys.argv[2]
if cmd=='dis':
    r=A(sys.argv[3]); print(f"; {rt} {r:X}{nm(r)}"); dis(r,int(sys.argv[4],0) if len(sys.argv)>4 else 0x200)
elif cmd=='xref':
    t=A(sys.argv[3]); va,vs,rp,rs,_=secs[0]
    for mm in re.finditer(rb'[\xe8\xe9]',img[rp:rp+rs]):
        p=mm.start()+rp; rel=struct.unpack_from('<i',img,p+1)[0]; r=va+p-rp
        if r+5+rel==t:
            f=fn_of(r); print(f"{'call' if img[p]==0xe8 else 'jmp'}@{r:X} in fn {f:X}{nm(f)} +{r-f:X}" if f else f"@{r:X}")
    # rip-relative lea refs
    for mm in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]',img[rp:rp+rs]):
        p=mm.start()+rp; rel=struct.unpack_from('<i',img,p+3)[0]; r=va+p-rp
        if r+7+rel==t:
            f=fn_of(r); print(f"lea@{r:X} in fn {f:X}{nm(f)} +{r-f:X}" if f else f"lea@{r:X}")
elif cmd=='ptr':
    t=A(sys.argv[3]); pat=struct.pack('<Q',B+t)
    for mm in re.finditer(re.escape(pat),img):
        r=rva_of(mm.start()); print(f"ptr@{r:X}" if r else f"ptr@raw{mm.start():X}")
elif cmd=='vt':
    base=A(sys.argv[3])
    for s in sys.argv[4:]:
        s=int(s,16); a=struct.unpack_from('<Q',img,raw(base+8*s))[0]-B; print(f"vt {base:X} slot {s:X} -> {a:X}{nm(a)}")
elif cmd=='str':
    t=sys.argv[3].encode()
    for mm in re.finditer(re.escape(t),img):
        r=rva_of(mm.start()); print(f"{r:X}: {cstr(r)}" if r else mm.start())
elif cmd=='rev':
    r=int(sys.argv[3],16); f=fn_of(r); print(rev.get(r), 'fn', hex(f) if f else None, rev.get(f))
elif cmd=='rip':   # find rip-relative references (any instr) to rva: brute scan disp32
    t=A(sys.argv[3]); va,vs,rp,rs,_=secs[0]; blob=img[rp:rp+rs]
    for off in range(0,len(blob)-4):
        rel=struct.unpack_from('<i',blob,off)[0]
        if va+off+4+rel==t:
            r=va+off; f=fn_of(r); print(f"ref@{r:X} in fn {f:X}{nm(f)} +{r-f:X}" if f else f"ref@{r:X}")
