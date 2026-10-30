"""Temporary owner A/B experiment; delete with MC2_EXPERIMENT_TEMPORAL_AB.

Run capture, perf, report in that order. Uses only existing background harnesses.
Camera tracks use 30 rendered frames/second of nominal camera time; simulation
and foliage are frozen at the same seeded trigger frame in every arm.
"""
import argparse
import html
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'scripts'), str(ROOT / 'tools/perf')]
import run_visual_capture as capture
from smoke_lib import host_lease, runner
from mission_cpu_profile import HostSamples, noise_report

OUT = Path('E:/mc2-archive-2026-10/proofs/temporal-ab')
INSTALL = Path('E:/mc2-installs/claude-temporal-ab')
MODES = {'A_smaa': 1, 'C_taa': 3, 'D_fsr1': 4, 'E_spatial': 5}
MISSIONS = ['mc2_01', 'mc2_10', 'mc2_24']


def track(mission):
    original = json.loads((ROOT / f'tests/visual/bookmarks/{mission}_camera.json').read_text())
    orbit = next(b for b in original['bookmarks'] if b['name'] == 'orbit')
    low = next(b for b in original['bookmarks'] if b['name'] == 'low_horizon_0')
    marks = []
    for group, pose, count, degrees in [('warm', orbit, 40, 0), ('hold', orbit, 30, 0),
            ('pan', orbit, 30, 3), ('orbit', orbit, 301, 360),
            ('low_warm', low, 30, 0), ('low', low, 121, 360)]:
        for i in range(count):
            mark = dict(pose, name=f'{group}_{i:03}', rot=(pose['rot'] + degrees*i/max(1,count-1)) % 360)
            mark.pop('sweep', None)
            marks.append(mark)
    return dict(schema_v=1, mission=mission, bookmarks=marks)


def declared(mode, width, height):
    return {'MC2_EXPERIMENT_TEMPORAL_AB': str(MODES[mode]), 'MC2_RENDER_SCALE': '0.67' if mode == 'D_fsr1' else '1',
            'MC2_WIDTH': str(width), 'MC2_HEIGHT': str(height), 'MC2_VK_DEVICE': '7900',
            'MC2_VK_GPU_TIMESTAMPS': '1', 'MC2_VSYNC': '0', 'MC2_FPS_CAP': '0',
            'MC2_VISUAL_CAPTURE_EXIT_AFTER': '1'}


def luminance(rgb):
    x = np.asarray(rgb, dtype=np.float32) / 255
    x = np.where(x <= .04045, x/12.92, ((x+.055)/1.055)**2.4)
    return x @ np.array([.2126,.7152,.0722], dtype=np.float32)


def metrics(frames):
    x = np.stack([luminance(f) for f in frames])
    return {'linear_luma_variance': float(x.var(axis=0).mean()),
            'uncompensated_mad': float(np.abs(np.diff(x,axis=0)).mean())}


def process(directory, mission):
    """Keep original frames until semantic crop review and final cleanup."""
    result = {}
    for group in ['hold','pan','orbit','low']:
        paths = sorted(directory.glob(f'{mission}_{group}_[0-9]*.png'))
        if len(paths) < 6:
            raise RuntimeError(f'{directory}: missing {group} frames')
        sides = [json.loads(p.with_suffix('.json').read_text()) for p in paths]
        ids = [s['frame'] for s in sides]
        result[group] = {'frames': ids, 'consecutive': all(b==a+1 for a,b in zip(ids,ids[1:])),
                         'sim_frames': sorted({s['sim_frame'] for s in sides}),
                         'seeds': sorted({s['seed'] for s in sides}),
                         'rendered_start_pose': {k:sides[0].get(k) for k in ['cap_pos','cap_rot','cap_proj','cap_alt']}}
        if not result[group]['consecutive']:
            raise RuntimeError(f'{directory}/{group}: captures are not consecutive')
        images = [Image.open(p).convert('RGB') for p in paths]
        if group in ('hold','pan'):
            # Fixed screen ROIs: slow-pan MAD includes content motion, not pure flicker.
            w,h = images[0].size
            boxes = {'whole_scene': (0,0,w,h*3//4), 'upper': (w//4,h//8,3*w//4,3*h//8),
                     'center': (w//4,3*h//8,3*w//4,5*h//8), 'left': (0,h//4,w//4,3*h//4),
                     'right': (3*w//4,h//4,w,3*h//4)}
            result[group]['regions'] = {name: metrics([im.crop(box) for im in images]) for name,box in boxes.items()}
        # Six consecutive frames in each strip. Two crops selected by maximum
        # temporal variance, spatially separated; labels explicitly say candidates.
        start=len(images)//2
        selected = images[start:start+6]
        result[group]['strip_frames']=ids[start:start+6]
        strip = Image.new('RGB',(6*320,180))
        for i,im in enumerate(selected): strip.paste(im.resize((320,180)),(320*i,0))
        strip.save(directory/f'{group}-strip.jpg',quality=88)
        stack = np.stack([luminance(im) for im in selected])
        var = stack.var(axis=0); h,w = var.shape
        candidates = [(float(var[y:y+128,x:x+192].mean()),x,y)
                      for y in range(0,h*3//4-128,64) for x in range(0,w-192,96)]
        chosen = []
        for _,x,y in sorted(candidates,reverse=True):
            if all(abs(x-a)>192 or abs(y-b)>128 for a,b in chosen): chosen.append((x,y))
            if len(chosen)==2: break
        reference=directory.parent/'A_smaa/metrics.json'
        if reference.exists() and directory.name!='A_smaa':
            chosen=[tuple(box[:2]) for box in json.loads(reference.read_text())[group]['crop_boxes']]
        result[group]['crop_boxes'] = [[x,y,x+192,y+128] for x,y in chosen]
        cropstrip=Image.new('RGB',(6*192,2*128))
        for j,(x,y) in enumerate(chosen):
            for i,im in enumerate(selected): cropstrip.paste(im.crop((x,y,x+192,y+128)),(192*i,128*j))
        cropstrip.save(directory/f'{group}-crops.jpg',quality=92)
        if group in ('orbit','low'):
            gif=[im.resize((480,270)) for im in images[::5]]
            gif[0].save(directory/f'{group}.gif',save_all=True,append_images=gif[1:],duration=167,loop=0)
        for im in images: im.close()
    (directory/'metrics.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    return result


def captures():
    os.environ.setdefault('MC2_HOST_LEASE_TIMEOUT','1200')
    for mission in MISSIONS:
        marks=OUT/f'{mission}-track.json'
        marks.write_text(json.dumps(track(mission)),encoding='utf-8')
        for mode in MODES:
            directory=OUT/mission/mode
            if (directory/'metrics.json').exists(): continue
            argv=[sys.executable,str(ROOT/'scripts/run_visual_capture.py'),'--exe',str(INSTALL/'mc2.exe'),
                  '--mission',mission,'--bookmarks',str(marks),'--out-dir',str(directory),
                  '--trigger-frame','300','--settle','0','--duration','600','--no-kill']
            for k,v in declared(mode,1920,1080).items(): argv += ['--env-declare',f'{k}={v}']
            with (OUT/f'{mission}-{mode}-harness.log').open('w',encoding='utf-8') as log:
                rc=subprocess.run(argv,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT).returncode
            if rc: raise RuntimeError(f'capture failed {mission}/{mode}: {rc}')
            result=process(directory,mission)
            print(f'CAPTURE {mission}/{mode}: consecutive='+str(all(r['consecutive'] for r in result.values())),flush=True)


def perf():
    import psutil
    for mission in MISSIONS:
        pose=next(b for b in track(mission)['bookmarks'] if b['name']=='orbit_000')
        park=','.join(str(x) for x in [*pose['pos'][:2],pose['rot'],pose['proj'],pose['alt']])
        for mode in MODES:
            directory=OUT/mission/mode; directory.mkdir(parents=True,exist_ok=True)
            if (directory/'perf.json').exists(): continue
            runs=[]
            for i in range(4):
                declared_env=declared(mode,3840,2160)
                declared_env.pop('MC2_VISUAL_CAPTURE_EXIT_AFTER')
                declared_env['MC2_CAMERA_PARK']=park+'@300'
                env,_=capture.build_capture_env(OUT/f'{mission}-track.json',directory,300,0,True,declared_env)
                for key in ['MC2_VISUAL_BOOKMARK_CAPTURE','MC2_VISUAL_CAPTURE_DIR','MC2_VISUAL_CAPTURE_FRAME','MC2_VISUAL_SETTLE']:
                    env.pop(key,None)
                start=time.monotonic()
                with host_lease.exclusive_window(f'temporal A/B {mission}/{mode}/{i}',timeout_s=1200):
                    waited=time.monotonic()-start
                    samples=HostSamples(psutil)
                    with samples:
                        run=runner.run_one(runner.RunConfig(exe=[str(INSTALL/'mc2.exe')],profile='stock',stem=mission,
                            duration=15,heartbeat_timeout_load_s=180,heartbeat_timeout_play_s=30,grace_s=240,env_extra=env))
                    noise=noise_report(samples)
                (directory/f'perf-{i}.log').write_text(run.stdout_text,encoding='utf-8')
                lines=[line for line in run.stdout_text.splitlines() if '[VK_GPU_PASSES v1]' in line and 'span_ms=' in line]
                if any(re.search(r'(?:dropped|overflow)=[1-9]',line) for line in lines):
                    noise['status']='NOISY'; noise['reasons'].append('GPU timestamp scope drop/overflow')
                spans=[float(re.search(r'span_ms=([\d.]+)',l)[1]) for l in lines if int(re.search(r'frame=(\d+)',l)[1])>=420]
                passes={}
                for line in lines:
                    if int(re.search(r'frame=(\d+)',line)[1])<420: continue
                    for name,value in re.findall(r'(VKIsl\.[\w.]+)=([\d.]+)',line): passes.setdefault(name,[]).append(float(value))
                if run.exit_code or run.killed_by_timeout or not run.verdict.passed or not spans:
                    raise RuntimeError(f'perf failed: {mission}/{mode}/{i}')
                runs.append(dict(noise=noise,lease_wait_seconds=waited,game_seconds=run.walltime_s,
                    mean_ms=float(np.mean(spans)),p95_ms=float(np.percentile(spans,95)),samples=len(spans),
                    passes={k:float(np.mean(v)) for k,v in passes.items()}))
                print(f'PERF {mission}/{mode}/{i}: {noise["status"]}',flush=True)
            (directory/'perf.json').write_text(json.dumps(runs,indent=2),encoding='utf-8')


def report():
    sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    lines=[f'# Temporal A/B — {sha}', '', 'DEV ONLY. Defaults unchanged. B is unavailable: multisample attachment/pipeline work exceeds cheap enablement.',
        'E uses AF16 plus derivative normal-variance roughness inflation (Toksvig-style; no stored mip moments).',
        'Simulation/foliage frozen at seed 0xC0FFEE, trigger frame 300. Orbit: 301 consecutive frames, 360°/10 s nominal at 30 fps; low sweep: 121 frames/4 s.',
        'Slow pan: 3°/s. Hold variance is linear luminance. Pan MAD is uncompensated and includes scene motion; do not call it pure shimmer.',
        'C is a simple encoded-color TAA with neighborhood rejection/clamp; no depth-history test or reactive-mask rejection. Inspect ghosting before any shipping decision.', '',
        '| Mission/mode | Hold variance | Pan MAD | 4K GPU mean ms | noise |','|---|---:|---:|---:|---|']
    panels=[]
    for mission in MISSIONS:
        columns=[]
        for mode in MODES:
            directory=OUT/mission/mode; relative=f'{mission}/{mode}'
            met=json.loads((directory/'metrics.json').read_text()) if (directory/'metrics.json').exists() else {}
            perfrows=json.loads((directory/'perf.json').read_text()) if (directory/'perf.json').exists() else []
            quiet=[r['mean_ms'] for r in perfrows if r['noise']['status']=='quiet']
            status='quiet' if len(quiet)==4 else 'NOISY / incomplete'
            mean=f'{np.mean(quiet):.3f} ({min(quiet):.3f}–{max(quiet):.3f})' if len(quiet)==4 else 'unproven'
            hold=met.get('hold',{}).get('regions',{}).get('whole_scene',{}).get('linear_luma_variance','unproven')
            pan=met.get('pan',{}).get('regions',{}).get('whole_scene',{}).get('uncompensated_mad','unproven')
            lines.append(f'| {mission}/{mode} | {hold} | {pan} | {mean} | {status} |')
            columns.append(f'<article><h3>{mode}</h3>'+''.join(f'<h4>{group}</h4><img loading="lazy" src="{relative}/{group}.gif"><a href="{relative}/{group}-strip.jpg">Six consecutive frames</a><img loading="lazy" src="{relative}/{group}-crops.jpg">' for group in ['orbit','low'])+
                f'<p>Crops: highest variance candidates; content labels require visual review.</p><a href="{relative}/metrics.json">ROI metrics and frame IDs</a> · <a href="{relative}/perf.json">GPU passes/load</a></article>')
        panels.append(f'<h2>{mission}</h2><section>'+''.join(columns)+'</section>')
    lines += ['', 'Recommendation pending completed evidence and visual review. No shipping/default recommendation can be made from incomplete runs.',
              '', 'FSR1 source: https://github.com/GPUOpen-Effects/FidelityFX-FSR/tree/a21ffb8f6c13233ba336352bdff293894c706575']
    summary='\n'.join(lines)+'\n'
    (OUT/f'summary-{sha[:12]}.md').write_text(summary,encoding='utf-8')
    (OUT/'summary.md').write_text(summary,encoding='utf-8')
    (OUT/'index.html').write_text('<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Temporal A/B</title><style>body{font:16px system-ui;background:#131820;color:#eee;margin:16px}section{display:grid;grid-template-columns:repeat(4,minmax(260px,1fr));gap:12px;overflow-x:auto}article{background:#222b38;padding:10px}img{width:100%;height:auto}a{color:#8cd2ff}pre{white-space:pre-wrap}</style><h1>Temporal shimmer A/B</h1><pre>'+html.escape(summary)+'</pre>'+''.join(panels),encoding='utf-8')
    print(OUT/'index.html',flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase',choices=['capture','perf','report'])
    args=parser.parse_args(); OUT.mkdir(parents=True,exist_ok=True)
    {'capture':captures,'perf':perf,'report':report}[args.phase]()
