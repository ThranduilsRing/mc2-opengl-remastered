"""Rebuild seed-42 before/after sheets and regional 1 wu/px detail evidence.

Run after generate.py has baked 1024 and 3072 into docs/mapgen/raster/generated.
The historical generator artifacts come from local git objects, never installs.
"""
import argparse
import gzip
import json
import subprocess
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from tools.mapgen.common import load_json,output_path,write_json
from tools.mapgen.raster import render
from tools.mapgen.scale_metrics import measure_scale


def baseline(size, revision):
    suffix = '.json.gz' if size>1024 else '.json'
    path = f'docs/mapgen/raster/generated/generated_{size}_42{suffix}'
    raw = subprocess.run(['git','show',f'{revision}:{path}'],capture_output=True,check=True).stdout
    return json.loads(gzip.decompress(raw) if suffix.endswith('.gz') else raw)


def regional_crops(layout):
    result = []
    for region in ('urban_core','industrial'):
        bases = [b for b in layout['bases'] if b.get('region')==region]
        base = max(bases,key=lambda b:b['requested_population'])
        members = [o for o in layout['objects'] if o.get('base_id')==base['id'] and o['class']=='building']
        centre = np.median([o['cell'] for o in members],axis=0) if members else np.mean(np.array(base['bounds']).reshape(2,2),axis=0)
        result.append((region,centre))
    trees = [o['cell'] for o in layout['objects'] if o['class']=='tree']
    result.append(('wilderness',trees[len(trees)//2]))
    size = layout['size']['rows']
    crops = []
    for label,centre in result:
        r,c = [min(size-24,max(0,int(x)-12)) for x in centre]
        crops.append((label,[r,c,r+24,c+24]))
    return crops


def review(size,revision,out):
    folder = Path('docs/mapgen/raster/generated')
    name = f'generated_{size}_42'
    after = load_json(folder/(name+('.json.gz' if size>1024 else '.json')))
    before = baseline(size,revision)
    png = subprocess.run(['git','show',f'{revision}:docs/mapgen/raster/generated/{name}.png'],
                         capture_output=True,check=True).stdout
    output_path(out/f'before_{size}.png').write_bytes(png)
    before_metrics,after_metrics = measure_scale(before),measure_scale(after)
    write_json(out/f'metrics_{size}.json',dict(before=before_metrics,after=after_metrics),pretty=True)
    crops = regional_crops(after)
    # Same world locations, scale and palette on both sides.
    for label,bounds in crops:
        for side,layout in (('before',before),('after',after)):
            render(layout,out/f'{side}_{size}_{label}_1wu.png',1.,bounds)
    del before,after
    try:
        font = ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',24)
        small = ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',19)
    except OSError:
        font,small = ImageFont.load_default(size=24),ImageFont.load_default(size=19)
    width = min(1536,Image.open(folder/(name+'.png')).width)
    margin,header = 16,125
    thumb = (width-4*margin)//3
    sheet = Image.new('RGB',(2*width+3*margin,header+width+thumb+90),(27,31,29))
    draw = ImageDraw.Draw(sheet)
    for column,(side,source,metrics) in enumerate((
        ('before',out/f'before_{size}.png',before_metrics),
        ('after',folder/(name+'.png'),after_metrics))):
        x = margin+column*(width+margin)
        draw.text((x,12),f'{size} cells / seed 42 / {side.upper()}',font=font,fill='white')
        draw.text((x,48),f"Site NN CV {metrics['parcel_nn_cv']:.2f} | empty land {metrics['empty_land_fraction']:.0%} | tree CV {metrics['tree_bin_cv']:.2f}",font=small,fill='white')
        draw.text((x,78),f"Parcel area range {metrics['parcel_area_max_min']:.1f}:1 | full map: {width}px sheet panel",font=small,fill='white')
        with Image.open(source) as full:
            sheet.paste(full.resize((width,width),Image.Resampling.NEAREST),(x,header))
        for i,(label,bounds) in enumerate(crops):
            xx,yy = x+margin+i*(thumb+margin),header+width+12
            draw.text((xx,yy),label.replace('_',' '),font=small,fill='white')
            with Image.open(out/f'{side}_{size}_{label}_1wu.png') as detail:
                sheet.paste(detail.resize((thumb,thumb),Image.Resampling.NEAREST),(xx,yy+30))
    path = out/f'before_after_{size}.png'
    sheet.save(output_path(path),optimize=True)
    write_json(out/f'crops_{size}.json',dict(baseline=revision,wu_per_pixel=1,crops=[dict(region=k,cells=b) for k,b in crops]),pretty=True)
    print(path,after_metrics,flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--size',type=int,choices=(1024,3072),nargs='+',default=[1024,3072])
    parser.add_argument('--baseline',default='6c0b66726')
    parser.add_argument('--out',type=Path,default=Path('docs/mapgen/scale-realism'))
    args = parser.parse_args()
    out = output_path(args.out)
    out.mkdir(parents=True,exist_ok=True)
    for size in args.size:
        review(size,args.baseline,out)


if __name__=='__main__':
    main()
