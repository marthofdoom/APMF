import sys,re,struct,pickle,bisect
rt=sys.argv[1]
P='/mnt/gaming/modlists/Projects/marth-follower-overhaul/binaries/'
exe={'ae':P+'1.6.1170/SkyrimSE.unpacked.exe','se':P+'1.5.97/SkyrimSE.unpacked.exe','r17':P+'1.7.104/SkyrimSE.exe'}[rt]
img=open(exe,'rb').read()
try: m=pickle.load(open('al_%s.pkl'%rt,'rb'))
except Exception: m={}
rev={v:k for k,v in m.items()}; starts=sorted(m.values())
def fn(r):
    if not starts: return ''
    k=bisect.bisect_right(starts,r)-1; f=starts[k]; return f"fn {f:X} id {rev[f]} +{r-f:X}"
pat=bytes.fromhex(sys.argv[2])
lim=0x400+0x1600000
for mm in re.finditer(re.escape(pat),img[:lim]):
    r=mm.start()-0x400+0x1000; print(f"{r:X} {fn(r)}")
