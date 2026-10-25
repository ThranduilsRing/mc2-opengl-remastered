"""Build one intent asset, without review renders or sheets."""
import argparse
import json
import time
from pathlib import Path

import intent


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('command', choices=['check', 'bake'])
    ap.add_argument('asset', choices=sorted(intent.ASSETS))
    ap.add_argument('--output-root', type=Path, default=Path('E:/mc2-ref/scifi-kit/art/intent-tools'))
    a = ap.parse_args()
    if a.command == 'check':
        result = intent.check(a.asset)
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result['ok'] else 1)
    import run
    import meshopt_lods
    out = a.output_root.resolve() / a.asset
    out.mkdir(parents=True, exist_ok=True)
    run.OUT = out  # Job files, logs and bakes all stay under the requested root.
    start = time.monotonic()
    job = dict(kind='assembly', tag=a.asset, out=str(out), assemblies=[a.asset],
               conditions=['pristine'], export=True, render=False)
    run.blender(job, out / 'build.log', timeout=7200)
    base, lods = meshopt_lods.lods(out / 'pbr' / f'{a.asset}_pristine_lod0.glb')
    report = out / 'pbr' / f'{a.asset}_pristine-bake.json'
    res = json.loads(report.read_text())
    res['lods'] = res['lods'][:1] + lods
    res['lods'][0]['triangles'] = base
    res['build_seconds'] = round(time.monotonic() - start, 2)
    report.write_text(json.dumps(res, indent=2))
    print(f"Built {a.asset} in {res['build_seconds']} s: {out / 'pbr'}")


if __name__ == '__main__':
    main()
