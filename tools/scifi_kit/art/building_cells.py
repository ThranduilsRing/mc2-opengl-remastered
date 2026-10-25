"""Blocking-cell diff of the building remakes against the stock (owner: the gameplay-relevant measure of the footprint
rule). For every building in the footprint sweep, compare the remake's blocking cells with the stock's at the sampled
recorded mission placements (all rotations), and split the footprint failures into (a) cells match, only thin parts
overhang the outline, and (b) the blocking cells differ.

  py -3 tools/scifi_kit/art/building_cells.py [--out FILE]
"""
import argparse
import json

import cells
import topdown

SWEEP = 'E:/mc2-ref/scifi-kit/art/intent-pass/footprint-sweep.json'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='E:/mc2-ref/scifi-kit/art/intent-pass/building-cells.json')
    a = ap.parse_args()
    rows = json.load(open(SWEEP))
    out = {}
    for n, r in rows.items():
        if r['kind'] != 'building':
            continue
        R = topdown.remake_tris(n)
        c = cells.check(n, R)
        out[n] = dict(footprint_ok=r['ok'], fails=r['fails'], plan_iou=r['metrics'].get('plan_iou'),
                      protrusion_m=r['metrics'].get('protrusion_m'), cells=c)
        tag = 'no placements' if not c.get('placements') else ('cells match' if c['remake_match'] == 1.0 else 'cells differ')
        print(f'{n:22s} footprint {"ok " if r["ok"] else "FAIL"} iou {out[n]["plan_iou"]} prot {out[n]["protrusion_m"]}  {tag} '
              f'{c.get("remake_match", "")} {[ (m["extra"], m["missing"]) for m in c.get("mismatches", [])[:3]]}')
    json.dump(out, open(a.out, 'w'), indent=1)
    fail = {n: v for n, v in out.items() if not v['footprint_ok']}
    a_ = [n for n, v in fail.items() if v['cells'].get('placements') and v['cells']['remake_match'] == 1.0]
    b_ = [n for n, v in fail.items() if v['cells'].get('placements') and v['cells']['remake_match'] < 1.0]
    c_ = [n for n, v in fail.items() if not v['cells'].get('placements')]
    print(f'\nfootprint failures {len(fail)}: (a) cells match {len(a_)}, (b) cells differ {len(b_)}, never placed {len(c_)}')
    print('(a)', sorted(a_))
    print('(b)', sorted(b_))
    print('never placed', sorted(c_))


if __name__ == '__main__':
    main()
