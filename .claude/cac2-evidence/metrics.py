import sys, os, glob, json
import numpy as np
from PIL import Image
Image.MAX_IMAGE_PIXELS=None

def lum(p):
    a=np.asarray(Image.open(p).convert('RGB'),dtype=np.float32)
    return 0.299*a[:,:,0]+0.587*a[:,:,1]+0.114*a[:,:,2]

def hardstep(L, thr=16.0):
    dh=np.abs(np.diff(L,axis=1)); dv=np.abs(np.diff(L,axis=0))
    n=dh.size+dv.size
    return 100.0*((dh>thr).sum()+(dv>thr).sum())/n

def cmp(a,b):
    d=np.abs(a-b)
    return 100.0*(d>0.5).mean(), float(d.mean()), float(d.max())

def poses(d):
    return sorted(glob.glob(os.path.join(d,'*.png')))

def atlas_spread(prefix):
    f=prefix+'.rgba'
    if not os.path.exists(f): return None
    a=np.fromfile(f,dtype=np.uint8)
    side=int(round((a.size/4)**0.5)); a=a.reshape(side,side,4)
    B,G,R=a[:,:,0].astype(float),a[:,:,1].astype(float),a[:,:,2].astype(float)
    L=0.299*R+0.587*G+0.114*B
    import csv
    rows=list(csv.DictReader(open(prefix+'.csv')))
    ts=int(rows[0]['tile_px']); gs=int(rows[0]['grid_side'])
    bl=[]
    for k in range(len(rows)):
        if rows[k]['is_alpha']!='0': continue
        y=(k//gs)*ts; x=(k%gs)*ts
        c=L[y:y+ts,x:x+ts].ravel(); s=np.sort(c)
        p50,p90=s[len(s)//2],s[int(len(s)*.9)]
        bl.append(c[(c>=p50)&(c<=p90)].mean())
    bl=np.array(bl)
    return dict(n=len(bl),min=float(bl.min()),max=float(bl.max()),
                spread=float(bl.max()-bl.min()),std=float(bl.std()),median=float(np.median(bl)))

m=sys.argv[1]
base='scratch/cac2/caps'
res={'mission':m,'poses':{},'atlas':{}}
for arm in ['OFF','A','B']:
    sp=atlas_spread('scratch/cac2/atlas/%s_%s'%(m,arm))
    if sp: res['atlas'][arm]=sp
off=poses(os.path.join(base,'%s_OFF'%m))
for p in off:
    name=os.path.basename(p)
    row={}
    Lo=lum(p)
    row['hardstep_OFF']=hardstep(Lo)
    f2=os.path.join(base,'%s_OFF_r2'%m,name)
    if os.path.exists(f2):
        c=cmp(Lo,lum(f2)); row['floor_pct']=c[0]; row['floor_mean']=c[1]; row['floor_max']=c[2]
    for arm in ['A','B']:
        fa=os.path.join(base,'%s_%s'%(m,arm),name)
        if os.path.exists(fa):
            La=lum(fa); c=cmp(Lo,La)
            row['%s_pct'%arm]=c[0]; row['%s_mean'%arm]=c[1]; row['%s_max'%arm]=c[2]
            row['hardstep_%s'%arm]=hardstep(La)
    res['poses'][name]=row
print(json.dumps(res,indent=1))
json.dump(res,open('scratch/cac2/metrics_%s.json'%m,'w'),indent=1)
