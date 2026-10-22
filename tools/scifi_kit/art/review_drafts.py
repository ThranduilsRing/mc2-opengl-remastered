#!/usr/bin/env python3
"""Stock | references | original draft | RTS proxy | damage review sheets."""
import argparse
import csv
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps

import drafts
import inventory as inv

OUT = inv.OUT
ART = OUT.parent / 'art'


def compose(tag):
    source=ART/tag;dest=OUT/'drafts';dest.mkdir(parents=True,exist_ok=True)
    rows={r['id']:r for r in csv.DictReader((OUT/'mapping.csv').open(encoding='utf-8-sig'))}
    contact=Image.new('RGB',(2880,112+300*len(drafts.DRAFTS)),(24,28,33));dc=ImageDraw.Draw(contact)
    dc.text((16,8),'MC2 STOCK UPGRADES / silhouette, proportions and palette belong to MC2 / references donate details only',font=inv.font(25),fill='white')
    for j,label in enumerate(['STOCK','MW5 / BT2018 REFERENCES','BAKED LOD0 / PRISTINE','RTS PROXY / 0.24 m/px','PARTIAL','DESTROYED / RUBBLE']):
        dc.text((j*480+14,58),label,font=inv.font(20),fill='#aebbc6')
    review=[]
    for idx,name in enumerate(drafts.DRAFTS):
        r=rows[name]
        sheet=Image.new('RGB',(2400,1860),(24,28,33));d=ImageDraw.Draw(sheet)
        d.text((18,10),f'{name.upper()} / rank {r["rank"]} / {r["stock_instances"]} stock placements in {r["stock_missions"]} missions',font=inv.font(30),fill='white')
        d.text((18,53),f'Stock bounds: {r["author_width_m"]} x {r["author_depth_m"]} x {r["author_height_m"]} authoring m; WU / 4. {r["identity"]}',font=inv.font(18),fill='#c1cbd2')
        inv.panel(sheet,r['stock_crop'],(12,110,420,320))
        d.text((18,85),'STOCK / footprint, silhouette, palette',font=inv.font(18),fill='white')
        refs=[('MW5 / '+r['mw5_family'],r['mw5_sheet']),('BT2018 / '+r['bt2018_family'],r['bt2018_sheet']),
              ('Godfrey / part & material vocabulary',r['detail_reference']),('Halim / local damage',r['damage_reference'])]
        if r.get('rubble_reference'):refs.append(('BT2018 / generic_rubble_piles / resting debris',r['rubble_reference']))
        for i,(label,p) in enumerate(refs):
            y=475+i*250
            d.text((18,y),label,font=inv.font(16),fill='#aebbc6')
            inv.panel(sheet,p,(12,y+25,420,210),reference=True)
        inv.panel(sheet,source/f'{name}_pristine_beauty.png',(455,110,1080,750))
        d.text((460,85),'UPGRADED MC2 / baked PBR LOD0 / original kit geometry',font=inv.font(22),fill='white')
        inv.panel(sheet,source/'review'/f'{name}_destroyed.png',(1545,110,840,580))
        d.text((1550,85),'DESTROYED / reframed exported-mesh detail',font=inv.font(22),fill='white')
        d.text((1550,725),'RTS PROXY / 360 x 180 / common 0.24 m per pixel',font=inv.font(20),fill='white')
        rts=Image.open(source/f'{name}_pristine_rts.png').convert('RGB')
        sheet.paste(rts.resize((720,360),Image.Resampling.NEAREST),(1590,760))
        d.text((1550,1140),'Offline baked game-mesh view; no engine/game capture.',font=inv.font(17),fill='#b4bec6')
        d.text((460,1200),'CONDITION REVIEW / every rendered state / bounds retained in the export proof',font=inv.font(22),fill='white')
        for j,cond in enumerate(('pristine','worn','abandoned','partial','destroyed')):
            x=450+j*388
            d.text((x+8,1245),cond.upper(),font=inv.font(18),fill='#c4ced5')
            inv.panel(sheet,source/f'{name}_{cond}_beauty.png',(x,1280,380,450))
        d.text((450,1780),r['reference_take'][:185],font=inv.font(16),fill='#acbac5')
        d.text((450,1810),'Reference images are study material only. Draft GLBs contain original kit geometry and authored material classes.',font=inv.font(16),fill='#acbac5')
        sheet.save(dest/f'{name}-review.png')
        cy=102+idx*300
        dc.text((12,cy),f'{r["rank"]}. {name} / {r["stock_instances"]} stock',font=inv.font(19),fill='white')
        inv.panel(contact,r['stock_crop'],(8,cy+28,465,250))
        inv.panel(contact,r['mw5_sheet'],(482,cy+32,230,225),reference=True)
        inv.panel(contact,r['bt2018_sheet'],(722,cy+32,230,225),reference=True)
        inv.panel(contact,source/f'{name}_pristine_beauty.png',(965,cy+25,465,260))
        contact.paste(rts.resize((468,234),Image.Resampling.NEAREST),(1446,cy+35))
        inv.panel(contact,source/f'{name}_partial_beauty.png',(1926,cy+25,465,260))
        inv.panel(contact,source/'review'/f'{name}_destroyed.png',(2406,cy+25,465,260))
        review.append(dict(id=name,rank=int(r['rank']),stock_instances=int(r['stock_instances']),
            stock_size_xyz_m=drafts.SIZES[name],review=(dest/f'{name}-review.png').as_posix(),
            render_directory=source.as_posix(),glb_directory=(source/'pbr').as_posix(),
            reference_families=[r['mw5_family'],r['bt2018_family']],
            geometry_source='original parameterized kit; source meshes/textures never read'))
    contact.save(OUT/'drafts-contact-sheet.png')
    (dest/'draft-manifest.json').write_text(json.dumps(review,indent=2),encoding='utf8')
    print(dest,OUT/'drafts-contact-sheet.png',flush=True)


if __name__=='__main__':
    import psutil
    proc=psutil.Process();proc.cpu_affinity(list(range(psutil.cpu_count()))[2:4])
    proc.nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
    ap=argparse.ArgumentParser();ap.add_argument('--pass',dest='tag',default='blend-d1')
    compose(ap.parse_args().tag)
