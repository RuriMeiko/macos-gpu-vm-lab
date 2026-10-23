import struct,sys
f=open('SystemKernelExtensions.kc','rb'); data=f.read()
def lcs(off):
    magic,cpu,sub,ft,n,sz,fl,_=struct.unpack_from('<IiiIIIII',data,off); p=off+32
    for _ in range(n):
        cmd,size=struct.unpack_from('<II',data,p); yield cmd,size,p; p+=size
want=sys.argv[1] if len(sys.argv)>1 else 'com.apple.driver.AppleParavirtGPU'
ents=[]
for cmd,size,p in lcs(0):
    if cmd==0x80000035:
        vmaddr,fileoff,eid=struct.unpack_from('<QQI',data,p+8)
        name=data[p+eid:p+size].split(b'\0')[0].decode()
        ents.append((name,vmaddr,fileoff))
print(len(ents),"fileset entries")
for n,v,o in ents:
    if 'ParavirtGPU' in n or 'Paravirt' in n: print("  ",n,hex(v),hex(o))
hit=[e for e in ents if e[0]==want]
if not hit: sys.exit("not found: "+want)
name,vm,off=hit[0]
print("carving",name)
segs=[]
for cmd,size,p in lcs(off):
    if cmd==0x19:
        sn=data[p+8:p+24].split(b'\0')[0].decode(); a,vs,fo,fs=struct.unpack_from('<QQQQ',data,p+24); nsects=struct.unpack_from('<I',data,p+64)[0]
        print("  seg",sn,hex(a),hex(vs),"fileoff",hex(fo),"filesz",hex(fs),"nsects",nsects)
        q=p+72
        for _ in range(nsects):
            sect=data[q:q+16].split(b'\0')[0].decode(); sa,ss,so=struct.unpack_from('<QQI',data,q+32); print("     sect",sect,hex(sa),hex(ss),"fileoff",hex(so)); segs.append((sn,sect,sa,ss,so)); q+=80
for sn,sect,sa,ss,so in segs:
    if sect=='__text':
        open('pvg_text.bin','wb').write(data[so:so+ss]); open('pvg_text.addr','w').write(hex(sa)); print("wrote pvg_text.bin",ss,"bytes at",hex(sa))
