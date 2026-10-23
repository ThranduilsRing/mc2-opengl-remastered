"""Read-only stock texture sampling and transparent rendered-object palette audit."""
import colorsys
import hashlib
import json
import math
from pathlib import Path
import numpy as np
from PIL import Image

STOCK=Path('G:/mc2-assets/out/scifi-kit/round2/stock')


def stats(rgb,weights):
    rgb=np.asarray(rgb,dtype=float);weights=np.asarray(weights,dtype=float)
    hsv=np.array([colorsys.rgb_to_hsv(*c) for c in rgb])
    chroma=weights*hsv[:,1]
    angle=np.angle(np.sum(chroma*np.exp(hsv[:,0]*2j*math.pi)))%(2*math.pi)
    return dict(mean_srgb=np.average(rgb,axis=0,weights=weights).tolist(),
                mean_hue_degrees=float(angle*180/math.pi),
                mean_saturation=float(np.average(hsv[:,1],weights=weights)),
                mean_value=float(np.average(hsv[:,2],weights=weights)))


def stock(name):
    d=json.loads((STOCK/(name+'.json')).read_text())
    images=[np.array(Image.open(p).convert('RGBA')) if p else None for p in d['texture_paths']]
    rgb=[];weights=[]
    # Deterministic equal-density barycentric samples, weighted by physical face
    # area: unused atlas tiles and emissive/shadow companion sheets do not bias it.
    for i,mi in enumerate(d['materials']):
        if mi>=len(images) or images[mi] is None or d['texture_alpha'][mi]:continue
        v=np.array(d['vertices_m'][i*3:i*3+3]);uv=np.array(d['uv'][i*3:i*3+3])
        area=np.linalg.norm(np.cross(v[1]-v[0],v[2]-v[0]))*.5
        im=images[mi];h,w=im.shape[:2]
        for a in range(12):
            for b in range(12-a):
                u=(a+1/3)/12;t=(b+1/3)/12;p=uv[0]*(1-u-t)+uv[1]*u+uv[2]*t
                x=int(p[0]*w)%w;y=int(p[1]*h)%h
                pixel=im[y,x]
                if pixel[3]<128 or (pixel[0]>240 and pixel[2]>240 and pixel[1]<20):continue
                rgb.append(pixel[:3]/255);weights.append(area/78)
    result=stats(rgb,weights)
    result['method']='opaque used-UV texture samples; 78 barycentric samples/face; physical face-area weighted; HSV circular hue weighted by saturation'
    result['textures']=[dict(path=p,sha256=hashlib.sha256(Path(p).read_bytes()).hexdigest()) for p in d['texture_paths'] if p]
    return result


def compare(name,path):
    baseline=stock(name)
    im=np.array(Image.open(path).convert('RGBA'),dtype=float)/255
    pixels=im[im[:,:,3]>.95]
    rendered=stats(pixels[:,:3],np.ones(len(pixels)))
    dh=(rendered['mean_hue_degrees']-baseline['mean_hue_degrees']+180)%360-180
    return dict(stock=baseline,render=rendered,render_path=str(path),foreground_pixels=len(pixels),
                delta_hue_degrees=dh,delta_value=rendered['mean_value']-baseline['mean_value'],
                caveat='Stock texture vs lit AgX render; not identical illumination or projection. Hue unstable on neutral grey. Alpha>.95 excludes ground/sky/antialias edges.')
