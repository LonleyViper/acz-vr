import struct, math, numpy as np
d=open(r"D:/SteamLibrary/steamapps/common/ACE COMBAT 8/ACZero/acz.exe",'rb').read()
secs=[('.text',2596864,1024,0x140001000),('.rdata',581632,2597888,0x140000000+2600960),('.data',112128,3179520,0x140000000+3182592),('_RDATA',88576,3390464,0x140000000+58691584),('.rsrc',272384,3479552,0x140000000+58785792)]
tx,ty=0.6152,0.3460
T={'tanx':(0.6152,1e-4),'tany':(0.3460,1.5e-4),'1/tanx':(1.6255,1e-4),'1/tany':(2.8902,1e-4),
'fullx_rad':(1.1030,2e-4),'fully_rad':(0.6663,2e-4),'halfx_rad':(0.5515,2e-4),'halfy_rad':(0.3331,2e-4),
'fullx_deg':(63.2,2e-3),'fully_deg':(38.1,2e-3),'halfx_deg':(31.6,2e-3),'halfy_deg':(19.05,2e-3),
'tanx_x16':(9.8432,1e-4),'tany_x16':(5.536,1.5e-4),'tanx_x64':(39.3728,1e-4),'tany_x64':(22.144,1.5e-4),
'itanx_x16':(26.008,1e-4),'itany_x16':(46.243,1e-4),'itanx_x64':(104.03,1e-4),'itany_x64':(184.97,1e-4),
'aspect169':(16/9,1e-6),'sx258.048':(258.048,1e-6),'sy225.792':(225.792,1e-6),'fx256/t':(416.12,1e-4),'fy224/t':(647.4,1.5e-4),'f258/t':(419.45,1e-4),'f225/t':(652.58,1.5e-4)}
out={}
for n,sz,fo,va in secs[1:]:
    b=d[fo:fo+sz]
    for dt,w,code in (('<f4',4,'f32'),('<f8',8,'f64')):
        for al in range(w):
            m=(len(b)-al)//w
            a=np.frombuffer(b,dtype=dt,count=m,offset=al)
            for k,(v,t) in T.items():
                idx=np.nonzero(np.abs(a-v)<=abs(v)*t)[0]
                for i in idx:
                    o=int(i)*w+al
                    out.setdefault(k,[]).append((n,code,fo+o,va+o,float(a[i]),al==0))
for k,(v,t) in T.items():
    L=out.get(k,[])
    al=[x for x in L if x[5]]
    print(k,v,'total',len(L),'aligned',len(al))
    for x in al[:6]: print('    ',x[0],x[1],hex(x[2]),hex(x[3]),x[4])
