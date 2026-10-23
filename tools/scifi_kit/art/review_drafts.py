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


STOCK_VIEWS = OUT.parent / 'studies' / 'stock-views'
SIDES = ('front', 'left', 'rear', 'right', 'top')


def _cell(im, path, box, nearest=False):
    from pathlib import Path as _P
    if not _P(path).is_file():
        return
    src = Image.open(path).convert('RGB')
    if nearest:
        t = src.resize((box[2], box[2] * src.height // src.width), Image.Resampling.NEAREST)
    else:
        t = ImageOps.contain(src, (box[2], box[3]), Image.Resampling.LANCZOS)
    im.paste(t, (box[0] + (box[2] - t.width) // 2, box[1] + (box[3] - t.height) // 2))


def count_sheet(tag, out=None):
    """Stock-side-N next to remake-side-N, all five sides (full stock model, never one iso)."""
    out = out or OUT / f'count-sides-{tag}.png'
    cw, ch, head, lab = 210, 150, 92, 170
    W = lab + len(SIDES) * 2 * cw + 20
    im = Image.new('RGB', (W, head + len(drafts.DRAFTS) * (ch + 10)), (24, 28, 33))
    d = ImageDraw.Draw(im)
    d.text((14, 8), f'COUNT / SILHOUETTE PROOF: decoded stock model (textured) vs remake LOD0, same camera per side  /  pass {tag}', font=inv.font(22), fill='white')
    for j, side in enumerate(SIDES):
        x = lab + j * 2 * cw
        d.text((x + 8, 56), f'stock / {side}', font=inv.font(15), fill='#c9c39a')
        d.text((x + cw + 8, 56), f'remake / {side}', font=inv.font(15), fill='#96c8a0')
        d.line([(x - 3, 50), (x - 3, im.height)], fill=(70, 76, 80), width=2)
    for i, name in enumerate(drafts.DRAFTS):
        y = head + i * (ch + 10)
        d.text((12, y + ch // 2 - 12), name, font=inv.font(18), fill='white')
        for j, side in enumerate(SIDES):
            x = lab + j * 2 * cw
            _cell(im, STOCK_VIEWS / f'{name}_stock_{side}.png', (x + 2, y, cw - 4, ch))
            _cell(im, ART / tag / f'{name}_count_{side}.png', (x + cw + 2, y, cw - 4, ch))
    im.save(out)
    print(out, flush=True)
    return out


def before_after(before, after, out=OUT / 'opus-before-after.png'):
    """Stock sides | round-2 | art-bar pass. Stock front/left views sit next to the
    matching before/after side views, then pristine, RTS proxy, partial, destroyed."""
    cols = [('stock front', lambda n: STOCK_VIEWS / f'{n}_stock_front.png', None),
            ('before front', lambda n: ART / before / f'{n}_count_front.png', None),
            ('after front', lambda n: ART / after / f'{n}_count_front.png', None),
            ('stock left', lambda n: STOCK_VIEWS / f'{n}_stock_left.png', None),
            ('before left', lambda n: ART / before / f'{n}_count_left.png', None),
            ('after left', lambda n: ART / after / f'{n}_count_left.png', None)]
    for label, pat, nearest in (('pristine', '{n}_pristine_beauty.png', False), ('RTS 0.24 m/px', '{n}_pristine_rts.png', True),
                                ('partial', '{n}_partial_beauty.png', False), ('destroyed', '{n}_destroyed_beauty.png', False)):
        cols.append(('before ' + label, (lambda n, p=pat: ART / before / p.format(n=n)), nearest))
        cols.append(('after ' + label, (lambda n, p=pat: ART / after / p.format(n=n)), nearest))
    cw, ch, head, lab = 200, 150, 92, 160
    W = lab + len(cols) * cw + 20
    im = Image.new('RGB', (W, head + len(drafts.DRAFTS) * (ch + 10)), (24, 28, 33))
    d = ImageDraw.Draw(im)
    d.text((14, 8), f'BEFORE (round 2, {before})  vs  AFTER (art bar, {after})  /  stock sides from the full decoded model  /  construction order, stock silhouette and palette', font=inv.font(22), fill='white')
    colour = {'stock': '#c9c39a', 'before': '#d29a8c', 'after': '#96c8a0'}
    for j, (label, _, _) in enumerate(cols):
        x = lab + j * cw
        d.text((x + 6, 58), label, font=inv.font(14), fill=colour[label.split()[0]])
        if label.startswith(('stock', 'before')) and (j == 0 or not cols[j - 1][0].startswith('stock')):
            d.line([(x - 3, 50), (x - 3, im.height)], fill=(70, 76, 80), width=2)
    for i, name in enumerate(drafts.DRAFTS):
        y = head + i * (ch + 10)
        d.text((12, y + ch // 2 - 12), name, font=inv.font(17), fill='white')
        for j, (_, fn, nearest) in enumerate(cols):
            _cell(im, fn(name), (lab + j * cw + 2, y, cw - 4, ch), nearest=bool(nearest))
    im.save(out)
    print(out, flush=True)


if __name__=='__main__':
    import psutil
    proc=psutil.Process();proc.cpu_affinity(list(range(psutil.cpu_count()))[2:4])
    proc.nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
    ap=argparse.ArgumentParser();ap.add_argument('--pass',dest='tag',default='blend-d1')
    ap.add_argument('--before',help='also write mapping/opus-before-after.png against this pass')
    a=ap.parse_args()
    compose(a.tag)
    count_sheet(a.tag)
    if a.before:before_after(a.before,a.tag)
