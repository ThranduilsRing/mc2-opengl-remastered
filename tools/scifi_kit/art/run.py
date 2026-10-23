#!/usr/bin/env python3
"""Art-directed kit CLI: render primitive contact sheets and building assemblies.

CPU rules: exactly one Blender at a time, --threads 2, Cycles threads 2, below-normal
priority, and CPU affinity that never includes core 0. Rendering uses the GPU when
Cycles finds one.

  py -3 tools/scifi_kit/art/run.py prims --pass p1 [--only wall_panel,tank] [--samples 48]
  py -3 tools/scifi_kit/art/run.py sheet --pass p1
  py -3 tools/scifi_kit/art/run.py build --pass b1 [--only barracks]
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / 'tools/blender_pipeline'))
from blender_runner import find_blender_exe  # noqa: E402

OUT = Path('E:/mc2-ref/scifi-kit/art')
THUMBS = Path('E:/mc2-archive-2026-10/asset-modernize/docs/assets/sheets/thumbs')
CONDITIONS = ['pristine', 'worn', 'abandoned', 'partial', 'destroyed']


def blender(job, log):
    import psutil
    path = OUT / 'jobs' / (job['tag'] + '.json')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(job, indent=1), encoding='utf-8')
    log.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(find_blender_exe()), '--background', '--factory-startup', '--threads', '2', '--python-exit-code', '1',
           '--python', str(HERE / 'blend.py'), '--', str(path)]
    print('[art]', ' '.join(cmd), flush=True)
    sys.path.insert(0,str(ROOT/'scripts'))
    from console_child import child_kwargs
    kwargs=child_kwargs(redirected=True)
    kwargs['creationflags']=kwargs.get('creationflags',0)|0x00004000
    with log.open('w', encoding='utf-8') as fh:
        proc = subprocess.Popen(cmd, stdout=fh, stderr=subprocess.STDOUT, **kwargs)
        try:
            cores = list(range(psutil.cpu_count()))
            psutil.Process(proc.pid).cpu_affinity(cores[2:4])
        except Exception as exc:  # affinity is best effort; threads are still 2
            print('[art] affinity not set:', exc)
        rc = proc.wait()
    if rc:
        print(log.read_text(encoding='utf-8', errors='replace')[-5000:])
        raise SystemExit(f'Blender failed ({rc}); log {log}')


def font(size):
    from PIL import ImageFont
    return ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', size)


def stock_crop(stock_id, box):
    from PIL import Image, ImageOps
    src = THUMBS / f'{stock_id}.png'
    alt = Path('G:/mc2-assets/out/scifi-kit/round2/stock/previews') / f'{stock_id}.png'
    if alt.is_file():
        src = alt
    if not src.is_file():
        return None
    im = Image.open(src)
    if im.mode in ('RGBA', 'LA', 'P'):
        im = im.convert('RGBA')
        bg = Image.new('RGBA', im.size, (32, 36, 44, 255))
        bg.alpha_composite(im)
        im = bg
    im = im.convert('RGB')
    return ImageOps.contain(im, box, Image.Resampling.LANCZOS)


def sheet(pass_dir, names, title, out_name, cell=360):
    from PIL import Image, ImageDraw
    sys.path.insert(0, str(HERE))
    import geo
    cols = ['stock'] + CONDITIONS
    head = 70
    rowh = cell + 34
    pages = [names[i:i + 6] for i in range(0, len(names), 6)]
    outs = []
    for pi, page in enumerate(pages):
        im = Image.new('RGB', (cell * len(cols) + 20, head + rowh * len(page)), (24, 28, 32))
        d = ImageDraw.Draw(im)
        d.text((12, 10), title + (f'  ({pi + 1}/{len(pages)})' if len(pages) > 1 else ''), font=font(26), fill=(230, 232, 228))
        for ci, c in enumerate(cols):
            d.text((14 + ci * cell, 44), c.upper(), font=font(17), fill=(170, 176, 170))
        for ri, name in enumerate(page):
            y = head + ri * rowh
            _, palette, stock = geo.PRIMITIVES.get(name, (None, '', name))
            d.text((14, y + 4), f'{name}  /  palette {palette}  /  stock {stock}', font=font(18), fill=(225, 228, 222))
            crop = stock_crop(stock, (cell - 16, cell - 16))
            if crop:
                im.paste(crop, (8 + (cell - crop.width) // 2, y + 30 + (cell - crop.height) // 2))
            for ci, c in enumerate(CONDITIONS, 1):
                p = pass_dir / f'{name}_{c}.png'
                if p.is_file():
                    t = Image.open(p).convert('RGB').resize((cell - 8, cell - 8), Image.Resampling.LANCZOS)
                    im.paste(t, (ci * cell + 4, y + 30))
        path = pass_dir.parent / f'{out_name}-{pi + 1:02d}.png'
        im.save(path)
        outs.append(path)
        print('[art] sheet', path)
    return outs


SOL = Path('G:/mc2-assets/out/scifi-kit/round2/parts')
SOL_NAME = {'quonset_shell': 'barrel_roof'}


def before_after(pass_dir, names, out_name='before-after'):
    """Sol round-2 vs art-directed rework, same primitive per row."""
    from PIL import Image, ImageDraw
    conds = ['pristine', 'abandoned', 'destroyed']
    cell = 260
    head = 96
    pages = [names[i:i + 6] for i in range(0, len(names), 6)]
    for pi, page in enumerate(pages):
        W = 200 + cell * 6 + 40
        im = Image.new('RGB', (W, head + len(page) * (cell + 10)), (24, 28, 32))
        d = ImageDraw.Draw(im)
        d.text((12, 10), f'BEFORE (Sol round 2)  vs  AFTER (art-directed kit, pass {pass_dir.name})  ({pi + 1}/{len(pages)})', font=font(26), fill=(232, 234, 230))
        for i, c in enumerate(conds):
            d.text((200 + i * cell + 8, 56), 'before / ' + c, font=font(16), fill=(200, 150, 140))
            d.text((200 + (i + 3) * cell + 48, 56), 'after / ' + c, font=font(16), fill=(150, 200, 160))
        d.line([(200 + 3 * cell + 20, 50), (200 + 3 * cell + 20, im.height)], fill=(90, 96, 100), width=3)
        for ri, name in enumerate(page):
            y = head + ri * (cell + 10)
            d.text((12, y + cell // 2 - 12), name, font=font(19), fill=(225, 228, 222))
            for i, c in enumerate(conds):
                a = SOL / f'{SOL_NAME.get(name, name)}_{c}.png'
                b = pass_dir / f'{name}_{c}.png'
                for path, x in ((a, 200 + i * cell), (b, 200 + (i + 3) * cell + 40)):
                    if path.is_file():
                        t = Image.open(path).convert('RGB').resize((cell - 6, cell - 6), Image.Resampling.LANCZOS)
                        im.paste(t, (x, y))
        path = pass_dir.parent / f'{out_name}-{pi + 1:02d}.png'
        im.save(path)
        print('[art] before/after', path)


def main():
    import psutil
    parent = psutil.Process()
    # The child inherits this affinity at spawn, before its own explicit pin.
    parent.cpu_affinity(list(range(psutil.cpu_count()))[2:4])
    parent.nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('command', choices=['prims', 'sheet', 'build', 'beforeafter'])
    ap.add_argument('--pass', dest='tag', required=True)
    ap.add_argument('--only')
    ap.add_argument('--samples', type=int, default=48)
    ap.add_argument('--conditions', default=','.join(CONDITIONS))
    ap.add_argument('--no-export', action='store_true')
    a = ap.parse_args()
    sys.path.insert(0, str(HERE))
    import geo
    names = a.only.split(',') if a.only else list(geo.PRIMITIVES)
    pass_dir = OUT / a.tag
    if a.command == 'prims':
        job = {'kind': 'primitives', 'tag': a.tag, 'out': str(pass_dir), 'primitives': names, 'conditions': a.conditions.split(','),
               'samples': a.samples, 'export': not a.no_export}
        blender(job, OUT / 'logs' / f'{a.tag}.log')
        sheet(pass_dir, names, f'Sci-fi kit primitives / pass {a.tag} / stock | pristine | worn | abandoned | destroyed', f'contact-{a.tag}')
    elif a.command == 'sheet':
        sheet(pass_dir, names, f'Sci-fi kit primitives / pass {a.tag} / stock | pristine | worn | abandoned | destroyed', f'contact-{a.tag}')
    elif a.command == 'beforeafter':
        before_after(pass_dir, names)
    elif a.command == 'build':
        import assemble
        assemble.run(a, OUT, blender)


if __name__ == '__main__':
    main()
