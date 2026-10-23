"""Reproducible stock playable demos and capture poses; never launches the game."""
import argparse
from pathlib import Path

import numpy as np

from tools.mapgen.common import output_path, write_json
from tools.mapgen.data import Install
from tools.mapgen.generate import generate
from tools.mapgen.mission_writer import STOCK, emit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--source',type=Path,default=STOCK)
    parser.add_argument('--size',type=int,choices=(180,360,720),action='append')
    parser.add_argument('--biome',choices=('suburban_forest',))
    parser.add_argument('--seed',type=int,action='append',help='repeat for a seed batch (one size)')
    args = parser.parse_args()
    out = output_path(args.out,[args.source])
    source = Install(args.source)
    sizes = args.size or ((360,) if args.biome else (180,720))
    if args.seed and len(sizes) != 1:
        parser.error('--seed batches require one --size')
    jobs = [(size,seed) for size in sizes for seed in (args.seed or [{180:7,360:17,720:23}[size]])]
    for size, seed in jobs:
        layout = generate(size,seed,None if args.biome else np.full((size,size),1000.),biome=args.biome)
        name = layout['name'] if args.biome else f'mapgen_{size}_{seed}'
        write_json(out/'layouts'/(name+'.json'),layout)
        report = emit(layout,source,out/'missions',name)
        poses = []
        for i, base in enumerate(layout['bases'][:2]):
            r0,c0,r1,c1 = base['bounds']
            x,y = ((c0+c1)/2-size/2)*128/3,(size/2-(r0+r1)/2)*128/3
            poses.append({'name':f'base{i}_topdown','pos':[x,y,1000],
                          'rot':0,'proj':75,'alt':4000,'covers':['generated_base'],
                          '_note':'Generated inspection pose from parcel bounds; no golden/parity claim.'})
        poses.append({'name':'player_lance','pos':layout['player_start_wu'],
                      'rot':0,'proj':55,'alt':3000,'covers':['generated_player_units']})
        if args.biome:
            poses.insert(0, {'name':'biome_overview','pos':[0,0,1000],
                'rot':0,'proj':85,'alt':18000,'covers':['parcels','forest','river','villages']})
        write_json(out/'bookmarks'/(name+'.json'),{'schema_v':1,'mission':name,'bookmarks':poses},pretty=True)
        print(report,flush=True)


if __name__ == '__main__':
    main()
