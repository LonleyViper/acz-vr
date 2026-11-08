import struct, sys, math, os
import numpy as np
def part(f,i):
    b=open(f,'rb').read(); n=struct.unpack_from('<I',b,0)[0]; o=list(struct.unpack_from('<%dI'%n,b,4))+[len(b)]
    return b[o[i]:o[i+1]]
def scan(p):
    """Walk 4-byte words; a VIF UNPACK V3-32 (0x68) with num>=1 followed by sane floats = a position batch."""
    batches=[]; i=336; n=len(p)//4
    w=np.frombuffer(p[:n*4],dtype='<u4'); f=w.view('<f4')
    k=84
    while k<n-4:
        x=int(w[k]); cmd=x>>24; num=(x>>16)&0xff
        if cmd==0x68 and 1<=num<=200 and k+1+num*3<=n:
            v=f[k+1:k+1+num*3]
            if np.all(np.isfinite(v)) and np.all(np.abs(v)<5000):
                batches.append((k*4,num,v.reshape(num,3).copy())); k+=1+num*3; continue
        k+=1
    return batches
if __name__=="__main__":
    fn,idx,out=sys.argv[1],int(sys.argv[2]),sys.argv[3]
    p=part(fn,idx); bs=scan(p)
    pts=np.vstack([b[2] for b in bs]) if bs else np.zeros((0,3))
    print(fn,idx,"bytes",len(p),"batches",len(bs),"verts",len(pts),"sizes",sorted({b[1] for b in bs}))
    if len(pts):
        print(" bbox min",pts.min(0).round(2),"max",pts.max(0).round(2),"extent",(pts.max(0)-pts.min(0)).round(2))
    # OBJ: each batch as a tristrip (v0 v1 v2, v1 v3 v2 for 4-vertex quads)
    with open(out,'w') as o:
        base=1
        for _,num,v in bs:
            for q in v: o.write("v %f %f %f\n"%tuple(q))
            for t in range(num-2):
                a,b,c=(base+t,base+t+1,base+t+2) if t%2==0 else (base+t+1,base+t,base+t+2)
                o.write("f %d %d %d\n"%(a,b,c))
            base+=num
    np.save(out+".npy",pts)
