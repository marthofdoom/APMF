import pickle,bisect,capstone,re,sys
m=pickle.load(open('al_ae.pkl','rb')); rev={v:k for k,v in m.items()}; starts=sorted(m.values())
img=open('/mnt/gaming/modlists/Projects/marth-follower-overhaul/binaries/1.6.1170/SkyrimSE.unpacked.exe','rb').read()
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
lo,hi=int(sys.argv[1],16),int(sys.argv[2],16)
k0=bisect.bisect_left(starts,lo); k1=bisect.bisect_left(starts,hi)
hits={}
for k in range(k0,k1):
    f=starts[k]; e=starts[k+1]
    ins=list(md.disasm(img[0x400+f-0x1000:0x400+e-0x1000],f))
    for i,x in enumerate(ins):
        mm=re.match(r'(r\w+), qword ptr \[(r\w+) \+ 0x38\]$',x.op_str) if x.mnemonic=='mov' else None
        if not mm: continue
        reg=mm.group(1)
        for y in ins[i+1:i+14]:
            if y.mnemonic in('call','ret','jmp'): break
            m2=re.search(r'\[%s \+ (0x[0-9a-f]+)\]'%reg,y.op_str)
            if m2 and y.mnemonic in('movss','mulss','comiss','addss','ucomiss','subss','divss','maxss','minss','test','mov','movzx','cmp') and 0x20<=int(m2.group(1),16)<=0xa0 and ('ss' in y.mnemonic or int(m2.group(1),16)>=0x9c):
                hits.setdefault((f,rev[f]),[]).append((x.address,y.address,y.mnemonic,y.op_str)); break
            if re.match(r'%s,'%reg,y.op_str) and y.mnemonic in('mov','lea','xor'): break
for (f,i),v in sorted(hits.items()):
    print(f"fn {f:X} id {i}: "+"; ".join(f"{a:X}->{b:X} {mn} {op}" for a,b,mn,op in v[:4]))
