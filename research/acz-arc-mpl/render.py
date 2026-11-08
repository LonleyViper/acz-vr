import numpy as np, matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt, sys
o=sys.argv[1]
sets=[("itesu part2 (23,910 v)",np.load(o+"/itesu_p2.obj.npy"),'tab:red'),("f16ck part2 (5,855 v)",np.load(o+"/f16ck_p2.obj.npy"),'tab:blue')]
body=np.load(o+"/f16ck_p0.obj.npy")
fig,ax=plt.subplots(2,3,figsize=(15,9))
views=[("side (z fwd/back vs y)",2,1),("top (x vs z)",0,2),("front (x vs y)",0,1)]
for r,(name,pts,c) in enumerate(sets):
    for k,(t,a,b) in enumerate(views):
        A=ax[r][k]; A.scatter(body[:,a],body[:,b],s=0.3,c='lightgray'); A.scatter(pts[:,a],pts[:,b],s=0.4,c=c)
        A.set_title(name+" - "+t,fontsize=9); A.set_aspect('equal'); A.grid(alpha=.3)
plt.tight_layout(); plt.savefig(o+"/cockpit_compare.png",dpi=80)
